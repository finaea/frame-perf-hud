// T09: the agent side of the camera helper (headset/camera_helper.*): install judgement, capability xattr, respawn
// limit, message decoding, command building. Linux only. No arguments.
#include "camera_helper.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace camhelper;

// ---------------------------------------------------------------------------------------------- cap xattr

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i)));
}

constexpr uint32_t kRev1 = 0x01000000u, kRev2 = 0x02000000u, kRev3 = 0x03000000u, kEff = 1u;
constexpr uint32_t kPtrace = 1u << 19;

std::vector<uint8_t> cap(uint32_t rev, uint32_t flags, uint32_t permitted0, uint32_t rootid = 0) {
    std::vector<uint8_t> v;
    put32(v, rev | flags);
    put32(v, permitted0);
    put32(v, 0);  // inheritable[0]
    if (rev == kRev1) return v;
    put32(v, 0);  // permitted[1]
    put32(v, 0);  // inheritable[1]
    if (rev == kRev3) put32(v, rootid);
    return v;
}

bool hasCap(const std::vector<uint8_t>& v) { return capXattrHasPtraceEffective(v.data(), v.size()); }

void testCapXattr() {
    CHECK(hasCap(cap(kRev2, kEff, kPtrace)));
    CHECK(hasCap(cap(kRev3, kEff, kPtrace, 0)));
    CHECK(hasCap(cap(kRev1, kEff, kPtrace)));
    CHECK(hasCap(cap(kRev2, kEff, kPtrace | 1u)));  // other caps alongside are fine
    CHECK(cap(kRev2, kEff, kPtrace).size() == 20);
    CHECK(cap(kRev3, kEff, kPtrace).size() == 24);

    CHECK(!hasCap(cap(kRev2, 0, kPtrace)));          // no effective flag
    CHECK(!hasCap(cap(kRev3, 0, kPtrace)));
    CHECK(!hasCap(cap(kRev2, kEff, 0)));             // no caps at all
    CHECK(!hasCap(cap(kRev2, kEff, 1u << 18)));      // another bit
    CHECK(!hasCap(cap(kRev2, kEff, 1u << 20)));
    CHECK(!hasCap(cap(kRev3, kEff, kPtrace, 1000)));  // namespaced: only valid inside that user namespace
    CHECK(!hasCap(cap(kRev3, kEff, kPtrace, 1)));

    // ptrace only in permitted[1] (that's cap 32+19) doesn't count
    {
        auto v = cap(kRev2, kEff, 0);
        v[12] = 0x08;  // permitted[1] byte 0 (offset 12)... bit 3
        CHECK(!hasCap(v));
        v[12] = 0;
        v[13] = 0x08;  // bit 11 of permitted[1]
        CHECK(!hasCap(v));
    }

    // wrong length / unknown revision / empty
    {
        auto v = cap(kRev2, kEff, kPtrace);
        v.pop_back();
        CHECK(!hasCap(v));
        v = cap(kRev2, kEff, kPtrace);
        v.push_back(0);
        CHECK(!hasCap(v));
        v = cap(kRev3, kEff, kPtrace);
        v.pop_back();
        CHECK(!hasCap(v));
        v = cap(kRev3, kEff, kPtrace);
        v.push_back(0);
        CHECK(!hasCap(v));
        v = cap(kRev2, kEff, kPtrace);
        v.resize(24);  // a rev2 header with a rev3 length
        CHECK(!hasCap(v));
        v = cap(kRev1, kEff, kPtrace);
        v.resize(20);
        CHECK(!hasCap(v));
    }
    CHECK(!hasCap(cap(0x04000000u, kEff, kPtrace)));
    CHECK(!hasCap(cap(0x00000000u, kEff, kPtrace)));
    CHECK(!hasCap(std::vector<uint8_t>{}));
    CHECK(!hasCap(std::vector<uint8_t>{1, 2, 3}));
    CHECK(!capXattrHasPtraceEffective(nullptr, 20));
    CHECK(!capXattrHasPtraceEffective(nullptr, 0));
}

