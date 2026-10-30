// T11 review M1: the Logs page's Record switch shows the writer's real state (start, stop, failed start, shutdown).
// Also tickAt's once-per-clock-second rule: every clock second gets exactly one row, however the loop's passes drift
// against it.
#include "json.h"
#include "session_service.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
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
    CHECK(s.ui().recordStatus.rfind("Direct Record failed", 0) == 0);
    CHECK(s.ui().recordLine == s.ui().recordStatus);
    CHECK(s.ui().recordPath.empty());
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
        s.setSessionLogs(true);
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
    s.setSessionLogs(true);
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

// ---- Session logs off (the default): no session is followed, written or shown; Save and Record are unaffected ----

void sessionLogsOffDropsEverything() {
    const std::string dir = tempDir("logsoff");
    SessionService s(dir);  // off by default
    fph::MetricStore store;
    // Save's 10-minute ring keys off real std::time(), not the clock-second passed to tickAt: drive this in a
    // window ending 51-120 s ago so a Save right after still finds it (well inside the 600 s window)
    const int64_t base = static_cast<int64_t>(std::time(nullptr)) - 120;
    for (int i = 0; i < 70; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, base + i, true, game(), store);
    }
    CHECK(s.ui().lastCard.empty());
    CHECK(s.hudCard(100.0 + 69).empty());
    s.shutdown();
    CHECK(findFile(dir + "/sessions", ".json").empty());
    CHECK(findFile(dir + "/sessions", ".csv.gz").empty());

    // Save still writes the last 10 minutes, even with Session Record off: it always summarises its own window
    s.save(game(), 200.0);
    const std::string json = findFile(dir + "/saves", "_save.json");
    const std::string csv = findFile(dir + "/saves", "_save.csv.gz");
    CHECK(!json.empty() && !csv.empty());
    const std::vector<int64_t> rows = firstColumn("gzip -dc '" + csv + "'");
    CHECK(!rows.empty());  // the timeline really has the ring's values, not just a header
    CHECK(dataSeconds(json) == 70);  // the window's own data_s: every ticked second landed in the ring
    const std::string text = slurp("cat '" + json + "'");
    JsonValue root;
    std::string error;
    CHECK(parseJson(text, root, error));
    const JsonValue* metrics = root.get("metrics");
    const JsonValue* fps = metrics != nullptr ? metrics->get(fph::findMetric(fph::metric::kFrameAppFps)->name) : nullptr;
    CHECK(fps != nullptr);
    if (fps != nullptr) {
        const JsonValue* samples = fps->get("samples");
        CHECK(samples != nullptr && samples->number == 70);
    }
    CHECK(root.get("session_so_far") == nullptr);  // Session Record is off: no open session to carry along
    CHECK(text.find("Direct Save") != std::string::npos);
    fs::remove_all(dir);
}

