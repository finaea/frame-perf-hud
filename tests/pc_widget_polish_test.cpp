// Widget polish round: PanelView's geometry must exactly agree with GridRenderer's for the same Layout +
// WidgetContext, since both are meant to lay out identically (just drawn at a different scale). Geometry
// only: these tests never call .render() on either object, so no font file needs to be loaded (the pc_*_test
// glob that builds this file doesn't define FPH_FONT_DIR the way the core render-grid-test target does --
// layoutGrid/widgetHeight/batteryRowCount don't touch fonts at all).
#include "grid_renderer.h"
#include "metric_store.h"
#include "panel_view.h"
#include "pc_editor.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

using namespace fph;

WidgetInstance inst(WidgetType type) {
    WidgetInstance w;
    w.type = type;
    return w;
}

bool geometriesMatch(const GridGeometry& a, const GridGeometry& b) {
    if (a.width != b.width || a.height != b.height || a.scale != b.scale || a.textureWidth != b.textureWidth ||
        a.textureHeight != b.textureHeight || a.columns != b.columns || a.slots.size() != b.slots.size()) {
        return false;
    }
    for (size_t i = 0; i < a.slots.size(); ++i) {
        const WidgetSlot& x = a.slots[i];
        const WidgetSlot& y = b.slots[i];
        if (x.widget.type != y.widget.type || x.column != y.column || x.x != y.x || x.y != y.y || x.w != y.w ||
            x.h != y.h) {
            return false;
        }
    }
    return true;
}

void checkAgree(const Layout& layout, const WidgetContext& ctx) {
    FontSet fonts;
    GridRenderer gr(fonts);
    pc::PanelView pv(fonts);
    gr.setLayout(layout);
    pv.setLayout(layout);
    gr.updateGeometry(ctx);
    pv.updateGeometry(ctx);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
}

void testPanelViewGeometryAgreesAcrossScenarios() {
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(WidgetType::Battery), inst(WidgetType::General)};
    layout.columns[1].widgets = {inst(WidgetType::Frame), inst(WidgetType::Power)};

    MetricStore minimal;
    WidgetContext minimalCtx;
    minimalCtx.store = &minimal;
    minimalCtx.now = 1.0;
    checkAgree(layout, minimalCtx);

    MetricStore pluggedFull;
    pluggedFull.setEnum(metric::kBatteryPlugged, 1, 1.0);
    pluggedFull.setF32(metric::kBatteryPdOfferedV, 20.0, 1.0);
    pluggedFull.setF32(metric::kBatterySagV, 0.3, 1.0);
    pluggedFull.setI32(metric::kBatteryTimeToFullS, 1200, 1.0);
    WidgetContext pluggedCtx;
    pluggedCtx.store = &pluggedFull;
    pluggedCtx.now = 1.0;
    checkAgree(layout, pluggedCtx);

    // no store at all: both must fall back identically, not crash
    WidgetContext noStoreCtx;
    noStoreCtx.now = 1.0;
    checkAgree(layout, noStoreCtx);
}

void testPanelViewReflowTransitionsMatchGridRenderer() {
    FontSet fonts;
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {inst(WidgetType::Battery), inst(WidgetType::General)};

    GridRenderer gr(fonts);
    pc::PanelView pv(fonts);
    gr.setLayout(layout);
    pv.setLayout(layout);

    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = 1.0;

    // stabilize both from the initial (ctx-less) fallback geometry to the real, context-aware one
    gr.updateGeometry(ctx);
    pv.updateGeometry(ctx);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));

    // nothing changed: neither reflows
    CHECK(!gr.updateGeometry(ctx));
    CHECK(!pv.updateGeometry(ctx));

    // plugging in adds rows: both reflow, and land on the same geometry
    store.setEnum(metric::kBatteryPlugged, 1, 1.0);
    store.setF32(metric::kBatteryPdOfferedV, 20.0, 1.0);
    store.setF32(metric::kBatterySagV, 0.3, 1.0);
    CHECK(gr.updateGeometry(ctx));
    CHECK(pv.updateGeometry(ctx));
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));

    // a same-shape value-only change reflows neither
    store.setF32(metric::kBatteryPct, 50, 2.0);
    CHECK(!gr.updateGeometry(ctx));
    CHECK(!pv.updateGeometry(ctx));

    // unplugging reflows both back down, in step
    store.setNa(metric::kBatteryPlugged, Reason::NotSampledYet, 3.0);
    CHECK(gr.updateGeometry(ctx));
    CHECK(pv.updateGeometry(ctx));
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
}

