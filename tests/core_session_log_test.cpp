// core/session_log tests (T11): session following + detour merge, timeline downsampling and cap, histogram, column stats,
// recent ring, summary / CSV / gzip, files, Record, end card. No platform code; links fph-session (zlib).
#include "json.h"
#include "session_log.h"
#include "stream_metrics.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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
using namespace fph::slog;
namespace fs = std::filesystem;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool near(double a, double b, double eps = 1e-3) {
    return std::fabs(a - b) <= eps;
}

SessionInfo info(int32_t id, const std::string& key, const std::string& name, int64_t start) {
    SessionInfo s;
    s.id = id;
    s.appKey = key;
    s.appName = name;
    s.startUnix = start;
    return s;
}

/** fps gauge, dropped count, throttle flag, resets counter: ids from the registry so names resolve. */
std::vector<LogColumn> smallColumns() {
    return {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kFrameDropped, LogKind::Count, 0, 0, 0, false},
        {metric::kFrameThrottled, LogKind::Flag, 0, 0, 0, false},
        {metric::kPcStreamStreamResets, LogKind::Cumulative, 0, 0, 0, false},
    };
}

std::vector<float> sample(float fps) {
    return {fps, 0.0f, 0.0f, 0.0f};
}

/** Feeds seconds [from, from + count) of one session: observe then add, once a second like the agent. */
void run(SessionLog& log, const SessionInfo& s, int64_t from, int count, float fps = 60.0f) {
    for (int64_t t = from; t < from + count; ++t) {
        log.observe(s, t);
        log.add(t, sample(fps));
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** @return the gunzipped text, "<fail>" if zlib refuses */
std::string gunzip(const std::string& gz) {
    z_stream zs {};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return "<fail>";
    std::string out;
    char buffer[16384];
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gz.data()));
    zs.avail_in = static_cast<uInt>(gz.size());
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        rc = inflate(&zs, Z_NO_FLUSH);
        out.append(buffer, sizeof(buffer) - zs.avail_out);
    }
    inflateEnd(&zs);
    return rc == Z_STREAM_END ? out : "<fail>";
}

int countLines(const std::string& text) {
    int n = 0;
    for (char c : text) n += c == '\n';
    return n;
}

/** A fresh empty directory under the system temp dir. */
std::string tempDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path p = fs::temp_directory_path() / (std::string("fph-slog-") + tag + "-" + std::to_string(stamp));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

size_t fileCount(const std::string& dir) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

// ---- SessionLog ----

void detourMergesIntoOneSession() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    const SessionInfo home = info(2, "steam.app.home", "Home", 1120);
    run(log, game, 1000, 120);
    run(log, home, 1120, 30);  // Home for 30 s
    CHECK(log.takeEnded().empty());
    run(log, game, 1150, 100);  // back: the tracker resumed id 1
    CHECK(log.takeEnded().empty());
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (ended.size() != 1) return;
    const SessionData& s = *ended[0];
    CHECK(s.info.id == 1);
    CHECK(s.info.startUnix == 1000);
    CHECK(s.dataSeconds == 250);
    CHECK(s.stats[0].samples == 250);
    CHECK(s.hist[0].total() == 250);
    CHECK(s.timeline.rows() == 250);
    CHECK(s.lastUnix == 1249);
    // offsets stay increasing and the detour sits where it happened (120..149)
    bool increasing = true;
    for (size_t i = 1; i < s.timeline.rows(); ++i) {
        if (s.timeline.row(i).offset <= s.timeline.row(i - 1).offset) increasing = false;
    }
    CHECK(increasing);
    CHECK(s.timeline.row(120).offset == 120);
    CHECK(s.timeline.row(249).offset == 249);
}

void twoGamesAreTwoSessions() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo a = info(1, "steam.app.1", "A", 1000);
    const SessionInfo b = info(2, "steam.app.2", "B", 1120);
    run(log, a, 1000, 120);
    run(log, b, 1120, 30);
    CHECK(log.takeEnded().empty());  // A is parked: B hasn't run 60 s yet
    run(log, b, 1150, 60);           // B reaches 60 s at 1180: A is final
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (!ended.empty()) {
        CHECK(ended[0]->info.id == 1);
        CHECK(ended[0]->dataSeconds == 120);
    }
    log.endAll();
    ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (!ended.empty()) {
        CHECK(ended[0]->info.id == 2);
        CHECK(ended[0]->dataSeconds == 90);
    }
}

void endAllEndsParkedThenCurrent() {
    SessionLog log(smallColumns(), "headset");
    run(log, info(1, "k1", "A", 1000), 1000, 120);
    run(log, info(2, "k2", "B", 1120), 1120, 90);  // A ended at 1180 already
    run(log, info(3, "k3", "C", 1210), 1210, 10);  // B parked, C current
    log.takeEnded();                               // drops A
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 2);
    if (ended.size() == 2) {
        CHECK(ended[0]->info.id == 2);  // the parked one first
        CHECK(ended[1]->info.id == 3);
    }
    CHECK(log.current() == nullptr);
    CHECK(log.takeEnded().empty());
}

void shortGameHasLittleData() {
    SessionLog log(smallColumns(), "headset");
    run(log, info(1, "k1", "A", 1000), 1000, 30);
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (!ended.empty()) {
        CHECK(ended[0]->dataSeconds == 30);
        CHECK(ended[0]->dataSeconds < kMinSessionSec);
    }
}

void allNanSamplesAreNotCounted() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo a = info(1, "k1", "A", 1000);
    log.observe(a, 1000);
    for (int t = 1000; t < 1030; ++t) log.add(t, {kNaN, kNaN, kNaN, kNaN});  // HUD hidden
    CHECK(log.current() != nullptr && log.current()->dataSeconds == 0);
    CHECK(log.current() != nullptr && log.current()->timeline.rows() == 0);
    // a sample with one value counts
    log.add(1030, {kNaN, 1.0f, kNaN, kNaN});
    CHECK(log.current()->dataSeconds == 1);
    CHECK(log.current()->stats[0].samples == 0);
    CHECK(log.current()->stats[1].samples == 1);
    CHECK(!anyValue({kNaN, kNaN}));
    CHECK(anyValue({kNaN, 0.0f}));
}

void noSessionIgnoresSamples() {
    SessionLog log(smallColumns(), "headset");
    log.observe(SessionInfo(), 1000);  // id 0
    log.add(1000, sample(60));
    CHECK(log.current() == nullptr);
    log.endAll();
    CHECK(log.takeEnded().empty());
}

void repeatedSecondIsIgnored() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo a = info(1, "k1", "A", 1000);
    log.observe(a, 1000);
    log.add(1000, sample(60));
    log.add(1000, sample(60));
    log.add(999, sample(60));
    CHECK(log.current()->dataSeconds == 1);
}

void nameFilledInLater() {
    SessionLog log(smallColumns(), "headset");
    log.observe(info(1, "k1", "", 1000), 1000);
    log.observe(info(1, "k1", "Name", 1000), 1001);
    CHECK(log.current()->info.appName == "Name");
}

// ---- Timeline ----

void timelineFullRateThenFiveSecondRows() {
    const std::vector<LogKind> kinds = {LogKind::Gauge, LogKind::Count, LogKind::Cumulative, LogKind::Flag};
    Timeline t(kinds);
    for (int64_t o = 0; o < kFullRateSec; ++o) {
        t.add(o, {1.0f, 1.0f, static_cast<float>(o), 0.0f});
    }
    CHECK(t.rows() == static_cast<size_t>(kFullRateSec));
    CHECK(t.step() == 1);
    CHECK(t.row(0).duration == 1);
    CHECK(t.row(100).offset == 100);

    for (int64_t k = 0; k < 10; ++k) {
        const int64_t o = kFullRateSec + k;
        t.add(o, {static_cast<float>(k), 1.0f, static_cast<float>(o), o == kFullRateSec + 2 ? 1.0f : 0.0f});
    }
    CHECK(t.step() == kDownsampledStepSec);
    CHECK(t.rows() == static_cast<size_t>(kFullRateSec) + 2);
    if (t.rows() == static_cast<size_t>(kFullRateSec) + 2) {
        const TimelineRow a = t.row(static_cast<size_t>(kFullRateSec));
        CHECK(a.offset == kFullRateSec);
        CHECK(a.duration == 5);
        CHECK(near(a.values[0], 2.0));                          // Gauge: avg of 0..4
        CHECK(near(a.values[1], 5.0));                          // Count: sum
        CHECK(near(a.values[2], kFullRateSec + 4));             // Cumulative: last
        CHECK(near(a.values[3], 1.0));                          // Flag: max
        const TimelineRow b = t.row(static_cast<size_t>(kFullRateSec) + 1);
        CHECK(b.offset == kFullRateSec + 5);
        CHECK(near(b.values[0], 7.0));
        CHECK(near(b.values[1], 5.0));
        CHECK(near(b.values[2], kFullRateSec + 9));
        CHECK(near(b.values[3], 0.0));
    }

    // a partial row shows up only after flush()
    for (int64_t k = 10; k < 13; ++k) t.add(kFullRateSec + k, {10.0f, 1.0f, 0.0f, 0.0f});
    CHECK(t.rows() == static_cast<size_t>(kFullRateSec) + 2);
    t.flush();
    CHECK(t.rows() == static_cast<size_t>(kFullRateSec) + 3);
    CHECK(t.row(t.rows() - 1).duration == 3);
    CHECK(near(t.row(t.rows() - 1).values[1], 3.0));
}

