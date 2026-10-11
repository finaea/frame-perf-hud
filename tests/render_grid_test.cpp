// Grid geometry, texture cap, draw rules and change detection of the widget grid renderer (links fph-render).
// Renders into memory; a few PNGs go to $FPH_PNG_DIR (default /tmp/fph-t04-tests/png) for a look by eye.
#include "atomic_file.h"
#include "grid_renderer.h"

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef FPH_FONT_DIR
#error "FPH_FONT_DIR must point at assets/fonts"
#endif

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;
using W = WidgetType;
using Pixels = std::vector<uint8_t>;

constexpr double kNow = 100.0;

FontSet& fonts() {
    static FontSet f;
    static bool loaded = false;
    if (!loaded) {
        f.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
        loaded = true;
    }
    return f;
}

std::string pngDir() {
    const char* env = std::getenv("FPH_PNG_DIR");
    const std::string dir = env != nullptr && env[0] != '\0' ? env : "/tmp/fph-t04-tests/png";
    makeDirectories(dir);
    return dir;
}

WidgetInstance inst(W type, uint32_t maxItems = 15, NetAdapter adapter = NetAdapter::Dongle) {
    WidgetInstance w;
    w.type = type;
    w.maxItems = maxItems;
    w.adapter = adapter;
    return w;
}

Layout oneWidget(const WidgetInstance& w) {
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {w};
    return layout;
}

/** Renders a layout with a fresh renderer (no graph history carried over). */
Pixels renderPx(const Layout& layout, const MetricStore& store, const ProcessListsView* procs = nullptr,
                const char* pngName = nullptr) {
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.processes = procs;
    ctx.now = kNow;
    renderer.render(ctx);
    if (pngName != nullptr) renderer.writePng(pngDir() + "/" + pngName);
    return renderer.toRgba();
}

// ---- 5. Texture sizing ----

void testGeometry() {
    const GridGeometry def = layoutGrid(defaultLayout());
    CHECK(def.width == 1536 && def.columns == 3);
    CHECK(def.scale == 1.0);
    CHECK(def.textureWidth == 1536);
    CHECK(def.textureHeight == def.height && def.height <= 2048);

    // 5 columns = 2560 px: scaled to fit 2048
    Layout wide;
    wide.columns.resize(5);
    for (LayoutColumn& c : wide.columns) c.widgets = {inst(W::Frame)};
    const GridGeometry gw = layoutGrid(wide);
    CHECK(gw.width == 2560);
    CHECK(gw.scale < 1.0);
    CHECK(gw.textureWidth <= 2048 && gw.textureHeight <= 2048);
    CHECK(std::abs(gw.textureWidth - std::ceil(gw.width * gw.scale)) <= 1);

    // one very tall column
    Layout tall;
    tall.columns.resize(1);
    for (int i = 0; i < 10; ++i) tall.columns[0].widgets.push_back(inst(W::Power));
    const GridGeometry gt = layoutGrid(tall);
    CHECK(gt.height > 2048);
    CHECK(gt.scale < 1.0);
    CHECK(gt.textureWidth <= 2048 && gt.textureHeight <= 2048);
    CHECK(std::abs(gt.textureHeight - std::ceil(gt.height * gt.scale)) <= 1);
    CHECK(std::abs(gt.textureWidth - std::ceil(gt.width * gt.scale)) <= 1);

    // the capped panel really renders at the capped size
    MetricStore store;
    GridRenderer renderer(fonts());
    renderer.setLayout(tall);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    renderer.render(ctx);
    CHECK(renderer.toRgba().size() == size_t(gt.textureWidth) * size_t(gt.textureHeight) * 4);
    renderer.writePng(pngDir() + "/tall_scaled.png");

    // empty layout: minimal one-column panel, renders without a crash
    const GridGeometry ge = layoutGrid(Layout());
    CHECK(ge.columns == 1 && ge.slots.empty());
    CHECK(ge.textureWidth == 512 && ge.textureHeight > 0);
    CHECK(!renderPx(Layout(), store).empty());
}

// ---- 6. Top N rows that fit ----

