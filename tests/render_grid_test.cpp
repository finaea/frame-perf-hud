// Grid geometry, texture cap, draw rules and change detection of the widget grid renderer (links fph-render).
// Renders into memory; a few PNGs go to $FPH_PNG_DIR (default /tmp/fph-t04-tests/png) for a look by eye.
#include "atomic_file.h"
#include "grid_renderer.h"
#include "stream_metrics.h"

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

// ---- Widget polish: Battery follows its visible rows ----

void testBatteryRowCountMirrorsDrawnPredicates() {
    MetricStore minimal;  // nothing set: unplugged, no optional field known
    WidgetContext minimalCtx;
    minimalCtx.store = &minimal;
    minimalCtx.now = kNow;
    CHECK(batteryRowCount(minimalCtx) == 5);  // charge, flow, health, charger, USB

    MetricStore withOptionals = minimal;
    withOptionals.setI32(metric::kBatteryTimeToFullS, 1200, kNow);
    withOptionals.setI32(metric::kBatteryTimeToEmptyS, 600, kNow);
    withOptionals.setF32(metric::kBatterySetCurrentA, 2.0, kNow);
    WidgetContext withOptionalsCtx;
    withOptionalsCtx.store = &withOptionals;
    withOptionalsCtx.now = kNow;
    CHECK(batteryRowCount(withOptionalsCtx) == 8);  // + toFull + toEmpty + setCurrent

    MetricStore pluggedBare = minimal;
    pluggedBare.setEnum(metric::kBatteryPlugged, 1, kNow);
    WidgetContext pluggedBareCtx;
    pluggedBareCtx.store = &pluggedBare;
    pluggedBareCtx.now = kNow;
    CHECK(batteryRowCount(pluggedBareCtx) == 6);  // + Arriving (unconditional once plugged)

    MetricStore pluggedFull = pluggedBare;
    pluggedFull.setF32(metric::kBatteryPdOfferedV, 20.0, kNow);
    pluggedFull.setF32(metric::kBatterySagV, 0.3, kNow);
    WidgetContext pluggedFullCtx;
    pluggedFullCtx.store = &pluggedFull;
    pluggedFullCtx.now = kNow;
    CHECK(batteryRowCount(pluggedFullCtx) == 8);  // + PdOffered + Sag

    // widgetHeight: no context falls back to the fixed legacy reservation; with one, it follows row count
    const WidgetInstance battery = inst(W::Battery);
    const int noCtx = widgetHeight(battery);
    const int minimalH = widgetHeight(battery, &minimalCtx);
    const int pluggedFullH = widgetHeight(battery, &pluggedFullCtx);
    CHECK(minimalH < pluggedFullH);  // 5 rows is shorter than 8
    CHECK(noCtx >= pluggedFullH);    // the legacy fixed reservation covers even the busiest case built here

    // the per-row height delta is a real, consistent constant, not a one-off correction
    MetricStore plusOne = minimal;
    plusOne.setI32(metric::kBatteryTimeToFullS, 1, kNow);
    WidgetContext plusOneCtx;
    plusOneCtx.store = &plusOne;
    plusOneCtx.now = kNow;
    MetricStore plusTwo = plusOne;
    plusTwo.setI32(metric::kBatteryTimeToEmptyS, 1, kNow);
    WidgetContext plusTwoCtx;
    plusTwoCtx.store = &plusTwo;
    plusTwoCtx.now = kNow;
    const int delta1 = widgetHeight(battery, &plusOneCtx) - minimalH;
    const int delta2 = widgetHeight(battery, &plusTwoCtx) - widgetHeight(battery, &plusOneCtx);
    CHECK(delta1 > 0 && delta1 == delta2);
}

