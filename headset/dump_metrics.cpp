#include "dump_metrics.h"

#include "layout.h"
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

bool parseWidgetList(const std::string& text, std::vector<frame_perf_overlay::WidgetType>& out) {
    out.clear();
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        char* end = nullptr;
        const long n = std::strtol(item.c_str(), &end, 10);
        if (*end != '\0' || n < 1 || n > 255) return false;
        if (frame_perf_overlay::widgetKey(static_cast<frame_perf_overlay::WidgetType>(n)) == nullptr) return false;
        out.push_back(static_cast<frame_perf_overlay::WidgetType>(n));
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
    frame_perf_overlay::MetricStore store;
    frame_perf_overlay::Scheduler scheduler;
    SensorBridge bridge(sensors, vrOk ? &vr : nullptr, store);
    bridge.registerSources(scheduler);
    std::vector<frame_perf_overlay::WidgetType> widgets = options.widgets;
    if (widgets.empty()) widgets = SensorBridge::allWidgets();
    scheduler.setEnabledWidgets(widgets);
    frame_perf_overlay::DemandInputs demand;
    demand.hudShown = true;
    demand.hmdInUse = true;
    scheduler.setDemand(demand);

    scheduler.runDue(monotonicSeconds());  // baselines for the delta sources
    for (int i = 1; i <= options.count && !stop; ++i) {
        sleepFor(options.intervalSec, stop);
        const double now = monotonicSeconds();
        scheduler.runDue(now);
        std::printf("== [%d/%d] %s ==\n%s\n%s", i, options.count, clockText().c_str(), scheduler.dump(now).c_str(),
                    store.dump(now).c_str());
        std::fflush(stdout);
    }
    if (vrOk) vr.shutdown();
    return 0;
}
