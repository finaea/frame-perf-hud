// T11: fpsVR-style session logs, kept in RAM during play and written once when a game ends (headset and PC).
// - A Timeline holds one row per second of the log columns, 5 s rows past 4 h, coarser again past ~4 MB.
// - Each column keeps running stats and a fixed-bin histogram (p95 without keeping every sample).
// - SessionLog follows the session id (SessionTracker / LINK_STATE). The session it leaves is parked until the new one
//   has run kSessionDetourSec, so a short detour that comes back (game → Home → game) is merged, not split. While the
//   session's app is gone (leftUnix) nothing is added, and id 0 (no app for kSessionDetourSec) ends it.
// - RecentRing: the last 10 minutes at 1 s, for Save.
// - Files: <start>_<app>.json (summary) + .csv.gz (timeline), written atomically; old ones pruned by count.
// Platform-free (zlib for the .gz, std::filesystem for listing).
#pragma once

#include "metric_registry.h"
#include "metric_store.h"
#include "session_tracker.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fph {
namespace slog {

constexpr int64_t kMinSessionSec = 60;          ///< sessions with less data than this aren't saved
constexpr int64_t kFullRateSec = 4 * 3600;      ///< 1 s rows up to here
constexpr int kDownsampledStepSec = 5;          ///< then 5 s rows
constexpr size_t kTimelineCapBytes = 4u << 20;  ///< past this, the whole timeline is halved again
constexpr int kRecentSec = 600;                 ///< Save's window
constexpr int kKeepSessions = 10;
constexpr int kKeepSaves = 20;
constexpr int kKeepCrashLogs = 5;

/**
 * Reads the columns' current values. A value counts only while fresh (sampled within 2.5 read intervals, at least
 * 3 s), so a paused source gives NaN, not its last value.
 * @param store values
 * @param columns the log columns
 * @param now monotonic seconds (the store's clock)
 * @return one value per column, NaN = none
 */
std::vector<float> sampleColumns(const MetricStore& store, const std::vector<LogColumn>& columns, double now);

/** @return true if any value isn't NaN */
bool anyValue(const std::vector<float>& values);

/** Fixed-bin histogram. */
class Histogram {
public:
    Histogram() = default;
    Histogram(float min, float max, uint16_t bins);
    void add(float v);
    void merge(const Histogram& other);
    /** @return the p-th fraction (0.95 = p95), linear inside the bin; NaN if empty */
    double percentile(double p) const;
    uint64_t total() const { return total_; }
    const std::vector<uint32_t>& counts() const { return counts_; }
    float min() const { return min_; }
    float max() const { return max_; }

private:
    float min_ = 0, max_ = 0;
    std::vector<uint32_t> counts_;
    uint64_t total_ = 0;
};

/** Running stats of one column. */
struct ColumnStats {
    uint64_t samples = 0;
    double sum = 0;
    float min = 0, max = 0;
    float first = 0, last = 0;  ///< Cumulative: the session's count is last − first (plus resets, below)
    double counted = 0;         ///< Count: sum; Cumulative: increase (a drop = the source restarted); Flag: rises
    bool high = false;          ///< Flag: last value was non-zero
    bool rebase = false;        ///< Cumulative, T11: back after no app: the next value is a new baseline, not a rise

    void add(LogKind kind, float v);
    /** @param bridge Cumulative: count the rise between this one's last value and later's first */
    void merge(LogKind kind, const ColumnStats& later, bool bridge = true);
    double avg() const { return samples ? sum / static_cast<double>(samples) : 0.0; }
};

/** One second (or more, downsampled) of every column. */
struct TimelineRow {
    int64_t offset = 0;  ///< seconds since the session start
    int duration = 1;    ///< seconds this row covers
    const float* values = nullptr;
};

/** The whole session at 1 s, 5 s past 4 h, coarser past the byte cap. */
class Timeline {
public:
    Timeline() = default;
    /** @param kinds one per column (how a row is merged) */
    Timeline(std::vector<LogKind> kinds, size_t capBytes = kTimelineCapBytes);

    /** Adds one 1 s sample. Offsets must grow; a repeat of the last offset is ignored. */
    void add(int64_t offset, const std::vector<float>& values);
    /** Appends another session's rows (resume after a detour), shifted by delta seconds. */
    void append(const Timeline& other, int64_t delta);
    /** Ends a partial downsampled row so rows() includes it. */
    void flush();

    size_t rows() const;
    TimelineRow row(size_t i) const;
    size_t bytes() const { return data_.capacity() * sizeof(float); }
    int step() const { return step_; }
    size_t columns() const { return kinds_.size(); }

private:
    std::vector<LogKind> kinds_;
    size_t cap_ = kTimelineCapBytes;
    std::vector<float> data_;  ///< rows of (offset, duration, values...)
    int step_ = 1;
    int64_t lastOffset_ = -1;
    // the row being built at step_ > 1
    std::vector<double> acc_;
    std::vector<uint32_t> accN_;
    int64_t accStart_ = -1;
    int64_t accEnd_ = -1;

    size_t stride() const { return kinds_.size() + 2; }
    void push(int64_t offset, int duration, const float* values);
    void halve();
};

/** One session's data. */
struct SessionData {
    SessionInfo info;
    std::string source;          ///< "headset", "pc copy", "pc stream"
    std::string note;            ///< e.g. "headset went silent at 13:05:09"
    int64_t lastUnix = 0;        ///< last sample
    int64_t dataSeconds = 0;     ///< seconds with at least one value
    std::vector<ColumnStats> stats;
    std::vector<Histogram> hist;
    Timeline timeline;
};

/** Follows one stream of sessions. Single-threaded. */
class SessionLog {
public:
    /**
     * @param columns the columns this log keeps (a subset of sessionLogColumns() on the PC)
     * @param source written into the summary
     */
    SessionLog(std::vector<LogColumn> columns, std::string source);