// ---------------------------------------------------------------------------------------------- judgeInstall

FileFacts goodDir() {
    FileFacts d;
    d.exists = d.directory = true;
    d.uid = 0;
    d.gid = 0;
    d.mode = 0755;
    return d;
}

FileFacts goodFile() {
    FileFacts f;
    f.exists = f.regular = true;
    f.uid = 0;
    f.gid = 0;
    f.mode = 0755;
    return f;
}

void testJudgeInstall() {
    const auto goodCap = cap(kRev2, kEff, kPtrace);
    CHECK(judgeInstall(goodDir(), goodFile(), goodCap) == InstallState::Ok);
    CHECK(judgeInstall(goodDir(), goodFile(), cap(kRev3, kEff, kPtrace, 0)) == InstallState::Ok);

    // missing file
    FileFacts none;
    CHECK(judgeInstall(goodDir(), none, goodCap) == InstallState::Missing);
    CHECK(judgeInstall(none, none, {}) == InstallState::Missing);

    // directory
    FileFacts d = goodDir();
    d.uid = 1000;
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);
    d = goodDir();
    d.mode = 0775;  // group writable
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);
    d.mode = 0757;  // other writable
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);
    d.mode = 01777;
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);
    d = goodDir();
    d.mode = 0700;  // root-only is fine for the check
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::Ok);
    d = goodDir();
    d.directory = false;  // e.g. a symlink to a directory
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);
    d = goodDir();
    d.exists = false;  // no directory info but the file exists
    d.directory = false;
    CHECK(judgeInstall(d, goodFile(), goodCap) == InstallState::UnsafeDir);

    // file kind
    FileFacts f = goodFile();
    f.regular = false;  // symlink
    CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::NotRegular);
    f.directory = true;
    CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::NotRegular);

    // owner
    f = goodFile();
    f.uid = 1000;
    CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::WrongOwner);
    f = goodFile();
    f.gid = 1000;
    CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::WrongOwner);
    f.uid = 1000;
    CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::WrongOwner);

    // mode
    for (mode_t m : {mode_t(0775), mode_t(04755), mode_t(0700), mode_t(0757), mode_t(0777), mode_t(0555), mode_t(02755)}) {
        f = goodFile();
        f.mode = m;
        CHECK(judgeInstall(goodDir(), f, goodCap) == InstallState::WrongMode);
    }

    // capability
    CHECK(judgeInstall(goodDir(), goodFile(), {}) == InstallState::NoCapability);
    CHECK(judgeInstall(goodDir(), goodFile(), cap(kRev2, 0, kPtrace)) == InstallState::NoCapability);
    CHECK(judgeInstall(goodDir(), goodFile(), cap(kRev2, kEff, 0)) == InstallState::NoCapability);
    CHECK(judgeInstall(goodDir(), goodFile(), cap(kRev3, kEff, kPtrace, 1000)) == InstallState::NoCapability);

    // every state has some text; the "not installed" message tells how to fix it
    for (auto s : {InstallState::Ok, InstallState::Missing, InstallState::NotRegular, InstallState::WrongOwner,
                   InstallState::WrongMode, InstallState::NoCapability, InstallState::UnsafeDir})
        CHECK(installStateText(s) && installStateText(s)[0] != '?');
    const std::string msg = notInstalledMessage();
    CHECK(msg.find("sudo") != std::string::npos);
    CHECK(msg.find("install-camera-helper.sh") != std::string::npos);

    // the real thing on a path that can't exist
    CHECK(checkInstall("/nonexistent-fph-dir", "/nonexistent-fph-dir/fph-camgrab") == InstallState::Missing);
}

// ---------------------------------------------------------------------------------------------- RespawnLimiter

