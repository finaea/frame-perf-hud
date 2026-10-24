// The metric registry: one table that says, for every metric, which widget owns it and which source
// reads it.
//
// Rules (enforced by validateRegistry(), run by the unit tests):
// - every id is unique, and ids are stable forever: new metrics get new ids, retired ids are never reused,
// - every metric has exactly one owner widget (no owner, or the same metric under two owners, fails),
// - every metric names a source and a read interval.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frame_perf_overlay {

/**
 * Widget types. Stable: they go into saved layouts. Numbers without a widget yet (7, 8, 10, 12, 13) are
 * reserved, never reused.
 */
enum class WidgetType : uint8_t {
    None = 0,
    Frame = 1,
    Cpu = 2,
    GpuRam = 3,
    Power = 4,
    Battery = 5,
    Temp = 6,
    Network = 9,
    General = 11,
};

/** Number of WidgetType values including None and the reserved gaps (for arrays indexed by type). */
constexpr size_t kWidgetTypeCount = 12;

/**
 * A source is one thing that gets read (a sysfs file group, a /proc file, an nl80211 query, an OpenVR call).
 * Internal: source ids aren't saved anywhere, so they may be renumbered.
 */
enum class SourceId : uint8_t {
    None = 0,
    CpuLoad,      ///< /proc/stat
    CpuClock,     ///< cpufreq scaling_cur_freq
    GpuClock,     ///< devfreq cur_freq
    Ram,          ///< /proc/meminfo
    Temps,        ///< thermal_zone*/temp
    BoardTemps,   ///< vadc iio board temperatures
    Throttle,     ///< cooling_device*/cur_state
    Fan,          ///< hwmon fan1_input
    Power,        ///< max34417 power*_input
    Battery,      ///< power_supply battery + charger
    Network,      ///< nl80211 + interface statistics
    FrameTiming,  ///< OpenVR GetFrameTimings, batched
    Controllers,  ///< OpenVR controller battery properties
    Clock,        ///< local time
    Count,
};

/** Number of SourceId values including None and excluding Count. */
constexpr size_t kSourceCount = static_cast<size_t>(SourceId::Count);

/** How a value is typed. */
enum class WireType : uint8_t {
    F32 = 0,
    I32 = 1,
    Enum = 2,         ///< a small unsigned code, meaning documented next to the metric
    ShortString = 3,  ///< up to kShortStringMax bytes of UTF-8
};

/**
 * The interval of the metrics read on every panel update, like the fixed panel did (frame timing, CPU, GPU clock,
 * memory, temperatures, throttle, fan). It's update_interval_ms's default: the scheduler moves these sources to the
 * configured interval (Scheduler::setUpdateInterval). Sources with any other interval keep it.
 */
constexpr uint32_t kPerUpdateMs = 500;

/** Longest short string a metric may carry (bytes). */
constexpr size_t kShortStringMax = 31;

/** One registry entry. */
struct MetricDef {
    uint16_t id;          ///< stable id
    const char* name;     ///< dotted name, unique ("cpu.usage_pct")
    const char* unit;     ///< display unit ("%", "W", "" for none)
    WidgetType owner;     ///< the one widget that may show it
    SourceId source;      ///< what reads it
    uint32_t intervalMs;  ///< how often the source reads it while needed
    WireType type;
};

