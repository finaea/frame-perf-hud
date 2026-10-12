// pc/streaming/service: lifecycle only (construct, idle take(), stop(), destruct), repeated to stress the
// worker-thread startup ordering the T08 review flagged -- Service's constructor used to launch the worker
// thread in a way that risked it running before the Service's own atomics, mutex, condition variable and
// snapshot vector existed. The fix (default-construct worker_ via its declaration, then launch it in the
// constructor body only after every other member has already initialized) makes that ordering a compile-time
// guarantee, not a race; the review itself couldn't reproduce the race deterministically without intrusive
// hooks ("a live failure was not reproduced"). This file is the next best thing: many construct/destruct
// cycles, so a regression in that ordering (or in stop()'s join, or take()'s locking) has many chances to
// hang or crash instead of none.
//
// Demand stays off throughout every test here. With demand off, Service::run() never calls Runtime::tick()
// (which is the only thing that touches vrserver, OpenVR or the driver_vrlink.txt log); it only waits on its
// condition variable. So none of this touches a real process, SteamVR, or the filesystem.
#include "metric_store.h"
#include "streaming/service.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

using namespace fph;
using namespace fph::pc::streaming;

void testRepeatedConstructTakeStopDestruct() {
    for (int i = 0; i < 50; ++i) {
        Service svc;
        MetricStore store;
        // demand is off: the worker never bumps its generation, so take() never has anything new to apply
        CHECK(!svc.take(store, 0.0));
        CHECK(!svc.take(store, 0.1));
        svc.stop();
        svc.stop(); // idempotent: a second stop() must not hang, block or crash
    } // ~Service() runs here too, every iteration (it calls stop() itself -- see the next test)
}

void testDestructWithoutExplicitStop() {
    // ~Service() must be just as safe as an explicit stop(): it calls stop() itself.
    for (int i = 0; i < 20; ++i) {
        Service svc;
        MetricStore store;
        (void)svc.take(store, 0.0);
    }
    CHECK(true); // reaching here (no hang, no crash) is the assertion
}

void testForceHeaderMismatchConstructDestruct() {
    // the forceHeaderMismatch flag only changes which anchors a (never-reached-here, demand-off) worker would
    // pick once it actually probes vrserver; construction/destruction must be identical either way.
    for (int i = 0; i < 10; ++i) {
        Service svc(true);
        MetricStore store;
        CHECK(!svc.take(store, 0.0));
        svc.stop();
    }
}

void testTakeBeforeAnyStop() {
    // take() must be safe to call on a freshly constructed, still-running Service, repeatedly, before stop()
    Service svc;
    MetricStore store;
    for (int i = 0; i < 20; ++i)
        CHECK(!svc.take(store, static_cast<double>(i)));
    svc.stop();
}

} // namespace

int main() {
    testRepeatedConstructTakeStopDestruct();
    testDestructWithoutExplicitStop();
    testForceHeaderMismatchConstructDestruct();
    testTakeBeforeAnyStop();
    if (failures == 0) {
        std::printf("pc streaming service: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
