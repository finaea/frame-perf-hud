#include "stream_metrics.h"
#include <cmath>
#include <set>
namespace fph {
bool isPcStreamMetric(uint16_t id) {
    switch (id) {
#define FPH_STREAM_CASE(n, id, name, unit, type)                                                                       \
    case id:                                                                                                           \
        return true;
        FPH_PC_STREAM_METRICS(FPH_STREAM_CASE)
#undef FPH_STREAM_CASE
    default:
        return false;
    }
}
std::vector<link::WireValue> pcStreamValues(const MetricStore& store) {
    std::vector<link::WireValue> out;
    for (const auto& d : registry())
        if (isPcStreamMetric(d.id)) {
            if (const auto* v = store.get(d.id))
                out.push_back(link::toWire(d.id, *v));
        }
    return out;
}
void setPcStreamNa(MetricStore& store, Reason reason, double now) {
    for (const auto& d : registry())
        if (isPcStreamMetric(d.id))
            store.setNa(d.id, reason, now);
}
std::string streamLinkName(const MetricStore& store, unsigned index) {
    const std::string fallback = "Link " + std::to_string(index + 1);
    if (index >= 4)
        return fallback;
    const auto* endpoint = store.get(static_cast<uint16_t>(metric::kPcStreamLink0Address + index));
    uint32_t ip = 0;
    if (!endpoint || !endpoint->ok || !link::ipv4FromText(endpoint->text, ip) || ip == 0 || (ip >> 24) == 127 ||
        (ip >> 24) >= 224)
        return fallback;
    const auto matches = [&](uint16_t id) {
        const auto* value = store.get(id);
        uint32_t adapter = 0;
        return value && value->ok && link::ipv4FromText(value->text, adapter) && adapter == ip;
    };
    const bool dongle = matches(metric::kNetDongleBase + metric::kNetAdapterIp);
    const bool home = matches(metric::kNetHomeBase + metric::kNetAdapterIp);
    if (dongle == home)
        return fallback; // no match, or an ambiguous duplicate address
    return dongle ? "Dongle" : "Home Wi-Fi";
}
std::vector<uint8_t> encodePcMetrics(const PcMetrics& m) {
    link::Writer w;
    w.id(m.pcId);
    w.u32(m.snapshotId);
    w.u16(static_cast<uint16_t>(m.values.size()));
    std::set<uint16_t> ids;
    for (const auto& v : m.values) {
        const auto* d = findMetric(v.id);
        if (!isPcStreamMetric(v.id) || !d || d->type != v.type || !ids.insert(v.id).second)
            return {};
        w.u16(v.id);
        w.u8(static_cast<uint8_t>(v.type));
        w.u8(v.ok ? 0 : 1);
        if (!v.ok) {
            w.u16(static_cast<uint16_t>(v.reason));
            continue;
        }
        switch (v.type) {
        case WireType::F32:
            if (!std::isfinite(v.f32))
                return {};
            w.f32(v.f32);
            break;
        case WireType::I32:
            w.i32(v.i32);
            break;
        case WireType::Enum:
            w.u8(v.enumValue);
            break;
        case WireType::ShortString:
            w.str8(v.text, kShortStringMax);
            break;
        }
    }
    return w.size() <= link::kMaxPayload ? w.bytes() : std::vector<uint8_t>{};
}
bool decodePcMetrics(const uint8_t* data, size_t size, PcMetrics& out) {
    if (size > link::kMaxPayload)
        return false;
    link::Reader r(data, size);
    PcMetrics m;
    m.pcId = r.id();
    m.snapshotId = r.u32();
    const auto count = r.u16();
    if (r.failed() || count > link::kMaxPayload / 5)
        return false;
    std::set<uint16_t> ids;
    for (unsigned j = 0; j < count; ++j) {
        link::WireValue v;
        if (!link::readWireValue(r, v) || !ids.insert(v.id).second ||
            (v.ok && v.type == WireType::F32 && !std::isfinite(v.f32)))
            return false;
        const auto* d = findMetric(v.id);
        if (!d) {
            ++m.unknownValues;
            continue;
        }
        if (!isPcStreamMetric(v.id) || v.type != d->type)
            return false;
        m.values.push_back(std::move(v));
    }
    if (!r.done())
        return false;
    out = std::move(m);
    return true;
}
void PcStreamReceiver::apply(const PcMetrics& m, MetricStore& store, double now) {
    setPcStreamNa(store, Reason::NotSampledYet, now);
    for (const auto& v : m.values)
        if (isPcStreamMetric(v.id))
            link::storeWire(store, v, now);
    heard_ = now;
    active_ = true;
}
void PcStreamReceiver::expire(MetricStore& store, double now) {
    if (!active_ || now - heard_ > 3.0)
        reset(store, now);
}
void PcStreamReceiver::reset(MetricStore& store, double now) {
    if (active_ ||
        (store.get(metric::kPcStreamPing) && store.get(metric::kPcStreamPing)->reason != Reason::PcAppNotRunning))
        setPcStreamNa(store, Reason::PcAppNotRunning, now);
    active_ = false;
}
} // namespace fph
