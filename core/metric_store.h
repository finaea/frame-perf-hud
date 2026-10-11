// Latest value of every registered metric, each with a status (ok, or N/A + reason).
#pragma once

#include "metric_registry.h"
#include "reason.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fph {

/** One metric's latest value. */
struct MetricValue {
    bool ok = false;                       ///< false = N/A, see reason
    Reason reason = Reason::NotSampledYet;  ///< Reason::None when ok
    WireType type = WireType::F32;
    float f32 = 0.0f;
    int32_t i32 = 0;
    uint32_t enumValue = 0;
    std::string text;       ///< ShortString only
    double sampledAt = -1;  ///< monotonic seconds of the last set (ok or N/A); -1 = never
};

/**
 * Holds one MetricValue per registry entry. Setters check the wire type, so a source can't put an i32
 * into an f32 metric by mistake.
 */
class MetricStore {
public:
    /** Builds the store for the compiled-in registry. Every value starts N/A NotSampledYet. */
    MetricStore();

    /** @param defs registry table to use instead (tests) */
    explicit MetricStore(const std::vector<MetricDef>& defs);

    /**
     * Sets an f32 metric. NaN becomes N/A with nanReason, so upstream's NaN convention maps straight in.
     * @return false if the id is unknown or not f32
     */
    bool setF32(uint16_t id, double value, double now, Reason nanReason = Reason::ReadFailed);
    /** @return false if the id is unknown or not i32 */
    bool setI32(uint16_t id, int32_t value, double now);
    /** @return false if the id is unknown or not an enum */
    bool setEnum(uint16_t id, uint32_t value, double now);
    /** Sets a short string, cut to kShortStringMax bytes. @return false if the id is unknown or not a string */
    bool setText(uint16_t id, const std::string& value, double now);
    /** Marks a metric N/A. @return false if the id is unknown */
    bool setNa(uint16_t id, Reason reason, double now);

    /** @return the value, or nullptr if the id is unknown */
    const MetricValue* get(uint16_t id) const;

    /** @return the f32 value, or NaN if N/A or not an f32 metric */
    double f32Or(uint16_t id, double fallback) const;
    /** @return the enum / i32 value, or fallback if N/A */
    int64_t intOr(uint16_t id, int64_t fallback) const;

    /**
     * One line per metric: id, name, value + unit or "N/A (reason)", owner, source, age.
     * @param now monotonic seconds
     */
    std::string dump(double now) const;

    /** @return the registry table this store was built for */
    const std::vector<MetricDef>& defs() const { return defs_; }

private:
    std::vector<MetricDef> defs_;
    std::vector<MetricValue> values_;  ///< same order as defs_

    MetricValue* slot(uint16_t id, WireType expected);
};

}  // namespace fph
