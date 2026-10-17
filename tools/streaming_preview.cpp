// Offline visual evidence for the shared renderer. These values are simulated, never a live measurement.
#include "grid_renderer.h"
#include "panel_view.h"
#include "stream_metrics.h"
#include "streaming/service.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
int main(int argc, char** argv) {
    if (argc != 2 && !(argc == 3 && std::strcmp(argv[2], "--live") == 0))
        return 2;
    std::filesystem::create_directories(std::filesystem::u8path(argv[1]));
    FontSet fonts;
    fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
    using namespace fph;
    using namespace metric;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        MetricStore store;
        store.setEnum(kStreamLinkMode, static_cast<uint32_t>(StreamLinkMode::Multi), 1);
        store.setI32(kStreamFecRetries, 3, 1);
        store.setI32(kStreamFecRetryPackets, 21, 1);
        setPcStreamNa(store, Reason::PcAppNotRunning, 1);
        if (scenario != 0) {
            for (const auto& d : registry())
                if (isPcStreamMetric(d.id)) {
                    if (d.type == WireType::F32)
                        store.setF32(d.id, 0, 1);
                    else if (d.type == WireType::I32)
                        store.setI32(d.id, 0, 1);
                    else if (d.type == WireType::ShortString)
                        store.setText(d.id, "", 1);
                }
            store.setF32(kPcStreamPing, 2.4, 1);
            store.setF32(kPcStreamFps, 72, 1);
            store.setF32(kPcStreamDisplayHz, 72, 1);
            store.setText(kPcStreamLink0Address, "10.35.78.1", 1);
            store.setText(kPcStreamLink1Address, "192.168.1.47", 1);
            store.setText(kNetDongleBase + kNetAdapterIp, "10.35.78.1", 1);
            store.setText(kNetHomeBase + kNetAdapterIp, "192.168.1.47", 1);
            store.setF32(kPcStreamWireMbps, 151.2, 1);
            store.setF32(kPcStreamVideoMbps, 125.4, 1);
            store.setF32(kPcStreamFecMbps, 17.8, 1);
            store.setF32(kPcStreamAudioMbps, 3.4, 1);
            store.setF32(kPcStreamRetryMbps, 3.2, 1);
            store.setF32(kPcStreamLogicMbps, 1.4, 1);
            const double latency[] = {1.1, 2.6, 0.2, 7.8, 8.3, 37.2, 57.2};
            for (unsigned j = 0; j < 7; ++j) {
                store.setF32(static_cast<uint16_t>(1040 + 2 * j), latency[j], 1);
                store.setF32(static_cast<uint16_t>(1041 + 2 * j), latency[j] + 0.8, 1);
            }
            store.setF32(kPcStreamNamedEncode, 2.7, 1);
            store.setF32(kPcStreamNamedSend, 0.2, 1);
            store.setF32(kPcStreamEstimatedBandwidth, 232, 1);
            store.setF32(kPcStreamLink0Ping, 11.9, 1);
            store.setF32(kPcStreamLink0Share, 67, 1);
            store.setF32(kPcStreamLink1Ping, 45.1, 1);
            store.setF32(kPcStreamLink1Share, 33, 1);
            for (uint16_t id = 1064; id <= 1067; ++id)
                store.setNa(id, Reason::NotConnected, 1);
            store.setText(kPcStreamBuild, "1791581198", 1);
            if (scenario == 2) {
                for (const auto& d : registry())
                    if (d.id >= 1030 && d.id <= 1053)
                        store.setNa(d.id, Reason::SteamVrChanged, 1);
                store.setText(kPcStreamL3Note, "CSV header/stride mismatch", 1);
            }
            if (scenario == 3)
                setPcStreamNa(store, Reason::NoSteamVr, 1);
        }
        for (unsigned compact = 0; compact < 2; ++compact) {  // R3: the full card and the Compact one
            Layout layout;
            layout.columns.resize(1);
            WidgetInstance w;
            w.type = WidgetType::Streaming;
            w.compact = compact != 0;
            layout.columns[0].widgets = {w};
            GridRenderer renderer(fonts);
            renderer.setLayout(layout);
            WidgetContext ctx;
            ctx.store = &store;
            ctx.now = 1;
            renderer.render(ctx);
            const char* names[] = {"streaming-no-pc", "streaming-simulated", "streaming-mismatch",
                                   "streaming-no-steamvr"};
            const std::string stem = std::string(names[scenario]) + (compact ? "-compact" : "");
            const auto path = (std::filesystem::u8path(argv[1]) / (stem + ".png")).u8string();
            if (!renderer.writePng(path))
                return 1;
            pc::PanelView panel(fonts);
            panel.setLayout(layout);
            const auto pcPath = (std::filesystem::u8path(argv[1]) / (stem + "-pc.png")).u8string();
            if (cairo_surface_write_to_png(panel.render(ctx, 1), pcPath.c_str()) != CAIRO_STATUS_SUCCESS)
                return 1;
            std::printf("Wrote simulated preview %s card=%d panel=%d PC=%d\n", path.c_str(),
                        renderer.geometry().slots[0].h, renderer.geometry().height, panel.geometry().height);
        }
    }
    for (unsigned plugged = 0; plugged < 2; ++plugged) {
        MetricStore store;
        store.setF32(kBatteryPct, 74, 1);
        store.setEnum(kBatteryStatus,
                      static_cast<uint32_t>(plugged ? BatteryStatus::Charging : BatteryStatus::Discharging), 1);
        store.setEnum(kBatteryPlugged, plugged, 1);
        store.setF32(kBatteryPowerW, plugged ? 3.45 : -5.12, 1);
        store.setF32(kBatteryVoltageV, 4.1, 1);
        store.setF32(kBatteryCurrentMa, plugged ? 841 : -1249, 1);
        store.setF32(kBatteryChargeNowMah, 2022, 1);
        store.setF32(kBatteryChargeFullMah, 2730, 1);
        store.setF32(kBatteryChargeDesignMah, 2730, 1);
        store.setText(kBatteryHealth, "Good", 1);
        store.setI32(kBatteryCycles, 29, 1);
        store.setText(kBatteryChargerMode, plugged ? "Charging" : "Unplugged", 1);
        store.setI32(plugged ? kBatteryTimeToFullS : kBatteryTimeToEmptyS, 3051, 1);
        store.setF32(kBatterySetCurrentA, 1, 1);
        store.setF32(kBatterySetCurrentMaxA, 3, 1);
        if (plugged) {
            store.setText(kBatteryUsbType, "PD", 1);
            store.setF32(kBatteryPdOfferedV, 9, 1);
            store.setF32(kBatteryPdOfferedA, 3, 1);
            store.setF32(kBatteryPdOfferedW, 27, 1);
            store.setF32(kBatteryInputV, 8.9, 1);
            store.setF32(kBatteryInputA, 1.2, 1);
            store.setF32(kBatteryInputW, 10.68, 1);
            store.setF32(kBatterySagV, 0.1, 1);
        }
        Layout layout;
        layout.columns.resize(1);
        WidgetInstance battery;
        battery.type = WidgetType::Battery;
        layout.columns[0].widgets = {battery};
        WidgetContext ctx;
        ctx.store = &store;
        ctx.now = 1;
        GridRenderer renderer(fonts);
        renderer.setLayout(layout);
        renderer.render(ctx);
        const std::string stem = plugged ? "battery-plugged" : "battery-unplugged";
        const auto path = (std::filesystem::u8path(argv[1]) / (stem + "-headset.png")).u8string();
        if (!renderer.writePng(path))
            return 1;
        pc::PanelView panel(fonts);
        panel.setLayout(layout);
        const auto pcPath = (std::filesystem::u8path(argv[1]) / (stem + "-pc.png")).u8string();
        if (cairo_surface_write_to_png(panel.render(ctx, 1), pcPath.c_str()) != CAIRO_STATUS_SUCCESS)
            return 1;
        std::printf("Simulated %s rows=%d card=%d panel=%d PC=%d\n", stem.c_str(), batteryRowCount(ctx),
                    renderer.geometry().slots[0].h, renderer.geometry().height, panel.geometry().height);
    }
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        MetricStore store;
        Layout layout;
        layout.columns.resize(1);
        WidgetInstance w;
        w.type = scenario < 2 ? WidgetType::General : scenario < 5 ? WidgetType::Network : WidgetType::Cpu;
        layout.columns[0].widgets = {w};
        WidgetContext ctx;
        ctx.store = &store;
        ctx.now = 1;
        ctx.clockFormat = scenario == 1 ? 0 : 24;
        store.setText(kClock, "2026-10-13 00:35", 1);
        store.setI32(kUptimeS, 7200, 1);
        store.setText(kSceneAppName, "Example game", 1);
        store.setF32(kCtrlLeftPct, 49, 1);
        store.setF32(kCtrlRightPct, 44, 1);
        store.setF32(kSelfCpuPct, 1.1, 1);
        store.setF32(kSelfRssMib, 64, 1);
        store.setF32(kSelfPowerW, 0.05, 1);
        store.setEnum(kNetDongleBase + kNetAdapterPresent, scenario == 3 ? 0 : 1, 1);
        store.setEnum(kNetDongleBase + kNetAdapterConnected, scenario == 2 ? 1 : 0, 1);
        store.setF32(kNetDongleBase + kNetAdapterRxMbps, 151.2, 1);
        store.setF32(kNetDongleBase + kNetAdapterTxMbps, 2.1, 1);
        store.setF32(kNetDongleBase + kNetAdapterRxLinkMbps, 2400, 1);
        store.setF32(kNetDongleBase + kNetAdapterTxLinkMbps, 2400, 1);
        store.setF32(kNetDongleBase + kNetAdapterSignalDbm, -40, 1);
        store.setText(kNetDongleBase + kNetAdapterIp, "10.35.78.1", 1);
        if (scenario >= 5) {
            store.setF32(kCpuUsagePct, 45, 1);
            store.setF32(kCpuMaxCorePct, 70, 1);
            for (uint16_t c = 0; c < kCpuCoreSlots; ++c)
                store.setF32(kCpuCore0Pct + c, 30 + c * 5, 1);
            for (uint16_t c = 0; c < kCpuClusterSlots; ++c)
                store.setNa(kCpuCluster0Mhz + c, Reason::SensorMissing, 1);
            if (scenario >= 6) {
                store.setF32(kCpuCluster0Mhz, 1800, 1);
                store.setF32(kCpuCluster0MaxMhz, 2035, 1);
                store.setF32(kCpuCluster0HwMaxMhz, 2265, 1);
            }
            if (scenario == 7) {
                store.setF32(kCpuCluster0Mhz + 2, 2035, 1);
                store.setF32(kCpuCluster0MaxMhz + 2, 2035, 1);
                store.setF32(kCpuCluster0HwMaxMhz + 2, 2956, 1);
            }
        }
        GridRenderer renderer(fonts);
        renderer.setLayout(layout);
        renderer.render(ctx);
        const char* names[] = {"general-clock-on", "general-clock-off", "network-connected", "network-absent",
                               "network-disconnected", "cpu-no-clocks", "cpu-one-clock-row", "cpu-two-clock-rows"};
        const auto path = (std::filesystem::u8path(argv[1]) / (std::string(names[scenario]) + ".png")).u8string();
        if (!renderer.writePng(path))
            return 1;
        std::printf("Simulated %s card=%d panel=%d\n", names[scenario], renderer.geometry().slots[0].h,
                    renderer.geometry().height);
    }
    if (argc == 3) {
        MetricStore store;
        pc::streaming::Service service;
        service.setDemand(true);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 8) {
            const double now =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            service.take(store, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        service.stop();
        Layout layout;
        layout.columns.resize(1);
        WidgetInstance streaming;
        streaming.type = WidgetType::Streaming;
        layout.columns[0].widgets = {streaming};
        WidgetContext ctx;
        ctx.store = &store;
        ctx.now = 1;
        GridRenderer renderer(fonts);
        renderer.setLayout(layout);
        renderer.render(ctx);
        const auto path = (std::filesystem::u8path(argv[1]) / "streaming-live-pc-reader.png").u8string();
        if (!renderer.writePng(path))
            return 1;
        std::printf("Live production PC reader; headset-owned mode/FEC/adapter IPs unavailable in this probe\n");
        std::printf("%s", store.dump(ctx.now).c_str());
    }
    return 0;
}
