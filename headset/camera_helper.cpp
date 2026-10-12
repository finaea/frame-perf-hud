// T09: see camera_helper.h
#include "camera_helper.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace camhelper {

namespace {

constexpr uint32_t kCapSysPtrace = 19;  // linux/capability.h
constexpr uint32_t kVfsCapRevisionMask = 0xFF000000u;
constexpr uint32_t kVfsCapFlagsEffective = 0x000001u;
constexpr uint32_t kVfsCapRevision1 = 0x01000000u;
constexpr uint32_t kVfsCapRevision2 = 0x02000000u;
constexpr uint32_t kVfsCapRevision3 = 0x03000000u;

uint32_t readLe32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

FileFacts factsOf(const char* path) {
    FileFacts f;
    struct stat st;
    if (lstat(path, &st) != 0) return f;
    f.exists = true;
    f.regular = S_ISREG(st.st_mode);
    f.directory = S_ISDIR(st.st_mode);
    f.uid = st.st_uid;
    f.gid = st.st_gid;
    f.mode = st.st_mode & 07777;
    return f;
}

double monotonicSec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ install check

bool capXattrHasPtraceEffective(const uint8_t* data, size_t len) {
    if (!data || len < 4) return false;
    const uint32_t magic = readLe32(data);
    const uint32_t revision = magic & kVfsCapRevisionMask;
    size_t want = 0;
    if (revision == kVfsCapRevision1) want = 12;
    else if (revision == kVfsCapRevision2) want = 20;
    else if (revision == kVfsCapRevision3) want = 24;
    else return false;
    if (len != want) return false;
    // revision 3 carries the user namespace's root uid: anything but 0 only applies inside that namespace
    if (revision == kVfsCapRevision3 && readLe32(data + 20) != 0) return false;
    if (!(magic & kVfsCapFlagsEffective)) return false;
    const uint32_t permitted0 = readLe32(data + 4);
    return (permitted0 >> kCapSysPtrace) & 1u;
}

InstallState judgeInstall(const FileFacts& dir, const FileFacts& file, const std::vector<uint8_t>& capXattr) {
    if (!file.exists) return InstallState::Missing;
    if (!dir.directory || dir.uid != 0 || (dir.mode & 022)) return InstallState::UnsafeDir;
    if (!file.regular) return InstallState::NotRegular;
    if (file.uid != 0 || file.gid != 0) return InstallState::WrongOwner;
    if (file.mode != 0755) return InstallState::WrongMode;
    if (!capXattrHasPtraceEffective(capXattr.data(), capXattr.size())) return InstallState::NoCapability;
    return InstallState::Ok;
}

InstallState checkInstall(const char* dir, const char* path) {
    const FileFacts d = factsOf(dir);
    const FileFacts f = factsOf(path);
    std::vector<uint8_t> cap(64);
    const ssize_t n = lgetxattr(path, "security.capability", cap.data(), cap.size());
    cap.resize(n > 0 ? size_t(n) : 0);
    return judgeInstall(d, f, cap);
}

const char* installStateText(InstallState state) {
    switch (state) {
        case InstallState::Ok: return "installed";
        case InstallState::Missing: return "not installed";
        case InstallState::NotRegular: return "not a regular file";
        case InstallState::WrongOwner: return "not owned by root:root";
        case InstallState::WrongMode: return "mode is not 0755";
        case InstallState::NoCapability: return "missing cap_sys_ptrace (the file was replaced?)";
        case InstallState::UnsafeDir: return "its directory isn't root-owned 0755";
    }
    return "?";
}

std::string notInstalledMessage() {
    return std::string("Camera helper not installed: run `sudo ") + kInstallerHint + "`";
}

// ------------------------------------------------------------------------------------------------ respawn limit

bool RespawnLimiter::tryStart(double nowSec) {
    if (started_ && nowSec - last_ < minGap_) return false;
    started_ = true;
    last_ = nowSec;
    return true;
}

// ------------------------------------------------------------------------------------------------ outward retry

void OutwardRetry::reset() {
    for (auto& s : state_) s = CamState::Unknown;
    nextAt_ = 0.0;
    partialRetries_ = 0;
    gaveUp_ = false;
}

void OutwardRetry::onCam(uint8_t cam, double nowSec) {
    if (cam >= FPH_OUTWARD_CAMS) return;
    state_[cam] = CamState::Present;
    reschedule(nowSec);
}

void OutwardRetry::onGone(uint8_t cam, uint8_t reason, double nowSec) {
    if (cam >= FPH_OUTWARD_CAMS) return;
    state_[cam] = (reason == FPH_GONE_OFF || reason == FPH_GONE_DENIED) ? CamState::Final : CamState::Missing;
    reschedule(nowSec);
}

void OutwardRetry::reschedule(double nowSec) {
    bool missing = false;
    for (auto s : state_) missing |= s == CamState::Missing;
    if (!missing || gaveUp_) {
        nextAt_ = 0.0;
    } else if (nextAt_ == 0.0) {
        nextAt_ = nowSec + interval_;
    }
}

bool OutwardRetry::due(double nowSec) {
    if (nextAt_ == 0.0 || nowSec < nextAt_) return false;
    nextAt_ = 0.0;  // the CAM / CAM_GONE answers to this ON reschedule it
    bool anyPresent = false;
    for (auto s : state_) anyPresent |= s == CamState::Present;
    if (anyPresent && ++partialRetries_ > maxPartial_) {
        gaveUp_ = true;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ messages

FdList& FdList::operator=(FdList&& other) noexcept {
    if (this != &other) {
        closeAll();
        fds_ = std::move(other.fds_);
        other.fds_.clear();
    }
    return *this;
}

std::vector<int> FdList::release() {
    std::vector<int> out = std::move(fds_);
    fds_.clear();
    return out;
}

void FdList::closeAll() {
    for (int fd : fds_)
        if (fd >= 0) close(fd);
    fds_.clear();
}

namespace {

template <typename T>
DecodeError copyExact(const void* data, size_t len, T& out) {
    if (len != sizeof(T)) return DecodeError::BadLength;
    std::memcpy(&out, data, sizeof(T));
    return DecodeError::None;
}

}  // namespace

DecodeError decodeMessage(const void* data, size_t len, std::vector<int> fds, Message& out) {
    FdList owned;
    for (int fd : fds) owned.add(fd);
    out = Message{};
    if (len < sizeof(fph_msg_hdr)) return DecodeError::TooShort;
    fph_msg_hdr hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != FPH_MSG_MAGIC) return DecodeError::BadMagic;
    if (hdr.version != FPH_PROTO_VERSION) return DecodeError::BadVersion;

    DecodeError e = DecodeError::None;
    size_t wantFds = 0;
    switch (hdr.type) {
        case FPH_MSG_HELLO: e = copyExact(data, len, out.hello); break;
        case FPH_MSG_CAM:
            e = copyExact(data, len, out.cam);
            if (e == DecodeError::None) {
                if (out.cam.cam >= FPH_OUTWARD_CAMS || out.cam.nbufs == 0 || out.cam.nbufs > FPH_MAX_BUFS)
                    return DecodeError::BadFields;
                wantFds = out.cam.nbufs;
            }
            break;
        case FPH_MSG_CAM_GONE:
        case FPH_MSG_EYE_GONE:
            e = copyExact(data, len, out.gone);
            if (e == DecodeError::None && out.gone.cam >= FPH_OUTWARD_CAMS) return DecodeError::BadFields;
            break;
        case FPH_MSG_EYE:
            e = copyExact(data, len, out.eye);
            if (e == DecodeError::None) {
                if (out.eye.narenas == 0 || out.eye.narenas > FPH_MAX_EYE_ARENAS) return DecodeError::BadFields;
                wantFds = out.eye.narenas;
            }
            break;
        case FPH_MSG_FRAMES:
            e = copyExact(data, len, out.frames);
            if (e == DecodeError::None && (out.frames.cam >= FPH_OUTWARD_CAMS || out.frames.n > FPH_MAX_BUFS))
                return DecodeError::BadFields;
            break;
        case FPH_MSG_RECHECK:
            e = copyExact(data, len, out.recheck);
            if (e == DecodeError::None && (out.recheck.cam >= FPH_OUTWARD_CAMS || out.recheck.result > FPH_RECHECK_INVALID))
                return DecodeError::BadFields;
            break;
        case FPH_MSG_ERROR: e = copyExact(data, len, out.error); break;
        case FPH_MSG_BYE: e = copyExact(data, len, out.bye); break;
        default: return DecodeError::BadType;
    }
    if (e != DecodeError::None) return e;
    if (owned.size() != wantFds) return DecodeError::FdMismatch;
    out.type = static_cast<fph_msg_type>(hdr.type);
    out.fds = std::move(owned);
    return DecodeError::None;
}

const char* decodeErrorText(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "ok";
        case DecodeError::TooShort: return "too short";
        case DecodeError::BadMagic: return "bad magic";
        case DecodeError::BadVersion: return "protocol version mismatch (reinstall the camera helper)";
        case DecodeError::BadType: return "unknown message type";
        case DecodeError::BadLength: return "wrong length for its type";
        case DecodeError::BadFields: return "fields out of range";
        case DecodeError::FdMismatch: return "fd count doesn't match";
    }
    return "?";
}

fph_cmd makeCommand(fph_op op, uint8_t cam, uint32_t index, uint32_t seq) {
    fph_cmd c{};
    c.magic = FPH_CMD_MAGIC;
    c.op = static_cast<uint8_t>(op);
    c.cam = cam;
    c.index = index;
    c.seq = seq;
    return c;
}

// ------------------------------------------------------------------------------------------------ the process

bool HelperProcess::start(const char* path, std::string* error) {
    stop();
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) != 0) {
        if (error) *error = std::string("socketpair: ") + std::strerror(errno);
        return false;
    }
    // The helper's end must be blocking for it, and must not be fd 3 already (dup2 onto itself would keep
    // FD_CLOEXEC on older glibc): move it above 3 first.
    int child = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
    close(sv[1]);
    if (child < 0) {
        close(sv[0]);
        if (error) *error = std::string("fcntl: ") + std::strerror(errno);
        return false;
    }
    fcntl(child, F_SETFL, fcntl(child, F_GETFL) & ~O_NONBLOCK);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, child, 3);  // dup2 clears FD_CLOEXEC on the new fd
    char arg0[] = "fph-camgrab";
    char* argv[] = {arg0, nullptr};
    char* envp[] = {nullptr};
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, path, &actions, nullptr, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    close(child);
    if (rc != 0) {
        close(sv[0]);
        if (error) *error = std::string("spawn ") + path + ": " + std::strerror(rc);
        return false;
    }
    sock_ = sv[0];
    pid_ = pid;
    return true;
}

