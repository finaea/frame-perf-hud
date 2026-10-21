#include "system_metrics.h"

#include <cmath>
#include <ctime>

using frame_perf_overlay::Reason;
using frame_perf_overlay::SourceId;
namespace metric = frame_perf_overlay::metric;

namespace {

uint32_t batteryStatusCode(const std::string& status) {
    using frame_perf_overlay::BatteryStatus;
    if (status == "Charging") return static_cast<uint32_t>(BatteryStatus::Charging);
    if (status == "Discharging") return static_cast<uint32_t>(BatteryStatus::Discharging);
    if (status == "Full") return static_cast<uint32_t>(BatteryStatus::Full);
    if (status == "Not charging") return static_cast<uint32_t>(BatteryStatus::NotCharging);
    return static_cast<uint32_t>(BatteryStatus::Unknown);
}

void storePower(frame_perf_overlay::MetricStore& st, const SensorSample& s, double now) {
    bool haveVph = false;
    for (const PowerChannel& ch : s.powerChannels) haveVph = haveVph || ch.label == "vph";
    st.setF32(metric::kPowerVphW, s.powerMainW, now, haveVph ? Reason::ReadFailed : Reason::SensorMissing);
    // Never vph + rails: the rails are fed from vph, so that sum double-counts
    const bool haveRails = s.powerChannels.size() > (haveVph ? 1u : 0u);
    st.setF32(metric::kPowerRailsW, s.powerRailsW, now, haveRails ? Reason::ReadFailed : Reason::SensorMissing);
}

void storeBattery(frame_perf_overlay::MetricStore& st, const SensorSample& s, double now) {
    const Reason bat = s.haveBattery ? Reason::ReadFailed : Reason::SensorMissing;
    st.setF32(metric::kBatteryPct, s.batteryPct, now, bat);
    st.setF32(metric::kBatteryVoltageV, s.batteryVoltageV, now, bat);
    st.setF32(metric::kBatteryCurrentMa, s.batteryCurrentMa, now, bat);
    st.setF32(metric::kTempBatteryC, s.batteryTempC, now, bat);
    if (s.haveBattery) {
        st.setEnum(metric::kBatteryStatus, batteryStatusCode(s.batteryStatus), now);
    } else {
        st.setNa(metric::kBatteryStatus, Reason::SensorMissing, now);
    }
    if (s.haveCharger) {
        st.setEnum(metric::kBatteryChargerOnline, s.chargerOnline ? 1 : 0, now);
    } else {
        st.setNa(metric::kBatteryChargerOnline, Reason::SensorMissing, now);
    }
    // Driver sign: positive = into the battery (checked while charging)
    st.setF32(metric::kBatteryPowerW, s.batteryVoltageV * s.batteryCurrentMa / 1000.0, now, bat);
}

void storeWifi(frame_perf_overlay::MetricStore& st, const WifiInfo& w, bool resumed, double now) {
    st.setEnum(metric::kNetInterfaceUp, w.interfaceUp ? 1 : 0, now);
    st.setEnum(metric::kNetConnected, w.connected ? 1 : 0, now);
    st.setEnum(metric::kNetHomeWifi, w.homeWifi ? 1 : 0, now);
    const Reason missing = w.connected ? Reason::ReadFailed : Reason::NotConnected;
    st.setF32(metric::kNetSignalDbm, w.signalDbm, now, missing);
    st.setF32(metric::kNetTxLinkMbps, w.txLinkMbps, now, missing);
    st.setF32(metric::kNetRxLinkMbps, w.rxLinkMbps, now, missing);
    // The rates are a delta: NaN on the first read after a (re)start
    const Reason noRate = !w.connected ? Reason::NotConnected : (resumed ? Reason::NotSampledYet : Reason::ReadFailed);
    st.setF32(metric::kNetRxMbps, w.rxMbps, now, noRate);
    st.setF32(metric::kNetTxMbps, w.txMbps, now, noRate);
}

}  // namespace