void testRespawnLimiter() {
    RespawnLimiter l;
    CHECK(l.nextAllowed() == 0.0);
    CHECK(l.tryStart(100.0));
    CHECK(l.nextAllowed() == 105.0);
    CHECK(!l.tryStart(104.9));
    CHECK(!l.tryStart(100.0));
    CHECK(l.nextAllowed() == 105.0);  // a refusal doesn't move the window
    CHECK(l.tryStart(105.0));
    CHECK(!l.tryStart(105.0));
    CHECK(l.tryStart(1000.0));

    // a burst: asked every 0.1 s for 20 s, never two starts closer than 5 s
    RespawnLimiter b;
    int starts = 0;
    double last = -1e9;
    bool spaced = true;
    for (int i = 0; i < 200; i++) {
        const double t = 200.0 + i / 10.0;
        if (b.tryStart(t)) {
            starts++;
            if (t - last < 5.0) spaced = false;
            last = t;
        }
    }
    CHECK(starts == 4);  // 200, 205, 210, 215
    CHECK(spaced);

    // first call at t = 0 is allowed too
    RespawnLimiter z;
    CHECK(z.tryStart(0.0));
    CHECK(!z.tryStart(1.0));

    // custom gap
    RespawnLimiter g(1.0);
    CHECK(g.tryStart(10.0));
    CHECK(!g.tryStart(10.5));
    CHECK(g.tryStart(11.0));
}

// ---------------------------------------------------------------------------------------------- OutwardRetry