// Metric ids. Grouped in blocks of 100 by owner widget, only for readability.
namespace metric {
// Frame (widget 1), read from FrameTiming
constexpr uint16_t kFrameAppFps = 100;
constexpr uint16_t kFrameDisplayHz = 101;
constexpr uint16_t kFrameGpuMs = 102;
constexpr uint16_t kFrameCpuMs = 103;
constexpr uint16_t kFrameGpuMaxMs = 104;
constexpr uint16_t kFrameCpuMaxMs = 105;
constexpr uint16_t kFrameReprojectedPct = 106;
constexpr uint16_t kFrameDropped = 107;
constexpr uint16_t kFrameThrottled = 108;  ///< 0 = not throttled, 1 = half rate, 2 = third
constexpr uint16_t kFrameCount = 109;      ///< frames aggregated since the last read
// CPU (widget 2)
constexpr uint16_t kCpuUsagePct = 200;
constexpr uint16_t kCpuMaxCorePct = 201;
constexpr uint16_t kCpuFastestClockMhz = 202;
// 203 is retired (the big core's clock; the CPU card shows the fastest cluster). Never reuse it.
// GPU & RAM (widget 3)
constexpr uint16_t kGpuBusyPct = 300;  ///< GPU load (frame GPU ms × app fps), see core/gpu_load.h
constexpr uint16_t kGpuClockMhz = 301;
constexpr uint16_t kGpuMaxClockMhz = 302;
constexpr uint16_t kRamUsedGib = 303;
constexpr uint16_t kRamTotalGib = 304;
// Power (widget 4)
constexpr uint16_t kPowerVphW = 400;
// 401 is retired (the "All ch" sum, which double-counts vph). Never reuse it.
constexpr uint16_t kPowerRailsW = 402;  ///< sum of the measured rails under vph (every channel but vph)
// Battery (widget 5)
constexpr uint16_t kBatteryPct = 500;
constexpr uint16_t kBatteryVoltageV = 501;
constexpr uint16_t kBatteryCurrentMa = 502;
constexpr uint16_t kBatteryStatus = 503;         ///< enum, see BatteryStatus
constexpr uint16_t kBatteryChargerOnline = 504;  ///< enum, 0 / 1
constexpr uint16_t kBatteryPowerW = 508;         ///< V × A of the battery, + = into it, − = out of it
// Temp (widget 6)
constexpr uint16_t kTempCpuC = 600;
constexpr uint16_t kTempGpuC = 601;
constexpr uint16_t kTempDdrC = 602;
constexpr uint16_t kTempBatteryC = 603;  ///< read by the Battery source, owned by Temp
constexpr uint16_t kTempDisplayC = 604;
constexpr uint16_t kTempExhaustC = 605;
constexpr uint16_t kTempHeatsinkC = 606;
constexpr uint16_t kThrottleCpu = 607;   ///< enum, 0 / 1
constexpr uint16_t kThrottleGpu = 608;   ///< enum, 0 / 1
constexpr uint16_t kFanRpm = 609;
// Network (widget 9): the wireless link (the dongle if it has a peer, else home Wi-Fi)
constexpr uint16_t kNetInterfaceUp = 900;  ///< enum, 0 / 1
constexpr uint16_t kNetConnected = 901;    ///< enum, 0 / 1
constexpr uint16_t kNetHomeWifi = 902;     ///< enum, 0 = dongle, 1 = home Wi-Fi
constexpr uint16_t kNetSignalDbm = 903;
constexpr uint16_t kNetTxLinkMbps = 904;
constexpr uint16_t kNetRxLinkMbps = 905;
constexpr uint16_t kNetRxMbps = 906;
constexpr uint16_t kNetTxMbps = 907;
// General (widget 11)
constexpr uint16_t kCtrlLeftPct = 1100;       ///< N/A NotConnected when the controller is absent
constexpr uint16_t kCtrlLeftCharging = 1101;  ///< enum, 0 / 1
constexpr uint16_t kCtrlRightPct = 1102;
constexpr uint16_t kCtrlRightCharging = 1103;
constexpr uint16_t kClock = 1111;  ///< short string, local "YYYY-MM-DD HH:MM:SS"
}  // namespace metric

/** Values of metric::kBatteryStatus. */
enum class BatteryStatus : uint32_t { Unknown = 0, Charging = 1, Discharging = 2, Full = 3, NotCharging = 4 };

/**
 * The registry compiled into this build.
 * @return all entries (static storage)
 */
const std::vector<MetricDef>& registry();

/**
 * Looks a metric up by id.
 * @param id metric id
 * @return the entry, or nullptr if this build doesn't know the id
 */
const MetricDef* findMetric(uint16_t id);

/**
 * Checks a registry table against the rules at the top of this file.
 * @param defs the table
 * @return one English line per problem; empty if the table is valid
 */
std::vector<std::string> validateRegistry(const std::vector<MetricDef>& defs);

/**
 * @param type a widget type
 * @return its name for logs and the debug dump ("Frame", "GPU & RAM", ...)
 */
const char* widgetName(WidgetType type);

/**
 * @param source a source id
 * @return its name for logs and the debug dump ("cpu_load", ...)
 */
const char* sourceName(SourceId source);

}  // namespace frame_perf_overlay
