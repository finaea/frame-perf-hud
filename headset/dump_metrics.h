// --dump-metrics: run the registry + scheduler outside the overlay and print every metric, the per-source
// read counters and the process lists. For checking readings on the Frame next to the installed HUD:
// it creates no overlay, and with --vr connects to SteamVR only as a Background app.
#pragma once

#include "metric_registry.h"

#include <csignal>
#include <string>
#include <vector>

struct DumpMetricsOptions {
    int count = 5;                         ///< prints
    double intervalSec = 1.0;              ///< between prints
    std::vector<fph::WidgetType> widgets;  ///< enabled widgets; empty = 1-11
    bool vr = false;                       ///< connect to SteamVR (Background) for frame timing, controllers, scene app
    bool sumSameName = false;              ///< process lists: one row per name
};

/**
 * Parses "1,7,9" (widget numbers from the brief's table).
 * @return false on anything that isn't a known widget number
 */
bool parseWidgetList(const std::string& text, std::vector<fph::WidgetType>& out);

/**
 * Runs the dump.
 * @param stop set by SIGINT / SIGTERM
 * @return exit code
 */
int runDumpMetrics(const DumpMetricsOptions& options, const volatile std::sig_atomic_t& stop);