void timelineIgnoresRepeatedOffset() {
    Timeline t({LogKind::Gauge});
    t.add(0, {1.0f});
    t.add(0, {2.0f});
    t.add(1, {3.0f});
    t.add(1, {kNaN, kNaN});  // wrong width
    CHECK(t.rows() == 2);
    CHECK(near(t.row(0).values[0], 1.0));
}

void timelineCapHalves() {
    // 1 column: 3 floats (12 bytes) a row; 96 bytes = 8 rows
    Timeline t({LogKind::Gauge}, 96);
    for (int i = 0; i < 8; ++i) t.add(i, {static_cast<float>(i)});
    CHECK(t.rows() == 8);
    CHECK(t.step() == 1);
    t.add(8, {8.0f});  // 9th row passes the cap: rows merge pairwise
    CHECK(t.step() == 2);
    CHECK(t.rows() == 5);
    CHECK(t.bytes() <= 96 + 12);
    if (t.rows() == 5) {
        CHECK(t.row(0).offset == 0 && t.row(0).duration == 2 && near(t.row(0).values[0], 0.5));
        CHECK(t.row(1).offset == 2 && t.row(1).duration == 2 && near(t.row(1).values[0], 2.5));
        CHECK(t.row(3).offset == 6 && near(t.row(3).values[0], 6.5));
        CHECK(t.row(4).offset == 8 && t.row(4).duration == 1 && near(t.row(4).values[0], 8.0));
    }
    // it keeps taking samples and stays under the cap
    for (int i = 9; i < 400; ++i) {
        t.add(i, {static_cast<float>(i)});
        if (t.bytes() > 96 + 12) {
            CHECK(t.bytes() <= 96 + 12);
            break;
        }
    }
    CHECK(t.step() >= 4);
}

void timelineHalveMergesByKind() {
    const std::vector<LogKind> kinds = {LogKind::Gauge, LogKind::Count, LogKind::Cumulative, LogKind::Flag};
    Timeline t(kinds, 8 * 6 * sizeof(float));  // 8 rows of (2 + 4) floats
    for (int i = 0; i < 9; ++i) {
        t.add(i, {static_cast<float>(i), 1.0f, static_cast<float>(10 * i), i == 1 ? 1.0f : 0.0f});
    }
    CHECK(t.step() == 2);
    CHECK(t.rows() == 5);
    if (t.rows() == 5) {
        const TimelineRow r = t.row(0);  // merged rows 0 and 1
        CHECK(near(r.values[0], 0.5));
        CHECK(near(r.values[1], 2.0));   // Count sums
        CHECK(near(r.values[2], 10.0));  // Cumulative keeps the later one
        CHECK(near(r.values[3], 1.0));   // Flag max
        CHECK(near(t.row(1).values[3], 0.0));
    }
    // a NaN on one side takes the other side
    Timeline u({LogKind::Gauge}, 96);
    for (int i = 0; i < 9; ++i) u.add(i, {i == 0 ? kNaN : static_cast<float>(i)});
    CHECK(u.rows() == 5);
    if (u.rows() == 5) CHECK(near(u.row(0).values[0], 1.0));
}

void timelineTenHoursStaysUnderCap() {
    const std::vector<LogColumn>& cols = sessionLogColumns();
    std::vector<LogKind> kinds;
    for (const LogColumn& c : cols) kinds.push_back(c.kind);
    Timeline t(kinds);  // kTimelineCapBytes
    std::vector<float> values(cols.size(), 1.0f);
    const size_t rowBytes = (cols.size() + 2) * sizeof(float);
    size_t worst = 0;
    for (int64_t o = 0; o < 10 * 3600; ++o) {
        for (size_t c = 0; c < values.size(); ++c) values[c] = static_cast<float>(o % 97) + static_cast<float>(c);
        t.add(o, values);
        worst = std::max(worst, t.bytes());
    }
    t.flush();
    CHECK(worst <= kTimelineCapBytes + rowBytes);
    CHECK(t.bytes() <= kTimelineCapBytes + rowBytes);
    CHECK(t.bytes() < 4400000);
    CHECK(t.step() > kDownsampledStepSec);  // 10 h didn't fit at 5 s: it halved
    // it still spans the whole session
    CHECK(t.rows() > 1000);
    const TimelineRow last = t.row(t.rows() - 1);
    CHECK(last.offset + last.duration >= 10 * 3600 - 20);
}

// ---- Histogram ----

void histogramPercentiles() {
    Histogram h(0, 100, 100);
    for (int v = 0; v < 100; ++v) h.add(static_cast<float>(v));
    CHECK(h.total() == 100);
    CHECK(near(h.percentile(0.95), 95.0, 1.0));
    CHECK(near(h.percentile(0.05), 5.0, 1.0));
    CHECK(near(h.percentile(0.5), 50.0, 1.0));
    CHECK(h.percentile(1.0) <= 100.0);
    CHECK(h.percentile(0.0) >= 0.0);
}

void histogramClampsAndEmpty() {
    Histogram empty(0, 10, 10);
    CHECK(std::isnan(empty.percentile(0.95)));
    Histogram h(0, 10, 10);
    h.add(kNaN);  // ignored
    CHECK(h.total() == 0);
    h.add(-50.0f);
    CHECK(h.total() == 1);
    CHECK(h.counts()[0] == 1);  // below min lands in the first bin
    CHECK(h.percentile(0.95) >= 0.0 && h.percentile(0.95) <= 1.0);
    Histogram g(0, 10, 10);
    g.add(500.0f);
    CHECK(g.counts()[9] == 1);  // above max lands in the last bin
    CHECK(g.percentile(0.5) >= 9.0 && g.percentile(0.5) <= 10.0);
    CHECK(g.percentile(5.0) <= 10.0);  // p > 1 is clamped
    // merge adds counts, a different bin count is refused
    h.merge(g);
    CHECK(h.total() == 2);
    Histogram other(0, 10, 5);
    h.merge(other);
    CHECK(h.total() == 2);
}

// ---- ColumnStats ----

void statsGauge() {
    ColumnStats s;
    s.add(LogKind::Gauge, 10);
    s.add(LogKind::Gauge, 30);
    s.add(LogKind::Gauge, 20);
    CHECK(s.samples == 3);
    CHECK(near(s.avg(), 20));
    CHECK(s.min == 10 && s.max == 30);
    CHECK(s.first == 10 && s.last == 20);
    CHECK(ColumnStats().avg() == 0.0);
}

void statsCountCumulativeFlag() {
    ColumnStats c;
    for (float v : {1.0f, 2.0f, 3.0f}) c.add(LogKind::Count, v);
    CHECK(near(c.counted, 6));

    ColumnStats u;  // 10 -> 15 (+5), reset to 3 (+3), 5 (+2)
    for (float v : {10.0f, 15.0f, 3.0f, 5.0f}) u.add(LogKind::Cumulative, v);
    CHECK(near(u.counted, 10));
    ColumnStats steady;
    for (float v : {7.0f, 7.0f, 9.0f}) steady.add(LogKind::Cumulative, v);
    CHECK(near(steady.counted, 2));  // the first value is the baseline, not counted

    ColumnStats f;  // 0 1 1 0 1 -> two rises
    for (float v : {0.0f, 1.0f, 1.0f, 0.0f, 1.0f}) f.add(LogKind::Flag, v);
    CHECK(near(f.counted, 2));
    CHECK(f.high);
    ColumnStats f2;  // starts high: that's a rise
    f2.add(LogKind::Flag, 1.0f);
    CHECK(near(f2.counted, 1));
}

