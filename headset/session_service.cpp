#include "session_service.h"

#include "atomic_file.h"
#include "power_off.h"

#include <cstdio>
#include <ctime>

namespace {

std::vector<fph::LogKind> kindsOf(const std::vector<fph::LogColumn>& columns) {
    std::vector<fph::LogKind> kinds;
    for (const fph::LogColumn& c : columns) kinds.push_back(c.kind);
    return kinds;
}

/** @return the file name part of a path */
std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

SessionService::SessionService(std::string stateDir)
    : stateDir_(std::move(stateDir)),
      columns_(fph::sessionLogColumns()),
      log_(columns_, "headset"),
      ring_(columns_.size()),
      crash_(stateDir_) {}

void SessionService::start(bool crashRecorder, fph::MetricStore& store, double now) {
    using namespace fph::metric;
    std::string log;
    ui_.powerOff = poweroff::lastPowerOff(stateDir_, log);
    if (!log.empty()) std::fprintf(stderr, "[power] %s\n", log.c_str());
    if (ui_.powerOff.empty()) {
        store.setNa(kLastPowerOff, fph::Reason::ReadFailed, now);
    } else {
        store.setText(kLastPowerOff, ui_.powerOff, now);
    }
    const std::string note = crash_.recover(log);
    if (!log.empty()) std::fprintf(stderr, "[crash] %s\n", log.c_str());
    ui_.crashStatus = note;
    if (note.empty()) {
        store.setNa(kCrashLogNote, fph::Reason::NotConnected, now);
    } else {
        store.setText(kCrashLogNote, note, now);
    }
    setCrashRecorder(crashRecorder);
    changed();
}

void SessionService::setCrashRecorder(bool on) {
    if (on == crash_.enabled()) return;
    std::string error;
    if (crash_.setEnabled(on, error)) {
        std::fprintf(stderr, "[crash] Crash Record %s\n", on ? "on: 1 Hz ring in crash/ring.bin" : "off");
    } else {
        std::fprintf(stderr, "[crash] Crash Record couldn't start: %s\n", error.c_str());
        ui_.crashStatus = "Crash Record failed: " + error;
    }
    ui_.crashRecorderOn = crash_.enabled();
    changed();
}

void SessionService::setSessionLogs(bool on) {
    if (on == sessionLogs_) return;
    sessionLogs_ = on;
    if (!on) {
        log_.endAll();
        const size_t dropped = log_.takeEnded().size();  // the user turned it off: nothing is written
        hudCard_.clear();
        hudCardUntil_ = 0;
        std::fprintf(stderr, "[session] Session Record off%s\n", dropped ? " (the open session wasn't saved)" : "");
    } else {
        std::fprintf(stderr, "[session] Session Record on\n");
    }
    changed();
}

std::vector<fph::SourceId> SessionService::pinnedSources() const {
    return crash_.enabled() ? crashrec::kitSources() : std::vector<fph::SourceId>();
}

void SessionService::tick(double now, bool looking, const fph::SessionInfo& session, const fph::MetricStore& store) {
    tickAt(now, static_cast<int64_t>(std::time(nullptr)), looking, session, store);
}

void SessionService::tickAt(double now, int64_t unixNow, bool looking, const fph::SessionInfo& session,
                            const fph::MetricStore& store) {
    // Once per clock second, on the first loop pass in it. The loop passes at least twice a second, so every second
    // gets its row even when passes run late; the old 0.95 s stopwatch gate skipped every other second then
    if (unixNow == lastUnix_) return;
    lastUnix_ = unixNow;
    if (sessionLogs_) log_.observe(session, unixNow);
    if (looking) {
        const std::vector<float> values = fph::slog::sampleColumns(store, columns_, now);
        if (sessionLogs_) log_.add(unixNow, values);
        if (fph::slog::anyValue(values)) ring_.add(unixNow, values);
        if (record_.isOpen()) {
            record_.add(unixNow, values, now);
            if (fph::slog::anyValue(values)) fph::slog::addToSummary(recordSummary_, columns_, unixNow, values);
            char line[48];
            const int64_t s = unixNow - recordStartUnix_;
            std::snprintf(line, sizeof(line), "Recording %lld:%02lld", static_cast<long long>(s / 60),
                          static_cast<long long>(s % 60));
            ui_.recordLine = line;
            ui_.recordStatus = ui_.recordLine + " to " + ui_.recordPath;
            changed();
        }
    }
    crash_.tick(store, now, unixNow);
    writeEnded(now);
}

void SessionService::writeEnded(double now) {
    for (const auto& s : log_.takeEnded()) {
        const std::string app = s->info.appName.empty() ? s->info.appKey : s->info.appName;
        if (s->dataSeconds < fph::slog::kMinSessionSec) {
            ++sessionsSkipped_;
            std::fprintf(stderr, "[session] Session %d (%s) ended with %lld s of data: not saved (under %lld s)\n",
                         s->info.id, app.c_str(), static_cast<long long>(s->dataSeconds),
                         static_cast<long long>(fph::slog::kMinSessionSec));
            continue;
        }
        const std::string stem = fph::slog::fileTime(s->info.startUnix) + "_" + fph::slog::fileSafeApp(s->info);
        std::string error;
        if (fph::slog::writeSession(stateDir_ + "/sessions", stem, *s, columns_, fph::slog::kKeepSessions, error)) {
            ++sessionsWritten_;
            std::fprintf(stderr, "[session] Session %d (%s) saved: sessions/%s.json (%lld s of data, %zu rows)\n",
                         s->info.id, app.c_str(), stem.c_str(), static_cast<long long>(s->dataSeconds),
                         s->timeline.rows());
        } else {
            std::fprintf(stderr, "[session] Session %d (%s) couldn't be saved: %s\n", s->info.id, app.c_str(),
                         error.c_str());
        }
        ui_.lastCard = fph::slog::shortCard(*s, columns_);  // the Logs tab: 3 lines
        hudCard_ = fph::slog::endCard(*s, columns_);       // the HUD's 10 s card: the whole card
        hudCardUntil_ = now + kHudCardSec;
        changed();
    }
}

void SessionService::setRecord(bool on, const fph::SessionInfo& session, double now) {
    if (on == record_.isOpen()) return;
    if (!on) {
        const uint64_t rows = record_.rows();
        const std::string name = baseName(record_.path());
        closeRecord();
        ui_.recordLine = "Recorded " + std::to_string(rows) + " s";
        ui_.recordPath = "saves/" + name;
        ui_.recordStatus = ui_.recordLine + " to " + ui_.recordPath;
        ui_.recording = false;
        std::fprintf(stderr, "[record] Direct Record stopped: %llu rows in saves/%s + .json\n",
                     static_cast<unsigned long long>(rows), name.c_str());
        changed();
        return;
    }
    (void)now;
    recordStartUnix_ = static_cast<int64_t>(std::time(nullptr));
    fph::SessionInfo named = session;
    if (named.id == 0) named.appName = "no-game";
    const std::string path = stateDir_ + "/saves/" + fph::slog::fileTime(recordStartUnix_) + "_" +
                             fph::slog::fileSafeApp(named) + "_record.csv";
    std::string error;
    if (record_.open(path, columns_, error)) {
        fph::SessionInfo window = named;
        window.startUnix = recordStartUnix_;
        recordSummary_ = fph::slog::emptySummary(window, columns_, "headset");
        recordSummary_.note = "Direct Record: summary of the recorded window";
        ui_.recordLine = "Recording 0:00";
        ui_.recordPath = "saves/" + baseName(path);
        ui_.recordStatus = "Recording to " + ui_.recordPath;
        std::fprintf(stderr, "[record] Direct Record started: %s (flushed every 5 s)\n", path.c_str());
    } else {
        ui_.recordLine = "Direct Record failed: " + error;
        ui_.recordPath.clear();
        ui_.recordStatus = ui_.recordLine;
        std::fprintf(stderr, "[record] Direct Record couldn't start: %s\n", error.c_str());
    }
    ui_.recording = record_.isOpen();  // a failed start stays visibly off
    changed();
}

void SessionService::save(const fph::SessionInfo& session, double now) {
    (void)now;
    const int64_t unixNow = static_cast<int64_t>(std::time(nullptr));
    fph::SessionInfo named = session;
    if (named.id == 0) named.appName = "no-game";
    const std::string stem = fph::slog::fileTime(unixNow) + "_" + fph::slog::fileSafeApp(named) + "_save";
    const std::string dir = stateDir_ + "/saves";
    // The timeline is the last 10 minutes, and the summary is that window's own (always). While Session Record is
    // on, the open session's summary so far rides along as "session_so_far"
    int64_t ringStart = 0;
    fph::slog::Timeline window = ring_.timeline(kindsOf(columns_), ringStart, unixNow);
    fph::SessionInfo windowInfo = named;
    windowInfo.startUnix = ringStart;
    fph::slog::SessionData data = fph::slog::emptySummary(windowInfo, columns_, "headset");
    for (size_t i = 0; i < window.rows(); ++i) {
        const fph::slog::TimelineRow row = window.row(i);
        const std::vector<float> values(row.values, row.values + window.columns());
        fph::slog::addToSummary(data, columns_, ringStart + row.offset, values);
    }
    if (data.lastUnix == 0) data.lastUnix = unixNow;  // an empty window still says when it was saved
    const fph::slog::SessionData* current = sessionLogs_ ? log_.current() : nullptr;
    data.note = current != nullptr
                    ? "Direct Save: summary and timeline of the last 10 minutes; session_so_far is the open session's "
                      "summary so far"
                    : "Direct Save: summary and timeline of the last 10 minutes (Session Record is off or no session "
                      "is open: no session summary)";
    data.timeline = std::move(window);
    std::string json = fph::slog::summaryJson(data, columns_);
    if (current != nullptr) {
        fph::slog::SessionData so;  // the stats only: its timeline stays in RAM
        so.info = current->info;
        so.source = current->source;
        so.stats = current->stats;
        so.hist = current->hist;
        so.dataSeconds = current->dataSeconds;
        so.lastUnix = current->lastUnix;
        // summaryJson ends "\n}\n": the session's object goes in as one more key
        const size_t end = json.rfind('}');
        if (end != std::string::npos) {
            std::string inner = fph::slog::summaryJson(so, columns_);
            while (!inner.empty() && inner.back() == '\n') inner.pop_back();
            json = json.substr(0, end);
            while (!json.empty() && json.back() == '\n') json.pop_back();
            json += ",\n  \"session_so_far\": " + inner + "\n}\n";
        }
    }
    std::string error;
    bool ok = fph::makeDirectories(dir) && fph::atomicWriteFile(dir + "/" + stem + ".json", json, error);
    if (ok) {
        const std::string gz = fph::slog::gzip(fph::slog::timelineCsv(data.timeline, columns_, ringStart));
        ok = !gz.empty() && fph::atomicWriteFile(dir + "/" + stem + ".csv.gz", gz, error);
    }
    if (ok) {
        fph::slog::pruneByCount(dir, fph::slog::kKeepSaves);
        const std::string seconds = std::to_string(ring_.sizeWithin(unixNow));
        ui_.saveStatus = "Saved " + fph::slog::clockTime(unixNow) + ": saves/" + stem + " (" + seconds + " s)";
        ui_.saveLine = "Saved " + fph::slog::clockTime(unixNow) + " (" + seconds + " s)";
        ui_.savePath = "saves/" + stem;
        std::fprintf(stderr, "[save] Direct Save: %s/%s.json + .csv.gz (%zu s of the last 10 min)\n", dir.c_str(),
                     stem.c_str(), ring_.sizeWithin(unixNow));
    } else {
        ui_.saveStatus = "Direct Save failed: " + (error.empty() ? std::string("can't write ") + dir : error);
        ui_.saveLine = ui_.saveStatus;
        ui_.savePath.clear();
        std::fprintf(stderr, "[save] %s\n", ui_.saveStatus.c_str());
    }
    changed();
}

void SessionService::shutdown() {
    log_.endAll();
    writeEnded(0);
    if (record_.isOpen()) closeRecord();
    ui_.recording = false;
    crash_.shutdown();
}

void SessionService::closeRecord() {
    std::string json = record_.path();
    record_.close();
    const size_t dot = json.rfind(".csv");
    json = (dot == std::string::npos ? json : json.substr(0, dot)) + ".json";
    std::string error;
    if (!fph::atomicWriteFile(json, fph::slog::summaryJson(recordSummary_, columns_), error)) {
        std::fprintf(stderr, "[record] Direct Record's summary couldn't be written: %s\n", error.c_str());
    }
    recordSummary_ = fph::slog::SessionData();
    fph::slog::pruneByCount(stateDir_ + "/saves", fph::slog::kKeepSaves);
}

std::vector<std::string> SessionService::hudCard(double now) const {
    return now < hudCardUntil_ ? hudCard_ : std::vector<std::string>();
}

std::string SessionService::debugText() const {
    char line[320];
    const fph::slog::SessionData* s = log_.current();
    std::snprintf(line, sizeof(line),
                  "== Session log ==\nopen session %d (%s): %lld s of data, %zu rows at %d s, %zu KB RAM | saved %llu, "
                  "skipped %llu | ring %zu s | record %s | crash recorder %s (%llu records)\n",
                  s ? s->info.id : 0, s ? s->info.appKey.c_str() : "-", s ? static_cast<long long>(s->dataSeconds) : 0LL,
                  s ? s->timeline.rows() : 0, s ? s->timeline.step() : 0, log_.bytes() / 1024,
                  static_cast<unsigned long long>(sessionsWritten_), static_cast<unsigned long long>(sessionsSkipped_),
                  ring_.size(), record_.isOpen() ? "on" : "off", crash_.enabled() ? "on" : "off",
                  static_cast<unsigned long long>(crash_.written()));
    return line;
}
