#include "metric_registry.h"

#include <map>
#include <set>

namespace frame_perf_overlay {

namespace {

using W = WidgetType;
using S = SourceId;
using T = WireType;

// Read every panel update (update_interval_ms, see kPerUpdateMs): frame timing, CPU, GPU clock, memory, temperatures,
// throttle, fan. The rest keep the fixed panel's slower intervals
constexpr uint32_t kTick = kPerUpdateMs;
constexpr uint32_t kCpuMs = kPerUpdateMs;      // /proc/stat, cpufreq
constexpr uint32_t kGpuMs = kPerUpdateMs;      // devfreq clock
constexpr uint32_t kRamMs = kPerUpdateMs;
constexpr uint32_t kThermalMs = kPerUpdateMs;  // thermal zones, throttle, fan
constexpr uint32_t kBoardMs = 5000;    // vadc board temps (an ADC conversion takes a few ms each)
constexpr uint32_t kPowerMs = 2000;    // max34417 updates every 1.5-2 s
constexpr uint32_t kChargeMs = 5000;   // battery, charger
constexpr uint32_t kClockMs = 1000;

const std::vector<MetricDef> kRegistry = {
    // Frame
    {metric::kFrameAppFps, "frame.app_fps", "fps", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameDisplayHz, "frame.display_hz", "Hz", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameGpuMs, "frame.gpu_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameCpuMs, "frame.cpu_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameGpuMaxMs, "frame.gpu_max_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameCpuMaxMs, "frame.cpu_max_ms", "ms", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameReprojectedPct, "frame.reprojected_pct", "%", W::Frame, S::FrameTiming, kTick, T::F32},
    {metric::kFrameDropped, "frame.dropped", "", W::Frame, S::FrameTiming, kTick, T::I32},
    {metric::kFrameThrottled, "frame.throttled", "", W::Frame, S::FrameTiming, kTick, T::Enum},
    {metric::kFrameCount, "frame.frames", "", W::Frame, S::FrameTiming, kTick, T::I32},
    // CPU
    {metric::kCpuUsagePct, "cpu.usage_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuMaxCorePct, "cpu.max_core_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuFastestClockMhz, "cpu.fastest_clock_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // GPU & RAM
    // GPU load = frame GPU ms × app fps, written by the FrameTiming reader (core/gpu_load.h). Its source is
    // FrameTiming, so a GPU & RAM widget alone keeps frame timing read
    {metric::kGpuBusyPct, "gpu.busy_pct", "%", W::GpuRam, S::FrameTiming, kTick, T::F32},
    {metric::kGpuClockMhz, "gpu.clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kGpuMaxClockMhz, "gpu.max_clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kRamUsedGib, "ram.used_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    {metric::kRamTotalGib, "ram.total_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    // Power. 401 (power.all_channels_w) is retired
    {metric::kPowerVphW, "power.vph_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerRailsW, "power.rails_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    // Battery
    {metric::kBatteryPct, "battery.pct", "%", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryVoltageV, "battery.voltage_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryCurrentMa, "battery.current_ma", "mA", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryStatus, "battery.status", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryChargerOnline, "battery.charger_online", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryPowerW, "battery.power_w", "W", W::Battery, S::Battery, kChargeMs, T::F32},
    // Temp
    {metric::kTempCpuC, "temp.cpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempGpuC, "temp.gpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempDdrC, "temp.ddr_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempBatteryC, "temp.battery_c", "°C", W::Temp, S::Battery, kChargeMs, T::F32},
    {metric::kTempDisplayC, "temp.display_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kTempExhaustC, "temp.exhaust_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kTempHeatsinkC, "temp.heatsink_c", "°C", W::Temp, S::BoardTemps, kBoardMs, T::F32},
    {metric::kThrottleCpu, "temp.throttle_cpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kThrottleGpu, "temp.throttle_gpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kFanRpm, "temp.fan_rpm", "rpm", W::Temp, S::Fan, kThermalMs, T::F32},
    // Network: nl80211 and the byte counters every 2 s (like the fixed panel)
    {metric::kNetInterfaceUp, "net.interface_up", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetConnected, "net.connected", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeWifi, "net.home_wifi", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetSignalDbm, "net.signal_dbm", "dBm", W::Network, S::Network, 2000, T::F32},
    {metric::kNetTxLinkMbps, "net.tx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetRxLinkMbps, "net.rx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetRxMbps, "net.rx_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetTxMbps, "net.tx_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    // General
    {metric::kCtrlLeftPct, "general.controller_left_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlLeftCharging, "general.controller_left_charging", "", W::General, S::Controllers, 10000, T::Enum},
    {metric::kCtrlRightPct, "general.controller_right_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlRightCharging, "general.controller_right_charging", "", W::General, S::Controllers, 10000,
     T::Enum},
    {metric::kClock, "general.clock", "", W::General, S::Clock, kClockMs, T::ShortString},
};

}  // namespace

const std::vector<MetricDef>& registry() {
    return kRegistry;
}

const MetricDef* findMetric(uint16_t id) {
    // Built once: id -> index. The registry is small, but this is called per value set.
    static const std::map<uint16_t, size_t> index = [] {
        std::map<uint16_t, size_t> m;
        for (size_t i = 0; i < kRegistry.size(); ++i) m.emplace(kRegistry[i].id, i);
        return m;
    }();
    const auto it = index.find(id);
    return it == index.end() ? nullptr : &kRegistry[it->second];
}

std::vector<std::string> validateRegistry(const std::vector<MetricDef>& defs) {
    std::vector<std::string> problems;
    std::set<uint16_t> ids;
    std::map<std::string, WidgetType> owners;  // metric name -> first owner seen
    for (const MetricDef& d : defs) {
        const std::string name = d.name != nullptr ? d.name : "";
        const std::string label = "metric " + std::to_string(d.id) + " (" + name + ")";
        if (!ids.insert(d.id).second) problems.push_back(label + ": id is used more than once");
        if (name.empty()) problems.push_back(label + ": has no name");
        if (d.owner == WidgetType::None || static_cast<size_t>(d.owner) >= kWidgetTypeCount) {
            problems.push_back(label + ": has no owner widget");
        }
        if (!name.empty()) {
            const auto it = owners.find(name);
            if (it == owners.end()) {
                owners.emplace(name, d.owner);
            } else if (it->second != d.owner) {
                problems.push_back(label + ": has two owners (" + widgetName(it->second) + " and " +
                                   widgetName(d.owner) + ")");
            } else {
                problems.push_back(label + ": name is registered more than once");
            }
        }
        if (d.source == SourceId::None || static_cast<size_t>(d.source) >= kSourceCount) {
            problems.push_back(label + ": has no source");
        }
        if (d.intervalMs == 0) problems.push_back(label + ": has no read interval");
        if (d.unit == nullptr) problems.push_back(label + ": unit is null (use \"\" for none)");
    }
    return problems;
}

const char* widgetName(WidgetType type) {
    switch (type) {
        case WidgetType::None: return "none";
        case WidgetType::Frame: return "Frame";
        case WidgetType::Cpu: return "CPU";
        case WidgetType::GpuRam: return "GPU & RAM";
        case WidgetType::Power: return "Power";
        case WidgetType::Battery: return "Battery";
        case WidgetType::Temp: return "Temp";
        case WidgetType::Network: return "Network";
        case WidgetType::General: return "General";
    }
    return "unknown";
}

const char* sourceName(SourceId source) {
    switch (source) {
        case SourceId::None: return "none";
        case SourceId::CpuLoad: return "cpu_load";
        case SourceId::CpuClock: return "cpu_clock";
        case SourceId::GpuClock: return "gpu_clock";
        case SourceId::Ram: return "ram";
        case SourceId::Temps: return "temps";
        case SourceId::BoardTemps: return "board_temps";
        case SourceId::Throttle: return "throttle";
        case SourceId::Fan: return "fan";
        case SourceId::Power: return "power";
        case SourceId::Battery: return "battery";
        case SourceId::Network: return "network";
        case SourceId::FrameTiming: return "frame_timing";
        case SourceId::Controllers: return "controllers";
        case SourceId::Clock: return "clock";
        case SourceId::Count: break;
    }
    return "unknown";
}

}  // namespace frame_perf_overlay
