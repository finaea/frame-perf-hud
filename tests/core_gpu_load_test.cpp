// gpu.busy_pct = frame GPU ms x app fps (core/gpu_load.h): the formula, and the registry / demand contract that
// makes a GPU & RAM widget alone keep FrameTiming read. No platform code (SensorBridge itself needs OpenVR, so the
// N/A paths are checked on the store with the same calls readFrameTiming makes).
#include "gpu_load.h"
#include "metric_registry.h"
#include "metric_store.h"
#include "scheduler.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool near(double a, double b, double eps = 0.01) {
    return std::fabs(a - b) <= eps;
}

void testFormula() {
    CHECK(near(estimateGpuLoadPct(16.5, 58.6), 96.69));  // Half-Life: Alyx at 72 Hz
    CHECK(near(estimateGpuLoadPct(3.28, 72), 23.616));
    CHECK(estimateGpuLoadPct(20, 90) == 100.0);  // overlap into the next frame is clamped
    CHECK(estimateGpuLoadPct(0, 72) == 0.0);
    CHECK(estimateGpuLoadPct(16, 0) == 0.0);
    CHECK(estimateGpuLoadPct(0, 0) == 0.0);

    CHECK(std::isnan(estimateGpuLoadPct(kNaN, 72)));
    CHECK(std::isnan(estimateGpuLoadPct(16, kNaN)));
    CHECK(std::isnan(estimateGpuLoadPct(kInf, 72)));
    CHECK(std::isnan(estimateGpuLoadPct(16, kInf)));
    CHECK(std::isnan(estimateGpuLoadPct(-kInf, 72)));
    CHECK(std::isnan(estimateGpuLoadPct(-1, 72)));
    CHECK(std::isnan(estimateGpuLoadPct(16, -1)));
}

void testRegistryContract() {
    const MetricDef* d = findMetric(metric::kGpuBusyPct);
    CHECK(d != nullptr);
    if (d == nullptr) return;
    CHECK(metric::kGpuBusyPct == 300);  // the link / session-log contract
    CHECK(std::string(d->name) == "gpu.busy_pct");
    CHECK(d->owner == WidgetType::GpuRam);
    CHECK(d->source == SourceId::FrameTiming);
    CHECK(d->type == WireType::F32);
    CHECK(validateRegistry(registry()).empty());
}

/** What readFrameTiming stores for gpu.busy_pct without frame timing (no OpenVR): N/A, never a stale number. */
void testNaWithoutFrameTiming() {
    MetricStore store;
    const MetricValue* v = store.get(metric::kGpuBusyPct);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::NotSampledYet);
    store.setF32(metric::kGpuBusyPct, 50, 1.0);
    CHECK(store.get(metric::kGpuBusyPct)->ok);
    store.setF32(metric::kGpuBusyPct, kNaN, 2.0, Reason::NoSteamVr);
    v = store.get(metric::kGpuBusyPct);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::NoSteamVr);
    store.setF32(metric::kGpuBusyPct, estimateGpuLoadPct(kNaN, 72), 3.0, Reason::NoFrames);
    v = store.get(metric::kGpuBusyPct);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::NoFrames);
}

DemandInputs looking() {
    DemandInputs d;
    d.hudShown = true;
    d.hmdInUse = true;
    return d;
}

void testDemand() {
    {
        Scheduler sched;
        sched.setEnabledWidgets({WidgetType::GpuRam});
        sched.setDemand(looking());
        CHECK(sched.refCount(SourceId::FrameTiming) > 0);
        CHECK(sched.interval(SourceId::FrameTiming) > 0);
    }
    {
        Scheduler sched;
        sched.setEnabledWidgets({WidgetType::Cpu});
        sched.setDemand(looking());
        CHECK(sched.refCount(SourceId::FrameTiming) == 0);
    }
    {
        // nobody looking: even GPU & RAM reads nothing
        Scheduler sched;
        sched.setEnabledWidgets({WidgetType::GpuRam});
        CHECK(sched.refCount(SourceId::FrameTiming) == 0);
    }
}

}  // namespace

int main() {
    testFormula();
    testRegistryContract();
    testNaWithoutFrameTiming();
    testDemand();
    if (failures == 0) std::printf("core-gpu-load-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
