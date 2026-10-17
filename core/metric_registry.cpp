#include "metric_registry.h"
#include "stream_metrics.h"

#include <map>
#include <set>

namespace fph {

namespace {

using W = WidgetType;
using S = SourceId;
using T = WireType;

constexpr uint32_t kTick = 500;  // the 2 Hz metrics deadline

// T02: system sensor intervals, from the tech plan's source table (headset agent › sources)
constexpr uint32_t kCpuMs = 500;      // /proc/stat, cpufreq
constexpr uint32_t kGpuMs = 1000;     // devfreq clock, DRM fdinfo busy
constexpr uint32_t kRamMs = 2000;
constexpr uint32_t kThermalMs = 2000;  // thermal zones, vadc board temps, throttle, fan
constexpr uint32_t kPowerMs = 2000;    // max34417 updates every 1.5-2 s
constexpr uint32_t kChargeMs = 2000;   // battery, charger, USB-PD
constexpr uint32_t kUptimeMs = 1000;
constexpr uint32_t kSelfMs = 2000;
constexpr uint32_t kProcessMs = 2000;  // T04: was 1000 (budget)

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
    {metric::kCpuBigCoreClockMhz, "cpu.big_core_clock_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 0, "cpu.core0_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 1, "cpu.core1_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 2, "cpu.core2_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 3, "cpu.core3_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 4, "cpu.core4_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 5, "cpu.core5_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 6, "cpu.core6_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCore0Pct + 7, "cpu.core7_pct", "%", W::Cpu, S::CpuLoad, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 0, "cpu.cluster0_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 1, "cpu.cluster1_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 2, "cpu.cluster2_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0Mhz + 3, "cpu.cluster3_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 0, "cpu.cluster0_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 1, "cpu.cluster1_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 2, "cpu.cluster2_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0MaxMhz + 3, "cpu.cluster3_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // T04: hardware max per cluster (read once at start; "capped" when scaling_max_freq is below it)
    {metric::kCpuCluster0HwMaxMhz + 0, "cpu.cluster0_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 1, "cpu.cluster1_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 2, "cpu.cluster2_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    {metric::kCpuCluster0HwMaxMhz + 3, "cpu.cluster3_hw_max_mhz", "MHz", W::Cpu, S::CpuClock, kCpuMs, T::F32},
    // GPU & RAM
    {metric::kGpuBusyPct, "gpu.busy_pct", "%", W::GpuRam, S::GpuBusy, kGpuMs, T::F32},
    {metric::kGpuClockMhz, "gpu.clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kGpuMaxClockMhz, "gpu.max_clock_mhz", "MHz", W::GpuRam, S::GpuClock, kGpuMs, T::F32},
    {metric::kRamUsedGib, "ram.used_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    {metric::kRamTotalGib, "ram.total_gib", "GiB", W::GpuRam, S::Ram, kRamMs, T::F32},
    // Power. 401 (power.all_channels_w) is retired
    {metric::kPowerVphW, "power.vph_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerRailsW, "power.rails_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerUnmeasuredW, "power.unmeasured_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc0W, "power.apc0_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc1W, "power.apc1_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc2W, "power.apc2_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerGfxW, "power.gfx_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerNsp1W, "power.nsp1_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerNsp2W, "power.nsp2_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS1cW, "power.s1c_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS3cW, "power.s3c_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS6cW, "power.s6c_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerBobW, "power.bob_w", "W", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerVphV, "power.vph_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc0W + metric::kPowerRailVoltsOffset, "power.apc0_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc1W + metric::kPowerRailVoltsOffset, "power.apc1_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerApc2W + metric::kPowerRailVoltsOffset, "power.apc2_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerGfxW + metric::kPowerRailVoltsOffset, "power.gfx_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerNsp1W + metric::kPowerRailVoltsOffset, "power.nsp1_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerNsp2W + metric::kPowerRailVoltsOffset, "power.nsp2_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS1cW + metric::kPowerRailVoltsOffset, "power.s1c_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS3cW + metric::kPowerRailVoltsOffset, "power.s3c_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerS6cW + metric::kPowerRailVoltsOffset, "power.s6c_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    {metric::kPowerBobW + metric::kPowerRailVoltsOffset, "power.bob_v", "V", W::Power, S::Power, kPowerMs, T::F32},
    // Battery
    {metric::kBatteryPct, "battery.pct", "%", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryVoltageV, "battery.voltage_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryCurrentMa, "battery.current_ma", "mA", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryStatus, "battery.status", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryChargerOnline, "battery.charger_online", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryChargeNowMah, "battery.charge_now_mah", "mAh", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryChargeFullMah, "battery.charge_full_mah", "mAh", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryChargeDesignMah, "battery.charge_design_mah", "mAh", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryPowerW, "battery.power_w", "W", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryTimeToFullS, "battery.time_to_full_s", "s", W::Battery, S::Battery, kChargeMs, T::I32},
    {metric::kBatteryTimeToEmptyS, "battery.time_to_empty_s", "s", W::Battery, S::Battery, kChargeMs, T::I32},
    {metric::kBatteryHealth, "battery.health", "", W::Battery, S::Battery, kChargeMs, T::ShortString},
    {metric::kBatteryCycles, "battery.cycles", "", W::Battery, S::Battery, kChargeMs, T::I32},
    {metric::kBatteryChargerMode, "battery.charger_mode", "", W::Battery, S::Battery, kChargeMs, T::ShortString},
    {metric::kBatterySetCurrentA, "battery.set_current_a", "A", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatterySetCurrentMaxA, "battery.set_current_max_a", "A", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryUsbType, "battery.usb_type", "", W::Battery, S::Battery, kChargeMs, T::ShortString},
    {metric::kBatteryPlugged, "battery.plugged", "", W::Battery, S::Battery, kChargeMs, T::Enum},
    {metric::kBatteryPdOfferedV, "battery.pd_offered_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryPdOfferedA, "battery.pd_offered_a", "A", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryPdOfferedW, "battery.pd_offered_w", "W", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryInputV, "battery.input_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryInputA, "battery.input_a", "A", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatteryInputW, "battery.input_w", "W", W::Battery, S::Battery, kChargeMs, T::F32},
    {metric::kBatterySagV, "battery.sag_v", "V", W::Battery, S::Battery, kChargeMs, T::F32},
    // Temp
    {metric::kTempCpuC, "temp.cpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempGpuC, "temp.gpu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempDdrC, "temp.ddr_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempBatteryC, "temp.battery_c", "°C", W::Temp, S::Battery, kChargeMs, T::F32},
    {metric::kTempDisplayC, "temp.display_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kTempExhaustC, "temp.exhaust_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kTempHeatsinkC, "temp.heatsink_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kThrottleCpu, "temp.throttle_cpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kThrottleGpu, "temp.throttle_gpu", "", W::Temp, S::Throttle, kThermalMs, T::Enum},
    {metric::kFanRpm, "temp.fan_rpm", "rpm", W::Temp, S::Fan, kThermalMs, T::F32},
    {metric::kTempNpuC, "temp.npu_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempCameraC, "temp.camera_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempVideoC, "temp.video_c", "°C", W::Temp, S::Temps, kThermalMs, T::F32},
    {metric::kTempWifiC, "temp.wifi_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kTempFlashC, "temp.flash_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kTempNrfC, "temp.nrf_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    {metric::kTempConnectorC, "temp.connector_c", "°C", W::Temp, S::BoardTemps, kThermalMs, T::F32},
    // Network
    {metric::kNetInterfaceUp, "net.interface_up", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetConnected, "net.connected", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeWifi, "net.home_wifi", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetSignalDbm, "net.signal_dbm", "dBm", W::Network, S::Network, 2000, T::F32},
    {metric::kNetTxLinkMbps, "net.tx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetRxLinkMbps, "net.rx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    // T03: byte counters every 1 s, the radio (nl80211, IP) every 2 s inside the same read
    {metric::kNetRxMbps, "net.rx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    {metric::kNetTxMbps, "net.tx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    // T03: Network per adapter (one block each, see kNetAdapter* offsets)
    {metric::kNetDongleBase + metric::kNetAdapterPresent, "net.dongle.present", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetDongleBase + metric::kNetAdapterConnected, "net.dongle.connected", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetDongleBase + metric::kNetAdapterRxMbps, "net.dongle.rx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    {metric::kNetDongleBase + metric::kNetAdapterTxMbps, "net.dongle.tx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    {metric::kNetDongleBase + metric::kNetAdapterTxLinkMbps, "net.dongle.tx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetDongleBase + metric::kNetAdapterRxLinkMbps, "net.dongle.rx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetDongleBase + metric::kNetAdapterSignalDbm, "net.dongle.signal_dbm", "dBm", W::Network, S::Network, 2000, T::F32},
    {metric::kNetDongleBase + metric::kNetAdapterIp, "net.dongle.ip", "", W::Network, S::Network, 2000, T::ShortString},
    {metric::kNetDongleBase + metric::kNetAdapterFreqMhz, "net.dongle.freq_mhz", "MHz", W::Network, S::Network, 2000, T::I32},
    {metric::kNetDongleBase + metric::kNetAdapterChannel, "net.dongle.channel", "", W::Network, S::Network, 2000, T::I32},
    {metric::kNetDongleBase + metric::kNetAdapterWidthMhz, "net.dongle.width_mhz", "MHz", W::Network, S::Network, 2000, T::I32},
    {metric::kNetDongleBase + metric::kNetAdapterBand, "net.dongle.band", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetDongleBase + metric::kNetAdapterIfName, "net.dongle.ifname", "", W::Network, S::Network, 2000, T::ShortString},
    {metric::kNetHomeBase + metric::kNetAdapterPresent, "net.home.present", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeBase + metric::kNetAdapterConnected, "net.home.connected", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeBase + metric::kNetAdapterRxMbps, "net.home.rx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    {metric::kNetHomeBase + metric::kNetAdapterTxMbps, "net.home.tx_mbps", "Mbps", W::Network, S::Network, 1000, T::F32},
    {metric::kNetHomeBase + metric::kNetAdapterTxLinkMbps, "net.home.tx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetHomeBase + metric::kNetAdapterRxLinkMbps, "net.home.rx_link_mbps", "Mbps", W::Network, S::Network, 2000, T::F32},
    {metric::kNetHomeBase + metric::kNetAdapterSignalDbm, "net.home.signal_dbm", "dBm", W::Network, S::Network, 2000, T::F32},
    {metric::kNetHomeBase + metric::kNetAdapterIp, "net.home.ip", "", W::Network, S::Network, 2000, T::ShortString},
    {metric::kNetHomeBase + metric::kNetAdapterFreqMhz, "net.home.freq_mhz", "MHz", W::Network, S::Network, 2000, T::I32},
    {metric::kNetHomeBase + metric::kNetAdapterChannel, "net.home.channel", "", W::Network, S::Network, 2000, T::I32},
    {metric::kNetHomeBase + metric::kNetAdapterWidthMhz, "net.home.width_mhz", "MHz", W::Network, S::Network, 2000, T::I32},
    {metric::kNetHomeBase + metric::kNetAdapterBand, "net.home.band", "", W::Network, S::Network, 2000, T::Enum},
    {metric::kNetHomeBase + metric::kNetAdapterIfName, "net.home.ifname", "", W::Network, S::Network, 2000, T::ShortString},
    // T03: CPU / RAM usage items (one /proc scan feeds both). T04: every 2 s, not 1 s: the scan costs 5.7-8.1 ms,
    // and the core HUD's 2%-of-a-core budget is tight with both lists on
    {metric::kProcScanned, "cpu_items.scanned", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kProcVanished, "cpu_items.vanished", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kProcScanMs, "cpu_items.scan_ms", "ms", W::CpuItems, S::Processes, kProcessMs, T::F32},
    {metric::kCpuItemsRows, "cpu_items.rows", "", W::CpuItems, S::Processes, kProcessMs, T::I32},
    {metric::kRamItemsRows, "ram_items.rows", "", W::RamItems, S::Processes, kProcessMs, T::I32},
    // PC streaming values arrive at 1 Hz, independent of the headset sensors.
#define FPH_STREAM_DEF(n,id,name,unit,type) {id, "streaming.pc." name, unit, W::Streaming, S::PcStreaming, 1000, T::type},
    FPH_PC_STREAM_METRICS(FPH_STREAM_DEF)
#undef FPH_STREAM_DEF
    // T03: Streaming, headset side
    {metric::kStreamLinkMode, "streaming.link_mode", "", W::Streaming, S::StreamLink, 1000, T::Enum},
    {metric::kStreamVrlinkRunning, "streaming.vrlink_running", "", W::Streaming, S::StreamLink, 1000, T::Enum},
    {metric::kStreamDongleSharePct, "streaming.dongle_share_pct", "%", W::Streaming, S::StreamLink, 1000, T::F32},
    {metric::kStreamFecRetries, "streaming.fec_retries", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamFecRetryPackets, "streaming.fec_retry_packets", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamFramesDropped, "streaming.frames_dropped", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamLateMetadata, "streaming.late_metadata", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamAborts, "streaming.stream_aborts", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamUnrecoverable, "streaming.unrecoverable", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    {metric::kStreamLogOmitted, "streaming.log_omitted", "", W::Streaming, S::StreamEvents, 1000, T::I32},
    // General
    {metric::kCtrlLeftPct, "general.controller_left_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlLeftCharging, "general.controller_left_charging", "", W::General, S::Controllers, 10000, T::Enum},
    {metric::kCtrlRightPct, "general.controller_right_pct", "%", W::General, S::Controllers, 10000, T::F32},
    {metric::kCtrlRightCharging, "general.controller_right_charging", "", W::General, S::Controllers, 10000,
     T::Enum},
    // T03: current game / session. The scheduler re-queries every 5 s as a safety net; connect and
    // VREvent_SceneApplicationChanged update them at once, whatever the demand
    {metric::kSceneAppKey, "general.app_key", "", W::General, S::SceneApp, 5000, T::ShortString},
    {metric::kSceneAppName, "general.app_name", "", W::General, S::SceneApp, 5000, T::ShortString},
    {metric::kScenePid, "general.scene_pid", "", W::General, S::SceneApp, 5000, T::I32},
    {metric::kSessionId, "general.session_id", "", W::General, S::SceneApp, 5000, T::I32},
    {metric::kSessionStartUnix, "general.session_start_unix", "s", W::General, S::SceneApp, 5000, T::I32},
    {metric::kStreamAppHint, "general.stream_app_hint", "", W::General, S::SceneApp, 5000, T::ShortString},
    // T02: uptime, clock, own cost
    {metric::kUptimeS, "general.uptime_s", "s", W::General, S::Uptime, kUptimeMs, T::I32},
    {metric::kClock, "general.clock", "", W::General, S::Uptime, kUptimeMs, T::ShortString},
    {metric::kSelfCpuPct, "general.self_cpu_pct", "%", W::General, S::SelfCost, kSelfMs, T::F32},
    {metric::kSelfRssMib, "general.self_rss_mib", "MiB", W::General, S::SelfCost, kSelfMs, T::F32},
    {metric::kSelfPowerW, "general.self_power_est_w", "W", W::General, S::SelfCost, kSelfMs, T::F32},
    // T11: set once at start by the session logger (no reader: the interval is only for the dump)
    {metric::kLastPowerOff, "general.last_power_off", "", W::General, S::SessionLog, 60000, T::ShortString},
    {metric::kCrashLogNote, "general.crash_log_note", "", W::General, S::SessionLog, 60000, T::ShortString},
};

using K = LogKind;

// T11: what a session log keeps. Gauges get a histogram (bins span the plausible range; the ends catch the rest).
// Per-read counts are summed, cumulative counters become last − first, flags count their rises
const std::vector<LogColumn> kLogColumns = {
    {metric::kFrameAppFps, K::Gauge, 0, 150, 150, true},
    {metric::kFrameDisplayHz, K::Gauge, 0, 150, 150, false},
    {metric::kFrameGpuMs, K::Gauge, 0, 50, 200, true},
    {metric::kFrameCpuMs, K::Gauge, 0, 50, 200, true},
    {metric::kFrameGpuMaxMs, K::Gauge, 0, 100, 200, false},
    {metric::kFrameCpuMaxMs, K::Gauge, 0, 100, 200, false},
    {metric::kFrameReprojectedPct, K::Gauge, 0, 100, 100, false},
    {metric::kFrameDropped, K::Count, 0, 0, 0, false},
    {metric::kFrameThrottled, K::Flag, 0, 0, 0, false},
    {metric::kCpuUsagePct, K::Gauge, 0, 100, 100, false},
    {metric::kCpuMaxCorePct, K::Gauge, 0, 100, 100, false},
    {metric::kCpuFastestClockMhz, K::Gauge, 0, 4000, 200, false},
    {metric::kGpuBusyPct, K::Gauge, 0, 100, 100, false},
    {metric::kGpuClockMhz, K::Gauge, 0, 1500, 150, false},
    {metric::kRamUsedGib, K::Gauge, 0, 16, 160, false},
    {metric::kPowerVphW, K::Gauge, 0, 30, 300, false},
    {metric::kPowerRailsW, K::Gauge, 0, 30, 300, false},
    {metric::kBatteryPct, K::Gauge, 0, 100, 100, false},
    {metric::kBatteryVoltageV, K::Gauge, 3, 4.5f, 150, false},
    {metric::kBatteryCurrentMa, K::Gauge, -6000, 6000, 240, false},
    {metric::kBatteryPowerW, K::Gauge, -30, 30, 240, false},
    {metric::kTempCpuC, K::Gauge, 0, 120, 240, false},
    {metric::kTempGpuC, K::Gauge, 0, 120, 240, false},
    {metric::kTempDdrC, K::Gauge, 0, 120, 240, false},
    {metric::kTempBatteryC, K::Gauge, 0, 80, 160, false},
    {metric::kTempDisplayC, K::Gauge, 0, 120, 240, false},
    {metric::kTempExhaustC, K::Gauge, 0, 120, 240, false},
    {metric::kTempHeatsinkC, K::Gauge, 0, 120, 240, false},
    {metric::kThrottleCpu, K::Flag, 0, 0, 0, false},
    {metric::kThrottleGpu, K::Flag, 0, 0, 0, false},
    {metric::kFanRpm, K::Gauge, 0, 10000, 100, false},
    {metric::kNetRxMbps, K::Gauge, 0, 1000, 200, false},
    {metric::kNetTxMbps, K::Gauge, 0, 1000, 200, false},
    {metric::kNetSignalDbm, K::Gauge, -100, 0, 100, false},
    {metric::kStreamDongleSharePct, K::Gauge, 0, 100, 100, false},
    {metric::kStreamFecRetries, K::Cumulative, 0, 0, 0, false},
    {metric::kStreamFecRetryPackets, K::Cumulative, 0, 0, 0, false},
    {metric::kStreamFramesDropped, K::Cumulative, 0, 0, 0, false},
    {metric::kStreamLateMetadata, K::Cumulative, 0, 0, 0, false},
    {metric::kStreamAborts, K::Cumulative, 0, 0, 0, false},
    {metric::kStreamUnrecoverable, K::Cumulative, 0, 0, 0, false},
    {metric::kCtrlLeftPct, K::Gauge, 0, 100, 100, false},
    {metric::kCtrlRightPct, K::Gauge, 0, 100, 100, false},
    {metric::kSelfCpuPct, K::Gauge, 0, 100, 200, false},
    {metric::kSelfRssMib, K::Gauge, 0, 1024, 256, false},
    // PC streaming (ticket 08): the latency stages, rates and the PC's counters since its reader started
    {metric::kPcStreamPing, K::Gauge, 0, 100, 200, false},
    {metric::kPcStreamEstimatedBandwidth, K::Gauge, 0, 2000, 200, false},
    {metric::kPcStreamRetransmitRequests, K::Gauge, 0, 1000, 100, false},
    {metric::kPcStreamSkipped, K::Gauge, 0, 100, 100, false},
    {metric::kPcStreamLate, K::Gauge, 0, 100, 100, false},
    {metric::kPcStreamFps, K::Gauge, 0, 150, 150, false},
    {metric::kPcStreamWireMbps, K::Gauge, 0, 1000, 200, false},
    {metric::kPcStreamGameAvg, K::Gauge, 0, 50, 200, true},
    {metric::kPcStreamEncodeAvg, K::Gauge, 0, 50, 200, true},
    {metric::kPcStreamSendAvg, K::Gauge, 0, 50, 200, true},
    {metric::kPcStreamNetworkAvg, K::Gauge, 0, 50, 200, true},
    {metric::kPcStreamDecodeAvg, K::Gauge, 0, 50, 200, true},
    {metric::kPcStreamSlackAvg, K::Gauge, -50, 50, 200, true},
    {metric::kPcStreamTotalAvg, K::Gauge, 0, 150, 300, true},
    {metric::kPcStreamStreamResets, K::Cumulative, 0, 0, 0, false},
    {metric::kPcStreamBadLinks, K::Cumulative, 0, 0, 0, false},
    {metric::kPcStreamEncoderReinits, K::Cumulative, 0, 0, 0, false},
    {metric::kPcStreamThrottleEvents, K::Cumulative, 0, 0, 0, false},
    {metric::kPcStreamDisplayHz, K::Gauge, 0, 150, 150, false},
};

}  // namespace

const std::vector<MetricDef>& registry() {
    return kRegistry;
}

const std::vector<LogColumn>& sessionLogColumns() {
    return kLogColumns;
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
        case WidgetType::CpuItems: return "CPU usage items";
        case WidgetType::RamItems: return "RAM usage items";
        case WidgetType::Network: return "Network";
        case WidgetType::Streaming: return "Streaming";
        case WidgetType::General: return "General";
        case WidgetType::HeadsetCamera: return "Headset camera";
        case WidgetType::EyeCamera: return "Eye camera";
    }
    return "unknown";
}

const char* sourceName(SourceId source) {
    switch (source) {
        case SourceId::None: return "none";
        case SourceId::CpuLoad: return "cpu_load";
        case SourceId::CpuClock: return "cpu_clock";
        case SourceId::GpuClock: return "gpu_clock";
        case SourceId::GpuBusy: return "gpu_busy";
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
        case SourceId::Processes: return "processes";
        case SourceId::SceneApp: return "scene_app";
        case SourceId::StreamLink: return "stream_link";
        case SourceId::StreamEvents: return "stream_events";
        case SourceId::Uptime: return "uptime";
        case SourceId::SelfCost: return "self_cost";
        case SourceId::PcStreaming: return "pc_streaming";
        case SourceId::SessionLog: return "session_log";
        case SourceId::Count: break;
    }
    return "unknown";
}

}  // namespace fph