    /**
     * Follows the session (call on every change and every tick). id 0 = none: ends the open sessions. A session with
     * an empty app key has no app now (LINK_STATE's form of leftUnix): parked from the first time it's seen so.
     */
    void observe(const SessionInfo& session, int64_t nowUnix);
    /** One 1 s sample for the current session (values from sampleColumns). Ignored with no session or no app. */
    void add(int64_t nowUnix, const std::vector<float>& values);
    /** Ends every open session (agent exit). */
    void endAll();
    /** @return sessions that ended since the last call, oldest first (short ones too: check dataSeconds) */
    std::vector<std::unique_ptr<SessionData>> takeEnded();

    /** @return the open session, nullptr if none */
    const SessionData* current() const { return current_.get(); }
    SessionData* current() { return current_.get(); }
    const std::vector<LogColumn>& columns() const { return columns_; }
    /** @return RAM held by the open (and parked) sessions' timelines */
    size_t bytes() const;

private:
    std::vector<LogColumn> columns_;
    std::vector<LogKind> kinds_;
    std::string source_;
    std::unique_ptr<SessionData> current_;
    std::unique_ptr<SessionData> parked_;  ///< the session before a possible detour
    std::vector<std::unique_ptr<SessionData>> ended_;

    std::unique_ptr<SessionData> start(const SessionInfo& info) const;
    void end(std::unique_ptr<SessionData> s);
};

/** The last kRecentSec seconds at 1 s, for Save. */
class RecentRing {
public:
    explicit RecentRing(size_t columns, int seconds = kRecentSec);
    void add(int64_t unixTime, const std::vector<float>& values);
    /**
     * @param nowUnix Save's time: samples older than nowUnix − the ring's seconds are left out (a pause leaves no rows,
     *                so the 600 rows can be far older than 10 minutes)
     * @return the samples in the window, oldest first, as a Timeline with offsets from the oldest one; startUnix gets
     *         its time (nowUnix when the window is empty)
     */
    Timeline timeline(const std::vector<LogKind>& kinds, int64_t& startUnix, int64_t nowUnix) const;
    size_t size() const { return count_; }
    /** @return how many samples are in the window ending at nowUnix */
    size_t sizeWithin(int64_t nowUnix) const;

private:
    size_t columns_;
    size_t capacity_;
    int seconds_;
    std::vector<float> values_;
    std::vector<int64_t> times_;
    size_t next_ = 0;
    size_t count_ = 0;
};

// ---- files ----

/** @return local time as "YYYY-MM-DD_HHMMSS" (file names sort by time) */
std::string fileTime(int64_t unixTime);
/** @return local "hh:mm:ss" */
std::string clockTime(int64_t unixTime);
/** @return the app name (key if no name) with anything but [A-Za-z0-9._-] made '-', at most 40 bytes */
std::string fileSafeApp(const SessionInfo& info);

/** @return when the session ended: when its app left, else its last sample (never before its start) */
int64_t endUnix(const SessionData& s);

/** @return the summary as JSON (counts, avg/min/max/p5/p95 per column, histograms, end card) */
std::string summaryJson(const SessionData& s, const std::vector<LogColumn>& columns);
/** @return the timeline as CSV; columns with no value in the whole timeline are left out */
std::string timelineCsv(const Timeline& t, const std::vector<LogColumn>& columns, int64_t startUnix);
/** @return text gzip-compressed (empty on failure) */
std::string gzip(const std::string& text);

/** The end-of-session card, a few short lines (duration, fps, power, battery, temps, streaming). */
std::vector<std::string> endCard(const SessionData& s, const std::vector<LogColumn>& columns);

/**
 * Writes <dir>/<stem>.json and <stem>.csv.gz atomically, then keeps the newest `keep` stems in dir.
 * @return true if both files are in place
 */
bool writeSession(const std::string& dir, const std::string& stem, const SessionData& s,
                  const std::vector<LogColumn>& columns, int keep, std::string& error);

/**
 * @return true if name is one of our log files: "YYYY-MM-DD_HHMMSS_<app...>" from [A-Za-z0-9_-] plus ".json",
 *         ".csv.gz" or ".csv"; stem gets the name without the suffix
 */
bool ownedLogFile(const std::string& name, std::string& stem);

/**
 * Deletes the oldest of our log files (ownedLogFile) in dir beyond `keep` distinct stems. Names start with the time,
 * so they sort oldest first. Anything else in the folder is neither counted nor touched.
 */
void pruneByCount(const std::string& dir, int keep);

/** Appends rows to a CSV file (Record), flushed every flushSec. */
class RecordWriter {
public:
    ~RecordWriter();
    bool open(const std::string& path, const std::vector<LogColumn>& columns, std::string& error);
    /** Adds one row; flushes to the file when flushSec passed since the last flush. */
    void add(int64_t unixTime, const std::vector<float>& values, double now, double flushSec = 5.0);
    void close();
    bool isOpen() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
    uint64_t rows() const { return rows_; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::string pending_;
    double lastFlush_ = -1e18;
    uint64_t rows_ = 0;
};

}  // namespace slog
}  // namespace fph
