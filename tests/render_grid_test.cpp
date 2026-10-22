// Grid geometry, texture cap, draw rules and change detection of the widget grid renderer (links frame-perf-render).
// Renders into memory; a few PNGs go to $FRAME_PERF_PNG_DIR (default /tmp/frame-perf-render-tests/png) for a look by eye.
#include "atomic_file.h"
#include "grid_renderer.h"

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace frame_perf_overlay;
using W = WidgetType;
using Pixels = std::vector<uint8_t>;

constexpr double kNow = 100.0;

// The same default fonts the headset asks for; where they're missing FontSet falls back to fontconfig
FontSet& fonts() {
    static FontSet f;
    static bool loaded = false;
    if (!loaded) {
        f.load("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc");
        loaded = true;
    }
    return f;
}

std::string pngDir() {
    const char* env = std::getenv("FRAME_PERF_PNG_DIR");
    const std::string dir = env != nullptr && env[0] != '\0' ? env : "/tmp/frame-perf-render-tests/png";
    makeDirectories(dir);
    return dir;
}

WidgetInstance inst(W type, bool compact = false) {
    WidgetInstance w;
    w.type = type;
    w.compact = compact;
    return w;
}

Layout oneWidget(const WidgetInstance& w) {
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {w};
    return layout;
}

/** Renders a layout with a fresh renderer (no graph history carried over). */
Pixels renderPx(const Layout& layout, const MetricStore& store, const char* pngName = nullptr) {
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    renderer.render(ctx);
    if (pngName != nullptr) renderer.writePng(pngDir() + "/" + pngName);
    return renderer.toRgba();
}

// ---- Texture sizing ----

