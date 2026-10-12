// pc/streaming/reader: driver_vrlink.txt event classification (parseLogEvent) and the streaming tail parser
// (LogParser): chunked feed() across line boundaries, a huge line with no newline (capped at 4096 bytes,
// never grows without bound, never crashes), and rewritten() (SteamVR truncates/recreates the log on its
// own start: the pending partial line is dropped so it can't glue onto the new file's content, but the
// cumulative counts survive, the same convention as the headset's VrlinkCounts). Event text is exactly what
// the tech plan names: "Reset video stream", "Bad link event", "Finished encoder reinit", "THROTTLE EVENT"
// (pc-app data source 3 / streaming-stats-spike).
#include "stream_metrics.h"
#include "streaming/reader.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph::pc::streaming;

bool allZero(const LogEvents& e) {
    return e.resets == 0 && e.badLinks == 0 && e.encoderReinits == 0 && e.throttles == 0;
}

void feed(LogParser& p, const std::string& s) {
    p.feed(s.data(), s.size());
}

// ---- parseLogEvent: one line at a time ----

void testParseLogEvent() {
    LogEvents e;
    CHECK(parseLogEvent("2026-10-11 19:08:03 - Reset video stream (requested by peer)", e));
    CHECK(e.resets == 1 && e.badLinks == 0 && e.encoderReinits == 0 && e.throttles == 0);

    CHECK(parseLogEvent("2026-10-11 19:08:04 - Bad link event on 10.35.78.1", e));
    CHECK(e.badLinks == 1);

    CHECK(parseLogEvent("Finished encoder reinit", e));
    CHECK(e.encoderReinits == 1);

    CHECK(parseLogEvent("2026-10-11 19:09:00 - THROTTLE EVENT: link saturated", e));
    CHECK(e.throttles == 1);

    // every field accumulates independently across repeated calls on the same LogEvents
    CHECK(parseLogEvent("- Reset video stream", e));
    CHECK(e.resets == 2);
    CHECK(e.badLinks == 1 && e.encoderReinits == 1 && e.throttles == 1);

    // lines that don't carry one of the four phrases are ignored, and don't touch the counts
    const LogEvents before = e;
    CHECK(!parseLogEvent("2026-10-11 19:09:01 - SVLDataLink::HandleFECChange( 32 )", e));
    CHECK(e.resets == before.resets && e.badLinks == before.badLinks && e.encoderReinits == before.encoderReinits &&
          e.throttles == before.throttles);
    CHECK(!parseLogEvent("", e));
    CHECK(!parseLogEvent("Link BW: Est 232.0", e));

    // only the first matching phrase counts per line (they never appear combined in the real log, but the
    // classifier must still make a definite choice rather than double-count)
    LogEvents fresh;
    CHECK(parseLogEvent("Reset video stream", fresh) && fresh.resets == 1 && fresh.badLinks == 0);
}

// ---- LogParser: chunked feed() ----

void testFeedWholeLines() {
    LogParser p;
    feed(p,
         "2026-10-11 19:08:03 - Reset video stream\n"
         "2026-10-11 19:08:04 - Bad link event\n"
         "2026-10-11 19:08:05 - Finished encoder reinit\n"
         "2026-10-11 19:08:06 - THROTTLE EVENT\n");
    CHECK(p.events().resets == 1 && p.events().badLinks == 1 && p.events().encoderReinits == 1 &&
          p.events().throttles == 1);
}

void testFeedSplitAcrossCalls() {
    // the same line, split at every possible byte boundary across two feed() calls
    const std::string line = "2026-10-11 19:08:03 - Reset video stream\n";
    for (size_t cut = 1; cut < line.size() - 1; ++cut) {
        LogParser q;
        q.feed(line.data(), cut);
        CHECK(allZero(q.events()));  // not counted until the newline arrives
        q.feed(line.data() + cut, line.size() - cut);
        CHECK(q.events().resets == 1);
    }

    // split across three calls, with a second, different line starting in the same chunk that completes it
    LogParser p;
    feed(p, "2026-10-11 19:08:03 - Re");
    CHECK(allZero(p.events()));
    feed(p, "set video stream\n2026-10-11 19:08:04 - Bad ");
    CHECK(p.events().resets == 1 && p.events().badLinks == 0);  // second line still incomplete
    feed(p, "link event\n");
    CHECK(p.events().resets == 1 && p.events().badLinks == 1);
}