void registerSystemSources(frame_perf_overlay::Scheduler& scheduler, Sensors& sensors,
                           frame_perf_overlay::MetricStore& store) {
    frame_perf_overlay::MetricStore& st = store;
    Sensors& se = sensors;

    scheduler.addSource(SourceId::CpuLoad, [&st, &se](double now, bool resumed) {
        // A delta: after a pause, rebuild the baseline instead of showing the average over the pause
        if (resumed) se.resetCpuBaseline();
        SensorSample s;
        se.readCpuUsage(s);
        const Reason nan = resumed ? Reason::NotSampledYet : Reason::ReadFailed;
        st.setF32(metric::kCpuUsagePct, s.cpuUsagePct, now, nan);
        st.setF32(metric::kCpuMaxCorePct, s.cpuMaxCorePct, now, nan);
    });
    scheduler.addSource(SourceId::CpuClock, [&st, &se](double now, bool) {
        SensorSample s;
        se.readCpuClocks(s);
        st.setF32(metric::kCpuFastestClockMhz, s.cpuFreqMHz, now);
    });
    scheduler.addSource(SourceId::GpuClock, [&st, &se](double now, bool) {
        SensorSample s;
        se.readGpuClock(s);
        st.setF32(metric::kGpuClockMhz, s.gpuFreqMHz, now);
        st.setF32(metric::kGpuMaxClockMhz, s.gpuMaxFreqMHz, now);
    });
    scheduler.addSource(SourceId::Ram, [&st, &se](double now, bool) {
        SensorSample s;
        se.readMemory(s);
        st.setF32(metric::kRamUsedGib, s.memUsedGiB, now);
        st.setF32(metric::kRamTotalGib, s.memTotalGiB, now);
    });
    scheduler.addSource(SourceId::Temps, [&st, &se](double now, bool) {
        SensorSample s;
        se.readThermal(s);
        st.setF32(metric::kTempCpuC, s.cpuTempC, now);
        st.setF32(metric::kTempGpuC, s.gpuTempC, now);
        st.setF32(metric::kTempDdrC, s.ddrTempC, now);
    });
    scheduler.addSource(SourceId::BoardTemps, [&st, &se](double now, bool) {
        SensorSample s;
        se.readIioTemps(s);
        st.setF32(metric::kTempDisplayC, s.displayTempC, now);
        st.setF32(metric::kTempExhaustC, s.exhaustTempC, now);
        st.setF32(metric::kTempHeatsinkC, s.heatsinkTempC, now);
    });
    scheduler.addSource(SourceId::Throttle, [&st, &se](double now, bool) {
        SensorSample s;
        se.readThrottle(s);
        st.setEnum(metric::kThrottleCpu, s.throttleCpu ? 1 : 0, now);
        st.setEnum(metric::kThrottleGpu, s.throttleGpu ? 1 : 0, now);
    });
    scheduler.addSource(SourceId::Fan, [&st, &se](double now, bool) {
        SensorSample s;
        se.readFan(s);
        st.setF32(metric::kFanRpm, s.fanRpm, now);
    });
    scheduler.addSource(SourceId::Power, [&st, &se](double now, bool) {
        SensorSample s;
        se.readPower(s);
        storePower(st, s, now);
    });
    scheduler.addSource(SourceId::Battery, [&st, &se](double now, bool) {
        SensorSample s;
        se.readBattery(s);
        storeBattery(st, s, now);
    });
    scheduler.addSource(SourceId::Network, [&st, &se](double now, bool resumed) {
        // Byte-counter rates: after a pause they'd be the average over the pause
        if (resumed) se.resetWifiCounters();
        SensorSample s;
        se.readWifi(s, now);
        storeWifi(st, s.wifi, resumed, now);
    });
    scheduler.addSource(SourceId::Clock, [&st](double now, bool) {
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        char text[32];
        if (localtime_r(&t, &tm) != nullptr && std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm) > 0) {
            st.setText(metric::kClock, text, now);
        } else {
            st.setNa(metric::kClock, Reason::ReadFailed, now);
        }
    });
}
