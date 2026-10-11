#include "sensor_bridge.h"

#include "vr_overlay.h"

#include <cmath>

using fph::Reason;
using fph::SourceId;
using fph::WidgetType;
namespace metric = fph::metric;

namespace {

/** Power rails by max34417 label. vph has its own metric (kPowerVphW). */
struct RailId {
    const char* label;
    uint16_t id;
};
constexpr RailId kRails[] = {
    {"apc0", metric::kPowerApc0W}, {"apc1", metric::kPowerApc1W}, {"apc2", metric::kPowerApc2W},
    {"gfx", metric::kPowerGfxW},   {"nsp1", metric::kPowerNsp1W}, {"nsp2", metric::kPowerNsp2W},
    {"s1c", metric::kPowerS1cW},   {"s3c", metric::kPowerS3cW},   {"s6c", metric::kPowerS6cW},
    {"bob", metric::kPowerBobW},
};

uint32_t batteryStatusCode(const std::string& status) {
    using fph::BatteryStatus;
    if (status == "Charging") return static_cast<uint32_t>(BatteryStatus::Charging);
    if (status == "Discharging") return static_cast<uint32_t>(BatteryStatus::Discharging);
    if (status == "Full") return static_cast<uint32_t>(BatteryStatus::Full);
    if (status == "Not charging") return static_cast<uint32_t>(BatteryStatus::NotCharging);
    return static_cast<uint32_t>(BatteryStatus::Unknown);
}

std::string batteryStatusString(int64_t code) {
    using fph::BatteryStatus;
    switch (static_cast<BatteryStatus>(code)) {
        case BatteryStatus::Charging: return "Charging";
        case BatteryStatus::Discharging: return "Discharging";
        case BatteryStatus::Full: return "Full";
        case BatteryStatus::NotCharging: return "Not charging";
        case BatteryStatus::Unknown: break;
    }
    return "";
}

}  // namespace

SensorBridge::SensorBridge(Sensors& sensors, VrOverlay* vr, fph::MetricStore& store)
    : sensors_(sensors), vr_(vr), store_(store) {}

std::vector<WidgetType> SensorBridge::upstreamPanelWidgets() {
    return {WidgetType::Frame,   WidgetType::Cpu,  WidgetType::GpuRam,  WidgetType::Power,
            WidgetType::Battery, WidgetType::Temp, WidgetType::Network, WidgetType::General};
}

void SensorBridge::registerSources(fph::Scheduler& scheduler) {
    fph::MetricStore& st = store_;
    Sensors& se = sensors_;

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
        st.setF32(metric::kCpuBigCoreClockMhz, s.cpuBigFreqMHz, now);
    });
    scheduler.addSource(SourceId::GpuClock, [&st, &se](double now, bool) {
        SensorSample s;
        se.readGpuClock(s);
        st.setF32(metric::kGpuClockMhz, s.gpuFreqMHz, now);
        st.setF32(metric::kGpuMaxClockMhz, s.gpuMaxFreqMHz, now);
    });
    scheduler.addSource(SourceId::GpuBusy, [&st, &se](double now, bool resumed) {
        SensorSample s;
        se.readGpuBusy(s, now);
        // A delta too: the first value after a pause would be the average over the pause
        if (resumed) {
            st.setNa(metric::kGpuBusyPct, Reason::NotSampledYet, now);
        } else {
            st.setF32(metric::kGpuBusyPct, s.gpuBusyPct, now);
        }
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
        st.setF32(metric::kPowerVphW, s.powerMainW, now);
        st.setF32(metric::kPowerAllChannelsW, s.powerSumW, now);
        for (const RailId& rail : kRails) {
            double watts = kNoValue;
            bool found = false;
            for (const PowerChannel& ch : s.powerChannels) {
                if (ch.label != rail.label) continue;
                found = true;
                watts = ch.watts;
                break;
            }
            if (found) {
                st.setF32(rail.id, watts, now);
            } else {
                st.setNa(rail.id, Reason::SensorMissing, now);
            }
        }
    });
    scheduler.addSource(SourceId::Battery, [&st, &se](double now, bool) {
        SensorSample s;
        se.readBattery(s);
        st.setF32(metric::kBatteryPct, s.batteryPct, now);
        st.setF32(metric::kBatteryVoltageV, s.batteryVoltageV, now);
        st.setF32(metric::kBatteryCurrentMa, s.batteryCurrentMa, now);
        st.setF32(metric::kTempBatteryC, s.batteryTempC, now);
        st.setEnum(metric::kBatteryStatus, batteryStatusCode(s.batteryStatus), now);
        st.setEnum(metric::kBatteryChargerOnline, s.chargerOnline ? 1 : 0, now);
    });
    scheduler.addSource(SourceId::Network, [&st, &se](double now, bool resumed) {
        SensorSample s;
        se.readWifi(s, now);
        const WifiInfo& w = s.wifi;
        st.setEnum(metric::kNetInterfaceUp, w.interfaceUp ? 1 : 0, now);
        st.setEnum(metric::kNetConnected, w.connected ? 1 : 0, now);
        st.setEnum(metric::kNetHomeWifi, w.homeWifi ? 1 : 0, now);
        const Reason missing = w.connected ? Reason::ReadFailed : Reason::NotConnected;
        st.setF32(metric::kNetSignalDbm, w.signalDbm, now, missing);
        st.setF32(metric::kNetTxLinkMbps, w.txLinkMbps, now, missing);
        st.setF32(metric::kNetRxLinkMbps, w.rxLinkMbps, now, missing);
        // Byte-counter rates: after a pause they'd be the average over the pause
        if (resumed) {
            st.setNa(metric::kNetRxMbps, Reason::NotSampledYet, now);
            st.setNa(metric::kNetTxMbps, Reason::NotSampledYet, now);
        } else {
            st.setF32(metric::kNetRxMbps, w.rxMbps, now, missing);
            st.setF32(metric::kNetTxMbps, w.txMbps, now, missing);
        }
    });
    scheduler.addSource(SourceId::FrameTiming, [this](double now, bool) { readFrameTiming(now); });
    scheduler.addSource(SourceId::Controllers, [this](double now, bool) { readControllers(now); });
}

