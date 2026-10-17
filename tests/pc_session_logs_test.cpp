// T11: the PC's session logs (pc/session_logs): the headset copy, the "headset went silent" early write, the PC
// stream log, Save. Uses a throwaway data dir.
#include "json.h"
#include "session_logs.h"
#include "stream_metrics.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
using fph::pc::PcSessionLogs;
namespace fs = std::filesystem;

constexpr int64_t kBase = 1800000000;

std::string tempDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path p = fs::temp_directory_path() / (std::string("fph-pcslog-") + tag + "-" + std::to_string(stamp));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

std::string readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** @return the files in dir whose name ends with suffix (not recursive) */
std::vector<fs::path> filesWith(const std::string& dir, const std::string& suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(ec) && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(it->path());
        }
    }
    return out;
}

bool parse(const fs::path& path, JsonValue& root) {
    std::string error;
    return parseJson(readFile(path), root, error);
}

int count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

SessionInfo session(int32_t id, const std::string& name, int64_t start) {
    SessionInfo s;
    s.id = id;
    s.appKey = "steam.app." + std::to_string(id);
    s.appName = name;
    s.startUnix = start;
    return s;
}

/** One UI tick a second: the headset's values arrive while live, and stay stale otherwise. */
struct Rig {
    std::string dir;
    MetricStore store;
    PcSessionLogs logs;
    SessionInfo headset;
    double now = 0;
    int64_t unixNow = kBase;
    double lastMetrics = -1;
    std::string allLog;

    explicit Rig(const char* tag) : dir(tempDir(tag)), logs(dir) {}
    ~Rig() { fs::remove_all(dir); }

    void sec(bool live, bool pcStream = false) {
        now += 1.0;
        unixNow += 1;
        if (live) {
            store.setF32(metric::kFrameAppFps, 60.0, now);
            store.setF32(metric::kPowerVphW, 5.0, now);
            lastMetrics = now;
        }
        if (pcStream) {
            store.setText(metric::kPcStreamSceneKey, "steam.app.99", now);
            store.setText(metric::kPcStreamSceneName, "PCGame", now);
            store.setF32(metric::kPcStreamTotalAvg, 20.0, now);
        }
        logs.tick(now, unixNow, headset, lastMetrics, store);
        const std::string l = logs.takeLog();
        if (!l.empty()) allLog += l + "\n";
    }
    void run(int seconds, bool live, bool pcStream = false) {
        for (int i = 0; i < seconds; ++i) sec(live, pcStream);
    }
};

void silentHeadsetIsWrittenOnce() {
    Rig r("silent");
    r.headset = session(7, "Game", kBase + 1);
    r.run(90, true);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());  // still playing: nothing yet
    const int64_t lastHeardUnix = r.unixNow;
    r.run(10, false);  // 10 s of silence is still within the limit
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    r.run(1, false);  // 11 s
    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    CHECK(filesWith(r.logs.sessionsDir(), ".csv.gz").size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        const JsonValue* note = root.get("note");
        CHECK(note != nullptr && note->text.rfind("headset went silent at ", 0) == 0);
        CHECK(note != nullptr && note->text.find(slog::clockTime(lastHeardUnix)) != std::string::npos);
        const JsonValue* data = root.get("data_s");
        CHECK(data != nullptr && data->number >= 60 && data->number <= 95);
        const JsonValue* source = root.get("source");
        CHECK(source != nullptr && source->text == "pc copy");
        const JsonValue* id = root.get("session_id");
        CHECK(id != nullptr && id->number == 7);
        CHECK(json[0].filename().string().find("_Game.json") != std::string::npos);
    }
    CHECK(count(r.allLog, "headset silent: saved its session now") == 1);

    // still silent: it isn't written again
    r.run(60, false);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").size() == 1);
    CHECK(count(r.allLog, "headset silent: saved its session now") == 1);

    // METRICS come back and go quiet again: that is a new silence
    r.run(3, true);
    r.run(12, false);
    CHECK(count(r.allLog, "headset silent: saved its session now") == 2);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").size() == 1);  // same session, same stem: overwritten

    // the final write on shutdown replaces it too, still one file
    r.logs.shutdown();
    CHECK(filesWith(r.logs.sessionsDir(), ".json").size() == 1);
    CHECK(filesWith(r.logs.sessionsDir(), ".csv.gz").size() == 1);
}

