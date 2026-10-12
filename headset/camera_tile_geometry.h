// T10: when the headset's camera tiles must be re-placed. They sit over their widget's spot in the panel, so they
// move whenever the panel's size / scale or a camera widget's slot changes, which can happen without a layout
// change (a widget above it changing height). Header-only so it's testable without OpenVR.
#pragma once

#include "grid_renderer.h"

namespace camtiles {

/** @return true if every camera tile would sit exactly where it did (same panel size, same camera slots) */
inline bool sameTileGeometry(const fph::GridGeometry& a, const fph::GridGeometry& b) {
    if (a.width != b.width || a.height != b.height || a.scale != b.scale) return false;
    auto isCamera = [](const fph::WidgetSlot& s) {
        return s.widget.type == fph::WidgetType::HeadsetCamera || s.widget.type == fph::WidgetType::EyeCamera;
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
