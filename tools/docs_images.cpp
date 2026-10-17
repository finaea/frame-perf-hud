// The README's pictures, drawn by the shared widget renderer (the same code the headset uses) with made-up but
// realistic sample values: a PC-streamed game on the dongle. Nothing here is a live measurement.
// Usage: fph-docs-images OUT_DIR   (writes panel-en.png, panel-compact-en.png, panel-warnings-en.png,
//                                  pc-window-layout.png, pc-window-profiles.png, pc-window-logs.png)
#include "layout.h"
#include "metric_registry.h"
#include "panel_view.h"
#include "pc_pages.h"
#include "stream_metrics.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace fph;
using namespace fph::metric;

/** One second-ish of a busy but healthy session at time t; warnings = the hot / throttled / low battery picture. */
void fillStore(MetricStore& st, double t, bool warnings) {
    const double wob = std::sin(t * 1.3), slow = std::sin(t * 0.37);
    // Frame: 72 Hz, the odd hitch
    const bool hitch = std::fmod(t, 9.0) < 0.5;
    const double hz = warnings ? 72.0 : 72.0;
    st.setF32(kFrameDisplayHz, hz, t);
    st.setF32(kFrameAppFps, warnings ? 36.0 - (hitch ? 2 : 0) : 71.4 + 0.5 * wob - (hitch ? 6 : 0), t);
    st.setF32(kFrameGpuMs, warnings ? 21.5 + wob : 8.6 + 1.2 * wob + (hitch ? 4 : 0), t);
    st.setF32(kFrameGpuMaxMs, warnings ? 24.0 + wob : 10.3 + 1.4 * wob + (hitch ? 6 : 0), t);
    st.setF32(kFrameCpuMs, 5.1 + 0.6 * slow, t);
    st.setF32(kFrameCpuMaxMs, 7.2 + 0.9 * slow + (hitch ? 3 : 0), t);
    st.setF32(kFrameReprojectedPct, warnings ? 50.0 : (hitch ? 4.0 : 0.3), t);
    st.setI32(kFrameDropped, hitch ? 1 : 0, t);
    st.setEnum(kFrameThrottled, warnings ? 1 : 0, t);
    st.setI32(kFrameCount, 36, t);
    // CPU
    const double cores[8] = {22, 18, 31, 27, 64, 58, 71, 40};
    double sum = 0;
    for (int i = 0; i < 8; ++i) {
        st.setF32(kCpuCore0Pct + i, cores[i] + 6 * std::sin(t * (0.7 + 0.1 * i)), t);
        sum += cores[i];
    }
    st.setF32(kCpuUsagePct, sum / 8 + 3 * slow, t);
    st.setF32(kCpuMaxCorePct, 71 + 6 * wob, t);
    st.setF32(kCpuFastestClockMhz, 2918, t);
    st.setF32(kCpuBigCoreClockMhz, 2496, t);
    const double cur[4] = {1555, 2246, 2496, 2918}, max[4] = {1785, 2496, 2918, 3052};
    for (int i = 0; i < 4; ++i) {
        st.setF32(kCpuCluster0Mhz + i, cur[i], t);
        st.setF32(kCpuCluster0MaxMhz + i, warnings && i == 3 ? 2400 : max[i], t);
        st.setF32(kCpuCluster0HwMaxMhz + i, max[i], t);
    }
    // GPU & RAM
    st.setF32(kGpuBusyPct, warnings ? 97 : 62 + 8 * wob, t);
    st.setF32(kGpuClockMhz, 680, t);
    st.setF32(kGpuMaxClockMhz, 900, t);
    st.setF32(kRamUsedGib, 6.4, t);
    st.setF32(kRamTotalGib, 15.3, t);
    // Power
    const double rails[10] = {0.62, 0.81, 1.24, 2.35, 0.05, 0.06, 0.41, 0.33, 0.48, 0.19};
    const double volts[10] = {0.60, 0.66, 0.72, 0.0, 0.81, 0.86, 1.26, 1.01, 1.89, 4.26};
    double railSum = 0;
    for (int i = 0; i < 10; ++i) {
        st.setF32(kPowerApc0W + i, rails[i] * (1 + 0.08 * wob), t);
        if (i == 3) st.setNa(kPowerApc0W + i + kPowerRailVoltsOffset, Reason::InvalidReading, t);
        else st.setF32(kPowerApc0W + i + kPowerRailVoltsOffset, volts[i], t);
        railSum += rails[i];
    }
    st.setF32(kPowerRailsW, railSum, t);
    st.setF32(kPowerVphW, 9.1 + 0.7 * wob, t);
    st.setF32(kPowerUnmeasuredW, 9.1 - railSum, t);
    st.setF32(kPowerVphV, 4.26, t);
    // Battery
    st.setF32(kBatteryPct, warnings ? 12 : 74, t);
    st.setEnum(kBatteryStatus, static_cast<uint32_t>(BatteryStatus::Discharging), t);
    st.setEnum(kBatteryChargerOnline, 0, t);
    st.setEnum(kBatteryPlugged, 0, t);
    st.setF32(kBatteryPowerW, -8.4, t);
    st.setF32(kBatteryVoltageV, 8.21, t);
    st.setF32(kBatteryCurrentMa, -1023, t);
    st.setF32(kBatteryChargeNowMah, warnings ? 328 : 2022, t);
    st.setF32(kBatteryChargeFullMah, 2730, t);
    st.setF32(kBatteryChargeDesignMah, 2730, t);
    st.setText(kBatteryHealth, "Good", t);
    st.setI32(kBatteryCycles, 31, t);
    st.setText(kBatteryChargerMode, "Unplugged", t);
    st.setI32(kBatteryTimeToEmptyS, warnings ? 1150 : 7116, t);
    st.setNa(kBatteryTimeToFullS, Reason::NotConnected, t);
    st.setF32(kBatterySetCurrentA, 1, t);
    st.setF32(kBatterySetCurrentMaxA, 3, t);
    // Temp
    st.setF32(kTempCpuC, warnings ? 86 : 61 + 2 * slow, t);
    st.setF32(kTempGpuC, warnings ? 84 : 57 + 2 * slow, t);
    st.setF32(kTempDdrC, 51, t);
    st.setF32(kTempBatteryC, 34, t);
    st.setF32(kTempDisplayC, 41, t);
    st.setF32(kTempExhaustC, 46, t);
    st.setF32(kTempHeatsinkC, 49, t);
    st.setEnum(kThrottleCpu, warnings ? 1 : 0, t);
    st.setEnum(kThrottleGpu, warnings ? 1 : 0, t);
    st.setF32(kFanRpm, 3380, t);
    st.setF32(kTempNpuC, 45, t);
    st.setF32(kTempCameraC, 48, t);
    st.setF32(kTempVideoC, 47, t);
    st.setF32(kTempWifiC, 47, t);
    st.setF32(kTempFlashC, 40, t);
    st.setF32(kTempNrfC, 38, t);
    st.setF32(kTempConnectorC, 36, t);
    // Network: the PC on the dongle, home Wi-Fi joined but quiet
    const struct {
        uint16_t base;
        bool dongle;
    } nets[] = {{kNetDongleBase, true}, {kNetHomeBase, false}};
    for (const auto& n : nets) {
        st.setEnum(n.base + kNetAdapterPresent, 1, t);
        st.setEnum(n.base + kNetAdapterConnected, 1, t);
        st.setF32(n.base + kNetAdapterRxMbps, n.dongle ? 186 + 12 * wob : 0.04, t);
        st.setF32(n.base + kNetAdapterTxMbps, n.dongle ? 2.1 : 0.02, t);
        st.setF32(n.base + kNetAdapterTxLinkMbps, n.dongle ? 1152.9 : 866.7, t);
        st.setF32(n.base + kNetAdapterRxLinkMbps, n.dongle ? 960.7 : 780.0, t);
        st.setF32(n.base + kNetAdapterSignalDbm, warnings ? -81 : (n.dongle ? -44 : -53), t);
        st.setText(n.base + kNetAdapterIp, n.dongle ? "10.35.78.1" : "192.168.1.47", t);
        st.setI32(n.base + kNetAdapterFreqMhz, n.dongle ? 6135 : 5500, t);
        st.setI32(n.base + kNetAdapterChannel, n.dongle ? 37 : 100, t);
        st.setI32(n.base + kNetAdapterWidthMhz, n.dongle ? 160 : 80, t);
        st.setEnum(n.base + kNetAdapterBand, static_cast<uint32_t>(n.dongle ? WifiBand::Ghz6 : WifiBand::Ghz5), t);
        st.setText(n.base + kNetAdapterIfName, n.dongle ? "wlanap" : "wlan0", t);
    }
    // Streaming, headset side + the PC's stats (as the PC app sends them)
    st.setEnum(kStreamLinkMode, static_cast<uint32_t>(StreamLinkMode::Dongle), t);
    st.setEnum(kStreamVrlinkRunning, 1, t);
    st.setF32(kStreamDongleSharePct, 100, t);
    st.setI32(kStreamFecRetries, 3, t);
    st.setI32(kStreamFecRetryPackets, 21, t);
    st.setI32(kStreamFramesDropped, 0, t);
    st.setI32(kStreamLateMetadata, 1, t);
    st.setI32(kStreamAborts, 0, t);
    st.setI32(kStreamUnrecoverable, 0, t);
    st.setI32(kStreamLogOmitted, 0, t);
    for (const auto& d : registry()) {
        if (!isPcStreamMetric(d.id)) continue;
        if (d.type == WireType::F32) st.setF32(d.id, 0, t);
        else if (d.type == WireType::I32) st.setI32(d.id, 0, t);
        else if (d.type == WireType::ShortString) st.setText(d.id, "", t);
    }
    st.setF32(kPcStreamPing, 2.4, t);
    st.setF32(kPcStreamFps, 72, t);
    st.setF32(kPcStreamDisplayHz, 72, t);
    st.setText(kPcStreamLink0Address, "10.35.78.1", t);
    st.setF32(kPcStreamWireMbps, 151.2 + 8 * wob, t);
    st.setF32(kPcStreamVideoMbps, 125.4 + 7 * wob, t);
    st.setF32(kPcStreamFecMbps, 17.8, t);
    st.setF32(kPcStreamAudioMbps, 3.4, t);
    st.setF32(kPcStreamRetryMbps, 3.2, t);
    st.setF32(kPcStreamLogicMbps, 1.4, t);
    const double stages[7] = {1.1, 2.6, 0.2, 7.8, 8.3, 37.2, 57.2};  // game, encode, send, network, decode, slack, total
    for (unsigned j = 0; j < 7; ++j) {
        st.setF32(static_cast<uint16_t>(1040 + 2 * j), stages[j] + 0.3 * wob, t);
        st.setF32(static_cast<uint16_t>(1041 + 2 * j), stages[j] + 1.1, t);
    }
    st.setF32(kPcStreamNamedEncode, 2.7, t);
    st.setF32(kPcStreamNamedSend, 0.2, t);
    st.setF32(kPcStreamEstimatedBandwidth, 232, t);
    st.setF32(kPcStreamLink0Ping, 11.9, t);
    st.setF32(kPcStreamLink0Share, 100, t);
    for (uint16_t id = 1062; id <= 1067; ++id) st.setNa(id, Reason::NotConnected, t);
    st.setText(kPcStreamSceneKey, "steam.app.546560", t);
    st.setText(kPcStreamSceneName, "Half-Life: Alyx", t);
    st.setText(kPcStreamBuild, "1791581198", t);
    // General
    st.setF32(kCtrlLeftPct, 80, t);
    st.setEnum(kCtrlLeftCharging, 0, t);
    st.setF32(kCtrlRightPct, warnings ? 8 : 75, t);
    st.setEnum(kCtrlRightCharging, 0, t);
    st.setText(kSceneAppKey, "vrlink.client", t);
    st.setText(kSceneAppName, "PC stream", t);
    st.setI32(kScenePid, 4127, t);
    st.setI32(kSessionId, 1873421, t);
    st.setI32(kSessionStartUnix, 1792243200, t);
    st.setText(kStreamAppHint, "steam.app.546560", t);
    st.setI32(kUptimeS, 5412 + static_cast<int>(t), t);
    st.setText(kClock, "2026-10-17 21:14:05", t);
    st.setF32(kSelfCpuPct, 1.4, t);
    st.setF32(kSelfRssMib, 14, t);
    st.setF32(kSelfPowerW, 0.03, t);
    st.setText(kLastPowerOff, "Normal shutdown", t);
    // the process scan behind both lists
    st.setI32(kProcScanned, 312, t);
    st.setI32(kProcVanished, 0, t);
    st.setF32(kProcScanMs, 6.1, t);
    st.setI32(kCpuItemsRows, 8, t);
    st.setI32(kRamItemsRows, 8, t);
}

