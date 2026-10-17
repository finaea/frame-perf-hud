// Link wire format (core/link_wire): header, every message, typed values, METRICS splitting, BLOB reassembly,
// helpers. Platform-free: no POSIX headers, so it also builds with MSVC.
#include "link_wire.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;
using namespace fph::link;

using Bytes = std::vector<uint8_t>;

Id128 idOf(uint8_t seed) {
    Id128 id {};
    for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(seed + i * 7);
    return id;
}

bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99) return false;
        for (size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

/** Every proper prefix of a valid payload must fail to decode (and never crash). */
template <typename T, typename Fn>
bool allPrefixesFail(const Bytes& payload, Fn decode) {
    for (size_t n = 0; n < payload.size(); ++n) {
        T out;
        if (decode(payload.data(), n, out)) return false;
    }
    return true;
}

template <typename T, typename Fn>
bool trailingByteFails(const Bytes& payload, Fn decode) {
    Bytes longer = payload;
    longer.push_back(0);
    T out;
    return !decode(longer.data(), longer.size(), out);
}

// ---- header ----

void testHeader() {
    const Bytes payload = {1, 2, 3, 4, 5};
    const Bytes packet = encodePacket(PacketType::Blob, 0xA1B2C3D4u, payload);
    CHECK(packet.size() == kHeaderSize + payload.size());
    CHECK(packet[0] == 'F' && packet[1] == 'P' && packet[2] == 'H');
    Header h;
    const uint8_t* p = nullptr;
    CHECK(decodePacket(packet.data(), packet.size(), h, p) == DecodeError::Ok);
    CHECK(h.type == PacketType::Blob && h.seq == 0xA1B2C3D4u && h.length == 5 && h.flags == 0);
    CHECK(h.version == kProtocolVersion);
    CHECK(p == packet.data() + kHeaderSize && std::memcmp(p, payload.data(), 5) == 0);

    // an empty payload is fine
    const Bytes empty = encodePacket(PacketType::Bye, 1, {});
    CHECK(empty.size() == kHeaderSize);
    CHECK(decodePacket(empty.data(), empty.size(), h, p) == DecodeError::Ok && h.length == 0);

    // short header: every length below 12, and nullptr
    for (size_t n = 0; n < kHeaderSize; ++n) CHECK(decodePacket(packet.data(), n, h, p) == DecodeError::TooShort);
    CHECK(decodePacket(nullptr, 20, h, p) == DecodeError::TooShort);

    // bad magic
    Bytes bad = packet;
    bad[1] = 'X';
    CHECK(decodePacket(bad.data(), bad.size(), h, p) == DecodeError::BadMagic);

    // bad version: reported, header still filled in
    bad = packet;
    bad[3] = 2;
    CHECK(decodePacket(bad.data(), bad.size(), h, p) == DecodeError::BadVersion);
    CHECK(h.version == 2 && h.type == PacketType::Blob);

    // encrypted flag
    bad = packet;
    bad[5] = kFlagEncrypted;
    CHECK(decodePacket(bad.data(), bad.size(), h, p) == DecodeError::Encrypted);
    bad[5] = 0x02;  // some other (unknown) flag bit is not an error
    CHECK(decodePacket(bad.data(), bad.size(), h, p) == DecodeError::Ok);

    // payload shorter / longer than the header says
    CHECK(decodePacket(packet.data(), packet.size() - 1, h, p) == DecodeError::TooShort);
    bad = packet;
    bad.push_back(0);
    CHECK(decodePacket(bad.data(), bad.size(), h, p) == DecodeError::TooLong);

    // length field over 1200 (with that many bytes really there)
    Bytes big = encodePacket(PacketType::Blob, 1, Bytes(kMaxPayload, 7));
    CHECK(big.size() == kMaxPacket);
    CHECK(decodePacket(big.data(), big.size(), h, p) == DecodeError::Ok && h.length == kMaxPayload);
    big.push_back(7);
    big[10] = static_cast<uint8_t>((kMaxPayload + 1) & 0xFF);
    big[11] = static_cast<uint8_t>((kMaxPayload + 1) >> 8);
    CHECK(decodePacket(big.data(), big.size(), h, p) == DecodeError::TooLong);
    // a huge length with few bytes: still rejected, no over-read
    Bytes huge = packet;
    huge[10] = 0xFF;
    huge[11] = 0xFF;
    CHECK(decodePacket(huge.data(), huge.size(), h, p) == DecodeError::TooLong);

    // encodePacket with an oversized payload
    CHECK(encodePacket(PacketType::Blob, 1, Bytes(kMaxPayload + 1, 0)).empty());
    CHECK(!encodePacket(PacketType::Blob, 1, Bytes(kMaxPayload, 0)).empty());

    // names
    CHECK(std::strcmp(packetTypeName(PacketType::Hello), "HELLO") == 0);
    CHECK(std::strcmp(packetTypeName(static_cast<PacketType>(99)), "?") == 0);
    CHECK(std::strcmp(peerStatusName(PeerStatus::UnknownSubscriber), "unknown_subscriber") == 0);
    CHECK(std::strcmp(rejectReasonName(RejectReason::SaveFailed), "save_failed") == 0);
}

// ---- messages ----

void testMessages() {
    {
        Hello m;
        m.pcId = idOf(1);
        m.pcName = "Jack's PC \xE2\x82\xAC";
        const Bytes b = encodeHello(m);
        Hello o;
        CHECK(decodeHello(b.data(), b.size(), o));
        CHECK(o.pcId == m.pcId && o.pcName == m.pcName);
        CHECK((allPrefixesFail<Hello>(b, decodeHello)));
        CHECK((trailingByteFails<Hello>(b, decodeHello)));
        // a name over 63 bytes is cut at a character boundary
        Hello longName;
        std::string accents;
        for (int i = 0; i < 40; ++i) accents += "\xC3\xA9";  // 80 bytes
        longName.pcName = accents;
        const Bytes lb = encodeHello(longName);
        CHECK(decodeHello(lb.data(), lb.size(), o));
        CHECK(o.pcName.size() <= kMaxNameBytes && validUtf8(o.pcName) && o.pcName.size() == 62);
        // empty name
        longName.pcName.clear();
        const Bytes eb = encodeHello(longName);
        CHECK(decodeHello(eb.data(), eb.size(), o) && o.pcName.empty());
        CHECK(!decodeHello(nullptr, 5, o));
    }
    {
        HelloAck m;
        m.headsetId = idOf(2);
        m.name = "Frame";
        m.status = PeerStatus::Pending;
        m.addresses = {{"wlan0", 0xC0A8012Fu}, {"wlanap", 0x0A234E01u}};
        m.layoutVersion = 77;
        m.seenIp = 0xC0A8012Du;
        m.seenPort = 51234;
        const Bytes b = encodeHelloAck(m);
        HelloAck o;
        CHECK(decodeHelloAck(b.data(), b.size(), o));
        CHECK(o.headsetId == m.headsetId && o.name == "Frame" && o.protocol == kProtocolVersion);
        CHECK(o.status == PeerStatus::Pending && o.layoutVersion == 77);
        CHECK(o.addresses.size() == 2 && o.addresses[0].ifName == "wlan0" && o.addresses[0].ip == 0xC0A8012Fu);
        CHECK(o.addresses[1].ifName == "wlanap" && o.addresses[1].ip == 0x0A234E01u);
        CHECK(o.seenIp == 0xC0A8012Du && o.seenPort == 51234);
        CHECK((allPrefixesFail<HelloAck>(b, decodeHelloAck)));
        CHECK((trailingByteFails<HelloAck>(b, decodeHelloAck)));
        for (PeerStatus s : {PeerStatus::Allowed, PeerStatus::Denied, PeerStatus::UnknownSubscriber}) {
            m.status = s;
            const Bytes sb = encodeHelloAck(m);
            CHECK(decodeHelloAck(sb.data(), sb.size(), o) && o.status == s);
        }
        // status byte out of range / more than 8 addresses claimed
        const size_t statusAt = 16 + 1 + m.name.size() + 1;
        Bytes bad = b;
        bad[statusAt] = 9;
        CHECK(!decodeHelloAck(bad.data(), bad.size(), o));
        bad = b;
        bad[statusAt + 1] = 9;
        CHECK(!decodeHelloAck(bad.data(), bad.size(), o));
        // more than 8 addresses given: only 8 go out; long interface names are cut to 15
        HelloAck many;
        for (uint32_t i = 0; i < 12; ++i) many.addresses.push_back({"averyveryverylonginterface", 100 + i});
        const Bytes mb = encodeHelloAck(many);
        CHECK(decodeHelloAck(mb.data(), mb.size(), o) && o.addresses.size() == 8);
        CHECK(o.addresses[0].ifName == "averyveryverylo" && o.addresses[7].ip == 107);
        // none
        HelloAck none;
        const Bytes nb = encodeHelloAck(none);
        CHECK(decodeHelloAck(nb.data(), nb.size(), o) && o.addresses.empty());
    }
    {
        Subscribe m;
        m.pcId = idOf(3);
        m.wants = kWantMetrics | kWantCpuList | kWantRamList | (1u << 9);
        const Bytes b = encodeSubscribe(m);
        Subscribe o;
        CHECK(decodeSubscribe(b.data(), b.size(), o) && o.pcId == m.pcId && o.wants == m.wants);
        CHECK((allPrefixesFail<Subscribe>(b, decodeSubscribe)));
        CHECK((trailingByteFails<Subscribe>(b, decodeSubscribe)));
    }
    {
        SubscribeAck m;
        m.status = PeerStatus::UnknownSubscriber;
        m.leaseMs = 6000;
        const Bytes b = encodeSubscribeAck(m);
        SubscribeAck o;
        CHECK(decodeSubscribeAck(b.data(), b.size(), o) && o.status == m.status && o.leaseMs == 6000);
        CHECK((allPrefixesFail<SubscribeAck>(b, decodeSubscribeAck)));
        CHECK((trailingByteFails<SubscribeAck>(b, decodeSubscribeAck)));
        Bytes bad = b;
        bad[0] = 4;
        CHECK(!decodeSubscribeAck(bad.data(), bad.size(), o));
        CHECK(SubscribeAck().leaseMs == 6000);
    }
    {
        LayoutGet m;
        m.pcId = idOf(4);
        const Bytes b = encodeLayoutGet(m);
        LayoutGet o;
        CHECK(decodeLayoutGet(b.data(), b.size(), o) && o.pcId == m.pcId);
        CHECK((allPrefixesFail<LayoutGet>(b, decodeLayoutGet)));
        CHECK((trailingByteFails<LayoutGet>(b, decodeLayoutGet)));
    }
    {
        LayoutSet m;
        m.pcId = idOf(5);
        m.requestId = 0xDEADBEEFu;
        m.baseVersion = 12;
        m.layoutJson = "{\"version\":0,\"columns\":[]}";
        const Bytes b = encodeLayoutSet(m);
        LayoutSet o;
        CHECK(decodeLayoutSet(b.data(), b.size(), o));
        CHECK(o.pcId == m.pcId && o.requestId == m.requestId && o.baseVersion == 12 && o.layoutJson == m.layoutJson);
        // the JSON is the rest: anything shorter than the 24-byte fixed part fails, an empty JSON is accepted
        for (size_t n = 0; n < 24; ++n) CHECK(!decodeLayoutSet(b.data(), n, o));
        CHECK(decodeLayoutSet(b.data(), 24, o) && o.layoutJson.empty());
    }
    {
        LayoutAck m;
        m.requestId = 9;
        m.version = 10;
        const Bytes b = encodeLayoutAck(m);
        LayoutAck o;
        CHECK(decodeLayoutAck(b.data(), b.size(), o) && o.requestId == 9 && o.version == 10);
        CHECK((allPrefixesFail<LayoutAck>(b, decodeLayoutAck)));
        CHECK((trailingByteFails<LayoutAck>(b, decodeLayoutAck)));
    }
    {
        for (RejectReason r : {RejectReason::Stale, RejectReason::NotAllowed, RejectReason::InvalidLayout,
                               RejectReason::SaveFailed, RejectReason::UnknownSubscriber}) {
            LayoutReject m;
            m.requestId = 5;
            m.currentVersion = 6;
            m.reason = r;
            const Bytes b = encodeLayoutReject(m);
            LayoutReject o;
            CHECK(decodeLayoutReject(b.data(), b.size(), o));
            CHECK(o.requestId == 5 && o.currentVersion == 6 && o.reason == r);
            CHECK((allPrefixesFail<LayoutReject>(b, decodeLayoutReject)));
            CHECK((trailingByteFails<LayoutReject>(b, decodeLayoutReject)));
            Bytes bad = b;
            bad[8] = 5;
            CHECK(!decodeLayoutReject(bad.data(), bad.size(), o));
        }
    }
    {
        Bye m;
        m.id = idOf(6);
        const Bytes b = encodeBye(m);
        Bye o;
        CHECK(decodeBye(b.data(), b.size(), o) && o.id == m.id);
        CHECK((allPrefixesFail<Bye>(b, decodeBye)));
        CHECK((trailingByteFails<Bye>(b, decodeBye)));
    }
    {
        ProcessListWire m;
        m.generation = 0x1122334455667788ull;
        m.cpuValid = true;
        m.rows = {{"vrserver", 1234, 1, 87.5f, 412.25f}, {"chrome", 0, 12, 3.0f, 9000.0f}};
        m.rows.push_back({std::string(100, 'n'), -1, 65535, 0.0f, 0.0f});
        const Bytes b = encodeProcessList(m);
        ProcessListWire o;
        CHECK(decodeProcessList(b.data(), b.size(), o));
        CHECK(o.generation == m.generation && o.cpuValid && o.rows.size() == 3);
        CHECK(o.rows[0].name == "vrserver" && o.rows[0].pid == 1234 && o.rows[0].cpuCorePct == 87.5f &&
              o.rows[0].rssMiB == 412.25f);
        CHECK(o.rows[1].count == 12 && o.rows[1].pid == 0);
        CHECK(o.rows[2].name.size() == kMaxNameBytes && o.rows[2].pid == -1 && o.rows[2].count == 65535);
        CHECK((allPrefixesFail<ProcessListWire>(b, decodeProcessList)));
        CHECK((trailingByteFails<ProcessListWire>(b, decodeProcessList)));
        Bytes bad = b;
        bad[0] = 2;  // unknown format
        CHECK(!decodeProcessList(bad.data(), bad.size(), o));
        // at most 255 rows
        ProcessListWire many;
        many.rows.assign(300, ProcessRowWire());
        const Bytes mb = encodeProcessList(many);
        CHECK(decodeProcessList(mb.data(), mb.size(), o) && o.rows.size() == 255 && !o.cpuValid);
        ProcessListWire none;
        const Bytes nb = encodeProcessList(none);
        CHECK(decodeProcessList(nb.data(), nb.size(), o) && o.rows.empty());
    }
}

// ---- typed values + METRICS ----

WireValue valueFor(uint16_t id, int kind) {
    WireValue v;
    v.id = id;
    switch (kind % 5) {
        case 0:
            v.type = WireType::F32;
            v.ok = true;
            v.f32 = 0.5f * static_cast<float>(id);
            break;
        case 1:
            v.type = WireType::I32;
            v.ok = true;
            v.i32 = -static_cast<int32_t>(id) * 1000;
            break;
        case 2:
            v.type = WireType::Enum;
            v.ok = true;
            v.enumValue = static_cast<uint8_t>(id);
            break;
        case 3:
            v.type = WireType::ShortString;
            v.ok = true;
            v.text = std::string(kShortStringMax, static_cast<char>('a' + id % 26));  // 31 bytes
            break;
        default:
            v.type = WireType::F32;
            v.ok = false;
            v.reason = static_cast<Reason>(1 + id % 11);
            break;
    }
    return v;
}

void testValues() {
    // one of each type, plus N/A with a reason of every wire type
    std::vector<WireValue> values;
    for (int k = 0; k < 5; ++k) values.push_back(valueFor(static_cast<uint16_t>(100 + k), k));
    for (WireType t : {WireType::I32, WireType::Enum, WireType::ShortString}) {
        WireValue na;
        na.id = 200 + static_cast<uint16_t>(t);
        na.type = t;
        na.ok = false;
        na.reason = Reason::NoSteamVr;
        values.push_back(na);
    }
    LinkState state;
    state.layoutVersion = 5;
    state.sessionId = -3;
    state.sessionStartUnix = 1760000000123ll;
    state.appKey = "steam:250820";
    state.appName = "SteamVR \xE2\x82\xAC";
    const std::vector<Bytes> parts = encodeMetrics(123456789012ull, 42, state, values);
    CHECK(parts.size() == 1);
    Metrics m;
    CHECK(decodeMetrics(parts[0].data(), parts[0].size(), m));
    CHECK(m.headsetMs == 123456789012ull && m.snapshotId == 42 && m.part == 0 && m.partCount == 1);
    CHECK(m.state == state);
    CHECK(m.values.size() == values.size());
    for (size_t i = 0; i < values.size() && i < m.values.size(); ++i) CHECK(m.values[i] == values[i]);
    CHECK(m.values[7].reason == Reason::NoSteamVr && !m.values[7].ok && m.values[7].type == WireType::ShortString);

    // every prefix fails, trailing byte fails
    CHECK((allPrefixesFail<Metrics>(parts[0], decodeMetrics)));
    CHECK((trailingByteFails<Metrics>(parts[0], decodeMetrics)));
    // part >= partCount, partCount 0
    Bytes bad = parts[0];
    bad[12] = 1;
    CHECK(!decodeMetrics(bad.data(), bad.size(), m));
    bad = parts[0];
    bad[13] = 0;
    CHECK(!decodeMetrics(bad.data(), bad.size(), m));
    // bad value type / status
    Metrics probe;
    {
        std::vector<WireValue> one = {valueFor(1, 0)};
        Bytes p = encodeMetrics(0, 1, LinkState(), one)[0];
        const size_t valueAt = p.size() - 8;  // id u16, type, status, f32
        Bytes t = p;
        t[valueAt + 2] = 9;
        CHECK(!decodeMetrics(t.data(), t.size(), probe));
        t = p;
        t[valueAt + 3] = 2;
        CHECK(!decodeMetrics(t.data(), t.size(), probe));
        CHECK(decodeMetrics(p.data(), p.size(), probe));
    }
    // LINK_STATE strings are cut to 63 bytes
    LinkState longState;
    longState.appKey = std::string(100, 'k');
    longState.appName = std::string(100, 'n');
    const std::vector<Bytes> lp = encodeMetrics(1, 1, longState, {});
    CHECK(lp.size() == 1 && decodeMetrics(lp[0].data(), lp[0].size(), m));
    CHECK(m.state.appKey.size() == kMaxNameBytes && m.state.appName.size() == kMaxNameBytes && m.values.empty());

    // f32 corner cases survive bit for bit
    WireValue f;
    f.id = 1;
    f.ok = true;
    f.type = WireType::F32;
    f.f32 = -0.0f;
    const Bytes fb = encodeMetrics(0, 1, LinkState(), {f})[0];
    CHECK(decodeMetrics(fb.data(), fb.size(), m) && m.values[0] == f);
}

void testToWire() {
    MetricValue v;
    v.ok = true;
    v.type = WireType::ShortString;
    v.text = std::string(40, 'z');
    v.enumValue = 300;
    WireValue w = toWire(7, v);
    CHECK(w.id == 7 && w.ok && w.type == WireType::ShortString && w.text.size() == kShortStringMax);
    CHECK(w.enumValue == 255);  // clamped to a u8
    CHECK(w.reason == Reason::None);
    MetricValue na;
    na.ok = false;
    na.reason = Reason::Paused;
    na.type = WireType::I32;
    w = toWire(8, na);
    CHECK(!w.ok && w.reason == Reason::Paused && w.type == WireType::I32);
}

void testMetricsSplit() {
    std::vector<WireValue> values;
    for (int i = 0; i < 300; ++i) values.push_back(valueFor(static_cast<uint16_t>(i + 1), i));
    LinkState state;
    state.layoutVersion = 9;
    state.sessionId = 4;
    state.sessionStartUnix = 1700000000;
    state.appKey = "app";
    state.appName = std::string(63, 'N');
    const std::vector<Bytes> parts = encodeMetrics(5000, 777, state, values);
    CHECK(parts.size() > 1 && parts.size() < 255);
    std::vector<WireValue> all;
    for (size_t i = 0; i < parts.size(); ++i) {
        CHECK(parts[i].size() <= kMaxPayload);
        // fits in a packet
        CHECK(!encodePacket(PacketType::Metrics, 1, parts[i]).empty());
        Metrics m;
        CHECK(decodeMetrics(parts[i].data(), parts[i].size(), m));
        CHECK(m.part == i && m.partCount == parts.size());
        CHECK(m.snapshotId == 777 && m.headsetMs == 5000);
        CHECK(m.state == state);  // LINK_STATE (incl. the layout version) in every part
        CHECK(!m.values.empty());
        for (const WireValue& v : m.values) all.push_back(v);
    }
    CHECK(all.size() == values.size());
    for (size_t i = 0; i < all.size() && i < values.size(); ++i) CHECK(all[i] == values[i]);

    // empty snapshot: one part with LINK_STATE and no values
    const std::vector<Bytes> empty = encodeMetrics(1, 2, state, {});
    Metrics m;
    CHECK(empty.size() == 1 && decodeMetrics(empty[0].data(), empty[0].size(), m) && m.values.empty() &&
          m.state == state && m.partCount == 1);
}

// ---- blobs ----

Bytes patternData(size_t n, uint8_t seed) {
    Bytes d(n);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(seed + i * 31 + (i >> 8));
    return d;
}

/** Splits and decodes: chunks point into the returned payload vectors, which must outlive them. */
struct Split {
    std::vector<Bytes> payloads;
    std::vector<BlobChunk> chunks;
};

Split makeSplit(uint32_t id, BlobKind kind, uint8_t stream, const Bytes& data) {
    Split s;
    s.payloads = splitBlob(id, kind, stream, data);
    s.chunks.resize(s.payloads.size());
    for (size_t i = 0; i < s.payloads.size(); ++i) {
        CHECK(s.payloads[i].size() <= kMaxPayload);
        CHECK(decodeBlobChunk(s.payloads[i].data(), s.payloads[i].size(), s.chunks[i]));
    }
    return s;
}

void testBlobs() {
    const Bytes data = patternData(3000, 5);  // 3 chunks
    Split a = makeSplit(10, BlobKind::ProcessList, 0, data);
    CHECK(a.chunks.size() == 3);
    CHECK(a.chunks[0].blobId == 10 && a.chunks[0].kind == BlobKind::ProcessList && a.chunks[0].count == 3 &&
          a.chunks[0].totalSize == 3000 && a.chunks[2].index == 2 && a.chunks[0].size == kBlobChunkData);
    Blob out;

    // in order
    {
        BlobAssembler asmb;
        CHECK(!asmb.add(1, a.chunks[0], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[1], 0.0, out));
        CHECK(asmb.inProgress() == 1);
        CHECK(asmb.add(1, a.chunks[2], 0.0, out));
        CHECK(out.data == data && out.blobId == 10 && out.kind == BlobKind::ProcessList && out.stream == 0 &&
              out.peer == 1);
        CHECK(asmb.inProgress() == 0);
    }
    // out of order
    {
        BlobAssembler asmb;
        CHECK(!asmb.add(1, a.chunks[2], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[0], 0.0, out));
        CHECK(asmb.add(1, a.chunks[1], 0.0, out));
        CHECK(out.data == data);
    }
    // a missing piece never completes; expires after 1 s
    {
        BlobAssembler asmb;
        CHECK(!asmb.add(1, a.chunks[0], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[2], 0.1, out));
        asmb.expire(0.9);
        CHECK(asmb.inProgress() == 1);
        asmb.expire(1.01);
        CHECK(asmb.inProgress() == 0);
        CHECK(asmb.dropped() >= 1);
        // the late piece alone doesn't complete anything
        CHECK(!asmb.add(1, a.chunks[1], 1.5, out));
        CHECK(asmb.inProgress() == 1);
        // and the old chunks can't "resurrect" it: it needs all 3 again
        CHECK(!asmb.add(1, a.chunks[0], 1.6, out));
        CHECK(asmb.add(1, a.chunks[2], 1.7, out) && out.data == data);
    }
    // expiry counts from the first chunk, not the last
    {
        BlobAssembler asmb;
        asmb.add(1, a.chunks[0], 0.0, out);
        asmb.add(1, a.chunks[1], 0.95, out);
        asmb.expire(1.05);
        CHECK(asmb.inProgress() == 0);
    }
    // duplicates
    {
        BlobAssembler asmb;
        CHECK(!asmb.add(1, a.chunks[0], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[0], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[1], 0.0, out));
        CHECK(!asmb.add(1, a.chunks[1], 0.0, out));
        CHECK(asmb.add(1, a.chunks[2], 0.0, out) && out.data == data);
    }
    // a newer id replaces the incomplete older one; an older id's straggler is dropped
    {
        const Bytes data2 = patternData(2500, 9);
        Split b = makeSplit(11, BlobKind::ProcessList, 0, data2);
        BlobAssembler asmb;
        asmb.add(1, a.chunks[0], 0.0, out);
        CHECK(!asmb.add(1, b.chunks[0], 0.1, out));  // replaces id 10
        CHECK(asmb.inProgress() == 1);
        const uint64_t before = asmb.dropped();
        CHECK(!asmb.add(1, a.chunks[1], 0.2, out));  // id 10 straggler
        CHECK(asmb.dropped() == before + 1 && asmb.inProgress() == 1);
        CHECK(!asmb.add(1, a.chunks[2], 0.2, out));
        for (size_t i = 1; i < b.chunks.size(); ++i) {
            const bool done = asmb.add(1, b.chunks[i], 0.3, out);
            CHECK(done == (i + 1 == b.chunks.size()));
        }
        CHECK(out.blobId == 11 && out.data == data2);
        // a chunk of an already completed id is dropped, and starts nothing
        const uint64_t d2 = asmb.dropped();
        CHECK(!asmb.add(1, b.chunks[0], 0.4, out));
        CHECK(asmb.inProgress() == 0 && asmb.dropped() == d2 + 1);
        // so is an even older completed one
        CHECK(!asmb.add(1, a.chunks[0], 0.4, out));
        CHECK(asmb.inProgress() == 0);
    }
    // the same id with a different shape is ignored
    {
        Split other = makeSplit(10, BlobKind::ProcessList, 0, patternData(1500, 1));  // 2 chunks, id 10
        BlobAssembler asmb;
        asmb.add(1, a.chunks[0], 0.0, out);
        CHECK(!asmb.add(1, other.chunks[1], 0.0, out));
        CHECK(asmb.inProgress() == 1);
        CHECK(!asmb.add(1, a.chunks[1], 0.0, out));
        CHECK(asmb.add(1, a.chunks[2], 0.0, out) && out.data == data);
    }
    // different kinds / streams / peers never cancel each other (even when their ids are "newer")
    {
        const Bytes dk = patternData(2800, 20);
        Split sKind = makeSplit(50, BlobKind::Layout, 0, dk);
        Split sStream = makeSplit(60, BlobKind::ProcessList, 1, dk);
        Split sPeer = makeSplit(70, BlobKind::ProcessList, 0, dk);
        BlobAssembler asmb;
        asmb.add(1, a.chunks[0], 0.0, out);       // (peer 1, ProcessList, 0), id 10
        asmb.add(1, sKind.chunks[0], 0.0, out);   // (1, Layout, 0)
        asmb.add(1, sStream.chunks[0], 0.0, out); // (1, ProcessList, 1)
        asmb.add(2, sPeer.chunks[0], 0.0, out);   // (2, ProcessList, 0)
        CHECK(asmb.inProgress() == 4);
        const uint64_t before = asmb.dropped();
        for (size_t i = 1; i < sKind.chunks.size(); ++i) asmb.add(1, sKind.chunks[i], 0.1, out);
        CHECK(out.kind == BlobKind::Layout && out.data == dk);
        for (size_t i = 1; i < sStream.chunks.size(); ++i) asmb.add(1, sStream.chunks[i], 0.1, out);
        CHECK(out.stream == 1 && out.data == dk);
        for (size_t i = 1; i < sPeer.chunks.size(); ++i) asmb.add(2, sPeer.chunks[i], 0.1, out);
        CHECK(out.peer == 2 && out.data == dk);
        CHECK(asmb.dropped() == before);
        // and the first one still completes
        CHECK(!asmb.add(1, a.chunks[1], 0.2, out));
        CHECK(asmb.add(1, a.chunks[2], 0.2, out) && out.data == data && out.blobId == 10);
    }
    // id wrap-around
    {
        CHECK(newerId(1, 0) && !newerId(0, 1) && !newerId(5, 5));
        CHECK(newerId(0, 0xFFFFFFFFu) && !newerId(0xFFFFFFFFu, 0));
        CHECK(newerId(2, 0xFFFFFFF0u) && !newerId(0xFFFFFFF0u, 2));
        Split old = makeSplit(0xFFFFFFFFu, BlobKind::Layout, 0, data);
        Split wrapped = makeSplit(0, BlobKind::Layout, 0, data);
        Split after = makeSplit(1, BlobKind::Layout, 0, data);
        BlobAssembler asmb;
        asmb.add(1, old.chunks[0], 0.0, out);
        CHECK(!asmb.add(1, wrapped.chunks[0], 0.1, out));  // id 0 is newer than 0xFFFFFFFF
        CHECK(!asmb.add(1, old.chunks[1], 0.1, out));      // so the old straggler is dropped
        CHECK(!asmb.add(1, wrapped.chunks[1], 0.1, out));
        CHECK(asmb.add(1, wrapped.chunks[2], 0.1, out) && out.blobId == 0 && out.data == data);
        CHECK(!asmb.add(1, old.chunks[0], 0.2, out));  // completed id 0: the id before it is old
        CHECK(asmb.inProgress() == 0);
        CHECK(!asmb.add(1, after.chunks[0], 0.3, out));
        CHECK(!asmb.add(1, after.chunks[1], 0.3, out));
        CHECK(asmb.add(1, after.chunks[2], 0.3, out) && out.blobId == 1);
    }
    // empty blob
    {
        Split e = makeSplit(3, BlobKind::Layout, 0, Bytes());
        CHECK(e.chunks.size() == 1 && e.chunks[0].size == 0 && e.chunks[0].totalSize == 0 && e.chunks[0].count == 1);
        BlobAssembler asmb;
        CHECK(asmb.add(1, e.chunks[0], 0.0, out));
        CHECK(out.data.empty() && out.blobId == 3);
    }
    // exact multiple of the chunk size, and one byte over
    {
        Split exact = makeSplit(4, BlobKind::Layout, 0, patternData(kBlobChunkData * 2, 3));
        CHECK(exact.chunks.size() == 2 && exact.chunks[1].size == kBlobChunkData);
        Split over = makeSplit(5, BlobKind::Layout, 0, patternData(kBlobChunkData * 2 + 1, 3));
        CHECK(over.chunks.size() == 3 && over.chunks[2].size == 1);
        Split one = makeSplit(6, BlobKind::Layout, 0, Bytes(1, 7));
        CHECK(one.chunks.size() == 1 && one.chunks[0].size == 1);
    }
    // too big: more than kMaxBlobChunks chunks
    CHECK(splitBlob(1, BlobKind::Camera, 0, Bytes(kBlobChunkData * kMaxBlobChunks + 1, 0)).empty());
    CHECK(splitBlob(1, BlobKind::Camera, 0, Bytes(kBlobChunkData * kMaxBlobChunks, 0)).size() == kMaxBlobChunks);

    // chunk shape mismatches rejected by decodeBlobChunk
    {
        const auto chunk = [](uint16_t index, uint16_t count, uint32_t total, size_t dataBytes) {
            Writer w;
            w.u32(1);
            w.u8(2);
            w.u8(0);
            w.u16(index);
            w.u16(count);
            w.u32(total);
            for (size_t i = 0; i < dataBytes; ++i) w.u8(0xAB);
            return w.bytes();
        };
        BlobChunk c;
        Bytes ok = chunk(0, 1, 10, 10);
        CHECK(decodeBlobChunk(ok.data(), ok.size(), c) && c.size == 10);
        Bytes b = chunk(0, 1, 10, 9);   // short data
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(0, 1, 10, 11);        // long data
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(1, 1, 10, 10);        // index >= count
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(0, 0, 0, 0);          // count 0
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(0, 2, 10, 10);        // count doesn't match the total size
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(0, 1, 5000, 1186);    // count too small for the total
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(0, 1025, 1186u * 1025u, 1186);  // over kMaxBlobChunks
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(1, 2, 1186 + 5, 4);   // last chunk one byte short
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        b = chunk(1, 2, 1186 + 5, 5);
        CHECK(decodeBlobChunk(b.data(), b.size(), c) && c.index == 1 && c.size == 5);
        b = chunk(0, 1, 0, 1);          // empty blob with data
        CHECK(!decodeBlobChunk(b.data(), b.size(), c));
        // every prefix of a real chunk fails
        for (size_t n = 0; n < a.payloads[2].size(); ++n) CHECK(!decodeBlobChunk(a.payloads[2].data(), n, c));
        CHECK(!decodeBlobChunk(nullptr, 0, c));
    }
}

void testForgetPeer() {
    const Bytes data = patternData(3000, 5);
    Split big = makeSplit(100, BlobKind::LayoutSet, 0, data);
    Split restarted = makeSplit(1, BlobKind::LayoutSet, 0, data);
    Split other = makeSplit(100, BlobKind::LayoutSet, 0, data);
    const auto feed = [](BlobAssembler& a, uint64_t peer, const Split& s, double now, Blob& out) {
        bool done = false;
        for (const BlobChunk& c : s.chunks) done = a.add(peer, c, now, out);
        return done;
    };
    Blob out;
    // (a) forgetPeer lets a restarted sender (ids from 1) through; other peers keep their state
    {
        BlobAssembler asmb;
        CHECK(feed(asmb, 7, big, 0.0, out) && out.blobId == 100);
        CHECK(feed(asmb, 8, other, 0.0, out) && out.blobId == 100);
        CHECK(!feed(asmb, 7, restarted, 1.0, out));  // dropped as older: that's why forgetPeer exists
        asmb.forgetPeer(7);
        CHECK(feed(asmb, 7, restarted, 2.0, out) && out.blobId == 1 && out.data == data);
        // peer 8 still remembers id 100: its own id-1 blob is dropped
        CHECK(!feed(asmb, 8, restarted, 2.0, out));
        // forgetting a peer with a blob in progress drops it too
        asmb.add(9, big.chunks[0], 3.0, out);
        CHECK(asmb.inProgress() == 1);
        asmb.forgetPeer(9);
        CHECK(asmb.inProgress() == 0);
        asmb.forgetPeer(12345);  // unknown peer: harmless
    }
    // (b) without forgetPeer: a sender stuck on old ids is forgotten 10 s after its last accepted chunk
    {
        BlobAssembler asmb;
        CHECK(feed(asmb, 7, big, 0.0, out));
        double t = 0.5;
        for (; t < 9.9; t += 0.5) {
            CHECK(!feed(asmb, 7, restarted, t, out));  // every chunk dropped...
            asmb.expire(t);
        }
        CHECK(!feed(asmb, 7, restarted, 9.9, out));  // ...and still dropped just before 10 s
        asmb.expire(9.9);
        CHECK(!feed(asmb, 7, restarted, 10.0, out));
        asmb.expire(10.1);
        CHECK(feed(asmb, 7, restarted, 10.2, out) && out.blobId == 1 && out.data == data);
    }
}

// ---- R3: CONTROL_STATE / CONTROL_ACTION / CONTROL_RESULT ----

void testControl() {
    CHECK(std::strcmp(packetTypeName(PacketType::ControlState), "CONTROL_STATE") == 0);
    CHECK(std::strcmp(packetTypeName(PacketType::ControlAction), "CONTROL_ACTION") == 0);
    CHECK(std::strcmp(packetTypeName(PacketType::ControlResult), "CONTROL_RESULT") == 0);
    CHECK(static_cast<int>(PacketType::ControlState) == 12 && static_cast<int>(PacketType::ControlAction) == 13 &&
          static_cast<int>(PacketType::ControlResult) == 14);

    // CONTROL_STATE: 8 profiles round trip
    ControlState st;
    st.revision = 41;
    st.layoutVersion = 7;
    st.flags = kControlRecording | kControlUnsaved;
    st.maxProfiles = 8;
    st.activeId = "profile-3";
    for (int i = 1; i <= 8; ++i) st.profiles.push_back({"profile-" + std::to_string(i), "Name " + std::to_string(i)});
    st.status = "Using Gaming";
    st.recordStatus = "Recording 3:12 to /x/y.csv";
    st.crashStatus = "Crash log saved (12:55)";
    st.powerOff = "Normal shutdown";
    const Bytes sb = encodeControlState(st);
    ControlState sd;
    CHECK(decodeControlState(sb.data(), sb.size(), sd));
    CHECK(sd.revision == 41 && sd.layoutVersion == 7 && sd.flags == st.flags && sd.maxProfiles == 8);
    CHECK(sd.activeId == "profile-3" && sd.profiles == st.profiles && sd.profiles.size() == 8);
    CHECK(sd.status == st.status && sd.recordStatus == st.recordStatus && sd.crashStatus == st.crashStatus &&
          sd.powerOff == st.powerOff);

    // empty state
    ControlState empty;
    const Bytes eb = encodeControlState(empty);
    ControlState ed;
    ed.profiles.push_back({"x", "y"});
    CHECK(decodeControlState(eb.data(), eb.size(), ed));
    CHECK(ed.profiles.empty() && ed.activeId.empty() && ed.status.empty() && ed.revision == 0);
    CHECK(ed.sameContent(empty));

    // max-length strings cut at UTF-8 boundaries (3-byte euro: 31 -> 30, 15 -> 15, 127 -> 126, 63 -> 63)
    std::string euros;
    for (int i = 0; i < 60; ++i) euros += "\xE2\x82\xAC";
    ControlState big;
    big.activeId = euros;
    big.profiles.push_back({euros, euros});
    big.status = euros;
    big.recordStatus = euros;
    big.crashStatus = euros;
    big.powerOff = euros;
    const Bytes bb = encodeControlState(big);
    ControlState bd;
    CHECK(decodeControlState(bb.data(), bb.size(), bd));
    CHECK(validUtf8(bd.activeId) && bd.activeId.size() == 15);
    CHECK(bd.profiles.size() == 1 && validUtf8(bd.profiles[0].id) && bd.profiles[0].id.size() == 15);
    CHECK(validUtf8(bd.profiles[0].name) && bd.profiles[0].name.size() == 30);
    CHECK(validUtf8(bd.status) && bd.status.size() == 126);
    CHECK(validUtf8(bd.recordStatus) && bd.recordStatus.size() == 126);
    CHECK(validUtf8(bd.crashStatus) && bd.crashStatus.size() == 63);
    CHECK(validUtf8(bd.powerOff) && bd.powerOff.size() == 30);
    ControlState ascii;
    ascii.status = std::string(300, 'a');
    ascii.profiles.push_back({std::string(40, 'i'), std::string(40, 'n')});
    ControlState ad;
    const Bytes ab = encodeControlState(ascii);
    CHECK(decodeControlState(ab.data(), ab.size(), ad));
    CHECK(ad.status.size() == 127 && ad.profiles[0].id.size() == 15 && ad.profiles[0].name.size() == 31);

    // forward compat: trailing bytes ignored; short payloads fail
    Bytes more = sb;
    for (int i = 0; i < 9; ++i) more.push_back(static_cast<uint8_t>(0xA0 + i));
    ControlState md;
    CHECK(decodeControlState(more.data(), more.size(), md) && md.profiles == st.profiles && md.powerOff == st.powerOff);
    CHECK(allPrefixesFail<ControlState>(sb, [](const uint8_t* p, size_t n, ControlState& o) {
        return decodeControlState(p, n, o);
    }));
    ControlState untouched;
    untouched.revision = 99;
    CHECK(!decodeControlState(sb.data(), sb.size() - 1, untouched) && untouched.revision == 99);

    // > 8 profiles on the wire fail (hand-built: the encoder clamps)
    Bytes nine = encodeControlState(empty);
    const size_t countAt = 4 + 4 + 1 + 1 + 1;  // revision, version, flags, max, activeId length byte
    CHECK(nine[countAt] == 0);
    nine[countAt] = 9;
    for (int i = 0; i < 9; ++i) nine.insert(nine.begin() + countAt + 1, {0, 0});
    ControlState nd;
    CHECK(!decodeControlState(nine.data(), nine.size(), nd));
    ControlState twelve;
    for (int i = 0; i < 12; ++i) twelve.profiles.push_back({"p" + std::to_string(i), "n"});
    const Bytes tb = encodeControlState(twelve);
    ControlState td;
    CHECK(decodeControlState(tb.data(), tb.size(), td) && td.profiles.size() == kMaxControlProfiles);

    // sameContent ignores only the revision
    ControlState a = st, b = st;
    b.revision = 1000;
    CHECK(a.sameContent(b));
    b = st; b.layoutVersion++; CHECK(!a.sameContent(b));
    b = st; b.flags ^= kControlCrashRecorder; CHECK(!a.sameContent(b));
    b = st; b.maxProfiles = 3; CHECK(!a.sameContent(b));
    b = st; b.activeId = "profile-4"; CHECK(!a.sameContent(b));
    b = st; b.profiles[2].name = "x"; CHECK(!a.sameContent(b));
    b = st; b.profiles.pop_back(); CHECK(!a.sameContent(b));
    b = st; b.status = "x"; CHECK(!a.sameContent(b));
    b = st; b.recordStatus = "x"; CHECK(!a.sameContent(b));
    b = st; b.crashStatus = "x"; CHECK(!a.sameContent(b));
    b = st; b.powerOff = "x"; CHECK(!a.sameContent(b));

    // CONTROL_ACTION
    ControlAction ac;
    ac.pcId = idOf(0x31);
    ac.requestId = 0xDEADBEEF;
    ac.kind = ControlKind::ProfileRename;
    ac.flags = kControlConfirmed;
    ac.layoutVersion = 12345;
    ac.profileId = "profile-5";
    ac.text = "R\xC3\xA9named \xE2\x82\xAC";
    const Bytes acb = encodeControlAction(ac);
    ControlAction acd;
    CHECK(decodeControlAction(acb.data(), acb.size(), acd));
    CHECK(acd.pcId == ac.pcId && acd.requestId == ac.requestId && acd.kind == ControlKind::ProfileRename &&
          acd.flags == kControlConfirmed && acd.layoutVersion == 12345 && acd.profileId == "profile-5" &&
          acd.text == ac.text);
    CHECK(allPrefixesFail<ControlAction>(acb, [](const uint8_t* p, size_t n, ControlAction& o) {
        return decodeControlAction(p, n, o);
    }));
    Bytes acMore = acb;
    acMore.push_back(1); acMore.push_back(2); acMore.push_back(3);
    CHECK(decodeControlAction(acMore.data(), acMore.size(), acd) && acd.text == ac.text);
    ControlAction longAc = ac;
    longAc.profileId = euros;
    longAc.text = euros;
    const Bytes lb = encodeControlAction(longAc);
    CHECK(decodeControlAction(lb.data(), lb.size(), acd) && acd.profileId.size() == 15 && acd.text.size() == 30 &&
          validUtf8(acd.profileId) && validUtf8(acd.text));
    ControlAction unk = ac;
    unk.kind = static_cast<ControlKind>(200);
    const Bytes ub = encodeControlAction(unk);
    CHECK(decodeControlAction(ub.data(), ub.size(), acd) && static_cast<uint8_t>(acd.kind) == 200);
    ControlAction minimal;
    const Bytes mb = encodeControlAction(minimal);
    CHECK(decodeControlAction(mb.data(), mb.size(), acd) && acd.profileId.empty() && acd.text.empty());

    // CONTROL_RESULT
    ControlResult rs;
    rs.requestId = 77;
    rs.outcome = ControlOutcome::NeedsConfirm;
    rs.layoutVersion = 9;
    rs.message = "Lose the changes?";
    const Bytes rb = encodeControlResult(rs);
    ControlResult rd;
    CHECK(decodeControlResult(rb.data(), rb.size(), rd));
    CHECK(rd.requestId == 77 && rd.outcome == ControlOutcome::NeedsConfirm && rd.layoutVersion == 9 &&
          rd.message == rs.message);
    CHECK(allPrefixesFail<ControlResult>(rb, [](const uint8_t* p, size_t n, ControlResult& o) {
        return decodeControlResult(p, n, o);
    }));
    Bytes rMore = rb;
    rMore.push_back(0xFF);
    CHECK(decodeControlResult(rMore.data(), rMore.size(), rd) && rd.message == rs.message);
    for (uint8_t o = 0; o <= 5; ++o) {
        ControlResult each;
        each.outcome = static_cast<ControlOutcome>(o);
        const Bytes eachB = encodeControlResult(each);
        CHECK(decodeControlResult(eachB.data(), eachB.size(), rd) && static_cast<uint8_t>(rd.outcome) == o);
    }
    ControlResult weird = rs;
    weird.outcome = static_cast<ControlOutcome>(77);
    const Bytes wb = encodeControlResult(weird);
    CHECK(decodeControlResult(wb.data(), wb.size(), rd) && rd.outcome == ControlOutcome::Failed &&
          rd.message == rs.message && rd.requestId == 77);
    ControlResult longRs = rs;
    longRs.message = euros;
    const Bytes lrb = encodeControlResult(longRs);
    CHECK(decodeControlResult(lrb.data(), lrb.size(), rd) && rd.message.size() == 126 && validUtf8(rd.message));
    CHECK(std::strcmp(controlOutcomeName(ControlOutcome::Done), "done") == 0 &&
          std::strcmp(controlOutcomeName(ControlOutcome::Unsupported), "unsupported") == 0);

    // controlActionConfirmed truth table
    auto act = [](ControlKind k, bool confirmed, uint32_t ver) {
        ControlAction x;
        x.kind = k;
        x.flags = confirmed ? kControlConfirmed : 0;
        x.layoutVersion = ver;
        return x;
    };
    for (bool unsaved : {false, true}) {
        CHECK(!controlActionConfirmed(act(ControlKind::ProfileDelete, false, 5), unsaved, 5));
        CHECK(controlActionConfirmed(act(ControlKind::ProfileDelete, true, 5), unsaved, 5));
        CHECK(controlActionConfirmed(act(ControlKind::ProfileDelete, true, 1), unsaved, 5));  // version irrelevant
        for (ControlKind k : {ControlKind::ProfileNew, ControlKind::ProfileSave, ControlKind::ProfileRename,
                              ControlKind::RecordOn, ControlKind::RecordOff, ControlKind::CrashRecorderOn,
                              ControlKind::CrashRecorderOff, static_cast<ControlKind>(200)}) {
            CHECK(controlActionConfirmed(act(k, false, 0), unsaved, 5));
            CHECK(controlActionConfirmed(act(k, true, 3), unsaved, 5));
        }
    }
    for (ControlKind k : {ControlKind::ProfileUse, ControlKind::ProfileReset}) {
        CHECK(controlActionConfirmed(act(k, false, 0), false, 5));  // nothing unsaved: one tap
        CHECK(controlActionConfirmed(act(k, true, 99), false, 5));
        CHECK(!controlActionConfirmed(act(k, false, 5), true, 5));  // unsaved, not confirmed
        CHECK(controlActionConfirmed(act(k, true, 5), true, 5));    // unsaved, confirmed, same version
        CHECK(!controlActionConfirmed(act(k, true, 4), true, 5));   // an edit since: ask again
        CHECK(!controlActionConfirmed(act(k, true, 6), true, 5));
    }
}

// ---- helpers ----

void testHelpers() {
    // cutUtf8
    CHECK(cutUtf8("hello", 10) == "hello" && cutUtf8("hello", 3) == "hel" && cutUtf8("hello", 0).empty());
    CHECK(cutUtf8("a\xC3\xA9", 2) == "a");                 // 'é' is 2 bytes
    CHECK(cutUtf8("a\xC3\xA9", 3) == "a\xC3\xA9");
    CHECK(cutUtf8("\xE2\x82\xAC\xE2\x82\xAC", 4) == "\xE2\x82\xAC");  // '€' is 3 bytes
    CHECK(cutUtf8("\xE2\x82\xAC", 1).empty() && cutUtf8("\xE2\x82\xAC", 2).empty());
    CHECK(cutUtf8("x\xF0\x9F\x98\x80", 4) == "x");         // an emoji is 4 bytes
    CHECK(cutUtf8("x\xF0\x9F\x98\x80", 5) == "x\xF0\x9F\x98\x80");
    const std::string mixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z\xC3\xA9";
    for (size_t n = 0; n <= mixed.size() + 2; ++n) {
        const std::string cut = cutUtf8(mixed, n);
        CHECK(cut.size() <= n && validUtf8(cut) && mixed.compare(0, cut.size(), cut) == 0);
    }

    // ipv4
    uint32_t ip = 0;
    CHECK(ipv4FromText("192.168.1.45", ip) && ip == 0xC0A8012Du);
    CHECK(ipv4FromText("0.0.0.0", ip) && ip == 0);
    CHECK(ipv4FromText("255.255.255.255", ip) && ip == 0xFFFFFFFFu);
    CHECK(ipv4ToText(0xC0A8012Du) == "192.168.1.45" && ipv4ToText(0) == "0.0.0.0");
    ip = 99;
    for (const char* bad : {"1.2.3", "256.1.1.1", "1.2.3.4.5", "01234.1.1.1", "", "1.2.3.", ".1.2.3", "1..2.3",
                            "a.b.c.d", " 1.2.3.4", "1.2.3.4 ", "1.2.3.4:80", "1.2.3.256", "-1.2.3.4"}) {
        CHECK(!ipv4FromText(bad, ip));
    }
    CHECK(ip == 99);  // failures leave the output alone

    // ids
    const Id128 id = idOf(0xA0);
    const std::string hex = idToHex(id);
    CHECK(hex.size() == 32 && hex.find_first_not_of("0123456789abcdef") == std::string::npos);
    Id128 back {};
    CHECK(idFromHex(hex, back) && back == id);
    std::string upper = hex;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(idFromHex(upper, back) && back == id);
    CHECK(!idFromHex(hex.substr(1), back) && !idFromHex(hex + "0", back) && !idFromHex("", back));
    std::string notHex = hex;
    notHex[5] = 'g';
    CHECK(!idFromHex(notHex, back));
    CHECK(randomId() != randomId());
}

}  // namespace

int main() {
    testHeader();
    testMessages();
    testValues();
    testToWire();
    testMetricsSplit();
    testBlobs();
    testForgetPeer();
    testControl();
    testHelpers();
    if (failures == 0) std::printf("core-link-wire-test: all passed\n");
    else std::printf("core-link-wire-test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
