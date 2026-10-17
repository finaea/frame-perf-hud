#include "sensor_bridge.h"

#include "system_metrics.h"
#include "vr_overlay.h"

#include <algorithm>  // T04
#include <cmath>
#include <cstdio>
#include <ctime>

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
    // T02: CPU, GPU & RAM, Power, Battery, Temp, uptime / clock / own cost (headset/system_metrics.cpp)
    registerSystemSources(scheduler, sensors_, store_);
    // T03: both adapters from the bridge's own WifiLink (Sensors::readWifi is no longer used)
    scheduler.addSource(SourceId::Network, [this](double now, bool resumed) { readNetwork(now, resumed); });
    scheduler.addSource(SourceId::FrameTiming, [this](double now, bool) { readFrameTiming(now); });
    scheduler.addSource(SourceId::Controllers, [this](double now, bool) { readControllers(now); });
    // T03
    scheduler.addSource(SourceId::Processes, [this](double now, bool resumed) { readProcesses(now, resumed); });
    scheduler.addSource(SourceId::SceneApp, [this](double now, bool) { refreshSceneApp(now); });
    scheduler.addSource(SourceId::StreamLink, [this](double now, bool resumed) { readStreamLink(now, resumed); });
    scheduler.addSource(SourceId::StreamEvents, [this](double now, bool) { readStreamEvents(now); });
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
    s.powerUnmeasuredW = f(metric::kPowerUnmeasuredW);
    // --print lists the rails; the chip name isn't a metric, so they come back without it
    if (st.get(metric::kPowerVphW)->ok) s.powerChannels.push_back({"", "vph", s.powerMainW, f(metric::kPowerVphV)});
    for (const RailId& rail : kRails) {
        const fph::MetricValue* v = st.get(rail.id);
        if (v->ok || v->reason != Reason::SensorMissing) {
            s.powerChannels.push_back({"", rail.label, f(rail.id), f(rail.id + metric::kPowerRailVoltsOffset)});
        }
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

// ---- T03: network per adapter, process lists, current game / session, stream link + events ----

namespace {

/** Stores one adapter's block (metric::kNetDongleBase / kNetHomeBase + kNetAdapter* offsets). */
void putAdapter(fph::MetricStore& st, uint16_t base, const AdapterInfo& a, bool resumed, double now) {
    using namespace fph::metric;
    st.setEnum(base + kNetAdapterPresent, a.present ? 1 : 0, now);
    const uint16_t rest[] = {kNetAdapterConnected, kNetAdapterRxMbps,  kNetAdapterTxMbps,   kNetAdapterTxLinkMbps,
                             kNetAdapterRxLinkMbps, kNetAdapterSignalDbm, kNetAdapterIp,     kNetAdapterFreqMhz,
                             kNetAdapterChannel,   kNetAdapterWidthMhz, kNetAdapterBand,     kNetAdapterIfName};
    if (!a.present) {
        for (uint16_t offset : rest) st.setNa(base + offset, Reason::SensorMissing, now);
        return;
    }
    st.setText(base + kNetAdapterIfName, a.ifName, now);
    st.setEnum(base + kNetAdapterConnected, a.connected ? 1 : 0, now);
    // Byte counters work without a peer; NaN = first read after a (re)start
    const Reason firstRead = resumed ? Reason::NotSampledYet : Reason::ReadFailed;
    st.setF32(base + kNetAdapterRxMbps, a.rxMbps, now, firstRead);
    st.setF32(base + kNetAdapterTxMbps, a.txMbps, now, firstRead);
    const Reason noPeer = a.connected ? Reason::ReadFailed : Reason::NotConnected;
    st.setF32(base + kNetAdapterTxLinkMbps, a.txLinkMbps, now, noPeer);
    st.setF32(base + kNetAdapterRxLinkMbps, a.rxLinkMbps, now, noPeer);
    st.setF32(base + kNetAdapterSignalDbm, a.signalDbm, now, noPeer);
    if (a.ip.empty()) {
        st.setNa(base + kNetAdapterIp, Reason::NotConnected, now);
    } else {
        st.setText(base + kNetAdapterIp, a.ip, now);
    }
    // nl80211 reports a channel only while the interface is on one
    if (a.freqMhz > 0) {
        st.setI32(base + kNetAdapterFreqMhz, a.freqMhz, now);
        const int channel = channelFromFreq(a.freqMhz);
        if (channel > 0) {
            st.setI32(base + kNetAdapterChannel, channel, now);
        } else {
            st.setNa(base + kNetAdapterChannel, Reason::InvalidReading, now);
        }
        st.setEnum(base + kNetAdapterBand, bandFromFreq(a.freqMhz), now);
    } else {
        st.setNa(base + kNetAdapterFreqMhz, Reason::NotConnected, now);
        st.setNa(base + kNetAdapterChannel, Reason::NotConnected, now);
        st.setNa(base + kNetAdapterBand, Reason::NotConnected, now);
    }
    if (a.widthMhz > 0) {
        st.setI32(base + kNetAdapterWidthMhz, a.widthMhz, now);
    } else {
        st.setNa(base + kNetAdapterWidthMhz, a.freqMhz > 0 ? Reason::ReadFailed : Reason::NotConnected, now);
    }
}

const char* linkModeName(int64_t mode) {
    switch (mode) {
        case 0: return "none";
        case 1: return "dongle";
        case 2: return "wifi";
        case 3: return "multi";
        case 4: return "idle";
        default: return "?";
    }
}

}  // namespace

void SensorBridge::readNetwork(double now, bool resumed) {
    // Byte-counter rates: after a pause they'd be the average over the pause
    if (resumed) wifi_.resetCounters();
    const WifiAdapters a = wifi_.readAdapters(now);
    putAdapter(store_, metric::kNetDongleBase, a.dongle, resumed, now);
    putAdapter(store_, metric::kNetHomeBase, a.home, resumed, now);

    // Upstream's single combined line (900-907), from the same read
    const WifiInfo w = combineAdapters(a);
    store_.setEnum(metric::kNetInterfaceUp, w.interfaceUp ? 1 : 0, now);
    store_.setEnum(metric::kNetConnected, w.connected ? 1 : 0, now);
    store_.setEnum(metric::kNetHomeWifi, w.homeWifi ? 1 : 0, now);
    const Reason missing = w.connected ? Reason::ReadFailed : Reason::NotConnected;
    store_.setF32(metric::kNetSignalDbm, w.signalDbm, now, missing);
    store_.setF32(metric::kNetTxLinkMbps, w.txLinkMbps, now, missing);
    store_.setF32(metric::kNetRxLinkMbps, w.rxLinkMbps, now, missing);
    const Reason noRate = !w.connected ? Reason::NotConnected : (resumed ? Reason::NotSampledYet : Reason::ReadFailed);
    store_.setF32(metric::kNetRxMbps, w.rxMbps, now, noRate);
    store_.setF32(metric::kNetTxMbps, w.txMbps, now, noRate);
}

void SensorBridge::readProcesses(double now, bool resumed) {
    // CPU % is a delta: after a pause, start a fresh one rather than average over the pause
    if (resumed) processes_.reset();
    const ProcessLists& l = processes_.scan(now, processOptions_);
    ++processScans_;
    if (l.scanned == 0) {
        for (uint16_t id : {metric::kProcScanned, metric::kProcVanished, metric::kProcScanMs, metric::kCpuItemsRows,
                            metric::kRamItemsRows}) {
            store_.setNa(id, Reason::ReadFailed, now);
        }
        return;
    }
    store_.setI32(metric::kProcScanned, l.scanned, now);
    store_.setI32(metric::kProcVanished, l.vanished, now);
    store_.setF32(metric::kProcScanMs, l.scanMs, now);
    if (l.cpuValid) {
        store_.setI32(metric::kCpuItemsRows, static_cast<int32_t>(l.byCpu.size()), now);
    } else {
        store_.setNa(metric::kCpuItemsRows, Reason::NotSampledYet, now);
    }
    store_.setI32(metric::kRamItemsRows, static_cast<int32_t>(l.byRam.size()), now);
}

void SensorBridge::refreshSceneApp(double now) {
    const uint16_t ids[] = {metric::kSceneAppKey, metric::kSceneAppName, metric::kScenePid, metric::kSessionId,
                            metric::kSessionStartUnix, metric::kStreamAppHint};
    SceneApp app;
    if (vr_ == nullptr || !vr_->readSceneApp(app)) {
        for (uint16_t id : ids) store_.setNa(id, Reason::NoSteamVr, now);
        return;
    }
    if (app.key == kVrlinkAppKey) {
        // PC streaming: the scene app is vrlink, not the game. Its -A arg is only a hint (fixed at stream start)
        app.name = "PC stream";
        streamHint_.clear();
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%u/cmdline", app.pid);
        if (FILE* file = std::fopen(path, "r")) {
            char buffer[4096];
            const size_t n = std::fread(buffer, 1, sizeof(buffer), file);
            std::fclose(file);
            streamHint_ = vrlinkAppHint(buffer, n);
        }
    } else if (app.pid != 0) {
        streamHint_.clear();  // another app; pid 0 (the compositor) keeps the stream's hint
    }
    if (streamHint_.empty()) {
        store_.setNa(metric::kStreamAppHint, Reason::NotConnected, now);
    } else {
        store_.setText(metric::kStreamAppHint, streamHint_, now);
    }
    const int32_t before = session_.current().id;
    if (session_.update(app.key, app.name, static_cast<int64_t>(std::time(nullptr)))) {
        const fph::SessionInfo& s = session_.current();
        // T11: back to the game after a short detour (Home) is the same session
        std::fprintf(stderr, "[session] %s session %d: %s (%s), scene pid %u\n",
                     s.id == previousSessionId_ ? "Resumed" : "New", s.id, s.appKey.c_str(),
                     s.appName.empty() ? "no name" : s.appName.c_str(), app.pid);
        previousSessionId_ = before;
    }
    store_.setI32(metric::kScenePid, static_cast<int32_t>(app.pid), now);
    const fph::SessionInfo& s = session_.current();
    if (s.id == 0) {
        // No scene app seen since start (the compositor or SteamVR Home without an app key)
        for (uint16_t id : {metric::kSceneAppKey, metric::kSceneAppName, metric::kSessionId, metric::kSessionStartUnix}) {
            store_.setNa(id, Reason::NotConnected, now);
        }
        return;
    }
    // The session's app, not the live scene: pid 0 (the compositor between scenes) keeps the session
    store_.setText(metric::kSceneAppKey, s.appKey, now);
    store_.setText(metric::kSceneAppName, s.appName, now);
    store_.setI32(metric::kSessionId, s.id, now);
    store_.setI32(metric::kSessionStartUnix, static_cast<int32_t>(s.startUnix), now);
}

void SensorBridge::readStreamLink(double now, bool resumed) {
    if (resumed) {
        streamDongle_.reset();
        streamHome_.reset();
    }
    // Interface names from Network's last radio read; the Frame's names if Network hasn't run
    const std::string dongle = wifi_.dongleName().empty() ? "wlanap" : wifi_.dongleName();
    const std::string home = wifi_.homeName().empty() ? "wlan0" : wifi_.homeName();
    streamDongle_.update(dongle, now);
    streamHome_.update(home, now);
    const bool running = vrlink_.running(now);
    store_.setEnum(metric::kStreamVrlinkRunning, running ? 1 : 0, now);
    const double d = streamDongle_.rxMbps();
    const double h = streamHome_.rxMbps();
    if (running && std::isnan(d) && std::isnan(h)) {
        // First read: no rates yet, so don't call it idle
        store_.setNa(metric::kStreamLinkMode, Reason::NotSampledYet, now);
        store_.setNa(metric::kStreamDongleSharePct, Reason::NotSampledYet, now);
        return;
    }
    store_.setEnum(metric::kStreamLinkMode, classifyLinkMode(running, d, h), now);
    store_.setF32(metric::kStreamDongleSharePct, running ? dongleSharePct(d, h) : kNoValue, now, Reason::NotConnected);
}

void SensorBridge::readStreamEvents(double now) {
    const uint16_t ids[] = {metric::kStreamFecRetries,  metric::kStreamFecRetryPackets, metric::kStreamFramesDropped,
                            metric::kStreamLateMetadata, metric::kStreamAborts,         metric::kStreamUnrecoverable,
                            metric::kStreamLogOmitted};
    std::string error;
    if (!journal_.poll(error)) {
        if (error != journalError_) {
            std::fprintf(stderr, "[stream] Can't read vrlink's journal: %s\n", error.c_str());
            journalError_ = error;
        }
        for (uint16_t id : ids) store_.setNa(id, Reason::ReadFailed, now);
        return;
    }
    journalError_.clear();
    const VrlinkCounts& c = journal_.counts();
    const int32_t values[] = {c.fecRetries,   c.fecRetryPackets, c.framesDropped, c.lateMetadata,
                              c.streamAborts, c.unrecoverable,   c.logOmitted};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) store_.setI32(ids[i], values[i], now);
}