void testBatteryReflow() {
    // Battery, then a card that must move when Battery's row count changes
    Layout layout;
    layout.columns.resize(1);
    layout.columns[0].widgets = {inst(W::Battery), inst(W::General)};

    MetricStore store;
    store.setF32(metric::kBatteryPct, 55, 1.0);
    GridRenderer renderer(fonts());
    renderer.setLayout(layout);
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;

    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 1);
    CHECK(renderer.geometry().slots.size() == 2);
    const int generalYBefore = renderer.geometry().slots[1].y;
    const int batteryHBefore = renderer.geometry().slots[0].h;

    // Plugging in adds rows: the panel reflows, costing exactly one more static build, and General moves down
    store.setEnum(metric::kBatteryPlugged, 1, 2.0);
    store.setF32(metric::kBatteryPdOfferedV, 20.0, 2.0);
    store.setF32(metric::kBatterySagV, 0.3, 2.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);
    CHECK(renderer.geometry().slots[0].h > batteryHBefore);
    CHECK(renderer.geometry().slots[1].y > generalYBefore);
    const int generalYPlugged = renderer.geometry().slots[1].y;
    const int batteryHPlugged = renderer.geometry().slots[0].h;

    // A same-shape metric update (percentage changes, but still plugged with the same optional fields known)
    // redraws the widget without touching the geometry or rebuilding the static layer
    store.setF32(metric::kBatteryPct, 56, 3.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 2);
    CHECK(renderer.geometry().slots[0].h == batteryHPlugged);
    CHECK(renderer.geometry().slots[1].y == generalYPlugged);

    // Unplugging reflows back down: one more static build, General moves back up
    store.setNa(metric::kBatteryPlugged, Reason::NotSampledYet, 4.0);
    CHECK(renderer.render(ctx));
    CHECK(renderer.staticBuilds() == 3);
    CHECK(renderer.geometry().slots[0].h == batteryHBefore);
    CHECK(renderer.geometry().slots[1].y == generalYBefore);
}

// ---- Widget polish: Streaming fails closed per part ----

void testStreamingNoPcStillShowsLinkAndFec() {
    MetricStore a;
    a.setEnum(metric::kStreamLinkMode, static_cast<uint32_t>(StreamLinkMode::Dongle), 1.0);
    a.setI32(metric::kStreamFecRetries, 2, 1.0);
    a.setI32(metric::kStreamFecRetryPackets, 14, 1.0);
    setPcStreamNa(a, Reason::PcAppNotRunning, 1.0);  // no PC app at all

    MetricStore b = a;
    b.setEnum(metric::kStreamLinkMode, static_cast<uint32_t>(StreamLinkMode::Multi), 1.0);
    b.setI32(metric::kStreamFecRetries, 9, 1.0);

    const Layout layout = oneWidget(inst(W::Streaming));
    CHECK(renderPx(layout, a) != renderPx(layout, b, nullptr, "streaming_no_pc_link_fec.png"));
}

void testStreamingHeaderMismatchKeepsL1L2HealthShown() {
    MetricStore base;  // a fully working PC snapshot, the shape streaming_preview.cpp calls "simulated"
    base.setEnum(metric::kStreamLinkMode, static_cast<uint32_t>(StreamLinkMode::Multi), 1.0);
    base.setF32(metric::kPcStreamPing, 2.4, 1.0);
    base.setF32(metric::kPcStreamFps, 72, 1.0);
    base.setF32(metric::kPcStreamWireMbps, 151.2, 1.0);
    for (unsigned j = 0; j < 7; ++j) {
        base.setF32(static_cast<uint16_t>(1040 + 2 * j), 1.0 + j, 1.0);
        base.setF32(static_cast<uint16_t>(1041 + 2 * j), 2.0 + j, 1.0);
    }
    base.setF32(metric::kPcStreamLink0Ping, 11.9, 1.0);
    base.setF32(metric::kPcStreamLink0Share, 67, 1.0);
    CHECK(base.setF32(metric::kPcStreamRetransmitRequests, 0, 1.0));
    base.setText(metric::kPcStreamBuild, "1791581198", 1.0);

    MetricStore mismatch = base;
    for (uint16_t id = 1030; id <= 1053; ++id) mismatch.setNa(id, Reason::SteamVrChanged, 1.0);
    mismatch.setText(metric::kPcStreamL3Note, "CSV header/stride mismatch", 1.0);

    const Layout layout = oneWidget(inst(W::Streaming));
    // the timing/bitrate block greys out: the picture differs from the fully-working one
    CHECK(renderPx(layout, base) != renderPx(layout, mismatch, nullptr, "streaming_header_mismatch.png"));

    // L2 (link ping/share) is untouched by an L3-only mismatch: its own data still changes the picture
    MetricStore mismatchDifferentLink = mismatch;
    mismatchDifferentLink.setF32(metric::kPcStreamLink0Ping, 45.1, 1.0);
    CHECK(renderPx(layout, mismatch) != renderPx(layout, mismatchDifferentLink));

    // likewise a health counter (retransmits): still responds to its own data under an L3 mismatch
    MetricStore mismatchDifferentHealth = mismatch;
    CHECK(mismatchDifferentHealth.setF32(metric::kPcStreamRetransmitRequests, 12, 1.0));
    CHECK(renderPx(layout, mismatch) != renderPx(layout, mismatchDifferentHealth));
}

