// Demand-driven reading. A source is read only when it's needed (an enabled widget owns a metric from it)
// AND someone is looking. Nothing else ever starts a read.
#pragma once

#include "metric_registry.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fph {

/** The signals that decide whether anyone is looking. */
struct DemandInputs {
    bool hudShown = false;    ///< the HUD overlay is switched on
    bool hmdInUse = false;    ///< HMD activity level isn't standby (off-head reads standby)
    bool editorOpen = false;  ///< the dashboard tab (editor) is visible
    bool pcLease = false;     ///< a PC holds a valid lease (stub until ticket 06)
    bool record = false;      ///< T11: Record is on

    bool operator==(const DemandInputs& o) const {
        return hudShown == o.hudShown && hmdInUse == o.hmdInUse && editorOpen == o.editorOpen &&
               pcLease == o.pcLease && record == o.record;
    }
    bool operator!=(const DemandInputs& o) const { return !(*this == o); }
};

/**
 * "Someone looking" = HUD shown and the headset in use, or the editor open, or a PC lease, or Record.
 * @param d the signals
 * @return true if sources may be read
 */
inline bool someoneLooking(const DemandInputs& d) {
    return (d.hudShown && d.hmdInUse) || d.editorOpen || d.pcLease || d.record;
}

/**
 * Keeps a reference count per source and runs the sources that are due.
 * Single-threaded: call everything from the main loop.
 */
class Scheduler {
public:
    /**
     * Reads one source and stores its metrics.
     * @param now monotonic seconds
     * @param resumed true on the first read after the source was inactive (or ever), so delta-based sources
     *                can rebuild their baseline instead of reporting an average over the pause
     */
    using ReadFn = std::function<void(double now, bool resumed)>;

    /** Uses the compiled-in registry. */
    Scheduler();
    /** @param defs registry table to use instead (tests) */
    explicit Scheduler(const std::vector<MetricDef>& defs);

    /**
     * Registers the reader for a source. A source with metrics but no reader is counted, never read.
     * @param source the source
     * @param read the reader
     * @param quickRefill a delta source (its resumed read only takes a baseline): the read after a resumed one
     *                    comes 0.25 s later instead of a full interval, then the interval again
     */
    void addSource(SourceId source, ReadFn read, bool quickRefill = false);

    /**
     * Sets the enabled widget instances (a type may appear more than once, e.g. two Network widgets)
     * and recomputes the reference counts.
     */
    void setEnabledWidgets(const std::vector<WidgetType>& widgets);

    /** Sets the "someone looking" signals; recomputes the reference counts if anything changed. */
    void setDemand(const DemandInputs& demand);

    /**
     * T11: sources read whatever the demand and the layout (the crash recorder's kit), at their own interval. Doesn't
     * make anyone "looking": the other sources stay paused.
     * @param sources the pinned sources (empty = none)
     */
    void setPinnedSources(const std::vector<SourceId>& sources);

    /**
     * Runs every active source that is due.
     * @param now monotonic seconds
     * @return number of sources read
     */
    int runDue(double now);

    /**
     * @param now monotonic seconds
     * @return when the next active source is due (monotonic seconds); +infinity if none is active
     */
    double nextDue(double now) const;

    /** @return the source's reference count (0 when nobody is looking) */
    uint32_t refCount(SourceId source) const;
    /** @return how many times the source has been read since start */
    uint64_t readCount(SourceId source) const;
    /** @return the source's read interval in seconds (shortest interval among its metrics); 0 if it has none */
    double interval(SourceId source) const;
    /** @return how many times the reference counts were recomputed */
    uint64_t recomputeCount() const { return recomputes_; }
    /** @return the current signals */
    const DemandInputs& demand() const { return demand_; }

    /**
     * Human-readable state: signals, enabled widgets, and per source: interval, refs, reads, last read.
     * @param now monotonic seconds
     */
    std::string dump(double now) const;

private:
    struct SourceState {
        double interval = 0.0;  ///< seconds; 0 = no metric uses this source
        uint32_t refs = 0;
        uint64_t reads = 0;
        double lastRead = -1.0;
        double nextDue = 0.0;
        bool pendingResume = true;  ///< next read is the first since (re)activation
        bool quickRefill = false;   ///< addSource: a delta source, read again kRefillSec after a resumed read
        double afterRefill = -1.0;  ///< the resumed read's own next deadline, used after the refill read (-1 = none)
        ReadFn read;
        /** Widgets owning at least one metric from this source (bit per WidgetType). */
        uint32_t ownerMask = 0;
    };

    std::array<SourceState, kSourceCount> sources_ {};
    std::vector<WidgetType> widgets_;
    DemandInputs demand_;
    uint32_t pinnedMask_ = 0;  ///< T11: bit per SourceId
    uint64_t recomputes_ = 0;
    double now_ = 0.0;  ///< last time passed to runDue (activation schedules the read for then)

    void recompute();
};

}  // namespace fph