void HelperProcess::stop() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
    if (pid_ <= 0) return;
    // EOF makes it exit within milliseconds; give it 1 s, then make sure
    for (int i = 0; i < 100; i++) {
        const pid_t r = waitpid(pid_, nullptr, WNOHANG);
        if (r == pid_ || (r < 0 && errno == ECHILD)) {
            pid_ = -1;
            return;
        }
        usleep(10 * 1000);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
}

bool HelperProcess::send(const fph_cmd& cmd) {
    if (sock_ < 0) return false;
    return ::send(sock_, &cmd, sizeof(cmd), MSG_NOSIGNAL) == ssize_t(sizeof(cmd));
}

bool HelperProcess::sendOutward(bool on) { return send(makeCommand(on ? FPH_OP_OUTWARD_ON : FPH_OP_OUTWARD_OFF)); }
bool HelperProcess::sendEye(bool on) { return send(makeCommand(on ? FPH_OP_EYE_ON : FPH_OP_EYE_OFF)); }
bool HelperProcess::sendRecheck(uint8_t cam, uint32_t index, uint32_t seq) {
    return send(makeCommand(FPH_OP_RECHECK, cam, index, seq));
}

HelperProcess::ReadResult HelperProcess::read(Message& out, DecodeError* why) {
    if (sock_ < 0) return ReadResult::Closed;
    alignas(8) unsigned char buf[sizeof(fph_msg_cam) + 64];
    union {
        char buf[CMSG_SPACE(sizeof(int) * FPH_MAX_BUFS)];
        cmsghdr align;
    } ctrl;
    iovec iov{buf, sizeof(buf)};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl.buf;
    mh.msg_controllen = sizeof(ctrl.buf);
    const ssize_t n = recvmsg(sock_, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return ReadResult::WouldBlock;
        return ReadResult::Closed;
    }
    std::vector<int> fds;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < k; i++) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            fds.push_back(fd);
        }
    }
    if (n == 0) {
        for (int fd : fds) close(fd);
        return ReadResult::Closed;
    }
    if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        for (int fd : fds) close(fd);
        if (why) *why = DecodeError::BadLength;
        return ReadResult::Bad;
    }
    const DecodeError e = decodeMessage(buf, size_t(n), std::move(fds), out);
    if (why) *why = e;
    return e == DecodeError::None ? ReadResult::Message : ReadResult::Bad;
}