ProcessListsView makeRows(int n) {
    ProcessListsView view;
    for (int i = 0; i < n; ++i) {
        ProcessItem p;
        p.name = "proc" + std::to_string(i);
        p.pid = 100 + i;
        p.cpuCorePct = 90.0 - i;
        p.rssMiB = 2000.0 - 10 * i;
        view.byCpu.push_back(p);
        view.byRam.push_back(p);
    }
    view.cpuValid = true;
    return view;
}

void testListRows() {
    CHECK(widgetHeight(inst(W::CpuItems, 10)) > widgetHeight(inst(W::CpuItems, 5)));
    CHECK(widgetHeight(inst(W::RamItems, 50)) > widgetHeight(inst(W::RamItems, 10)));

    MetricStore store;
    store.setI32(metric::kCpuItemsRows, 5, 1.0);
    store.setI32(metric::kRamItemsRows, 5, 1.0);
    const ProcessListsView five = makeRows(5);
    const ProcessListsView twenty = makeRows(20);
    for (const W type : {W::CpuItems, W::RamItems}) {
        const Layout layout = oneWidget(inst(type, 5));
        CHECK(renderPx(layout, store, &five) == renderPx(layout, store, &twenty));
    }
    // ...and the rows do show: 3 of 20 differs from 5 of 20 when the list is allowed to
    const ProcessListsView two = makeRows(2);
    CHECK(renderPx(oneWidget(inst(W::CpuItems, 5)), store, &twenty) !=
          renderPx(oneWidget(inst(W::CpuItems, 5)), store, &two));
    renderPx(oneWidget(inst(W::CpuItems, 5)), store, &twenty, "cpu_items_5_of_20.png");
}

// ---- 7. Draw rules ----

void testBatteryTimeRows() {
    MetricStore base;
    base.setF32(metric::kBatteryPct, 55, 1.0);
    base.setEnum(metric::kBatteryStatus, uint32_t(BatteryStatus::Discharging), 1.0);
    const Layout layout = oneWidget(inst(W::Battery));
    const Pixels na = renderPx(layout, base);

    MetricStore otherReason = base;
    otherReason.setNa(metric::kBatteryTimeToEmptyS, Reason::ReadFailed, 1.0);
    CHECK(renderPx(layout, otherReason) == na);  // N/A rows aren't drawn, whatever the reason

    MetricStore withTime = base;
    withTime.setI32(metric::kBatteryTimeToEmptyS, 3600, 1.0);
    CHECK(renderPx(layout, withTime, nullptr, "battery_time_to_empty.png") != na);
}

void testUnmeasuredNegative() {
    MetricStore zero;
    zero.setF32(metric::kPowerUnmeasuredW, 0.0, 1.0);
    MetricStore negative;
    negative.setF32(metric::kPowerUnmeasuredW, -0.3, 1.0);
    MetricStore positive;
    positive.setF32(metric::kPowerUnmeasuredW, 0.3, 1.0);
    const Layout layout = oneWidget(inst(W::Power));
    CHECK(renderPx(layout, negative) == renderPx(layout, zero));
    CHECK(renderPx(layout, positive) != renderPx(layout, zero));
}

void testGfxVoltsDash() {
    const uint16_t gfxV = metric::kPowerGfxW + metric::kPowerRailVoltsOffset;
    MetricStore invalid;
    invalid.setNa(gfxV, Reason::InvalidReading, 1.0);
    MetricStore failed;
    failed.setNa(gfxV, Reason::ReadFailed, 1.0);
    const Layout layout = oneWidget(inst(W::Power));
    CHECK(renderPx(layout, invalid, nullptr, "power_gfx_dash.png") != renderPx(layout, failed));
}

