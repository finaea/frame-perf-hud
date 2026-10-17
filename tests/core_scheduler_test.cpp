// core/ scheduler tests: reference counting, visibility rules, intervals (CTest runs them; no platform code).
#include "scheduler.h"

#include <cmath>
#include <cstdio>
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
using W = WidgetType;
using S = SourceId;

// 3 sources, 3 widgets. CpuLoad is shared by Cpu and CpuItems; Ram has two metrics (shortest interval wins).
const std::vector<MetricDef> kDefs = {
    {1, "cpu.a", "", W::Cpu, S::CpuLoad, 500, WireType::F32},
    {2, "cpu.items", "", W::CpuItems, S::CpuLoad, 1000, WireType::F32},
    {3, "gpu.busy", "", W::GpuRam, S::GpuClock, 2000, WireType::F32},
    {4, "ram.used", "", W::GpuRam, S::Ram, 2000, WireType::F32},
    {5, "ram.total", "", W::GpuRam, S::Ram, 1000, WireType::F32},
};

struct Counts {
    int cpu = 0, gpu = 0, ram = 0;
    bool lastCpuResumed = false;
    std::vector<bool> cpuResumed;
};

struct Rig {
    Scheduler sched {kDefs};
    Counts c;
    double now = 0.0;
    Rig() {
        sched.addSource(S::CpuLoad, [this](double, bool resumed) {
            ++c.cpu;
            c.lastCpuResumed = resumed;
            c.cpuResumed.push_back(resumed);
        });
        sched.addSource(S::GpuClock, [this](double, bool) { ++c.gpu; });
        sched.addSource(S::Ram, [this](double, bool) { ++c.ram; });
    }
    /** Runs the scheduler on a 500 ms tick for the given number of ticks. */
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            sched.runDue(now);
            now += 0.5;
        }
    }
    void look() {
        DemandInputs d;
        d.editorOpen = true;
        sched.setDemand(d);
    }
};

void testNothingWithoutWidgets() {
    Rig r;
    r.look();
    r.run(10);
    CHECK(r.c.cpu == 0 && r.c.gpu == 0 && r.c.ram == 0);
    CHECK(r.sched.refCount(S::CpuLoad) == 0);
}

void testEnableDisable() {
    Rig r;
    r.look();
    r.sched.setEnabledWidgets({W::Cpu});
    CHECK(r.sched.refCount(S::CpuLoad) == 1);
    CHECK(r.sched.refCount(S::GpuClock) == 0);
    r.run(4);
    CHECK(r.c.cpu == 4);  // 500 ms source, 500 ms ticks
    CHECK(r.sched.readCount(S::CpuLoad) == 4);
    CHECK(r.c.gpu == 0);

    r.sched.setEnabledWidgets({});
    CHECK(r.sched.refCount(S::CpuLoad) == 0);
    const int frozen = r.c.cpu;
    r.run(10);
    CHECK(r.c.cpu == frozen);
}

void testSharedSource() {
    Rig r;
    r.look();
    r.sched.setEnabledWidgets({W::Cpu, W::CpuItems});
    CHECK(r.sched.refCount(S::CpuLoad) == 2);
    r.sched.setEnabledWidgets({W::CpuItems});
    CHECK(r.sched.refCount(S::CpuLoad) == 1);
    r.run(4);
    CHECK(r.c.cpu > 0);
}

void testTwoInstances() {
    Rig r;
    r.look();
    r.sched.setEnabledWidgets({W::Cpu, W::Cpu});
    CHECK(r.sched.refCount(S::CpuLoad) == 2);
}

void testVisibility() {
    Rig r;
    r.sched.setEnabledWidgets({W::Cpu});

    DemandInputs d;
    d.hudShown = true;
    d.hmdInUse = false;
    r.sched.setDemand(d);
    r.run(4);
    CHECK(r.c.cpu == 0);  // shown but headset in standby

    d.hmdInUse = true;
    r.sched.setDemand(d);
    r.run(4);
    CHECK(r.c.cpu == 4);

    d.hudShown = false;
    r.sched.setDemand(d);
    const int frozen = r.c.cpu;
    r.run(6);
    CHECK(r.c.cpu == frozen);
}

