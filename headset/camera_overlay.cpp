// T10: see camera_overlay.h
#include "camera_overlay.h"

#include "placement.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool check(const char* what, vr::EVROverlayError e) {
    if (e == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[camera] %s failed: %s\n", what, vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
    return false;
}

}  // namespace

void CameraOverlays::setGeometry(const fph::GridGeometry& geometry) {
    std::vector<Tile> next;
    for (const fph::WidgetSlot& slot : geometry.slots) {
        for (const fph::cam::Tile& t : fph::cam::cameraTiles(slot.widget)) {
            Tile tile;
            tile.camera = t.camera;
            tile.tile = t;
            tile.slotX = slot.x;
            tile.slotY = slot.y;
            next.push_back(std::move(tile));
        }
    }
    // reuse overlays (and textures, when the camera stays the same) by position
    for (size_t k = 0; k < next.size() && k < tiles_.size(); ++k) {
        next[k].handle = tiles_[k].handle;
        next[k].curvature = tiles_[k].curvature;  // R2: the overlay's, so it goes with the handle
        tiles_[k].handle = 0;
        if (next[k].camera == tiles_[k].camera) {
            next[k].texture = std::move(tiles_[k].texture);
            next[k].retired = std::move(tiles_[k].retired);
            next[k].texW = tiles_[k].texW;
            next[k].texH = tiles_[k].texH;
            next[k].hasFrame = tiles_[k].hasFrame;
        } else {
            if (next[k].handle != 0) vr::VROverlay()->HideOverlay(next[k].handle);  // wait for this camera's frame
            bury(std::move(tiles_[k].texture));
            bury(std::move(tiles_[k].retired));
        }
    }
    for (size_t k = next.size(); k < tiles_.size(); ++k) destroyTile(tiles_[k]);
    tiles_ = std::move(next);
    geometry_ = geometry;
    ++geometryRevision_;
    for (Tile& t : tiles_) {
        t.placedRevision = 0;
        t.visible = false;  // re-applied below / on the next frame
        if (t.handle) vr::VROverlay()->HideOverlay(t.handle);
        place(t);
        updateVisibility(t);
    }
}

void CameraOverlays::follow(const PanelPlacement& panel) {
    if (!graveyard_.empty()) freeBuried(false);
    const bool moved = panel.revision != panel_.revision;
    panel_ = panel;
    if (!moved) return;
    for (Tile& t : tiles_) {
        place(t);
        updateVisibility(t);
    }
}

void CameraOverlays::place(Tile& t) {
    if (t.handle == 0 || !t.hasFrame || !panel_.valid) return;
    const uint64_t key = (panel_.poseRevision + 1) ^ (geometryRevision_ << 40);
    if (t.placedRevision == key) return;
    const fph::cam::Rect local = fph::cam::fitFrame(t.tile, t.texW, t.texH);
    const fph::cam::Rect rect {t.slotX + local.x, t.slotY + local.y, local.w, local.h};
    const TilePlacement p = placeTile(rect, geometry_, panel_);
    vr::IVROverlay* overlay = vr::VROverlay();
    check("SetOverlayWidthInMeters(camera)", overlay->SetOverlayWidthInMeters(t.handle, p.widthM));
    // R2: only when it changed (place() runs on every pose update, e.g. at the laser grab's rate)
    if (p.curvature != t.curvature &&
        check("SetOverlayCurvature(camera)", overlay->SetOverlayCurvature(t.handle, p.curvature))) {
        t.curvature = p.curvature;
    }
    const vr::HmdMatrix34_t m = composeTransform(panel_.transform, p.offset);
    const bool ok = panel_.absolute
                        ? check("SetOverlayTransformAbsolute(camera)",
                                overlay->SetOverlayTransformAbsolute(t.handle, vr::TrackingUniverseStanding, &m))
                        : check("SetOverlayTransformTrackedDeviceRelative(camera)",
                                overlay->SetOverlayTransformTrackedDeviceRelative(t.handle, panel_.device, &m));
    if (ok) t.placedRevision = key;
}

void CameraOverlays::updateVisibility(Tile& t) {
    if (t.handle == 0) return;
    const bool want = panel_.valid && panel_.shown && t.hasFrame && (live_ & (1u << t.camera)) &&
                      t.placedRevision != 0;
    vr::IVROverlay* overlay = vr::VROverlay();
    if (want) overlay->SetOverlayAlpha(t.handle, panel_.alpha);
    if (want == t.visible) return;
    if (want ? check("ShowOverlay(camera)", overlay->ShowOverlay(t.handle))
             : check("HideOverlay(camera)", overlay->HideOverlay(t.handle))) {
        t.visible = want;
    }
}