void shortSessionIsNotWrittenWhenSilent() {
    Rig r("short");
    r.headset = session(7, "Game", kBase + 1);
    r.run(30, true);
    r.run(30, false);  // silent for a long time
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    CHECK(count(r.allLog, "headset silent") == 0);
    r.logs.shutdown();  // 30 s of data: skipped at the end too
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    CHECK(r.logs.takeLog().find("not saved") != std::string::npos);
}

void neverHeardIsNotSilent() {
    Rig r("never");
    r.headset = session(7, "Game", kBase + 1);
    r.run(30, false);  // no METRICS yet (lastMetricsAt < 0)
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    CHECK(r.allLog.find("silent") == std::string::npos);
}

void newSessionWritesTheOldOneAfterSixtySeconds() {
    Rig r("switch");
    r.headset = session(7, "Game", kBase + 1);
    r.run(90, true);
    r.headset = session(8, "Other", r.unixNow + 1);
    r.run(59, true);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());  // the old one is parked: Other could be a detour
    r.run(2, true);
    std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("session_id") != nullptr && root.get("session_id")->number == 7);
        CHECK(root.get("app_name") != nullptr && root.get("app_name")->text == "Game");
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
        CHECK(root.get("note") != nullptr && root.get("note")->text.empty());
    }
    r.logs.shutdown();  // the open one (61 s) is written now
    json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 2);
}

void detourBackToTheGameIsOneSession() {
    Rig r("detour");
    r.headset = session(7, "Game", kBase + 1);
    r.run(90, true);
    r.headset = session(8, "Home", r.unixNow + 1);
    r.run(20, true);
    r.headset = session(7, "Game", kBase + 1);  // the headset's tracker resumed id 7
    r.run(40, true);
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 150);
    }
}

void shutdownWritesOpenSession() {
    Rig r("shutdown");
    r.headset = session(7, "Game", kBase + 1);
    r.run(90, true);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    CHECK(filesWith(r.logs.sessionsDir(), ".csv.gz").size() == 1);
    CHECK(r.logs.takeLog().find("session saved") != std::string::npos);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
    }
    // nothing of the PC's Streaming values was there: no pc-stream file
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
}

void pcStreamLogFollowsThePcSceneApp() {
    Rig r("stream");  // no headset session at all (id 0): only the PC's own log runs
    r.run(70, false, true);
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    CHECK(filesWith(r.logs.streamDir(), ".csv.gz").size() == 1);
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());  // the headset copy is separate
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("source") != nullptr && root.get("source")->text == "pc stream");
        CHECK(root.get("app_name") != nullptr && root.get("app_name")->text == "PCGame");
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 70);
        const JsonValue* metrics = root.get("metrics");
        CHECK(metrics != nullptr && metrics->get(findMetric(metric::kPcStreamTotalAvg)->name) != nullptr);
        // only the PC's ids are in this copy
        CHECK(metrics != nullptr && metrics->get(findMetric(metric::kFrameAppFps)->name) == nullptr);
    }
}

/** Seconds where the PC's reader marks every Streaming value N/A with `reason` (NoSteamVr = SteamVR quit). */
void runStreamNa(Rig& r, int seconds, Reason reason) {
    for (int i = 0; i < seconds; ++i) {
        r.now += 1.0;
        r.unixNow += 1;
        setPcStreamNa(r.store, reason, r.now);
        r.logs.tick(r.now, r.unixNow, r.headset, r.lastMetrics, r.store);
        const std::string l = r.logs.takeLog();
        if (!l.empty()) r.allLog += l + "\n";
    }
}

// Review M3: PC SteamVR quitting ends the stream session at once; the same game after a restart is a new one
void pcSteamVrQuitEndsStreamSession() {
    Rig r("stream-quit");
    r.run(90, false, true);
    runStreamNa(r, 30, Reason::NoSteamVr);
    CHECK(filesWith(r.logs.streamDir(), ".json").size() == 1);  // written while the app sits in the tray
    CHECK(count(r.allLog, "PC SteamVR gone") == 1);
    r.run(90, false, true);  // the same game again
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 2);
    for (const fs::path& p : json) {
        JsonValue root;
        CHECK(parse(p, root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
    }
}

// Re-check M3: the reader reconnecting after a Streaming pause → resume reads NoSteamVr for a few seconds; that is not
// a quit: one session, nothing written during play
void pcStreamReconnectBlipKeepsSession() {
    Rig r("stream-blip");
    r.run(90, false, true);
    runStreamNa(r, 2, Reason::Paused);     // widget removed
    runStreamNa(r, 6, Reason::NoSteamVr);  // re-added inside the reader's 5 s discovery wait
    r.run(90, false, true);                // the same scene again
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    CHECK(r.allLog.find("PC SteamVR gone") == std::string::npos);
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 180);
    }
}