// ------------------------------------------------------------------------------------------------ debug

namespace {

const char* goneText(uint8_t reason) {
    switch (reason) {
        case FPH_GONE_OFF: return "turned off";
        case FPH_GONE_NO_PROCESS: return "process not running";
        case FPH_GONE_NO_DEVICE: return "no such device / buffers";
        case FPH_GONE_DENIED: return "permission denied (helper not installed with the capability?)";
        case FPH_GONE_FORMAT: return "unsupported format";
        case FPH_GONE_FAILED: return "failed";
    }
    return "?";
}

const char* recheckText(uint8_t r) {
    switch (r) {
        case FPH_RECHECK_KEEP: return "keep";
        case FPH_RECHECK_NEWER: return "newer";
        case FPH_RECHECK_REQUEUED: return "requeued";
        case FPH_RECHECK_STALE: return "stale";
        case FPH_RECHECK_INVALID: return "invalid";
    }
    return "?";
}

const char* byeText(uint32_t r) {
    switch (r) {
        case FPH_BYE_XRSERVICE_EXITED: return "XRService exited";
        case FPH_BYE_EYETRACKING_EXITED: return "eyetracking exited";
        case FPH_BYE_SOCKET_ERROR: return "socket error";
    }
    return "?";
}

struct Mapping {
    const uint8_t* ptr = nullptr;
    size_t len = 0;
};

/** One outward cam as the dump sees it: mapped buffers + per-second counters. */
struct DumpCam {
    bool present = false;
    fph_msg_cam info{};
    std::vector<Mapping> maps;
    FdList fds;
    // counters for the current second
    int reports = 0, frames = 0, keep = 0, newer = 0, requeued = 0, stale = 0, invalid = 0;
    uint32_t newestSeq = 0;
    double lastMean = -1.0;
    double parityMean[2] = {-1.0, -1.0};

