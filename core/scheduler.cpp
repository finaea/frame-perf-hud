#include "scheduler.h"

#include <cstdio>
#include <limits>

namespace fph {

namespace {

/** A source counts as due this much early, so sources that share the 2 Hz tick run together. */
constexpr double kDueSlackSec = 0.05;
/** A delta source's second read after a resume comes this soon, so it has a value without waiting an interval. */
constexpr double kRefillSec = 0.25;

static_assert(kSourceCount <= 32, "pinnedMask_ has a bit per source");

size_t indexOf(SourceId source) {
    return static_cast<size_t>(source);
}

bool validSource(SourceId source) {
    return source != SourceId::None && indexOf(source) < kSourceCount;
}

}  // namespace

Scheduler::Scheduler() : Scheduler(registry()) {}

Scheduler::Scheduler(const std::vector<MetricDef>& defs) {
    for (const MetricDef& d : defs) {
        if (!validSource(d.source) || d.intervalMs == 0) continue;
        SourceState& s = sources_[indexOf(d.source)];
        const double sec = d.intervalMs / 1000.0;
        if (s.interval == 0.0 || sec < s.interval) s.interval = sec;
        if (static_cast<size_t>(d.owner) < kWidgetTypeCount) s.ownerMask |= 1u << static_cast<unsigned>(d.owner);
    }
    // Streaming names its links using the headset adapter IPs from the existing Network source.
    sources_[indexOf(SourceId::Network)].ownerMask |= 1u << static_cast<unsigned>(WidgetType::Streaming);
}

void Scheduler::addSource(SourceId source, ReadFn read, bool quickRefill) {
    if (!validSource(source)) return;
    sources_[indexOf(source)].read = std::move(read);
    sources_[indexOf(source)].quickRefill = quickRefill;
}

void Scheduler::setEnabledWidgets(const std::vector<WidgetType>& widgets) {
    widgets_ = widgets;
    recompute();
}

void Scheduler::setDemand(const DemandInputs& demand) {
    if (demand == demand_) return;
    demand_ = demand;
    recompute();
}

void Scheduler::setPinnedSources(const std::vector<SourceId>& sources) {
    uint32_t mask = 0;
    for (SourceId source : sources) {
        if (validSource(source)) mask |= 1u << indexOf(source);
    }
    if (mask == pinnedMask_) return;
    pinnedMask_ = mask;
    recompute();
}

void Scheduler::recompute() {
    ++recomputes_;
    const bool looking = someoneLooking(demand_);
    for (SourceState& s : sources_) {
        uint32_t refs = 0;
        if (looking && s.interval > 0.0) {
            for (WidgetType w : widgets_) {
                if (static_cast<size_t>(w) < kWidgetTypeCount && (s.ownerMask & (1u << static_cast<unsigned>(w)))) {
                    ++refs;
                }
            }
        }
        // T11: a pinned source counts one reference of its own
        const size_t index = static_cast<size_t>(&s - sources_.data());
        if (s.interval > 0.0 && (pinnedMask_ & (1u << index))) ++refs;
        if (s.refs == 0 && refs > 0) {
            // Just became active: read on the next runDue, and tell the reader its baseline is stale
            s.nextDue = now_;
            s.pendingResume = true;
        }
        s.refs = refs;
    }
}

int Scheduler::runDue(double now) {
    now_ = now;
    int ran = 0;
    for (SourceState& s : sources_) {
        if (s.refs == 0 || !s.read) continue;
        if (now < s.nextDue - kDueSlackSec) continue;
        const bool resumed = s.pendingResume;
        s.pendingResume = false;
        s.read(now, resumed);
        ++s.reads;
        s.lastRead = now;
        if (resumed && s.quickRefill && kRefillSec < s.interval) {
            // A resumed delta source only took its baseline: one extra read kRefillSec later gives a value, then
            // it goes back to its own deadline, so it stays in step with the other sources
            s.nextDue = now + kRefillSec;
            s.afterRefill = now + s.interval;
        } else {
            s.nextDue = s.afterRefill > now ? s.afterRefill : now + s.interval;
            s.afterRefill = -1.0;
        }
        ++ran;
    }
    return ran;
}

double Scheduler::nextDue(double now) const {
    double best = std::numeric_limits<double>::infinity();
    for (const SourceState& s : sources_) {
        if (s.refs == 0 || !s.read) continue;
        const double due = s.nextDue < now ? now : s.nextDue;
        if (due < best) best = due;
    }
    return best;
}

void Scheduler::requestRead(SourceId source) {
    if (!validSource(source)) return;
    SourceState& s = sources_[indexOf(source)];
    if (s.refs > 0 && s.read) s.nextDue = std::min(s.nextDue, now_);
}

uint32_t Scheduler::refCount(SourceId source) const {
    return validSource(source) ? sources_[indexOf(source)].refs : 0;
}

uint64_t Scheduler::readCount(SourceId source) const {
    return validSource(source) ? sources_[indexOf(source)].reads : 0;
}

double Scheduler::interval(SourceId source) const {
    return validSource(source) ? sources_[indexOf(source)].interval : 0.0;
}

std::string Scheduler::dump(double now) const {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "demand: hud_shown=%d hmd_in_use=%d editor_open=%d pc_lease=%d record=%d -> looking=%d "
                  "(recomputed %llu times)\n",
                  demand_.hudShown, demand_.hmdInUse, demand_.editorOpen, demand_.pcLease, demand_.record,
                  someoneLooking(demand_), static_cast<unsigned long long>(recomputes_));
    out += line;
    out += "enabled widgets:";
    for (WidgetType w : widgets_) {
        out += " ";
        out += widgetName(w);
        out += ",";
    }
    if (!widgets_.empty()) out.pop_back();
    out += "\n";
    std::snprintf(line, sizeof(line), "%-14s %9s %5s %8s %10s\n", "source", "interval", "refs", "reads",
                  "last read");
    out += line;
    for (size_t i = 1; i < kSourceCount; ++i) {
        const SourceState& s = sources_[i];
        if (s.interval == 0.0) continue;
        char last[32];
        if (s.lastRead < 0) {
            std::snprintf(last, sizeof(last), "never");
        } else {
            std::snprintf(last, sizeof(last), "%.1fs ago", now - s.lastRead);
        }
        std::snprintf(line, sizeof(line), "%-14s %8.1fs %5u %8llu %10s%s\n", sourceName(static_cast<SourceId>(i)),
                      s.interval, s.refs, static_cast<unsigned long long>(s.reads), last, s.read ? "" : "  (no reader)");
        out += line;
    }
    return out;
}

}  // namespace fph
