// T11: the PC's copy of the session logs. Two logs, each written when its session ends:
// - the headset's data follows the headset's session (LINK_STATE: id, app, start) → sessions\,
// - the PC's own Streaming stats follow the PC's OpenVR scene app (1080 / 1081) → sessions\pc-stream\.
// If the headset goes silent for more than 10 s mid-session (a crash, a power cut), the headset copy is written at
// once with the note "headset went silent at hh:mm:ss": then it may be the only copy left. Save writes the last 10
// minutes + the headset session's summary to saves\. Single-threaded (the UI thread).
#pragma once

#include "metric_store.h"
#include "session_log.h"
#include "session_tracker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fph {
namespace pc {

class PcSessionLogs {
public:
    /** The headset counts as silent after this long without METRICS. */
    static constexpr double kSilentSec = 10.0;
    /** PC SteamVR counts as quit after its scene app reads NoSteamVr this long without a break. */
    static constexpr double kSteamVrGoneSec = 10.0;

    /** @param dataDir %LOCALAPPDATA%\frame-perf-hud */
    explicit PcSessionLogs(std::string dataDir);

    /**
     * Call every UI tick; acts once a second.
     * @param now monotonic seconds (the store's clock)
     * @param unixNow wall-clock seconds
     * @param headset the headset's session from LINK_STATE (id 0 = none yet)
     * @param lastMetricsAt monotonic time of the last METRICS from the headset (< 0 = never)
     * @param store the client's store (headset values + the PC's own Streaming values)
     */
    void tick(double now, int64_t unixNow, const SessionInfo& headset, double lastMetricsAt, const MetricStore& store);

    /** Save: the last 10 minutes + the headset session's summary to saves\. @return a line for the status bar */
    std::string save(int64_t unixNow);

    /** Clean exit (Exit, Windows shutting down): both open sessions are written. */
    void shutdown();

    /** @return the last thing worth logging ("" = nothing new), then clears it */
    std::string takeLog();

    /** R3: the end card of the last headset session this PC saved (the Logs tab), empty if none yet */
    const std::vector<std::string>& lastCard() const { return lastCard_; }

    const slog::SessionLog& headsetLog() const { return headset_; }
    const slog::SessionLog& streamLog() const { return stream_; }
    std::string sessionsDir() const;
    std::string streamDir() const;
    std::string savesDir() const;

private:
    std::string dataDir_;
    std::vector<LogColumn> all_;
    slog::SessionLog headset_;  ///< headset columns (not the PC's Streaming ids)
    slog::SessionLog stream_;   ///< the PC's Streaming ids
    SessionTracker pcScene_;    ///< the PC's scene app → the stream log's sessions
    slog::RecentRing ring_;     ///< every column
    double lastTick_ = -1e9;
    int64_t lastUnix_ = 0;
    int32_t silentSavedId_ = 0;  ///< the headset session already saved for this silence
    double noSteamVrSince_ = -1;  ///< when the PC scene app started reading NoSteamVr (< 0 = it doesn't)
    std::string log_;
    std::vector<std::string> lastCard_;  ///< R3

    void writeEnded();
    void note(const std::string& line);
};

}  // namespace pc
}  // namespace fph
