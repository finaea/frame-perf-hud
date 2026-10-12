// T10: the headset's camera views. Each camera tile of a camera widget gets its own OpenVR overlay, placed over
// its spot in the panel, so the ~2 Hz panel texture never redraws at camera rate: a new frame uploads only that
// tile's small texture. The tiles follow the panel: its transform (head / wrist: same device-relative parent;
// Detached: absolute) composed with the tile's offset in the panel plane, 1 mm in front of it, and its alpha /
// visibility. A tile is shown only while its camera is live; otherwise the panel's card shows the state text.
#pragma once

#include "camera_frame.h"
#include "camera_service.h"
#include "grid_renderer.h"
#include "openvr.h"
#include "vk_texture.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

/** How the HUD panel is placed right now (VrOverlay::panelPlacement). */
struct PanelPlacement {
    bool valid = false;          ///< a transform has been applied
    bool shown = false;          ///< the panel overlay is shown
    float alpha = 1.0f;          ///< its applied alpha
    bool absolute = false;       ///< Detached: standing-space transform; else relative to `device`
    uint32_t device = 0;
    vr::HmdMatrix34_t transform {};
    float widthM = 0.2f;         ///< the whole panel overlay's width (m)
    int textureWidth = 1;        ///< panel texture px
    int visibleHeight = 1;       ///< panel texture rows shown
    uint64_t revision = 0;       ///< bumps whenever anything above changes
    uint64_t poseRevision = 0;   ///< bumps when the transform, size or device changes (not alpha / shown)
};

/** The pure maths: where a tile's overlay goes relative to the panel. */
struct TilePlacement {
    vr::HmdMatrix34_t offset {};  ///< panel-local transform of the tile overlay's centre
    float widthM = 0.0f;
};

/**
 * @param rect the tile's picture rect in unscaled panel px (slot + fitFrame)
 * @param geometry the grid (unscaled width/height, scale)
 * @param panel the panel's placement (width and visible height)
 */
TilePlacement placeTile(const fph::cam::Rect& rect, const fph::GridGeometry& geometry, const PanelPlacement& panel);

class CameraOverlays {
public:
    explicit CameraOverlays(VulkanContext& vulkan) : vulkan_(vulkan) {}
    ~CameraOverlays() { destroyOverlays(); destroyTextures(); }
    CameraOverlays(const CameraOverlays&) = delete;
    CameraOverlays& operator=(const CameraOverlays&) = delete;

    /** The grid changed: one tile per camera tile of each camera widget (overlays are reused by position). */
    void setGeometry(const fph::GridGeometry& geometry);

    /** Applies the panel's placement to every tile (cheap when its revision didn't change). */
    void follow(const PanelPlacement& panel);

    /**
     * A new frame: uploads it to every tile of that camera (and shows them if the panel is shown). Skipped while the
     * panel is hidden or faded out (nobody sees it; the next frame after it reappears comes within 1/15 s).
     */
    void onFrame(uint8_t camera, const CameraImage& image);

    /** The cameras' states: a tile whose camera isn't live is hidden (the panel's card shows why). */
    void setLive(uint8_t liveMask);

    /** @return cameras that have a visible tile (bit = camera id) */
    uint8_t cameras() const;

    /** Before VR_Shutdown: hide, clear and destroy the overlays. */
    void destroyOverlays();
    /** After VR_Shutdown: free the Vulkan images. */
    void destroyTextures();

    /** @return average upload ms (wall, main thread) and count since the last call, and frames skipped hidden */
    void takeUploadStats(double& avgMs, unsigned& count, unsigned& skippedHidden);

private:
    struct Tile {
        uint8_t camera = 0;
        fph::cam::Tile tile;  ///< card-relative
        int slotX = 0, slotY = 0;
        uint64_t handle = 0;  ///< vr::VROverlayHandle_t
        std::unique_ptr<OverlayTexture> texture;
        std::unique_ptr<OverlayTexture> retired;  ///< the previous size's texture, freed on the next upload
        int texW = 0, texH = 0;
        bool hasFrame = false;
        bool visible = false;
        uint64_t placedRevision = 0;
    };

    VulkanContext& vulkan_;
    fph::GridGeometry geometry_;
    std::vector<Tile> tiles_;
    PanelPlacement panel_;
    uint8_t live_ = 0;
    uint64_t geometryRevision_ = 1;
    double uploadMs_ = 0.0;
    unsigned uploads_ = 0;
    unsigned skippedHidden_ = 0;
    /** Textures of removed tiles: the compositor may still read them for a frame or two, so they go a second later */
    std::vector<std::pair<double, std::unique_ptr<OverlayTexture>>> graveyard_;

    void bury(std::unique_ptr<OverlayTexture> texture);
    void freeBuried(bool all);

    void place(Tile& t);
    void updateVisibility(Tile& t);
    void destroyTile(Tile& t);
};