/** Regression: a shape change confined to one column (Battery's) must be detected even when the overall
 *  panel width/height don't move, because the adjacent column is -- and stays -- taller. sameGridGeometry
 *  compares every slot's x/y/w/h, not just the panel totals, so this must still report "changed" exactly
 *  once per transition. */
void testReflowWithConstantPanelSize() {
    FontSet fonts;
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(WidgetType::Battery), inst(WidgetType::General)};
    // generously tall: several hundred px taller than anything Battery's row count alone could add, so this
    // column stays the tallest (and the panel height stays put) through every transition below
    layout.columns[1].widgets = {inst(WidgetType::Frame), inst(WidgetType::Power), inst(WidgetType::Temp),
                                 inst(WidgetType::Network)};

    GridRenderer gr(fonts);
    pc::PanelView pv(fonts);
    gr.setLayout(layout);
    pv.setLayout(layout);

    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = 1.0;

    CHECK(gr.updateGeometry(ctx));
    CHECK(pv.updateGeometry(ctx));
    const int width0 = gr.geometry().width;
    const int height0 = gr.geometry().height;
    CHECK(pv.geometry().width == width0 && pv.geometry().height == height0);
    CHECK(!gr.updateGeometry(ctx));
    CHECK(!pv.updateGeometry(ctx));

    // plug in: Battery's own slots move/grow, but column 1 is still the tallest, so the panel's own
    // width/height don't change even though the geometry (and so sameGridGeometry) must report "changed"
    store.setEnum(metric::kBatteryPlugged, 1, 1.0);
    store.setF32(metric::kBatteryPdOfferedV, 20.0, 1.0);
    store.setF32(metric::kBatterySagV, 0.3, 1.0);
    store.setI32(metric::kBatteryTimeToFullS, 1200, 1.0);
    CHECK(gr.updateGeometry(ctx)); // changed: true, exactly once
    CHECK(pv.updateGeometry(ctx));
    CHECK(gr.geometry().width == width0 && gr.geometry().height == height0);
    CHECK(pv.geometry().width == width0 && pv.geometry().height == height0);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
    CHECK(!gr.updateGeometry(ctx)); // stable again
    CHECK(!pv.updateGeometry(ctx));

    // unplug: reflows back down, again without moving the panel's own width/height
    store.setNa(metric::kBatteryPlugged, Reason::NotSampledYet, 2.0);
    CHECK(gr.updateGeometry(ctx));
    CHECK(pv.updateGeometry(ctx));
    CHECK(gr.geometry().width == width0 && gr.geometry().height == height0);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
    CHECK(!gr.updateGeometry(ctx));
    CHECK(!pv.updateGeometry(ctx));
}

/** Regression: PcEditor::sync() now also gates on sameGridGeometry, so a shape-only change (Battery's rows,
 *  same layout version and widgets, same overall panel dimensions) must still count as "changed" -- not be
 *  swallowed by the layout-level check alone. */
void testPcEditorSyncAcceptsShapeChangeWithSameDimensions() {
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {inst(WidgetType::Battery), inst(WidgetType::General)};
    layout.columns[1].widgets = {inst(WidgetType::Frame), inst(WidgetType::Power), inst(WidgetType::Temp),
                                 inst(WidgetType::Network)};

    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = 1.0;

    const GridGeometry g1 = layoutGrid(layout, &ctx);
    pc::PcEditor editor;
    CHECK(editor.sync(layout, g1));  // first sync: always a change
    CHECK(!editor.sync(layout, g1)); // identical layout + geometry: no change

    store.setEnum(metric::kBatteryPlugged, 1, 1.0);
    store.setF32(metric::kBatteryPdOfferedV, 20.0, 1.0);
    store.setF32(metric::kBatterySagV, 0.3, 1.0);
    const GridGeometry g2 = layoutGrid(layout, &ctx);
    CHECK(g2.width == g1.width && g2.height == g1.height); // the panel's own size didn't move
    CHECK(!sameGridGeometry(g1, g2));                      // but the slot shapes did
    CHECK(editor.sync(layout, g2));                        // same layout version/widgets: sync must still fire
    CHECK(!editor.sync(layout, g2));
}