    void unmap() {
        for (auto& m : maps)
            if (m.ptr) munmap(const_cast<uint8_t*>(m.ptr), m.len);
        maps.clear();
        fds.closeAll();
        present = false;
    }
};

/** Mean of every 97th Y byte of one frame. No cache maintenance (T10's job), so it's approximate. */
double sampleMean(const DumpCam& c, uint32_t index) {
    if (index >= c.maps.size() || !c.maps[index].ptr) return -1.0;
    const uint8_t* p = c.maps[index].ptr;
    uint64_t sum = 0, count = 0;
    for (uint32_t y = 0; y < c.info.height; y += 8) {
        const uint8_t* row = p + size_t(y) * c.info.pitch;
        for (uint32_t x = 0; x < c.info.width; x += 97) {
            sum += row[x];
            count++;
        }
    }
    return count ? double(sum) / double(count) : -1.0;
}

}  // namespace

int runCameraHelperDump(const CameraHelperDumpOptions& options, const volatile std::sig_atomic_t& stop) {
    const InstallState state = checkInstall();
    std::printf("camera helper %s: %s\n", kHelperPath, installStateText(state));
    const char* path = options.helperPath.empty() ? kHelperPath : options.helperPath.c_str();
    if (options.helperPath.empty() && state != InstallState::Ok) {
        std::printf("%s\n", notInstalledMessage().c_str());
        if (state == InstallState::Missing) return 1;
    }
    if (!options.helperPath.empty())
        std::printf("using %s instead (protocol test: without the capability every grab is denied)\n", path);

    HelperProcess helper;
    RespawnLimiter limiter;
    DumpCam cams[FPH_OUTWARD_CAMS];
    std::vector<Mapping> eyeMaps;
    FdList eyeFds;
    auto dropAll = [&] {
        for (auto& c : cams) c.unmap();
        for (auto& m : eyeMaps) munmap(const_cast<uint8_t*>(m.ptr), m.len);
        eyeMaps.clear();
        eyeFds.closeAll();
    };

    const double t0 = monotonicSec();
    double nextSummary = t0 + 1.0;
    int starts = 0;
    // A target or camera that isn't there yet (SteamVR still starting after a restart) comes back as GONE and
    // the helper stays up: re-send ON every 2 s until it shows up (OutwardRetry for the outward cams). T10
    // does the same.
    constexpr double kRetrySec = 2.0;
    OutwardRetry outwardRetry(kRetrySec);
    bool reportedGiveUp = false;
    double eyeRetryAt = 0.0;  // 0 = no retry pending
    auto retryable = [](uint8_t reason) { return reason != FPH_GONE_OFF && reason != FPH_GONE_DENIED; };
    while (!stop && monotonicSec() - t0 < options.seconds) {
        const double now = monotonicSec();
        if (!helper.running()) {
            if (!limiter.tryStart(now)) {
                usleep(100 * 1000);
                continue;
            }
            std::string err;
            if (!helper.start(path, &err)) {
                std::printf("[%6.2f] start failed: %s\n", now - t0, err.c_str());
                continue;
            }
            starts++;
            std::printf("[%6.2f] started helper pid %d (start #%d)\n", now - t0, int(helper.pid()), starts);
            if (options.outward) helper.sendOutward(true);
            if (options.eye) helper.sendEye(true);
            outwardRetry.reset();
            reportedGiveUp = false;
            eyeRetryAt = 0.0;
        }
        if (options.outward && outwardRetry.due(now)) {
            std::printf("[%6.2f] retrying outward on (a cam is still missing)\n", now - t0);
            helper.sendOutward(true);
        }
        if (outwardRetry.gaveUp() && !reportedGiveUp) {
            reportedGiveUp = true;
            std::printf("[%6.2f] gave up retrying the missing outward cam(s)\n", now - t0);
        }
        if (eyeRetryAt > 0.0 && now >= eyeRetryAt) {
            eyeRetryAt = 0.0;
            std::printf("[%6.2f] retrying eye on\n", now - t0);
            helper.sendEye(true);
        }

        pollfd p{helper.fd(), POLLIN, 0};
        double wakeAt = nextSummary;
        if (outwardRetry.nextAt() > 0.0 && outwardRetry.nextAt() < wakeAt) wakeAt = outwardRetry.nextAt();
        if (eyeRetryAt > 0.0 && eyeRetryAt < wakeAt) wakeAt = eyeRetryAt;
        const int waitMs = int((wakeAt - monotonicSec()) * 1000.0);
        poll(&p, 1, waitMs > 0 ? waitMs : 0);

        for (;;) {
            Message m;
            DecodeError why = DecodeError::None;
            const auto r = helper.read(m, &why);
            const double t = monotonicSec() - t0;
            if (r == HelperProcess::ReadResult::WouldBlock) break;
            if (r == HelperProcess::ReadResult::Closed) {
                std::printf("[%6.2f] helper exited (EOF); unmapping everything, respawn allowed at +%.1fs\n", t,
                            limiter.nextAllowed() - t0);
                helper.stop();
                dropAll();
                outwardRetry.reset();
                eyeRetryAt = 0.0;
                break;
            }
            if (r == HelperProcess::ReadResult::Bad) {
                std::printf("[%6.2f] bad message: %s\n", t, decodeErrorText(why));
                continue;
            }
            switch (m.type) {
                case FPH_MSG_HELLO:
                    std::printf("[%6.2f] HELLO pid %u, cap_sys_ptrace %s\n", t, m.hello.helper_pid,
                                m.hello.has_ptrace ? "yes" : "no");
                    break;
                case FPH_MSG_CAM: {
                    DumpCam& c = cams[m.cam.cam];
                    c.unmap();
                    c.info = m.cam;
                    c.present = true;
                    int mapped = 0;
                    for (size_t i = 0; i < m.fds.size(); i++) {
                        Mapping mp;
                        mp.len = m.cam.bufs[i].length;
                        void* ptr = mmap(nullptr, mp.len, PROT_READ, MAP_SHARED, m.fds[i], 0);
                        mp.ptr = ptr == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(ptr);
                        mapped += mp.ptr != nullptr;
                        c.maps.push_back(mp);
                    }
                    c.fds = std::move(m.fds);
                    outwardRetry.onCam(m.cam.cam, monotonicSec());
                    const uint32_t f = m.cam.pixelformat;
                    std::printf("[%6.2f] CAM %u = /dev/video%u gen %u: %ux%u %c%c%c%c pitch %u (bytesperline %u, "
                                "sizeimage %u), %u buffers, %d mmapped read-only; inodes",
                                t, m.cam.cam, m.cam.node, m.cam.gen, m.cam.width, m.cam.height, char(f), char(f >> 8),
                                char(f >> 16), char(f >> 24), m.cam.pitch, m.cam.bytesperline, m.cam.sizeimage,
                                m.cam.nbufs, mapped);
                    for (unsigned i = 0; i < m.cam.nbufs; i++)
                        std::printf(" %llu", static_cast<unsigned long long>(m.cam.bufs[i].ino));
                    std::printf("\n");
                    break;
                }
                case FPH_MSG_CAM_GONE:
                    cams[m.gone.cam].unmap();
                    std::printf("[%6.2f] CAM %u gone: %s%s%s\n", t, m.gone.cam, goneText(m.gone.reason),
                                m.gone.err ? ", " : "", m.gone.err ? std::strerror(m.gone.err) : "");
                    outwardRetry.onGone(m.gone.cam, m.gone.reason, monotonicSec());
                    break;
                case FPH_MSG_EYE: {
                    for (auto& mp : eyeMaps) munmap(const_cast<uint8_t*>(mp.ptr), mp.len);
                    eyeMaps.clear();
                    std::printf("[%6.2f] EYE gen %u: %u udmabuf arenas:", t, m.eye.gen, m.eye.narenas);
                    for (size_t i = 0; i < m.fds.size(); i++) {
                        const size_t len = size_t(m.eye.arenas[i].size);
                        void* ptr = mmap(nullptr, len, PROT_READ, MAP_SHARED, m.fds[i], 0);
                        std::printf(" [ino %llu, %zu MiB, %s]", static_cast<unsigned long long>(m.eye.arenas[i].ino),
                                    len >> 20, ptr == MAP_FAILED ? "mmap failed" : "mmapped");
                        if (ptr != MAP_FAILED) eyeMaps.push_back({static_cast<const uint8_t*>(ptr), len});
                    }
                    std::printf("\n");
                    eyeFds = std::move(m.fds);
                    eyeRetryAt = 0.0;
                    break;
                }
                case FPH_MSG_EYE_GONE:
                    std::printf("[%6.2f] EYE gone: %s%s%s\n", t, goneText(m.gone.reason), m.gone.err ? ", " : "",
                                m.gone.err ? std::strerror(m.gone.err) : "");
                    if (options.eye && retryable(m.gone.reason) && eyeRetryAt == 0.0)
                        eyeRetryAt = monotonicSec() + kRetrySec;
                    break;
                case FPH_MSG_FRAMES: {
                    DumpCam& c = cams[m.frames.cam];
                    if (!c.present || m.frames.gen != c.info.gen || m.frames.n == 0) break;
                    c.reports++;
                    c.frames += m.frames.n;
                    // like T10: copy the newest finished frame, then ask whether it was still that frame
                    const fph_frame_entry& e = m.frames.entries[m.frames.n - 1];
                    c.newestSeq = e.seq;
                    c.lastMean = sampleMean(c, e.index);
                    c.parityMean[e.seq & 1] = c.lastMean;
                    helper.sendRecheck(m.frames.cam, e.index, e.seq);
                    break;
                }
                case FPH_MSG_RECHECK: {
                    DumpCam& c = cams[m.recheck.cam];
                    switch (m.recheck.result) {
                        case FPH_RECHECK_KEEP: c.keep++; break;
                        case FPH_RECHECK_NEWER: c.newer++; break;
                        case FPH_RECHECK_REQUEUED: c.requeued++; break;
                        case FPH_RECHECK_STALE: c.stale++; break;
                        default: c.invalid++; break;
                    }
                    if (m.recheck.result == FPH_RECHECK_STALE || m.recheck.result == FPH_RECHECK_INVALID)
                        std::printf("[%6.2f] RECHECK cam %u buf %u seq %u: %s\n", t, m.recheck.cam, m.recheck.index,
                                    m.recheck.seq, recheckText(m.recheck.result));
                    break;
                }
                case FPH_MSG_ERROR:
                    std::printf("[%6.2f] ERROR from helper: code %u\n", t, m.error.code);
                    break;
                case FPH_MSG_BYE:
                    std::printf("[%6.2f] BYE: %s\n", t, byeText(m.bye.reason));
                    break;
            }
        }

        if (monotonicSec() >= nextSummary) {
            nextSummary += 1.0;
            for (unsigned i = 0; i < FPH_OUTWARD_CAMS; i++) {
                DumpCam& c = cams[i];
                if (!c.present) continue;
                std::printf("         cam %u video%u: %d reports, %d frames, newest seq %u, recheck keep %d / newer %d / "
                            "requeued %d / stale %d, mean even %.1f odd %.1f\n",
                            i, c.info.node, c.reports, c.frames, c.newestSeq, c.keep, c.newer, c.requeued, c.stale,
                            c.parityMean[0], c.parityMean[1]);
                c.reports = c.frames = c.keep = c.newer = c.requeued = c.stale = c.invalid = 0;
            }
        }
    }
    helper.stop();
    dropAll();
    std::printf("stopped (%d helper start%s)\n", starts, starts == 1 ? "" : "s");
    return 0;
}

}  // namespace camhelper
