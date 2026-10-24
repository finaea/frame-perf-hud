#include "metric_store.h"

#include <cmath>
#include <cstdio>

namespace frame_perf_overlay {

MetricStore::MetricStore() : MetricStore(registry()) {}

MetricStore::MetricStore(const std::vector<MetricDef>& defs) : defs_(defs), values_(defs.size()) {
    for (size_t i = 0; i < defs_.size(); ++i) values_[i].type = defs_[i].type;
}

MetricValue* MetricStore::slot(uint16_t id, WireType expected) {
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id == id) return defs_[i].type == expected ? &values_[i] : nullptr;
    }
    return nullptr;
}

const MetricValue* MetricStore::get(uint16_t id) const {
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id == id) return &values_[i];
    }
    return nullptr;
}

bool MetricStore::setF32(uint16_t id, double value, double now, Reason nanReason) {
    MetricValue* v = slot(id, WireType::F32);
    if (v == nullptr) return false;
    v->sampledAt = now;
    if (std::isnan(value)) {
        v->ok = false;
        v->reason = nanReason;
        return true;
    }
    v->ok = true;
    v->reason = Reason::None;
    v->f32 = static_cast<float>(value);
    return true;
}

bool MetricStore::setI32(uint16_t id, int32_t value, double now) {
    MetricValue* v = slot(id, WireType::I32);
    if (v == nullptr) return false;
    v->ok = true;
    v->reason = Reason::None;
    v->i32 = value;
    v->sampledAt = now;
    return true;
}

bool MetricStore::setEnum(uint16_t id, uint32_t value, double now) {
    MetricValue* v = slot(id, WireType::Enum);
    if (v == nullptr) return false;
    v->ok = true;
    v->reason = Reason::None;
    v->enumValue = value;
    v->sampledAt = now;
    return true;
}

bool MetricStore::setText(uint16_t id, const std::string& value, double now) {
    MetricValue* v = slot(id, WireType::ShortString);
    if (v == nullptr) return false;
    v->ok = true;
    v->reason = Reason::None;
    v->text = value.substr(0, kShortStringMax);
    v->sampledAt = now;
    return true;
}

bool MetricStore::setNa(uint16_t id, Reason reason, double now) {
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id != id) continue;
        values_[i].ok = false;
        values_[i].reason = reason == Reason::None ? Reason::ReadFailed : reason;
        values_[i].sampledAt = now;
        return true;
    }
    return false;
}

double MetricStore::f32Or(uint16_t id, double fallback) const {
    const MetricValue* v = get(id);
    if (v == nullptr || !v->ok || v->type != WireType::F32) return fallback;
    return v->f32;
}

int64_t MetricStore::intOr(uint16_t id, int64_t fallback) const {
    const MetricValue* v = get(id);
    if (v == nullptr || !v->ok) return fallback;
    if (v->type == WireType::I32) return v->i32;
    if (v->type == WireType::Enum) return v->enumValue;
    return fallback;
}

std::string MetricStore::dump(double now) const {
    std::string out;
    char line[256];
    for (size_t i = 0; i < defs_.size(); ++i) {
        const MetricDef& d = defs_[i];
        const MetricValue& v = values_[i];
        std::string value;
        if (!v.ok) {
            value = std::string("N/A (") + reasonText(v.reason) + ")";
        } else {
            char text[64];
            switch (v.type) {
                case WireType::F32: std::snprintf(text, sizeof(text), "%.3f %s", v.f32, d.unit); break;
                case WireType::I32: std::snprintf(text, sizeof(text), "%d %s", v.i32, d.unit); break;
                case WireType::Enum: std::snprintf(text, sizeof(text), "enum %u", v.enumValue); break;
                case WireType::ShortString: std::snprintf(text, sizeof(text), "\"%s\"", v.text.c_str()); break;
            }
            value = text;
        }
        char age[32];
        if (v.sampledAt < 0) {
            std::snprintf(age, sizeof(age), "never");
        } else {
            std::snprintf(age, sizeof(age), "%.1fs ago", now - v.sampledAt);
        }
        std::snprintf(line, sizeof(line), "%5u %-34s %-28s %-10s %-12s %s\n", d.id, d.name, value.c_str(),
                      widgetName(d.owner), sourceName(d.source), age);
        out += line;
    }
    return out;
}

}  // namespace frame_perf_overlay