void statsMerge() {
    ColumnStats a, b;
    for (float v : {10.0f, 15.0f}) a.add(LogKind::Cumulative, v);  // counted 5, last 15
    for (float v : {20.0f, 25.0f}) b.add(LogKind::Cumulative, v);  // counted 5, first 20
    a.merge(LogKind::Cumulative, b);
    CHECK(near(a.counted, 5 + 5 + 5));  // 15 -> 20 between them is counted too
    CHECK(a.samples == 4 && a.last == 25 && a.max == 25 && a.min == 10);

    ColumnStats c, d;
    c.add(LogKind::Cumulative, 50.0f);
    d.add(LogKind::Cumulative, 4.0f);  // the counter restarted in between
    c.merge(LogKind::Cumulative, d);
    CHECK(near(c.counted, 4));

    ColumnStats e, g;
    e.add(LogKind::Count, 3.0f);
    g.add(LogKind::Count, 4.0f);
    e.merge(LogKind::Count, g);
    CHECK(near(e.counted, 7));
    ColumnStats none;
    none.merge(LogKind::Gauge, g);  // merging into an empty one copies
    CHECK(none.samples == 1);
    e.merge(LogKind::Count, ColumnStats());  // merging an empty one changes nothing
    CHECK(e.samples == 2);
}

// ---- RecentRing ----

void recentRingWraps() {
    RecentRing ring(2);  // 600
    for (int i = 0; i < 650; ++i) ring.add(1000 + i, {static_cast<float>(i), 0.0f});
    CHECK(ring.size() == 600);
    int64_t start = 0;
    const Timeline t = ring.timeline({LogKind::Gauge, LogKind::Gauge}, start, 1649);
    CHECK(start == 1050);
    CHECK(t.rows() == 600);
    if (t.rows() == 600) {
        CHECK(t.row(0).offset == 0 && near(t.row(0).values[0], 50));
        CHECK(t.row(1).offset == 1 && near(t.row(1).values[0], 51));
        CHECK(t.row(599).offset == 599 && near(t.row(599).values[0], 649));
    }
    // a repeated or older second is ignored; a wrong width too
    ring.add(1649, {1.0f, 1.0f});
    ring.add(1000, {1.0f, 1.0f});
    ring.add(2000, {1.0f});
    CHECK(ring.size() == 600);

    RecentRing small(1, 5);
    for (int i = 0; i < 7; ++i) small.add(100 + i, {static_cast<float>(i)});
    CHECK(small.size() == 5);
    const Timeline s = small.timeline({LogKind::Gauge}, start, 106);
    CHECK(start == 102);
    CHECK(s.rows() == 5 && near(s.row(0).values[0], 2) && near(s.row(4).values[0], 6));

    RecentRing empty(1, 5);
    CHECK(empty.size() == 0);
    const Timeline e = empty.timeline({LogKind::Gauge}, start, 106);
    CHECK(e.rows() == 0 && start == 106);  // an empty window starts at the Save time
}

// ---- summary / csv / gzip ----

std::unique_ptr<SessionData> sessionWithData(const std::vector<LogColumn>& cols) {
    SessionLog log(cols, "headset");
    const SessionInfo a = info(7, "steam.app.438100", "VR \"Chat\"", 1000000);
    for (int64_t t = 1000000; t < 1000100; ++t) {
        const float fps = 60.0f + static_cast<float>(t % 10);
        log.observe(a, t);
        log.add(t, {fps, (t % 40 == 0) ? 1.0f : 0.0f, (t >= 1000050 && t < 1000060) ? 1.0f : 0.0f, kNaN});
    }
    log.endAll();
    auto ended = log.takeEnded();
    return ended.empty() ? nullptr : std::move(ended[0]);
}

void summaryJsonParses() {
    const std::vector<LogColumn> cols = smallColumns();
    const auto s = sessionWithData(cols);
    CHECK(s != nullptr);
    if (!s) return;
    const std::string text = summaryJson(*s, cols);
    JsonValue root;
    std::string error;
    CHECK(parseJson(text, root, error));
    if (!root.isObject()) return;
    const JsonValue* v = root.get("format");
    CHECK(v != nullptr && v->isString());
    v = root.get("app_name");
    CHECK(v != nullptr && v->isString() && v->text == "VR \"Chat\"");  // escaping round-trips
    v = root.get("app_key");
    CHECK(v != nullptr && v->text == "steam.app.438100");
    v = root.get("source");
    CHECK(v != nullptr && v->text == "headset");
    v = root.get("session_id");
    CHECK(v != nullptr && near(v->number, 7));
    v = root.get("data_s");
    CHECK(v != nullptr && near(v->number, 100));
    v = root.get("start_unix");
    CHECK(v != nullptr && near(v->number, 1000000));
    v = root.get("duration_s");
    CHECK(v != nullptr && near(v->number, 99));
    v = root.get("card");
    CHECK(v != nullptr && !v->items.empty());
    const JsonValue* metrics = root.get("metrics");
    CHECK(metrics != nullptr && metrics->isObject());
    if (metrics != nullptr) {
        const JsonValue* fps = metrics->get(findMetric(metric::kFrameAppFps)->name);
        CHECK(fps != nullptr && fps->isObject());
        if (fps != nullptr) {
            CHECK(fps->get("avg") != nullptr && near(fps->get("avg")->number, 64.5, 0.05));
            CHECK(fps->get("min") != nullptr && near(fps->get("min")->number, 60));
            CHECK(fps->get("max") != nullptr && near(fps->get("max")->number, 69));
            CHECK(fps->get("p95") != nullptr && fps->get("p95")->number >= 60 && fps->get("p95")->number <= 70);
            CHECK(fps->get("p5") != nullptr);
            CHECK(fps->get("samples") != nullptr && near(fps->get("samples")->number, 100));
        }
        const JsonValue* dropped = metrics->get(findMetric(metric::kFrameDropped)->name);
        CHECK(dropped != nullptr && dropped->get("total") != nullptr && near(dropped->get("total")->number, 3));
        const JsonValue* thr = metrics->get(findMetric(metric::kFrameThrottled)->name);
        CHECK(thr != nullptr && thr->get("events") != nullptr && near(thr->get("events")->number, 1));
        CHECK(thr != nullptr && thr->get("share_pct") != nullptr && near(thr->get("share_pct")->number, 10.0, 0.1));
        // the all-NaN column has no entry
        CHECK(metrics->get(findMetric(metric::kPcStreamStreamResets)->name) == nullptr);
    }
    const JsonValue* hist = root.get("histograms");
    CHECK(hist != nullptr && hist->isObject());
    if (hist != nullptr) {
        const JsonValue* h = hist->get(findMetric(metric::kFrameAppFps)->name);
        CHECK(h != nullptr && h->get("bins") != nullptr && !h->get("bins")->items.empty());
        CHECK(h != nullptr && h->get("bin_count") != nullptr && near(h->get("bin_count")->number, 150));
    }
}

void timelineCsvDropsEmptyColumns() {
    const std::vector<LogColumn> cols = smallColumns();
    Timeline t({LogKind::Gauge, LogKind::Count, LogKind::Flag, LogKind::Cumulative});
    t.add(0, {5.0f, kNaN, 1.0f, kNaN});
    t.add(1, {7.0f, kNaN, 0.0f, kNaN});
    const std::string csv = timelineCsv(t, cols, 5000);
    const size_t eol = csv.find('\n');
    const std::string header = csv.substr(0, eol);
    const std::string expected = std::string("unix,t_s,dur_s,") + findMetric(metric::kFrameAppFps)->name + "," +
                                 findMetric(metric::kFrameThrottled)->name;
    CHECK(header == expected);
    CHECK(csv.find(findMetric(metric::kFrameDropped)->name) == std::string::npos);
    CHECK(csv.find("\n5000,0,1,5,1\n") != std::string::npos);
    CHECK(csv.find("\n5001,1,1,7,0\n") != std::string::npos);
    CHECK(countLines(csv) == 3);
}

void gzipRoundTrips() {
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "1234," + std::to_string(i) + ",60.5\n";
    const std::string gz = gzip(text);
    CHECK(!gz.empty());
    CHECK(gz.size() < text.size());
    CHECK(gz.size() > 2 && static_cast<unsigned char>(gz[0]) == 0x1f && static_cast<unsigned char>(gz[1]) == 0x8b);
    CHECK(gunzip(gz) == text);
    CHECK(gunzip(gzip("")) == "");
}

