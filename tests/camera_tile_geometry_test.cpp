// T10: when the headset's camera tiles are re-placed (headset/camera_tile_geometry.h).
#include "camera_tile_geometry.h"
#include "camera_tile_place.h"

#include <cmath>
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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// R2: the curved panel's arc maths
void testArc() {
    // curvature <-> radius
    CHECK(camtiles::overlayCurvature(1.2, 0.0) == 0.0f);
    CHECK(camtiles::overlayCurvature(1.2, -1.0) == 0.0f);
    CHECK(camtiles::overlayCurvature(0.0, 1.0) == 0.0f);
    CHECK(near(camtiles::overlayCurvature(1.2, 0.764), 1.2 / (2 * M_PI * 0.764), 1e-6));
    CHECK(near(camtiles::overlayCurvature(1.2, 0.764), 0.25, 1e-3));
    CHECK(camtiles::overlayCurvature(10.0, 0.1) == 1.0f);  // a full circle at most
    CHECK(near(camtiles::overlayCurvature(2 * M_PI, 1.0), 1.0, 1e-6));
    CHECK(camtiles::curvatureRadius(1.2, 0.0) == 0.0);
    CHECK(camtiles::curvatureRadius(1.2, -0.5) == 0.0);
    // R3: the Detached curve is an arc in degrees (curvature = deg / 360), so R = W / angle
    CHECK(near(camtiles::curvatureRadius(0.4, 0.25f), 0.4 / (M_PI / 2), 1e-5));  // 90°: ≈ 0.2546
    CHECK(near(camtiles::curvatureRadius(0.4, static_cast<float>(40.0 / 360.0)), 0.4 / (40.0 * M_PI / 180.0), 1e-5));
    for (const double w : {0.3, 1.2, 2.0})
        for (const double r : {0.6, 1.0, 2.0}) {
            if (w / (2 * M_PI * r) > 1.0) continue;
            CHECK(near(camtiles::curvatureRadius(w, camtiles::overlayCurvature(w, r)), r, 1e-5));
        }

    // arcPose at the centre: straight ahead, lifted by the lift
    {
        const camtiles::ArcPose p = camtiles::arcPose(0.0, 0.764, 0.001);
        CHECK(near(p.x, 0.0) && near(p.z, 0.001) && near(p.angle, 0.0));
    }
    // Ground truth probed on the Frame (W = 1.2, c = 0.25, R = 0.764): uv.u 0.0735 -> local (-0.4743, 0.1651),
    // normal (x +0.6209, z +0.7839) in the world, i.e. (-sin, cos) of the angle in the panel's frame
    {
        const double R = camtiles::curvatureRadius(1.2, 0.25);
        CHECK(near(R, 0.7639, 1e-3));
        const double s = (0.0735 - 0.5) * 1.2;
        const camtiles::ArcPose p = camtiles::arcPose(s, R, 0.0);
        CHECK(near(p.x, -0.4743, 2e-3));
        CHECK(near(p.z, 0.1651, 2e-3));
        CHECK(near(-std::sin(p.angle), 0.6209, 2e-3));  // normal = (-sin, cos)
        CHECK(near(std::cos(p.angle), 0.7839, 2e-3));
    }
    // Every point: on the cylinder (distance R - lift from the axis (0, R)), axes orthonormal, front points at the axis
    for (const double lift : {0.0, 0.001})
        for (const double R : {0.6, 1.0, 2.0})
            for (double s = -0.9; s <= 0.9; s += 0.15) {
                const camtiles::ArcPose p = camtiles::arcPose(s, R, lift);
                CHECK(near(p.angle, s / R));
                CHECK(near(std::hypot(p.x, p.z - R), R - lift, 1e-9));
                const double xx = std::cos(p.angle), xz = std::sin(p.angle);      // the tile's x axis (x, z)
                const double zx = -std::sin(p.angle), zz = std::cos(p.angle);     // its front
                CHECK(near(xx * xx + xz * xz, 1.0, 1e-12) && near(zx * zx + zz * zz, 1.0, 1e-12));
                CHECK(near(xx * zx + xz * zz, 0.0, 1e-12));
                // front points from the point to the axis (0, R)
                const double dx = -p.x, dz = R - p.z, dl = std::hypot(dx, dz);
                CHECK(near(zx, dx / dl, 1e-9) && near(zz, dz / dl, 1e-9));
            }
    // Symmetric
    {
        const camtiles::ArcPose l = camtiles::arcPose(-0.4, 1.0, 0.001), r = camtiles::arcPose(0.4, 1.0, 0.001);
        CHECK(near(l.x, -r.x) && near(l.z, r.z) && near(l.angle, -r.angle));
    }
}

