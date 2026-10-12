// T10: when the headset's camera tiles are re-placed (headset/camera_tile_geometry.h).
#include "camera_tile_geometry.h"

#include <cstdio>

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using fph::WidgetType;

fph::WidgetInstance inst(WidgetType t) {
    fph::WidgetInstance w;
    w.type = t;
    return w;
}

fph::Layout layoutOf(std::vector<std::vector<WidgetType>> columns) {
    fph::Layout l;
    for (auto& c : columns) {
        fph::LayoutColumn col;
        for (WidgetType t : c) col.widgets.push_back(inst(t));
        l.columns.push_back(col);
    }
    return l;
}

}  // namespace

int main() {
    using W = WidgetType;
    const fph::GridGeometry base = fph::layoutGrid(layoutOf({{W::Frame, W::HeadsetCamera}, {W::Cpu, W::EyeCamera}}));
    CHECK(camtiles::sameTileGeometry(base, base));

    // A widget above a camera card changing height (no layout edit) moves the tile: re-place
    fph::GridGeometry taller = base;
    for (auto& s : taller.slots)
        if (s.widget.type == W::HeadsetCamera) s.y += 20;
    CHECK(!camtiles::sameTileGeometry(base, taller));

    // A non-camera widget changing height elsewhere (panel size unchanged) doesn't move any tile
    fph::GridGeometry other = base;
    for (auto& s : other.slots)
        if (s.widget.type == W::Cpu) s.h += 4;
    CHECK(camtiles::sameTileGeometry(base, other));

    // Panel size / scale changes move every tile (they're placed in metres relative to the panel)
    fph::GridGeometry wider = base;
    wider.width += 512;
    CHECK(!camtiles::sameTileGeometry(base, wider));
    fph::GridGeometry scaled = base;
    scaled.scale = 0.9;
    CHECK(!camtiles::sameTileGeometry(base, scaled));

    // Camera settings (which cams) change the tiles
    fph::GridGeometry cams = base;
    for (auto& s : cams.slots)
        if (s.widget.type == W::HeadsetCamera) s.widget.cameras = 0x0F;
    CHECK(!camtiles::sameTileGeometry(base, cams));

    // A camera widget added or removed
    const fph::GridGeometry fewer = fph::layoutGrid(layoutOf({{W::Frame, W::HeadsetCamera}, {W::Cpu}}));
    CHECK(!camtiles::sameTileGeometry(base, fewer));
    CHECK(!camtiles::sameTileGeometry(fewer, base));

    // No camera widgets at all, same panel size: nothing to re-place
    fph::GridGeometry none1 = fph::layoutGrid(layoutOf({{W::Frame}}));
    fph::GridGeometry none2 = none1;
    none2.slots[0].h += 10;
    CHECK(camtiles::sameTileGeometry(none1, none2));

    if (failures) {
        std::printf("camera-tile-geometry-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("camera-tile-geometry-test: all passed\n");
    return 0;
}