void sessionLogsOnWritesAFile() {
    const std::string dir = tempDir("logson");
    SessionService s(dir);
    s.setSessionLogs(true);
    fph::MetricStore store;
    for (int i = 0; i < 70; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.shutdown();  // ends the session (70 s of data, past kMinSessionSec): it's written and the card is set
    CHECK(!s.ui().lastCard.empty());
    CHECK(!findFile(dir + "/sessions", ".json").empty());
    CHECK(!findFile(dir + "/sessions", ".csv.gz").empty());
    fs::remove_all(dir);
}

void sessionLogsToggledOffMidSessionDropsIt() {
    const std::string dir = tempDir("logstoggle");
    SessionService s(dir);
    s.setSessionLogs(true);
    fph::MetricStore store;
    // 70 s: past kMinSessionSec (60), so this would be written on shutdown if Off didn't really discard it
    for (int i = 0; i < 70; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.setSessionLogs(false);  // the open session is dropped unsaved
    CHECK(findFile(dir + "/sessions", ".json").empty());
    CHECK(findFile(dir + "/sessions", ".csv.gz").empty());
    s.shutdown();
    CHECK(findFile(dir + "/sessions", ".json").empty());
    CHECK(findFile(dir + "/sessions", ".csv.gz").empty());
    fs::remove_all(dir);
}

// Off right before Save (no shutdown in between): a mature, open session is dropped, not carried into the save
void sessionLogsOffThenSaveHasNoSummary() {
    const std::string dir = tempDir("offsave");
    SessionService s(dir);
    s.setSessionLogs(true);
    fph::MetricStore store;
    for (int i = 0; i < 70; ++i) {  // past kMinSessionSec: a real, saveable-looking session
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.setSessionLogs(false);  // dropped right away
    s.save(game(), 200.0);
    const std::string json = findFile(dir + "/saves", "_save.json");
    CHECK(!json.empty());
    const std::string text = slurp("cat '" + json + "'");
    JsonValue root;
    std::string error;
    CHECK(parseJson(text, root, error));
    CHECK(root.get("session_so_far") == nullptr);  // the mature session was dropped, not carried into the save
    CHECK(text.find("Direct Save") != std::string::npos);

    s.shutdown();
    CHECK(findFile(dir + "/sessions", ".json").empty());  // the dropped session never reaches sessions/
    CHECK(findFile(dir + "/sessions", ".csv.gz").empty());
    fs::remove_all(dir);
}

// ---- Direct Record writes a JSON summary (<file>_record.json) when it stops ----

void recordWritesJsonSummaryOnStop() {
    const std::string dir = tempDir("recordjson");
    SessionService s(dir);
    fph::MetricStore store;
    s.setRecord(true, game(), 100.0);
    CHECK(s.recording());
    const int kSeconds = 10;
    for (int i = 0; i < kSeconds; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 72.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.setRecord(false, game(), 200.0);
    const std::string csv = findFile(dir + "/saves", "_record.csv");
    CHECK(!csv.empty());
    const std::string json = csv.substr(0, csv.size() - 4) + ".json";  // same stem as the .csv
    CHECK(fs::exists(json));
    JsonValue root;
    std::string error;
    CHECK(parseJson(slurp("cat '" + json + "'"), root, error));
    CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == kSeconds);
    const JsonValue* metrics = root.get("metrics");
    CHECK(metrics != nullptr);
    const JsonValue* fps = metrics != nullptr ? metrics->get(fph::findMetric(fph::metric::kFrameAppFps)->name) : nullptr;
    CHECK(fps != nullptr);
    if (fps != nullptr) {
        const JsonValue* avg = fps->get("avg");
        const JsonValue* samples = fps->get("samples");
        CHECK(avg != nullptr && std::fabs(avg->number - 72.0) < 0.01);  // a constant value -> avg == that value
        CHECK(samples != nullptr && samples->number == kSeconds);
    }
    fs::remove_all(dir);
}

void recordJsonSummaryWrittenOnShutdownToo() {
    const std::string dir = tempDir("recordshutdown");
    SessionService s(dir);
    fph::MetricStore store;
    s.setRecord(true, game(), 100.0);
    for (int i = 0; i < 5; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 60.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.shutdown();
    const std::string csv = findFile(dir + "/saves", "_record.csv");
    CHECK(!csv.empty());
    CHECK(fs::exists(csv.substr(0, csv.size() - 4) + ".json"));
    fs::remove_all(dir);
}

/** @return how many files sit directly in dir. */
size_t fileCountDirect(const std::string& dir) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
}

// pruneByCount must treat a Record's .csv + .json as one stem, not two: with more stems than the keep limit, exactly
// `keep` stems survive, each keeping both its files (never one half of a pair)
void pruneByCountKeepsRecordStemAsOne() {
    const std::string dir = tempDir("prunerecord");
    for (int i = 1; i <= fph::slog::kKeepSaves + 2; ++i) {
        char stem[64];
        std::snprintf(stem, sizeof(stem), "2026-01-%02d_100000_game_record", i);
        std::ofstream(dir + "/" + stem + ".csv") << "unix,t_s\n";
        std::ofstream(dir + "/" + stem + ".json") << "{}";
    }
    fph::slog::pruneByCount(dir, fph::slog::kKeepSaves);
    // the 2 oldest stems are gone entirely (both halves), the newest kKeepSaves keep both halves
    CHECK(!fs::exists(dir + "/2026-01-01_100000_game_record.csv"));
    CHECK(!fs::exists(dir + "/2026-01-01_100000_game_record.json"));
    CHECK(!fs::exists(dir + "/2026-01-02_100000_game_record.csv"));
    CHECK(!fs::exists(dir + "/2026-01-02_100000_game_record.json"));
    char newest[64];
    std::snprintf(newest, sizeof(newest), "2026-01-%02d_100000_game_record", fph::slog::kKeepSaves + 2);
    CHECK(fs::exists(dir + "/" + std::string(newest) + ".csv"));
    CHECK(fs::exists(dir + "/" + std::string(newest) + ".json"));
    CHECK(fileCountDirect(dir) == static_cast<size_t>(fph::slog::kKeepSaves) * 2);  // each surviving stem keeps both
    fs::remove_all(dir);
}

// ---- Direct Save: Session Record on with an open session adds "session_so_far" ----

void saveIncludesSessionSoFarWhileSessionRecordIsOn() {
    const std::string dir = tempDir("sessionsofar");
    SessionService s(dir);
    s.setSessionLogs(true);
    fph::MetricStore store;
    // real time, so the ring window (std::time()-keyed) actually has this data too
    const int64_t base = static_cast<int64_t>(std::time(nullptr)) - 30;
    const int kSeconds = 20;
    for (int i = 0; i < kSeconds; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, base + i, true, game(), store);
    }
    s.save(game(), 200.0);
    const std::string json = findFile(dir + "/saves", "_save.json");
    CHECK(!json.empty());
    JsonValue root;
    std::string error;
    CHECK(parseJson(slurp("cat '" + json + "'"), root, error));
    CHECK(root.get("data_s") != nullptr && root.get("data_s")->number > 0);  // the window's own data, at top level
    const JsonValue* metrics = root.get("metrics");
    CHECK(metrics != nullptr && !metrics->members.empty());
    const JsonValue* soFar = root.get("session_so_far");
    CHECK(soFar != nullptr && soFar->isObject());
    if (soFar != nullptr) {
        const JsonValue* soFarDataS = soFar->get("data_s");
        CHECK(soFarDataS != nullptr && soFarDataS->number == kSeconds);  // the open session's own data_s
    }
    s.shutdown();
    fs::remove_all(dir);
}

void recordWritesWithSessionLogsOff() {
    const std::string dir = tempDir("recordnologs");
    SessionService s(dir);  // off by default
    fph::MetricStore store;
    s.setRecord(true, game(), 100.0);
    for (int i = 0; i < 5; ++i) {
        store.setF32(fph::metric::kFrameAppFps, 90.0, 100.0 + i);
        s.tickAt(100.0 + i, kStart + i, true, game(), store);
    }
    s.setRecord(false, game(), 120.0);
    CHECK(!findFile(dir + "/saves", "_record.csv").empty());
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
    sessionLogsOffDropsEverything();
    sessionLogsOnWritesAFile();
    sessionLogsToggledOffMidSessionDropsIt();
    sessionLogsOffThenSaveHasNoSummary();
    recordWritesJsonSummaryOnStop();
    recordJsonSummaryWrittenOnShutdownToo();
    pruneByCountKeepsRecordStemAsOne();
    saveIncludesSessionSoFarWhileSessionRecordIsOn();
    recordWritesWithSessionLogsOff();
    if (failures == 0) std::printf("session_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