void testSingleSignals() {
    for (int which = 0; which < 3; ++which) {
        Rig r;
        r.sched.setEnabledWidgets({W::Cpu});
        DemandInputs d;
        if (which == 0) d.editorOpen = true;
        if (which == 1) d.pcLease = true;
        if (which == 2) d.record = true;
        r.sched.setDemand(d);
        r.run(4);
        CHECK(r.c.cpu == 4);

        r.sched.setDemand(DemandInputs {});
        const int frozen = r.c.cpu;
        r.run(10);
        CHECK(r.c.cpu == frozen);
    }
}

void testResumed() {
    Rig r;
    r.look();
    r.sched.setEnabledWidgets({W::Cpu});
    r.run(3);
    CHECK(r.c.cpuResumed.size() == 3);
    CHECK(r.c.cpuResumed.size() == 3 && r.c.cpuResumed[0] && !r.c.cpuResumed[1] && !r.c.cpuResumed[2]);

    // pause, then reactivate: first read is resumed again
    r.sched.setDemand(DemandInputs {});
    r.run(4);
    r.look();
    r.c.cpuResumed.clear();
    r.run(3);
    CHECK(r.c.cpuResumed.size() == 3);
    CHECK(r.c.cpuResumed.size() == 3 && r.c.cpuResumed[0] && !r.c.cpuResumed[1] && !r.c.cpuResumed[2]);
}

void testIntervals() {
    Rig r;
    CHECK(std::fabs(r.sched.interval(S::CpuLoad) - 0.5) < 1e-9);  // shortest of 500 / 1000
    CHECK(std::fabs(r.sched.interval(S::GpuClock) - 2.0) < 1e-9);
    CHECK(std::fabs(r.sched.interval(S::Ram) - 1.0) < 1e-9);       // shortest of 2000 / 1000
    CHECK(r.sched.interval(S::Fan) == 0.0);                        // no metric uses it

    CHECK(std::isinf(r.sched.nextDue(0.0)));  // nothing active

    r.look();
    r.sched.setEnabledWidgets({W::GpuRam});
    r.run(8);  // 4 seconds of 500 ms ticks
    CHECK(r.c.gpu == 2);  // reads at t=0 and t=2 only (t=4 not reached)
    CHECK(r.c.ram == 4);  // 1 s interval: t=0,1,2,3
    CHECK(!std::isinf(r.sched.nextDue(r.now)));

    r.sched.setEnabledWidgets({});
    CHECK(std::isinf(r.sched.nextDue(r.now)));
}

void testRecomputeCount() {
    Rig r;
    const uint64_t start = r.sched.recomputeCount();
    DemandInputs d;
    d.editorOpen = true;
    r.sched.setDemand(d);
    CHECK(r.sched.recomputeCount() == start + 1);
    r.sched.setDemand(d);  // identical inputs
    CHECK(r.sched.recomputeCount() == start + 1);
    d.record = true;
    r.sched.setDemand(d);
    CHECK(r.sched.recomputeCount() == start + 2);
}

// ---- Streaming's implicit demand for SourceId::Network: link naming (Dongle / Home Wi-Fi) needs
// net.dongle.ip / net.home.ip even when there's no Network widget on screen at all. The scheduler grants
// this via an extra ownerMask bit on SourceId::Network for WidgetType::Streaming, not a duplicated metric, so
// it doesn't show up in the registry defs below -- it's purely scheduler-side wiring. ----

const std::vector<MetricDef> kStreamingDefs = {
    {20, "net.ip", "", W::Network, S::Network, 2000, WireType::F32},
    {21, "stream.link", "", W::Streaming, S::StreamLink, 1000, WireType::F32},
};