void testOutwardRetry() {
    // 1. the review bug: three cams up, one missing, in both message orders
    for (int order = 0; order < 3; order++) {
        OutwardRetry r(2.0, 30);
        const double t = 100.0;
        CHECK(r.nextAt() == 0.0);
        if (order == 0) {  // CAMs first
            r.onCam(0, t);
            r.onCam(1, t);
            r.onCam(3, t);
            r.onGone(2, FPH_GONE_NO_DEVICE, t);
        } else if (order == 1) {  // GONE first
            r.onGone(2, FPH_GONE_NO_DEVICE, t);
            r.onCam(0, t);
            r.onCam(1, t);
            r.onCam(3, t);
        } else {  // interleaved
            r.onCam(0, t);
            r.onGone(2, FPH_GONE_NO_DEVICE, t);
            r.onCam(1, t);
            r.onCam(3, t);
        }
        CHECK(r.nextAt() == t + 2.0);
        CHECK(!r.due(t));
        CHECK(!r.due(t + 1.9));
        CHECK(r.nextAt() == t + 2.0);  // a refused due doesn't move it
        CHECK(r.due(t + 2.0));
        CHECK(r.nextAt() == 0.0);       // cleared until the answers come in
        CHECK(!r.due(t + 2.0));         // and not due twice
        // the answers to that ON: all four up
        for (uint8_t c = 0; c < FPH_OUTWARD_CAMS; c++) r.onCam(c, t + 2.0);
        CHECK(r.nextAt() == 0.0);
        CHECK(!r.gaveUp());
        for (int i = 0; i < 100; i++) CHECK(!r.due(t + 2.0 + i));
    }

    // 2. nothing present: retried every 2 s, no cap
    {
        OutwardRetry r(2.0, 3);
        double t = 10.0;
        int dues = 0;
        for (int round = 0; round < 100; round++) {
            for (uint8_t c = 0; c < FPH_OUTWARD_CAMS; c++) r.onGone(c, FPH_GONE_NO_PROCESS, t);
            CHECK(r.nextAt() == t + 2.0);
            CHECK(!r.due(t + 1.9));
            if (r.due(t + 2.0)) dues++;
            t += 2.0;
        }
        CHECK(dues == 100);
        CHECK(!r.gaveUp());
    }

    // 3. partial: capped, gaveUp, reset clears it
    {
        OutwardRetry r(2.0, 3);
        double t = 0.5;
        int dues = 0;
        auto answers = [&](double at) {
            r.onCam(0, at);
            r.onGone(1, FPH_GONE_NO_DEVICE, at);
            r.onCam(2, at);
            r.onCam(3, at);
        };
        answers(t);
        for (int i = 0; i < 3; i++) {
            CHECK(r.due(t + 2.0));
            dues++;
            t += 2.0;
            answers(t);
        }
        CHECK(dues == 3);
        CHECK(!r.gaveUp());
        CHECK(r.nextAt() == t + 2.0);
        CHECK(!r.due(t + 2.0));  // the fourth is refused
        CHECK(r.gaveUp());
        CHECK(r.nextAt() == 0.0);
        answers(t + 2.0);  // more answers don't re-arm it
        CHECK(r.nextAt() == 0.0);
        CHECK(!r.due(t + 100.0));
        CHECK(r.gaveUp());

        r.reset();
        CHECK(!r.gaveUp());
        CHECK(r.nextAt() == 0.0);
        t = 50.0;
        answers(t);
        CHECK(r.nextAt() == t + 2.0);
        for (int i = 0; i < 3; i++) {  // the counter restarted: three more allowed
            CHECK(r.due(t + 2.0));
            t += 2.0;
            answers(t);
        }
        CHECK(!r.due(t + 2.0));
        CHECK(r.gaveUp());
    }

    // 4. OFF and DENIED are final
    {
        OutwardRetry r(2.0, 30);
        for (uint8_t c = 0; c < FPH_OUTWARD_CAMS; c++) r.onGone(c, FPH_GONE_DENIED, 5.0);
        CHECK(r.nextAt() == 0.0);
        CHECK(!r.due(1000.0));
        for (uint8_t c = 0; c < FPH_OUTWARD_CAMS; c++) r.onGone(c, FPH_GONE_OFF, 5.0);
        CHECK(r.nextAt() == 0.0);
        CHECK(!r.due(1000.0));

        r.reset();
        r.onGone(0, FPH_GONE_DENIED, 5.0);
        r.onGone(1, FPH_GONE_DENIED, 5.0);
        r.onGone(2, FPH_GONE_NO_DEVICE, 5.0);
        r.onGone(3, FPH_GONE_DENIED, 5.0);
        CHECK(r.nextAt() == 7.0);
        CHECK(r.due(7.0));

        // other retryable reasons
        for (uint8_t reason : {uint8_t(FPH_GONE_NO_PROCESS), uint8_t(FPH_GONE_NO_DEVICE), uint8_t(FPH_GONE_FORMAT),
                               uint8_t(FPH_GONE_FAILED)}) {
            OutwardRetry q(2.0, 30);
            q.onGone(1, reason, 1.0);
            CHECK(q.nextAt() == 3.0);
        }
        // a cam that comes back clears its own missing state
        OutwardRetry q(2.0, 30);
        q.onGone(1, FPH_GONE_NO_DEVICE, 1.0);
        CHECK(q.nextAt() == 3.0);
        q.onCam(1, 1.5);
        CHECK(q.nextAt() == 0.0);
        CHECK(!q.due(3.0));
    }

    // 5. out-of-range cam ids are ignored
    {
        OutwardRetry r(2.0, 30);
        r.onGone(FPH_OUTWARD_CAMS, FPH_GONE_NO_DEVICE, 1.0);
        r.onGone(255, FPH_GONE_NO_PROCESS, 1.0);
        CHECK(r.nextAt() == 0.0);
        r.onCam(4, 1.0);
        r.onCam(255, 1.0);
        CHECK(r.nextAt() == 0.0);
        CHECK(!r.due(100.0));
        // and don't disturb a real state
        r.onGone(0, FPH_GONE_NO_DEVICE, 2.0);
        r.onCam(255, 2.5);
        r.onGone(255, FPH_GONE_OFF, 2.5);
        CHECK(r.nextAt() == 4.0);
    }
}

// ---------------------------------------------------------------------------------------------- decodeMessage

