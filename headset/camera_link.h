// T10: camera frames to the PCs. Each lease holder that asked for a camera (SUBSCRIBE wants bits 8-13) whose
// widget is on gets that camera's JPEGs as Camera BLOBs (stream id = camera), at its camera address (the PC's
// dongle path when it registered one, else its lease address). A camera with no live frames sends its state
// (not worn, helper missing, ...) once a second instead, so the PC's card says why. Single-threaded: main loop.
#pragma once

#include "camera_frame.h"
#include "camera_service.h"
#include "link_server.h"

#include <cstdint>
#include <functional>
#include <vector>

/** One PC's camera subscription (LinkServer::cameraTargets). */
using CameraTarget = LinkCameraTarget;

class CameraLink {
public:
    using Send = std::function<void(const LinkEndpoint& to, uint8_t camera, const std::vector<uint8_t>& blob)>;

    static constexpr double kStateEverySec = 1.0;

    /**
     * Sends what's new: fresh JPEGs to every target that wants that camera, and the state of wanted cameras that
     * aren't live (at most once a second each).
     * @param widgetCameras cameras whose widget is on (bit = camera id)
     */
    void pump(CameraService& service, const std::vector<CameraTarget>& targets, uint8_t widgetCameras, double now,
              const Send& send);

    /** @return cameras some target wants whose widget is on */
    static uint8_t wanted(const std::vector<CameraTarget>& targets, uint8_t widgetCameras);
    /** @return the highest fps any target asked for (1-30; 10 if none said) */
    static double fps(const std::vector<CameraTarget>& targets);

    uint64_t framesSent() const { return framesSent_; }
    uint64_t bytesSent() const { return bytesSent_; }

private:
    double nextState_[fph::cam::kCameraCount] = {};
    uint64_t framesSent_ = 0;
    uint64_t bytesSent_ = 0;
};