// ---- Widget polish: conditional shrink across widgets 9 (Network), 10 (Streaming), 11 (General) ----

/** A fully working PC snapshot: every Streaming section (L1 named, L2 links, L3 stages + bitrate) present. */
MetricStore fullStreamingStore() {
    MetricStore s;
    s.setF32(metric::kPcStreamPing, 2.4, kNow);
    s.setF32(metric::kPcStreamNamedEncode, 2.6, kNow);
    s.setF32(metric::kPcStreamNamedSend, 0.2, kNow);
    for (unsigned j = 0; j < 7; ++j) {
        s.setF32(static_cast<uint16_t>(1040 + 2 * j), 1.0 + j, kNow);
        s.setF32(static_cast<uint16_t>(1041 + 2 * j), 2.0 + j, kNow);
    }
    for (uint16_t id : {metric::kPcStreamWireMbps, metric::kPcStreamVideoMbps, metric::kPcStreamFecMbps,
                        metric::kPcStreamAudioMbps, metric::kPcStreamRetryMbps, metric::kPcStreamLogicMbps}) {
        s.setF32(id, 10.0, kNow);
    }
    s.setF32(metric::kPcStreamLink0Ping, 11.9, kNow);
    s.setF32(metric::kPcStreamLink0Share, 67, kNow);
    CHECK(s.setF32(metric::kPcStreamRetransmitRequests, 0, kNow));
    s.setText(metric::kPcStreamBuild, "1791581198", kNow);
    return s;
}

void testStreamingHeightByState() {
    const WidgetInstance streaming = inst(W::Streaming);
    WidgetContext ctx;
    ctx.now = kNow;

    // No PC app at all: collapsed to the fixed 108 px (link mode / FEC only)
    MetricStore noPc;
    setPcStreamNa(noPc, Reason::PcAppNotRunning, kNow);
    ctx.store = &noPc;
    CHECK(widgetHeight(streaming, &ctx) == 108);

    // PC present, but SteamVR isn't running on it: also collapsed to 108
    MetricStore noSteamVr;
    setPcStreamNa(noSteamVr, Reason::NoSteamVr, kNow);
    ctx.store = &noSteamVr;
    CHECK(widgetHeight(streaming, &ctx) == 108);

    // Fully working: taller than the collapsed state
    MetricStore full = fullStreamingStore();
    ctx.store = &full;
    const int fullH = widgetHeight(streaming, &ctx);
    CHECK(fullH > 108);

    // L3 N/A (header mismatch): the stage legend and bitrate drop (shorter than full), but L1 (named),
    // L2 (links) and health stay -- so it's still well above the fully-collapsed 108 px
    MetricStore l3Na = full;
    for (uint16_t id = 1030; id <= 1053; ++id) l3Na.setNa(id, Reason::SteamVrChanged, kNow);
    l3Na.setText(metric::kPcStreamL3Note, "CSV header/stride mismatch", kNow);
    ctx.store = &l3Na;
    const int l3NaH = widgetHeight(streaming, &ctx);
    CHECK(l3NaH < fullH);
    CHECK(l3NaH > 108);
    CHECK(renderPx(oneWidget(streaming), full) != renderPx(oneWidget(streaming), l3Na, nullptr,
                                                           "streaming_l3_na_keeps_l1_l2_health.png"));
}

void testStreamingHealthChipsFixedWidthRegardlessOfCount() {
    // "Health chips fixed-width 3 columns, counts must not resize": a count moving from single digit to six
    // digits must not change the card's height (chips wrap at a fixed column count, not by content width)
    MetricStore small = fullStreamingStore();
    CHECK(small.setF32(metric::kPcStreamRetransmitRequests, 0, kNow));
    MetricStore big = fullStreamingStore();
    CHECK(big.setF32(metric::kPcStreamRetransmitRequests, 999999, kNow));
    WidgetContext smallCtx;
    smallCtx.store = &small;
    smallCtx.now = kNow;
    WidgetContext bigCtx;
    bigCtx.store = &big;
    bigCtx.now = kNow;
    const WidgetInstance streaming = inst(W::Streaming);
    CHECK(widgetHeight(streaming, &smallCtx) == widgetHeight(streaming, &bigCtx));
    // the picture still responds to the value (it's just not wider/taller for it)
    CHECK(renderPx(oneWidget(streaming), small) != renderPx(oneWidget(streaming), big));
}

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
    CHECK(withClock - withoutClock > 0);  // the Time row's height, exactly

    ctx.clockFormat = 12;
    CHECK(widgetHeight(general, &ctx) == withClock);  // 12 h / 24 h both show the row; only "Off" hides it

    // no context at all (e.g. the layout editor, which doesn't know the dashboard's clock setting): the fixed
    // fallback must reserve at least as much room as showing the clock does
    CHECK(widgetHeight(general) >= withClock);
}

