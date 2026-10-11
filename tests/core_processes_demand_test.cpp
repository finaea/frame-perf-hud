// With the real compiled-in registry: only the CPU / RAM usage items widgets (7 / 8) keep the /proc scan running.
#include "scheduler.h"

#include <cmath>
#include <cstdio>

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
using W = WidgetType;

struct Rig {
    Scheduler sched;  // default ctor = compiled-in registry
    int reads = 0;
    double now = 0.0;
    Rig() {
        sched.addSource(SourceId::Processes, [this](double, bool) { ++reads; });
        DemandInputs d;
        d.editorOpen = true;
        sched.setDemand(d);
    }
    /** 500 ms ticks for the given number of seconds. */
    void run(double seconds) {
        const int ticks = static_cast<int>(seconds * 2.0);
        for (int i = 0; i < ticks; ++i) {
            sched.runDue(now);
            now += 0.5;
        }
    }
};

void cpuItemsScansEveryTwoSeconds() {
    Rig r;
    CHECK(r.sched.interval(SourceId::Processes) == 2.0);  // T04: was 1 s (budget)
    r.sched.setEnabledWidgets({W::CpuItems});
    CHECK(r.sched.refCount(SourceId::Processes) == 1);
    r.run(5.0);
    CHECK(r.reads == 3);  // 2 s interval over 5 s: t = 0, 2, 4
}

void ramItemsAloneStillScans() {
    Rig r;
    r.sched.setEnabledWidgets({W::RamItems});
    CHECK(r.sched.refCount(SourceId::Processes) == 1);
    r.run(5.0);
    CHECK(r.reads == 3);
}

void withoutItemsWidgetsScanStops() {
    Rig r;
    r.sched.setEnabledWidgets({W::CpuItems, W::RamItems});
    CHECK(r.sched.refCount(SourceId::Processes) == 2);
    r.run(3.0);
    CHECK(r.reads > 0);

    r.sched.setEnabledWidgets({W::Frame, W::Network});
    CHECK(r.sched.refCount(SourceId::Processes) == 0);
    const int frozen = r.reads;
    r.run(10.0);
    CHECK(r.reads == frozen);
}

void noScanWithoutAnyWidgetEver() {
    Rig r;
    r.sched.setEnabledWidgets({W::Frame, W::Network});
    r.run(5.0);
    CHECK(r.reads == 0);
}

}  // namespace

int main() {
    cpuItemsScansEveryTwoSeconds();
    ramItemsAloneStillScans();
    withoutItemsWidgetsScanStops();
    noScanWithoutAnyWidgetEver();
    if (failures == 0) std::printf("core_processes_demand_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