struct StreamingRig {
    Scheduler sched {kStreamingDefs};
    int networkReads = 0, streamReads = 0;
    std::vector<bool> networkResumed;
    double now = 0.0;
    StreamingRig() {
        sched.addSource(S::Network, [this](double, bool resumed) {
            ++networkReads;
            networkResumed.push_back(resumed);
        });
        sched.addSource(S::StreamLink, [this](double, bool) { ++streamReads; });
    }
    void run(int ticks, double stepSec = 1.0) {
        for (int i = 0; i < ticks; ++i) {
            sched.runDue(now);
            now += stepSec;
        }
    }
    void look() {
        DemandInputs d;
        d.editorOpen = true;
        sched.setDemand(d);
    }
};

void testStreamingAloneEnablesNetwork() {
    StreamingRig r;
    r.look();
    r.sched.setEnabledWidgets({W::Streaming});  // no Network widget anywhere
    CHECK(r.sched.refCount(S::Network) == 1);   // Streaming alone still pulls Network in
    CHECK(r.sched.refCount(S::StreamLink) == 1);

    r.run(4);  // Network's interval is 2 s, StreamLink's is 1 s
    CHECK(r.networkReads == 2);
    CHECK(r.streamReads == 4);

    // removing Streaming stops Network too (nothing else demands it in this table)
    r.sched.setEnabledWidgets({});
    CHECK(r.sched.refCount(S::Network) == 0);
    CHECK(r.sched.refCount(S::StreamLink) == 0);
    const int frozenNet = r.networkReads;
    const int frozenStream = r.streamReads;
    r.run(10);
    CHECK(r.networkReads == frozenNet && r.streamReads == frozenStream);
}

void testStreamingNetworkResumeSemanticsUnchanged() {
    StreamingRig r;
    r.look();
    r.sched.setEnabledWidgets({W::Streaming});
    r.run(3);
    CHECK(r.networkResumed.size() == 2);  // 2 s interval over 3 x 1 s ticks: reads at t=0, t=2
    CHECK(r.networkResumed[0] && !r.networkResumed[1]);  // first read since activation is "resumed"

    // pause, then reactivate: the first read after the pause is "resumed" again, same as any other source
    r.sched.setDemand(DemandInputs {});
    r.run(4);
    r.look();
    r.networkResumed.clear();
    r.run(3);
    CHECK(r.networkResumed.size() == 2);
    CHECK(r.networkResumed[0] && !r.networkResumed[1]);
}

void testStreamingRemovalStopsNetworkReads() {
    StreamingRig r;
    r.look();
    r.sched.setEnabledWidgets({W::Streaming});
    r.run(2);
    CHECK(r.networkReads > 0);
    r.sched.setEnabledWidgets({});  // Streaming (the only thing demanding Network here) goes away
    const int frozen = r.networkReads;
    r.run(10);
    CHECK(r.networkReads == frozen);
}

void testTwoNetworkWidgetsPlusStreamingShareOneCallbackPerTick() {
    StreamingRig r;
    r.look();
    // two real Network widget instances, plus Streaming's implicit demand: refs stack (3), but runDue must
    // still invoke the registered callback exactly once per due tick, not once per contributing widget
    r.sched.setEnabledWidgets({W::Network, W::Network, W::Streaming});
    CHECK(r.sched.refCount(S::Network) == 3);
    r.run(4);
    CHECK(r.networkReads == 2);  // same count as the Streaming-alone case: one callback per due tick

    // dropping back to just the two Network widgets keeps it active (refs 2, still > 0)
    r.sched.setEnabledWidgets({W::Network, W::Network});
    CHECK(r.sched.refCount(S::Network) == 2);
    const int before = r.networkReads;
    r.run(4);
    CHECK(r.networkReads == before + 2);  // unaffected: still exactly one callback per due tick
}

// ---- T11: pinned sources read with nobody looking (the crash recorder's kit) ----

