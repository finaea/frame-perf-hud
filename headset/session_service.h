// T11: session logs on the headset, wired into the main loop. Once a second it follows the session (SensorBridge's
// SessionTracker), samples the log columns while someone is looking, writes a session's files when it ends, feeds
// the 10-minute ring (Save), Record and the crash recorder. Nothing touches the disk during play unless Record or
// the crash recorder is on: a session's files are written once, when it ends.
#pragma once

#include "crash_recorder.h"
#include "logs_ui.h"
#include "metric_store.h"
#include "session_log.h"
#include "session_tracker.h"

#include <cstdint>
#include <string>
#include <vector>

class SessionService {
public:
    /** How long the end card stays on the HUD. */
    static constexpr double kHudCardSec = 10.0;

    /** @param stateDir ~/.local/state/frame-perf-hud (sessions/, saves/, crashes/, crash/ go under it) */
    explicit SessionService(std::string stateDir);

    /**
     * At agent start: the last power-off reason (once per boot), a crash log left by an unclean end, then the crash
     * recorder as the config says. Sets general.last_power_off / general.crash_log_note.
     */
    void start(bool crashRecorder, fph::MetricStore& store, double now);

    /**
     * Call every loop pass; it acts once per clock second, on the first pass in it.
     * @param looking someone is looking (only then are the columns sampled: a paused source is a gap, not data)
     */
    void tick(double now, bool looking, const fph::SessionInfo& session, const fph::MetricStore& store);
    /** tick() with the clock second given (tests drive it with their own). */
    void tickAt(double now, int64_t unixNow, bool looking, const fph::SessionInfo& session,
                const fph::MetricStore& store);

    /** Record on / off (Record also counts as someone looking: the caller puts it in the demand). */
    void setRecord(bool on, const fph::SessionInfo& session, double now);
    bool recording() const { return record_.isOpen(); }

    /** Save: the last 10 minutes + the session's summary to saves/, now. */
    void save(const fph::SessionInfo& session, double now);

    /** The crash recorder on / off (the config's crash_recorder). */
    void setCrashRecorder(bool on);
    /** @return the sources the scheduler must keep reading for the crash recorder (empty when it's off) */
    std::vector<fph::SourceId> pinnedSources() const;

    /** Clean exit (SIGTERM, SteamVR quitting, Quit): writes the open session, ends Record, removes the crash marker. */
    void shutdown();

    const LogsUiState& ui() const { return ui_; }
    /** @return the end card while it's on the HUD (kHudCardSec after a session was saved), else empty */
    std::vector<std::string> hudCard(double now) const;
    /** @return a few lines for the debug dump */
    std::string debugText() const;

private:
    std::string stateDir_;
    std::vector<fph::LogColumn> columns_;
    fph::slog::SessionLog log_;
    fph::slog::RecentRing ring_;
    fph::slog::RecordWriter record_;
    int64_t recordStartUnix_ = 0;
    crashrec::CrashRecorder crash_;
    LogsUiState ui_;
    int64_t lastUnix_ = 0;
    std::vector<std::string> hudCard_;
    double hudCardUntil_ = 0;
    uint64_t sessionsWritten_ = 0;
    uint64_t sessionsSkipped_ = 0;

    void writeEnded(double now);
    void changed() { ++ui_.revision; }
};