void fileNamesAndTimes() {
    CHECK(fileTime(0).size() == 17);  // YYYY-MM-DD_HHMMSS
    CHECK(fileTime(0)[4] == '-' && fileTime(0)[10] == '_');
    CHECK(clockTime(0).size() == 8 && clockTime(0)[2] == ':');
    CHECK(fileSafeApp(info(1, "k", "VR Chat: the game!", 0)) == "VR-Chat-the-game");
    CHECK(fileSafeApp(info(1, "steam.app.1", "", 0)) == "steam-app-1");  // '.' never survives
    CHECK(fileSafeApp(info(1, "", "", 0)) == "app");
    CHECK(fileSafeApp(info(1, "", std::string(100, 'x'), 0)).size() == 40);
}

// ---- files ----

void writeSessionAtomicAndPrunes() {
    const std::string dir = tempDir("write");
    const std::vector<LogColumn> cols = smallColumns();
    const auto s = sessionWithData(cols);
    CHECK(s != nullptr);
    if (!s) return;
    std::string error;
    CHECK(writeSession(dir, "2026-10-17_120000_Game", *s, cols, 10, error));
    CHECK(error.empty());
    CHECK(fs::exists(fs::path(dir) / "2026-10-17_120000_Game.json"));
    CHECK(fs::exists(fs::path(dir) / "2026-10-17_120000_Game.csv.gz"));
    CHECK(fileCount(dir) == 2);  // no .tmp left behind
    JsonValue root;
    std::string parseError;
    CHECK(parseJson(readFile(dir + "/2026-10-17_120000_Game.json"), root, parseError));
    const std::string csv = gunzip(readFile(dir + "/2026-10-17_120000_Game.csv.gz"));
    CHECK(csv.rfind("unix,t_s,dur_s,", 0) == 0);
    CHECK(countLines(csv) == 101);  // header + 100 rows

    // overwrite the same stem: still two files
    CHECK(writeSession(dir, "2026-10-17_120000_Game", *s, cols, 10, error));
    CHECK(fileCount(dir) == 2);

    // missing directory is created
    const std::string nested = dir + "/a/b";
    CHECK(writeSession(nested, "2026-10-17_130000_Game", *s, cols, 10, error));
    CHECK(fs::exists(nested + "/2026-10-17_130000_Game.json"));

    // keep=2 prunes while writing
    CHECK(writeSession(dir, "2026-10-18_120000_Game", *s, cols, 2, error));
    CHECK(writeSession(dir, "2026-10-19_120000_Game", *s, cols, 2, error));
    CHECK(!fs::exists(fs::path(dir) / "2026-10-17_120000_Game.json"));
    CHECK(fs::exists(fs::path(dir) / "2026-10-18_120000_Game.json"));
    CHECK(fs::exists(fs::path(dir) / "2026-10-19_120000_Game.csv.gz"));
    fs::remove_all(dir);
}

// Review M2: Save's window is the last 600 s by time; a pause leaves no rows, so old rows must not leak in
void recentRingCutsByTime() {
    const std::vector<LogKind> kinds = {LogKind::Gauge, LogKind::Gauge};
    RecentRing ring(2);
    for (int i = 0; i < 600; ++i) ring.add(1000 + i, {static_cast<float>(i), 0.0f});  // 1000..1599
    int64_t start = 0;
    // Save at 2000, no new sample since 1599: only 1401..1599 are within 600 s
    Timeline t = ring.timeline(kinds, start, 2000);
    CHECK(t.rows() == 199);
    CHECK(start == 1401);
    CHECK(ring.sizeWithin(2000) == 199);
    // a long pause, then one new sample: only that one
    ring.add(10599, {7.0f, 7.0f});
    t = ring.timeline(kinds, start, 10600);
    CHECK(t.rows() == 1);
    CHECK(start == 10599);
    if (t.rows() == 1) CHECK(near(t.row(0).values[0], 7));
    // a wholly idle stretch: nothing, and the start is the Save time
    t = ring.timeline(kinds, start, 50000);
    CHECK(t.rows() == 0);
    CHECK(start == 50000);
    // a gap inside the window stays a gap (offsets keep the hole)
    RecentRing gap(2);
    gap.add(5000, {1.0f, 1.0f});
    gap.add(5300, {2.0f, 2.0f});
    t = gap.timeline(kinds, start, 5400);
    CHECK(t.rows() == 2);
    if (t.rows() == 2) CHECK(t.row(1).offset == 300);
}

// Review S1: retention only counts and deletes our own file names
void pruneLeavesForeignFiles() {
    const std::string dir = tempDir("prune-foreign");
    std::ofstream(dir + "/2026-10-01_100000_g.json") << "{}";
    std::ofstream(dir + "/2026-10-01_100000_g.csv.gz") << "x";
    std::ofstream(dir + "/2026-10-02_100000_g_record.csv") << "x";
    std::ofstream(dir + "/000_customer-note.txt") << "keep";
    std::ofstream(dir + "/2026-10-01_100000_g.json.bak") << "keep";
    std::ofstream(dir + "/2026-10-01_100000_g.json.tmp") << "keep";
    std::ofstream(dir + "/1234.json") << "keep";
    std::ofstream(dir + "/2026-10-01_100000_a.b.json") << "keep";  // a '.' inside the app part isn't ours
    std::string stem;
    CHECK(ownedLogFile("2026-10-01_100000_g.csv.gz", stem) && stem == "2026-10-01_100000_g");
    CHECK(ownedLogFile("2026-10-17_133018_crash.csv", stem) && stem == "2026-10-17_133018_crash");
    CHECK(!ownedLogFile("000_customer-note.txt", stem));
    CHECK(!ownedLogFile("2026-10-01_100000_g.json.bak", stem));
    CHECK(!ownedLogFile("2026-10-01_100000_.json", stem));
    pruneByCount(dir, 0);
    CHECK(!fs::exists(dir + "/2026-10-01_100000_g.json"));
    CHECK(!fs::exists(dir + "/2026-10-01_100000_g.csv.gz"));
    CHECK(!fs::exists(dir + "/2026-10-02_100000_g_record.csv"));
    CHECK(fileCount(dir) == 5);  // the five foreign files
    fs::remove_all(dir);
}

void pruneKeepsNewestStems() {
    const std::string dir = tempDir("prune");
    for (int i = 1; i <= 12; ++i) {
        char stem[64];
        std::snprintf(stem, sizeof(stem), "2026-10-%02d_100000_g", i);
        std::ofstream(dir + "/" + stem + ".json") << "{}";
        std::ofstream(dir + "/" + stem + ".csv.gz") << "x";
    }
    std::ofstream(dir + "/notes.txt") << "keep me";  // not a stem (no leading digit)
    pruneByCount(dir, 10);
    CHECK(!fs::exists(dir + "/2026-10-01_100000_g.json"));
    CHECK(!fs::exists(dir + "/2026-10-01_100000_g.csv.gz"));
    CHECK(!fs::exists(dir + "/2026-10-02_100000_g.json"));
    CHECK(!fs::exists(dir + "/2026-10-02_100000_g.csv.gz"));
    CHECK(fs::exists(dir + "/2026-10-03_100000_g.json"));
    CHECK(fs::exists(dir + "/2026-10-12_100000_g.csv.gz"));
    CHECK(fs::exists(dir + "/notes.txt"));
    CHECK(fileCount(dir) == 21);  // 10 stems x 2 + notes
    pruneByCount(dir, 10);        // nothing more to do
    CHECK(fileCount(dir) == 21);
    pruneByCount(dir, 0);
    CHECK(fileCount(dir) == 1);
    pruneByCount(dir + "/missing", 3);  // no such dir: no crash
    fs::remove_all(dir);
}

void recordWriterFlushesOnBoundary() {
    const std::string dir = tempDir("record");
    const std::string path = dir + "/rec/out.csv";
    const std::vector<LogColumn> cols = smallColumns();
    RecordWriter w;
    CHECK(!w.isOpen());
    std::string error;
    CHECK(w.open(path, cols, error));
    CHECK(w.isOpen() && w.path() == path);
    w.add(100, sample(60), 0.0);  // first add flushes (header + row)
    CHECK(countLines(readFile(path)) == 2);
    const std::string head = readFile(path).substr(0, readFile(path).find('\n'));
    CHECK(head.rfind("unix,", 0) == 0);
    CHECK(head.find(findMetric(metric::kFrameAppFps)->name) != std::string::npos);
    w.add(101, sample(61), 1.0);
    w.add(102, sample(62), 2.0);
    w.add(103, sample(63), 4.9);
    CHECK(countLines(readFile(path)) == 2);  // not yet: under 5 s
    w.add(104, sample(64), 5.0);
    CHECK(countLines(readFile(path)) == 6);  // flushed
    CHECK(w.rows() == 5);
    w.add(105, {kNaN, kNaN, kNaN, kNaN}, 6.0);
    w.close();
    CHECK(!w.isOpen());
    const std::string all = readFile(path);
    CHECK(countLines(all) == 7);  // close writes what was pending
    CHECK(all.find("\n105,,,,\n") != std::string::npos);
    CHECK(all.find("\n100,60,0,0,0\n") != std::string::npos);
    w.add(106, sample(1), 7.0);  // closed: ignored
    CHECK(w.rows() == 6);
    fs::remove_all(dir);
}

