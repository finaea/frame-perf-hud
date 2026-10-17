#include "link_wire.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace fph {
namespace link {

namespace {

constexpr uint8_t kMagic[3] = {'F', 'P', 'H'};
constexpr size_t kMaxAddresses = 8;
constexpr size_t kMaxProcessRows = 255;
constexpr uint8_t kProcessListFormat = 1;

/** @return payload when the reader consumed it exactly, for the decode* one-liners */
template <typename Fn>
bool readAll(const uint8_t* data, size_t size, Fn fn) {
    if (data == nullptr && size > 0) return false;
    Reader r(data, size);
    fn(r);
    return r.done();
}

}  // namespace

const char* packetTypeName(PacketType type) {
    switch (type) {
        case PacketType::Hello: return "HELLO";
        case PacketType::HelloAck: return "HELLO_ACK";
        case PacketType::Subscribe: return "SUBSCRIBE";
        case PacketType::SubscribeAck: return "SUBSCRIBE_ACK";
        case PacketType::Metrics: return "METRICS";
        case PacketType::PcMetrics: return "PC_METRICS";
        case PacketType::Blob: return "BLOB";
        case PacketType::LayoutGet: return "LAYOUT_GET";
        case PacketType::LayoutAck: return "LAYOUT_ACK";
        case PacketType::LayoutReject: return "LAYOUT_REJECT";
        case PacketType::Bye: return "BYE";
        case PacketType::ControlState: return "CONTROL_STATE";    // R3
        case PacketType::ControlAction: return "CONTROL_ACTION";
        case PacketType::ControlResult: return "CONTROL_RESULT";
    }
    return "?";
}

const char* peerStatusName(PeerStatus status) {
    switch (status) {
        case PeerStatus::Allowed: return "allowed";
        case PeerStatus::Pending: return "pending";
        case PeerStatus::Denied: return "denied";
        case PeerStatus::UnknownSubscriber: return "unknown_subscriber";
    }
    return "?";
}

const char* rejectReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::Stale: return "stale";
        case RejectReason::NotAllowed: return "not_allowed";
        case RejectReason::InvalidLayout: return "invalid_layout";
        case RejectReason::SaveFailed: return "save_failed";
        case RejectReason::UnknownSubscriber: return "unknown_subscriber";
    }
    return "?";
}

std::string idToHex(const Id128& id) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : id) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

bool idFromHex(const std::string& text, Id128& out) {
    if (text.size() != 32) return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Id128 id {};
    for (size_t i = 0; i < 16; ++i) {
        const int hi = nibble(text[2 * i]);
        const int lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        id[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    out = id;
    return true;
}

Id128 randomId() {
    std::random_device rd;
    Id128 id {};
    for (size_t i = 0; i < id.size(); i += 4) {
        const uint32_t v = rd();
        for (size_t k = 0; k < 4; ++k) id[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    return id;
}

std::string cutUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t end = maxBytes;
    // Back up over continuation bytes (10xxxxxx) so the cut lands on a sequence start
    while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

std::string ipv4ToText(uint32_t ip) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255,
                  ip & 255);
    return buffer;
}

bool ipv4FromText(const std::string& text, uint32_t& out) {
    uint32_t ip = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        uint32_t value = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + static_cast<uint32_t>(text[i] - '0');
            if (++digits > 3) return false;
            ++i;
        }
        if (value > 255) return false;
        ip = ip << 8 | value;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = ip;
    return true;
}

// ---- header ----

DecodeError decodePacket(const uint8_t* data, size_t size, Header& header, const uint8_t*& payload) {
    payload = nullptr;
    if (data == nullptr || size < kHeaderSize) return DecodeError::TooShort;
    if (std::memcmp(data, kMagic, 3) != 0) return DecodeError::BadMagic;
    Reader r(data + 3, kHeaderSize - 3);
    header.version = r.u8();
    header.type = static_cast<PacketType>(r.u8());
    header.flags = r.u8();
    header.seq = r.u32();
    header.length = r.u16();
    if (header.version != kProtocolVersion) return DecodeError::BadVersion;
    if (header.flags & kFlagEncrypted) return DecodeError::Encrypted;
    if (header.length > kMaxPayload) return DecodeError::TooLong;
    if (size < kHeaderSize + header.length) return DecodeError::TooShort;
    if (size > kHeaderSize + header.length) return DecodeError::TooLong;
    payload = data + kHeaderSize;
    return DecodeError::Ok;
}

std::vector<uint8_t> encodePacket(PacketType type, uint32_t seq, const std::vector<uint8_t>& payload) {
    if (payload.size() > kMaxPayload) return {};
    Writer w;
    w.raw(kMagic, 3);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(type));
    w.u8(0);
    w.u32(seq);
    w.u16(static_cast<uint16_t>(payload.size()));
    w.raw(payload.data(), payload.size());
    return std::move(w.bytes());
}