// R2: placeTile (headset/camera_tile_place.h)
void testPlaceTile() {
    // 1000 px wide grid, 0.5 m wide panel -> 0.5 mm per px; 600 px visible rows
    fph::GridGeometry g;
    g.width = 1000;
    g.height = 700;
    g.scale = 1.0;
    PanelPlacement panel;
    panel.widthM = 0.5f;
    panel.visibleHeight = 600;

    // Flat: the old formula, identity rotation, 1 mm in front
    {
        const fph::cam::Rect r {100, 50, 200, 100};  // centre (200, 100)
        const TilePlacement p = placeTile(r, g, panel);
        CHECK(near(p.offset.m[0][3], (200 - 500) * 0.0005, 1e-6));
        CHECK(near(p.offset.m[1][3], (300 - 100) * 0.0005, 1e-6));
        CHECK(near(p.offset.m[2][3], 0.001, 1e-7));
        CHECK(near(p.widthM, 0.1, 1e-6));
        CHECK(p.curvature == 0.0f);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) CHECK(p.offset.m[i][j] == (i == j ? 1.0f : 0.0f));
    }
    // R3: a 0° Detached curve (curvature 0.0f) places exactly like the flat panel
    {
        PanelPlacement zero = panel;
        zero.curvature = 0.0f;
        const fph::cam::Rect r {100, 50, 200, 100};
        const TilePlacement a = placeTile(r, g, panel), b = placeTile(r, g, zero);
        CHECK(a.curvature == 0.0f && b.curvature == 0.0f && a.widthM == b.widthM);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) CHECK(a.offset.m[i][j] == b.offset.m[i][j]);
    }
    // Flat with a UI scale: visible rows are in scaled px, so visibleH = 600 / 0.75 = 800 unscaled
    {
        g.scale = 0.75;
        const fph::cam::Rect r {400, 100, 200, 100};  // centre (500, 150)
        const TilePlacement p = placeTile(r, g, panel);
        CHECK(near(p.offset.m[0][3], 0.0, 1e-6));
        CHECK(near(p.offset.m[1][3], (400 - 150) * 0.0005, 1e-6));
        g.scale = 1.0;
    }
    // Curved, probe 2 (W 0.6, R 0.6, curvature 0.1592): 1000 px grid at 0.6 mm/px, 0.12 m (200 px) tile
    {
        PanelPlacement cp;
        cp.widthM = 0.6f;
        cp.visibleHeight = 600;
        cp.curvature = 0.1592f;
        const double R = camtiles::curvatureRadius(0.6, 0.1592);
        const double expectCurv = 0.12 / (2 * M_PI * (R - 0.001));

        // centred: identity rotation, on the surface 1 mm in
        const TilePlacement c = placeTile({400, 50, 200, 100}, g, cp);
        CHECK(near(c.offset.m[0][0], 1.0) && near(c.offset.m[0][2], 0.0) && near(c.offset.m[2][0], 0.0) &&
              near(c.offset.m[2][2], 1.0));
        CHECK(near(c.offset.m[0][3], 0.0, 1e-6) && near(c.offset.m[2][3], 0.001, 1e-6));
        CHECK(near(c.curvature, expectCurv, 1e-5));
        CHECK(near(c.curvature, 0.0319, 1e-4));

        // s = -0.20 -> centre px 500 - 0.20/0.0006 = 166.67; s = +0.15 -> 750
        struct Probe { double s, x, z, yaw; } probes[] = {{-0.20, -0.1960, 0.0340, -0.3333}, {0.15, 0.1482, 0.0196, 0.2500}};
        for (const Probe& pr : probes) {
            const double cxPx = 500 + pr.s / 0.0006;
            const TilePlacement t = placeTile({cxPx - 100, 50, 200, 100}, g, cp);
            const double yaw = std::atan2(t.offset.m[2][0], t.offset.m[0][0]);
            CHECK(near(yaw, pr.yaw, 1e-3));
            CHECK(near(t.offset.m[0][3], pr.x, 1e-3));
            CHECK(near(t.offset.m[2][3], pr.z, 1e-3));
            CHECK(near(t.curvature, 0.0319, 1e-4));
            // x axis (cos, 0, sin), z axis (-sin, 0, cos), y untouched; on the cylinder R - lift from the axis
            CHECK(near(t.offset.m[0][0], std::cos(yaw), 1e-5) && near(t.offset.m[2][0], std::sin(yaw), 1e-5));
            CHECK(near(t.offset.m[0][2], -std::sin(yaw), 1e-5) && near(t.offset.m[2][2], std::cos(yaw), 1e-5));
            CHECK(t.offset.m[1][1] == 1.0f && t.offset.m[0][1] == 0.0f && t.offset.m[1][0] == 0.0f &&
                  t.offset.m[1][2] == 0.0f && t.offset.m[2][1] == 0.0f);
            CHECK(near(std::hypot(t.offset.m[0][3], t.offset.m[2][3] - R), R - kTileLiftM, 1e-5));
            CHECK(near(yaw, pr.s / R, 1e-4));
        }
        // vertical offset is unchanged by curvature
        const TilePlacement v = placeTile({400, 150, 200, 100}, g, cp);  // centre y 200
        CHECK(near(v.offset.m[1][3], (300 - 200) * 0.0006, 1e-6));
    }
}

}  // namespace

int main() {
    testArc();
    testPlaceTile();
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