void testPlaceholders() {
    // Dynamic layer only (the title is static): 12 and 13 say the same thing, 10 says something else
    const auto dynamicPx = [](W type) {
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 512, 100);
        cairo_t* cr = cairo_create(surface);
        const Pen pen {cr, &fonts()};
        MetricStore store;
        WidgetContext ctx;
        ctx.store = &store;
        ctx.now = kNow;
        drawWidgetDynamic(pen, 8, 0, inst(type), ctx, WidgetHistory());
        cairo_surface_flush(surface);
        const uint8_t* data = cairo_image_surface_get_data(surface);
        Pixels px(data, data + size_t(cairo_image_surface_get_stride(surface)) * 100);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return px;
    };
    const Pixels streaming = dynamicPx(W::Streaming);
    const Pixels headsetCam = dynamicPx(W::HeadsetCamera);
    const Pixels eyeCam = dynamicPx(W::EyeCamera);
    CHECK(headsetCam == eyeCam);
    CHECK(streaming != headsetCam);

    // Whole card: all three draw, and 12 vs 13 differ by their titles
    MetricStore store;
    const Pixels a = renderPx(oneWidget(inst(W::Streaming)), store);
    const Pixels b = renderPx(oneWidget(inst(W::HeadsetCamera)), store);
    const Pixels c = renderPx(oneWidget(inst(W::EyeCamera)), store);
    CHECK(a != b);
    CHECK(b != c);
}

void testEmptyStoreAllTypes() {
    MetricStore store;  // everything N/A NotSampledYet
    const ProcessListsView procs;
    for (int t = 1; t <= 13; ++t) {
        const WidgetInstance w = inst(static_cast<W>(t));
        CHECK(!renderPx(oneWidget(w), store).empty());
        CHECK(!renderPx(oneWidget(w), store, &procs).empty());
        WidgetInstance home = w;
        home.adapter = NetAdapter::Home;
        CHECK(!renderPx(oneWidget(home), store).empty());
    }
    // no store, no process lists at all
    GridRenderer renderer(fonts());
    renderer.setLayout(defaultLayout());
    WidgetContext ctx;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    renderer.writePng(pngDir() + "/default_empty_store.png");
}

void testDefaultFilled() {
    MetricStore store;
    store.setF32(metric::kFrameAppFps, 90, 1.0);
    store.setF32(metric::kFrameDisplayHz, 90, 1.0);
    store.setF32(metric::kCpuUsagePct, 35, 1.0);
    store.setF32(metric::kGpuBusyPct, 60, 1.0);
    store.setF32(metric::kPowerVphW, 7.5, 1.0);
    store.setF32(metric::kBatteryPct, 80, 1.0);
    store.setF32(metric::kTempCpuC, 55, 1.0);
    store.setI32(metric::kCpuItemsRows, 15, 1.0);
    store.setI32(metric::kRamItemsRows, 15, 1.0);
    const ProcessListsView procs = makeRows(20);
    CHECK(!renderPx(defaultLayout(), store, &procs, "default_filled.png").empty());
}

// ---- 8. Change detection ----

void testChangeDetection() {
    MetricStore store;
    GridRenderer renderer(fonts());
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;

    renderer.setLayout(defaultLayout());
    CHECK(renderer.render(ctx));          // first render: always new
    CHECK(!renderer.render(ctx));         // same store, same now
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 1);

    store.setF32(metric::kCpuUsagePct, 42, 1.0);
    CHECK(renderer.render(ctx));          // a value changed
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 1);

    // Same widgets, new version: no rebuild
    Layout same = defaultLayout();
    same.version = 9;
    CHECK(!renderer.setLayout(same));
    renderer.render(ctx);
    CHECK(renderer.staticBuilds() == 1);

    // Different widgets: one more static build
    Layout changed = defaultLayout();
    changed.columns[1].widgets.pop_back();
    CHECK(renderer.setLayout(changed));
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);

    // A setting tweak (max_items) is a different widget too
    Layout tweaked = changed;
    tweaked.columns[2].widgets[2].maxItems = 8;
    CHECK(renderer.setLayout(tweaked));
    renderer.render(ctx);
    CHECK(renderer.staticBuilds() == 3);

    // Every widget removed: the empty panel must still go up once (else the old widgets stay frozen on
    // screen), and an unchanged empty frame doesn't
    CHECK(renderer.setLayout(Layout()));
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);
    CHECK(!renderer.render(ctx));
}

// ---- 9. Per-widget redraw: partial == full ----

Pixels pixelsOf(const GridRenderer& r) {
    const GridGeometry& g = r.geometry();
    const uint8_t* data = r.pixels();
    return Pixels(data, data + size_t(g.textureWidth) * size_t(g.textureHeight) * 4);
}