// NoSteamVr held 10 s ends the session once; its end is the last data second, not the debounce time
void pcSteamVrGoneEndsAtLastData() {
    Rig r("stream-gone");
    r.run(90, false, true);
    const int64_t lastData = r.unixNow;
    runStreamNa(r, 9, Reason::NoSteamVr);
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());  // not yet 10 s
    runStreamNa(r, 20, Reason::NoSteamVr);
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    CHECK(count(r.allLog, "PC SteamVR gone") == 1);
    CHECK(count(r.allLog, "pc stream session saved") == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("end_unix") != nullptr && root.get("end_unix")->number == static_cast<double>(lastData));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
    }
}

/** Like runStreamNa(NoSteamVr), but the reader's log tail keeps the cumulative counters valid (the real producer). */
void runNoSteamVrWithCounters(Rig& r, int seconds) {
    for (int i = 0; i < seconds; ++i) {
        r.now += 1.0;
        r.unixNow += 1;
        setPcStreamNa(r.store, Reason::NoSteamVr, r.now);
        r.store.setI32(metric::kPcStreamStreamResets, 2, r.now);
        r.store.setI32(metric::kPcStreamBadLinks, 0, r.now);
        r.store.setI32(metric::kPcStreamEncoderReinits, 1, r.now);
        r.store.setI32(metric::kPcStreamThrottleEvents, 0, r.now);
        r.logs.tick(r.now, r.unixNow, r.headset, r.lastMetrics, r.store);
        const std::string l = r.logs.takeLog();
        if (!l.empty()) r.allLog += l + "\n";
    }
}

// Re-check 2: the counters republished without SteamVR aren't data: the end and data_s stay at the last real second
void noSteamVrCountersAreNotData() {
    Rig r("stream-counters");
    r.run(90, false, true);
    const int64_t lastData = r.unixNow;
    runNoSteamVrWithCounters(r, 15);
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
        CHECK(root.get("end_unix") != nullptr && root.get("end_unix")->number == static_cast<double>(lastData));
    }
}

// ... and a 50 s game followed by a quit stays under 60 s: not saved
void shortGameThenQuitIsNotSaved() {
    Rig r("stream-short-quit");
    r.run(50, false, true);
    runNoSteamVrWithCounters(r, 15);
    r.logs.shutdown();
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    CHECK(r.allLog.find("not saved") != std::string::npos);
}

// ... while the Streaming widget being switched off (Paused) keeps the session open
void pcStreamPauseKeepsSession() {
    Rig r("stream-pause");
    r.run(90, false, true);
    runStreamNa(r, 30, Reason::Paused);
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    r.run(90, false, true);
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 180);
    }
}

// Review M2 (PC): Save after a long headset outage carries no rows older than 10 minutes
void saveCutsOldRows() {
    Rig r("save-old");
    r.headset = session(7, "Game", kBase + 1);
    r.run(120, true);
    r.run(900, false);  // 15 min of nothing
    r.logs.save(r.unixNow);
    const std::vector<fs::path> gz = filesWith(r.logs.savesDir(), ".csv.gz");
    CHECK(gz.size() == 1);
    CHECK(r.logs.takeLog().find("(0 s)") != std::string::npos);
}

void bothLogsAtOnce() {
    Rig r("both");
    r.headset = session(7, "Game", kBase + 1);
    for (int i = 0; i < 80; ++i) r.sec(true, true);
    r.logs.shutdown();
    CHECK(filesWith(r.logs.sessionsDir(), ".json").size() == 1);
    CHECK(filesWith(r.logs.streamDir(), ".json").size() == 1);
    // the headset copy holds the headset's values, not the PC stream ones
    JsonValue root;
    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    if (json.size() == 1 && parse(json[0], root)) {
        const JsonValue* metrics = root.get("metrics");
        CHECK(metrics != nullptr && metrics->get(findMetric(metric::kFrameAppFps)->name) != nullptr);
        CHECK(metrics != nullptr && metrics->get(findMetric(metric::kPcStreamTotalAvg)->name) == nullptr);
    }
}