void testNetworkShrinksWhenNotExpanded() {
    const WidgetInstance dongle = inst(W::Network, 15, NetAdapter::Dongle);

    MetricStore absent;  // kNetAdapterPresent N/A: not expanded
    WidgetContext absentCtx;
    absentCtx.store = &absent;
    absentCtx.now = kNow;
    CHECK(widgetHeight(dongle, &absentCtx) == 64);

    MetricStore disconnected;
    disconnected.setEnum(metric::kNetDongleBase + metric::kNetAdapterPresent, 1, kNow);
    disconnected.setEnum(metric::kNetDongleBase + metric::kNetAdapterConnected, 0, kNow);
    WidgetContext disconnectedCtx;
    disconnectedCtx.store = &disconnected;
    disconnectedCtx.now = kNow;
    CHECK(widgetHeight(dongle, &disconnectedCtx) == 64);

    MetricStore connected;
    connected.setEnum(metric::kNetDongleBase + metric::kNetAdapterPresent, 1, kNow);
    connected.setEnum(metric::kNetDongleBase + metric::kNetAdapterConnected, 1, kNow);
    WidgetContext connectedCtx;
    connectedCtx.store = &connected;
    connectedCtx.now = kNow;
    CHECK(widgetHeight(dongle, &connectedCtx) == 108);

    CHECK(widgetHeight(dongle) == 108);  // no ctx: fixed fallback keeps the full reservation

    CHECK(renderPx(oneWidget(dongle), disconnected, nullptr, "network_collapsed.png") !=
          renderPx(oneWidget(dongle), connected, nullptr, "network_expanded.png"));
}

void testListAndCameraHeightsIgnoreContext() {
    // these never had a dynamic-height concern (maxItems / fixed tile layout only), so adding the ctx
    // parameter elsewhere must not have changed anything about them
    MetricStore store;
    WidgetContext ctx;
    ctx.store = &store;
    ctx.now = kNow;
    for (const WidgetInstance& w : {inst(W::CpuItems, 10), inst(W::RamItems, 6), inst(W::HeadsetCamera),
                                    inst(W::EyeCamera)}) {
        CHECK(widgetHeight(w) == widgetHeight(w, &ctx));
    }
}

