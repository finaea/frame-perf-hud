// Bridge between upstream's readers (Sensors, VrOverlay) and the metric registry.
// Each scheduler source calls one reader and stores its metrics in the MetricStore. The upstream panel
// then gets its SensorSample / FrameStats / ControllerStatus rebuilt from the store, so everything it
// shows has gone through the registry. Ticket 04 replaces the upstream panel with widgets that read the
// store directly.
#pragma once

#include "history.h"
#include "layout.h"         // T04
#include "metric_store.h"
#include "process_scan.h"   // T03
#include "scheduler.h"
#include "sensors.h"
#include "session_tracker.h"  // T03
#include "stream_link.h"    // T03
#include "wifi_link.h"      // T03
#include "widgets.h"        // T04: ProcessListsView

#include <string>
#include <vector>

class VrOverlay;

class SensorBridge {
public:
    /**
     * @param sensors upstream sensors (discover() already called)
     * @param vr the overlay for frame timings and controllers, or nullptr when running without OpenVR
     * @param store where values go
     */
    SensorBridge(Sensors& sensors, VrOverlay* vr, fph::MetricStore& store);

    /** Registers one reader per source with the scheduler. */
    void registerSources(fph::Scheduler& scheduler);

    /** @return upstream's sensor sample, rebuilt from the store (N/A → NaN / false / empty) */
    SensorSample sample() const;
    /** @return upstream's frame stats, rebuilt from the store */
    FrameStats frame() const;
    /** @return upstream's controller status, rebuilt from the store */
    ControllerStatus controllers() const;

    /** The widgets the upstream panel shows. Ticket 04 replaces this with the saved layout. */
    static std::vector<fph::WidgetType> upstreamPanelWidgets();

    // ---- T03 ----

    /**
     * Re-queries the scene app and updates the session metrics (1104-1108). Call on (re)connect and on
     * VREvent_SceneApplicationChanged: it runs whatever the demand, since the session identity is
     * bookkeeping for the link and the logs (2 IPCs), not a widget read.
     * @param now monotonic seconds
     */
    void refreshSceneApp(double now);
    /**
     * T11: ends the session once its app has been gone kSessionDetourSec. The scene app is only re-queried on
     * events, so call this every tick.
     * @param now monotonic seconds
     */
    void expireSession(double now);

    /** @return the process lists from the last scan (widgets 7 / 8; BLOBs on the link in ticket 06) */
    const ProcessLists& processLists() const { return processes_.lists(); }
    /** Sets list sizes and "sum same name" (ticket 04 wires the widget settings); used from the next scan. */
    void setProcessListOptions(const ProcessListOptions& options) { processOptions_ = options; }
    /** R2: the rules Hide system uses (not owned; nullptr = nothing hidden) */
    void setHideRules(HideRules* rules) { processes_.setHideRules(rules); }

    /** T04: the process lists as the widgets draw them (exe basename when readable, else comm) */
    fph::ProcessListsView processListsView() const;

    /**
     * T04: scan options for a layout: the largest max_items of the list widgets, and each list's "sum same
     * name" (on if any instance of that list asks for it). R2: "hide system" works the same way.
     */
    static ProcessListOptions processOptionsFor(const fph::Layout& layout);

    /** @return the current headset session */
    const fph::SessionInfo& session() const { return session_.current(); }

    /** @return the process lists and the session, for the debug dump */
    std::string debugText() const;

private:
    Sensors& sensors_;
    VrOverlay* vr_;
    fph::MetricStore& store_;

    void readFrameTiming(double now);
    void readControllers(double now);

    // T03
    WifiLink wifi_;
    ProcessScanner processes_;
    ProcessListOptions processOptions_;
    uint64_t processScans_ = 0;  ///< T04: ProcessListsView::generation
    InterfaceCounters streamDongle_;  ///< own baselines: Network's would be reset by the same tick
    InterfaceCounters streamHome_;
    VrlinkWatch vrlink_;
    VrlinkJournal journal_;
    std::string journalError_;  ///< last journal error logged (logged once per change)
    fph::SessionTracker session_;
    int32_t previousSessionId_ = 0;  ///< T11: the session before the current one (a resume logs "Resumed")
    std::string streamHint_;  ///< vrlink's -A as "steam.app.<id>" while PC-streaming

    void readNetwork(double now, bool resumed);
    void readProcesses(double now, bool resumed);
    void readStreamLink(double now, bool resumed);
    void readStreamEvents(double now);
};