void saveWritesTwoFiles() {
    Rig r("save");
    r.headset = session(7, "My Game", kBase + 1);
    r.run(90, true);
    const std::string status = r.logs.save(r.unixNow);
    CHECK(status.rfind("Saved the last 2 min to saves", 0) == 0);
    const std::vector<fs::path> json = filesWith(r.logs.savesDir(), "_save.json");
    const std::vector<fs::path> gz = filesWith(r.logs.savesDir(), "_save.csv.gz");
    CHECK(json.size() == 1 && gz.size() == 1);
    CHECK(json.size() == 1 && json[0].filename().string().find("_My-Game_save.json") != std::string::npos);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("note") != nullptr && root.get("note")->text.rfind("Save:", 0) == 0);
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);
        const JsonValue* metrics = root.get("metrics");
        CHECK(metrics != nullptr && metrics->get(findMetric(metric::kFrameAppFps)->name) != nullptr);
    }
    if (gz.size() == 1) {
        const std::string bytes = readFile(gz[0]);
        CHECK(bytes.size() > 20);
        CHECK(bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0x1f &&
              static_cast<unsigned char>(bytes[1]) == 0x8b);
    }
    CHECK(r.logs.takeLog().find("[save]") != std::string::npos);
    // Save doesn't end or write the session
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());
    CHECK(r.logs.headsetLog().current() != nullptr);

    // Save with no game: still writes, under "no-game"
    Rig empty("save-empty");
    empty.run(5, true);
    const std::string s2 = empty.logs.save(empty.unixNow);
    CHECK(s2.rfind("Saved", 0) == 0);
    CHECK(filesWith(empty.logs.savesDir(), "_no-game_save.json").size() == 1);
}

void saveKeepsTwentyAtMost() {
    Rig r("save-keep");
    r.headset = session(7, "Game", kBase + 1);
    r.run(5, true);
    for (int i = 0; i < 25; ++i) {
        r.logs.save(kBase + 100000 + i * 60);
    }
    CHECK(filesWith(r.logs.savesDir(), "_save.json").size() == static_cast<size_t>(slog::kKeepSaves));
}

// ---- T11 R3: no scene app ----

/** Seconds where the PC's scene key reads ok but empty (no scene app), Streaming values still arriving. */
void runStreamNoApp(Rig& r, int seconds) {
    for (int i = 0; i < seconds; ++i) {
        r.now += 1.0;
        r.unixNow += 1;
        r.store.setText(metric::kPcStreamSceneKey, "", r.now);
        r.store.setText(metric::kPcStreamSceneName, "", r.now);
        r.store.setF32(metric::kPcStreamTotalAvg, 20.0, r.now);
        r.logs.tick(r.now, r.unixNow, r.headset, r.lastMetrics, r.store);
        const std::string l = r.logs.takeLog();
        if (!l.empty()) r.allLog += l + "\n";
    }
}

// The headset says "no app" through LINK_STATE as an empty key (same id), then id 0 after a minute: the PC copy ends where
// the app left, and counts what the headset's own log counts
void pcCopyAgreesWithTheHeadsetAboutNoApp() {
    Rig r("noapp-copy");
    r.headset = session(7, "Game", kBase + 1);
    // the headset side, as it would log the same feed itself
    const std::vector<LogColumn> cols = {
        {metric::kFrameAppFps, LogKind::Gauge, 0, 150, 150, true},
        {metric::kPowerVphW, LogKind::Gauge, 0, 30, 300, false},
    };
    slog::SessionLog headsetSide(cols, "headset");
    const auto feed = [&](const SessionInfo& s) {
        headsetSide.observe(s, r.unixNow);
        headsetSide.add(r.unixNow, {60.0f, 5.0f});
    };
    for (int i = 0; i < 120; ++i) {
        r.sec(true);
        feed(r.headset);
    }
    const int64_t left = r.unixNow + 1;  // the first tick that sees the empty key
    SessionInfo away = r.headset;
    away.appKey.clear();  // the PC copy's form
    SessionInfo awayOnHeadset = r.headset;
    awayOnHeadset.leftUnix = left;  // the headset's own form
    r.headset = away;
    for (int i = 0; i < 59; ++i) {
        r.sec(true);  // values keep arriving: they must not count
        feed(awayOnHeadset);
    }
    CHECK(filesWith(r.logs.sessionsDir(), ".json").empty());  // parked: nothing written yet
    r.headset = SessionInfo();  // the headset gave up: id 0
    r.sec(true);
    headsetSide.observe(SessionInfo(), r.unixNow);
    auto ended = headsetSide.takeEnded();
    CHECK(ended.size() == 1);

    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() != 1 || ended.size() != 1) return;
    JsonValue root;
    CHECK(parse(json[0], root));
    const double endUnix = root.get("end_unix") != nullptr ? root.get("end_unix")->number : 0;
    CHECK(std::fabs(endUnix - static_cast<double>(left)) <= 1.0);
    CHECK(std::fabs(endUnix - static_cast<double>(slog::endUnix(*ended[0]))) <= 1.0);
    const double data = root.get("data_s") != nullptr ? root.get("data_s")->number : 0;
    CHECK(std::fabs(data - static_cast<double>(ended[0]->dataSeconds)) <= 1.0);
    CHECK(std::fabs(data - 120.0) <= 1.0);
    CHECK(root.get("duration_s") != nullptr &&
          std::fabs(root.get("duration_s")->number - static_cast<double>(left - (kBase + 1))) <= 1.0);
    CHECK(filesWith(r.logs.sessionsDir(), ".csv.gz").size() == 1);
}