// ---- end card ----

void endCardLines() {
    const std::vector<LogColumn> cols = {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kFrameDropped, LogKind::Count, 0, 0, 0, false},
        {metric::kPowerVphW, LogKind::Gauge, 0, 30, 300, false},
        {metric::kBatteryPct, LogKind::Gauge, 0, 100, 100, false},
        {metric::kTempCpuC, LogKind::Gauge, 0, 120, 240, false},
        {metric::kTempGpuC, LogKind::Gauge, 0, 120, 240, false},
        {metric::kTempBatteryC, LogKind::Gauge, 0, 80, 160, false},
        {metric::kPcStreamTotalAvg, LogKind::Gauge, 0, 150, 300, true},
        {metric::kPcStreamSlackAvg, LogKind::Gauge, -50, 50, 200, true},
        {metric::kPcStreamStreamResets, LogKind::Cumulative, 0, 0, 0, false},
    };
    SessionLog log(cols, "pc stream");
    const SessionInfo a = info(1, "steam.app.1", "Game", 1000);
    for (int i = 0; i < 100; ++i) {
        const int64_t t = 1000 + i;
        log.observe(a, t);
        log.add(t, {60.0f, i < 3 ? 1.0f : 0.0f, 5.0f + (i == 50 ? 3.0f : 0.0f), 80.0f - 0.05f * static_cast<float>(i),
                    60.0f + (i == 10 ? 10.0f : 0.0f), 65.0f, 33.0f, 20.0f, 4.0f + static_cast<float>(i % 5),
                    i < 40 ? 0.0f : 2.0f});
    }
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (ended.empty()) return;
    const std::vector<std::string> lines = endCard(*ended[0], cols);
    std::string all;
    for (const std::string& l : lines) all += l + "\n";
    CHECK(lines.size() == 6);
    CHECK(!lines.empty() && lines[0] == "Game · 1 min 39 s");
    CHECK(all.find("fps avg 60.0 · 3 dropped") != std::string::npos);
    CHECK(all.find("Power avg 5.0 W · peak 8.0 W") != std::string::npos);
    CHECK(all.find("Battery 80% to 75% (-5 points)") != std::string::npos);
    if (all.find("Battery 80% to 75% (-5 points)") == std::string::npos) std::printf("card:\n%s", all.c_str());
    CHECK(all.find("Peak CPU 70 °C · GPU 65 °C · battery 33 °C") != std::string::npos);
    CHECK(all.find("Stream latency avg 20.0 ms · min slack 4.0 ms · 2 link resets") != std::string::npos);
    if (all.find("2 link resets") == std::string::npos) std::printf("card:\n%s", all.c_str());

    // only fps: two lines; nothing recorded: just the title
    const std::vector<LogColumn> one = {cols[0]};
    SessionLog small(one, "headset");
    for (int64_t t = 1000; t < 1005; ++t) {
        small.observe(a, t);
        small.add(t, {60.0f});
    }
    small.endAll();
    auto e2 = small.takeEnded();
    CHECK(e2.size() == 1);
    if (!e2.empty()) {
        const auto l2 = endCard(*e2[0], one);
        CHECK(l2.size() == 2);
        CHECK(l2.size() == 2 && l2[1] == "fps avg 60.0 · 0 dropped");
    }
    SessionData blank;
    blank.info = a;
    CHECK(endCard(blank, cols).size() == 1);
}

// ---- Direct Record / Direct Save summaries: emptySummary + addToSummary ----

void emptySummaryAndAddToSummary() {
    const std::vector<LogColumn> cols = {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kFrameDropped, LogKind::Count, 0, 0, 0, false},
    };
    const SessionInfo a = info(1, "k1", "A", 1000);
    SessionData s = emptySummary(a, cols, "headset");
    CHECK(s.info.id == 1 && s.source == "headset");
    CHECK(s.stats.size() == cols.size());
    CHECK(s.hist.size() == cols.size());
    CHECK(s.dataSeconds == 0 && s.lastUnix == 0);
    CHECK(s.timeline.rows() == 0);

    addToSummary(s, cols, 1000, {60.0f, 1.0f});
    CHECK(s.dataSeconds == 1 && s.lastUnix == 1000);
    CHECK(s.stats[0].samples == 1 && near(s.stats[0].avg(), 60.0));
    CHECK(s.stats[1].counted == 1.0);
    CHECK(s.hist[0].total() == 1);

    // NaN values are skipped (stats/hist untouched), but dataSeconds and lastUnix still move
    addToSummary(s, cols, 1001, {kNaN, kNaN});
    CHECK(s.dataSeconds == 2 && s.lastUnix == 1001);
    CHECK(s.stats[0].samples == 1);
    CHECK(s.hist[0].total() == 1);

    addToSummary(s, cols, 1002, {90.0f, 0.0f});
    CHECK(s.dataSeconds == 3 && s.lastUnix == 1002);
    CHECK(s.stats[0].samples == 2 && near(s.stats[0].avg(), 75.0));  // (60 + 90) / 2
    CHECK(s.hist[0].total() == 2);

    CHECK(s.timeline.rows() == 0);  // addToSummary never touches the timeline
}

// ---- shortCard: the Logs tab's "last session" line, at most 3 lines ----

void shortCardLines() {
    const std::vector<LogColumn> cols = {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kFrameDropped, LogKind::Count, 0, 0, 0, false},
        {metric::kPowerVphW, LogKind::Gauge, 0, 30, 300, false},
        {metric::kBatteryPct, LogKind::Gauge, 0, 100, 100, false},
        {metric::kTempCpuC, LogKind::Gauge, 0, 120, 240, false},
    };
    SessionLog log(cols, "headset");
    const SessionInfo a = info(1, "steam.app.1", "Game", 1000);
    for (int i = 0; i < 100; ++i) {
        const int64_t t = 1000 + i;
        log.observe(a, t);
        log.add(t, {60.0f, i < 3 ? 1.0f : 0.0f, 5.0f + (i == 50 ? 3.0f : 0.0f), 80.0f - 0.05f * static_cast<float>(i),
                    60.0f + (i == 10 ? 10.0f : 0.0f)});
    }
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (ended.empty()) return;
    const std::vector<std::string> lines = shortCard(*ended[0], cols);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == "Last: Game · 1 min 39 s");
        CHECK(lines[1] == "fps avg 60.0 · 3 dropped · 5.0 W");
        CHECK(lines[2] == "Battery 80% to 75% · peak CPU 70 °C");
        if (lines[2] != "Battery 80% to 75% · peak CPU 70 °C") std::printf("line2: %s\n", lines[2].c_str());
    }

    // only fps: 2 lines; dropped defaults to 0 when its own column is absent (same convention as endCard)
    const std::vector<LogColumn> fpsOnly = {cols[0]};
    SessionLog small(fpsOnly, "headset");
    for (int64_t t = 1000; t < 1005; ++t) {
        small.observe(a, t);
        small.add(t, {60.0f});
    }
    small.endAll();
    auto e2 = small.takeEnded();
    CHECK(e2.size() == 1);
    if (!e2.empty()) {
        const auto l2 = shortCard(*e2[0], fpsOnly);
        CHECK(l2.size() == 2);
        CHECK(l2.size() == 2 && l2[1] == "fps avg 60.0 · 0 dropped");
    }

    // no samples at all: just the title
    SessionData blank;
    blank.info = a;
    CHECK(shortCard(blank, cols).size() == 1);
}

// ---- sampleColumns ----

void sampleColumnsFreshness() {
    MetricStore store;
    const std::vector<LogColumn> cols = {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kCpuUsagePct, LogKind::Gauge, 0, 100, 100, false},
    };
    store.setF32(metric::kFrameAppFps, 72.0, 10.0);
    std::vector<float> v = sampleColumns(store, cols, 10.5);
    CHECK(v.size() == 2 && near(v[0], 72.0));
    CHECK(std::isnan(v[1]));  // never sampled
    v = sampleColumns(store, cols, 1000.0);
    CHECK(std::isnan(v[0]));  // stale: a gap, not the last value
}

