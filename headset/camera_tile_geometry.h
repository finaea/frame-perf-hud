// T10: when the headset's camera tiles must be re-placed. They sit over their widget's spot in the panel, so they
// move whenever the panel's size / scale or a camera widget's slot changes, which can happen without a layout
// change (a widget above it changing height). Header-only so it's testable without OpenVR.
#pragma once

#include "grid_renderer.h"

#include <algorithm>
#include <cmath>

namespace camtiles {

// R2: a curved panel (SetOverlayCurvature). Checked on the Frame with ComputeOverlayIntersection: the overlay becomes
// an arc of a cylinder of radius R = width / (2π · curvature), its axis parallel to the overlay's y and R in front of
// it (+z, the viewer's side), with the overlay's centre on the arc and the texture's u spread evenly along the arc.

/** @return SetOverlayCurvature's value for an overlay `widthM` wide on a cylinder of `radiusM` (≤ 0 = flat); at most 1 */
inline float overlayCurvature(double widthM, double radiusM) {
    if (radiusM <= 0.0 || widthM <= 0.0) return 0.0f;
    return static_cast<float>(std::min(1.0, widthM / (2.0 * M_PI * radiusM)));
}

/** @return the radius an overlay `widthM` wide with `curvature` is bent to (0 = flat) */
inline double curvatureRadius(double widthM, double curvature) {
    return curvature > 0.0 ? widthM / (2.0 * M_PI * curvature) : 0.0;
}

/** Where a point of a curved panel goes, in the panel's own frame. */
struct ArcPose {
    double x = 0.0;      ///< panel x (m)
    double z = 0.0;      ///< panel z (m, + = towards the viewer)
    double angle = 0.0;  ///< its yaw about the panel's y (rad): its x axis is (cos, 0, sin), its front (−sin, 0, cos)
};

/**
 * The point `s` metres along the arc from the panel's centre (+ = right), lifted `lift` metres off the surface towards
 * the axis (the camera tiles sit 1 mm in front of the panel).
 * @param s arc length from the centre (m)
 * @param radiusM the cylinder's radius (m, > 0)
 * @param lift distance in front of the surface (m)
 */
inline ArcPose arcPose(double s, double radiusM, double lift) {
    ArcPose p;
    p.angle = s / radiusM;
    p.x = (radiusM - lift) * std::sin(p.angle);
    p.z = radiusM - (radiusM - lift) * std::cos(p.angle);
    return p;
}

/** @return true if every camera tile would sit exactly where it did (same panel size, same camera slots) */
inline bool sameTileGeometry(const fph::GridGeometry& a, const fph::GridGeometry& b) {
    if (a.width != b.width || a.height != b.height || a.scale != b.scale) return false;
    auto isCamera = [](const fph::WidgetSlot& s) {
        return fph::isCameraWidget(s.widget.type);
    };
    size_t i = 0, j = 0;
    for (;;) {
        while (i < a.slots.size() && !isCamera(a.slots[i])) ++i;
        while (j < b.slots.size() && !isCamera(b.slots[j])) ++j;
        if (i == a.slots.size() || j == b.slots.size()) return i == a.slots.size() && j == b.slots.size();
        const fph::WidgetSlot& x = a.slots[i++];
        const fph::WidgetSlot& y = b.slots[j++];
        if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h || x.widget != y.widget) return false;
    }
}

}  // namespace camtiles
