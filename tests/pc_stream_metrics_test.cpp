// core/stream_metrics: the PC_METRICS codec (encodePcMetrics / decodePcMetrics), the id allow-list
// (isPcStreamMetric, pcStreamValues, setPcStreamNa) and PcStreamReceiver (the headset side: apply / expire /
// reset). Uses the compiled registry so sender and receiver type/ID agreement is exercised as shipped.
#include "link_wire.h"
#include "metric_registry.h"
#include "metric_store.h"
#include "reason.h"
#include "stream_metrics.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

using namespace fph;
using namespace fph::link;
using Bytes = std::vector<uint8_t>;

Id128 idOf(uint8_t seed) {
    Id128 id{};
    for (size_t i = 0; i < id.size(); ++i)
        id[i] = static_cast<uint8_t>(seed + i * 7);
    return id;
}

/** Every proper prefix of a valid payload must fail to decode (and never crash). */
bool allPrefixesFail(const Bytes& payload) {
    for (size_t n = 0; n < payload.size(); ++n) {
        PcMetrics out;
        if (decodePcMetrics(payload.data(), n, out))
            return false;
    }
    return true;
}

bool trailingByteFails(const Bytes& payload) {
    Bytes longer = payload;
    longer.push_back(0);
    PcMetrics out;
    return !decodePcMetrics(longer.data(), longer.size(), out);
}

// ---- isPcStreamMetric: the 1020+ block, and nothing outside it ----

void testIdAllowList() {
    CHECK(isPcStreamMetric(metric::kPcStreamPing));
    CHECK(isPcStreamMetric(metric::kPcStreamDisplayHz));
    CHECK(!isPcStreamMetric(metric::kStreamLinkMode)); // headset-side streaming (1000-1019), not PC
    CHECK(!isPcStreamMetric(metric::kFrameAppFps));    // widget 1
    CHECK(!isPcStreamMetric(0));
    CHECK(!isPcStreamMetric(65535));
}

// ---- encode / decode round trip ----

WireValue f32(uint16_t id, float v) {
    WireValue w;
    w.id = id;
    w.type = WireType::F32;
    w.ok = true;
    w.f32 = v;
    return w;
}
WireValue i32(uint16_t id, int32_t v) {
    WireValue w;
    w.id = id;
    w.type = WireType::I32;
    w.ok = true;
    w.i32 = v;
    return w;
}
WireValue str(uint16_t id, const std::string& v) {
    WireValue w;
    w.id = id;
    w.type = WireType::ShortString;
    w.ok = true;
    w.text = v;
    return w;
}
WireValue na(uint16_t id, WireType t, Reason r) {
    WireValue w;
    w.id = id;
    w.type = t;
    w.ok = false;
    w.reason = r;
    return w;
}

void testRoundTrip() {
    PcMetrics m;
    m.pcId = idOf(9);
    m.snapshotId = 4242;
    m.values = {
        f32(metric::kPcStreamPing, 2.47f),
        f32(metric::kPcStreamEstimatedBandwidth, 232.0f),
        f32(metric::kPcStreamEncoderResets, 4.0f),
        i32(metric::kPcStreamStreamResets, 0),
        str(metric::kPcStreamBuild, "1791581198"),
        na(metric::kPcStreamNamedPing, WireType::F32, Reason::SteamVrChanged),
        na(metric::kPcStreamLink2Ping, WireType::F32, Reason::NotSampledYet),
    };
    const Bytes b = encodePcMetrics(m);
    CHECK(!b.empty());
    CHECK(b.size() <= kMaxPayload);
    PcMetrics o;
    CHECK(decodePcMetrics(b.data(), b.size(), o));
    CHECK(o.pcId == m.pcId && o.snapshotId == 4242);
    CHECK(o.values.size() == m.values.size());
    for (size_t i = 0; i < o.values.size() && i < m.values.size(); ++i)
        CHECK(o.values[i] == m.values[i]);
    CHECK(allPrefixesFail(b));
    CHECK(trailingByteFails(b));

    // Empty snapshot still encodes (and fits inside the packet framing)
    PcMetrics empty;
    empty.pcId = idOf(1);
    const Bytes eb = encodePcMetrics(empty);
    CHECK(!eb.empty());
    CHECK(!encodePacket(PacketType::PcMetrics, 1, eb).empty());
    PcMetrics eo;
    CHECK(decodePcMetrics(eb.data(), eb.size(), eo) && eo.values.empty());
}