std::string SensorBridge::debugText() const {
    std::string out;
    char line[256];
    const fph::SessionInfo& s = session_.current();
    std::snprintf(line, sizeof(line), "session: id %d, app %s (%s), started %lld\n", s.id,
                  s.appKey.empty() ? "-" : s.appKey.c_str(), s.appName.empty() ? "-" : s.appName.c_str(),
                  static_cast<long long>(s.startUnix));
    out += line;
    std::snprintf(line, sizeof(line), "stream: vrlink pid %d, link mode %s\n", vrlink_.pid(),
                  store_.get(metric::kStreamLinkMode)->ok ? linkModeName(store_.intOr(metric::kStreamLinkMode, -1))
                                                          : "n/a");
    out += line;
    const ProcessLists& l = processes_.lists();
    std::snprintf(line, sizeof(line), "processes: %d scanned, %d vanished, %.2f ms, sum same name %s\n", l.scanned,
                  l.vanished, l.scanMs, processOptions_.sumSameName ? "on" : "off");
    out += line;
    const auto list = [&](const char* title, const std::vector<ProcessRow>& rows) {
        out += title;
        for (const ProcessRow& r : rows) {
            std::snprintf(line, sizeof(line), "  %-16s pid %-7d x%-3d cpu %6.1f%%core  rss %8.1f MiB\n", r.name.c_str(),
                          r.pid, r.count, r.cpuCorePct, r.rssMiB);
            out += line;
        }
    };
    list(l.cpuValid ? "by CPU (% of one core):\n" : "by CPU: no interval yet\n", l.byCpu);
    list("by RSS:\n", l.byRam);
    return out;
}

