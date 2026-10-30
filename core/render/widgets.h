// The widgets (brief's table, 1-13) drawn in frame-perf-overlay's look. Each widget reads only the metrics it
// owns (core/metric_registry.h) from the MetricStore, so the headset and the PC app draw the same thing.
// The camera widgets (12, 13, 14) draw their cards, labels and state text; the pictures go on top (T10).
#pragma once

#include "draw.h"
#include "layout.h"
#include "metric_store.h"
#include "series.h"
#include "thresholds.h"

#include <string>
#include <vector>

namespace fph {

/** Width of one grid column (px): upstream's panel width. */
constexpr int kColumnWidth = 512;

/** One process-list row (a process, or every process of one name). */
struct ProcessItem {
    std::string name;  ///< what to show: the exe basename when known, else comm
    int pid = 0;       ///< 0 for a summed row
    int count = 1;     ///< processes in this row
    double cpuCorePct = 0.0;  ///< % of one core
    double rssMiB = 0.0;
};

/** Both process lists, as of the last scan (rows aren't metrics: they go over the link as BLOBs, ticket 06). */
struct ProcessListsView {
    std::vector<ProcessItem> byCpu;
    std::vector<ProcessItem> byRam;
    bool cpuValid = false;  ///< byCpu has a real interval behind it
    uint64_t generation = 0;  ///< bumped on every scan (the list widgets redraw only when it moves)
    bool hideRulesBroken = false;  ///< hide_rules.json doesn't parse: a Hide system card says so (headset only)
};

/** What every widget gets to draw with. */
struct WidgetContext {
    const MetricStore* store = nullptr;
    const ProcessListsView* processes = nullptr;  ///< nullptr = no lists (drawn as N/A)
    Thresholds thresholds;
    double now = 0.0;        ///< monotonic seconds (graph right edge)
    int graphSeconds = 30;   ///< graph window
    int clockFormat = 24;    ///< 12 or 24; 0 = the dashboard's clock "Off": General hides its Time row
    /** T10: per camera id (0-6), a cam::CameraState. A Live tile is left dark (the picture goes on top of it:
     *  an overlay in the headset, CameraFeed::paint on the PC); any other state is written in the tile. */
    uint8_t cameraStates[7] = {1, 1, 1, 1, 1, 1, 1};
};

/** Graph history one widget instance keeps between redraws. */
struct WidgetHistory {
    History a;  ///< main series (fps, vph W, CPU °C, GPU load %)
    History b;  ///< second series (GPU °C)
    History c;  ///< R2 Frame: worst CPU frame time per read (ms)
    History d;  ///< R2 Frame: worst GPU frame time per read (ms)
};

/** A throughput as text, value and unit apart so they can be styled differently. */
struct RateText {
    std::string value;  ///< "24", "4.6", "123", "--"
    std::string unit;   ///< " kbps" / " Mbps" (leading space)
};

/**
 * Network ↓/↑ text: below 1 Mbps whole kbps ("24 kbps", "0 kbps"), else Mbps with one decimal ("1.0", "4.6"),
 * whole from 100 Mbps. NaN (N/A) gives "--". Anything that rounds to 1000 kbps already reads "1.0 Mbps".
 * @param mbps megabits per second
 */
RateText formatRate(double mbps);

/**
 * Natural height of a widget's card (px). Battery follows its visible rows when a context is supplied. A Compact
 * instance (R2) is shorter.
 * @param w the instance
 * @return height
 */
int widgetHeight(const WidgetInstance& w, const WidgetContext* ctx = nullptr);
int batteryRowCount(const WidgetContext& ctx);

/**
 * T05: the card's title (what the editor labels it with).
 * @param w the instance (a Network's title names its adapter)
 * @return the title
 */
const char* widgetTitle(const WidgetInstance& w);

/**
 * Draws the parts that only change with the layout or the theme: the card, the graph beds, the title.
 * @param pen drawing tools
 * @param x card left
 * @param y card top
 * @param w the instance
 */
void drawWidgetStatic(const Pen& pen, double x, double y, const WidgetInstance& w, int heightOverride = 0);

/**
 * What a widget instance's picture depends on: when each metric it owns was last set, the process scan, and
 * the settings it draws with. Equal signatures = identical pixels, so the grid skips that widget.
 * @param w the instance
 * @param ctx values
 * @return a hash
 */
uint64_t widgetSignature(const WidgetInstance& w, const WidgetContext& ctx);

/**
 * Appends this redraw's samples to the instance's graphs. Call once per redraw, before drawWidgetDynamic.
 * @param w the instance
 * @param ctx values
 * @param history its graphs
 */
void pushWidgetHistory(const WidgetInstance& w, const WidgetContext& ctx, WidgetHistory& history);

/**
 * Draws the values on top of the static layer.
 * @param pen drawing tools
 * @param x card left
 * @param y card top
 * @param w the instance
 * @param ctx values
 * @param history its graphs
 */
void drawWidgetDynamic(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
                       const WidgetHistory& history);

}  // namespace fph