void SensorBridge::readFrameTiming(double now) {
    static constexpr uint16_t kIds[] = {
        metric::kFrameAppFps,   metric::kFrameDisplayHz,      metric::kFrameGpuMs,   metric::kFrameCpuMs,
        metric::kFrameGpuMaxMs, metric::kFrameCpuMaxMs,       metric::kFrameReprojectedPct,
        metric::kFrameDropped,  metric::kFrameThrottled,      metric::kFrameCount,
    };
    // Batched: VrOverlay::readFrameStats aggregates every frame since the previous call
    const FrameStats f = vr_ != nullptr ? vr_->readFrameStats() : FrameStats();
    if (!f.valid) {
        for (uint16_t id : kIds) store_.setNa(id, vr_ != nullptr ? Reason::NoFrames : Reason::NoSteamVr, now);
        return;
    }
    store_.setF32(metric::kFrameAppFps, f.appFps, now, Reason::NoFrames);
    store_.setF32(metric::kFrameDisplayHz, f.displayHz > 0 ? f.displayHz : kNoValue, now);
    store_.setF32(metric::kFrameReprojectedPct, f.reprojectedPct, now);
    store_.setI32(metric::kFrameDropped, static_cast<int32_t>(f.dropped), now);
    store_.setEnum(metric::kFrameThrottled, static_cast<uint32_t>(f.throttledFrames), now);
    store_.setI32(metric::kFrameCount, static_cast<int32_t>(f.frames), now);
    // No new frame in this batch = no times (upstream's haveTimes)
    const Reason noTimes = Reason::NoFrames;
    store_.setF32(metric::kFrameGpuMs, f.haveTimes ? f.gpuMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameCpuMs, f.haveTimes ? f.cpuMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameGpuMaxMs, f.haveTimes ? f.gpuMaxMs : kNoValue, now, noTimes);
    store_.setF32(metric::kFrameCpuMaxMs, f.haveTimes ? f.cpuMaxMs : kNoValue, now, noTimes);
}

void SensorBridge::readControllers(double now) {
    const ControllerStatus c = vr_ != nullptr ? vr_->readControllers() : ControllerStatus();
    const Reason absent = vr_ != nullptr ? Reason::NotConnected : Reason::NoSteamVr;
    const auto put = [&](const ControllerBattery& b, uint16_t pctId, uint16_t chargingId) {
        if (!b.present) {
            store_.setNa(pctId, absent, now);
            store_.setNa(chargingId, absent, now);
            return;
        }
        store_.setF32(pctId, b.pct, now);
        store_.setEnum(chargingId, b.charging ? 1 : 0, now);
    };
    put(c.left, metric::kCtrlLeftPct, metric::kCtrlLeftCharging);
    put(c.right, metric::kCtrlRightPct, metric::kCtrlRightCharging);
}