void testFeedTrailingPartialNotCounted() {
    LogParser p;
    feed(p, "2026-10-11 19:08:03 - Reset video stream");  // no trailing '\n' yet
    CHECK(allZero(p.events()));
    feed(p, "\n");
    CHECK(p.events().resets == 1);
}

void testFeedCrlf() {
    // driver_vrlink.txt is a Windows text file: \r\n line endings must not break the match (the phrase is
    // found by substring search, so a trailing \r left in the line is harmless either way)
    LogParser p;
    feed(p, "2026-10-11 19:08:03 - Reset video stream\r\n2026-10-11 19:08:04 - Bad link event\r\n");
    CHECK(p.events().resets == 1 && p.events().badLinks == 1);
}

void testFeedManyNoiseLines() {
    // feed() must not get confused by a long run of irrelevant lines, and still catch a real event right
    // after them
    LogParser p;
    std::string chunk;
    for (int i = 0; i < 50; ++i) chunk += "- SVLDataLink::HandleFECChange( " + std::to_string(i) + " )\n";
    chunk += "- Finished encoder reinit\n";
    feed(p, chunk);
    CHECK(p.events().encoderReinits == 1 && p.events().resets == 0 && p.events().badLinks == 0);
}

// ---- LogParser: a huge line with no newline never grows memory without bound, and self-heals ----

void testFeedHugeLineIsSafe() {
    LogParser p;
    // well past any reasonable single-line cap, fed in large chunks, with no newline at all
    const std::string junkChunk(1 << 16, 'x');
    for (int i = 0; i < 64; ++i) feed(p, junkChunk);  // 4 MiB, still no '\n'
    CHECK(allZero(p.events()));  // nothing matched (and nothing crashed getting here)

    // a real event line right after: the parser must still recognize it once a newline finally arrives, even
    // though it spent the whole run above dropping the oversized fragment
    feed(p, "\n2026-10-11 19:10:00 - Reset video stream\n");
    CHECK(p.events().resets == 1);
}

// ---- rewritten(): SteamVR recreates the log on its own start ----

void testRewritten() {
    LogParser p;
    feed(p, "2026-10-11 19:08:03 - Reset video stream\n");
    CHECK(p.events().resets == 1);

    // a dangling fragment (no newline yet) right before the file is recreated must not glue onto whatever
    // comes after reopening from byte 0 of the new file
    feed(p, "2026-10-11 19:08:05 - Bad li");
    p.rewritten();
    feed(p, "nk event\n");  // if the old fragment had survived, this would complete a (wrong) bad-link line
    CHECK(p.events().badLinks == 0);

    // counts accumulated before the rewrite are not thrown away by it (same convention as the headset's
    // VrlinkCounts: cumulative since the agent started watching, not reset on a reconnect)
    CHECK(p.events().resets == 1);

    // the new file's own lines are read normally after the reset, adding onto the same counters
    feed(p, "2026-10-11 20:00:00 - Finished encoder reinit\n");
    CHECK(p.events().encoderReinits == 1);
    CHECK(p.events().resets == 1);
}

}  // namespace

int main() {
    testParseLogEvent();
    testFeedWholeLines();
    testFeedSplitAcrossCalls();
    testFeedTrailingPartialNotCounted();
    testFeedCrlf();
    testFeedManyNoiseLines();
    testFeedHugeLineIsSafe();
    testRewritten();
    if (failures == 0) {
        std::printf("pc streaming log: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