ProcessListsView processes(uint64_t generation) {
    ProcessListsView v;
    v.cpuValid = true;
    v.generation = generation;
    v.byCpu = {{"vrcompositor", 1210, 1, 41.2, 412},  {"vrlink", 4127, 1, 33.5, 188},
               {"vrserver", 1188, 1, 18.4, 296},      {"XRService", 902, 1, 9.7, 131},
               {"steamwebhelper", 2210, 1, 4.1, 238}, {"pipewire", 744, 1, 2.2, 21},
               {"frame-perf-hud", 3891, 1, 1.4, 14},  {"Xwayland", 801, 1, 0.9, 64}};
    v.byRam = {{"vrcompositor", 1210, 1, 41.2, 412},  {"vrserver", 1188, 1, 18.4, 296},
               {"steamwebhelper", 2210, 1, 4.1, 238}, {"steam", 1502, 1, 0.6, 221},
               {"vrlink", 4127, 1, 33.5, 188},        {"XRService", 902, 1, 9.7, 131},
               {"plasmashell", 1003, 1, 0.3, 96},     {"Xwayland", 801, 1, 0.9, 64}};
    return v;
}

WidgetInstance widget(WidgetType type, bool compact = false) {
    WidgetInstance w;
    w.type = type;
    w.compact = compact;
    w.maxItems = 8;
    return w;
}

