// T10 review fixes: the eye search's wake-up uses the same deadlines as its work (no busy spin in the slow backoff),
// and an outward stream's "newest shown" watermark is reset with its buffers (a fresh CAM may restart the sequence).
#include "camera_eye_ring.h"
#include "camera_frame.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using eyering::SearchSchedule;

/** The camera thread's loop around the search, with every discovery failing after `attempt` seconds. */
void testNoBusySpin(double attempt) {
    SearchSchedule s;
    s.reset(0.0);
    double t = 0.0;
    int wakes = 0, notes = 0, scans = 0, zeroTimeouts = 0;
    double lastScanStart = 0.0;
    while (t < 120.0 && wakes < 100000) {
        ++wakes;
        if (s.noteDue(t)) {
            s.noted(t);
            ++notes;
            // in the slow backoff, tracking only runs in the lead-in to the next attempt
            if (s.slow(t)) CHECK(t >= s.scanAt(t) - SearchSchedule::kSlowNoteLeadSec);
        }
        if (s.scanDue(t)) {
            s.scanned(t);
            ++scans;
            lastScanStart = t;
            t += attempt;      // discovery's 1.6 s of checks
            s.scanned(t);      // ... failed: the next attempt counts from its end
        }
        const double wake = s.nextWake(t);
        // poll(timeout 0) is right when work is due (e.g. tracking after a long attempt); with nothing due it's a spin
        if (wake <= t && !s.noteDue(t) && !s.scanDue(t)) ++zeroTimeouts;
        t = wake > t ? wake : t;
    }
    (void)lastScanStart;
    CHECK(zeroTimeouts == 0);
    // 15 s fast (about one attempt per 1 s + attempt), then one per 10 s: a handful of wakes per attempt, not thousands
    CHECK(wakes < 400);
    CHECK(scans > 5 && scans < 30);
    CHECK(notes > 0);
}

/** The reviewer's probe: slow mode, last attempt failed at t = 20: nothing is due until 28.75 (= 30 - 1.25). */
void testReviewerProbe() {
    SearchSchedule s;
    s.reset(0.0);
    s.scanned(20.0);
    s.noted(19.0);
    for (double t : {21.0, 25.0, 28.0}) {
        CHECK(s.slow(t));
        CHECK(!s.noteDue(t));
        CHECK(!s.scanDue(t));
        CHECK(s.nextWake(t) == 28.75);  // the old code woke with timeout 0 here
    }
    CHECK(s.noteDue(28.75));
    CHECK(!s.scanDue(28.75));
    CHECK(s.scanDue(30.0));
    // fast mode (a lock was just lost): 250 ms tracking, 1 s attempts
    s.restart(40.0);
    s.scanned(40.0);
    s.noted(40.0);
    CHECK(!s.slow(40.5));
    CHECK(s.nextWake(40.0) == 40.25);
    CHECK(s.scanAt(40.0) == 41.0);
}

void testWatermarkResetsWithTheStream() {
    fph::cam::DarkParityFilter dark;  // unknown: nothing is dark
    fph::cam::ShownWatermark w;
    w.shown(100000);  // the old stream got this far
    const std::vector<fph::cam::FrameEntry> restarted = {{3, 10}, {4, 11}};
    CHECK(fph::cam::pickFrame(restarted, dark, w) == -1);  // looks old against the old stream's watermark
    w.reset();  // what a fresh CAM / CAM_GONE does
    CHECK(!w.have());
    CHECK(fph::cam::pickFrame(restarted, dark, w) == 1);  // the new stream's newest frame shows at once
    w.shown(11);
    CHECK(fph::cam::pickFrame(restarted, dark, w) == -1);  // and still never twice
    const std::vector<fph::cam::FrameEntry> next = {{5, 12}};
    CHECK(fph::cam::pickFrame(next, dark, w) == 0);
}

}  // namespace

int main() {
    testNoBusySpin(1.6);
    testNoBusySpin(0.0);
    testReviewerProbe();
    testWatermarkResetsWithTheStream();
    if (failures) {
        std::printf("camera-review-fixes-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("camera-review-fixes-test: all passed\n");
    return 0;
}