WidgetInstance netInst(NetAdapter adapter) {
    WidgetInstance w = inst(WidgetType::Network);
    w.adapter = adapter;
    return w;
}

/** The other widget-polish shrink cases (Network, General, Streaming) go through the same updateGeometry()
 *  path as Battery, so PanelView and GridRenderer must still agree on every one of them. */
void testPanelViewAgreesAcrossOtherShrinkingWidgets() {
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {netInst(NetAdapter::Dongle), inst(WidgetType::General), inst(WidgetType::Streaming)};

    MetricStore collapsed; // network absent, clock off, no PC app: every shrinkable widget at its smallest
    WidgetContext collapsedCtx;
    collapsedCtx.store = &collapsed;
    collapsedCtx.now = 1.0;
    collapsedCtx.clockFormat = 0;
    checkAgree(layout, collapsedCtx);

    MetricStore expanded;
    expanded.setEnum(metric::kNetDongleBase + metric::kNetAdapterPresent, 1, 1.0);
    expanded.setEnum(metric::kNetDongleBase + metric::kNetAdapterConnected, 1, 1.0);
    WidgetContext expandedCtx;
    expandedCtx.store = &expanded;
    expandedCtx.now = 1.0;
    expandedCtx.clockFormat = 24;
    checkAgree(layout, expandedCtx);

    // the transition itself: both renderers reflow in step
    FontSet fonts;
    GridRenderer gr(fonts);
    pc::PanelView pv(fonts);
    gr.setLayout(layout);
    pv.setLayout(layout);
    gr.updateGeometry(collapsedCtx);
    pv.updateGeometry(collapsedCtx);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
    CHECK(gr.updateGeometry(expandedCtx));
    CHECK(pv.updateGeometry(expandedCtx));
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
}

/** Cpu's cluster-clock rows (2 rows / 1 row / 0 rows depending on which cluster slots have a real sensor vs.
 *  SensorMissing) go through the same updateGeometry() path too. */
void testPanelViewAgreesOnCpuClusterRows() {
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {inst(WidgetType::Cpu), inst(WidgetType::General)};

    MetricStore allMissing;
    for (uint16_t k = 0; k < 4; ++k) {
        allMissing.setNa(static_cast<uint16_t>(metric::kCpuCluster0Mhz + k), Reason::SensorMissing, 1.0);
    }
    WidgetContext allMissingCtx;
    allMissingCtx.store = &allMissing;
    allMissingCtx.now = 1.0;
    checkAgree(layout, allMissingCtx);

    MetricStore allValid;
    for (uint16_t k = 0; k < 4; ++k) {
        allValid.setF32(static_cast<uint16_t>(metric::kCpuCluster0Mhz + k), 2000.0 + k * 100, 1.0);
    }
    WidgetContext allValidCtx;
    allValidCtx.store = &allValid;
    allValidCtx.now = 1.0;
    checkAgree(layout, allValidCtx);

    // the transition itself: both reflow in step (General sitting below Cpu moves with it)
    FontSet fonts;
    GridRenderer gr(fonts);
    pc::PanelView pv(fonts);
    gr.setLayout(layout);
    pv.setLayout(layout);
    gr.updateGeometry(allMissingCtx);
    pv.updateGeometry(allMissingCtx);
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
    const int generalYBefore = gr.geometry().slots[1].y;
    CHECK(gr.updateGeometry(allValidCtx));
    CHECK(pv.updateGeometry(allValidCtx));
    CHECK(geometriesMatch(gr.geometry(), pv.geometry()));
    CHECK(gr.geometry().slots[1].y > generalYBefore); // sanity: General really did move down
}

} // namespace

int main() {
    testPanelViewGeometryAgreesAcrossScenarios();
    testPanelViewReflowTransitionsMatchGridRenderer();
    testReflowWithConstantPanelSize();
    testPcEditorSyncAcceptsShapeChangeWithSameDimensions();
    testPanelViewAgreesAcrossOtherShrinkingWidgets();
    testPanelViewAgreesOnCpuClusterRows();
    if (failures == 0) {
        std::printf("pc widget polish: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