SensorSample SensorBridge::sample() const {
    const fph::MetricStore& st = store_;
    const auto f = [&st](uint16_t id) { return st.f32Or(id, kNoValue); };
    const auto flag = [&st](uint16_t id) { return st.intOr(id, 0) != 0; };
    SensorSample s;
    s.cpuUsagePct = f(metric::kCpuUsagePct);
    s.cpuMaxCorePct = f(metric::kCpuMaxCorePct);
    s.cpuFreqMHz = f(metric::kCpuFastestClockMhz);
    s.cpuBigFreqMHz = f(metric::kCpuBigCoreClockMhz);
    s.gpuFreqMHz = f(metric::kGpuClockMhz);
    s.gpuMaxFreqMHz = f(metric::kGpuMaxClockMhz);
    s.gpuBusyPct = f(metric::kGpuBusyPct);
    s.cpuTempC = f(metric::kTempCpuC);
    s.gpuTempC = f(metric::kTempGpuC);
    s.ddrTempC = f(metric::kTempDdrC);
    s.batteryTempC = f(metric::kTempBatteryC);
    s.displayTempC = f(metric::kTempDisplayC);
    s.exhaustTempC = f(metric::kTempExhaustC);
    s.heatsinkTempC = f(metric::kTempHeatsinkC);
    s.throttleCpu = flag(metric::kThrottleCpu);
    s.throttleGpu = flag(metric::kThrottleGpu);
    s.wifi.interfaceUp = flag(metric::kNetInterfaceUp);
    s.wifi.connected = flag(metric::kNetConnected);
    s.wifi.homeWifi = flag(metric::kNetHomeWifi);
    s.wifi.signalDbm = f(metric::kNetSignalDbm);
    s.wifi.txLinkMbps = f(metric::kNetTxLinkMbps);
    s.wifi.rxLinkMbps = f(metric::kNetRxLinkMbps);
    s.wifi.rxMbps = f(metric::kNetRxMbps);
    s.wifi.txMbps = f(metric::kNetTxMbps);
    s.powerMainW = f(metric::kPowerVphW);
    s.powerSumW = f(metric::kPowerAllChannelsW);
    // --print lists the rails; the chip name isn't a metric, so they come back without it
    if (st.get(metric::kPowerVphW)->ok) s.powerChannels.push_back({"", "vph", s.powerMainW});
    for (const RailId& rail : kRails) {
        const fph::MetricValue* v = st.get(rail.id);
        if (v->ok || v->reason != Reason::SensorMissing) s.powerChannels.push_back({"", rail.label, f(rail.id)});
    }
    s.batteryPct = f(metric::kBatteryPct);
    s.batteryVoltageV = f(metric::kBatteryVoltageV);
    s.batteryCurrentMa = f(metric::kBatteryCurrentMa);
    s.batteryStatus = batteryStatusString(st.intOr(metric::kBatteryStatus, 0));
    s.chargerOnline = flag(metric::kBatteryChargerOnline);
    s.fanRpm = f(metric::kFanRpm);
    s.memUsedGiB = f(metric::kRamUsedGib);
    s.memTotalGiB = f(metric::kRamTotalGib);
    return s;
}

FrameStats SensorBridge::frame() const {
    const fph::MetricStore& st = store_;
    FrameStats fr;
    // frame.frames is set on every valid read, so it stands for upstream's "valid"
    fr.valid = st.get(metric::kFrameCount)->ok;
    if (!fr.valid) return fr;
    fr.appFps = st.f32Or(metric::kFrameAppFps, kNoValue);
    fr.displayHz = st.f32Or(metric::kFrameDisplayHz, 0.0);
    fr.targetMs = fr.displayHz > 0 ? 1000.0 / fr.displayHz : 0.0;
    fr.haveTimes = st.get(metric::kFrameGpuMs)->ok;
    fr.gpuMs = st.f32Or(metric::kFrameGpuMs, 0.0);
    fr.cpuMs = st.f32Or(metric::kFrameCpuMs, 0.0);
    fr.gpuMaxMs = st.f32Or(metric::kFrameGpuMaxMs, 0.0);
    fr.cpuMaxMs = st.f32Or(metric::kFrameCpuMaxMs, 0.0);
    fr.frames = static_cast<uint32_t>(st.intOr(metric::kFrameCount, 0));
    fr.reprojectedPct = st.f32Or(metric::kFrameReprojectedPct, 0.0);
    fr.dropped = static_cast<uint32_t>(st.intOr(metric::kFrameDropped, 0));
    fr.throttledFrames = static_cast<int>(st.intOr(metric::kFrameThrottled, 0));
    return fr;
}

ControllerStatus SensorBridge::controllers() const {
    const fph::MetricStore& st = store_;
    ControllerStatus c;
    const auto get = [&st](uint16_t pctId, uint16_t chargingId) {
        ControllerBattery b;
        const fph::MetricValue* pct = st.get(pctId);
        b.present = pct->ok || pct->reason == Reason::ReadFailed;  // present but the % didn't read
        b.pct = st.f32Or(pctId, kNoValue);
        b.charging = st.intOr(chargingId, 0) != 0;
        return b;
    };
    c.left = get(metric::kCtrlLeftPct, metric::kCtrlLeftCharging);
    c.right = get(metric::kCtrlRightPct, metric::kCtrlRightCharging);
    return c;
}
