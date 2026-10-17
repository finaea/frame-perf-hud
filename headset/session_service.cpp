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
        std::fprintf(stderr, "[crash] Crash recorder %s\n", on ? "on: 1 Hz ring in crash/ring.bin" : "off");
    } else {
        std::fprintf(stderr, "[crash] Crash recorder couldn't start: %s\n", error.c_str());
        ui_.crashStatus = "Crash recorder failed: " + error;
    }
    ui_.crashRecorderOn = crash_.enabled();
    changed();
}

std::vector<fph::SourceId> SessionService::pinnedSources() const {
    return crash_.enabled() ? crashrec::kitSources() : std::vector<fph::SourceId>();
}

void SessionService::tick(double now, bool looking, const fph::SessionInfo& session, const fph::MetricStore& store) {
    if (now - lastTick_ < 0.95) return;  // the loop's 2 Hz tick: every other pass
    lastTick_ = now;
    const int64_t unixNow = static_cast<int64_t>(std::time(nullptr));
    if (unixNow == lastUnix_) return;
    lastUnix_ = unixNow;
    log_.observe(session, unixNow);
    if (looking) {
        const std::vector<float> values = fph::slog::sampleColumns(store, columns_, now);
        log_.add(unixNow, values);
        if (fph::slog::anyValue(values)) ring_.add(unixNow, values);
        if (record_.isOpen()) {
            record_.add(unixNow, values, now);
            char status[96];
            const int64_t s = unixNow - recordStartUnix_;
            std::snprintf(status, sizeof(status), "Recording %lld:%02lld to saves/%s", static_cast<long long>(s / 60),
                          static_cast<long long>(s % 60), baseName(record_.path()).c_str());
            ui_.recordStatus = status;
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
        ui_.lastCard = fph::slog::endCard(*s, columns_);
        hudCard_ = ui_.lastCard;
        hudCardUntil_ = now + kHudCardSec;
        changed();
    }
}

void SessionService::setRecord(bool on, const fph::SessionInfo& session, double now) {
    if (on == record_.isOpen()) return;
    if (!on) {
        const uint64_t rows = record_.rows();
        const std::string name = baseName(record_.path());
        record_.close();
        fph::slog::pruneByCount(stateDir_ + "/saves", fph::slog::kKeepSaves);
        ui_.recordStatus = "Recorded " + std::to_string(rows) + " s to saves/" + name;
        ui_.recording = false;
        std::fprintf(stderr, "[record] Stopped: %llu rows in saves/%s\n", static_cast<unsigned long long>(rows),
                     name.c_str());
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
        ui_.recordStatus = "Recording to saves/" + baseName(path);
        std::fprintf(stderr, "[record] Started: %s (flushed every 5 s)\n", path.c_str());
    } else {
        ui_.recordStatus = "Record failed: " + error;
        std::fprintf(stderr, "[record] Couldn't start: %s\n", error.c_str());
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
    // The summary is the whole session so far; the timeline is the last 10 minutes
    fph::slog::SessionData data;
    if (const fph::slog::SessionData* current = log_.current()) {
        data.info = current->info;
        data.stats = current->stats;
        data.hist = current->hist;
        data.dataSeconds = current->dataSeconds;
        data.lastUnix = current->lastUnix;
    } else {
        data.info = named;
        data.info.startUnix = unixNow;
        data.lastUnix = unixNow;
    }
    data.source = "headset";
    data.note = "Save: summary of the session so far; the timeline is the last 10 minutes";
    int64_t ringStart = 0;
    data.timeline = ring_.timeline(kindsOf(columns_), ringStart, unixNow);
    std::string error;
    bool ok = fph::makeDirectories(dir) &&
              fph::atomicWriteFile(dir + "/" + stem + ".json", fph::slog::summaryJson(data, columns_), error);
    if (ok) {
        const std::string gz = fph::slog::gzip(fph::slog::timelineCsv(data.timeline, columns_, ringStart));
        ok = !gz.empty() && fph::atomicWriteFile(dir + "/" + stem + ".csv.gz", gz, error);
    }
    if (ok) {
        fph::slog::pruneByCount(dir, fph::slog::kKeepSaves);
        ui_.saveStatus = "Saved " + fph::slog::clockTime(unixNow) + ": saves/" + stem + " (" +
                         std::to_string(ring_.sizeWithin(unixNow)) + " s)";
        std::fprintf(stderr, "[save] %s/%s.json + .csv.gz (%zu s of the last 10 min)\n", dir.c_str(), stem.c_str(),
                     ring_.sizeWithin(unixNow));
    } else {
        ui_.saveStatus = "Save failed: " + (error.empty() ? std::string("can't write ") + dir : error);
        std::fprintf(stderr, "[save] failed: %s\n", ui_.saveStatus.c_str());
    }
    changed();
}

void SessionService::shutdown() {
    log_.endAll();
    writeEnded(0);
    if (record_.isOpen()) {
        record_.close();
        fph::slog::pruneByCount(stateDir_ + "/saves", fph::slog::kKeepSaves);
    }
    ui_.recording = false;
    crash_.shutdown();
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