bool fdClosed(int fd) { return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

/** @return n open fds (the read ends of pipes; the write ends are closed right away) */
std::vector<int> openFds(int n) {
    std::vector<int> v;
    for (int i = 0; i < n; i++) {
        int p[2];
        if (pipe(p) != 0) {
            std::printf("FAIL pipe: %s\n", std::strerror(errno));
            ++failures;
            break;
        }
        close(p[1]);
        v.push_back(p[0]);
    }
    return v;
}

void closeAllFds(const std::vector<int>& v) {
    for (int fd : v) close(fd);
}

template <typename T>
void hdr(T& m, uint16_t type) {
    m.hdr.magic = FPH_MSG_MAGIC;
    m.hdr.type = type;
    m.hdr.version = FPH_PROTO_VERSION;
}

template <typename T>
DecodeError dec(const T& m, std::vector<int> fds, Message& out) {
    return decodeMessage(&m, sizeof(m), std::move(fds), out);
}

fph_msg_cam goodCam(uint8_t nbufs) {
    fph_msg_cam c{};
    hdr(c, FPH_MSG_CAM);
    c.cam = 2;
    c.node = 9;
    c.nbufs = nbufs;
    c.gen = 3;
    c.width = 1056;
    c.height = 1024;
    c.pitch = 1152;
    for (unsigned i = 0; i < nbufs && i < FPH_MAX_BUFS; i++) {
        c.bufs[i].index = i;
        c.bufs[i].length = 1769472;
        c.bufs[i].ino = 100 + i;
    }
    return c;
}

void testDecode() {
    Message m;

    // HELLO
    {
        fph_msg_hello h{};
        hdr(h, FPH_MSG_HELLO);
        h.helper_pid = 1234;
        h.has_ptrace = 1;
        CHECK(dec(h, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_HELLO && m.hello.helper_pid == 1234 && m.hello.has_ptrace == 1);
        CHECK(m.fds.size() == 0);
    }

    // CAM with nbufs fds: ownership moves into the message, they stay open until it's destroyed
    {
        auto fds = openFds(3);
        const auto c = goodCam(3);
        CHECK(dec(c, fds, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_CAM && m.cam.cam == 2 && m.cam.node == 9 && m.cam.nbufs == 3 && m.cam.gen == 3);
        CHECK(m.cam.pitch == 1152 && m.cam.bufs[2].ino == 102);
        CHECK(m.fds.size() == 3);
        CHECK(m.fds[0] == fds[0] && m.fds[1] == fds[1] && m.fds[2] == fds[2]);
        for (int fd : fds) CHECK(!fdClosed(fd));
        m = Message{};  // drops the FdList
        for (int fd : fds) CHECK(fdClosed(fd));
    }
    // CAM with the maximum number of buffers
    {
        auto fds = openFds(FPH_MAX_BUFS);
        const auto c = goodCam(FPH_MAX_BUFS);
        CHECK(dec(c, fds, m) == DecodeError::None);
        CHECK(m.fds.size() == FPH_MAX_BUFS);
        m = Message{};
        for (int fd : fds) CHECK(fdClosed(fd));
    }

    // CAM_GONE / EYE_GONE
    {
        fph_msg_gone g{};
        hdr(g, FPH_MSG_CAM_GONE);
        g.cam = 1;
        g.reason = FPH_GONE_DENIED;
        g.err = EPERM;
        CHECK(dec(g, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_CAM_GONE && m.gone.cam == 1 && m.gone.reason == FPH_GONE_DENIED && m.gone.err == EPERM);
        hdr(g, FPH_MSG_EYE_GONE);
        g.cam = 0;
        CHECK(dec(g, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_EYE_GONE);
        g.cam = FPH_OUTWARD_CAMS;
        CHECK(dec(g, {}, m) == DecodeError::BadFields);
    }

    // EYE
    {
        fph_msg_eye e{};
        hdr(e, FPH_MSG_EYE);
        e.narenas = 2;
        e.gen = 1;
        e.arenas[0] = {16777216, 5};
        e.arenas[1] = {8388608, 6};
        auto fds = openFds(2);
        CHECK(dec(e, fds, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_EYE && m.eye.narenas == 2 && m.eye.arenas[1].ino == 6 && m.fds.size() == 2);
        m = Message{};
        for (int fd : fds) CHECK(fdClosed(fd));
        e.narenas = 0;
        CHECK(dec(e, {}, m) == DecodeError::BadFields);
        e.narenas = FPH_MAX_EYE_ARENAS + 1;
        fds = openFds(FPH_MAX_EYE_ARENAS + 1);
        CHECK(dec(e, fds, m) == DecodeError::BadFields);
        for (int fd : fds) CHECK(fdClosed(fd));
    }

    // FRAMES
    {
        fph_msg_frames f{};
        hdr(f, FPH_MSG_FRAMES);
        f.cam = 3;
        f.n = 2;
        f.gen = 9;
        f.entries[0] = {4, 100, 111};
        f.entries[1] = {5, 101, 222};
        CHECK(dec(f, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_FRAMES && m.frames.n == 2 && m.frames.entries[1].seq == 101 && m.frames.gen == 9);
        f.n = FPH_MAX_BUFS;
        CHECK(dec(f, {}, m) == DecodeError::None);
        f.n = FPH_MAX_BUFS + 1;
        CHECK(dec(f, {}, m) == DecodeError::BadFields);
        f.n = 0;
        CHECK(dec(f, {}, m) == DecodeError::None);  // an empty report is legal, the agent ignores it
        f.n = 1;
        f.cam = FPH_OUTWARD_CAMS;
        CHECK(dec(f, {}, m) == DecodeError::BadFields);
    }

    // RECHECK
    {
        fph_msg_recheck r{};
        hdr(r, FPH_MSG_RECHECK);
        r.cam = 1;
        r.result = FPH_RECHECK_REQUEUED;
        r.gen = 2;
        r.index = 7;
        r.seq = 55;
        CHECK(dec(r, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_RECHECK && m.recheck.result == FPH_RECHECK_REQUEUED && m.recheck.index == 7 &&
              m.recheck.seq == 55);
        r.result = FPH_RECHECK_INVALID;
        CHECK(dec(r, {}, m) == DecodeError::None);
        r.result = FPH_RECHECK_INVALID + 1;
        CHECK(dec(r, {}, m) == DecodeError::BadFields);
        r.result = 0;
        r.cam = 4;
        CHECK(dec(r, {}, m) == DecodeError::BadFields);
    }

    // ERROR, BYE
    {
        fph_msg_error e{};
        hdr(e, FPH_MSG_ERROR);
        e.code = FPH_ERR_BAD_OP;
        CHECK(dec(e, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_ERROR && m.error.code == FPH_ERR_BAD_OP);
        fph_msg_bye b{};
        hdr(b, FPH_MSG_BYE);
        b.reason = FPH_BYE_XRSERVICE_EXITED;
        CHECK(dec(b, {}, m) == DecodeError::None);
        CHECK(m.type == FPH_MSG_BYE && m.bye.reason == FPH_BYE_XRSERVICE_EXITED);
    }

    // header problems
    {
        fph_msg_hello h{};
        hdr(h, FPH_MSG_HELLO);
        CHECK(decodeMessage(&h, 0, {}, m) == DecodeError::TooShort);
        CHECK(decodeMessage(&h, 7, {}, m) == DecodeError::TooShort);
        h.hdr.magic = 0;
        CHECK(dec(h, {}, m) == DecodeError::BadMagic);
        h.hdr.magic = FPH_CMD_MAGIC;  // a command's magic isn't a message's
        CHECK(dec(h, {}, m) == DecodeError::BadMagic);
        hdr(h, FPH_MSG_HELLO);
        h.hdr.version = FPH_PROTO_VERSION + 1;
        CHECK(dec(h, {}, m) == DecodeError::BadVersion);
        h.hdr.version = 0;
        CHECK(dec(h, {}, m) == DecodeError::BadVersion);
        hdr(h, 0);
        CHECK(dec(h, {}, m) == DecodeError::BadType);
        hdr(h, FPH_MSG_BYE + 1);
        CHECK(dec(h, {}, m) == DecodeError::BadType);
        hdr(h, 0xFFFF);
        CHECK(dec(h, {}, m) == DecodeError::BadType);
        // wrong length for the type: shorter and longer
        hdr(h, FPH_MSG_HELLO);
        CHECK(decodeMessage(&h, sizeof(h) - 1, {}, m) == DecodeError::BadLength);
        unsigned char big[sizeof(h) + 8];
        std::memset(big, 0, sizeof(big));
        std::memcpy(big, &h, sizeof(h));
        CHECK(decodeMessage(big, sizeof(h) + 1, {}, m) == DecodeError::BadLength);
        CHECK(decodeMessage(big, sizeof(h) + 8, {}, m) == DecodeError::BadLength);
        CHECK(decodeMessage(big, sizeof(fph_msg_hdr), {}, m) == DecodeError::BadLength);  // header only
        // a CAM-typed header with a HELLO-sized body
        hdr(h, FPH_MSG_CAM);
        CHECK(dec(h, {}, m) == DecodeError::BadLength);
    }

    // errors close the fds they were given, whatever the error
    {
        auto fds = openFds(2);
        fph_msg_hello h{};
        hdr(h, FPH_MSG_HELLO);
        h.hdr.magic = 1;
        CHECK(dec(h, fds, m) == DecodeError::BadMagic);
        for (int fd : fds) CHECK(fdClosed(fd));

        fds = openFds(2);
        hdr(h, FPH_MSG_HELLO);
        h.hdr.version = 99;
        CHECK(dec(h, fds, m) == DecodeError::BadVersion);
        for (int fd : fds) CHECK(fdClosed(fd));

        fds = openFds(2);
        hdr(h, 77);
        CHECK(dec(h, fds, m) == DecodeError::BadType);
        for (int fd : fds) CHECK(fdClosed(fd));

        fds = openFds(2);
        hdr(h, FPH_MSG_HELLO);
        CHECK(decodeMessage(&h, 3, fds, m) == DecodeError::TooShort);
        for (int fd : fds) CHECK(fdClosed(fd));

        fds = openFds(2);
        CHECK(decodeMessage(&h, sizeof(h) - 1, fds, m) == DecodeError::BadLength);
        for (int fd : fds) CHECK(fdClosed(fd));
    }

    // CAM field checks (the fds are closed on each)
    {
        auto c = goodCam(0);
        auto fds = openFds(1);
        CHECK(dec(c, fds, m) == DecodeError::BadFields);
        for (int fd : fds) CHECK(fdClosed(fd));

        c = goodCam(FPH_MAX_BUFS);
        c.nbufs = FPH_MAX_BUFS + 1;
        fds = openFds(FPH_MAX_BUFS + 1);
        CHECK(dec(c, fds, m) == DecodeError::BadFields);
        for (int fd : fds) CHECK(fdClosed(fd));

        c = goodCam(2);
        c.cam = FPH_OUTWARD_CAMS;
        fds = openFds(2);
        CHECK(dec(c, fds, m) == DecodeError::BadFields);
        for (int fd : fds) CHECK(fdClosed(fd));
    }

    // fd count mismatch: FdMismatch and every fd closed
    {
        auto c = goodCam(3);
        auto fds = openFds(2);  // too few
        CHECK(dec(c, fds, m) == DecodeError::FdMismatch);
        for (int fd : fds) CHECK(fdClosed(fd));
        CHECK(m.fds.size() == 0);

        fds = openFds(4);  // too many
        CHECK(dec(c, fds, m) == DecodeError::FdMismatch);
        for (int fd : fds) CHECK(fdClosed(fd));

        CHECK(dec(c, {}, m) == DecodeError::FdMismatch);  // none

        fph_msg_hello h{};
        hdr(h, FPH_MSG_HELLO);
        fds = openFds(1);  // fds on a message that takes none
        CHECK(dec(h, fds, m) == DecodeError::FdMismatch);
        for (int fd : fds) CHECK(fdClosed(fd));

        fph_msg_frames f{};
        hdr(f, FPH_MSG_FRAMES);
        fds = openFds(1);
        CHECK(dec(f, fds, m) == DecodeError::FdMismatch);
        for (int fd : fds) CHECK(fdClosed(fd));

        fph_msg_eye e{};
        hdr(e, FPH_MSG_EYE);
        e.narenas = 2;
        fds = openFds(1);
        CHECK(dec(e, fds, m) == DecodeError::FdMismatch);
        for (int fd : fds) CHECK(fdClosed(fd));
    }

    // a failed decode resets `out` (no stale type / fds)
    {
        auto fds = openFds(2);
        const auto c = goodCam(2);
        CHECK(dec(c, fds, m) == DecodeError::None);
        fph_msg_hello h{};
        hdr(h, FPH_MSG_HELLO);
        h.hdr.magic = 0;
        CHECK(dec(h, {}, m) == DecodeError::BadMagic);
        CHECK(m.fds.size() == 0);
        for (int fd : fds) CHECK(fdClosed(fd));  // the old message's fds were released when `out` was reset
    }

    for (auto e : {DecodeError::None, DecodeError::TooShort, DecodeError::BadMagic, DecodeError::BadVersion,
                   DecodeError::BadType, DecodeError::BadLength, DecodeError::BadFields, DecodeError::FdMismatch})
        CHECK(decodeErrorText(e) && decodeErrorText(e)[0] != '?');
}

// ---------------------------------------------------------------------------------------------- FdList / makeCommand

void testFdListAndCommand() {
    {
        auto fds = openFds(2);
        FdList a;
        a.add(fds[0]);
        a.add(fds[1]);
        FdList b(std::move(a));
        CHECK(a.size() == 0 && b.size() == 2);
        auto released = b.release();
        CHECK(released.size() == 2 && b.size() == 0);
        CHECK(!fdClosed(fds[0]) && !fdClosed(fds[1]));
        closeAllFds(released);
        CHECK(fdClosed(fds[0]) && fdClosed(fds[1]));
    }
    {
        auto fds = openFds(2);
        FdList a;
        a.add(fds[0]);
        FdList b;
        b.add(fds[1]);
        b = std::move(a);  // closes b's old fd
        CHECK(fdClosed(fds[1]) && !fdClosed(fds[0]));
        b.closeAll();
        CHECK(fdClosed(fds[0]));
    }

    const fph_cmd c = makeCommand(FPH_OP_RECHECK, 2, 9, 1234);
    CHECK(sizeof(c) == 16);
    CHECK(c.magic == FPH_CMD_MAGIC && c.op == FPH_OP_RECHECK && c.cam == 2 && c.index == 9 && c.seq == 1234 &&
          c.reserved == 0);
    const fph_cmd d = makeCommand(FPH_OP_OUTWARD_ON);
    CHECK(d.magic == FPH_CMD_MAGIC && d.op == FPH_OP_OUTWARD_ON && d.cam == 0 && d.index == 0 && d.seq == 0 &&
          d.reserved == 0);
    for (auto op : {FPH_OP_OUTWARD_ON, FPH_OP_OUTWARD_OFF, FPH_OP_EYE_ON, FPH_OP_EYE_OFF})
        CHECK(makeCommand(op).op == uint8_t(op));
}

}  // namespace

int main() {
    testCapXattr();
    testJudgeInstall();
    testRespawnLimiter();
    testOutwardRetry();
    testDecode();
    testFdListAndCommand();
    if (failures) {
        std::printf("camera_helper_test: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("camera_helper_test: all passed\n");
    return 0;
}