void testGeometry() {
    const GridGeometry def = layoutGrid(defaultLayout());
    CHECK(def.width == 512 && def.columns == 1);  // one column: the fixed panel's width
    CHECK(def.scale == 1.0);
    CHECK(def.textureWidth == 512);
    CHECK(def.textureHeight == def.height && def.height <= 2048);
    CHECK(def.slots.size() == 8);

    // 5 columns = 2560 px: scaled to fit 2048
    Layout wide;
    wide.columns.resize(5);
    for (LayoutColumn& c : wide.columns) c.widgets = {inst(W::Frame)};
    const GridGeometry gw = layoutGrid(wide);
    CHECK(gw.width == 2560);
    CHECK(gw.scale < 1.0);
    CHECK(gw.textureWidth <= 2048 && gw.textureHeight <= 2048);
    CHECK(std::abs(gw.textureWidth - std::ceil(gw.width * gw.scale)) <= 1);

    // one very tall column (more cards than a real layout allows, just to pass the cap)
    Layout tall;
    tall.columns.resize(1);
    for (int i = 0; i < 20; ++i) tall.columns[0].widgets.push_back(inst(W::Power));
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

// ---- Heights that follow the data ----

void testGeneralClockOffShrinks() {
    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    const WidgetInstance general = inst(W::General);

    ctx.clockFormat = 24;
    const int withClock = widgetHeight(general, &ctx);
    ctx.clockFormat = 0;
    const int withoutClock = widgetHeight(general, &ctx);
    CHECK(withClock - withoutClock == 22);  // the Time row, exactly one text line

    ctx.clockFormat = 12;
    CHECK(widgetHeight(general, &ctx) == withClock);  // 12 h / 24 h both show the row; only "Off" hides it

    // no context at all (the layout editor doesn't know the dashboard's clock setting): the fixed fallback
    // reserves the room the clock needs
    CHECK(widgetHeight(general) == withClock);
}

void testNetworkShrinksWhenNotExpanded() {
    const WidgetInstance network = inst(W::Network);

    MetricStore absent;  // interface state N/A: not expanded
    WidgetContext absentCtx;
    absentCtx.store = &absent;
    absentCtx.now = kNow;
    CHECK(widgetHeight(network, &absentCtx) == 64);

    MetricStore disconnected;
    disconnected.setEnum(metric::kNetInterfaceUp, 1, kNow);
    disconnected.setEnum(metric::kNetConnected, 0, kNow);
    WidgetContext disconnectedCtx;
    disconnectedCtx.store = &disconnected;
    disconnectedCtx.now = kNow;
    CHECK(widgetHeight(network, &disconnectedCtx) == 64);

    MetricStore connected;
    connected.setEnum(metric::kNetInterfaceUp, 1, kNow);
    connected.setEnum(metric::kNetConnected, 1, kNow);
    WidgetContext connectedCtx;
    connectedCtx.store = &connected;
    connectedCtx.now = kNow;
    CHECK(widgetHeight(network, &connectedCtx) == 88);

    CHECK(widgetHeight(network) == 88);  // no ctx: fixed fallback keeps the full reservation

    CHECK(renderPx(oneWidget(network), disconnected, "network_collapsed.png") !=
          renderPx(oneWidget(network), connected, "network_expanded.png"));
}

void testCompactHeights() {
    CHECK(widgetHeight(inst(W::Temp)) == 154);
    CHECK(widgetHeight(inst(W::Temp, true)) == 102);
    CHECK(widgetHeight(inst(W::Battery)) == 142);
    CHECK(widgetHeight(inst(W::Battery, true)) == 88);
    // the others have no compact card
    for (const W type : {W::Frame, W::Power, W::GpuRam, W::Cpu, W::Network, W::General}) {
        CHECK(widgetHeight(inst(type, true)) == widgetHeight(inst(type)));
    }

    // no jitter: values moving doesn't move a height or the grid
    MetricStore low;
    low.setF32(metric::kTempCpuC, 50, kNow);
    low.setF32(metric::kBatteryPct, 40, kNow);
    MetricStore high;
    high.setF32(metric::kTempCpuC, 80, kNow);
    high.setF32(metric::kBatteryPct, 90, kNow);
    WidgetContext lowCtx, highCtx;
    lowCtx.store = &low;
    lowCtx.now = kNow;
    highCtx.store = &high;
    highCtx.now = kNow;
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(W::Frame), inst(W::Temp)};
    layout.columns[1].widgets = {inst(W::Power), inst(W::Battery, true)};
    CHECK(sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(layout, &highCtx)));

    // toggling compact changes the geometry
    Layout toggled = layout;
    toggled.columns[0].widgets[1].compact = true;
    CHECK(!sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(toggled, &lowCtx)));
    CHECK(layoutGrid(toggled, &lowCtx).slots[1].h == 102);
    CHECK(layoutGrid(layout, &lowCtx).slots[1].h == 154);

    // Compact Temp keeps the graph: its graph bed (the panel's background colour, sunk into the card) is where the
    // full card's is. The card sits at (8, 8); the bed starts 202 px in and 10 px down, 84 px tall
    MetricStore none;
    const Pixels compact = renderPx(oneWidget(inst(W::Temp, true)), none, "temp-compact.png");
    const Pixels full = renderPx(oneWidget(inst(W::Temp)), none);
    const auto rgbAt = [](const Pixels& px, int width, int x, int y) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return (unsigned(px[i]) << 16) | (unsigned(px[i + 1]) << 8) | px[i + 2];
    };
    const int bedX = 8 + 202 + 20, bedY = 8 + 10 + 76;  // low in the bed, clear of the guides and the label
    CHECK(rgbAt(compact, 512, bedX, bedY) == rgbAt(full, 512, bedX, bedY));
    CHECK(rgbAt(compact, 512, bedX, bedY) != rgbAt(compact, 512, 8 + 190, bedY));  // the card, left of the bed
}

void testTitles() {
    for (const W type : {W::Frame, W::Cpu, W::GpuRam, W::Power, W::Battery, W::Temp, W::Network, W::General}) {
        for (const WidgetLanguage language : {WidgetLanguage::En, WidgetLanguage::Ja}) {
            const char* title = widgetTitle(inst(type), language);
            CHECK(title != nullptr && title[0] != '\0');
        }
    }
}

// ---- Draw rules ----

void testEmptyStoreAllTypes() {
    MetricStore store;  // everything N/A NotSampledYet
    for (const W type : {W::Frame, W::Cpu, W::GpuRam, W::Power, W::Battery, W::Temp, W::Network, W::General}) {
        CHECK(!renderPx(oneWidget(inst(type)), store).empty());
        if (hasCompactView(type)) CHECK(!renderPx(oneWidget(inst(type, true)), store).empty());
    }
    // no store at all
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
    CHECK(!renderPx(defaultLayout(), store, "default_filled.png").empty());
}

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