void testPartialRedraw() {
    // No graph widgets, so history can't make a partial and a fresh render differ
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(W::Battery), inst(W::General), inst(W::Network)};
    layout.columns[1].widgets = {inst(W::CpuItems, 6), inst(W::RamItems, 6)};

    MetricStore store;
    store.setF32(metric::kBatteryPct, 80, 1.0);
    store.setI32(metric::kCpuItemsRows, 10, 1.0);
    store.setI32(metric::kRamItemsRows, 10, 1.0);
    ProcessListsView procs = makeRows(10);
    procs.generation = 1;

    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.processes = &procs;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 5);  // first render draws everything

    // Nothing changed: no redraw at all
    CHECK(!renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);

    // One battery metric changes: only the Battery widget redraws, and the picture equals a from-scratch one
    store.setF32(metric::kBatteryPct, 79, 2.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 1);
    GridRenderer fresh(fonts());
    fresh.setLayout(layout);
    CHECK(fresh.render(ctx));
    CHECK(pixelsOf(renderer) == pixelsOf(fresh));

    // Same value sampled again later: the signature moves (sampledAt), still the same picture
    store.setF32(metric::kBatteryPct, 79, 3.0);
    renderer.render(ctx);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh));

    // A new process scan: only the two list widgets redraw, and the picture follows the new rows
    procs.byCpu[0].name = "renamed";
    procs.byRam[0].name = "renamed";
    ++procs.generation;
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 2);
    GridRenderer fresh2(fonts());
    fresh2.setLayout(layout);
    fresh2.render(ctx);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh2));
    CHECK(pixelsOf(renderer) != pixelsOf(fresh));
    CHECK(!renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 0);

    // markAllDirty redraws everything, same picture
    renderer.markAllDirty();
    renderer.render(ctx);
    CHECK(renderer.lastRedrawn() == 5);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh2));
}

}  // namespace

// ---- T04 fixes: clock "Off", capped label ----

/** Renders one General widget with a given clock format. */
Pixels renderGeneral(const MetricStore& store, int clockFormat) {
    GridRenderer renderer(fonts());
    renderer.setLayout(oneWidget(inst(W::General)));
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.clockFormat = clockFormat;
    renderer.render(ctx);
    return renderer.toRgba();
}

void testClockOffHidesTime() {
    MetricStore a;
    a.setText(metric::kClock, "2026-10-11 23:05:00", 1.0);
    MetricStore b;
    b.setText(metric::kClock, "2026-10-12 07:41:00", 1.0);
    // Shown (12 / 24 h): the time is drawn, so a different time changes pixels
    CHECK(renderGeneral(a, 24) != renderGeneral(b, 24));
    CHECK(renderGeneral(a, 12) != renderGeneral(a, 24));
    // Off (0): the Time row is gone, so the time no longer matters
    CHECK(renderGeneral(a, 0) == renderGeneral(b, 0));
    CHECK(renderGeneral(a, 0) != renderGeneral(a, 24));
}

void testCappedShowsHardwareMax() {
    const Layout layout = oneWidget(inst(W::Cpu));
    const auto store = [](double maxMhz, double hwMaxMhz) {
        MetricStore st;
        st.setF32(metric::kCpuCluster0Mhz, 2035.2, 1.0);
        st.setF32(metric::kCpuCluster0MaxMhz, maxMhz, 1.0);
        st.setF32(metric::kCpuCluster0HwMaxMhz, hwMaxMhz, 1.0);
        return st;
    };
    const Pixels uncapped = renderPx(layout, store(3148.8, 3148.8));
    const Pixels capped = renderPx(layout, store(2035.2, 3148.8), nullptr, "cpu_capped.png");
    CHECK(capped != uncapped);
    // The label names the hardware max, so a different max draws differently
    CHECK(renderPx(layout, store(2035.2, 2265.6)) != capped);
}

int main() {
    testGeometry();
    testListRows();
    testBatteryTimeRows();
    testUnmeasuredNegative();
    testGfxVoltsDash();
    testPlaceholders();
    testEmptyStoreAllTypes();
    testDefaultFilled();
    testChangeDetection();
    testPartialRedraw();
    testClockOffHidesTime();
    testCappedShowsHardwareMax();
    if (failures == 0) std::printf("render-grid-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
