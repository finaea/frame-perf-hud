// T10: where a camera tile's overlay goes relative to the HUD panel (moved out of camera_overlay.* in R2 so it's
// testable without OpenVR's runtime or Vulkan: only OpenVR's matrix type is used).
#pragma once

#include "camera_frame.h"
#include "camera_tile_geometry.h"  // R2: the curved panel's arc
#include "grid_renderer.h"
#include "openvr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

constexpr float kTileLiftM = 0.001f;  ///< tiles sit 1 mm in front of the panel (+z is the panel's front)

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
    float curvature = 0.0f;      ///< R2: the panel's SetOverlayCurvature (Detached only; 0 = flat)
    uint64_t revision = 0;       ///< bumps whenever anything above changes
    uint64_t poseRevision = 0;   ///< bumps when the transform, size or device changes (not alpha / shown)
};

/** The pure maths: where a tile's overlay goes relative to the panel. */
struct TilePlacement {
    vr::HmdMatrix34_t offset {};  ///< panel-local transform of the tile overlay's centre
    float widthM = 0.0f;
    float curvature = 0.0f;       ///< R2: on a curved panel, the tile's own curvature (same cylinder axis)
};

/**
 * Flat panel: the tile's offset in the panel plane. Curved panel (R2): the tile sits on the same cylinder, its
 * centre rotated round the axis by its distance along the arc, bent to the same axis.
 * @param rect the tile's picture rect in unscaled panel px (slot + fitFrame)
 * @param geometry the grid (unscaled width/height, scale)
 * @param panel the panel's placement (width and visible height)
 */
inline TilePlacement placeTile(const fph::cam::Rect& rect, const fph::GridGeometry& geometry,
                               const PanelPlacement& panel) {
    TilePlacement p;
    const double width = std::max(1, geometry.width);
    const double scale = geometry.scale > 0.0 ? geometry.scale : 1.0;
    const double visibleH = panel.visibleHeight / scale;  // unscaled panel px shown
    const double metersPerPx = panel.widthM / width;
    const double cx = rect.x + rect.w / 2.0;
    const double cy = rect.y + rect.h / 2.0;
    p.widthM = static_cast<float>(rect.w * metersPerPx);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) p.offset.m[r][c] = r == c ? 1.0f : 0.0f;
    p.offset.m[0][3] = static_cast<float>((cx - width / 2.0) * metersPerPx);
    p.offset.m[1][3] = static_cast<float>((visibleH / 2.0 - cy) * metersPerPx);  // panel +y is up, px y is down
    p.offset.m[2][3] = kTileLiftM;
    if (panel.curvature > 0.0f) {  // R2: onto the arc, 1 mm inside it (towards the axis)
        const double radius = camtiles::curvatureRadius(panel.widthM, panel.curvature);
        const camtiles::ArcPose arc = camtiles::arcPose(p.offset.m[0][3], radius, kTileLiftM);
        const double c = std::cos(arc.angle), s = std::sin(arc.angle);
        p.offset.m[0][0] = static_cast<float>(c);
        p.offset.m[0][2] = static_cast<float>(-s);
        p.offset.m[2][0] = static_cast<float>(s);
        p.offset.m[2][2] = static_cast<float>(c);
        p.offset.m[0][3] = static_cast<float>(arc.x);
        p.offset.m[2][3] = static_cast<float>(arc.z);
        p.curvature = camtiles::overlayCurvature(p.widthM, radius - kTileLiftM);
    }
    return p;
}