Layout fullLayout() {
    Layout l;
    WidgetInstance home = widget(WidgetType::Network);
    home.adapter = NetAdapter::Home;
    l.columns = {{{widget(WidgetType::Frame), widget(WidgetType::Cpu), widget(WidgetType::GpuRam), widget(WidgetType::General)}},
                 {{widget(WidgetType::Power), widget(WidgetType::Battery), widget(WidgetType::Temp)}},
                 {{widget(WidgetType::Streaming), widget(WidgetType::Network), home}},
                 {{widget(WidgetType::CpuItems), widget(WidgetType::RamItems)}}};
    return l;
}

Layout compactLayout() {
    Layout l;
    l.columns = {{{widget(WidgetType::Frame, true), widget(WidgetType::Power, true), widget(WidgetType::Battery, true)}},
                 {{widget(WidgetType::Temp, true), widget(WidgetType::Streaming, true), widget(WidgetType::General)}}};
    return l;
}

Layout warningsLayout() {
    Layout l;
    l.columns = {{{widget(WidgetType::Frame), widget(WidgetType::Battery)}},
                 {{widget(WidgetType::Temp), widget(WidgetType::Cpu), widget(WidgetType::Network)}}};
    return l;
}

/** Runs 40 simulated seconds at 2 Hz (so the graphs fill) and leaves the panel drawn at `scale`. */
cairo_surface_t* simulate(fph::pc::PanelView& panel, MetricStore& store, const ProcessListsView*& lists,
                          ProcessListsView& listStorage, bool warnings, double scale) {
    cairo_surface_t* out = nullptr;
    for (int i = 0; i <= 80; ++i) {
        const double t = 100.0 + i * 0.5;
        fillStore(store, t, warnings);
        listStorage = processes(static_cast<uint64_t>(i / 4 + 1));
        lists = &listStorage;
        WidgetContext ctx;
        ctx.store = &store;
        ctx.processes = lists;
        ctx.now = t;
        panel.updateGeometry(ctx);
        out = panel.render(ctx, scale);
    }
    return out;
}