// ---- T11 R3: no scene app, histogram ranges, running totals ----

/** @return the session's own copy of sessionLogColumns() entry for id */
LogColumn colOf(uint16_t id) {
    for (const LogColumn& c : sessionLogColumns()) {
        if (c.id == id) return c;
    }
    std::printf("FAIL: column %u isn't in sessionLogColumns()\n", static_cast<unsigned>(id));
    ++failures;
    return LogColumn {id, LogKind::Gauge, 0, 1, 1, false};
}

/** The tracker's form of a session whose app left at leftUnix. */
SessionInfo gone(SessionInfo s, int64_t leftUnix) {
    s.leftUnix = leftUnix;
    return s;
}

/** One session of one column, fed one value a second, ended. */
std::unique_ptr<SessionData> solo(const LogColumn& col, const std::vector<float>& values) {
    SessionLog log({col}, "headset");
    const SessionInfo a = info(1, "k1", "A", 1000);
    int64_t t = 1000;
    for (float v : values) {
        log.observe(a, t);
        log.add(t, {v});
        ++t;
    }
    log.endAll();
    auto ended = log.takeEnded();
    return ended.empty() ? nullptr : std::move(ended[0]);
}

const JsonValue* metricOf(const JsonValue& root, uint16_t id) {
    const JsonValue* metrics = root.get("metrics");
    return metrics != nullptr ? metrics->get(findMetric(id)->name) : nullptr;
}

void noAppEndsTheSessionWhereTheAppLeft() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    run(log, game, 1000, 120);
    const SessionInfo away = gone(game, 1120);
    for (int64_t t = 1120; t < 1180; ++t) {  // 60 s of no app, values still arriving
        log.observe(away, t);
        log.add(t, sample(60.0f));
    }
    CHECK(log.takeEnded().empty());  // parked, not over yet
    log.observe(SessionInfo(), 1180);  // the tracker gave up: id 0
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    CHECK(log.current() == nullptr);
    if (ended.size() != 1) return;
    const SessionData& s = *ended[0];
    CHECK(s.dataSeconds == 120);  // the no-app seconds aren't counted
    CHECK(endUnix(s) == 1120);
    CHECK(s.timeline.rows() == 120);
    CHECK(s.timeline.row(s.timeline.rows() - 1).offset == 119);  // no rows after T
    JsonValue root;
    std::string error;
    CHECK(parseJson(summaryJson(s, smallColumns()), root, error));
    CHECK(root.get("end_unix") != nullptr && root.get("end_unix")->number == 1120);
    CHECK(root.get("duration_s") != nullptr && root.get("duration_s")->number == 120);
    CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 120);
    const std::vector<std::string> card = endCard(s, smallColumns());
    CHECK(!card.empty() && card[0] == "A · 2 min 00 s");
}

void backFromNoAppIsOneSessionWithAGap() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    run(log, game, 1000, 120);
    const SessionInfo away = gone(game, 1120);
    for (int64_t t = 1120; t < 1150; ++t) {
        log.observe(away, t);
        log.add(t, sample(60.0f));
    }
    run(log, game, 1150, 50);  // the app is back
    CHECK(log.takeEnded().empty());
    log.endAll();
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (ended.size() != 1) return;
    const SessionData& s = *ended[0];
    CHECK(s.info.id == 1);
    CHECK(s.dataSeconds == 170);
    CHECK(s.stats[0].samples == 170);
    CHECK(s.timeline.rows() == 170);
    if (s.timeline.rows() == 170) {
        CHECK(s.timeline.row(119).offset == 119);
        CHECK(s.timeline.row(120).offset == 150);  // the gap: offsets 120..149 have no rows
    }
    CHECK(endUnix(s) == 1199);
    CHECK(s.info.leftUnix == 0);
}

void shutdownWhileParkedEndsAtTheLeave() {
    SessionLog log(smallColumns(), "headset");
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    run(log, game, 1000, 120);
    const SessionInfo away = gone(game, 1120);
    for (int64_t t = 1120; t < 1140; ++t) {
        log.observe(away, t);
        log.add(t, sample(60.0f));
    }
    log.endAll();  // the HUD is shutting down
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (ended.size() != 1) return;
    CHECK(endUnix(*ended[0]) == 1120);
    CHECK(ended[0]->dataSeconds == 120);
}

void linkStateFormEmptyKeyParksAtFirstSight() {
    // the PC copy: same id, appKey "" from the moment the app left; no leftUnix on the wire
    SessionLog log(smallColumns(), "pc copy");
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    run(log, game, 1000, 100);
    SessionInfo empty = game;
    empty.appKey.clear();
    for (int64_t t = 1100; t < 1130; ++t) {
        log.observe(empty, t);
        log.add(t, sample(60.0f));
    }
    CHECK(log.current() != nullptr && log.current()->info.leftUnix == 1100);
    CHECK(log.current() != nullptr && log.current()->info.appKey == "steam.app.1");  // the key is kept
    CHECK(log.current() != nullptr && log.current()->dataSeconds == 100);
    log.observe(SessionInfo(), 1160);
    auto ended = log.takeEnded();
    CHECK(ended.size() == 1);
    if (!ended.empty()) {
        CHECK(endUnix(*ended[0]) == 1100);
        CHECK(ended[0]->dataSeconds == 100);
    }
    // back with the key again: the gap closes
    SessionLog back(smallColumns(), "pc copy");
    run(back, game, 1000, 100);
    for (int64_t t = 1100; t < 1120; ++t) {
        back.observe(empty, t);
        back.add(t, sample(60.0f));
    }
    run(back, game, 1120, 30);
    CHECK(back.current() != nullptr && back.current()->dataSeconds == 130);
    CHECK(back.current() != nullptr && back.current()->info.leftUnix == 0);
}

void idZeroWithNothingOpenIsHarmless() {
    SessionLog log(smallColumns(), "headset");
    log.observe(SessionInfo(), 100);
    CHECK(log.takeEnded().empty());
    CHECK(log.current() == nullptr);
}

void runningTotalIsRebasedAfterNoApp() {
    SessionLog log(smallColumns(), "headset");  // column 3 = kPcStreamStreamResets, Cumulative
    const SessionInfo game = info(1, "steam.app.1", "A", 1000);
    for (int64_t t = 1000; t < 1060; ++t) {  // 10 for 30 s, then 12
        log.observe(game, t);
        log.add(t, {60.0f, 0.0f, 0.0f, t < 1030 ? 10.0f : 12.0f});
    }
    const SessionInfo away = gone(game, 1060);
    for (int64_t t = 1060; t < 1090; ++t) {  // the counter climbs to 50 while no app runs
        log.observe(away, t);
        log.add(t, {60.0f, 0.0f, 0.0f, static_cast<float>(12 + (t - 1060) * 38 / 29)});
    }
    log.observe(game, 1090);
    log.add(1090, {60.0f, 0.0f, 0.0f, 51.0f});
    log.observe(game, 1091);
    log.add(1091, {60.0f, 0.0f, 0.0f, 52.0f});
    CHECK(log.current() != nullptr);
    if (log.current() == nullptr) return;
    // 2 before the gap + 0 for the jump over the gap + 1 after
    CHECK(near(log.current()->stats[3].counted, 3.0));
}

void batteryVoltageHistogramCoversThePack() {
    std::vector<float> v;
    for (int i = 0; i < 100; ++i) v.push_back(7.4f + 1.1f * static_cast<float>(i) / 99.0f);
    const auto s = solo(colOf(metric::kBatteryVoltageV), v);
    CHECK(s != nullptr);
    if (!s) return;
    JsonValue root;
    std::string error;
    CHECK(parseJson(summaryJson(*s, {colOf(metric::kBatteryVoltageV)}), root, error));
    const JsonValue* m = metricOf(root, metric::kBatteryVoltageV);
    CHECK(m != nullptr);
    if (m == nullptr) return;
    const double p5 = m->get("p5")->number, p95 = m->get("p95")->number;
    CHECK(p5 >= 7.4 - 1e-6 && p5 <= 7.6);
    CHECK(p95 >= 8.3 && p95 <= 8.5 + 1e-6);
    CHECK(p95 > 5.0);  // not the old 3..4.5 V range's clamp
    CHECK(s->hist[0].counts().back() == 0);
    CHECK(s->hist[0].counts().front() == 0);
}

