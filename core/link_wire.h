// The headset <-> PC link's wire format (plain UDP). Shared by the headset agent and the PC app, so both
// sides encode and decode with the same code. Platform-free: no sockets here, only bytes.
//
// Every datagram: a 12-byte header + a payload of at most 1200 bytes (so nothing fragments at the IP layer).
//   magic "FPH" | protocol version u8 | type u8 | flags u8 | seq u32 | payload length u16
// Little-endian throughout. Flags bit 0 is reserved for "encrypted" (v1 never sets it and drops packets
// that do). See the tech plan's link protocol page for the conversation rules (discovery, lease, layout sync).
#pragma once

#include "metric_registry.h"
#include "metric_store.h"
#include "reason.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace fph {
namespace link {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 47630;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxPayload = 1200;
constexpr size_t kMaxPacket = kHeaderSize + kMaxPayload;
constexpr uint8_t kFlagEncrypted = 0x01;  ///< reserved: v1 never sets it

// Timing rules both sides follow
constexpr double kHelloRepeatSec = 2.0;      ///< PC: HELLO every 2 s until answered (and while pending)
constexpr double kSubscribeEverySec = 2.0;   ///< PC: lease renewal
constexpr double kLeaseSec = 6.0;            ///< headset: a lease not renewed for this long expires
constexpr double kLayoutResendSec = 1.0;     ///< PC: resend LAYOUT_SET / LAYOUT_GET until answered
constexpr double kBlobExpirySec = 1.0;       ///< an incomplete blob is dropped this long after its first chunk
constexpr size_t kLayoutAnswerMemory = 8;    ///< headset: LAYOUT_SET answers remembered per PC

/** Packet types. Stable: append only. */
enum class PacketType : uint8_t {
    Hello = 1,         ///< PC -> headset
    HelloAck = 2,      ///< headset -> PC
    Subscribe = 3,     ///< PC -> headset: lease renewal + wants
    SubscribeAck = 4,  ///< headset -> PC: lease status (ok / pending / denied / unknown_subscriber)
    Metrics = 5,       ///< headset -> PC: full snapshot (possibly split), LINK_STATE in every part
    PcMetrics = 6,     ///< PC -> headset (ticket 08)
    Blob = 7,          ///< both: one chunk of a large item (process list, layout, LAYOUT_SET, camera frame)
    LayoutGet = 8,     ///< PC -> headset: answered with a Blob of kind Layout
    LayoutAck = 9,     ///< headset -> PC
    LayoutReject = 10, ///< headset -> PC
    Bye = 11,          ///< both: clean exit
    // R3: the PC window's Profiles / Logs tabs. An older peer ignores these: the PC's default branch drops unknown
    // types silently; an older headset counts a CONTROL_ACTION as malformed, which only happens on a button press
    ControlState = 12,   ///< headset -> PC: profiles, Record, crash recorder, last power-off (on change + every 2 s)
    ControlAction = 13,  ///< PC -> headset: a Profiles / Logs button (resent every 1 s until answered)
    ControlResult = 14,  ///< headset -> PC: the action's outcome
};

/** @return the type's name for logs ("HELLO", ...); "?" if unknown */
const char* packetTypeName(PacketType type);

/** Peer status in HELLO_ACK / SUBSCRIBE_ACK. */
enum class PeerStatus : uint8_t {
    Allowed = 0,
    Pending = 1,            ///< waiting for Allow on the headset's dashboard tab
    Denied = 2,
    UnknownSubscriber = 3,  ///< this headset process never got a HELLO from that PC (e.g. after a SteamVR restart): HELLO again
};

/** @return "allowed" / "pending" / "denied" / "unknown_subscriber" */
const char* peerStatusName(PeerStatus status);

/** Why a LAYOUT_SET was rejected. */
enum class RejectReason : uint8_t {
    Stale = 0,              ///< base_version isn't current: reload (LAYOUT_GET) and redo the edit
    NotAllowed = 1,         ///< the PC isn't allowed (pending / denied)
    InvalidLayout = 2,      ///< the layout JSON couldn't be read
    SaveFailed = 3,         ///< the headset couldn't save its config; nothing changed
    UnknownSubscriber = 4,  ///< HELLO first
};

/** @return the reason's name for logs */
const char* rejectReasonName(RejectReason reason);

/** BLOB kinds. Stable: append only. */
enum class BlobKind : uint8_t {
    ProcessList = 1,  ///< headset -> PC; stream id = kStreamCpuList / kStreamRamList
    Layout = 2,       ///< headset -> PC, the answer to LAYOUT_GET: compact layout JSON (version inside)
    LayoutSet = 3,    ///< PC -> headset: LAYOUT_SET { pc id, request id, base version, layout JSON }
    Camera = 4,       ///< headset -> PC, ticket 10; stream id = camera id
};

constexpr uint8_t kStreamCpuList = 0;
constexpr uint8_t kStreamRamList = 1;

/**
 * SUBSCRIBE wants. Bits 8-13: camera ids 0-5 (ticket 10, core/camera_frame.h); bit 31: a camera-path-only
 * SUBSCRIBE (core/camera_frame.h kWantCameraPathOnly).
 */
constexpr uint32_t kWantMetrics = 1u << 0;
constexpr uint32_t kWantCpuList = 1u << 1;
constexpr uint32_t kWantRamList = 1u << 2;

/** A 128-bit id (PC id, headset id). */
using Id128 = std::array<uint8_t, 16>;

/** @return 32 lowercase hex chars */
std::string idToHex(const Id128& id);
/** @return false unless text is exactly 32 hex chars */
bool idFromHex(const std::string& text, Id128& out);
/** @return a random id (std::random_device) */
Id128 randomId();

/** Longest PC / headset name on the wire (bytes; cut at a UTF-8 boundary). */
constexpr size_t kMaxNameBytes = 63;

/** @return text cut to at most maxBytes without splitting a UTF-8 sequence */
std::string cutUtf8(const std::string& text, size_t maxBytes);

/** @return "a.b.c.d" for a host-order IPv4 address */
std::string ipv4ToText(uint32_t ip);
/** Parses "a.b.c.d" (no port). @return false if it isn't a dotted IPv4 address */
bool ipv4FromText(const std::string& text, uint32_t& out);

// ---- header ----

struct Header {
    uint8_t version = kProtocolVersion;
    PacketType type = PacketType::Hello;
    uint8_t flags = 0;
    uint32_t seq = 0;
    uint16_t length = 0;  ///< payload bytes
};

enum class DecodeError {
    Ok = 0,
    TooShort,      ///< less than a header, or fewer payload bytes than the header says
    BadMagic,
    BadVersion,    ///< protocol version this build doesn't speak (the header is still filled in)
    Encrypted,     ///< flags bit 0: v1 can't read it
    TooLong,       ///< payload length over kMaxPayload, or trailing bytes after the payload
};

/**
 * Splits a datagram into header + payload.
 * @param data the datagram
 * @param size its length
 * @param header the header (filled in whenever the magic matched)
 * @param payload points into data on success
 * @return Ok, or why the packet must be dropped
 */
DecodeError decodePacket(const uint8_t* data, size_t size, Header& header, const uint8_t*& payload);

/**
 * Builds a datagram.
 * @return the bytes; empty if the payload is over kMaxPayload
 */
std::vector<uint8_t> encodePacket(PacketType type, uint32_t seq, const std::vector<uint8_t>& payload);

// ---- little-endian helpers (bounds-checked reader) ----

class Writer {
public:
    void u8(uint8_t v) { bytes_.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f32(float v);
    void id(const Id128& v) { bytes_.insert(bytes_.end(), v.begin(), v.end()); }
    /** A u8 length + bytes, cut to maxBytes at a UTF-8 boundary (maxBytes ≤ 255). */
    void str8(const std::string& v, size_t maxBytes = 255);
    void raw(const uint8_t* data, size_t size) { bytes_.insert(bytes_.end(), data, data + size); }
    std::vector<uint8_t>& bytes() { return bytes_; }
    size_t size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int32_t i32() { return static_cast<int32_t>(u32()); }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32();
    Id128 id();
    std::string str8();
    /** @return a pointer to the next n bytes, or nullptr (and failed) if there aren't n left */
    const uint8_t* take(size_t n);
    size_t left() const { return failed_ ? 0 : size_ - pos_; }
    /** @return true if any read ran past the end */
    bool failed() const { return failed_; }
    /** @return true if every byte was read and nothing failed */
    bool done() const { return !failed_ && pos_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool failed_ = false;
};

// ---- messages ----

/** HELLO (PC -> headset). The protocol version is the header's. */
struct Hello {
    Id128 pcId {};
    std::string pcName;
};

/** One headset address in HELLO_ACK. */
struct LocalAddress {
    std::string ifName;  ///< "wlan0", "wlanap", ...
    uint32_t ip = 0;     ///< host order
};

/** HELLO_ACK (headset -> PC). */
struct HelloAck {
    Id128 headsetId {};
    std::string name;
    uint8_t protocol = kProtocolVersion;
    PeerStatus status = PeerStatus::Pending;
    std::vector<LocalAddress> addresses;  ///< at most 8
    uint32_t layoutVersion = 0;
    uint32_t seenIp = 0;     ///< the PC's address as the headset saw it (host order): tells the PC which path
    uint16_t seenPort = 0;
};

/** SUBSCRIBE (PC -> headset). */
struct Subscribe {
    Id128 pcId {};
    uint32_t wants = kWantMetrics;
    /**
     * T10: camera frames per second the PC wants (1-30). On the wire exactly when wants has a camera bit (8-13), as a
     * trailing u8, so a SUBSCRIBE without cameras is byte-for-byte what it was. 0 = not given (the headset's 10).
     */
    uint8_t cameraFps = 0;
};

/** SUBSCRIBE_ACK (headset -> PC). */
struct SubscribeAck {
    PeerStatus status = PeerStatus::Allowed;
    uint16_t leaseMs = static_cast<uint16_t>(kLeaseSec * 1000);
};

/** LINK_STATE: carried in every METRICS part. */
struct LinkState {
    uint32_t layoutVersion = 0;
    int32_t sessionId = 0;         ///< 0 = no session yet
    int64_t sessionStartUnix = 0;
    std::string appKey;            ///< ≤ 63 bytes on the wire; T11: empty while the session's app is gone
    std::string appName;           ///< ≤ 63 bytes on the wire

    bool operator==(const LinkState& o) const {
        return layoutVersion == o.layoutVersion && sessionId == o.sessionId &&
               sessionStartUnix == o.sessionStartUnix && appKey == o.appKey && appName == o.appName;
    }
};

/** One typed metric value on the wire: id u16, type u8, status u8, then the value (ok) or a reason u16 (N/A). */
struct WireValue {
    uint16_t id = 0;
    WireType type = WireType::F32;
    bool ok = false;
    Reason reason = Reason::NotSampledYet;  ///< when !ok
    float f32 = 0.0f;
    int32_t i32 = 0;
    uint8_t enumValue = 0;  ///< enums are u8 on the wire
    std::string text;       ///< ≤ kShortStringMax bytes

    bool operator==(const WireValue& o) const;
};

/** @return the wire form of a store value */
WireValue toWire(uint16_t id, const MetricValue& value);

/**
 * Puts a received value into a store (the PC side). Unknown ids or a type that disagrees with this build's
 * registry are skipped (an older or newer peer).
 * @return true if stored
 */
bool storeWire(MetricStore& store, const WireValue& value, double now);
/** Reads a typed value without consulting the local registry, so newer peers' ids can be skipped. */
bool readWireValue(Reader& reader, WireValue& value);

/** One METRICS part. */
struct Metrics {
    uint64_t headsetMs = 0;    ///< headset monotonic ms when the snapshot was taken
    uint32_t snapshotId = 0;   ///< the same for every part of one snapshot
    uint8_t part = 0;
    uint8_t partCount = 1;
    LinkState state;
    std::vector<WireValue> values;
};

/**
 * Encodes one snapshot as METRICS payloads, each ≤ kMaxPayload and each carrying LINK_STATE, so a lost part
 * only loses its own values.
 * @return one payload per part (at least one, even with no values)
 */
std::vector<std::vector<uint8_t>> encodeMetrics(uint64_t headsetMs, uint32_t snapshotId, const LinkState& state,
                                                const std::vector<WireValue>& values);

/** @return false if the payload is malformed */
bool decodeMetrics(const uint8_t* payload, size_t size, Metrics& out);

/** One BLOB chunk. */
struct BlobChunk {
    uint32_t blobId = 0;
    BlobKind kind = BlobKind::ProcessList;
    uint8_t stream = 0;
    uint16_t index = 0;
    uint16_t count = 1;
    uint32_t totalSize = 0;
    const uint8_t* data = nullptr;  ///< points into the payload
    size_t size = 0;
};

constexpr size_t kBlobChunkHeader = 14;
constexpr size_t kBlobChunkData = kMaxPayload - kBlobChunkHeader;
constexpr uint16_t kMaxBlobChunks = 1024;  ///< ~1.2 MB: room for a camera JPEG

/**
 * Splits a blob into BLOB payloads.
 * @return the payloads; empty if data needs more than kMaxBlobChunks chunks
 */
std::vector<std::vector<uint8_t>> splitBlob(uint32_t blobId, BlobKind kind, uint8_t stream,
                                            const std::vector<uint8_t>& data);

/** @return false if the payload is malformed (chunk data must match index/count/totalSize) */
bool decodeBlobChunk(const uint8_t* payload, size_t size, BlobChunk& out);

/** A reassembled blob. */
struct Blob {
    uint64_t peer = 0;
    uint32_t blobId = 0;
    BlobKind kind = BlobKind::ProcessList;
    uint8_t stream = 0;
    std::vector<uint8_t> data;
};

/**
 * Reassembles BLOBs. One blob in progress per (peer, kind, stream): a chunk of a newer blob id replaces the
 * older incomplete one, chunks of older (or already completed) blob ids are dropped, and an incomplete blob
 * expires kBlobExpirySec after its first chunk. Different kinds or streams never cancel each other.
 */
class BlobAssembler {
public:
    /**
     * @param peer who sent it (the caller's key for the source address)
     * @param chunk a decoded chunk
     * @param now monotonic seconds
     * @param out the blob, when this chunk completed it
     * @return true if a blob was completed
     */
    bool add(uint64_t peer, const BlobChunk& chunk, double now, Blob& out);

    /** Drops expired incomplete blobs and forgets keys with no accepted chunk for 10 s. */
    void expire(double now);

    /**
     * Forgets everything about one peer (in-progress blobs and the newest completed ids). Call when the peer
     * (re)starts its conversation: its blob ids may have restarted at 1, and would otherwise read as old.
     */
    void forgetPeer(uint64_t peer);

    /** @return blobs in progress */
    size_t inProgress() const;
    /** @return chunks dropped (stale id, mismatched shape, expired), for the debug dump */
    uint64_t dropped() const { return dropped_; }

private:
    struct Partial {
        bool active = false;
        uint32_t blobId = 0;
        uint16_t count = 0;
        uint16_t received = 0;
        uint32_t totalSize = 0;
        double started = 0.0;
        std::vector<uint8_t> data;
        std::vector<bool> have;
        bool haveCompleted = false;
        uint32_t lastCompleted = 0;  ///< newest blob id completed on this key
        double lastActivity = 0.0;
    };
    using Key = std::tuple<uint64_t, uint8_t, uint8_t>;
    std::map<Key, Partial> partials_;
    uint64_t dropped_ = 0;
};

/** @return true if blob id a is newer than b (serial number arithmetic, so ids may wrap) */
inline bool newerId(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

/** LAYOUT_GET (PC -> headset). */
struct LayoutGet {
    Id128 pcId {};
};

/** LAYOUT_SET, carried as a BLOB of kind LayoutSet so layouts of any size fit. */
struct LayoutSet {
    Id128 pcId {};
    uint32_t requestId = 0;
    uint32_t baseVersion = 0;
    std::string layoutJson;  ///< layoutToJson(layout, 0); its "version" is ignored
};

/** LAYOUT_ACK (headset -> PC). */
struct LayoutAck {
    uint32_t requestId = 0;
    uint32_t version = 0;  ///< the new version
};

/** LAYOUT_REJECT (headset -> PC). */
struct LayoutReject {
    uint32_t requestId = 0;
    uint32_t currentVersion = 0;
    RejectReason reason = RejectReason::Stale;
};

/** BYE (both ways): the sender's id. */
struct Bye {
    Id128 id {};
};

/** A process list row on the wire (widgets 7 / 8). */
struct ProcessRowWire {
    std::string name;  ///< ≤ 63 bytes
    int32_t pid = 0;   ///< 0 for a summed row
    uint16_t count = 1;
    float cpuCorePct = 0.0f;
    float rssMiB = 0.0f;
};

/** A process list BLOB's contents. */
struct ProcessListWire {
    uint64_t generation = 0;  ///< the headset's scan counter
    bool cpuValid = false;    ///< CPU list only: there's a real interval behind it
    std::vector<ProcessRowWire> rows;
};

// ---- R3: CONTROL_STATE / CONTROL_ACTION / CONTROL_RESULT ----
// Newer fields go at the end: the decoders below ignore trailing bytes, so a newer peer can append without
// breaking an older one.

constexpr size_t kMaxControlProfiles = 8;
constexpr double kControlStateEverySec = 2.0;  ///< headset: CONTROL_STATE to each lease holder at least this often
constexpr double kControlResendSec = 1.0;      ///< PC: resend CONTROL_ACTION until CONTROL_RESULT
constexpr double kControlGiveUpSec = 5.0;      ///< PC: no answer after this long: give up ("no answer")

/** CONTROL_STATE flags. */
constexpr uint8_t kControlRecording = 1u << 0;      ///< Record is on
constexpr uint8_t kControlCrashRecorder = 1u << 1;  ///< the crash recorder runs
constexpr uint8_t kControlUnsaved = 1u << 2;        ///< Use / Reset would lose the live layout (headset's rule)

/** One saved profile in CONTROL_STATE. */
struct ControlProfile {
    std::string id;    ///< "profile-3" (≤ 15 bytes)
    std::string name;  ///< ≤ 31 bytes

    bool operator==(const ControlProfile& o) const { return id == o.id && name == o.name; }
};

/**
 * CONTROL_STATE (headset -> PC): what the headset's Profiles and Logs pages show. A full snapshot, like METRICS:
 * a lost one is healed by the next.
 *   revision u32 | layout version u32 | flags u8 | max profiles u8 | active id str8 | count u8 |
 *   count × (id str8, name str8) | status str8 | record status str8 | crash status str8 | power-off str8
 */
struct ControlState {
    uint32_t revision = 0;       ///< bumps whenever anything below changes
    uint32_t layoutVersion = 0;  ///< the live layout's version: a confirmed Use / Reset is bound to it
    uint8_t flags = 0;           ///< kControl*
    uint8_t maxProfiles = 8;
    std::string activeId;        ///< "" = no active profile
    std::vector<ControlProfile> profiles;  ///< by id number, ≤ kMaxControlProfiles
    std::string status;          ///< the last profile action's result (≤ 127 bytes)
    std::string recordStatus;    ///< "Recording 3:12 to …" (≤ 127)
    std::string crashStatus;     ///< "Crash log kept (12:55)" (≤ 63)
    std::string powerOff;        ///< the last power-off reason (≤ 31)

    /** Equal apart from the revision. */
    bool sameContent(const ControlState& o) const;
};

/** CONTROL_ACTION kinds. Stable: append only (a headset that doesn't know one answers Unsupported). */
enum class ControlKind : uint8_t {
    ProfileNew = 1,
    ProfileUse = 2,     ///< profileId
    ProfileSave = 3,    ///< the active profile
    ProfileRename = 4,  ///< profileId + text
    ProfileDelete = 5,  ///< profileId; needs kControlConfirmed
    ProfileReset = 6,
    RecordOn = 7,
    RecordOff = 8,
    CrashRecorderOn = 9,
    CrashRecorderOff = 10,
};

/** CONTROL_ACTION flags. */
constexpr uint8_t kControlConfirmed = 1u << 0;  ///< the PC's second tap: "lose the changes" (Use / Reset) / delete

/**
 * CONTROL_ACTION (PC -> headset), from a leased PC only.
 *   pc id 16 | request id u32 | kind u8 | flags u8 | layout version u32 | profile id str8 | text str8
 */
struct ControlAction {
    Id128 pcId {};
    uint32_t requestId = 0;
    ControlKind kind = ControlKind::ProfileNew;
    uint8_t flags = 0;
    uint32_t layoutVersion = 0;  ///< the layout version the PC's confirmation was for (CONTROL_STATE's)
    std::string profileId;       ///< ≤ 15 bytes
    std::string text;            ///< Rename's new name, ≤ 31 bytes
};

/** CONTROL_RESULT outcomes. */
enum class ControlOutcome : uint8_t {
    Done = 0,               ///< carried out (or nothing to do)
    NeedsConfirm = 1,       ///< Use / Reset would lose unsaved changes, Delete needs a second tap: ask, then resend confirmed
    Failed = 2,             ///< tried and failed: message says why
    NotAllowed = 3,         ///< no valid lease from this address
    UnknownSubscriber = 4,  ///< HELLO first
    Unsupported = 5,        ///< a kind this headset doesn't know
};

/** @return the outcome's name for logs */
const char* controlOutcomeName(ControlOutcome outcome);

/** CONTROL_RESULT (headset -> PC): request id u32 | outcome u8 | layout version u32 | message str8 */
struct ControlResult {
    uint32_t requestId = 0;
    ControlOutcome outcome = ControlOutcome::Done;
    uint32_t layoutVersion = 0;  ///< the headset's current layout version (NeedsConfirm: confirm against this)
    std::string message;         ///< ≤ 127 bytes ("Using Gaming", "Couldn't save: …")
};

/**
 * The headset's second-tap rule for a PC's action, the same as a tap on its own dashboard: Delete always needs the
 * confirmation; Use and Reset need it when they'd lose unsaved changes, and the confirmation only counts for the
 * layout version the PC saw (an edit since then must be confirmed again).
 * @param unsaved the headset's own "unsaved" right now (not a cached copy)
 * @param currentVersion the live layout's version right now
 * @return true if it may go ahead; false = answer NeedsConfirm
 */
bool controlActionConfirmed(const ControlAction& action, bool unsaved, uint32_t currentVersion);

std::vector<uint8_t> encodeControlState(const ControlState& m);
bool decodeControlState(const uint8_t* payload, size_t size, ControlState& out);
std::vector<uint8_t> encodeControlAction(const ControlAction& m);
/** Unknown kinds decode fine (the receiver answers Unsupported); only short / malformed payloads fail. */
bool decodeControlAction(const uint8_t* payload, size_t size, ControlAction& out);
std::vector<uint8_t> encodeControlResult(const ControlResult& m);
bool decodeControlResult(const uint8_t* payload, size_t size, ControlResult& out);

std::vector<uint8_t> encodeHello(const Hello& m);
bool decodeHello(const uint8_t* payload, size_t size, Hello& out);
std::vector<uint8_t> encodeHelloAck(const HelloAck& m);
bool decodeHelloAck(const uint8_t* payload, size_t size, HelloAck& out);
std::vector<uint8_t> encodeSubscribe(const Subscribe& m);
bool decodeSubscribe(const uint8_t* payload, size_t size, Subscribe& out);
std::vector<uint8_t> encodeSubscribeAck(const SubscribeAck& m);
bool decodeSubscribeAck(const uint8_t* payload, size_t size, SubscribeAck& out);
std::vector<uint8_t> encodeLayoutGet(const LayoutGet& m);
bool decodeLayoutGet(const uint8_t* payload, size_t size, LayoutGet& out);
/** The LayoutSet BLOB's contents (split with splitBlob). */
std::vector<uint8_t> encodeLayoutSet(const LayoutSet& m);
bool decodeLayoutSet(const uint8_t* data, size_t size, LayoutSet& out);
std::vector<uint8_t> encodeLayoutAck(const LayoutAck& m);
bool decodeLayoutAck(const uint8_t* payload, size_t size, LayoutAck& out);
std::vector<uint8_t> encodeLayoutReject(const LayoutReject& m);
bool decodeLayoutReject(const uint8_t* payload, size_t size, LayoutReject& out);
std::vector<uint8_t> encodeBye(const Bye& m);
bool decodeBye(const uint8_t* payload, size_t size, Bye& out);
/** The ProcessList BLOB's contents (at most 255 rows). */
std::vector<uint8_t> encodeProcessList(const ProcessListWire& m);
bool decodeProcessList(const uint8_t* data, size_t size, ProcessListWire& out);

}  // namespace link
}  // namespace fph
