#include "dump_metrics.h"

#include "metric_store.h"
#include "scheduler.h"
#include "sensor_bridge.h"
#include "sensors.h"
#include "vr_overlay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <thread>

namespace {

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void sleepFor(double seconds, const volatile std::sig_atomic_t& stop) {
    const double end = monotonicSeconds() + seconds;
    while (!stop && monotonicSeconds() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

std::string clockText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

}  // namespace

bool parseWidgetList(const std::string& text, std::vector<fph::WidgetType>& out) {
    out.clear();
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        char* end = nullptr;
        const long n = std::strtol(item.c_str(), &end, 10);
        if (*end != '\0' || n < 1 || n >= static_cast<long>(fph::kWidgetTypeCount)) return false;
        out.push_back(static_cast<fph::WidgetType>(n));
    }
    return true;
}

int runDumpMetrics(const DumpMetricsOptions& options, const volatile std::sig_atomic_t& stop) {
    Sensors sensors;
    sensors.discover();
    VrOverlay vr;
    bool vrOk = false;
    if (options.vr) {
        std::string message;
        vrOk = vr.connectReadOnly(message);
        if (!vrOk) std::fprintf(stderr, "[VR] Can't connect (continuing without SteamVR): %s\n", message.c_str());
    }
    fph::MetricStore store;
    fph::Scheduler scheduler;
    SensorBridge bridge(sensors, vrOk ? &vr : nullptr, store);
    ProcessListOptions lists;
    lists.sumSameName = options.sumSameName;
    bridge.setProcessListOptions(lists);
    bridge.registerSources(scheduler);
    std::vector<fph::WidgetType> widgets = options.widgets;
    if (widgets.empty()) {
        for (int w = 1; w <= 11; ++w) widgets.push_back(static_cast<fph::WidgetType>(w));
    }
    scheduler.setEnabledWidgets(widgets);
    fph::DemandInputs demand;
    demand.hudShown = true;
    demand.hmdInUse = true;
    scheduler.setDemand(demand);

    // Like the overlay on connect: the scene app is queried at once, not on the first event
    const double t0 = monotonicSeconds();
    bridge.refreshSceneApp(t0);
    const fph::SessionInfo& s = bridge.session();
    std::printf("== startup %s: scene app queried in %.1f ms: session %d, app %s (%s)\n", clockText().c_str(),
                (monotonicSeconds() - t0) * 1e3, s.id, s.appKey.empty() ? "-" : s.appKey.c_str(),
                s.appName.empty() ? "-" : s.appName.c_str());

    scheduler.runDue(monotonicSeconds());  // baselines for the delta sources
    for (int i = 1; i <= options.count && !stop; ++i) {
        sleepFor(options.intervalSec, stop);
        const double now = monotonicSeconds();
        if (vrOk) bridge.refreshSceneApp(now);  // no event pump here, so re-query each print
        scheduler.runDue(now);
        std::printf("== [%d/%d] %s ==\n%s\n%s\n%s", i, options.count, clockText().c_str(), scheduler.dump(now).c_str(),
                    store.dump(now).c_str(), bridge.debugText().c_str());
        std::fflush(stdout);
    }
    if (vrOk) vr.shutdown();
    return 0;
}