void retransmitRequestsIsARunningTotal() {
    const LogColumn col = colOf(metric::kPcStreamRetransmitRequests);
    CHECK(col.kind == LogKind::Cumulative);
    std::vector<float> v;
    for (int i = 0; i < 100; ++i) v.push_back(static_cast<float>(11964 + (867 * i + 49) / 99));
    v.back() = 12831;
    const auto s = solo(col, v);
    CHECK(s != nullptr);
    if (!s) return;
    JsonValue root;
    std::string error;
    CHECK(parseJson(summaryJson(*s, {col}), root, error));
    const JsonValue* m = metricOf(root, metric::kPcStreamRetransmitRequests);
    CHECK(m != nullptr && m->get("count") != nullptr);
    if (m != nullptr) {
        CHECK(m->get("count") != nullptr && near(m->get("count")->number, 867.0));
        CHECK(m->get("p95") == nullptr && m->get("p5") == nullptr && m->get("avg") == nullptr);
    }
    // histograms keep out of it too
    CHECK(root.get("histograms") != nullptr && root.get("histograms")->get(findMetric(col.id)->name) == nullptr);

    // the counter restarted in the middle: 12000 -> 12100, then 40, then 90
    std::vector<float> w;
    for (int i = 0; i < 10; ++i) w.push_back(12000.0f);
    for (int i = 0; i < 10; ++i) w.push_back(12100.0f);
    for (int i = 0; i < 10; ++i) w.push_back(40.0f);
    for (int i = 0; i < 10; ++i) w.push_back(90.0f);
    const auto s2 = solo(col, w);
    CHECK(s2 != nullptr);
    if (!s2) return;
    JsonValue root2;
    CHECK(parseJson(summaryJson(*s2, {col}), root2, error));
    const JsonValue* m2 = metricOf(root2, metric::kPcStreamRetransmitRequests);
    CHECK(m2 != nullptr && m2->get("count") != nullptr && near(m2->get("count")->number, 100 + 40 + 50));
}

void gaugePercentilesStayInsideWhatWasSeen() {
    // a constant display rate: the bin's upper part (72.95) must not show
    const auto hz = solo(colOf(metric::kFrameDisplayHz), std::vector<float>(100, 72.0f));
    CHECK(hz != nullptr);
    if (hz) {
        JsonValue root;
        std::string error;
        CHECK(parseJson(summaryJson(*hz, {colOf(metric::kFrameDisplayHz)}), root, error));
        const JsonValue* m = metricOf(root, metric::kFrameDisplayHz);
        CHECK(m != nullptr && m->get("p5") != nullptr && near(m->get("p5")->number, 72.0, 1e-6));
        CHECK(m != nullptr && m->get("p95") != nullptr && near(m->get("p95")->number, 72.0, 1e-6));
    }
    // a fan whose highest reading is 3809.5
    std::vector<float> fan;
    for (int i = 0; i < 100; ++i) fan.push_back(3000.0f + 809.5f * static_cast<float>(i) / 99.0f);
    const auto f = solo(colOf(metric::kFanRpm), fan);
    CHECK(f != nullptr);
    if (f) {
        JsonValue root;
        std::string error;
        CHECK(parseJson(summaryJson(*f, {colOf(metric::kFanRpm)}), root, error));
        const JsonValue* m = metricOf(root, metric::kFanRpm);
        CHECK(m != nullptr && m->get("p95") != nullptr && m->get("p95")->number <= 3809.5 + 1e-3);
        CHECK(m != nullptr && m->get("p95") != nullptr && m->get("p95")->number > 3700.0);
        CHECK(m != nullptr && m->get("p5") != nullptr && m->get("p5")->number >= 3000.0 - 1e-6);
    }
}

void slackHistogramSpansHighValues() {
    const LogColumn col = colOf(metric::kPcStreamSlackAvg);
    CHECK(col.histMax >= 150.0f && col.bins == 200 && col.histMin == -50.0f);
    std::vector<float> v;
    for (int i = 0; i < 100; ++i) v.push_back(60.0f + 80.0f * static_cast<float>(i) / 99.0f);
    const auto s = solo(col, v);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(s->hist[0].counts().back() == 0);  // not piled in the last bin
    JsonValue root;
    std::string error;
    CHECK(parseJson(summaryJson(*s, {col}), root, error));
    const JsonValue* m = metricOf(root, metric::kPcStreamSlackAvg);
    CHECK(m != nullptr && m->get("p95") != nullptr);
    if (m != nullptr && m->get("p95") != nullptr) {
        CHECK(m->get("p95")->number < 150.0);
        CHECK(m->get("p95")->number >= 130.0 && m->get("p95")->number <= 140.0 + 1e-6);
        CHECK(m->get("p5")->number >= 60.0 - 1e-6 && m->get("p5")->number <= 70.0);
    }
}

// ---- T11 R3 review: running totals across no-app gaps and detours ----

/** fps (Gauge) + the retransmit running total (Cumulative), the review probe's columns. */
std::vector<LogColumn> totalColumns() {
    return {{metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
            {metric::kPcStreamRetransmitRequests, LogKind::Cumulative, 0, 0, 0, false}};
}

void feedTotal(SessionLog& l, const SessionInfo& s, int64_t t, float count) {
    l.observe(s, t);
    l.add(t, {72.0f, count});
}

// M1: back from no app, the first row has fps but the counter is N/A: the counter's baseline is still reset
void totalRebasesWhenTheFirstRowAfterTheGapHasNoCounter() {
    SessionLog log(totalColumns(), "probe");
    const SessionInfo a = info(1, "A", "A", 1000);
    SessionInfo awayA = a;
    awayA.leftUnix = 1060;
    feedTotal(log, a, 1000, 10);
    feedTotal(log, a, 1059, 12);
    feedTotal(log, awayA, 1060, 50);  // ignored: no app
    feedTotal(log, a, 1090, kNaN);
    CHECK(log.current() != nullptr && log.current()->stats[1].rebase);  // still pending after a row without it
    feedTotal(log, a, 1091, 50);
    CHECK(log.current() != nullptr && !log.current()->stats[1].rebase);
    feedTotal(log, a, 1092, 51);
    CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 3.0));  // 2 before, 1 after (was 41)
}

// M2a: A -> no app -> B (< 60 s) -> A: A's gap isn't bridged
void totalNotBridgedOverAGapBeforeADetour() {
    SessionLog log(totalColumns(), "probe");
    const SessionInfo a = info(1, "A", "A", 1000), b = info(2, "B", "B", 1090);
    SessionInfo awayA = a;
    awayA.leftUnix = 1060;
    feedTotal(log, a, 1000, 10);
    feedTotal(log, a, 1059, 12);
    feedTotal(log, awayA, 1060, 50);
    feedTotal(log, b, 1090, 50);
    feedTotal(log, b, 1099, 55);
    feedTotal(log, a, 1100, 55);
    CHECK(log.current() != nullptr && log.current()->info.id == 1);
    CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 7.0));  // 2 in A + 5 in B (was 45)
}

// M2b: A -> B -> B's app leaves -> A: the rise while B had no app isn't counted
void totalNotBridgedOverAGapAfterADetour() {
    SessionLog log(totalColumns(), "probe");
    const SessionInfo a = info(1, "A", "A", 1000), b = info(2, "B", "B", 1090);
    SessionInfo awayB = b;
    awayB.leftUnix = 1100;
    feedTotal(log, a, 1000, 10);
    feedTotal(log, a, 1059, 12);
    feedTotal(log, b, 1090, 12);
    feedTotal(log, b, 1099, 15);
    feedTotal(log, awayB, 1100, 50);
    feedTotal(log, a, 1120, 50);
    CHECK(log.current() != nullptr && log.current()->info.id == 1);
    CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 5.0));  // 2 in A + 3 in B (was 40)
}

// A plain detour with no gap still bridges the counter between the two sessions
void totalStillBridgesAPlainDetour() {
    SessionLog log(totalColumns(), "probe");
    const SessionInfo a = info(1, "A", "A", 1000), b = info(2, "B", "B", 1090);
    feedTotal(log, a, 1000, 10);
    feedTotal(log, a, 1059, 12);
    feedTotal(log, b, 1090, 15);
    feedTotal(log, b, 1099, 18);
    feedTotal(log, a, 1100, 18);
    CHECK(log.current() != nullptr && log.current()->info.id == 1);
    CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 8.0));  // 2 + bridge 3 + 3
    CHECK(log.current() != nullptr && log.current()->stats[1].samples == 5);
}

