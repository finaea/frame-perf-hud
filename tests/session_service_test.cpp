// T11 review M1: the Logs page's Record switch shows the writer's real state (start, stop, failed start, shutdown).
// Also tickAt's once-per-clock-second rule: every clock second gets exactly one row, however the loop's passes drift
// against it.
#include "session_service.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
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

namespace fs = std::filesystem;

std::string tempDir(const char* tag) {
    const fs::path p = fs::temp_directory_path() / (std::string("fph-svc-") + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

fph::SessionInfo game() {
    fph::SessionInfo s;
    s.id = 5;
    s.appKey = "steam.app.438100";
    s.appName = "VRChat";
    s.startUnix = 1792200000;
    return s;
}

size_t recordFiles(const std::string& dir) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir + "/saves", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().find("_record.csv") != std::string::npos) ++n;
    }
    return n;
}

void recordSwitchFollowsWriter() {
    const std::string dir = tempDir("record");
    SessionService s(dir);
    CHECK(!s.ui().recording);
    uint64_t rev = s.ui().revision;
    s.setRecord(true, game(), 1.0);
    CHECK(s.recording());
    CHECK(s.ui().recording);  // the switch shows On while rows go to disk
    CHECK(s.ui().revision != rev);
    CHECK(recordFiles(dir) == 1);
    rev = s.ui().revision;
    s.setRecord(false, game(), 2.0);
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    CHECK(s.ui().revision != rev);
    // shutdown while recording turns it off too
    s.setRecord(true, game(), 3.0);
    CHECK(s.ui().recording);
    s.shutdown();
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    fs::remove_all(dir);
}

void failedStartStaysOff() {
    // a state dir that can't be created: the switch must stay Off
    SessionService s("/proc/fph-no-such-dir");
    s.setRecord(true, game(), 1.0);
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    CHECK(s.ui().recordStatus.rfind("Record failed", 0) == 0);
}

// ---- tickAt: one row per clock second ----

const int64_t kStart = 1792200010;  // game().startUnix + 10 s

std::string slurp(const std::string& cmd) {
    std::string text;
    FILE* f = ::popen(cmd.c_str(), "r");
    if (f == nullptr) return text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    ::pclose(f);
    return text;
}

/** The first column of a CSV (header line first), as printed by a command. */
std::vector<int64_t> firstColumn(const std::string& cmd) {
    std::vector<int64_t> out;
    const std::string text = slurp(cmd);
    size_t pos = text.find('\n');  // skip the header
    while (pos != std::string::npos && pos + 1 < text.size()) {
        out.push_back(std::strtoll(text.c_str() + pos + 1, nullptr, 10));
        pos = text.find('\n', pos + 1);
    }
    return out;
}

/** @return the path of the first file in dir ending in suffix, or empty */
std::string findFile(const std::string& dir, const std::string& suffix) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return it->path().string();
        }
    }
    return std::string();
}

/** @return the summary json's data_s, or -1 */
long long dataSeconds(const std::string& jsonPath) {
    const std::string text = slurp("cat '" + jsonPath + "'");
    const size_t at = text.find("\"data_s\":");
    return at == std::string::npos ? -1 : std::strtoll(text.c_str() + at + 9, nullptr, 10);
}

struct Call {
    double now;
    int64_t sec;
};

/** Drives tickAt with the calls (a fresh fps value each pass), ends the session; @return the saved timeline's seconds. */
std::vector<int64_t> sessionRows(const char* tag, const std::vector<Call>& calls, long long& dataS) {
    const std::string dir = tempDir(tag);
    {
        SessionService s(dir);
        fph::MetricStore store;
        for (const Call& c : calls) {
            store.setF32(fph::metric::kFrameAppFps, 90.0, c.now);
            s.tickAt(c.now, c.sec, true, game(), store);
        }
        s.shutdown();
    }
    const std::string csv = findFile(dir + "/sessions", ".csv.gz");
    const std::string json = findFile(dir + "/sessions", ".json");
    CHECK(!csv.empty() && !json.empty());
    std::vector<int64_t> rows;
    if (!csv.empty()) rows = firstColumn("gzip -dc '" + csv + "'");
    dataS = json.empty() ? -1 : dataSeconds(json);
    fs::remove_all(dir);
    return rows;
}