void testCpuClusterRowsShrinkWhenSensorsMissing() {
    const WidgetInstance cpu = inst(W::Cpu);
    WidgetContext ctx;
    ctx.now = kNow;

    // no cluster clock info at all: core bars/header stay, both cluster rows gone
    MetricStore allMissing;
    for (uint16_t k = 0; k < 4; ++k) allMissing.setNa(static_cast<uint16_t>(metric::kCpuCluster0Mhz + k),
                                                      Reason::SensorMissing, kNow);
    ctx.store = &allMissing;
    CHECK(widgetHeight(cpu, &ctx) == 120);

    // only cluster 0 (row 0, the first of its pair) valid: row 0 reserved, row 1 (both its slots missing) isn't
    MetricStore c0Valid = allMissing;
    c0Valid.setF32(metric::kCpuCluster0Mhz, 2035.2, kNow);
    ctx.store = &c0Valid;
    CHECK(widgetHeight(cpu, &ctx) == 144);

    // only cluster 2 (row 1) valid: row 1's fixed position still has to be reserved, so this is the full
    // 2-row height even though row 0 itself is empty (clusters are drawn at a fixed slot, not reflowed)
    MetricStore c2Valid = allMissing;
    c2Valid.setF32(static_cast<uint16_t>(metric::kCpuCluster0Mhz + 2), 2035.2, kNow);
    ctx.store = &c2Valid;
    CHECK(widgetHeight(cpu, &ctx) == 166);

    // all four present (today's common real-hardware case): unchanged from the old fixed height
    MetricStore allValid = allMissing;
    for (uint16_t k = 0; k < 4; ++k) {
        allValid.setF32(static_cast<uint16_t>(metric::kCpuCluster0Mhz + k), 2000.0 + k * 100, kNow);
    }
    ctx.store = &allValid;
    CHECK(widgetHeight(cpu, &ctx) == 166);

    // a numeric value change (still present) must not move the height
    MetricStore allValidChanged = allValid;
    allValidChanged.setF32(metric::kCpuCluster0Mhz, 3000.0, kNow);
    ctx.store = &allValidChanged;
    CHECK(widgetHeight(cpu, &ctx) == 166);

    // then a sensor actually goes missing: the row it was the last occupant of shrinks
    MetricStore row1Gone = allValid;
    row1Gone.setNa(static_cast<uint16_t>(metric::kCpuCluster0Mhz + 2), Reason::SensorMissing, kNow);
    row1Gone.setNa(static_cast<uint16_t>(metric::kCpuCluster0Mhz + 3), Reason::SensorMissing, kNow);
    ctx.store = &row1Gone;
    CHECK(widgetHeight(cpu, &ctx) == 144);

    // ReadFailed (not SensorMissing) still counts as present -- the existing draw predicate only skips a
    // slot that's both N/A and specifically SensorMissing ("past the last core/cluster")
    MetricStore readFailed = allMissing;
    readFailed.setNa(static_cast<uint16_t>(metric::kCpuCluster0Mhz + 2), Reason::ReadFailed, kNow);
    ctx.store = &readFailed;
    CHECK(widgetHeight(cpu, &ctx) == 166);

    // no context at all: the fixed fallback must cover at least the busiest real case built here
    CHECK(widgetHeight(cpu) >= 166);

    CHECK(renderPx(oneWidget(cpu), allMissing) != renderPx(oneWidget(cpu), allValid, nullptr, "cpu_clusters.png"));
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
    CHECK(headsetCam != eyeCam);  // T10: one video0 tile vs the two eye tiles
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

namespace {

// ---- R2: Compact cards and the Frame frame-time graphs ----

WidgetInstance compactInst(W type, bool compact) {
    WidgetInstance w = inst(type);
    w.compact = compact;
    return w;
}

MetricStore r2Store(double scale, double now) {
    MetricStore s;
    s.setF32(metric::kFrameAppFps, 60 + 10 * scale, now);
    s.setF32(metric::kFrameDisplayHz, 90, now);
    s.setF32(metric::kFrameCpuMs, 4 * scale, now);
    s.setF32(metric::kFrameGpuMs, 5 * scale, now);
    s.setF32(metric::kFrameCpuMaxMs, 9 * scale, now);
    s.setF32(metric::kFrameGpuMaxMs, 12 * scale, now);
    s.setF32(metric::kPowerVphW, 3 + 4 * scale, now);
    s.setF32(metric::kTempCpuC, 40 + 10 * scale, now);
    s.setF32(metric::kTempGpuC, 45 + 10 * scale, now);
    s.setF32(metric::kBatteryPct, 40 + 20 * scale, now);
    return s;
}

void testCompactHeights() {
    CHECK(widgetHeight(compactInst(W::Frame, false)) == 194);
    CHECK(widgetHeight(compactInst(W::Frame, true)) == 102);
    CHECK(widgetHeight(compactInst(W::Power, false)) == 372);
    CHECK(widgetHeight(compactInst(W::Power, true)) == 102);
    CHECK(widgetHeight(compactInst(W::Temp, false)) == 198);
    CHECK(widgetHeight(compactInst(W::Temp, true)) == 102);
    CHECK(widgetHeight(compactInst(W::Battery, true)) == 88);
    CHECK(widgetHeight(compactInst(W::Battery, true)) < widgetHeight(compactInst(W::Battery, false)));

    // no jitter: values moving doesn't move a height or the grid
    const MetricStore low = r2Store(1.0, kNow);
    const MetricStore high = r2Store(2.5, kNow);
    WidgetContext lowCtx, highCtx;
    lowCtx.store = &low;
    lowCtx.now = kNow;
    highCtx.store = &high;
    highCtx.now = kNow;
    for (const W type : {W::Frame, W::Power, W::Temp, W::Battery}) {
        for (const bool compact : {false, true}) {
            const WidgetInstance w = compactInst(type, compact);
            CHECK(widgetHeight(w, &lowCtx) == widgetHeight(w, &highCtx));
        }
    }
    Layout layout;
    layout.columns.resize(2);
    layout.columns[0].widgets = {compactInst(W::Frame, false), compactInst(W::Power, true)};
    layout.columns[1].widgets = {compactInst(W::Temp, false), compactInst(W::Battery, true)};
    CHECK(sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(layout, &highCtx)));

    // toggling compact changes the geometry
    Layout toggled = layout;
    toggled.columns[0].widgets[0].compact = true;
    CHECK(!sameGridGeometry(layoutGrid(layout, &lowCtx), layoutGrid(toggled, &lowCtx)));
    CHECK(layoutGrid(toggled, &lowCtx).slots[0].h == 102);
    CHECK(layoutGrid(layout, &lowCtx).slots[0].h == 194);
}

/** The Frame card's frame-time band (under the fps row), from a rendered grid. */
Pixels frameBand(GridRenderer& renderer) {
    const WidgetSlot& slot = renderer.geometry().slots[0];
    const int stride = renderer.geometry().textureWidth * 4;
    const std::vector<uint8_t>& rgba = renderer.toRgba();
    const int top = slot.y + 124;  // kFrameTimesBedY
    Pixels band;
    for (int row = top; row < top + 60; ++row) {
        band.insert(band.end(), rgba.begin() + row * stride + slot.x * 4,
                    rgba.begin() + row * stride + (slot.x + slot.w) * 4);
    }
    return band;
}

/** Renders a Frame widget over 12 seconds of data; hz 0 leaves the display rate unknown. */
Pixels renderFrameHistory(double hz, double dataScale) {
    GridRenderer renderer(fonts());
    renderer.setLayout(oneWidget(compactInst(W::Frame, false)));
    for (int i = 0; i < 12; ++i) {
        MetricStore s = r2Store(dataScale * (1.0 + 0.1 * (i % 4)), kNow + i);
        if (hz == 0) s.setF32(metric::kFrameDisplayHz, std::nan(""), kNow + i);
        else s.setF32(metric::kFrameDisplayHz, hz, kNow + i);
        WidgetContext ctx;
        ctx.store = &s;
        ctx.now = kNow + i;
        renderer.render(ctx);
    }
    return frameBand(renderer);
}

void testFrameGraphsDraw() {
    const Pixels withData = renderFrameHistory(90, 1.0);
    const Pixels spike = renderFrameHistory(90, 3.0);  // taller series and a bigger msMax
    const Pixels noHz = renderFrameHistory(0, 1.0);    // no dotted guide at 1000/Hz
    CHECK(!withData.empty());
    CHECK(withData != spike);
    CHECK(withData != noHz);

    // empty store: draws without crashing, and differs from the one with data
    MetricStore empty;
    GridRenderer renderer(fonts());
    renderer.setLayout(oneWidget(compactInst(W::Frame, false)));
    WidgetContext ctx;
    ctx.store = &empty;
    ctx.now = kNow;
    CHECK(renderer.render(ctx));
    CHECK(frameBand(renderer) != withData);
}

void testCompactToggleKeepsFrameHistory() {
    const Layout full = oneWidget(compactInst(W::Frame, false));
    const Layout compact = oneWidget(compactInst(W::Frame, true));
    GridRenderer toggled(fonts());
    GridRenderer steady(fonts());
    toggled.setLayout(full);
    steady.setLayout(full);
    MetricStore last;
    for (int i = 0; i < 12; ++i) {
        last = r2Store(1.0 + 0.1 * (i % 4), kNow + i);
        WidgetContext ctx;
        ctx.store = &last;
        ctx.now = kNow + i;
        toggled.render(ctx);
        steady.render(ctx);
    }
    toggled.setLayout(compact);
    CHECK(toggled.geometry().slots[0].h == 102);
    WidgetContext ctx;
    ctx.store = &last;
    ctx.now = kNow + 11;
    toggled.render(ctx);
    toggled.setLayout(full);
    CHECK(toggled.geometry().slots[0].h == 194);
    toggled.render(ctx);
    steady.render(ctx);
    CHECK(frameBand(toggled) == frameBand(steady));
}

// ---- R3: the segmented pill ----

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

    // a one-part pill (the cam toggles): selected = 0 fills with the accent, -1 stays kControl inside
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 100, 48, {{"video3"}}, 0, true);
    CHECK(pixelRgb(surface, 10 + 5 + 4, 34) == 0xe27dfd);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    drawSegmentedPill(pen, 10, 10, 100, 48, {{"video3"}}, -1, true);
    CHECK(pixelRgb(surface, 10 + 5 + 4, 34) == 0x21262d);
    CHECK(pixelRgb(surface, 10 + 50, 11) != 0x21262d);  // the border ring, not the fill
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// ---- Streaming: Slack split into Hold + Display ----