void columnStatsMergeBridgeAndRebase() {
    ColumnStats a, b;
    for (float v : {10.0f, 12.0f}) a.add(LogKind::Cumulative, v);  // counted 2, last 12
    for (float v : {20.0f, 25.0f}) b.add(LogKind::Cumulative, v);  // counted 5, first 20
    ColumnStats bridged = a;
    bridged.merge(LogKind::Cumulative, b);  // default: the 12 -> 20 rise is counted
    CHECK(near(bridged.counted, 2 + 8 + 5));
    ColumnStats unbridged = a;
    unbridged.merge(LogKind::Cumulative, b, false);
    CHECK(near(unbridged.counted, 2 + 5));
    CHECK(unbridged.samples == 4 && near(unbridged.last, 25));

    // a rebase pending on this side: no bridge either, and the flag follows the later side
    ColumnStats pending = a;
    pending.rebase = true;
    pending.merge(LogKind::Cumulative, b);
    CHECK(near(pending.counted, 2 + 5));
    CHECK(!pending.rebase);
    ColumnStats later = b;
    later.rebase = true;
    ColumnStats first = a;
    first.merge(LogKind::Cumulative, later);
    CHECK(first.rebase);

    // the next valid value is a new baseline, not a rise, and clears the flag
    ColumnStats s;
    for (float v : {10.0f, 12.0f}) s.add(LogKind::Cumulative, v);
    s.rebase = true;
    s.add(LogKind::Cumulative, 50.0f);
    CHECK(!s.rebase);
    CHECK(near(s.counted, 2.0));
    s.add(LogKind::Cumulative, 51.0f);
    CHECK(near(s.counted, 3.0));
    // a Count column isn't affected by the bridge flag
    ColumnStats c1, c2;
    c1.add(LogKind::Count, 3.0f);
    c2.add(LogKind::Count, 4.0f);
    c1.merge(LogKind::Count, c2, false);
    CHECK(near(c1.counted, 7.0));
}

// ---- T11 R3 re-review: a detour that never read the counter ----

// M2b2: A -> no app -> B (counter N/A throughout) -> A: the rise during the gap isn't counted, either form
void totalRebasedWhenTheDetourHadNoCounter() {
    for (int pcForm = 0; pcForm < 2; ++pcForm) {
        SessionLog log(totalColumns(), "probe");
        const SessionInfo a = info(1, "A", "A", 1000), b = info(2, "B", "B", 1090);
        SessionInfo away = a;
        if (pcForm) away.appKey.clear();  // LINK_STATE's form: same id, empty key
        else away.leftUnix = 1060;        // the headset's own form
        feedTotal(log, a, 1000, 10);
        feedTotal(log, a, 1059, 12);
        feedTotal(log, away, 1060, 50);   // ignored: no app
        feedTotal(log, b, 1090, kNaN);
        feedTotal(log, b, 1099, kNaN);
        feedTotal(log, a, 1100, 50);
        feedTotal(log, a, 1101, 51);
        CHECK(log.current() != nullptr && log.current()->info.id == 1);
        CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 3.0));  // 2 before + 1 after (was 41)
    }
}

// Nothing left and B never read the counter: the rise is bridged as before
void totalBridgedOverADetourWithoutCounter() {
    SessionLog log(totalColumns(), "probe");
    const SessionInfo a = info(1, "A", "A", 1000), b = info(2, "B", "B", 1090);
    feedTotal(log, a, 1000, 10);
    feedTotal(log, a, 1059, 12);
    feedTotal(log, b, 1090, kNaN);
    feedTotal(log, b, 1099, kNaN);
    feedTotal(log, a, 1100, 15);
    feedTotal(log, a, 1101, 16);
    CHECK(log.current() != nullptr && log.current()->info.id == 1);
    CHECK(log.current() != nullptr && near(log.current()->stats[1].counted, 6.0));  // 2 + bridge 3 + 1
}

void columnStatsEmptyMerge() {
    ColumnStats base;
    for (float v : {10.0f, 12.0f}) base.add(LogKind::Cumulative, v);
    const ColumnStats none;

    ColumnStats noBridge = base;  // a gap before the detour: the next value is a new baseline
    noBridge.merge(LogKind::Cumulative, none, false);
    CHECK(noBridge.rebase);
    CHECK(noBridge.samples == 2 && near(noBridge.counted, 2.0) && near(noBridge.last, 12.0));

    ColumnStats laterFlag = base;  // the detour's own app had left
    ColumnStats flagged;
    flagged.rebase = true;
    laterFlag.merge(LogKind::Cumulative, flagged, true);
    CHECK(laterFlag.rebase);

    ColumnStats plain = base;  // a plain detour with nothing read: untouched
    plain.merge(LogKind::Cumulative, none, true);
    CHECK(!plain.rebase);
    CHECK(plain.samples == 2 && near(plain.counted, 2.0));

    ColumnStats pending = base;  // a rebase that's already pending is kept
    pending.rebase = true;
    pending.merge(LogKind::Cumulative, none, true);
    CHECK(pending.rebase);
    pending.merge(LogKind::Cumulative, none, false);
    CHECK(pending.rebase);

    // the flag only means something for a Cumulative column
    ColumnStats gauge;
    gauge.add(LogKind::Gauge, 5.0f);
    gauge.merge(LogKind::Gauge, none, false);
    CHECK(!gauge.rebase);
}

}  // namespace

// gpu.busy_pct (now written by the FrameTiming reader) stays a session-log column and carries its value
void gpuLoadColumnKeepsValue() {
    const std::vector<LogColumn>& all = sessionLogColumns();
    size_t at = all.size();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].id == metric::kGpuBusyPct) at = i;
    }
    CHECK(at < all.size());
    if (at == all.size()) return;
    CHECK(all[at].kind == LogKind::Gauge);
    CHECK(all[at].histMax == 100.0f);

    MetricStore store;
    CHECK(std::isnan(sampleColumns(store, all, 10.0)[at]));  // not sampled yet
    store.setF32(metric::kGpuBusyPct, 96.5, 10.0);
    CHECK(near(sampleColumns(store, all, 10.5)[at], 96.5));
    CHECK(std::isnan(sampleColumns(store, all, 14.0)[at]));  // the source stopped: a gap, not the last value
    store.setNa(metric::kGpuBusyPct, Reason::NoFrames, 15.0);
    CHECK(std::isnan(sampleColumns(store, all, 15.0)[at]));
}

int main() {
    gpuLoadColumnKeepsValue();
    detourMergesIntoOneSession();
    twoGamesAreTwoSessions();
    endAllEndsParkedThenCurrent();
    shortGameHasLittleData();
    allNanSamplesAreNotCounted();
    noSessionIgnoresSamples();
    repeatedSecondIsIgnored();
    nameFilledInLater();
    timelineFullRateThenFiveSecondRows();
    timelineIgnoresRepeatedOffset();
    timelineCapHalves();
    timelineHalveMergesByKind();
    timelineTenHoursStaysUnderCap();
    histogramPercentiles();
    histogramClampsAndEmpty();
    statsGauge();
    statsCountCumulativeFlag();
    statsMerge();
    recentRingWraps();
    summaryJsonParses();
    timelineCsvDropsEmptyColumns();
    gzipRoundTrips();
    fileNamesAndTimes();
    writeSessionAtomicAndPrunes();
    pruneKeepsNewestStems();
    recentRingCutsByTime();
    pruneLeavesForeignFiles();
    recordWriterFlushesOnBoundary();
    endCardLines();
    emptySummaryAndAddToSummary();
    shortCardLines();
    sampleColumnsFreshness();
    noAppEndsTheSessionWhereTheAppLeft();
    backFromNoAppIsOneSessionWithAGap();
    shutdownWhileParkedEndsAtTheLeave();
    linkStateFormEmptyKeyParksAtFirstSight();
    idZeroWithNothingOpenIsHarmless();
    runningTotalIsRebasedAfterNoApp();
    batteryVoltageHistogramCoversThePack();
    retransmitRequestsIsARunningTotal();
    gaugePercentilesStayInsideWhatWasSeen();
    slackHistogramSpansHighValues();
    totalRebasesWhenTheFirstRowAfterTheGapHasNoCounter();
    totalNotBridgedOverAGapBeforeADetour();
    totalNotBridgedOverAGapAfterADetour();
    totalStillBridgesAPlainDetour();
    columnStatsMergeBridgeAndRebase();
    totalRebasedWhenTheDetourHadNoCounter();
    totalBridgedOverADetourWithoutCounter();
    columnStatsEmptyMerge();
    if (failures == 0) std::printf("core_session_log_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