std::vector<int64_t> seconds(int64_t from, int64_t to) {
    std::vector<int64_t> v;
    for (int64_t t = from; t <= to; ++t) v.push_back(t);
    return v;
}

void driftedLoopHasNoGaps() {
    // Jack's session: passes slipped to one per 0.93 s, so the old 0.95 s stopwatch gate dropped every other pass
    std::vector<Call> calls;
    for (int i = 0; i < 90; ++i) {
        calls.push_back({100.0 + 0.93 * i, kStart + static_cast<int64_t>(std::floor(0.93 * i))});
    }
    const int64_t last = calls.back().sec;
    long long dataS = 0;
    const std::vector<int64_t> rows = sessionRows("drift", calls, dataS);
    CHECK(rows == seconds(kStart, last));  // every second once, none missing
    CHECK(dataS == last - kStart + 1);     // the data seconds are the distinct seconds
}

void sameSecondGivesOneRow() {
    // 10 passes 0.05 s apart inside the first second, then a pass a second to make a saveable session
    std::vector<Call> calls;
    for (int i = 0; i < 10; ++i) calls.push_back({100.0 + 0.05 * i, kStart});
    for (int i = 1; i < 70; ++i) calls.push_back({100.0 + i, kStart + i});
    long long dataS = 0;
    const std::vector<int64_t> rows = sessionRows("same", calls, dataS);
    CHECK(rows == seconds(kStart, kStart + 69));
    CHECK(dataS == 70);
}

void realStallLosesOnlyItsSecond() {
    // the clock jumps by 2 once: exactly that second is missing
    std::vector<Call> calls;
    for (int i = 0; i < 80; ++i) calls.push_back({100.0 + i, kStart + i + (i >= 30 ? 1 : 0)});
    std::vector<int64_t> want;
    for (int64_t t = kStart; t <= calls.back().sec; ++t) {
        if (t != kStart + 30) want.push_back(t);
    }
    long long dataS = 0;
    const std::vector<int64_t> rows = sessionRows("stall", calls, dataS);
    CHECK(rows == want);
    CHECK(dataS == static_cast<long long>(want.size()));
}

void recordGetsOneRowPerSecond() {
    const std::string dir = tempDir("rectick");
    SessionService s(dir);
    fph::MetricStore store;
    s.setRecord(true, game(), 100.0);
    CHECK(s.recording());
    // drifted passes over 11 s, each with a second pass in the same clock second
    int64_t last = kStart;
    for (int i = 0; i < 12; ++i) {
        const double now = 100.0 + 0.93 * i;
        last = kStart + static_cast<int64_t>(std::floor(0.93 * i));
        store.setF32(fph::metric::kFrameAppFps, 90.0, now);
        s.tickAt(now, last, true, game(), store);
        s.tickAt(now + 0.01, last, true, game(), store);
    }
    s.setRecord(false, game(), 120.0);
    const std::string csv = findFile(dir + "/saves", "_record.csv");
    CHECK(!csv.empty());
    CHECK(firstColumn("cat '" + csv + "'") == seconds(kStart, last));
    fs::remove_all(dir);
}

void notLookingSamplesNothingButObserves() {
    const std::string dir = tempDir("nolook");
    SessionService s(dir);
    fph::MetricStore store;
    s.setRecord(true, game(), 100.0);
    for (int i = 0; i < 70; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, false, game(), store);
    }
    const std::string dbg = s.debugText();
    CHECK(dbg.find("open session 5 (steam.app.438100): 0 s of data") != std::string::npos);  // observed, not sampled
    CHECK(dbg.find("ring 0 s") != std::string::npos);
    s.setRecord(false, game(), 200.0);
    CHECK(firstColumn("cat '" + findFile(dir + "/saves", "_record.csv") + "'").empty());  // header only
    s.shutdown();
    CHECK(findFile(dir + "/sessions", ".json").empty());  // no data: no files
    fs::remove_all(dir);
}

}  // namespace

int main() {
    recordSwitchFollowsWriter();
    failedStartStaysOff();
    driftedLoopHasNoGaps();
    sameSecondGivesOneRow();
    realStallLosesOnlyItsSecond();
    recordGetsOneRowPerSecond();
    notLookingSamplesNothingButObserves();
    if (failures == 0) std::printf("session_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