/** fullStreamingStore() plus the four split metrics (ids 1054-1057). */
MetricStore splitStreamingStore(double holdAvg = 4.0, double holdP95 = 6.0) {
    MetricStore s = fullStreamingStore();
    s.setF32(1054, holdAvg, kNow);
    s.setF32(1055, holdP95, kNow);
    s.setF32(1056, 32.0, kNow);
    s.setF32(1057, 33.0, kNow);
    return s;
}

void testStreamingHoldDisplaySplit() {
    WidgetInstance full = inst(W::Streaming);
    WidgetInstance compact = inst(W::Streaming);
    compact.compact = true;

    const MetricStore fallback = fullStreamingStore(); // 1054-1057 absent: single Slack segment
    const MetricStore split = splitStreamingStore();
    const MetricStore negative = splitStreamingStore(-3.0, -1.0); // frame missed its slot: bar width clamps to 0
    MetricStore hdNa = splitStreamingStore();                     // reader had no Decode Query this window
    for (uint16_t id = 1054; id <= 1057; ++id) hdNa.setNa(id, Reason::NoFrames, kNow);
    MetricStore partial = splitStreamingStore();                  // one of the four missing -> fallback
    partial.setNa(1056, Reason::NoFrames, kNow);
    MetricStore nonFinite = splitStreamingStore();
    nonFinite.setF32(1055, std::numeric_limits<double>::infinity(), kNow);

    // layout is unchanged by the split in either mode (measureStreaming doesn't look at 1054-1057)
    for (const WidgetInstance& w : {full, compact}) {
        int heights[6];
        const MetricStore* stores[6] = {&fallback, &split, &negative, &hdNa, &partial, &nonFinite};
        for (int i = 0; i < 6; ++i) {
            WidgetContext ctx;
            ctx.store = stores[i];
            ctx.now = kNow;
            heights[i] = widgetHeight(w, &ctx);
            CHECK(heights[i] == heights[0]);
        }
    }

    // every variant renders; the split draws differently from the fallback, and a degraded split IS the fallback
    for (const WidgetInstance& w : {full, compact}) {
        const Pixels fb = renderPx(oneWidget(w), fallback);
        CHECK(!fb.empty());
        CHECK(renderPx(oneWidget(w), split) != fb);
        CHECK(renderPx(oneWidget(w), negative, nullptr, w.compact ? "streaming_compact_negative_hold.png"
                                                                   : "streaming_negative_hold.png") != fb);
        CHECK(!renderPx(oneWidget(w), negative).empty());
        CHECK(renderPx(oneWidget(w), hdNa) == fb);
        CHECK(renderPx(oneWidget(w), partial) == fb);
        CHECK(renderPx(oneWidget(w), nonFinite) == fb);
    }
    // negative hold and zero hold draw the same bar (clamped), though the legend text differs
    CHECK(!renderPx(oneWidget(full), splitStreamingStore(0.0, 0.0)).empty());
}