void testRejectsNonStreamIds() {
    // a value whose id isn't in the PC-stream block is refused at encode time (whole packet, not silently dropped)
    PcMetrics m;
    m.pcId = idOf(1);
    m.values = {f32(metric::kFrameAppFps, 72.0f)};
    CHECK(encodePcMetrics(m).empty());

    m.values = {f32(metric::kPcStreamPing, 1.0f), f32(metric::kFrameAppFps, 72.0f)};
    CHECK(encodePcMetrics(m).empty());
}

void testRejectsDuplicateIds() {
    PcMetrics m;
    m.pcId = idOf(1);
    m.values = {f32(metric::kPcStreamPing, 1.0f), f32(metric::kPcStreamPing, 2.0f)};
    CHECK(encodePcMetrics(m).empty());
}

void testRejectsWrongType() {
    // the id is a real PC-stream metric, but tagged with the wrong wire type for it (registry says I32)
    WireValue bad = i32(metric::kPcStreamStreamResets, 1);
    bad.type = WireType::F32;
    bad.f32 = 1.0f;
    PcMetrics m;
    m.pcId = idOf(1);
    m.values = {bad};
    CHECK(encodePcMetrics(m).empty());
}

void testRejectsNonFiniteFloat() {
    PcMetrics m;
    m.pcId = idOf(1);
    m.values = {f32(metric::kPcStreamPing, std::numeric_limits<float>::quiet_NaN())};
    CHECK(encodePcMetrics(m).empty());
    m.values = {f32(metric::kPcStreamPing, std::numeric_limits<float>::infinity())};
    CHECK(encodePcMetrics(m).empty());
}

void testDecodeRejectsTampering() {
    // Layout: id(16) + snapshotId u32(4) + count u16(2) == 22-byte header, then per value:
    // id u16(2) + type u8(1) + status u8(1) + payload.
    constexpr size_t kCountAt = 20;
    constexpr size_t kFirstValueIdAt = 22;
    constexpr size_t kFirstValueStatusAt = 22 + 2 + 1;

    PcMetrics m;
    m.pcId = idOf(2);
    m.values = {f32(metric::kPcStreamPing, 1.5f)};
    const Bytes b = encodePcMetrics(m);
    PcMetrics o;
    CHECK(decodePcMetrics(b.data(), b.size(), o));

    // the count field claims more values than are actually present / than the registry allows (> 80)
    {
        Bytes tampered = b;
        tampered[kCountAt] = 0xFF;
        tampered[kCountAt + 1] = 0xFF;
        CHECK(!decodePcMetrics(tampered.data(), tampered.size(), o));
    }
    // unknown id: accepted and skipped, not rejected (mixed-version tolerance) -- counted via unknownValues
    {
        Bytes tampered = b;
        tampered[kFirstValueIdAt] = 0xFF;
        tampered[kFirstValueIdAt + 1] = 0xFF;
        PcMetrics skipped;
        CHECK(decodePcMetrics(tampered.data(), tampered.size(), skipped));
        CHECK(skipped.values.empty());
        CHECK(skipped.unknownValues == 1);
    }
    // status byte out of range (only 0 ok / 1 N/A allowed)
    {
        Bytes tampered = b;
        tampered[kFirstValueStatusAt] = 2;
        CHECK(!decodePcMetrics(tampered.data(), tampered.size(), o));
    }
    // oversized payload refused outright
    {
        Bytes huge(kMaxPayload + 1, 0);
        CHECK(!decodePcMetrics(huge.data(), huge.size(), o));
    }
}