bool writePanel(const FontSet& fonts, const Layout& layout, bool warnings, const std::string& path) {
    fph::pc::PanelView panel(fonts);
    panel.setLayout(layout);
    MetricStore store;
    ProcessListsView storage;
    const ProcessListsView* lists = nullptr;
    cairo_surface_t* s = simulate(panel, store, lists, storage, warnings, 1.0);
    const bool ok = cairo_surface_write_to_png(s, path.c_str()) == CAIRO_STATUS_SUCCESS;
    std::printf("wrote %s (%dx%d)\n", path.c_str(), panel.geometry().width, panel.geometry().height);
    return ok;
}

/** The PC window: status bar, tabs, and the Layout panel or a page (as app_window.cpp lays them out). */
bool writeWindow(const FontSet& fonts, fph::pc::PcTab tab, const std::string& path) {
    using namespace fph::pc;
    const double s = 1.25;  // 120 DPI
    const int w = static_cast<int>(1180 * s), h = static_cast<int>(780 * s);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surface);
    Pen pen {cr, &fonts};
    pen.color(kBg);
    cairo_paint(cr);
    const double barH = 30 * s, pad = 8 * s;
    // the status bar, live
    drawDisc(cr, 14 * s, barH / 2, 4.5 * s, kSuccess);
    const PageBox save = drawStatusSaveButton(pen, w, barH, s);
    const std::string status = "Connected to 'frame'  ·  right-click for options";
    pen.text(26 * s, centerBaseline(0, barH, 13.5 * s), status,
             fitSize(pen, status, 13.5 * s, 9 * s, save.x - 44 * s, false), kTextMuted);
    PcPages pages;
    pages.setTab(tab);
    pages.drawTabs(pen, pad, barH, s);
    const double top = barH + PcPages::tabBarHeight(s) + pad * 0.5;
    if (tab == PcTab::Layout) {
        PanelView panel(fonts);
        Layout layout = fullLayout();
        panel.setLayout(layout);
        MetricStore store;
        ProcessListsView storage;
        const ProcessListsView* lists = nullptr;
        // fit like the window does
        WidgetContext probe;
        probe.store = &store;
        panel.updateGeometry(probe);
        simulate(panel, store, lists, storage, false, 1.0);
        const GridGeometry& g = panel.geometry();
        const double scale = std::min({(w - 2 * pad) / g.width, (h - top - pad) / g.height, 2.0 * s});
        WidgetContext ctx;
        ctx.store = &store;
        ctx.processes = lists;
        ctx.now = 140.0;
        cairo_set_source_surface(cr, panel.render(ctx, scale), std::floor(pad + std::max(0.0, (w - 2 * pad - g.width * scale) / 2)),
                                 std::floor(top));
        cairo_paint(cr);
    } else {
        link::ControlState hs;
        hs.revision = 9;
        hs.layoutVersion = 112;
        hs.flags = link::kControlUnsaved;
        hs.maxProfiles = 8;
        hs.profiles = {{"profile-1", "PC streaming"}, {"profile-2", "Standalone"}, {"profile-3", "Minimal"}};
        hs.activeId = "profile-1";
        hs.status = "Using PC streaming";
        hs.recordStatus = "";
        hs.powerOff = "Normal shutdown";
        PagesView view;
        view.headset = &hs;
        view.subscribed = true;
        view.headsetMessage = tab == PcTab::Profiles ? "Using PC streaming" : "";
        view.pcMessage = "Saved the last 10 min to saves\\2026-10-17_211405_PC-stream_save";
        view.lastCard = {"Half-Life: Alyx · 1 h 05 min", "fps avg 71.8 · 12 dropped", "Power avg 9.4 W · peak 12.1 W",
                         "Battery 82% to 61% (-21 points)", "Peak CPU 66 °C · GPU 61 °C · battery 36 °C",
                         "Stream latency avg 57.2 ms · min slack 31.4 ms · 0 link resets"};
        view.dataDir = "C:\\Users\\<user>\\AppData\\Local\\frame-perf-hud";
        pages.drawPage(pen, {pad, top, w - 2 * pad, h - top - pad}, s, view);
    }
    cairo_surface_flush(surface);
    const bool ok = cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
    std::printf("wrote %s\n", path.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path dir = std::filesystem::u8path(argv[1]);
    std::filesystem::create_directories(dir);
    FontSet fonts;
    fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
    const auto at = [&](const char* name) { return (dir / name).u8string(); };
    bool ok = writePanel(fonts, fullLayout(), false, at("panel-en.png"));
    ok &= writePanel(fonts, compactLayout(), false, at("panel-compact-en.png"));
    ok &= writePanel(fonts, warningsLayout(), true, at("panel-warnings-en.png"));
    ok &= writeWindow(fonts, fph::pc::PcTab::Layout, at("pc-window-layout.png"));
    ok &= writeWindow(fonts, fph::pc::PcTab::Profiles, at("pc-window-profiles.png"));
    ok &= writeWindow(fonts, fph::pc::PcTab::Logs, at("pc-window-logs.png"));
    return ok ? 0 : 1;
}
