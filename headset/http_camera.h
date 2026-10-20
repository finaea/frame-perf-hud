// HTTP camera (14): the thread that reads the widget's stream: any MJPEG-over-HTTP stream (e.g. a mouth camera
// from Toggle's mouthcam on 127.0.0.1:8082). Its own class, not part of CameraService, so the helper code stays as it is;
// same shape though: an eventfd to the main loop and a single-slot mailbox (latest frame wins).
// - Connected only while wanted: the widget is on and the HUD is shown on a worn headset, or a PC asked for it.
//   Otherwise the socket is closed and the thread sleeps in poll() with no timeout.
// - `GET path HTTP/1.0` + Host (Toggle's preview client, toggles/src/preview.cpp), 1 s connect timeout, retried
//   every second. Every frame is read (the server pushes them all); only the newest complete JPEG is used.
// - Headset tile: at the widget's fps, TurboJPEG decodes the luma (half size unless Full res), turned by the
//   widget's rotation. PC: the JPEG goes out as it came (the PC decodes and turns it).
#pragma once

#include "camera_frame.h"
#include "camera_service.h"  // CameraImage, CameraJpeg

#include <atomic>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

/** What the main loop wants from the HTTP camera right now. */
struct HttpCameraWants {
    std::string url;        ///< the live widget's address ("" = no widget)
    bool display = false;   ///< tile frames: the widget is on, the HUD shown and the headset worn
    bool pc = false;        ///< JPEGs: a PC asked for camera 6 and the widget is on
    bool worn = true;       ///< for the state text when nothing is wanted
    double displayFps = 15.0;
    double pcFps = 10.0;
    bool fullSize = false;  ///< tile frames at full size (default: half)
    uint8_t rotation = 0;   ///< clockwise quarter turns for the tile frames

    bool active() const { return !url.empty() && (display || pc); }
    bool operator==(const HttpCameraWants& o) const {
        return url == o.url && display == o.display && pc == o.pc && worn == o.worn && displayFps == o.displayFps &&
               pcFps == o.pcFps && fullSize == o.fullSize && rotation == o.rotation;
    }
    bool operator!=(const HttpCameraWants& o) const { return !(*this == o); }
};

class HttpCamera {
public:
    HttpCamera();
    ~HttpCamera();
    HttpCamera(const HttpCamera&) = delete;
    HttpCamera& operator=(const HttpCamera&) = delete;

    /** Starts the thread (once). @return false if the eventfds couldn't be made */
    bool start();
    void stop();

    /** The eventfd the main loop polls: readable when a frame or a state change is waiting. */
    int eventFd() const { return eventFd_; }
    void clearEvent();

    /** Main thread: what's wanted now. Cheap when unchanged; a new address reconnects. */
    void setWants(const HttpCameraWants& wants);

    /** Main thread: the newest tile frame, if there's one it hasn't taken. */
    bool takeImage(CameraImage& out);
    /** Main thread: the newest JPEG for the PCs, if there's one it hasn't taken. */
    bool takeJpeg(CameraJpeg& out);

    fph::cam::CameraState state() const { return static_cast<fph::cam::CameraState>(state_.load()); }
    /** @return bumps whenever state() changes */
    uint64_t stateRevision() const { return stateRevision_.load(); }

    /** @return one paragraph for the debug dump */
    std::string debugText() const;

    /** Frames handled since start (for --http-camera-bench). */
    struct Counters {
        uint64_t framesIn = 0;    ///< complete JPEGs off the stream
        uint64_t decoded = 0;     ///< tile frames made
        uint64_t relayed = 0;     ///< JPEGs handed out for the PCs
        double decodeMs = 0.0;    ///< total decode + rotate + RGBA time
        uint64_t bytesIn = 0;
    };
    Counters counters() const;

private:
    friend class HttpCameraWorker;

    void run();
    HttpCameraWants readWants(bool& changed);
    void post(CameraImage* image, CameraJpeg* jpeg);
    void setState(fph::cam::CameraState state);
    void signal();

    std::thread thread_;
    std::atomic<bool> stop_ {false};
    int eventFd_ = -1;  ///< thread → main loop
    int wakeFd_ = -1;   ///< main loop → thread (wants changed, stop)

    mutable std::mutex wantsLock_;
    HttpCameraWants wants_;
    bool wantsDirty_ = false;

    std::mutex boxLock_;
    CameraImage image_;
    bool imageFresh_ = false;
    CameraJpeg jpeg_;
    bool jpegFresh_ = false;

    std::atomic<uint8_t> state_;
    std::atomic<uint64_t> stateRevision_ {1};

    mutable std::mutex debugLock_;
    std::string debug_;
    Counters counters_;  ///< under debugLock_
};

/** --http-camera-bench: the thread on its own against a real stream, for its cost (hidden option). */
struct HttpCameraBenchOptions {
    std::string url = fph::kDefaultHttpCameraUrl;
    double seconds = 20.0;
    double fps = 15.0;        ///< tile frame rate (the widget's FPS)
    bool pc = false;          ///< also relay JPEGs, as if a PC asked (at 10 fps)
    bool fullSize = false;
    uint8_t rotation = 0;     ///< quarter turns
    std::string snapshotDir;  ///< empty = none; else the first tile frame as http-camera.pgm
};

/** @return exit code (1 if no frame came) */
int runHttpCameraBench(const HttpCameraBenchOptions& options, const volatile std::sig_atomic_t& stop);