// ---- Mixed-version tolerance: unknown ids (a newer peer's fields this build's registry doesn't have) are
// skipped and counted, never rejected outright. encodePcMetrics can't produce these itself (it validates
// every id against isPcStreamMetric), so these build the wire bytes directly -- standing in for a real future
// id exactly the same way a truly unknown one would decode. ----

/** Builds a PC_METRICS payload without encodePcMetrics's registry validation, so it can include ids the
 *  current build doesn't know about (standing in for a mixed-version peer). */
Bytes rawPcMetrics(const Id128& pcId, uint32_t snapshotId, const std::vector<WireValue>& values) {
    Writer w;
    w.id(pcId);
    w.u32(snapshotId);
    w.u16(static_cast<uint16_t>(values.size()));
    for (const WireValue& v : values) {
        w.u16(v.id);
        w.u8(static_cast<uint8_t>(v.type));
        w.u8(v.ok ? 0 : 1);
        if (!v.ok) {
            w.u16(static_cast<uint16_t>(v.reason));
            continue;
        }
        switch (v.type) {
        case WireType::F32:
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
    return w.bytes();
}

void testUnknownIdsSkippedBetweenKnownEntriesAllTypes() {
    WireValue unknownF32;
    unknownF32.id = 65000;
    unknownF32.type = WireType::F32;
    unknownF32.ok = true;
    unknownF32.f32 = 1.0f;
    WireValue unknownI32;
    unknownI32.id = 65001;
    unknownI32.type = WireType::I32;
    unknownI32.ok = true;
    unknownI32.i32 = 7;
    WireValue unknownEnum;
    unknownEnum.id = 65002;
    unknownEnum.type = WireType::Enum;
    unknownEnum.ok = true;
    unknownEnum.enumValue = 3;
    WireValue unknownStr;
    unknownStr.id = 65003;
    unknownStr.type = WireType::ShortString;
    unknownStr.ok = true;
    unknownStr.text = "x";
    WireValue unknownNa;
    unknownNa.id = 65004;
    unknownNa.type = WireType::F32;
    unknownNa.ok = false;
    unknownNa.reason = Reason::NotConnected;

    const std::vector<WireValue> values = {
        f32(metric::kPcStreamPing, 2.4f),          unknownF32, unknownI32, unknownEnum, unknownStr, unknownNa,
        str(metric::kPcStreamBuild, "1791581198"),
    };
    const Bytes b = rawPcMetrics(idOf(7), 1, values);
    PcMetrics o;
    CHECK(decodePcMetrics(b.data(), b.size(), o));
    CHECK(o.unknownValues == 5);
    CHECK(o.values.size() == 2);
    CHECK(o.values[0].id == metric::kPcStreamPing && o.values[0].ok && o.values[0].f32 == 2.4f);
    CHECK(o.values[1].id == metric::kPcStreamBuild && o.values[1].ok && o.values[1].text == "1791581198");
}

void testUnknownIdBadTypeOrShortBufferStillRejected() {
    // an unknown id doesn't get a pass on a nonsense type byte: the value itself must still parse
    {
        Writer w;
        w.id(idOf(8));
        w.u32(1);
        w.u16(1);
        w.u16(65000); // unknown id
        w.u8(99);     // not a valid WireType
        w.u8(0);
        PcMetrics o;
        CHECK(!decodePcMetrics(w.bytes().data(), w.bytes().size(), o));
    }
    // an unknown id's declared payload truncated mid-value: still a hard failure, not a skip
    {
        Writer w;
        w.id(idOf(8));
        w.u32(1);
        w.u16(1);
        w.u16(65000);
        w.u8(static_cast<uint8_t>(WireType::F32));
        w.u8(0);
        // the 4-byte F32 payload is omitted entirely
        PcMetrics o;
        CHECK(!decodePcMetrics(w.bytes().data(), w.bytes().size(), o));
    }
}

// ---- pcStreamValues / setPcStreamNa over a real MetricStore ----

void testStoreRoundTrip() {
    MetricStore store;
    const double t0 = 10.0;
    CHECK(store.setF32(metric::kPcStreamPing, 2.5, t0));
    CHECK(store.setF32(metric::kPcStreamEncoderResets, 7.0, t0));
    CHECK(store.setNa(metric::kPcStreamNamedPing, Reason::SteamVrChanged, t0));

    const std::vector<WireValue> values = pcStreamValues(store);
    CHECK(!values.empty());
    for (const WireValue& v : values)
        CHECK(isPcStreamMetric(v.id));

    bool sawPing = false, sawResets = false, sawNaNamedPing = false;
    for (const WireValue& v : values) {
        if (v.id == metric::kPcStreamPing) {
            sawPing = true;
            CHECK(v.ok && v.f32 == 2.5f);
        }
        if (v.id == metric::kPcStreamEncoderResets) {
            sawResets = true;
            CHECK(v.ok && v.f32 == 7.0f);
        }
        if (v.id == metric::kPcStreamNamedPing) {
            sawNaNamedPing = true;
            CHECK(!v.ok && v.reason == Reason::SteamVrChanged);
        }
    }
    CHECK(sawPing && sawResets && sawNaNamedPing);

    setPcStreamNa(store, Reason::PcAppNotRunning, t0 + 1.0);
    for (const WireValue& v : pcStreamValues(store))
        CHECK(!v.ok && v.reason == Reason::PcAppNotRunning);

    // metrics outside the PC-stream block are untouched by setPcStreamNa
    CHECK(store.setF32(metric::kFrameAppFps, 72.0, t0));
    setPcStreamNa(store, Reason::PcAppNotRunning, t0 + 2.0);
    CHECK(store.f32Or(metric::kFrameAppFps, -1.0) == 72.0);
}

// ---- Link0..3Address (1090-1093): round trip through the same codec as any other PC-stream field ----

void testEndpointAddressRoundTrip() {
    PcMetrics m;
    m.pcId = idOf(4);
    m.values = {
        str(metric::kPcStreamLink0Address, "10.35.78.1"),
        str(metric::kPcStreamLink1Address, "192.168.1.47"),
        na(metric::kPcStreamLink2Address, WireType::ShortString, Reason::NotConnected),
    };
    const Bytes b = encodePcMetrics(m);
    CHECK(!b.empty());
    PcMetrics o;
    CHECK(decodePcMetrics(b.data(), b.size(), o));
    CHECK(o.values.size() == 3);
    for (size_t i = 0; i < o.values.size(); ++i)
        CHECK(o.values[i] == m.values[i]);
}

// ---- streamLinkName: naming a link from its PC-reported endpoint + the headset's own adapter IPs ----

void testStreamLinkName() {
    MetricStore store;
    const double t0 = 1.0;
    store.setText(metric::kNetDongleBase + metric::kNetAdapterIp, "10.35.78.1", t0);
    store.setText(metric::kNetHomeBase + metric::kNetAdapterIp, "192.168.1.47", t0);

    // an exact match to the dongle's own address
    store.setText(metric::kPcStreamLink0Address, "10.35.78.1", t0);
    CHECK(streamLinkName(store, 0) == "Dongle");
    // an exact match to home Wi-Fi's own address
    store.setText(metric::kPcStreamLink1Address, "192.168.1.47", t0);
    CHECK(streamLinkName(store, 1) == "Home Wi-Fi");

    // a valid, sane address that matches neither adapter: generic naming, 1-based
    store.setText(metric::kPcStreamLink2Address, "203.0.113.5", t0);
    CHECK(streamLinkName(store, 2) == "Link 3");

    // the address field is N/A (readLinkEndpoint failed, or nothing published yet): generic naming
    store.setNa(metric::kPcStreamLink3Address, Reason::NotConnected, t0);
    CHECK(streamLinkName(store, 3) == "Link 4");

    // text that isn't a dotted IPv4 at all: generic naming, not a crash
    MetricStore garbage;
    garbage.setText(metric::kPcStreamLink0Address, "not-an-ip", t0);
    CHECK(streamLinkName(garbage, 0) == "Link 1");

    // out of range index: generic naming, never reads past Link3Address
    CHECK(streamLinkName(store, 4) == "Link 5");
    CHECK(streamLinkName(store, 99) == "Link 100");

    // an ambiguous address (somehow equal to both the headset's dongle and home IPs at once): neither is
    // trusted over the other, so it falls back rather than guessing
    MetricStore ambiguous;
    ambiguous.setText(metric::kNetDongleBase + metric::kNetAdapterIp, "10.0.0.1", t0);
    ambiguous.setText(metric::kNetHomeBase + metric::kNetAdapterIp, "10.0.0.1", t0);
    ambiguous.setText(metric::kPcStreamLink0Address, "10.0.0.1", t0);
    CHECK(streamLinkName(ambiguous, 0) == "Link 1");

    // an unset MetricStore (no PC app, nothing published): every index falls back cleanly
    MetricStore empty;
    for (unsigned j = 0; j < 4; ++j)
        CHECK(streamLinkName(empty, j) == "Link " + std::to_string(j + 1));
}

// ---- PcStreamReceiver: apply / expire / reset on the headset side ----

PcMetrics samplePc(double ping) {
    PcMetrics m;
    m.pcId = idOf(3);
    m.snapshotId = 1;
    m.values = {f32(metric::kPcStreamPing, static_cast<float>(ping))};
    return m;
}

void testReceiverLifecycle() {
    MetricStore store;
    PcStreamReceiver recv;

    // Before any packet: still PcAppNotRunning (the initial state of a fresh store, or recv.reset())
    recv.reset(store, 0.0);
    CHECK(!store.get(metric::kPcStreamPing)->ok);
    CHECK(store.get(metric::kPcStreamPing)->reason == Reason::PcAppNotRunning);

    // A packet arrives: the ping value becomes ok, and every other PC-stream metric goes NotSampledYet
    // (ready for the next snapshot) rather than staying stale
    recv.apply(samplePc(3.2), store, 10.0);
    CHECK(store.get(metric::kPcStreamPing)->ok && store.get(metric::kPcStreamPing)->f32 == 3.2f);
    CHECK(!store.get(metric::kPcStreamEncoderResets)->ok);
    CHECK(store.get(metric::kPcStreamEncoderResets)->reason == Reason::NotSampledYet);

    // expire() inside the 3 s window: no change
    recv.expire(store, 12.0);
    CHECK(store.get(metric::kPcStreamPing)->ok);

    // expire() past 3 s with no new packet: everything falls back to PcAppNotRunning
    recv.expire(store, 13.5);
    CHECK(!store.get(metric::kPcStreamPing)->ok);
    CHECK(store.get(metric::kPcStreamPing)->reason == Reason::PcAppNotRunning);

    // A fresh packet after the lapse brings it back
    recv.apply(samplePc(1.1), store, 20.0);
    CHECK(store.get(metric::kPcStreamPing)->ok && store.get(metric::kPcStreamPing)->f32 == 1.1f);

    // An explicit reset (BYE, PC vanished) behaves the same as an expiry
    recv.reset(store, 21.0);
    CHECK(!store.get(metric::kPcStreamPing)->ok);
    CHECK(store.get(metric::kPcStreamPing)->reason == Reason::PcAppNotRunning);
}

} // namespace

int main() {
    testIdAllowList();
    testRoundTrip();
    testRejectsNonStreamIds();
    testRejectsDuplicateIds();
    testRejectsWrongType();
    testRejectsNonFiniteFloat();
    testDecodeRejectsTampering();
    testUnknownIdsSkippedBetweenKnownEntriesAllTypes();
    testUnknownIdBadTypeOrShortBufferStillRejected();
    testEndpointAddressRoundTrip();
    testStreamLinkName();
    testStoreRoundTrip();
    testReceiverLifecycle();
    if (failures == 0) {
        std::printf("pc stream metrics: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