void CameraOverlays::onFrame(uint8_t camera, const CameraImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) return;
    if (!panel_.valid || !panel_.shown || panel_.alpha <= 0.001f) {  // hidden / faded out: nobody sees it
        ++skippedHidden_;
        return;
    }
    for (size_t k = 0; k < tiles_.size(); ++k) {
        Tile& t = tiles_[k];
        if (t.camera != camera) continue;
        vr::IVROverlay* overlay = vr::VROverlay();
        if (t.handle == 0) {
            vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
            const std::string key = "finaea.frame-perf-hud.camera" + std::to_string(k);
            const std::string name = std::string("Frame Perf HUD ") + fph::cam::cameraName(camera);
            if (!check("CreateOverlay(camera)", overlay->CreateOverlay(key.c_str(), name.c_str(), &h))) continue;
            t.handle = h;
            t.curvature = 0.0f;  // R2: a new overlay is flat
            overlay->SetOverlaySortOrder(h, 1);  // above the panel it sits on
            t.placedRevision = 0;
        }
        std::string error;
        if (t.retired) {  // off-screen for a whole frame interval: safe to free now
            t.retired->destroy();
            t.retired.reset();
        }
        if (!t.texture || t.texW != image.width || t.texH != image.height) {
            auto next = std::make_unique<OverlayTexture>();
            if (!next->create(vulkan_, image.width, image.height, error)) {
                std::fprintf(stderr, "[camera] can't create a %dx%d texture: %s\n", image.width, image.height,
                             error.c_str());
                continue;
            }
            t.retired = std::move(t.texture);
            t.texture = std::move(next);
            t.texW = image.width;
            t.texH = image.height;
            t.placedRevision = 0;  // the picture's aspect decides its rect
        }
        const double t0 = nowMs();
        if (!t.texture->updateNoWait(t.handle, image.rgba.data(), error)) {
            std::fprintf(stderr, "[camera] upload failed: %s\n", error.c_str());
            continue;
        }
        uploadMs_ += nowMs() - t0;
        ++uploads_;
        t.hasFrame = true;
        place(t);
        updateVisibility(t);
    }
}

void CameraOverlays::setLive(uint8_t liveMask) {
    if (liveMask == live_) return;
    live_ = liveMask;
    for (Tile& t : tiles_) updateVisibility(t);
}

uint8_t CameraOverlays::cameras() const {
    uint8_t mask = 0;
    for (const Tile& t : tiles_) mask |= static_cast<uint8_t>(1u << t.camera);
    return mask;
}

void CameraOverlays::destroyTile(Tile& t) {
    if (t.handle != 0 && vr::VROverlay() != nullptr) {
        vr::VROverlay()->HideOverlay(t.handle);
        vr::VROverlay()->ClearOverlayTexture(t.handle);
        vr::VROverlay()->DestroyOverlay(t.handle);
    }
    t.handle = 0;
    t.visible = false;
    bury(std::move(t.texture));
    bury(std::move(t.retired));
}

void CameraOverlays::bury(std::unique_ptr<OverlayTexture> texture) {
    if (texture) graveyard_.emplace_back(nowMs(), std::move(texture));
}

void CameraOverlays::freeBuried(bool all) {
    const double now = nowMs();
    for (auto it = graveyard_.begin(); it != graveyard_.end();) {
        if (all || now - it->first > 1000.0) {
            it->second->destroy();
            it = graveyard_.erase(it);
        } else {
            ++it;
        }
    }
}

void CameraOverlays::destroyOverlays() {
    for (Tile& t : tiles_) {
        if (t.handle == 0 || vr::VROverlay() == nullptr) continue;
        vr::VROverlay()->HideOverlay(t.handle);
        vr::VROverlay()->ClearOverlayTexture(t.handle);
        vr::VROverlay()->DestroyOverlay(t.handle);
        t.handle = 0;
        t.visible = false;
    }
}

void CameraOverlays::destroyTextures() {
    freeBuried(true);
    for (Tile& t : tiles_) {
        if (t.texture) t.texture->destroy();
        if (t.retired) t.retired->destroy();
        t.texture.reset();
        t.retired.reset();
    }
}

void CameraOverlays::takeUploadStats(double& avgMs, unsigned& count, unsigned& skippedHidden) {
    count = uploads_;
    avgMs = uploads_ ? uploadMs_ / uploads_ : 0.0;
    skippedHidden = skippedHidden_;
    uploadMs_ = 0.0;
    uploads_ = 0;
    skippedHidden_ = 0;
}