// ---- Writer / Reader ----

void Writer::u16(uint16_t v) {
    u8(static_cast<uint8_t>(v));
    u8(static_cast<uint8_t>(v >> 8));
}

void Writer::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
}

void Writer::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
}

void Writer::f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
}

void Writer::str8(const std::string& v, size_t maxBytes) {
    const std::string cut = cutUtf8(v, std::min<size_t>(maxBytes, 255));
    u8(static_cast<uint8_t>(cut.size()));
    raw(reinterpret_cast<const uint8_t*>(cut.data()), cut.size());
}

const uint8_t* Reader::take(size_t n) {
    if (failed_ || size_ - pos_ < n) {
        failed_ = true;
        return nullptr;
    }
    const uint8_t* p = data_ + pos_;
    pos_ += n;
    return p;
}

uint8_t Reader::u8() {
    const uint8_t* p = take(1);
    return p != nullptr ? p[0] : 0;
}

uint16_t Reader::u16() {
    const uint8_t* p = take(2);
    return p != nullptr ? static_cast<uint16_t>(p[0] | p[1] << 8) : 0;
}

uint32_t Reader::u32() {
    const uint8_t* p = take(4);
    if (p == nullptr) return 0;
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

uint64_t Reader::u64() {
    const uint8_t* p = take(8);
    if (p == nullptr) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

float Reader::f32() {
    const uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

Id128 Reader::id() {
    Id128 v {};
    const uint8_t* p = take(v.size());
    if (p != nullptr) std::memcpy(v.data(), p, v.size());
    return v;
}

std::string Reader::str8() {
    const uint8_t n = u8();
    const uint8_t* p = take(n);
    return p != nullptr ? std::string(reinterpret_cast<const char*>(p), n) : std::string();
}

// ---- typed values ----

bool WireValue::operator==(const WireValue& o) const {
    if (id != o.id || type != o.type || ok != o.ok) return false;
    if (!ok) return reason == o.reason;
    switch (type) {
        case WireType::F32: return std::memcmp(&f32, &o.f32, sizeof(f32)) == 0;
        case WireType::I32: return i32 == o.i32;
        case WireType::Enum: return enumValue == o.enumValue;
        case WireType::ShortString: return text == o.text;
    }
    return false;
}

WireValue toWire(uint16_t id, const MetricValue& value) {
    WireValue w;
    w.id = id;
    w.type = value.type;
    w.ok = value.ok;
    w.reason = value.ok ? Reason::None : value.reason;
    w.f32 = value.f32;
    w.i32 = value.i32;
    w.enumValue = static_cast<uint8_t>(std::min<uint32_t>(value.enumValue, 255));
    w.text = cutUtf8(value.text, kShortStringMax);
    return w;
}

bool storeWire(MetricStore& store, const WireValue& value, double now) {
    const MetricDef* def = findMetric(value.id);
    if (def == nullptr || def->type != value.type) return false;
    if (!value.ok) return store.setNa(value.id, value.reason, now);
    switch (value.type) {
        case WireType::F32: return store.setF32(value.id, value.f32, now);
        case WireType::I32: return store.setI32(value.id, value.i32, now);
        case WireType::Enum: return store.setEnum(value.id, value.enumValue, now);
        case WireType::ShortString: return store.setText(value.id, value.text, now);
    }
    return false;
}

namespace {

void writeValue(Writer& w, const WireValue& v) {
    w.u16(v.id);
    w.u8(static_cast<uint8_t>(v.type));
    w.u8(v.ok ? 0 : 1);
    if (!v.ok) {
        w.u16(static_cast<uint16_t>(v.reason));
        return;
    }
    switch (v.type) {
        case WireType::F32: w.f32(v.f32); break;
        case WireType::I32: w.i32(v.i32); break;
        case WireType::Enum: w.u8(v.enumValue); break;
        case WireType::ShortString: w.str8(v.text, kShortStringMax); break;
    }
}

bool readValue(Reader& r, WireValue& v) {
    v.id = r.u16();
    const uint8_t type = r.u8();
    const uint8_t status = r.u8();
    if (r.failed() || type > static_cast<uint8_t>(WireType::ShortString) || status > 1) return false;
    v.type = static_cast<WireType>(type);
    v.ok = status == 0;
    if (!v.ok) {
        v.reason = static_cast<Reason>(r.u16());
        return !r.failed();
    }
    v.reason = Reason::None;
    switch (v.type) {
        case WireType::F32: v.f32 = r.f32(); break;
        case WireType::I32: v.i32 = r.i32(); break;
        case WireType::Enum: v.enumValue = r.u8(); break;
        case WireType::ShortString:
            v.text = r.str8();
            if (v.text.size() > kShortStringMax) return false;
            break;
    }
    return !r.failed();
}

void writeLinkState(Writer& w, const LinkState& s) {
    w.u32(s.layoutVersion);
    w.i32(s.sessionId);
    w.i64(s.sessionStartUnix);
    w.str8(s.appKey, kMaxNameBytes);
    w.str8(s.appName, kMaxNameBytes);
}

void readLinkState(Reader& r, LinkState& s) {
    s.layoutVersion = r.u32();
    s.sessionId = r.i32();
    s.sessionStartUnix = r.i64();
    s.appKey = r.str8();
    s.appName = r.str8();
}

/** Metrics part header: ms u64, snapshot u32, part u8, count u8, LINK_STATE, value count u16. */
Writer metricsPartHeader(uint64_t headsetMs, uint32_t snapshotId, const LinkState& state) {
    Writer w;
    w.u64(headsetMs);
    w.u32(snapshotId);
    w.u8(0);  // part, patched below
    w.u8(1);  // part count, patched below
    writeLinkState(w, state);
    w.u16(0);  // value count, patched below
    return w;
}

constexpr size_t kPartIndexOffset = 12;
constexpr size_t kPartCountOffset = 13;

}  // namespace

bool readWireValue(Reader& reader, WireValue& value) {
    return readValue(reader, value);
}

std::vector<std::vector<uint8_t>> encodeMetrics(uint64_t headsetMs, uint32_t snapshotId, const LinkState& state,
                                                const std::vector<WireValue>& values) {
    std::vector<std::vector<uint8_t>> parts;
    Writer part = metricsPartHeader(headsetMs, snapshotId, state);
    const size_t headerSize = part.size();
    uint16_t count = 0;
    const auto finish = [&]() {
        std::vector<uint8_t>& bytes = part.bytes();
        bytes[headerSize - 2] = static_cast<uint8_t>(count);
        bytes[headerSize - 1] = static_cast<uint8_t>(count >> 8);
        parts.push_back(std::move(bytes));
        part = metricsPartHeader(headsetMs, snapshotId, state);
        count = 0;
    };
    for (const WireValue& v : values) {
        Writer one;
        writeValue(one, v);
        if (part.size() + one.size() > kMaxPayload && count > 0) finish();
        part.raw(one.bytes().data(), one.size());
        ++count;
    }
    if (count > 0 || parts.empty()) finish();
    const size_t n = std::min<size_t>(parts.size(), 255);
    parts.resize(n);
    for (size_t i = 0; i < n; ++i) {
        parts[i][kPartIndexOffset] = static_cast<uint8_t>(i);
        parts[i][kPartCountOffset] = static_cast<uint8_t>(n);
    }
    return parts;
}

bool decodeMetrics(const uint8_t* payload, size_t size, Metrics& out) {
    Metrics m;
    Reader r(payload, size);
    m.headsetMs = r.u64();
    m.snapshotId = r.u32();
    m.part = r.u8();
    m.partCount = r.u8();
    readLinkState(r, m.state);
    const uint16_t count = r.u16();
    if (r.failed() || m.partCount == 0 || m.part >= m.partCount) return false;
    m.values.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        WireValue v;
        if (!readValue(r, v)) return false;
        m.values.push_back(std::move(v));
    }
    if (!r.done()) return false;
    out = std::move(m);
    return true;
}

// ---- blobs ----

std::vector<std::vector<uint8_t>> splitBlob(uint32_t blobId, BlobKind kind, uint8_t stream,
                                            const std::vector<uint8_t>& data) {
    const size_t count = std::max<size_t>(1, (data.size() + kBlobChunkData - 1) / kBlobChunkData);
    if (count > kMaxBlobChunks) return {};
    std::vector<std::vector<uint8_t>> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = i * kBlobChunkData;
        const size_t end = std::min(data.size(), begin + kBlobChunkData);
        Writer w;
        w.u32(blobId);
        w.u8(static_cast<uint8_t>(kind));
        w.u8(stream);
        w.u16(static_cast<uint16_t>(i));
        w.u16(static_cast<uint16_t>(count));
        w.u32(static_cast<uint32_t>(data.size()));
        if (end > begin) w.raw(data.data() + begin, end - begin);
        out.push_back(std::move(w.bytes()));
    }
    return out;
}

bool decodeBlobChunk(const uint8_t* payload, size_t size, BlobChunk& out) {
    Reader r(payload, size);
    BlobChunk c;
    c.blobId = r.u32();
    c.kind = static_cast<BlobKind>(r.u8());
    c.stream = r.u8();
    c.index = r.u16();
    c.count = r.u16();
    c.totalSize = r.u32();
    if (r.failed() || c.count == 0 || c.count > kMaxBlobChunks || c.index >= c.count) return false;
    // The shape must be exactly what splitBlob makes: full chunks, then the remainder in the last one
    const size_t expectedCount = std::max<size_t>(1, (c.totalSize + kBlobChunkData - 1) / kBlobChunkData);
    if (expectedCount != c.count) return false;
    const size_t begin = static_cast<size_t>(c.index) * kBlobChunkData;
    const size_t expectedSize = std::min<size_t>(kBlobChunkData, c.totalSize - std::min<size_t>(begin, c.totalSize));
    if (r.left() != expectedSize) return false;
    c.size = expectedSize;
    c.data = r.take(expectedSize);
    if (expectedSize > 0 && c.data == nullptr) return false;
    out = c;
    return true;
}

bool BlobAssembler::add(uint64_t peer, const BlobChunk& chunk, double now, Blob& out) {
    Partial& p = partials_[Key(peer, static_cast<uint8_t>(chunk.kind), chunk.stream)];
    // lastActivity moves only on accepted chunks, so a sender whose ids restarted (stuck below lastCompleted)
    // is forgotten after kForgetIdleSec even if it keeps sending
    // Older than (or the same as) a blob already completed here: a late duplicate
    if (p.haveCompleted && !newerId(chunk.blobId, p.lastCompleted)) {
        ++dropped_;
        return false;
    }
    if (p.active && p.blobId != chunk.blobId) {
        if (!newerId(chunk.blobId, p.blobId)) {  // a straggler from an older blob
            ++dropped_;
            return false;
        }
        p.active = false;  // a newer blob of the same kind + stream replaces the incomplete one
        ++dropped_;
    }
    if (p.active && (p.count != chunk.count || p.totalSize != chunk.totalSize)) {
        ++dropped_;  // same id, different shape: corrupt, ignore the chunk
        return false;
    }
    p.lastActivity = now;
    if (!p.active) {
        p.active = true;
        p.blobId = chunk.blobId;
        p.count = chunk.count;
        p.received = 0;
        p.totalSize = chunk.totalSize;
        p.started = now;
        p.data.assign(chunk.totalSize, 0);
        p.have.assign(chunk.count, false);
    }
    if (p.have[chunk.index]) return false;  // duplicate chunk
    p.have[chunk.index] = true;
    ++p.received;
    if (chunk.size > 0) {
        std::memcpy(p.data.data() + static_cast<size_t>(chunk.index) * kBlobChunkData, chunk.data, chunk.size);
    }
    if (p.received < p.count) return false;
    out.peer = peer;
    out.blobId = p.blobId;
    out.kind = chunk.kind;
    out.stream = chunk.stream;
    out.data = std::move(p.data);
    p.active = false;
    p.data.clear();
    p.have.clear();
    p.haveCompleted = true;
    p.lastCompleted = out.blobId;
    return true;
}

void BlobAssembler::expire(double now) {
    constexpr double kForgetIdleSec = 10.0;  // a key's "last completed" memory (late duplicates)
    for (auto it = partials_.begin(); it != partials_.end();) {
        Partial& p = it->second;
        if (p.active && now - p.started > kBlobExpirySec) {
            p.active = false;
            p.data.clear();
            p.have.clear();
            ++dropped_;
        }
        if (!p.active && now - p.lastActivity > kForgetIdleSec) {
            it = partials_.erase(it);
        } else {
            ++it;
        }
    }
}

void BlobAssembler::forgetPeer(uint64_t peer) {
    for (auto it = partials_.begin(); it != partials_.end();) {
        if (std::get<0>(it->first) == peer) {
            it = partials_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t BlobAssembler::inProgress() const {
    size_t n = 0;
    for (const auto& kv : partials_) n += kv.second.active ? 1 : 0;
    return n;
}

// ---- messages ----

std::vector<uint8_t> encodeHello(const Hello& m) {
    Writer w;
    w.id(m.pcId);
    w.str8(m.pcName, kMaxNameBytes);
    return std::move(w.bytes());
}

bool decodeHello(const uint8_t* payload, size_t size, Hello& out) {
    Hello m;
    if (!readAll(payload, size, [&](Reader& r) {
            m.pcId = r.id();
            m.pcName = r.str8();
        })) {
        return false;
    }
    out = m;
    return true;
}

std::vector<uint8_t> encodeHelloAck(const HelloAck& m) {
    Writer w;
    w.id(m.headsetId);
    w.str8(m.name, kMaxNameBytes);
    w.u8(m.protocol);
    w.u8(static_cast<uint8_t>(m.status));
    const size_t n = std::min(m.addresses.size(), kMaxAddresses);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.str8(m.addresses[i].ifName, 15);
        w.u32(m.addresses[i].ip);
    }
    w.u32(m.layoutVersion);
    w.u32(m.seenIp);
    w.u16(m.seenPort);
    return std::move(w.bytes());
}

bool decodeHelloAck(const uint8_t* payload, size_t size, HelloAck& out) {
    HelloAck m;
    bool ok = readAll(payload, size, [&](Reader& r) {
        m.headsetId = r.id();
        m.name = r.str8();
        m.protocol = r.u8();
        const uint8_t status = r.u8();
        m.status = static_cast<PeerStatus>(status);
        if (status > static_cast<uint8_t>(PeerStatus::UnknownSubscriber)) r.take(size + 1);  // fail
        const uint8_t n = r.u8();
        if (n > kMaxAddresses) r.take(size + 1);
        for (uint8_t i = 0; i < n && !r.failed(); ++i) {
            LocalAddress a;
            a.ifName = r.str8();
            a.ip = r.u32();
            m.addresses.push_back(a);
        }
        m.layoutVersion = r.u32();
        m.seenIp = r.u32();
        m.seenPort = r.u16();
    });
    if (!ok) return false;
    out = m;
    return true;
}

std::vector<uint8_t> encodeSubscribe(const Subscribe& m) {
    Writer w;
    w.id(m.pcId);
    w.u32(m.wants);
    if (m.wants & (0x3Fu << 8)) w.u8(m.cameraFps);  // T10
    return std::move(w.bytes());
}

bool decodeSubscribe(const uint8_t* payload, size_t size, Subscribe& out) {
    Subscribe m;
    if (!readAll(payload, size, [&](Reader& r) {
            m.pcId = r.id();
            m.wants = r.u32();
            if (m.wants & (0x3Fu << 8)) m.cameraFps = r.u8();  // T10: there exactly when cameras are wanted
        })) {
        return false;
    }
    out = m;
    return true;
}

std::vector<uint8_t> encodeSubscribeAck(const SubscribeAck& m) {
    Writer w;
    w.u8(static_cast<uint8_t>(m.status));
    w.u16(m.leaseMs);
    return std::move(w.bytes());
}

bool decodeSubscribeAck(const uint8_t* payload, size_t size, SubscribeAck& out) {
    SubscribeAck m;
    uint8_t status = 0;
    if (!readAll(payload, size, [&](Reader& r) {
            status = r.u8();
            m.leaseMs = r.u16();
        })) {
        return false;
    }
    if (status > static_cast<uint8_t>(PeerStatus::UnknownSubscriber)) return false;
    m.status = static_cast<PeerStatus>(status);
    out = m;
    return true;
}

std::vector<uint8_t> encodeLayoutGet(const LayoutGet& m) {
    Writer w;
    w.id(m.pcId);
    return std::move(w.bytes());
}

bool decodeLayoutGet(const uint8_t* payload, size_t size, LayoutGet& out) {
    LayoutGet m;
    if (!readAll(payload, size, [&](Reader& r) { m.pcId = r.id(); })) return false;
    out = m;
    return true;
}

std::vector<uint8_t> encodeLayoutSet(const LayoutSet& m) {
    Writer w;
    w.id(m.pcId);
    w.u32(m.requestId);
    w.u32(m.baseVersion);
    w.raw(reinterpret_cast<const uint8_t*>(m.layoutJson.data()), m.layoutJson.size());
    return std::move(w.bytes());
}

bool decodeLayoutSet(const uint8_t* data, size_t size, LayoutSet& out) {
    Reader r(data, size);
    LayoutSet m;
    m.pcId = r.id();
    m.requestId = r.u32();
    m.baseVersion = r.u32();
    if (r.failed()) return false;
    const size_t n = r.left();
    const uint8_t* json = r.take(n);
    m.layoutJson.assign(reinterpret_cast<const char*>(json), n);
    out = std::move(m);
    return true;
}

std::vector<uint8_t> encodeLayoutAck(const LayoutAck& m) {
    Writer w;
    w.u32(m.requestId);
    w.u32(m.version);
    return std::move(w.bytes());
}

bool decodeLayoutAck(const uint8_t* payload, size_t size, LayoutAck& out) {
    LayoutAck m;
    if (!readAll(payload, size, [&](Reader& r) {
            m.requestId = r.u32();
            m.version = r.u32();
        })) {
        return false;
    }
    out = m;
    return true;
}

// ---- R3: CONTROL_* ----

bool ControlState::sameContent(const ControlState& o) const {
    return layoutVersion == o.layoutVersion && flags == o.flags && maxProfiles == o.maxProfiles &&
           activeId == o.activeId && profiles == o.profiles && status == o.status && recordStatus == o.recordStatus &&
           crashStatus == o.crashStatus && powerOff == o.powerOff;
}

const char* controlOutcomeName(ControlOutcome outcome) {
    switch (outcome) {
        case ControlOutcome::Done: return "done";
        case ControlOutcome::NeedsConfirm: return "needs_confirm";
        case ControlOutcome::Failed: return "failed";
        case ControlOutcome::NotAllowed: return "not_allowed";
        case ControlOutcome::UnknownSubscriber: return "unknown_subscriber";
        case ControlOutcome::Unsupported: return "unsupported";
    }
    return "?";
}

bool controlActionConfirmed(const ControlAction& action, bool unsaved, uint32_t currentVersion) {
    const bool confirmed = (action.flags & kControlConfirmed) != 0;
    switch (action.kind) {
        case ControlKind::ProfileDelete: return confirmed;
        case ControlKind::ProfileUse:
        case ControlKind::ProfileReset: return !unsaved || (confirmed && action.layoutVersion == currentVersion);
        default: return true;
    }
}

std::vector<uint8_t> encodeControlState(const ControlState& m) {
    Writer w;
    w.u32(m.revision);
    w.u32(m.layoutVersion);
    w.u8(m.flags);
    w.u8(m.maxProfiles);
    w.str8(m.activeId, 15);
    const size_t count = std::min(m.profiles.size(), kMaxControlProfiles);
    w.u8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        w.str8(m.profiles[i].id, 15);
        w.str8(m.profiles[i].name, 31);
    }
    w.str8(m.status, 127);
    w.str8(m.recordStatus, 127);
    w.str8(m.crashStatus, 63);
    w.str8(m.powerOff, 31);
    return std::move(w.bytes());
}

bool decodeControlState(const uint8_t* payload, size_t size, ControlState& out) {
    Reader r(payload, size);
    ControlState m;
    m.revision = r.u32();
    m.layoutVersion = r.u32();
    m.flags = r.u8();
    m.maxProfiles = r.u8();
    m.activeId = r.str8();
    const uint8_t count = r.u8();
    if (r.failed() || count > kMaxControlProfiles) return false;
    for (uint8_t i = 0; i < count; ++i) {
        ControlProfile p;
        p.id = r.str8();
        p.name = r.str8();
        m.profiles.push_back(p);
    }
    m.status = r.str8();
    m.recordStatus = r.str8();
    m.crashStatus = r.str8();
    m.powerOff = r.str8();
    if (r.failed()) return false;  // trailing bytes = a newer headset's fields: fine
    out = m;
    return true;
}

std::vector<uint8_t> encodeControlAction(const ControlAction& m) {
    Writer w;
    w.id(m.pcId);
    w.u32(m.requestId);
    w.u8(static_cast<uint8_t>(m.kind));
    w.u8(m.flags);
    w.u32(m.layoutVersion);
    w.str8(m.profileId, 15);
    w.str8(m.text, 31);
    return std::move(w.bytes());
}

bool decodeControlAction(const uint8_t* payload, size_t size, ControlAction& out) {
    Reader r(payload, size);
    ControlAction m;
    m.pcId = r.id();
    m.requestId = r.u32();
    m.kind = static_cast<ControlKind>(r.u8());
    m.flags = r.u8();
    m.layoutVersion = r.u32();
    m.profileId = r.str8();
    m.text = r.str8();
    if (r.failed()) return false;
    out = m;
    return true;
}

std::vector<uint8_t> encodeControlResult(const ControlResult& m) {
    Writer w;
    w.u32(m.requestId);
    w.u8(static_cast<uint8_t>(m.outcome));
    w.u32(m.layoutVersion);
    w.str8(m.message, 127);
    return std::move(w.bytes());
}

bool decodeControlResult(const uint8_t* payload, size_t size, ControlResult& out) {
    Reader r(payload, size);
    ControlResult m;
    m.requestId = r.u32();
    const uint8_t outcome = r.u8();
    m.layoutVersion = r.u32();
    m.message = r.str8();
    if (r.failed()) return false;
    // a newer headset's outcome: read it as a failure with its message rather than dropping the answer
    m.outcome = outcome <= static_cast<uint8_t>(ControlOutcome::Unsupported) ? static_cast<ControlOutcome>(outcome)
                                                                             : ControlOutcome::Failed;
    out = m;
    return true;
}

std::vector<uint8_t> encodeLayoutReject(const LayoutReject& m) {
    Writer w;
    w.u32(m.requestId);
    w.u32(m.currentVersion);
    w.u8(static_cast<uint8_t>(m.reason));
    return std::move(w.bytes());
}

bool decodeLayoutReject(const uint8_t* payload, size_t size, LayoutReject& out) {
    LayoutReject m;
    uint8_t reason = 0;
    if (!readAll(payload, size, [&](Reader& r) {
            m.requestId = r.u32();
            m.currentVersion = r.u32();
            reason = r.u8();
        })) {
        return false;
    }
    if (reason > static_cast<uint8_t>(RejectReason::UnknownSubscriber)) return false;
    m.reason = static_cast<RejectReason>(reason);
    out = m;
    return true;
}

std::vector<uint8_t> encodeBye(const Bye& m) {
    Writer w;
    w.id(m.id);
    return std::move(w.bytes());
}

bool decodeBye(const uint8_t* payload, size_t size, Bye& out) {
    Bye m;
    if (!readAll(payload, size, [&](Reader& r) { m.id = r.id(); })) return false;
    out = m;
    return true;
}

std::vector<uint8_t> encodeProcessList(const ProcessListWire& m) {
    Writer w;
    w.u8(kProcessListFormat);
    w.u64(m.generation);
    w.u8(m.cpuValid ? 1 : 0);
    const size_t n = std::min(m.rows.size(), kMaxProcessRows);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const ProcessRowWire& row = m.rows[i];
        w.str8(row.name, kMaxNameBytes);
        w.i32(row.pid);
        w.u16(row.count);
        w.f32(row.cpuCorePct);
        w.f32(row.rssMiB);
    }
    return std::move(w.bytes());
}

bool decodeProcessList(const uint8_t* data, size_t size, ProcessListWire& out) {
    ProcessListWire m;
    bool formatOk = true;
    if (!readAll(data, size, [&](Reader& r) {
            formatOk = r.u8() == kProcessListFormat;
            m.generation = r.u64();
            m.cpuValid = r.u8() != 0;
            const uint8_t n = r.u8();
            for (uint8_t i = 0; i < n && !r.failed() && formatOk; ++i) {
                ProcessRowWire row;
                row.name = r.str8();
                row.pid = r.i32();
                row.count = r.u16();
                row.cpuCorePct = r.f32();
                row.rssMiB = r.f32();
                m.rows.push_back(std::move(row));
            }
        })) {
        return false;
    }
    if (!formatOk) return false;
    out = std::move(m);
    return true;
}

}  // namespace link
}  // namespace fph