// ---- R3: Streaming Compact ----

void testStreamingCompact() {
    WidgetInstance full = inst(W::Streaming);
    WidgetInstance compact = inst(W::Streaming);
    compact.compact = true;
    CHECK(widgetHeight(compact) == 230 && widgetHeight(full) == 504);  // no context: the fixed reservations

    // PC present
    MetricStore pc = fullStreamingStore();
    WidgetContext ctx;
    ctx.store = &pc;
    ctx.now = kNow;
    const int fullH = widgetHeight(full, &ctx);
    const int compactH = widgetHeight(compact, &ctx);
    CHECK(fullH == 347);  // one link row
    CHECK(compactH == 230);

    // a second link adds a row to full (20) and nothing to compact (no Links section): 367 / 230
    MetricStore twoLinks = fullStreamingStore();
    twoLinks.setF32(metric::kPcStreamLink0Ping + 2, 14.0, kNow);
    WidgetContext twoCtx;
    twoCtx.store = &twoLinks;
    twoCtx.now = kNow;
    CHECK(widgetHeight(full, &twoCtx) == 367);
    CHECK(widgetHeight(compact, &twoCtx) == 230);

    // no jitter: the numbers moving doesn't move either height
    MetricStore moved = fullStreamingStore();
    moved.setF32(metric::kPcStreamPing, 55.5, kNow);
    moved.setF32(metric::kPcStreamLink0Ping, 99.9, kNow);
    moved.setF32(metric::kPcStreamLink0Share, 12, kNow);
    moved.setF32(metric::kPcStreamNamedEncode, 9.9, kNow);
    for (const uint16_t id : {metric::kPcStreamWireMbps, metric::kPcStreamVideoMbps}) moved.setF32(id, 250.0, kNow);
    WidgetContext movedCtx;
    movedCtx.store = &moved;
    movedCtx.now = kNow;
    CHECK(widgetHeight(full, &movedCtx) == fullH);
    CHECK(widgetHeight(compact, &movedCtx) == compactH);

    // a note line makes both grow by one line (16)
    MetricStore noted = fullStreamingStore();
    noted.setText(metric::kPcStreamL1Note, "a note", kNow);
    WidgetContext notedCtx;
    notedCtx.store = &noted;
    notedCtx.now = kNow;
    CHECK(widgetHeight(full, &notedCtx) == fullH + 16);
    CHECK(widgetHeight(compact, &notedCtx) == compactH + 16);

    // PC absent / no SteamVR: 108 full, 72 compact (state line only)
    MetricStore noPc;
    setPcStreamNa(noPc, Reason::PcAppNotRunning, kNow);
    MetricStore noSteamVr;
    setPcStreamNa(noSteamVr, Reason::NoSteamVr, kNow);
    for (const MetricStore* store : {&noPc, &noSteamVr}) {
        WidgetContext c;
        c.store = store;
        c.now = kNow;
        CHECK(widgetHeight(full, &c) == 108);
        CHECK(widgetHeight(compact, &c) == 72);
    }

    // it draws, and Compact looks different from full
    CHECK(renderPx(oneWidget(compact), pc) != renderPx(oneWidget(full), pc));
    CHECK(!renderPx(oneWidget(compact), noPc).empty());
}