// The app comes back inside the minute: one session, the gap isn't data
void pcCopyKeepsOneSessionWhenTheAppReturns() {
    Rig r("noapp-back");
    r.headset = session(7, "Game", kBase + 1);
    r.run(100, true);
    SessionInfo away = r.headset;
    away.appKey.clear();
    r.headset = away;
    r.run(20, true);
    r.headset = session(7, "Game", kBase + 1);
    r.run(40, true);
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.sessionsDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 140);
    }
}

// The PC's own scene app: an ok empty key parks the stream session and a minute of it ends it at the moment it left
void pcStreamNoAppEndsTheStreamSession() {
    Rig r("stream-noapp");
    r.run(90, false, true);
    const int64_t left = r.unixNow + 1;
    runStreamNoApp(r, 1);
    const MetricValue* key = r.store.get(metric::kPcStreamSceneKey);
    CHECK(key != nullptr && key->ok && key->text.empty());  // the premise: ok and empty
    runStreamNoApp(r, 58);
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());  // parked
    runStreamNoApp(r, 2);                                   // a minute without an app
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("end_unix") != nullptr &&
              std::fabs(root.get("end_unix")->number - static_cast<double>(left)) <= 1.0);
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 90);  // the no-app values aren't data
        CHECK(root.get("source") != nullptr && root.get("source")->text == "pc stream");
    }
    // the same game afterwards is a new session
    r.run(70, false, true);
    r.logs.shutdown();
    CHECK(filesWith(r.logs.streamDir(), ".json").size() == 2);
}

void pcStreamNoAppThenBackIsOneSession() {
    Rig r("stream-noapp-back");
    r.run(80, false, true);
    runStreamNoApp(r, 30);
    r.run(40, false, true);
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 120);
    }
}

// Paused / not-yet-read say nothing about the app: the session isn't parked or ended, however long it lasts
void pcStreamPausedNeverParks() {
    Rig r("stream-paused-long");
    r.run(90, false, true);
    runStreamNa(r, 150, Reason::Paused);  // far past a minute
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    CHECK(r.allLog.find("not saved") == std::string::npos);
    runStreamNa(r, 70, Reason::NotSampledYet);
    CHECK(filesWith(r.logs.streamDir(), ".json").empty());
    r.run(30, false, true);  // the same app is still there
    r.logs.shutdown();
    const std::vector<fs::path> json = filesWith(r.logs.streamDir(), ".json");
    CHECK(json.size() == 1);  // one session all along
    if (json.size() == 1) {
        JsonValue root;
        CHECK(parse(json[0], root));
        CHECK(root.get("data_s") != nullptr && root.get("data_s")->number == 120);
    }
}

}  // namespace

int main() {
    silentHeadsetIsWrittenOnce();
    shortSessionIsNotWrittenWhenSilent();
    neverHeardIsNotSilent();
    newSessionWritesTheOldOneAfterSixtySeconds();
    detourBackToTheGameIsOneSession();
    shutdownWritesOpenSession();
    pcStreamLogFollowsThePcSceneApp();
    bothLogsAtOnce();
    saveWritesTwoFiles();
    saveKeepsTwentyAtMost();
    pcSteamVrQuitEndsStreamSession();  // review M3
    pcStreamPauseKeepsSession();
    pcStreamReconnectBlipKeepsSession();  // re-check M3
    pcSteamVrGoneEndsAtLastData();
    noSteamVrCountersAreNotData();  // re-check 2
    shortGameThenQuitIsNotSaved();
    saveCutsOldRows();                 // review M2
    pcCopyAgreesWithTheHeadsetAboutNoApp();
    pcCopyKeepsOneSessionWhenTheAppReturns();
    pcStreamNoAppEndsTheStreamSession();
    pcStreamNoAppThenBackIsOneSession();
    pcStreamPausedNeverParks();
    if (failures == 0) std::printf("pc_session_logs_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
