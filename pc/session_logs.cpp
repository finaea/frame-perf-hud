#include "session_logs.h"

#include "atomic_file.h"
#include "stream_metrics.h"

namespace fph {
namespace pc {

namespace {

std::vector<LogColumn> pick(bool pcStream) {
    std::vector<LogColumn> out;
    for (const LogColumn& c : sessionLogColumns()) {
        if (isPcStreamMetric(c.id) == pcStream) out.push_back(c);
    }
    return out;
}

/** @return the ShortString metric's text, "" when N/A */
std::string textOf(const MetricStore& store, uint16_t id) {
    const MetricValue* v = store.get(id);
    return v != nullptr && v->ok ? v->text : std::string();
}

std::string stemOf(const slog::SessionData& s) {
    return slog::fileTime(s.info.startUnix) + "_" + slog::fileSafeApp(s.info);
}

std::string appOf(const slog::SessionData& s) {
    return s.info.appName.empty() ? s.info.appKey : s.info.appName;
}

std::vector<LogKind> kindsOf(const std::vector<LogColumn>& columns) {
    std::vector<LogKind> kinds;
    for (const LogColumn& c : columns) kinds.push_back(c.kind);
    return kinds;
}

}  // namespace

PcSessionLogs::PcSessionLogs(std::string dataDir)
    : dataDir_(std::move(dataDir)),
      all_(sessionLogColumns()),
      headset_(pick(false), "pc copy"),
      stream_(pick(true), "pc stream"),
      ring_(all_.size()) {}

std::string PcSessionLogs::sessionsDir() const {
    return dataDir_ + "\\sessions";
}

std::string PcSessionLogs::streamDir() const {
    return dataDir_ + "\\sessions\\pc-stream";
}

std::string PcSessionLogs::savesDir() const {
    return dataDir_ + "\\saves";
}

void PcSessionLogs::note(const std::string& line) {
    log_ += (log_.empty() ? "" : "\n") + line;
}

std::string PcSessionLogs::takeLog() {
    std::string out;
    out.swap(log_);
    return out;
}

void PcSessionLogs::tick(double now, int64_t unixNow, const SessionInfo& headset, double lastMetricsAt,
                         const MetricStore& store) {
    if (now - lastTick_ < 0.95) return;
    lastTick_ = now;
    if (unixNow == lastUnix_) return;
    lastUnix_ = unixNow;

    headset_.observe(headset, unixNow);
    // PC SteamVR gone (the reader marks the scene NoSteamVr for kSteamVrGoneSec straight): the stream session ends, and
    // the same game after a SteamVR restart is a new one. Shorter NoSteamVr stretches are the reader reconnecting
    // (a Streaming pause → resume), not a quit. An empty key (the compositor) and Paused keep the session open
    const MetricValue* scene = store.get(metric::kPcStreamSceneKey);
    const bool noSteamVr = scene != nullptr && !scene->ok && scene->reason == Reason::NoSteamVr;
    if (noSteamVr) {
        if (noSteamVrSince_ < 0) noSteamVrSince_ = now;
        if (now - noSteamVrSince_ >= kSteamVrGoneSec && pcScene_.current().id != 0) {
            // the summary's end is the session's last data second, not now
            note("[session] PC SteamVR gone for " + std::to_string(static_cast<int>(kSteamVrGoneSec)) +
                 " s: the PC stream session ends");
            stream_.endAll();
            pcScene_ = SessionTracker();
        }
    } else {
        noSteamVrSince_ = -1;
        pcScene_.update(textOf(store, metric::kPcStreamSceneKey), textOf(store, metric::kPcStreamSceneName), unixNow);
        stream_.observe(pcScene_.current(), unixNow);
    }
    // A paused source or a silent headset gives NaN (not fresh), so those seconds are gaps, not data
    headset_.add(unixNow, slog::sampleColumns(store, headset_.columns(), now));
    // Without SteamVR the reader still republishes driver_vrlink.txt's cumulative counters: not stream data, so the
    // session's data and end stay at its last real second
    if (!noSteamVr) stream_.add(unixNow, slog::sampleColumns(store, stream_.columns(), now));
    const std::vector<float> all = slog::sampleColumns(store, all_, now);
    if (slog::anyValue(all)) ring_.add(unixNow, all);

    // The headset went quiet mid-session: write its copy now, it may be the only one left
    slog::SessionData* cur = headset_.current();
    const bool silent = lastMetricsAt >= 0 && now - lastMetricsAt > kSilentSec;
    if (!silent) {
        silentSavedId_ = 0;
    } else if (cur != nullptr && cur->info.id != silentSavedId_ && cur->dataSeconds >= slog::kMinSessionSec) {
        silentSavedId_ = cur->info.id;
        const int64_t heardUnix = unixNow - static_cast<int64_t>(now - lastMetricsAt);
        cur->note = "headset went silent at " + slog::clockTime(heardUnix);
        slog::SessionData copy = *cur;
        copy.timeline.flush();
        std::string error;
        if (slog::writeSession(sessionsDir(), stemOf(copy), copy, headset_.columns(), slog::kKeepSessions, error)) {
            note("[session] headset silent: saved its session now: sessions\\" + stemOf(copy) + ".json (" +
                 std::to_string(copy.dataSeconds) + " s, " + cur->note + ")");
        } else {
            note("[session] headset silent, but its session couldn't be saved: " + error);
        }
    }
    writeEnded();
}

void PcSessionLogs::writeEnded() {
    const struct {
        slog::SessionLog* log;
        std::string dir;
    } logs[] = {{&headset_, sessionsDir()}, {&stream_, streamDir()}};
    for (const auto& l : logs) {
        for (const auto& s : l.log->takeEnded()) {
            if (s->dataSeconds < slog::kMinSessionSec) {
                note("[session] " + s->source + " session " + std::to_string(s->info.id) + " (" + appOf(*s) +
                     ") ended with " + std::to_string(s->dataSeconds) + " s of data: not saved");
                continue;
            }
            std::string error;
            if (slog::writeSession(l.dir, stemOf(*s), *s, l.log->columns(), slog::kKeepSessions, error)) {
                note("[session] " + s->source + " session saved: " + l.dir + "\\" + stemOf(*s) + ".json (" +
                     std::to_string(s->dataSeconds) + " s of data)");
                if (l.log == &headset_) lastCard_ = slog::endCard(*s, l.log->columns());  // R3: the Logs tab
            } else {
                note("[session] " + s->source + " session couldn't be saved: " + error);
            }
        }
    }
}

std::string PcSessionLogs::save(int64_t unixNow) {
    slog::SessionData data;
    if (const slog::SessionData* cur = headset_.current()) {
        data.info = cur->info;
        data.dataSeconds = cur->dataSeconds;
        data.lastUnix = cur->lastUnix;
        // the headset session's summary, in the order of all_ (headset columns, then nothing for the PC's)
        data.stats.resize(all_.size());
        data.hist.resize(all_.size());
        for (size_t i = 0, h = 0; i < all_.size(); ++i) {
            if (h < headset_.columns().size() && headset_.columns()[h].id == all_[i].id) {
                data.stats[i] = cur->stats[h];
                data.hist[i] = cur->hist[h];
                ++h;
            }
        }
    } else {
        data.info.appName = "no-game";
        data.info.startUnix = unixNow;
        data.lastUnix = unixNow;
    }
    data.source = "pc copy";
    data.note = "Save: summary of the headset session so far; the timeline is the last 10 minutes";
    int64_t ringStart = 0;
    data.timeline = ring_.timeline(kindsOf(all_), ringStart, unixNow);
    const std::string stem = slog::fileTime(unixNow) + "_" + slog::fileSafeApp(data.info) + "_save";
    std::string error;
    bool ok = makeDirectories(savesDir()) &&
              atomicWriteFile(savesDir() + "\\" + stem + ".json", slog::summaryJson(data, all_), error);
    if (ok) {
        const std::string gz = slog::gzip(slog::timelineCsv(data.timeline, all_, ringStart));
        ok = !gz.empty() && atomicWriteFile(savesDir() + "\\" + stem + ".csv.gz", gz, error);
    }
    if (!ok) {
        const std::string why = error.empty() ? "can't write " + savesDir() : error;
        note("[save] failed: " + why);
        return "Save failed: " + why;
    }
    slog::pruneByCount(savesDir(), slog::kKeepSaves);
    const size_t seconds = ring_.sizeWithin(unixNow);
    note("[save] " + savesDir() + "\\" + stem + ".json + .csv.gz (" + std::to_string(seconds) + " s)");
    return "Saved the last " + std::to_string((seconds + 30) / 60) + " min to saves\\" + stem;
}

void PcSessionLogs::shutdown() {
    headset_.endAll();
    stream_.endAll();
    writeEnded();
}

}  // namespace pc
}  // namespace fph