// The header: the mode pill follows the title, and stays clear of the right-aligned "fps | Hz | ping" group.
// (Same formulas as drawStreaming: pill at kInX + title + 10, width text + 12; the group ends at kInR = 490.)
void testStreamingHeaderDoesNotOverlap() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts()};
    const double inX = 6, inR = 496 - 6;
    const std::string title = widgetTitle(inst(W::Streaming));
    const double titleEnd = inX + pen.measure(title, 16);
    const double pillX = titleEnd + 10;
    CHECK(pillX > titleEnd);
    const std::string right = "144 fps  |  144 Hz  |  ping 99.9 ms";
    const double rightLeft = inR - pen.measure(right, 11);
    for (const char* mode : {"Not streaming", "Dongle", "Home Wi-Fi", "Multi-link", "Idle", "Link --"}) {
        const double pillRight = pillX + pen.measure(mode, 11) + 12;
        CHECK(pillRight < rightLeft);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

}  // namespace

int main() {
    testGeometry();
    testListRows();
    testBatteryTimeRows();
    testUnmeasuredNegative();
    testGfxVoltsDash();
    testBatteryRowCountMirrorsDrawnPredicates();
    testBatteryReflow();
    testStreamingNoPcStillShowsLinkAndFec();
    testStreamingHeaderMismatchKeepsL1L2HealthShown();
    testStreamingHeightByState();
    testStreamingHealthChipsFixedWidthRegardlessOfCount();
    testGeneralClockOffShrinks();
    testNetworkShrinksWhenNotExpanded();
    testListAndCameraHeightsIgnoreContext();
    testCpuClusterRowsShrinkWhenSensorsMissing();
    testPlaceholders();
    testEmptyStoreAllTypes();
    testDefaultFilled();
    testChangeDetection();
    testPartialRedraw();
    testClockOffHidesTime();
    testCappedShowsHardwareMax();
    testCompactHeights();
    testFrameGraphsDraw();
    testCompactToggleKeepsFrameHistory();
    testSegmentedPillColours();
    testStreamingCompact();
    testStreamingHoldDisplaySplit();
    testStreamingHeaderDoesNotOverlap();
    if (failures == 0) std::printf("render-grid-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
