// T10: the camera thread. It owns the camera helper (spawned from this thread, which lives as long as the agent, so
// the helper's PR_SET_PDEATHSIG follows it), maps the dmabufs the helper hands over read-only, copies frames with
// cache maintenance, keeps an outward frame only after the helper's post-copy recheck says KEEP, filters the dark
// exposures, finds and reads the eye ring, and hands finished frames to the main loop through a single-slot mailbox
// per camera + an eventfd (latest frame wins, so nothing queues up behind a slow consumer).
//
// Cameras off → the helper isn't running and the thread sleeps in poll() with no timeout: zero cost.
#pragma once

#include "camera_frame.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/** What the main loop wants from the cameras right now. */
struct CameraWants {
    uint8_t display = 0;    ///< cameras shown in the headset (their widget is on and the HUD is shown); bit = camera id
    uint8_t pc = 0;         ///< cameras a PC asked for whose widget is on
    bool fullSize = false;  ///< outward frames for the headset at full size (default 2x down)
    bool worn = true;       ///< the HMD isn't in standby (the cameras pause in standby)
    double displayFps[fph::cam::kCameraCount] = {15, 15, 15, 15, 15, 15};  ///< per camera (cam::displayFpsInLayout)
    double pcFps = 10.0;

    bool operator==(const CameraWants& o) const {
        for (uint8_t c = 0; c < fph::cam::kCameraCount; ++c)
            if (displayFps[c] != o.displayFps[c]) return false;
        return display == o.display && pc == o.pc && fullSize == o.fullSize && worn == o.worn && pcFps == o.pcFps;
    }
    bool operator!=(const CameraWants& o) const { return !(*this == o); }
    uint8_t any() const { return static_cast<uint8_t>(display | pc); }
};

/** A finished frame for a headset tile. */
struct CameraImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  ///< width × height × 4, opaque
    uint32_t frameId = 0;
};

/** A finished JPEG for the PC. */
struct CameraJpeg {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> data;
    uint32_t frameId = 0;
};

class CameraService {
public:
    CameraService();
    ~CameraService();
    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    /** Starts the thread (once). @return false if the eventfds couldn't be made */
    bool start();
    /** Stops the thread (and with it the helper). */
    void stop();

    /** The eventfd the main loop polls: readable when a frame or a state change is waiting. */
    int eventFd() const { return eventFd_; }
    /** Drains the eventfd (after poll() said it's readable). */
    void clearEvent();

    /** Main thread: what's wanted now. Cheap when unchanged. */
    void setWants(const CameraWants& wants);

    /** Main thread: the newest frame for a headset tile, if there's one it hasn't taken. */
    bool takeImage(uint8_t camera, CameraImage& out);
    /** Main thread: the newest JPEG for the PC, if there's one it hasn't taken. */
    bool takeJpeg(uint8_t camera, CameraJpeg& out);

    /** @return what a camera's tile shows when it has no live frame */
    fph::cam::CameraState state(uint8_t camera) const;
    /** @return bumps whenever any state() changes (the panel redraws its camera cards then) */
    uint64_t stateRevision() const { return stateRevision_.load(); }

    /** @return one paragraph for the debug dump: helper, per-camera counters and the thread's CPU cost */
    std::string debugText() const;

private:
    friend class CameraWorker;  // the thread's state lives in camera_service.cpp

    struct Mailbox {
        std::mutex lock;
        CameraImage image;
        bool imageFresh = false;
        CameraJpeg jpeg;
        bool jpegFresh = false;
    };

    void run();
    /** @return the wants, and whether they changed since the last call */
    CameraWants readWants(bool& changed);
    void post(uint8_t camera, CameraImage* image, CameraJpeg* jpeg);
    void setState(uint8_t camera, fph::cam::CameraState state);
    void signal();

    std::thread thread_;
    std::atomic<bool> stop_ {false};
    int eventFd_ = -1;  ///< camera thread → main loop
    int wakeFd_ = -1;   ///< main loop → camera thread (wants changed, stop)

    mutable std::mutex wantsLock_;
    CameraWants wants_;
    bool wantsDirty_ = false;

    Mailbox boxes_[fph::cam::kCameraCount];
    std::atomic<uint8_t> states_[fph::cam::kCameraCount];
    std::atomic<uint64_t> stateRevision_ {1};

    mutable std::mutex debugLock_;
    std::string debug_;  ///< written by the thread once a second
};