void testLanguageChangesTitles() {
    MetricStore store;
    const Layout layout = defaultLayout();
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    ctx.language = WidgetLanguage::En;
    CHECK(renderer.render(ctx));
    const uint64_t builds = renderer.staticBuilds();
    CHECK(!renderer.render(ctx));
    // a new language rebuilds the static layer (the titles) and repaints
    ctx.language = WidgetLanguage::Ja;
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == builds + 1);
}

// ---- Change detection ----

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
    changed.columns[0].widgets.pop_back();
    CHECK(renderer.setLayout(changed));
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);
    CHECK(!renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);

    // A setting tweak (Compact) is a different widget too
    Layout tweaked = changed;
    tweaked.columns[0].widgets[2].compact = true;  // Temp
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

// ---- Per-widget redraw: partial == full ----

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
    layout.columns[1].widgets = {inst(W::Cpu)};

    MetricStore store;
    store.setF32(metric::kBatteryPct, 80, 1.0);

    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 4);  // first render draws everything

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

    // A CPU value changes: only the CPU widget redraws, and the picture follows
    store.setF32(metric::kCpuUsagePct, 63, 4.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.lastRedrawn() == 1);
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
    CHECK(renderer.lastRedrawn() == 4);
    CHECK(pixelsOf(renderer) == pixelsOf(fresh2));
}

/** One card of the last render, cut out of the panel (RGBA). */
Pixels slotPixels(GridRenderer& renderer, size_t index) {
    const WidgetSlot& slot = renderer.geometry().slots[index];
    const int stride = renderer.geometry().textureWidth * 4;
    const std::vector<uint8_t>& rgba = renderer.toRgba();
    Pixels out;
    for (int row = slot.y; row < slot.y + slot.h; ++row) {
        out.insert(out.end(), rgba.begin() + row * stride + slot.x * 4,
                   rgba.begin() + row * stride + (slot.x + slot.w) * 4);
    }
    return out;
}

// A graph keeps its history when another card's setting changes (Compact on the card below it)
void testCompactToggleKeepsGraphHistory() {
    Layout withFull;
    withFull.columns.resize(1);
    withFull.columns[0].widgets = {inst(W::Power), inst(W::Battery)};
    Layout withCompact = withFull;
    withCompact.columns[0].widgets[1].compact = true;

    GridRenderer toggled(fonts());
    GridRenderer steady(fonts());
    toggled.setLayout(withFull);
    steady.setLayout(withFull);
    MetricStore last;
    for (int i = 0; i < 12; ++i) {
        last.setF32(metric::kPowerVphW, 5.0 + 0.5 * (i % 4), kNow + i);
        WidgetContext ctx;
        ctx.store = &last;
        ctx.now = kNow + i;
        toggled.render(ctx);
        steady.render(ctx);
    }
    toggled.setLayout(withCompact);
    CHECK(toggled.geometry().slots[1].h == 88);
    WidgetContext ctx;
    ctx.store = &last;
    ctx.now = kNow + 11;
    toggled.render(ctx);
    steady.render(ctx);
    CHECK(slotPixels(toggled, 0) == slotPixels(steady, 0));
    // and the graph really holds a line: a renderer that never saw the history draws something else
    GridRenderer unseen(fonts());
    unseen.setLayout(withFull);
    unseen.render(ctx);
    CHECK(slotPixels(unseen, 0) != slotPixels(steady, 0));
}

// ---- Network: the interface being down is "Not connected", not an N/A ----

