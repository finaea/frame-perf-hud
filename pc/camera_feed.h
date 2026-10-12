// T10: the PC side of the camera widgets. Takes reassembled Camera BLOBs (LinkClient::onBlob; the BlobAssembler
// keys them by stream id = camera, so two cameras never cancel each other), decodes the JPEG (libjpeg-turbo), keeps
// the newest picture per camera, and paints them into the camera widgets' tiles on top of the panel. When a camera
// has no live picture, the panel's card shows its state text instead (core/render/widgets.cpp).
#pragma once

#include "camera_frame.h"
#include "grid_renderer.h"

#include <cairo.h>

#include <cstdint>
#include <vector>

namespace fph {
namespace pc {

class CameraFeed {
public:
    /** A picture older than this (no new BLOB) stops counting as live: the card shows "Starting..." again. */
    static constexpr double kStaleSec = 2.0;

    CameraFeed();
    ~CameraFeed();
    CameraFeed(const CameraFeed&) = delete;
    CameraFeed& operator=(const CameraFeed&) = delete;

    /**
     * One reassembled Camera BLOB.
     * @param stream the BLOB's stream id (must match the header's camera)
     * @return false if malformed or the JPEG didn't decode (nothing changes)
     */
    bool onBlob(uint8_t stream, const uint8_t* data, size_t size, double now);

    /** Forget everything (back to discovery: the headset's frame ids restart). */
    void reset();

    /** @return what a camera's card shows: Live while pictures arrive, else the headset's last reported state */
    cam::CameraState state(uint8_t camera, double now) const;
    /** @return bumps when any state(…, now) answer or picture changes (redraw) */
    uint64_t revision() const { return revision_; }
    /** Call on the UI tick: moves stale cameras out of Live (bumps revision). */
    void tick(double now);

    /** @return the SUBSCRIBE wants bits for the cameras in this layout */
    static uint32_t wantsFor(const Layout& layout);

    /**
     * Paints every live camera's picture into its tiles.
     * @param cr the window's context, with the panel already drawn at (0, 0) and `scale` device px per panel px
     */
    void paint(cairo_t* cr, const GridGeometry& geometry, double scale, double now) const;

    /** @return the newest picture's size (0 if none), for tests */
    int width(uint8_t camera) const { return camera < cam::kCameraCount ? cams_[camera].width : 0; }
    int height(uint8_t camera) const { return camera < cam::kCameraCount ? cams_[camera].height : 0; }
    uint32_t frameId(uint8_t camera) const { return camera < cam::kCameraCount ? cams_[camera].frameId : 0; }
    uint64_t decoded() const { return decoded_; }
    uint64_t rejected() const { return rejected_; }

private:
    struct Cam {
        cam::CameraState reported = cam::CameraState::Starting;
        bool havePicture = false;
        int width = 0;
        int height = 0;
        uint32_t frameId = 0;
        double at = -1e9;
        cairo_surface_t* surface = nullptr;  ///< RGB24, width × height
        bool live = false;  ///< as of the last tick / blob
    };

    void* tj_ = nullptr;  ///< tjhandle
    Cam cams_[cam::kCameraCount];
    std::vector<uint8_t> grey_;
    uint64_t revision_ = 1;
    uint64_t decoded_ = 0;
    uint64_t rejected_ = 0;
};

}  // namespace pc
}  // namespace fph
