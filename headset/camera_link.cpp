// T10: see camera_link.h
#include "camera_link.h"

#include <algorithm>

namespace cam = fph::cam;

uint8_t CameraLink::wanted(const std::vector<CameraTarget>& targets, uint8_t widgetCameras) {
    uint8_t mask = 0;
    for (const CameraTarget& t : targets) mask |= t.cameras;
    return static_cast<uint8_t>(mask & widgetCameras);
}

double CameraLink::fps(const std::vector<CameraTarget>& targets) {
    double best = 0.0;
    for (const CameraTarget& t : targets)
        if (t.cameras) best = std::max(best, t.fps);
    return best > 0.0 ? std::clamp(best, 1.0, 30.0) : 10.0;
}

void CameraLink::pump(const Source& source, const std::vector<CameraTarget>& targets, uint8_t widgetCameras,
                      double now, const Send& send) {
    const uint8_t want = wanted(targets, widgetCameras);
    for (uint8_t c = 0; c < cam::kCameraCount; ++c) {
        const uint8_t bit = static_cast<uint8_t>(1u << c);
        CameraJpeg jpeg;
        const bool fresh = source.takeJpeg(c, jpeg);  // always drained, so a stale one never goes out later
        if (!(want & bit)) continue;
        std::vector<uint8_t> blob;
        if (fresh && !jpeg.data.empty()) {
            cam::BlobHeader h;
            h.camera = c;
            h.state = cam::CameraState::Live;
            h.width = jpeg.width;
            h.height = jpeg.height;
            h.frameId = jpeg.frameId;
            blob = cam::encodeCameraBlob(h, jpeg.data.data(), jpeg.data.size());
            nextState_[c] = now + kStateEverySec;
        } else {
            const cam::CameraState s = source.state(c);
            if (s == cam::CameraState::Live || now < nextState_[c]) continue;
            nextState_[c] = now + kStateEverySec;
            cam::BlobHeader h;
            h.camera = c;
            h.state = s;
            blob = cam::encodeCameraBlob(h, nullptr, 0);
        }
        for (const CameraTarget& t : targets) {
            if (!(t.cameras & bit)) continue;
            send(t.to, c, blob);
            if (blob.size() > cam::BlobHeader::kSize) {
                ++framesSent_;
                bytesSent_ += blob.size();
            }
        }
    }
}