void testNetworkInterfaceDown() {
    const Layout layout = oneWidget(inst(W::Network));
    MetricStore unread;  // interface state never read: the N/A text
    MetricStore down;    // read, and down (no dongle AP and no home Wi-Fi)
    down.setEnum(metric::kNetInterfaceUp, 0, 1.0);
    MetricStore upNotConnected;
    upNotConnected.setEnum(metric::kNetInterfaceUp, 1, 1.0);
    upNotConnected.setEnum(metric::kNetConnected, 0, 1.0);
    MetricStore failed;  // the read failed: N/A with its reason
    failed.setNa(metric::kNetInterfaceUp, Reason::ReadFailed, 1.0);

    CHECK(renderPx(layout, down, "network_down.png") != renderPx(layout, unread));
    CHECK(renderPx(layout, down) != renderPx(layout, failed));
    CHECK(renderPx(layout, down) != renderPx(layout, upNotConnected));  // the link row differs
    // the same for the collapsed height
    WidgetContext ctx;
    ctx.store = &down;
    ctx.now = kNow;
    CHECK(widgetHeight(inst(W::Network), &ctx) == 64);

    // "Not connected" is in the card's language
    const auto render = [&](WidgetLanguage language) {
        GridRenderer renderer(fonts());
        renderer.setLayout(layout);
        WidgetContext c;
        c.store = &down;
        c.now = kNow;
        c.language = language;
        renderer.render(c);
        return renderer.toRgba();
    };
    CHECK(render(WidgetLanguage::En) != render(WidgetLanguage::Ja));
}

// ---- Text with broken UTF-8 doesn't upset cairo ----

void testInvalidUtf8Text() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 40);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts()};
    const Color white {1, 1, 1};
    const char* broken[] = {"a\xFF" "b", "\xC0\xAF", "end\xE3\x81", "\xED\xA0\x80", "\x80\x80"};
    for (const char* text : broken) {
        pen.text(10, 25, text, 16, white);
        CHECK(pen.measure(text, 16) > 0);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    }
    // measured as the replacement character, so layout and drawing agree
    CHECK(pen.measure("a\xFF" "b", 16) == pen.measure("a\xEF\xBF\xBD" "b", 16));
    CHECK(pen.measure(std::string("x\xE3\x81"), 16) == pen.measure(std::string("x\xEF\xBF\xBD\xEF\xBF\xBD"), 16));
    CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// ---- The segmented pill ----

/** @return the pixel as 0xRRGGBB (cairo ARGB32 is B, G, R, A bytes on little-endian) */
unsigned pixelRgb(cairo_surface_t* surface, int x, int y) {
    cairo_surface_flush(surface);
    const uint8_t* p = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4;
    return (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[1]) << 8) | p[0];
}

void testSegmentedPillColours() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 240, 70);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    const Pen pen {cr, &fonts()};
    // 200 x 48 at (10, 10): parts are 95 wide from x = 15 and x = 110. Sample 12 px inside each part's outer
    // end and mid-height, clear of the label and the check.
    for (const int selected : {0, 1}) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        drawSegmentedPill(pen, 10, 10, 200, 48, {{"On"}, {"Off"}}, selected, true);
        const unsigned left = pixelRgb(surface, 15 + 12, 34);
        const unsigned right = pixelRgb(surface, 205 - 12, 34);
        CHECK((selected == 0 ? left : right) == 0xe27dfd);   // kAccent
        CHECK((selected == 0 ? right : left) == 0x21262d);   // kControl
    }
    // unusable: no accent at full strength
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 200, 48, {{"On"}, {"Off"}}, 0, false);
    CHECK(pixelRgb(surface, 27, 34) != 0xe27dfd);

    // three parts (the attach pill: Head / L / R): 210 wide from x = 15, so 66.7 px each; the middle one is selected
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 210, 48, {{"Head"}, {"L"}, {"R"}}, 1, true);
    CHECK(pixelRgb(surface, 82 + 6, 34) == 0xe27dfd);  // the middle part's fill
    CHECK(pixelRgb(surface, 15 + 8, 34) != 0xe27dfd);
    CHECK(pixelRgb(surface, 215 - 8, 34) != 0xe27dfd);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

}  // namespace

int main() {
    testGeometry();
    testGeneralClockOffShrinks();
    testNetworkShrinksWhenNotExpanded();
    testCompactHeights();
    testTitles();
    testEmptyStoreAllTypes();
    testDefaultFilled();
    testClockOffHidesTime();
    testNetworkInterfaceDown();
    testInvalidUtf8Text();
    testLanguageChangesTitles();
    testChangeDetection();
    testPartialRedraw();
    testCompactToggleKeepsGraphHistory();
    testSegmentedPillColours();
    if (failures == 0) std::printf("render-grid-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