void testPinnedSourceReadsWithNobodyLooking() {
    Rig r;  // no widgets, no demand at all
    r.sched.setPinnedSources({S::CpuLoad});
    CHECK(r.sched.refCount(S::CpuLoad) == 1);
    CHECK(r.sched.refCount(S::GpuClock) == 0);
    CHECK(r.sched.refCount(S::Ram) == 0);
    r.run(4);
    CHECK(r.c.cpu == 4);  // its own 500 ms interval
    CHECK(r.c.gpu == 0 && r.c.ram == 0);
    CHECK(r.c.cpuResumed.size() == 4 && r.c.cpuResumed[0] && !r.c.cpuResumed[1]);
    CHECK(!std::isinf(r.sched.nextDue(r.now)));
}

void testUnpinStopsReading() {
    Rig r;
    r.sched.setPinnedSources({S::CpuLoad, S::Ram});
    r.run(4);
    CHECK(r.c.cpu == 4 && r.c.ram == 2);
    r.sched.setPinnedSources({S::Ram});
    CHECK(r.sched.refCount(S::CpuLoad) == 0);
    const int frozen = r.c.cpu;
    r.run(6);
    CHECK(r.c.cpu == frozen);
    CHECK(r.c.ram > 2);
    r.sched.setPinnedSources({});
    CHECK(r.sched.refCount(S::Ram) == 0);
    const int ramFrozen = r.c.ram;
    r.run(6);
    CHECK(r.c.ram == ramFrozen);
    CHECK(std::isinf(r.sched.nextDue(r.now)));
}

void testPinDoesNotMakeAnyoneLooking() {
    Rig r;
    r.sched.setEnabledWidgets({W::Cpu, W::GpuRam});
    r.sched.setPinnedSources({S::CpuLoad});
    CHECK(r.sched.refCount(S::CpuLoad) == 1);  // the pin only; the widget isn't counted while nobody looks
    CHECK(r.sched.refCount(S::GpuClock) == 0);
    CHECK(r.sched.refCount(S::Ram) == 0);
    r.run(4);
    CHECK(r.c.cpu == 4 && r.c.gpu == 0 && r.c.ram == 0);

    r.look();  // now the widgets count, and the pin stacks on top
    CHECK(r.sched.refCount(S::CpuLoad) == 2);
    CHECK(r.sched.refCount(S::GpuClock) == 1);
    r.sched.setPinnedSources({});
    CHECK(r.sched.refCount(S::CpuLoad) == 1);
    r.sched.setDemand(DemandInputs {});
    CHECK(r.sched.refCount(S::CpuLoad) == 0);
}

void testPinRecomputeAndUnusedSource() {
    Rig r;
    const uint64_t start = r.sched.recomputeCount();
    r.sched.setPinnedSources({S::CpuLoad});
    CHECK(r.sched.recomputeCount() == start + 1);
    r.sched.setPinnedSources({S::CpuLoad});  // same set
    CHECK(r.sched.recomputeCount() == start + 1);
    r.sched.setPinnedSources({S::Fan});  // no metric uses it in this table: nothing to read
    CHECK(r.sched.refCount(S::Fan) == 0);
    CHECK(r.sched.refCount(S::CpuLoad) == 0);
}

}  // namespace

int main() {
    testNothingWithoutWidgets();
    testEnableDisable();
    testSharedSource();
    testTwoInstances();
    testVisibility();
    testSingleSignals();
    testResumed();
    testIntervals();
    testRecomputeCount();
    testStreamingAloneEnablesNetwork();
    testStreamingNetworkResumeSemanticsUnchanged();
    testStreamingRemovalStopsNetworkReads();
    testTwoNetworkWidgetsPlusStreamingShareOneCallbackPerTick();
    testPinnedSourceReadsWithNobodyLooking();
    testUnpinStopsReading();
    testPinDoesNotMakeAnyoneLooking();
    testPinRecomputeAndUnusedSource();
    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