// ---- T04 ----

fph::ProcessListsView SensorBridge::processListsView() const {
    const ProcessLists& l = processes_.lists();
    const auto convert = [](const std::vector<ProcessRow>& rows) {
        std::vector<fph::ProcessItem> out;
        out.reserve(rows.size());
        for (const ProcessRow& r : rows) {
            fph::ProcessItem item;
            // comm is the main thread's name ("vrlinkrunthread"); the exe basename ("vrlink") reads better
            item.name = r.exe.empty() ? r.name : r.exe;
            item.pid = r.pid;
            item.count = r.count;
            item.cpuCorePct = r.cpuCorePct;
            item.rssMiB = r.rssMiB;
            out.push_back(std::move(item));
        }
        return out;
    };
    fph::ProcessListsView view;
    view.byCpu = convert(l.byCpu);
    view.byRam = convert(l.byRam);
    view.cpuValid = l.cpuValid;
    view.generation = processScans_;
    return view;
}

ProcessListOptions SensorBridge::processOptionsFor(const fph::Layout& layout) {
    ProcessListOptions options;
    options.cpuMax = 0;
    options.ramMax = 0;
    bool ramSum = false;
    for (const fph::LayoutColumn& column : layout.columns) {
        for (const fph::WidgetInstance& w : column.widgets) {
            if (w.type == fph::WidgetType::CpuItems) {
                options.cpuMax = std::max<size_t>(options.cpuMax, w.maxItems);
                options.sumSameName = options.sumSameName || w.sumSameName;
            } else if (w.type == fph::WidgetType::RamItems) {
                options.ramMax = std::max<size_t>(options.ramMax, w.maxItems);
                ramSum = ramSum || w.sumSameName;
            }
        }
    }
    options.ramSumSameName = ramSum ? 1 : 0;
    return options;
}
