// T10: see camera_service.h
#include "camera_service.h"

#include "camera_cache.h"
#include "camera_eye_ring.h"
#include "camera_helper.h"

#include <turbojpeg.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using fph::cam::CameraState;
namespace cam = fph::cam;

namespace {

constexpr double kLiveHoldSec = 1.5;      ///< a camera counts as live this long after its last shown frame
constexpr double kFrameGapResetSec = 0.5; ///< frames resuming after this gap relearn the dark parity
constexpr double kInstallRecheckSec = 5.0;
constexpr double kEyeRetrySec = 2.0;
constexpr double kEyePollSec = 0.010;     ///< locked ring: fingerprint poll
constexpr double kEyeStopSec = 10.0;      ///< locked ring silent this long: re-locate it (framestream)
constexpr double kEyeAlignCheckSec = 20.0;
constexpr double kEyeOtherSearchSec = 5.0;  ///< one eye only: look for the other this often
constexpr int kJpegQuality = 75;
constexpr int kDownscaleAbove = 800;  ///< outward frames wider than this are halved (1056×1024 → 528×512)

double monoNow() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

double threadCpu() {
    timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

struct Mapping {
    const uint8_t* ptr = nullptr;
    size_t len = 0;
};

void unmapAll(std::vector<Mapping>& maps) {
    for (auto& m : maps)
        if (m.ptr) munmap(const_cast<uint8_t*>(m.ptr), m.len);
    maps.clear();
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

/** Grey (w×h, stride w) → downscaled grey (w/ds × h/ds). */
void shrink(const uint8_t* src, int w, int h, int ds, std::vector<uint8_t>& out, int& ow, int& oh) {
    ow = ds > 1 ? w / 2 : w;
    oh = ds > 1 ? h / 2 : h;
    out.resize(static_cast<size_t>(ow) * oh);
    cam::copyLuma(out.data(), src, w, h, static_cast<size_t>(w), ds, false);
}

}  // namespace

/** Everything the camera thread owns. */
class CameraWorker {
public:
    explicit CameraWorker(CameraService& svc) : svc_(svc), finder_(eyeCache()), reader_(eyeCache(), &camcache::dropRange) {
        jpeg_ = tjInitCompress();
        cpuMark_ = threadCpu();
        wallMark_ = monoNow();
    }
    ~CameraWorker() {
        stopHelper();
        if (jpeg_) tjDestroy(jpeg_);
    }

    void loop();

private:
    struct OutCam {
        bool present = false;
        fph_msg_cam info {};
        std::vector<Mapping> maps;  ///< by V4L2 buffer index
        camhelper::FdList fds;
        cam::RecheckGate gate;
        cam::DarkParityFilter dark;
        cam::RateGate displayGate {15.0};
        cam::RateGate pcGate {10.0};
        cam::ShownWatermark watermark;   ///< newest sequence shown from these buffers (reset with them)
        double lastReportFrames = -1e9;  ///< last FRAMES with entries
        double lastLive = -1e9;
        bool gone = false;
        uint8_t goneReason = 0;
        std::vector<uint8_t> scratch;  ///< the copy in flight (full size, stride = width)
        uint32_t frameId = 0;
        // counters since the last debug line
        unsigned copies = 0, keeps = 0, darkSkipped = 0, shown = 0, jpegs = 0;
        unsigned discard[5] = {};
        size_t jpegBytes = 0;
    };

    static eyering::Cache eyeCache() {
        eyering::Cache c;
        c.dropLine = &camcache::dropLine;
        c.barrier = &camcache::barrier;
        return c;
    }

    CameraService& svc_;
    CameraWants wants_;
    camhelper::HelperProcess helper_;
    camhelper::RespawnLimiter limiter_;
    camhelper::OutwardRetry outwardRetry_;
    bool outwardOn_ = false;  ///< ON sent to the running helper
    bool eyeOn_ = false;
    bool helperMissing_ = false;
    bool helperOutdated_ = false;
    double nextInstallCheck_ = 0.0;
    double outdatedUntil_ = 0.0;
    double eyeRetryAt_ = 0.0;
    OutCam out_[cam::kOutwardCount];

    // eyes
    std::vector<Mapping> eyeMaps_;
    camhelper::FdList eyeFds_;
    bool eyeGone_ = false;
    uint8_t eyeGoneReason_ = 0;
    uint32_t eyeGen_ = 0;  ///< bumps on every EYE / EYE_GONE (a discovery in progress aborts)
    eyering::EyeRingFinder finder_;
    eyering::RingReader reader_;
    double nextEyePoll_ = 0.0;
    eyering::SearchSchedule search_;  ///< unlocked: change tracking + discovery attempts (work and wake-up)
    double lastOtherSearch_ = 0.0;
    double otherSearchFrom_ = 0.0;
    double lastAlignCheck_ = 0.0;
    int misaligned_ = 0;
    cam::RateGate eyeDisplayGate_[2] = {cam::RateGate {15.0}, cam::RateGate {15.0}};
    cam::RateGate eyePcGate_[2] = {cam::RateGate {10.0}, cam::RateGate {10.0}};
    double eyeLive_[2] = {-1e9, -1e9};
    uint32_t eyeFrameId_[2] = {0, 0};
    std::vector<uint8_t> eyeScratch_ = std::vector<uint8_t>(eyering::kEyeFrameBytes);
    unsigned eyeShown_[2] = {0, 0}, eyeJpegs_[2] = {0, 0}, eyeTorn_ = 0, eyeDiscoveries_ = 0;

    tjhandle jpeg_ = nullptr;
    std::vector<uint8_t> grey_;

    double cpuMark_ = 0.0;
    double wallMark_ = 0.0;
    double nextDebug_ = 0.0;
    double cpuPct_ = 0.0;

    void applyWants(double t);
    void ensureHelper(double t);
    void stopHelper();
    void dropOutward(int c);
    void dropEyes();
    void serviceHelper();
    void onCam(camhelper::Message& m, double t);
    void onFrames(const fph_msg_frames& f, double t);
    void onRecheck(const fph_msg_recheck& r, double t);
    void publishOutward(int c, double t);
    void eyeWork(double t);
    bool eyeWait(uint32_t gen);
    void pollEyes(double t);
    void publish(uint8_t camera, const uint8_t* grey, int w, int h, bool forDisplay, bool forPc, int displayDs,
                 int pcDs, uint32_t frameId);
    void updateStates(double t);
    double nextWake(double t) const;
    void writeDebug(double t);
    bool wantedEyes() const { return (wants_.any() & 0x30) != 0; }
    bool wantedOutward() const { return (wants_.any() & 0x0F) != 0; }
};

// ------------------------------------------------------------------------------------------------ helper

void CameraWorker::applyWants(double t) {
    // a camera with no rate (no widget shows it) keeps its last one: its display bit is off anyway
    for (int c = 0; c < cam::kOutwardCount; ++c) {
        if (wants_.displayFps[c] > 0.0) out_[c].displayGate.setFps(wants_.displayFps[c]);
        out_[c].pcGate.setFps(wants_.pcFps);
    }
    for (int e = 0; e < 2; ++e) {
        if (wants_.displayFps[cam::kEyeBase + e] > 0.0) eyeDisplayGate_[e].setFps(wants_.displayFps[cam::kEyeBase + e]);
        eyePcGate_[e].setFps(wants_.pcFps);
    }
    if (!wants_.any()) {
        if (helper_.running()) {
            std::fprintf(stderr, "[camera] no camera wanted: stopping the helper\n");
            stopHelper();
        }
        return;
    }
    if (!helper_.running()) return;  // ensureHelper turns the right parts on
    if (wantedOutward() != outwardOn_) {
        outwardOn_ = wantedOutward();
        helper_.sendOutward(outwardOn_);
        outwardRetry_.reset();
        if (!outwardOn_)
            for (int c = 0; c < cam::kOutwardCount; ++c) dropOutward(c);
    }
    if (wantedEyes() != eyeOn_) {
        eyeOn_ = wantedEyes();
        helper_.sendEye(eyeOn_);
        eyeRetryAt_ = 0.0;
        if (!eyeOn_) dropEyes();
    }
    (void)t;
}

void CameraWorker::ensureHelper(double t) {
    if (!wants_.any() || helper_.running()) return;
    if (helperOutdated_ && t < outdatedUntil_) return;
    if (t < nextInstallCheck_ && helperMissing_) return;
    const camhelper::InstallState install = camhelper::checkInstall();
    if (install != camhelper::InstallState::Ok) {
        if (!helperMissing_)
            std::fprintf(stderr, "[camera] helper %s: %s\n", camhelper::kHelperPath, camhelper::installStateText(install));
        helperMissing_ = true;
        nextInstallCheck_ = t + kInstallRecheckSec;
        return;
    }
    helperMissing_ = false;
    if (!limiter_.tryStart(t)) return;
    std::string error;
    if (!helper_.start(camhelper::kHelperPath, &error)) {
        std::fprintf(stderr, "[camera] starting the helper failed: %s\n", error.c_str());
        return;
    }
    std::fprintf(stderr, "[camera] helper started (pid %d): outward %s, eyes %s\n", int(helper_.pid()),
                 wantedOutward() ? "on" : "off", wantedEyes() ? "on" : "off");
    helperOutdated_ = false;
    outwardOn_ = wantedOutward();
    eyeOn_ = wantedEyes();
    if (outwardOn_) helper_.sendOutward(true);
    if (eyeOn_) helper_.sendEye(true);
    outwardRetry_.reset();
    eyeRetryAt_ = 0.0;
}

void CameraWorker::stopHelper() {
    helper_.stop();
    for (int c = 0; c < cam::kOutwardCount; ++c) dropOutward(c);
    dropEyes();
    outwardOn_ = eyeOn_ = false;
    outwardRetry_.reset();
    eyeRetryAt_ = 0.0;
}

void CameraWorker::dropOutward(int c) {
    OutCam& o = out_[c];
    unmapAll(o.maps);
    o.fds.closeAll();
    o.present = false;
    o.gate.onGone();
    o.dark.reset();
    // a fresh CAM (new gen) may restart the sequence numbers: forget this stream's watermark and timestamps
    o.watermark.reset();
    o.lastReportFrames = -1e9;
    o.lastLive = -1e9;
}

void CameraWorker::dropEyes() {
    reader_.unlock();
    finder_.clear();
    unmapAll(eyeMaps_);
    eyeFds_.closeAll();
    ++eyeGen_;
}

void CameraWorker::serviceHelper() {
    for (int guard = 0; guard < 256 && helper_.running(); ++guard) {
        camhelper::Message m;
        camhelper::DecodeError why = camhelper::DecodeError::None;
        const auto r = helper_.read(m, &why);
        const double t = monoNow();
        if (r == camhelper::HelperProcess::ReadResult::WouldBlock) return;
        if (r == camhelper::HelperProcess::ReadResult::Closed) {
            std::fprintf(stderr, "[camera] helper exited; respawn allowed in %.1f s\n",
                         std::max(0.0, limiter_.nextAllowed() - t));
            stopHelper();
            return;
        }
        if (r == camhelper::HelperProcess::ReadResult::Bad) {
            std::fprintf(stderr, "[camera] bad message from the helper: %s\n", camhelper::decodeErrorText(why));
            if (why == camhelper::DecodeError::BadVersion) {
                helperOutdated_ = true;
                outdatedUntil_ = t + 60.0;
                stopHelper();
                return;
            }
            continue;
        }
        switch (m.type) {
            case FPH_MSG_HELLO:
                if (!m.hello.has_ptrace) std::fprintf(stderr, "[camera] helper runs without cap_sys_ptrace\n");
                break;
            case FPH_MSG_CAM: onCam(m, t); break;
            case FPH_MSG_CAM_GONE: {
                const int c = m.gone.cam;
                dropOutward(c);
                out_[c].gone = true;
                out_[c].goneReason = m.gone.reason;
                outwardRetry_.onGone(m.gone.cam, m.gone.reason, t);
                if (m.gone.reason != FPH_GONE_OFF)
                    std::fprintf(stderr, "[camera] %s gone (reason %u, errno %d)\n", cam::cameraName(uint8_t(c)),
                                 m.gone.reason, m.gone.err);
                break;
            }
            case FPH_MSG_EYE: {
                dropEyes();
                std::vector<eyering::Arena> arenas;
                for (size_t i = 0; i < m.fds.size(); i++) {
                    const size_t len = size_t(m.eye.arenas[i].size);
                    void* p = mmap(nullptr, len, PROT_READ, MAP_SHARED, m.fds[i], 0);
                    if (p == MAP_FAILED) continue;
                    eyeMaps_.push_back({static_cast<const uint8_t*>(p), len});
                    arenas.push_back({static_cast<const uint8_t*>(p), len});
                }
                eyeFds_ = std::move(m.fds);
                finder_.setArenas(arenas);
                finder_.clearBlockChanges();
                eyeGone_ = false;
                eyeRetryAt_ = 0.0;
                search_.reset(t);  // give change tracking a second before the first attempt
                std::fprintf(stderr, "[camera] eye arenas: %zu mapped\n", arenas.size());
                break;
            }
            case FPH_MSG_EYE_GONE:
                dropEyes();
                eyeGone_ = true;
                eyeGoneReason_ = m.gone.reason;
                if (wantedEyes() && m.gone.reason != FPH_GONE_OFF && m.gone.reason != FPH_GONE_DENIED && eyeRetryAt_ == 0.0)
                    eyeRetryAt_ = t + kEyeRetrySec;
                break;
            case FPH_MSG_FRAMES: onFrames(m.frames, t); break;
            case FPH_MSG_RECHECK: onRecheck(m.recheck, t); break;
            case FPH_MSG_ERROR: std::fprintf(stderr, "[camera] helper rejected a command: code %u\n", m.error.code); break;
            case FPH_MSG_BYE: std::fprintf(stderr, "[camera] helper says bye (reason %u)\n", m.bye.reason); break;
        }
    }
}

void CameraWorker::onCam(camhelper::Message& m, double t) {
    const int c = m.cam.cam;
    OutCam& o = out_[c];
    dropOutward(c);
    o.info = m.cam;
    o.gone = false;
    outwardRetry_.onCam(m.cam.cam, t);
    const fph_msg_cam& info = m.cam;
    const bool formatOk = (info.pixelformat == fourcc('N', 'V', '1', '2') || info.pixelformat == fourcc('G', 'R', 'E', 'Y')) &&
                          info.width >= 2 && info.height >= 2 && info.width <= 4096 && info.height <= 4096 &&
                          info.pitch >= info.width;
    const size_t need = size_t(info.pitch) * (info.height - 1) + info.width;
    o.maps.assign(FPH_MAX_BUFS, Mapping {});
    int mapped = 0;
    for (size_t i = 0; formatOk && i < m.fds.size(); i++) {
        const fph_buf_info& b = info.bufs[i];
        if (b.index >= FPH_MAX_BUFS || b.length < need) continue;  // can't hold a frame: never read past its end
        void* p = mmap(nullptr, b.length, PROT_READ, MAP_SHARED, m.fds[i], 0);
        if (p == MAP_FAILED) continue;
        o.maps[b.index] = {static_cast<const uint8_t*>(p), b.length};
        ++mapped;
    }
    o.fds = std::move(m.fds);
    if (!formatOk || mapped == 0) {
        std::fprintf(stderr, "[camera] %s: unusable format/buffers (%ux%u pitch %u, %d mapped)\n",
                     cam::cameraName(uint8_t(c)), info.width, info.height, info.pitch, mapped);
        dropOutward(c);
        o.gone = true;
        o.goneReason = FPH_GONE_FORMAT;
        return;
    }
    o.present = true;
    o.gate.onCam(info.gen);
    o.dark.reset();
    o.scratch.resize(size_t(info.width) * info.height);
    std::fprintf(stderr, "[camera] %s gen %u: %ux%u pitch %u, %d buffers mapped\n", cam::cameraName(uint8_t(c)),
                 info.gen, info.width, info.height, info.pitch, mapped);
}

void CameraWorker::onFrames(const fph_msg_frames& f, double t) {
    OutCam& o = out_[f.cam];
    if (!o.present || !o.gate.currentGen(f.gen) || f.n == 0) return;
    if (t - o.lastReportFrames > kFrameGapResetSec) o.dark.reset();  // back from standby: relearn the parity
    o.lastReportFrames = t;
    const uint8_t bit = uint8_t(1u << f.cam);
    const bool forDisplay = (wants_.display & bit) && o.displayGate.ready(t);
    const bool forPc = (wants_.pc & bit) && o.pcGate.ready(t);
    if (!forDisplay && !forPc) return;
    if (!o.gate.canCopy(t)) return;
    const fph_msg_cam& info = o.info;
    const cam::CacheOps cache = camcache::ops();
    // learn the dark parity from the two newest frames (one of each parity), sampled cheaply
    for (int k = std::max(0, int(f.n) - 2); k < f.n; ++k) {
        const fph_frame_entry& e = f.entries[k];
        if (e.index >= o.maps.size() || !o.maps[e.index].ptr) continue;
        o.dark.observe(e.seq, cam::sampleLevel(o.maps[e.index].ptr, int(info.width), int(info.height), info.pitch, cache));
    }
    if (!o.dark.known()) return;  // a single frame so far: wait for the other parity rather than risk a dark one
    std::vector<cam::FrameEntry> entries(f.n);
    for (int k = 0; k < f.n; ++k) entries[k] = {f.entries[k].index, f.entries[k].seq};
    const int pick = cam::pickFrame(entries, o.dark, o.watermark);
    if (pick < 0) return;
    o.darkSkipped += unsigned(f.n - 1 - pick);
    const cam::FrameEntry e = entries[size_t(pick)];
    if (e.index >= o.maps.size() || !o.maps[e.index].ptr) return;
    cam::copyLuma(o.scratch.data(), o.maps[e.index].ptr, int(info.width), int(info.height), info.pitch, 1, false, cache);
    ++o.copies;
    // shown only if the helper confirms the buffer still held this frame after the copy (never a torn one)
    o.gate.begin(e.index, e.seq, t);
    if (!helper_.sendRecheck(f.cam, e.index, e.seq)) o.gate.onCam(info.gen);  // not sent: nothing in flight
}

void CameraWorker::onRecheck(const fph_msg_recheck& r, double t) {
    OutCam& o = out_[r.cam];
    const cam::RecheckGate::Verdict v = o.gate.onRecheck(r.gen, r.index, r.seq, r.result);
    if (v == cam::RecheckGate::Verdict::Ignore) return;
    if (v == cam::RecheckGate::Verdict::Discard) {
        ++o.discard[std::min<int>(r.result, 4)];
        return;
    }
    ++o.keeps;
    o.watermark.shown(r.seq);
    publishOutward(r.cam, t);
}

void CameraWorker::publishOutward(int c, double t) {
    OutCam& o = out_[c];
    const uint8_t bit = uint8_t(1u << c);
    const bool forDisplay = (wants_.display & bit) && o.displayGate.take(t);
    const bool forPc = (wants_.pc & bit) && o.pcGate.take(t);
    if (!forDisplay && !forPc) return;
    o.lastLive = t;
    const int w = int(o.info.width), h = int(o.info.height);
    const int big = w > kDownscaleAbove ? 2 : 1;
    publish(uint8_t(c), o.scratch.data(), w, h, forDisplay, forPc, wants_.fullSize ? 1 : big, big, ++o.frameId);
    if (forDisplay) ++o.shown;
    if (forPc) ++o.jpegs;
}

void CameraWorker::publish(uint8_t camera, const uint8_t* grey, int w, int h, bool forDisplay, bool forPc,
                           int displayDs, int pcDs, uint32_t frameId) {
    CameraImage image;
    CameraJpeg jpeg;
    if (forDisplay) {
        int ow = 0, oh = 0;
        shrink(grey, w, h, displayDs, grey_, ow, oh);
        image.width = ow;
        image.height = oh;
        image.rgba.resize(size_t(ow) * oh * 4);
        cam::greyToRgba(image.rgba.data(), grey_.data(), size_t(ow) * oh);
        image.frameId = frameId;
    }
    if (forPc && jpeg_) {
        int ow = 0, oh = 0;
        shrink(grey, w, h, pcDs, grey_, ow, oh);
        unsigned char* buf = nullptr;
        unsigned long size = 0;
        if (tjCompress2(jpeg_, grey_.data(), ow, ow, oh, TJPF_GRAY, &buf, &size, TJSAMP_GRAY, kJpegQuality, TJFLAG_FASTDCT) == 0) {
            jpeg.width = uint16_t(ow);
            jpeg.height = uint16_t(oh);
            jpeg.data.assign(buf, buf + size);
            jpeg.frameId = frameId;
            if (camera < cam::kOutwardCount) out_[camera].jpegBytes += size;
        } else {
            forPc = false;
        }
        if (buf) tjFree(buf);
    }
    svc_.post(camera, forDisplay ? &image : nullptr, forPc ? &jpeg : nullptr);
}

// ------------------------------------------------------------------------------------------------ eyes

bool CameraWorker::eyeWait(uint32_t gen) {
    // between discovery's samples: keep the helper read (it exits if not read for 1 s) and the outward cams served
    pollfd p[2] = {{helper_.fd(), POLLIN, 0}, {svc_.wakeFd_, POLLIN, 0}};
    ::poll(p, 2, 4);
    if (p[1].revents & POLLIN) {
        uint64_t v;
        (void)!::read(svc_.wakeFd_, &v, sizeof(v));
    }
    bool changed = false;
    const CameraWants w = svc_.readWants(changed);
    if (changed) {
        wants_ = w;
        applyWants(monoNow());
    }
    serviceHelper();
    const double t = monoNow();
    if (reader_.locked()) pollEyes(t);
    updateStates(t);
    return !svc_.stop_.load() && gen == eyeGen_ && wantedEyes() && wants_.worn && helper_.running();
}

void CameraWorker::eyeWork(double t) {
    if (!wantedEyes() || finder_.arenas().empty()) return;
    if (reader_.locked()) {
        if (t >= nextEyePoll_) {
            nextEyePoll_ = t + kEyePollSec;
            pollEyes(t);
        }
        if (t - reader_.lastChange() > kEyeStopSec) {
            std::fprintf(stderr, "[camera] eye frames stopped, re-locating the ring\n");
            reader_.unlock();
            finder_.resetBlockChanges();
            search_.restart(t);
            return;
        }
        // framestream: re-check the alignment now and then; 3 misses in a row = the ring moved
        if (t - lastAlignCheck_ > kEyeAlignCheckSec && t - reader_.lastChange() < 1.0) {
            lastAlignCheck_ = t;
            const eyering::Ring& ring = reader_.ring();
            const uint8_t* p0 = finder_.arenas()[size_t(ring.arena)].map + ring.off[0];
            camcache::dropRange(p0, eyering::kEyeFrameBytes);
            if (eyering::looksLikeEyeFrame(p0)) {
                misaligned_ = 0;
            } else if (++misaligned_ >= 3) {
                std::fprintf(stderr, "[camera] eye frame alignment drifted, re-locking\n");
                misaligned_ = 0;
                reader_.unlock();
                finder_.resetBlockChanges();
                search_.restart(t);
                return;
            }
        }
        // one eye only (the proximity sensor covered rather than worn): look for a ring with both every 5 s
        if (!reader_.ring().bothEyes && wants_.worn) {
            if (otherSearchFrom_ == 0.0 && t - lastOtherSearch_ > kEyeOtherSearchSec) {
                finder_.resetBlockChanges();
                otherSearchFrom_ = t;
            } else if (otherSearchFrom_ != 0.0 && t - otherSearchFrom_ > 0.5) {
                finder_.noteBlockChanges();
                eyering::Ring found;
                const uint32_t gen = eyeGen_;
                if (finder_.discover(found, [&] { return eyeWait(gen); }, &monoNow) && found.bothEyes && gen == eyeGen_) {
                    reader_.lock(found, finder_.arenas()[size_t(found.arena)], monoNow());
                    std::fprintf(stderr, "[camera] found both eyes: ring of %d slots, pitch %ld\n", found.slots, found.pitch);
                }
                otherSearchFrom_ = 0.0;
                lastOtherSearch_ = monoNow();
            }
        }
        return;
    }
    if (!wants_.worn) return;  // the eye cams only run while worn: don't scan idle memory
    // change tracking costs a pass over the arenas: in slow mode only in the second before an attempt
    if (search_.noteDue(t)) {
        search_.noted(t);
        finder_.noteBlockChanges();
    }
    if (!search_.scanDue(t)) return;
    search_.scanned(t);
    eyering::Ring found;
    const uint32_t gen = eyeGen_;
    ++eyeDiscoveries_;
    if (finder_.discover(found, [&] { return eyeWait(gen); }, &monoNow) && gen == eyeGen_) {
        reader_.lock(found, finder_.arenas()[size_t(found.arena)], monoNow());
        lastAlignCheck_ = monoNow();
        misaligned_ = 0;
        lastOtherSearch_ = monoNow();
        otherSearchFrom_ = 0.0;
        std::fprintf(stderr, "[camera] eye ring locked: %d slots at 0x%zx, pitch %ld, %s\n", found.slots, found.off[0],
                     found.pitch, found.bothEyes ? "both eyes" : "one eye so far");
    } else if (gen == eyeGen_) {
        finder_.clearBlockChanges();
        search_.scanned(monoNow());
    }
}

void CameraWorker::pollEyes(double t) {
    int settled[2];
    reader_.poll(t, settled);
    for (int e = 0; e < 2; ++e) {
        if (settled[e] < 0) continue;
        const uint8_t camera = uint8_t(cam::kEyeBase + e);
        const uint8_t bit = uint8_t(1u << camera);
        const bool forDisplay = (wants_.display & bit) && eyeDisplayGate_[e].ready(t);
        const bool forPc = (wants_.pc & bit) && eyePcGate_[e].ready(t);
        if (!forDisplay && !forPc) continue;
        // framestream's orientation fix: physical eye 1 is mounted upside down (opt_flip = {0, FLIP_V})
        if (!reader_.copySlot(settled[e], eyeScratch_.data(), e == 1)) {
            ++eyeTorn_;
            continue;
        }
        const bool d = forDisplay && eyeDisplayGate_[e].take(t);
        const bool p = forPc && eyePcGate_[e].take(t);
        eyeLive_[e] = t;
        publish(camera, eyeScratch_.data(), eyering::kEyeStride, eyering::kEyeHeight, d, p, 1, 1, ++eyeFrameId_[e]);
        if (d) ++eyeShown_[e];
        if (p) ++eyeJpegs_[e];
    }
}

// ------------------------------------------------------------------------------------------------ states

void CameraWorker::updateStates(double t) {
    const uint8_t any = wants_.any();
    for (uint8_t c = 0; c < cam::kCameraCount; ++c) {
        CameraState s = CameraState::Starting;
        const bool live = cam::isOutward(c) ? t - out_[c].lastLive < kLiveHoldSec
                                            : t - eyeLive_[c - cam::kEyeBase] < kLiveHoldSec;
        if (!(any & (1u << c))) {
            s = CameraState::Off;
        } else if (helperMissing_) {
            s = CameraState::HelperMissing;
        } else if (helperOutdated_) {
            s = CameraState::HelperOutdated;
        } else if (live) {
            s = CameraState::Live;
        } else if (!wants_.worn) {
            s = CameraState::NotWorn;
        } else if (cam::isOutward(c)) {
            const OutCam& o = out_[c];
            if (!o.present && o.gone)
                s = o.goneReason == FPH_GONE_DENIED ? CameraState::HelperMissing
                    : (o.goneReason == FPH_GONE_OFF ? CameraState::Starting : CameraState::Unavailable);
        } else if (finder_.arenas().empty() && eyeGone_) {
            s = eyeGoneReason_ == FPH_GONE_DENIED ? CameraState::HelperMissing : CameraState::Unavailable;
        }
        svc_.setState(c, s);
    }
}

double CameraWorker::nextWake(double t) const {
    double at = 1e18;
    if (wants_.any()) {
        at = t + 0.5;  // states (live → not live) and respawn checks
        if (!helper_.running()) at = std::min(at, std::max(limiter_.nextAllowed(), t + 0.1));
        if (outwardRetry_.nextAt() > 0.0) at = std::min(at, outwardRetry_.nextAt());
        if (eyeRetryAt_ > 0.0) at = std::min(at, eyeRetryAt_);
        if (wantedEyes() && !finder_.arenas().empty()) {
            if (reader_.locked()) at = std::min(at, nextEyePoll_);
            else if (wants_.worn) at = std::min(at, search_.nextWake(t));  // the same deadlines eyeWork uses
        }
        at = std::min(at, nextDebug_);
    }
    return at;
}

void CameraWorker::writeDebug(double t) {
    const double cpu = threadCpu();
    const double wall = t - wallMark_;
    if (wall <= 0.0) return;
    cpuPct_ = 100.0 * (cpu - cpuMark_) / wall;
    char line[512];
    std::string text = "== Cameras ==\n";
    std::snprintf(line, sizeof(line),
                  "helper %s%s%s | wants display 0x%02x pc 0x%02x full %d worn %d | camera thread CPU %.2f%% of one "
                  "core over %.1f s\n",
                  helper_.running() ? "running" : "stopped", helperMissing_ ? " (not installed)" : "",
                  helperOutdated_ ? " (outdated)" : "", wants_.display, wants_.pc, int(wants_.fullSize),
                  int(wants_.worn), cpuPct_, wall);
    text += line;
    for (int c = 0; c < cam::kOutwardCount; ++c) {
        OutCam& o = out_[c];
        if (!(wants_.any() & (1u << c))) continue;
        std::snprintf(line, sizeof(line),
                      "%s: %s state %d | copies %u keep %u discard newer %u requeued %u stale %u invalid %u | dark "
                      "skipped %u | shown %u, jpeg %u (%.0f kB) | dark level even %.1f odd %.1f\n",
                      cam::cameraName(uint8_t(c)), o.present ? "mapped" : "absent", int(svc_.state(uint8_t(c))),
                      o.copies, o.keeps, o.discard[1], o.discard[2], o.discard[3], o.discard[4], o.darkSkipped, o.shown,
                      o.jpegs, o.jpegBytes / 1024.0, o.dark.level(0), o.dark.level(1));
        text += line;
        o.copies = o.keeps = o.darkSkipped = o.shown = o.jpegs = 0;
        std::memset(o.discard, 0, sizeof(o.discard));
        o.jpegBytes = 0;
    }
    if (wantedEyes()) {
        std::snprintf(line, sizeof(line),
                      "eyes: %zu arenas, ring %s (%d slots, %s) | shown %u / %u, jpeg %u / %u, torn copies dropped %u, "
                      "discoveries %u\n",
                      finder_.arenas().size(), reader_.locked() ? "locked" : "searching", reader_.ring().slots,
                      reader_.ring().bothEyes ? "both eyes" : "one eye", eyeShown_[0], eyeShown_[1], eyeJpegs_[0],
                      eyeJpegs_[1], eyeTorn_, eyeDiscoveries_);
        text += line;
        eyeShown_[0] = eyeShown_[1] = eyeJpegs_[0] = eyeJpegs_[1] = eyeTorn_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(svc_.debugLock_);
        svc_.debug_ = text;
    }
    cpuMark_ = cpu;
    wallMark_ = t;
}

void CameraWorker::loop() {
    while (!svc_.stop_.load()) {
        bool changed = false;
        const CameraWants w = svc_.readWants(changed);
        double t = monoNow();
        if (changed) {
            wants_ = w;
            applyWants(t);
        }
        ensureHelper(t);
        if (helper_.running()) {
            if (outwardOn_ && outwardRetry_.due(t)) helper_.sendOutward(true);
            if (eyeOn_ && eyeRetryAt_ > 0.0 && t >= eyeRetryAt_) {
                eyeRetryAt_ = 0.0;
                helper_.sendEye(true);
            }
        }
        eyeWork(t);
        updateStates(t);
        if (wants_.any() && t >= nextDebug_) {
            writeDebug(t);
            nextDebug_ = t + 10.0;
        }
        t = monoNow();
        const double at = nextWake(t);
        const int timeoutMs = at >= 1e17 ? -1 : std::max(0, int(std::ceil((at - t) * 1000.0)));
        pollfd p[2] = {{svc_.wakeFd_, POLLIN, 0}, {helper_.fd(), POLLIN, 0}};
        const int n = ::poll(p, helper_.running() ? 2 : 1, timeoutMs);
        if (n > 0 && (p[0].revents & POLLIN)) {
            uint64_t v;
            (void)!::read(svc_.wakeFd_, &v, sizeof(v));
        }
        serviceHelper();
    }
    stopHelper();
}

// ------------------------------------------------------------------------------------------------ service

CameraService::CameraService() {
    for (auto& s : states_) s.store(static_cast<uint8_t>(CameraState::Off));
}

CameraService::~CameraService() {
    stop();
    if (eventFd_ >= 0) ::close(eventFd_);
    if (wakeFd_ >= 0) ::close(wakeFd_);
}

bool CameraService::start() {
    if (thread_.joinable()) return true;
    if (eventFd_ < 0) eventFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd_ < 0 || wakeFd_ < 0) return false;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void CameraService::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    const uint64_t one = 1;
    (void)!::write(wakeFd_, &one, sizeof(one));
    thread_.join();
}

void CameraService::run() {
    CameraWorker worker(*this);
    worker.loop();
}

void CameraService::clearEvent() {
    uint64_t v;
    if (eventFd_ >= 0) (void)!::read(eventFd_, &v, sizeof(v));
}

void CameraService::signal() {
    const uint64_t one = 1;
    if (eventFd_ >= 0) (void)!::write(eventFd_, &one, sizeof(one));
}

void CameraService::setWants(const CameraWants& wants) {
    {
        std::lock_guard<std::mutex> lock(wantsLock_);
        if (wants == wants_) return;
        wants_ = wants;
        wantsDirty_ = true;
    }
    const uint64_t one = 1;
    if (wakeFd_ >= 0) (void)!::write(wakeFd_, &one, sizeof(one));
}

CameraWants CameraService::readWants(bool& changed) {
    std::lock_guard<std::mutex> lock(wantsLock_);
    changed = wantsDirty_;
    wantsDirty_ = false;
    return wants_;
}

void CameraService::post(uint8_t camera, CameraImage* image, CameraJpeg* jpeg) {
    if (camera >= fph::cam::kCameraCount || (!image && !jpeg)) return;
    Mailbox& box = boxes_[camera];
    {
        std::lock_guard<std::mutex> lock(box.lock);
        if (image) {
            box.image = std::move(*image);
            box.imageFresh = true;
        }
        if (jpeg) {
            box.jpeg = std::move(*jpeg);
            box.jpegFresh = true;
        }
    }
    signal();
}

bool CameraService::takeImage(uint8_t camera, CameraImage& out) {
    if (camera >= fph::cam::kCameraCount) return false;
    Mailbox& box = boxes_[camera];
    std::lock_guard<std::mutex> lock(box.lock);
    if (!box.imageFresh) return false;
    std::swap(out, box.image);  // hands the buffer over; the thread's next frame reuses `out`'s old allocation
    box.imageFresh = false;
    return true;
}

bool CameraService::takeJpeg(uint8_t camera, CameraJpeg& out) {
    if (camera >= fph::cam::kCameraCount) return false;
    Mailbox& box = boxes_[camera];
    std::lock_guard<std::mutex> lock(box.lock);
    if (!box.jpegFresh) return false;
    std::swap(out, box.jpeg);
    box.jpegFresh = false;
    return true;
}

void CameraService::setState(uint8_t camera, CameraState state) {
    if (camera >= fph::cam::kCameraCount) return;
    if (states_[camera].exchange(static_cast<uint8_t>(state)) != static_cast<uint8_t>(state)) {
        stateRevision_.fetch_add(1);
        signal();
    }
}

CameraState CameraService::state(uint8_t camera) const {
    if (camera >= fph::cam::kCameraCount) return CameraState::Off;
    return static_cast<CameraState>(states_[camera].load());
}

std::string CameraService::debugText() const {
    std::lock_guard<std::mutex> lock(debugLock_);
    return debug_.empty() ? "== Cameras ==\noff\n" : debug_;
}
