// Headset side of the PC link: LinkServer's protocol logic (driven with handlePacket / tick against a fake
// LinkHostEnv), the vrlink host-IP lookup, LayoutAuthority, and the "link" config key.
// POSIX only (symlinks, dup2 to capture stderr). Works in a temp dir; never touches ~/.config.
#include "atomic_file.h"
#include "config.h"
#include "layout_authority.h"
#include "link_server.h"
#include "link_wire.h"
#include "stream_metrics.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
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

using namespace fph::link;
using Bytes = std::vector<uint8_t>;

std::string tempDir;

constexpr uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a << 24 | b << 16 | c << 8 | d; }

LinkEndpoint ep(uint32_t address, uint16_t port) {
    LinkEndpoint e;
    e.ip = address;
    e.port = port;
    return e;
}

Id128 pcId(uint8_t seed) {
    Id128 id {};
    for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(seed + i * 5);
    return id;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

/** Redirects fd 2 into a file while alive; text() returns everything written meanwhile. */
class StderrCapture {
public:
    explicit StderrCapture(const std::string& path) : path_(path) {
        std::fflush(stderr);
        saved_ = ::dup(2);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(fd, 2);
        ::close(fd);
    }
    ~StderrCapture() { stop(); }
    void stop() {
        if (saved_ < 0) return;
        std::fflush(stderr);
        ::dup2(saved_, 2);
        ::close(saved_);
        saved_ = -1;
    }
    std::string text() {
        std::fflush(stderr);
        return readFile(path_);
    }

private:
    std::string path_;
    int saved_ = -1;
};

// ---- the fake agent ----

struct Sent {
    LinkEndpoint to;
    Header header;
    Bytes payload;
};

struct FakeEnv : LinkHostEnv {
    std::vector<Sent> sent;
    std::vector<uint32_t> hostIps;
    std::function<std::vector<uint32_t>()> hostIpFn;  ///< overrides hostIps when set
    std::vector<LocalAddress> addresses = {{"wlan0", ip(192, 168, 1, 47)}, {"wlanap", ip(10, 35, 78, 1)}};
    LinkState state;
    std::vector<WireValue> values;
    ProcessListWire cpu, ram;
    uint64_t generation = 0;
    int processListCalls = 0;
    int saves = 0;
    bool saveOk = true;
    std::vector<fph::PcMetrics> pcMetricsCalls;  // ticket 08: what reached the PC_METRICS gate

    FakeEnv() {
        state.sessionId = 7;
        state.appKey = "steam:1";
        state.appName = "Some Game";
        for (uint16_t i = 1; i <= 3; ++i) {
            WireValue v;
            v.id = i;
            v.type = fph::WireType::F32;
            v.ok = true;
            v.f32 = static_cast<float>(i);
            values.push_back(v);
        }
        cpu.cpuValid = true;
        cpu.rows = {{"vrserver", 10, 1, 50.0f, 300.0f}};
        ram.rows = {{"chrome", 0, 5, 0.0f, 2000.0f}};
    }

    void send(const LinkEndpoint& to, const Bytes& datagram) override {
        Sent s;
        s.to = to;
        const uint8_t* payload = nullptr;
        CHECK(decodePacket(datagram.data(), datagram.size(), s.header, payload) == DecodeError::Ok);
        if (payload != nullptr) s.payload.assign(payload, payload + s.header.length);
        sent.push_back(std::move(s));
    }
    std::vector<uint32_t> streamHostIps() override { return hostIpFn ? hostIpFn() : hostIps; }
    std::vector<LocalAddress> localAddresses() override { return addresses; }
    LinkState linkState() override { return state; }
    std::vector<WireValue> metricValues() override { return values; }
    uint64_t processLists(ProcessListWire& c, ProcessListWire& r) override {
        ++processListCalls;
        c = cpu;
        r = ram;
        c.generation = r.generation = generation;
        return generation;
    }
    bool saveConfig(std::string& error) override {
        ++saves;
        if (!saveOk) {
            error = "disk full";
            return false;
        }
        return true;
    }
    void pcMetrics(const fph::PcMetrics& m, double) override { pcMetricsCalls.push_back(m); }
};

fph::Layout modLayout(int n) {
    fph::Layout layout = fph::defaultLayout();
    for (fph::LayoutColumn& column : layout.columns) {
        for (fph::WidgetInstance& w : column.widgets) {
            if (w.type == fph::WidgetType::CpuItems) {
                w.maxItems = static_cast<uint32_t>(3 + n);
                return layout;
            }
        }
    }
    return layout;
}

Config& prepare(Config& c) {
    c.link.headsetId = idToHex(pcId(0xEE));
    return c;
}

/** A server wired to a fake env and a LayoutAuthority that counts saves and applies. */
struct Rig {
    Config config;
    FakeEnv env;
    int layoutSaves = 0;
    int applies = 0;
    bool layoutSaveOk = true;
    fph::Layout lastApplied;
    LayoutAuthority auth;
    LinkServer server;

    Rig()
        : auth(prepare(config),
               [this](const Config&, std::string& error) {
                   ++layoutSaves;
                   if (!layoutSaveOk) {
                       error = "disk full";
                       return false;
                   }
                   return true;
               },
               [this](const fph::Layout& l) {
                   ++applies;
                   lastApplied = l;
               }),
          server(config, auth, env, "Frame") {}

    void allowInConfig(uint8_t seed, const std::string& name = "Old PC") {
        config.link.allowedPcs.push_back({idToHex(pcId(seed)), name});
    }

    void send(PacketType type, const Bytes& payload, const LinkEndpoint& from, double now) {
        const Bytes datagram = encodePacket(type, 1, payload);
        server.handlePacket(datagram.data(), datagram.size(), from, now);
    }

    std::vector<Sent> take() {
        std::vector<Sent> out = std::move(env.sent);
        env.sent.clear();
        return out;
    }

    HelloAck hello(uint8_t seed, const LinkEndpoint& from, double now, const std::string& name = "PC") {
        Hello m;
        m.pcId = pcId(seed);
        m.pcName = name;
        send(PacketType::Hello, encodeHello(m), from, now);
        HelloAck ack;
        const std::vector<Sent> out = take();
        CHECK(out.size() == 1 && out[0].header.type == PacketType::HelloAck && out[0].to == from);
        if (!out.empty()) CHECK(decodeHelloAck(out[0].payload.data(), out[0].payload.size(), ack));
        return ack;
    }

    SubscribeAck subscribe(uint8_t seed, const LinkEndpoint& from, double now, uint32_t wants = kWantMetrics) {
        Subscribe m;
        m.pcId = pcId(seed);
        m.wants = wants;
        send(PacketType::Subscribe, encodeSubscribe(m), from, now);
        SubscribeAck ack;
        const std::vector<Sent> out = take();
        CHECK(out.size() == 1 && out[0].header.type == PacketType::SubscribeAck && out[0].to == from);
        if (!out.empty()) CHECK(decodeSubscribeAck(out[0].payload.data(), out[0].payload.size(), ack));
        return ack;
    }

    void bye(uint8_t seed, const LinkEndpoint& from, double now) {
        Bye m;
        m.id = pcId(seed);
        send(PacketType::Bye, encodeBye(m), from, now);
    }

    /** Sends a LAYOUT_SET as a BLOB (kind LayoutSet) under blobId. */
    void layoutSet(uint8_t seed, const LinkEndpoint& from, uint32_t requestId, uint32_t base, const std::string& json,
                   uint32_t blobId, double now) {
        LayoutSet m;
        m.pcId = pcId(seed);
        m.requestId = requestId;
        m.baseVersion = base;
        m.layoutJson = json;
        for (const Bytes& chunk : splitBlob(blobId, BlobKind::LayoutSet, 0, encodeLayoutSet(m))) {
            send(PacketType::Blob, chunk, from, now);
        }
    }
};

/** The LAYOUT_ACK / LAYOUT_REJECT answers among sent packets, in order. */
struct Answer {
    bool ack = false;
    uint32_t requestId = 0;
    uint32_t version = 0;  ///< ACK: the new version; REJECT: the current one
    RejectReason reason = RejectReason::Stale;
    LinkEndpoint to;
};

std::vector<Answer> answersIn(const std::vector<Sent>& sent) {
    std::vector<Answer> out;
    for (const Sent& s : sent) {
        Answer a;
        a.to = s.to;
        if (s.header.type == PacketType::LayoutAck) {
            LayoutAck m;
            CHECK(decodeLayoutAck(s.payload.data(), s.payload.size(), m));
            a.ack = true;
            a.requestId = m.requestId;
            a.version = m.version;
            out.push_back(a);
        } else if (s.header.type == PacketType::LayoutReject) {
            LayoutReject m;
            CHECK(decodeLayoutReject(s.payload.data(), s.payload.size(), m));
            a.requestId = m.requestId;
            a.version = m.currentVersion;
            a.reason = m.reason;
            out.push_back(a);
        }
    }
    return out;
}

std::vector<Sent> ofType(const std::vector<Sent>& sent, PacketType type) {
    std::vector<Sent> out;
    for (const Sent& s : sent) {
        if (s.header.type == type) out.push_back(s);
    }
    return out;
}

// ---- allow-list ----

void testAllowList() {
    const uint32_t hostIp = ip(192, 168, 1, 45);
    const uint32_t otherIp = ip(192, 168, 1, 99);
    {
        Rig r;
        r.env.hostIps = {hostIp};
        const HelloAck ack = r.hello(1, ep(hostIp, 40000), 0.0, "Gaming PC");
        CHECK(ack.status == PeerStatus::Allowed);
        CHECK(idToHex(ack.headsetId) == r.config.link.headsetId && ack.name == "Frame");
        CHECK(ack.seenIp == hostIp && ack.seenPort == 40000 && ack.layoutVersion == 1);
        CHECK(ack.addresses.size() == 2 && ack.addresses[1].ifName == "wlanap");
        CHECK(r.server.uiState().pending.empty());
        CHECK(r.env.saves == 0);  // a Steam Link host is allowed without touching the config
    }
    {
        Rig r;
        r.env.hostIps = {hostIp};
        // pending: not a host IP; listed oldest first (first seen, not id order)
        CHECK(r.hello(9, ep(otherIp, 40001), 0.0, "Laptop").status == PeerStatus::Pending);
        CHECK(r.hello(1, ep(ip(192, 168, 1, 98), 40002), 1.0, "Desk").status == PeerStatus::Pending);
        LinkUiState ui = r.server.uiState();
        CHECK(ui.pending.size() == 2);
        CHECK(ui.pending[0].id == idToHex(pcId(9)) && ui.pending[0].name == "Laptop");
        CHECK(ui.pending[1].id == idToHex(pcId(1)) && ui.pending[1].name == "Desk");
        CHECK(ui.knownPcs == 0);
        // a repeated HELLO doesn't reorder or duplicate
        r.hello(9, ep(otherIp, 40001), 2.0, "Laptop");
        CHECK(r.server.uiState().pending.size() == 2 && r.server.uiState().pending[0].name == "Laptop");

        // allow: saved, listed in allowed_pcs, revision moves, next HELLO is allowed
        const uint64_t rev = ui.revision;
        const int saves = r.env.saves;
        r.server.allow(idToHex(pcId(9)));
        ui = r.server.uiState();
        CHECK(r.env.saves == saves + 1);
        CHECK(ui.knownPcs == 1 && ui.pending.size() == 1 && ui.pending[0].name == "Desk");
        CHECK(ui.revision != rev);
        CHECK(r.config.link.allowedPcs.size() == 1 && r.config.link.allowedPcs[0].id == idToHex(pcId(9)) &&
              r.config.link.allowedPcs[0].name == "Laptop");
        CHECK(r.hello(9, ep(otherIp, 40001), 3.0, "Laptop").status == PeerStatus::Allowed);
        CHECK(r.subscribe(9, ep(otherIp, 40001), 3.0).status == PeerStatus::Allowed);
        // allowing an id that isn't pending / unknown is ignored
        const int saves2 = r.env.saves;
        r.server.allow(idToHex(pcId(77)));
        r.server.allow(idToHex(pcId(9)));
        CHECK(r.env.saves == saves2 && r.config.link.allowedPcs.size() == 1);

        // deny: the next HELLO says denied, it leaves the pending list, and it has no layout rights
        const uint64_t rev2 = r.server.uiState().revision;
        r.server.deny(idToHex(pcId(1)));
        CHECK(r.server.uiState().pending.empty() && r.server.uiState().revision != rev2);
        CHECK(r.hello(1, ep(ip(192, 168, 1, 98), 40002), 4.0).status == PeerStatus::Denied);
        CHECK(r.subscribe(1, ep(ip(192, 168, 1, 98), 40002), 4.0).status == PeerStatus::Denied);
        r.layoutSet(1, ep(ip(192, 168, 1, 98), 40002), 1, 1, fph::layoutToJson(modLayout(1)), 1, 4.0);
        CHECK(r.take().empty() && r.applies == 0);
        // even a denied PC whose IP is a host IP stays denied this run
        r.env.hostIps.push_back(ip(192, 168, 1, 98));
        CHECK(r.hello(1, ep(ip(192, 168, 1, 98), 40002), 20.0).status == PeerStatus::Denied);
    }
    {
        // forgetAll: clears allowed_pcs (saved), the PC's next SUBSCRIBE is unknown_subscriber
        Rig r;
        r.env.hostIps = {hostIp};
        const LinkEndpoint pc = ep(otherIp, 40001);
        r.hello(5, pc, 0.0);
        r.server.allow(idToHex(pcId(5)));
        CHECK(r.hello(5, pc, 1.0).status == PeerStatus::Allowed);
        CHECK(r.subscribe(5, pc, 1.0).status == PeerStatus::Allowed && r.server.hasLease());
        // one auto-allowed (Steam Link host) PC with a lease too
        const LinkEndpoint host = ep(hostIp, 40010);
        r.hello(6, host, 1.0);
        CHECK(r.subscribe(6, host, 1.0).status == PeerStatus::Allowed);
        const int saves = r.env.saves;
        const uint64_t rev = r.server.uiState().revision;
        r.server.forgetAll();
        CHECK(r.config.link.allowedPcs.empty() && r.server.uiState().knownPcs == 0);
        CHECK(r.env.saves == saves + 1 && r.server.uiState().revision != rev);
        CHECK(r.subscribe(5, pc, 2.0).status == PeerStatus::UnknownSubscriber);
        CHECK(r.subscribe(6, host, 2.0).status == PeerStatus::Allowed);  // host-IP PCs stay allowed
        r.server.tick(2.5);
        CHECK(!ofType(r.take(), PacketType::Metrics).empty());  // ...and keep their lease
        // the forgotten PC asks again: back to pending
        CHECK(r.hello(5, pc, 3.0).status == PeerStatus::Pending);
    }
    {
        // allowed_pcs from the config: allowed, and no host lookup needed
        Rig r;
        r.allowInConfig(3);
        CHECK(r.hello(3, ep(otherIp, 40003), 0.0).status == PeerStatus::Allowed);
        CHECK(r.server.hostIpLookups() == 0);
        CHECK(r.server.uiState().knownPcs == 1);
    }
    {
        // host-IP lookup cached 5 s
        Rig r;
        r.hello(1, ep(otherIp, 1), 0.0);
        r.hello(2, ep(otherIp, 1), 1.0);
        CHECK(r.server.hostIpLookups() == 1);
        r.hello(1, ep(otherIp, 1), 4.9);
        CHECK(r.server.hostIpLookups() == 1);
        r.hello(1, ep(otherIp, 1), 6.0);
        CHECK(r.server.hostIpLookups() == 2);
    }
    {
        // the cached answer is used: a host that shows up inside the window is picked up at the next lookup
        Rig r;
        const LinkEndpoint pc = ep(hostIp, 5);
        CHECK(r.hello(1, pc, 0.0).status == PeerStatus::Pending);
        r.env.hostIps = {hostIp};
        CHECK(r.hello(1, pc, 1.0).status == PeerStatus::Pending);  // still cached
        CHECK(r.hello(1, pc, 6.0).status == PeerStatus::Allowed);
        CHECK(r.server.uiState().pending.empty());
    }
    {
        // a PC that becomes pending/allowed bumps the revision; a pending PC that goes quiet is forgotten
        Rig r;
        const uint64_t rev0 = r.server.uiState().revision;
        r.hello(1, ep(otherIp, 1), 0.0);
        const uint64_t rev1 = r.server.uiState().revision;
        CHECK(rev1 != rev0);
        r.server.tick(5.0);
        CHECK(r.server.uiState().pending.size() == 1);
        r.server.tick(16.0);
        CHECK(r.server.uiState().pending.empty() && r.server.uiState().revision != rev1);
    }
    {
        // a new headset id is made when the config has none; the owner saves it (the server doesn't)
        FakeEnv env;
        Config c;
        LayoutAuthority auth(c, nullptr, nullptr);
        {
            LinkServer s(c, auth, env, "Frame");
            Id128 id {};
            CHECK(idFromHex(c.link.headsetId, id));
            CHECK(s.headsetIdIsNew());
            CHECK(env.saves == 0);
        }
        FakeEnv env2;
        Config c2;
        c2.link.headsetId = std::string(32, 'a');
        LinkServer s2(c2, auth, env2, "Frame");
        CHECK(!s2.headsetIdIsNew() && env2.saves == 0 && c2.link.headsetId == std::string(32, 'a'));
    }
    {
        // garbage and wrong-direction packets are counted and dropped
        Rig r;
        const uint8_t junk[5] = {1, 2, 3, 4, 5};
        r.server.handlePacket(junk, sizeof(junk), ep(otherIp, 1), 0.0);
        CHECK(r.server.malformed() == 1);
        SubscribeAck m;
        r.send(PacketType::SubscribeAck, encodeSubscribeAck(m), ep(otherIp, 1), 0.0);
        CHECK(r.server.malformed() == 2);
        r.send(PacketType::Hello, Bytes(3, 0), ep(otherIp, 1), 0.0);
        CHECK(r.server.malformed() == 3);
        r.send(PacketType::PcMetrics, Bytes(), ep(otherIp, 1), 0.0);  // ticket 08: too short to decode
        CHECK(r.server.malformed() == 4 && r.take().empty());
        // a well-formed PC_METRICS that decodes fine, just from a pc id nobody ever said HELLO to, is
        // silently ignored rather than malformed (see testPcMetrics for the gate itself)
        fph::PcMetrics unknownPc;
        unknownPc.pcId = pcId(250);
        unknownPc.snapshotId = 1;
        r.send(PacketType::PcMetrics, fph::encodePcMetrics(unknownPc), ep(otherIp, 1), 0.0);
        CHECK(r.server.malformed() == 4 && r.take().empty() && r.env.pcMetricsCalls.empty());
        // PCs send only LayoutSet blobs
        const std::vector<Bytes> chunks = splitBlob(1, BlobKind::Camera, 0, Bytes(10, 1));
        r.send(PacketType::Blob, chunks[0], ep(otherIp, 1), 0.0);
        CHECK(r.server.malformed() == 5 && r.take().empty());
        CHECK(!r.server.debugText(1.0).empty());
    }
}

// ---- PC_METRICS (ticket 08): the lease/from-address/ordering gate ----

Bytes pcMetricsBytes(uint8_t seed, uint32_t snapshotId) {
    fph::PcMetrics m;
    m.pcId = pcId(seed);
    m.snapshotId = snapshotId;
    return fph::encodePcMetrics(m);
}

void testPcMetrics() {
    const uint32_t otherIp = ip(192, 168, 1, 99);
    const uint32_t otherIp2 = ip(192, 168, 1, 100);
    Rig r;
    const LinkEndpoint pc = ep(otherIp, 40020);
    r.allowInConfig(20);
    CHECK(r.hello(20, pc, 0.0).status == PeerStatus::Allowed);
    CHECK(r.subscribe(20, pc, 0.0).status == PeerStatus::Allowed);  // leaseUntil = kLeaseSec (6 s)

    // accepted: in order, from the lease address
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 1), pc, 0.5);
    CHECK(r.server.malformed() == 0 && r.env.pcMetricsCalls.size() == 1);
    CHECK(r.env.pcMetricsCalls[0].snapshotId == 1);

    // a repeated or older snapshot id is a silent reorder drop, not malformed
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 1), pc, 0.6);
    CHECK(r.env.pcMetricsCalls.size() == 1);
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 0), pc, 0.7);
    CHECK(r.env.pcMetricsCalls.size() == 1);

    // a newer snapshot id is accepted
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 2), pc, 0.8);
    CHECK(r.env.pcMetricsCalls.size() == 2 && r.env.pcMetricsCalls[1].snapshotId == 2);

    // from a different address than the lease holds: dropped (the lease is bound to one endpoint)
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 5), ep(otherIp2, 40099), 0.9);
    CHECK(r.env.pcMetricsCalls.size() == 2 && r.server.malformed() == 0);

    // past the lease (SUBSCRIBE was at t=0, kLeaseSec later it's gone): dropped
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 5), pc, kLeaseSec + 1.0);
    CHECK(r.env.pcMetricsCalls.size() == 2 && r.server.malformed() == 0);

    // re-subscribing renews the lease; the snapshot ordering keeps counting from where it left off
    CHECK(r.subscribe(20, pc, kLeaseSec + 1.0).status == PeerStatus::Allowed);
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 5), pc, kLeaseSec + 1.1);
    CHECK(r.env.pcMetricsCalls.size() == 3 && r.env.pcMetricsCalls[2].snapshotId == 5);

    // a pending (not yet allowed) PC gets no PC_METRICS acceptance even from a consistent address
    const LinkEndpoint pending = ep(ip(192, 168, 1, 150), 41000);
    CHECK(r.hello(21, pending, kLeaseSec + 1.2).status == PeerStatus::Pending);
    r.send(PacketType::PcMetrics, pcMetricsBytes(21, 1), pending, kLeaseSec + 1.3);
    CHECK(r.env.pcMetricsCalls.size() == 3 && r.server.malformed() == 0);

    // quick restart: the PC says HELLO again (a new process), so its snapshot counter resets; a *lower*
    // snapshot id than it last sent is accepted again instead of reading as a stale duplicate
    const double restartAt = kLeaseSec + 2.0;
    CHECK(r.hello(20, pc, restartAt).status == PeerStatus::Allowed);
    CHECK(r.subscribe(20, pc, restartAt).status == PeerStatus::Allowed);
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 1), pc, restartAt + 0.1);
    CHECK(r.env.pcMetricsCalls.size() == 4 && r.env.pcMetricsCalls[3].snapshotId == 1);
    // ... and ordering resumes normally from there (the same id again is a dup)
    r.send(PacketType::PcMetrics, pcMetricsBytes(20, 1), pc, restartAt + 0.2);
    CHECK(r.env.pcMetricsCalls.size() == 4);
}

/** Builds a PC_METRICS payload without encodePcMetrics's registry validation, so it can carry ids this build
 *  doesn't know about (standing in for a newer PC build / mixed-version peer). */
Bytes rawPcMetricsBytes(uint8_t seed, uint32_t snapshotId, const std::vector<WireValue>& values) {
    Writer w;
    w.id(pcId(seed));
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
            case fph::WireType::F32: w.f32(v.f32); break;
            case fph::WireType::I32: w.i32(v.i32); break;
            case fph::WireType::Enum: w.u8(v.enumValue); break;
            case fph::WireType::ShortString: w.str8(v.text, fph::kShortStringMax); break;
        }
    }
    return w.bytes();
}

void testPcMetricsUnknownIdsFromNewerPc() {
    // this (installed) headset build doesn't know some of the PC's fields yet (e.g. it predates the
    // Link0-3Address ids): the gate still accepts the packet, applies every known field, and counts the rest
    // via a separate debug counter rather than rejecting the whole snapshot
    const uint32_t otherIp = ip(192, 168, 1, 199);
    Rig r;
    const LinkEndpoint pc = ep(otherIp, 40030);
    r.allowInConfig(30);
    CHECK(r.hello(30, pc, 0.0).status == PeerStatus::Allowed);
    CHECK(r.subscribe(30, pc, 0.0).status == PeerStatus::Allowed);

    fph::link::WireValue known;
    known.id = fph::metric::kPcStreamPing;
    known.type = fph::WireType::F32;
    known.ok = true;
    known.f32 = 2.4f;
    fph::link::WireValue unknown;
    unknown.id = 65000;  // stands in for a real id this build's registry doesn't carry yet
    unknown.type = fph::WireType::ShortString;
    unknown.ok = true;
    unknown.text = "10.35.78.1";

    const uint64_t before = r.server.unknownMetricValues();
    r.send(PacketType::PcMetrics, rawPcMetricsBytes(30, 1, {known, unknown}), pc, 0.5);
    CHECK(r.server.malformed() == 0);
    CHECK(r.env.pcMetricsCalls.size() == 1);
    CHECK(r.env.pcMetricsCalls[0].values.size() == 1);
    CHECK(r.env.pcMetricsCalls[0].values[0].id == fph::metric::kPcStreamPing);
    CHECK(r.env.pcMetricsCalls[0].unknownValues == 1);
    CHECK(r.server.unknownMetricValues() == before + 1);
}

// ---- vrlink host IPs ----

std::string cmdlineOf(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& a : args) {
        out += a;
        out += '\0';
    }
    return out;
}

std::vector<uint32_t> parse(const std::string& cmdline) {
    return parseVrlinkHostIps(cmdline.data(), cmdline.size());
}

void testParseVrlink() {
    const std::string real = cmdlineOf({"vrlink", "-A", "438100", "-c", "192.168.1.45", "-e", "SECRET-abc~10400~10400",
                                        "-c", "10.35.78.24"});
    std::vector<uint32_t> ips = parse(real);
    CHECK(ips.size() == 2 && ips[0] == ip(192, 168, 1, 45) && ips[1] == ip(10, 35, 78, 24));
    // the secret is never mistaken for a host, and nothing else is returned
    CHECK(parse(cmdlineOf({"vrlink", "-e", "SECRET-abc~10400~10400"})).empty());

    ips = parse(cmdlineOf({"vrlink", "-c=1.2.3.4"}));
    CHECK(ips.size() == 1 && ips[0] == ip(1, 2, 3, 4));
    ips = parse(cmdlineOf({"vrlink", "-c", "1.2.3.4:5678"}));
    CHECK(ips.size() == 1 && ips[0] == ip(1, 2, 3, 4));
    ips = parse(cmdlineOf({"vrlink", "-c", "1.2.3.4,5.6.7.8:9"}));
    CHECK(ips.size() == 2 && ips[1] == ip(5, 6, 7, 8));
    ips = parse(cmdlineOf({"vrlink", "-c", "1.2.3.4;5.6.7.8"}));
    CHECK(ips.size() == 2);
    // junk values skipped, good ones kept, duplicates removed
    ips = parse(cmdlineOf({"vrlink", "-c", "myhost.local", "-c", "1.2.3", "-c", "256.1.1.1", "-c", "", "-c", "9.9.9.9",
                           "-c", "9.9.9.9"}));
    CHECK(ips.size() == 1 && ips[0] == ip(9, 9, 9, 9));
    // "-c" last with no value; a value that looks like a flag; other options that merely contain "-c"
    CHECK(parse(cmdlineOf({"vrlink", "-c"})).empty());
    CHECK(parse(cmdlineOf({"vrlink", "-cc", "1.2.3.4", "--c", "1.2.3.4", "1.2.3.4"})).empty());
    // no trailing NUL, empty input
    ips = parse("vrlink");
    CHECK(ips.empty());
    const std::string noNul = std::string("vrlink") + '\0' + "-c" + '\0' + "7.7.7.7";
    CHECK(parse(noNul).size() == 1);
    CHECK(parseVrlinkHostIps("", 0).empty());
}

/** A fake /proc: 100 = the real vrlink, 200 = comm vrlink-ish but another exe, 300 = bash, 400 = deleted exe. */
std::string makeProcTree() {
    const std::string root = tempDir + "/proc";
    fph::makeDirectories(root);
    const auto proc = [&](const char* pid, const std::string& comm, const std::string& exe, const std::string& cmdline) {
        const std::string dir = root + "/" + pid;
        fph::makeDirectories(dir);
        writeFile(dir + "/comm", comm);
        writeFile(dir + "/cmdline", cmdline);
        if (!exe.empty()) CHECK(::symlink(exe.c_str(), (dir + "/exe").c_str()) == 0);
    };
    proc("100", "vrlinkrunthread\n", "/home/steamos/.steam/vrlink/bin/vrlink",
         cmdlineOf({"/x/vrlink", "-A", "438100", "-c", "192.168.1.45", "-e", "SECRET-abc~10400~10400", "-c",
                    "10.35.78.24"}));
    proc("200", "vrlink-fake\n", "/usr/bin/not-vrlink", cmdlineOf({"fake", "-c", "9.9.9.9", "-e", "SECRET-other"}));
    proc("300", "bash\n", "/usr/bin/bash", cmdlineOf({"bash", "-c", "8.8.8.8"}));
    proc("400", "vrlink\n", "/gone/vrlink (deleted)", cmdlineOf({"vrlink", "-c", "10.0.0.4"}));
    fph::makeDirectories(root + "/self");  // not a pid
    writeFile(root + "/self/comm", "vrlink\n");
    return root;
}

void testReadVrlink() {
    const std::string root = makeProcTree();
    StderrCapture capture(tempDir + "/stderr-secret.txt");

    const std::vector<uint32_t> ips = readVrlinkHostIps(root);
    CHECK(ips.size() == 3);
    const auto has = [&](uint32_t v) { return std::find(ips.begin(), ips.end(), v) != ips.end(); };
    CHECK(has(ip(192, 168, 1, 45)) && has(ip(10, 35, 78, 24)) && has(ip(10, 0, 0, 4)));
    CHECK(!has(ip(9, 9, 9, 9)) && !has(ip(8, 8, 8, 8)));  // a comm that merely starts with vrlink, and bash
    CHECK(readVrlinkHostIps(tempDir + "/no-such-proc").empty());
    const std::string emptyRoot = tempDir + "/emptyproc";
    fph::makeDirectories(emptyRoot);
    CHECK(readVrlinkHostIps(emptyRoot).empty());

    // end to end through the server: a HELLO whose allow decision reads the fixture's cmdline
    Rig r;
    r.env.hostIpFn = [&]() { return readVrlinkHostIps(root); };
    CHECK(r.hello(1, ep(ip(10, 35, 78, 24), 4000), 0.0, "Dongle PC").status == PeerStatus::Allowed);
    CHECK(r.hello(2, ep(ip(9, 9, 9, 9), 4000), 0.0, "Stranger").status == PeerStatus::Pending);
    r.server.tick(1.0);
    const std::string debug = r.server.debugText(1.0);

    const std::string err = capture.text();
    capture.stop();
    CHECK(err.find("SECRET") == std::string::npos);
    CHECK(err.find("abc~10400") == std::string::npos);
    CHECK(debug.find("SECRET") == std::string::npos);
    CHECK(!err.empty());  // the server did log the HELLOs: the capture itself works
}

// ---- lease ----

void testLease() {
    const uint32_t pcIp = ip(192, 168, 1, 45);
    const LinkEndpoint helloFrom = ep(pcIp, 40000);
    const LinkEndpoint subFrom = ep(pcIp, 40001);
    {
        Rig r;
        // SUBSCRIBE before HELLO
        const SubscribeAck early = r.subscribe(1, subFrom, 0.0);
        CHECK(early.status == PeerStatus::UnknownSubscriber && !r.server.hasLease());
        r.allowInConfig(1);
        CHECK(r.hello(1, helloFrom, 0.5).status == PeerStatus::Allowed);
        CHECK(!r.server.hasLease());
        r.server.tick(0.6);
        CHECK(r.take().empty());  // no lease: nothing sent, nothing sampled
        const SubscribeAck ok = r.subscribe(1, subFrom, 1.0);
        CHECK(ok.status == PeerStatus::Allowed && ok.leaseMs == 6000 && r.server.hasLease());

        // tick: METRICS go to the SUBSCRIBE's source, LINK_STATE carries the layout version
        r.server.tick(1.5);
        std::vector<Sent> out = r.take();
        const std::vector<Sent> metrics = ofType(out, PacketType::Metrics);
        CHECK(!metrics.empty() && metrics.size() == out.size());
        for (const Sent& s : metrics) {
            CHECK(s.to == subFrom);
            Metrics m;
            CHECK(decodeMetrics(s.payload.data(), s.payload.size(), m));
            CHECK(m.state.layoutVersion == r.auth.version() && m.state.sessionId == 7 && m.state.appName == "Some Game");
            CHECK(m.values.size() == 3 && m.headsetMs == 1500);
        }
        CHECK(r.server.metricsSent() == 1);
        CHECK(r.env.processListCalls == 0);  // metrics only: the process scan isn't asked for

        // renewals every 2 s keep it
        for (double t = 3.0; t <= 9.0; t += 2.0) {
            CHECK(r.subscribe(1, subFrom, t).status == PeerStatus::Allowed);
            r.server.tick(t + 1.0);
            CHECK(!ofType(r.take(), PacketType::Metrics).empty());
        }
        // no renewal after t=9: still sending at 14.9, gone at the first tick after 15
        r.server.tick(14.9);
        CHECK(!ofType(r.take(), PacketType::Metrics).empty() && r.server.hasLease());
        r.server.tick(15.1);
        CHECK(!r.server.hasLease() && r.take().empty());
        r.server.tick(15.6);
        CHECK(r.take().empty());
        // it can come back (the peer is still remembered)
        CHECK(r.subscribe(1, subFrom, 16.0).status == PeerStatus::Allowed && r.server.hasLease());
        // after 15 s of silence the headset forgets it: SUBSCRIBE is unknown_subscriber
        r.server.tick(40.0);
        CHECK(!r.server.hasLease());
        CHECK(r.subscribe(1, subFrom, 41.0).status == PeerStatus::UnknownSubscriber);
    }
    {
        // exactly at the deadline is still alive; the lease is 6 s from the last SUBSCRIBE
        Rig r;
        r.allowInConfig(1);
        r.hello(1, helloFrom, 0.0);
        r.subscribe(1, subFrom, 10.0);
        r.server.tick(15.9);
        CHECK(r.server.hasLease());
        r.take();
        r.server.tick(16.1);
        CHECK(!r.server.hasLease() && r.take().empty());
    }
    {
        // BYE ends it at once
        Rig r;
        r.allowInConfig(1);
        r.hello(1, helloFrom, 0.0);
        r.subscribe(1, subFrom, 0.0);
        CHECK(r.server.hasLease());
        r.bye(1, subFrom, 0.5);
        CHECK(!r.server.hasLease());
        r.server.tick(1.0);
        CHECK(r.take().empty());
        // a BYE from a PC we never heard of is harmless
        r.bye(44, subFrom, 1.0);
        CHECK(r.take().empty());
    }
    {
        // a second SUBSCRIBE from a new address moves the data there: one lease per PC id
        Rig r;
        r.allowInConfig(1);
        r.hello(1, helloFrom, 0.0);
        const LinkEndpoint a = ep(pcIp, 40001), b = ep(ip(10, 35, 78, 24), 40002);
        r.subscribe(1, a, 0.0);
        r.server.tick(0.5);
        CHECK(!ofType(r.take(), PacketType::Metrics).empty());
        r.subscribe(1, b, 1.0);
        r.server.tick(1.5);
        const std::vector<Sent> out = r.take();
        const std::vector<Sent> metrics = ofType(out, PacketType::Metrics);
        CHECK(!metrics.empty());
        size_t toA = 0;
        for (const Sent& s : metrics) {
            if (s.to == a) ++toA;
            CHECK(s.to == b);
        }
        CHECK(toA == 0);
        CHECK(r.server.metricsSent() == 2);  // one snapshot per tick for the one lease
        // two PCs = two leases, each gets the snapshot
        r.allowInConfig(2);
        r.hello(2, ep(pcIp, 41000), 2.0);
        r.subscribe(2, ep(pcIp, 41001), 2.0);
        r.subscribe(1, b, 2.0);
        r.server.tick(2.5);
        std::vector<Sent> two = ofType(r.take(), PacketType::Metrics);
        size_t toB = 0, toC = 0;
        for (const Sent& s : two) {
            toB += s.to == b;
            toC += s.to == ep(pcIp, 41001);
        }
        CHECK(toB > 0 && toB == toC && toB + toC == two.size());
    }
    {
        // a pending PC's SUBSCRIBE gets pending and no lease
        Rig r;
        CHECK(r.hello(1, helloFrom, 0.0).status == PeerStatus::Pending);
        CHECK(r.subscribe(1, subFrom, 0.5).status == PeerStatus::Pending);
        CHECK(!r.server.hasLease());
        r.server.tick(1.0);
        CHECK(r.take().empty());
        r.server.allow(idToHex(pcId(1)));
        CHECK(r.subscribe(1, subFrom, 1.5).status == PeerStatus::Allowed && r.server.hasLease());
        // being denied later ends the lease
        r.server.deny(idToHex(pcId(1)));
        CHECK(!r.server.hasLease());
        CHECK(r.subscribe(1, subFrom, 2.0).status == PeerStatus::Denied);
    }
    {
        // process lists: BLOBs only when the generation changes, only for wanted streams
        Rig r;
        r.allowInConfig(1);
        r.allowInConfig(2);
        r.hello(1, helloFrom, 0.0);
        r.hello(2, ep(pcIp, 41000), 0.0);
        r.env.generation = 5;
        r.subscribe(1, subFrom, 0.0, kWantMetrics | kWantCpuList);
        r.subscribe(2, ep(pcIp, 41001), 0.0, kWantRamList);  // lists only, no metrics
        r.server.tick(0.5);
        std::vector<Sent> out = r.take();
        struct Seen {
            int blobs = 0, metrics = 0;
            std::vector<int> streams;
            BlobAssembler asmb;
            ProcessListWire list;
        };
        const auto scan = [&](const std::vector<Sent>& sent, const LinkEndpoint& to) {
            Seen seen;
            for (const Sent& s : sent) {
                if (!(s.to == to)) continue;
                if (s.header.type == PacketType::Metrics) ++seen.metrics;
                if (s.header.type != PacketType::Blob) continue;
                BlobChunk chunk;
                CHECK(decodeBlobChunk(s.payload.data(), s.payload.size(), chunk));
                CHECK(chunk.kind == BlobKind::ProcessList);
                ++seen.blobs;
                seen.streams.push_back(chunk.stream);
                Blob blob;
                if (seen.asmb.add(1, chunk, 0.0, blob)) CHECK(decodeProcessList(blob.data.data(), blob.data.size(), seen.list));
            }
            return seen;
        };
        Seen one = scan(out, subFrom);
        CHECK(one.metrics >= 1 && one.blobs == 1 && one.streams[0] == kStreamCpuList);
        CHECK(one.list.generation == 5 && one.list.cpuValid && one.list.rows.size() == 1 &&
              one.list.rows[0].name == "vrserver");
        Seen two = scan(out, ep(pcIp, 41001));
        CHECK(two.metrics == 0 && two.blobs == 1 && two.streams[0] == kStreamRamList);
        CHECK(two.list.rows.size() == 1 && two.list.rows[0].name == "chrome");
        CHECK(r.env.processListCalls == 1);  // one scan read per tick, shared

        // same generation: no new blobs, METRICS keep coming
        r.subscribe(1, subFrom, 1.0, kWantMetrics | kWantCpuList);
        r.subscribe(2, ep(pcIp, 41001), 1.0, kWantRamList);
        r.server.tick(1.5);
        out = r.take();
        CHECK(scan(out, subFrom).blobs == 0 && scan(out, subFrom).metrics >= 1);
        CHECK(scan(out, ep(pcIp, 41001)).blobs == 0);
        // generation moves: both get a new list again
        r.env.generation = 6;
        r.server.tick(2.0);
        out = r.take();
        CHECK(scan(out, subFrom).blobs == 1 && scan(out, ep(pcIp, 41001)).blobs == 1);
        CHECK(scan(out, subFrom).list.generation == 6);
        // no list wanted by anybody: the scan isn't read at all
        Rig q;
        q.allowInConfig(1);
        q.hello(1, helloFrom, 0.0);
        q.env.generation = 3;
        q.subscribe(1, subFrom, 0.0, kWantMetrics);
        q.server.tick(0.5);
        CHECK(q.env.processListCalls == 0 && scan(q.take(), subFrom).blobs == 0);
        // generation 0 = no lists yet: nothing sent
        Rig z;
        z.allowInConfig(1);
        z.hello(1, helloFrom, 0.0);
        z.env.generation = 0;
        z.subscribe(1, subFrom, 0.0, kWantCpuList | kWantRamList);
        z.server.tick(0.5);
        CHECK(scan(z.take(), subFrom).blobs == 0);
        // a re-subscribe after the lease lapsed gets the current lists again
        r.server.tick(30.0);
        r.hello(1, helloFrom, 31.0);
        r.subscribe(1, subFrom, 31.0, kWantMetrics | kWantCpuList);
        r.take();
        r.server.tick(31.5);
        CHECK(scan(r.take(), subFrom).blobs == 1);
    }
    {
        // sayGoodbye: BYE with the headset id to every lease holder
        Rig r;
        r.allowInConfig(1);
        r.hello(1, helloFrom, 0.0);
        r.subscribe(1, subFrom, 0.0);
        r.server.sayGoodbye();
        const std::vector<Sent> out = r.take();
        CHECK(out.size() == 1 && out[0].header.type == PacketType::Bye && out[0].to == subFrom);
        Bye b;
        CHECK(decodeBye(out[0].payload.data(), out[0].payload.size(), b) && idToHex(b.id) == r.config.link.headsetId);
    }
}

// ---- layout compare-and-set ----

void testLayoutSet() {
    const uint32_t pcIp = ip(192, 168, 1, 45);
    const LinkEndpoint pc = ep(pcIp, 40000);
    const std::string json = fph::layoutToJson(modLayout(1), 0);
    {
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        const fph::Layout original = r.config.layout;
        CHECK(r.auth.version() == 1);

        // base == current: ACK with version + 1, applied once, saved
        r.layoutSet(1, pc, 10, 1, json, 1, 1.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].requestId == 10 && a[0].version == 2 && a[0].to == pc);
        CHECK(r.applies == 1 && r.layoutSaves == 1 && r.server.layoutApplies() == 1);
        CHECK(r.auth.version() == 2 && r.config.layout.sameWidgets(modLayout(1)) && !r.config.layout.sameWidgets(original));
        CHECK(r.lastApplied.sameWidgets(modLayout(1)) && r.lastApplied.version == 2);

        // the ACK is "lost": the PC resends (new blob id) -> the same answer, not applied again
        r.layoutSet(1, pc, 10, 1, json, 2, 2.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].requestId == 10 && a[0].version == 2);
        CHECK(r.applies == 1 && r.layoutSaves == 1 && r.auth.version() == 2);
        // ...and again, and with a different layout body under the same request id: still the remembered answer
        r.layoutSet(1, pc, 10, 1, fph::layoutToJson(modLayout(5), 0), 3, 2.5);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 2 && r.applies == 1);
        CHECK(r.config.layout.sameWidgets(modLayout(1)));

        // stale base: REJECT stale with the current version, nothing applied
        r.layoutSet(1, pc, 11, 1, fph::layoutToJson(modLayout(2), 0), 4, 3.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::Stale && a[0].version == 2 &&
              a[0].requestId == 11);
        CHECK(r.applies == 1 && r.layoutSaves == 1 && r.config.layout.sameWidgets(modLayout(1)));
        // the stale answer is remembered as well (a resend gets the same REJECT)
        r.layoutSet(1, pc, 11, 2, fph::layoutToJson(modLayout(2), 0), 5, 3.5);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::Stale && r.applies == 1);
        // a fresh request on the new base goes through
        r.layoutSet(1, pc, 12, 2, fph::layoutToJson(modLayout(2), 0), 6, 4.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 3 && r.applies == 2);

        // the version METRICS carry follows
        r.subscribe(1, pc, 4.0);
        r.server.tick(4.5);
        for (const Sent& s : ofType(r.take(), PacketType::Metrics)) {
            Metrics m;
            CHECK(decodeMetrics(s.payload.data(), s.payload.size(), m) && m.state.layoutVersion == 3);
        }
    }
    {
        // two PCs racing on the same base: one ACK, one REJECT stale, applied once
        Rig r;
        r.allowInConfig(1);
        r.allowInConfig(2);
        const LinkEndpoint pcB = ep(ip(192, 168, 1, 50), 41000);
        r.hello(1, pc, 0.0);
        r.hello(2, pcB, 0.0);
        r.layoutSet(1, pc, 1, 1, fph::layoutToJson(modLayout(1), 0), 1, 1.0);
        r.layoutSet(2, pcB, 1, 1, fph::layoutToJson(modLayout(2), 0), 1, 1.0);  // same request id, other PC
        const std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 2);
        CHECK(a.size() == 2 && a[0].ack && a[0].to == pc && a[0].version == 2);
        CHECK(a.size() == 2 && !a[1].ack && a[1].to == pcB && a[1].reason == RejectReason::Stale && a[1].version == 2);
        CHECK(r.applies == 1 && r.config.layout.sameWidgets(modLayout(1)));
    }
    {
        // only the last 8 request ids are remembered
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        for (uint32_t i = 0; i < 9; ++i) {
            r.layoutSet(1, pc, 100 + i, 1 + i, fph::layoutToJson(modLayout(static_cast<int>(i)), 0), 1 + i, 1.0);
            const std::vector<Answer> a = answersIn(r.take());
            CHECK(a.size() == 1 && a[0].ack && a[0].version == 2 + i);
        }
        CHECK(r.applies == 9 && r.auth.version() == 10);
        // 108 (newest): remembered
        r.layoutSet(1, pc, 108, 9, json, 20, 2.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 10 && r.applies == 9);
        // 101 (oldest of the last 8): remembered
        r.layoutSet(1, pc, 101, 2, json, 21, 2.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 3 && r.applies == 9);
        // 100 fell off: it's judged again, and its base is stale now
        r.layoutSet(1, pc, 100, 1, json, 22, 2.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::Stale && a[0].version == 10);
        CHECK(r.applies == 9);
    }
    {
        // invalid JSON
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        const fph::Layout original = r.config.layout;
        StderrCapture quiet(tempDir + "/quiet.txt");
        r.layoutSet(1, pc, 1, 1, "this is not json", 1, 1.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::InvalidLayout && a[0].version == 1);
        r.layoutSet(1, pc, 2, 1, "{\"columns\":5}", 2, 1.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::InvalidLayout);
        r.layoutSet(1, pc, 3, 1, "", 3, 1.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].reason == RejectReason::InvalidLayout);
        CHECK(r.applies == 0 && r.layoutSaves == 0 && r.config.layout.sameWidgets(original) && r.auth.version() == 1);
    }
    {
        // save failing: REJECT save_failed, layout unchanged
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        const fph::Layout original = r.config.layout;
        r.layoutSaveOk = false;
        StderrCapture quiet(tempDir + "/quiet.txt");
        r.layoutSet(1, pc, 1, 1, json, 1, 1.0);
        const std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::SaveFailed && a[0].version == 1);
        CHECK(r.applies == 0 && r.auth.version() == 1 && r.config.layout.sameWidgets(original));
        // once the disk is back, a new request (new id) works
        r.layoutSaveOk = true;
        r.layoutSet(1, pc, 2, 1, json, 2, 2.0);
        const std::vector<Answer> b = answersIn(r.take());
        CHECK(b.size() == 1 && b[0].ack && b[0].version == 2 && r.applies == 1);
    }
    {
        // unknown PC / pending PC / denied PC
        Rig r;
        r.layoutSet(1, pc, 1, 1, json, 1, 0.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::UnknownSubscriber && a[0].to == pc);
        r.hello(1, pc, 1.0);  // pending
        r.layoutSet(1, pc, 2, 1, json, 2, 1.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && !a[0].ack && a[0].reason == RejectReason::NotAllowed && a[0].version == 1);
        CHECK(r.applies == 0 && r.layoutSaves == 0);
    }
    {
        // a multi-chunk LAYOUT_SET (padded JSON) goes through; losing a chunk for over a second drops it
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        const std::string padded = json + std::string(3000, ' ');
        r.layoutSet(1, pc, 1, 1, padded, 1, 1.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 2);

        LayoutSet m;
        m.pcId = pcId(1);
        m.requestId = 2;
        m.baseVersion = 2;
        m.layoutJson = fph::layoutToJson(modLayout(3), 0) + std::string(3000, ' ');
        const std::vector<Bytes> chunks = splitBlob(2, BlobKind::LayoutSet, 0, encodeLayoutSet(m));
        CHECK(chunks.size() >= 3);
        r.send(PacketType::Blob, chunks[0], pc, 2.0);
        r.server.tick(3.5);  // the half-received blob expired
        for (size_t i = 1; i < chunks.size(); ++i) r.send(PacketType::Blob, chunks[i], pc, 3.5);
        CHECK(answersIn(r.take()).empty() && r.applies == 1);
        // a layout needing more than 16 chunks is dropped without an answer
        LayoutSet huge = m;
        huge.requestId = 3;
        huge.layoutJson = fph::layoutToJson(modLayout(3), 0) + std::string(20000, ' ');
        for (const Bytes& c : splitBlob(9, BlobKind::LayoutSet, 0, encodeLayoutSet(huge))) {
            r.send(PacketType::Blob, c, pc, 5.0);
        }
        CHECK(answersIn(r.take()).empty() && r.applies == 1 && r.server.malformed() > 0);
    }
    {
        // a restarted PC app (same id, same address:port, blob ids from 1 again): its HELLO resets the blob state
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        r.layoutSet(1, pc, 1, 1, json, 100, 1.0);
        std::vector<Answer> a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].version == 2);
        // without a HELLO, blob id 1 from the same address looks older than 100 and is dropped: no answer
        r.layoutSet(1, pc, 2, 2, fph::layoutToJson(modLayout(2), 0), 1, 2.0);
        CHECK(answersIn(r.take()).empty() && r.applies == 1);
        // after the HELLO the same request goes through
        r.hello(1, pc, 3.0);
        r.layoutSet(1, pc, 2, 2, fph::layoutToJson(modLayout(2), 0), 1, 3.0);
        a = answersIn(r.take());
        CHECK(a.size() == 1 && a[0].ack && a[0].requestId == 2 && a[0].version == 3 && r.applies == 2);
    }
    {
        // LAYOUT_GET: answered with a Layout BLOB whose JSON parses back to the current layout
        Rig r;
        r.allowInConfig(1);
        r.hello(1, pc, 0.0);
        r.layoutSet(1, pc, 1, 1, json, 1, 1.0);
        r.take();
        LayoutGet get;
        get.pcId = pcId(1);
        r.send(PacketType::LayoutGet, encodeLayoutGet(get), pc, 2.0);
        const std::vector<Sent> out = r.take();
        BlobAssembler asmb;
        Blob blob;
        bool done = false;
        for (const Sent& s : out) {
            CHECK(s.header.type == PacketType::Blob && s.to == pc);
            BlobChunk chunk;
            CHECK(decodeBlobChunk(s.payload.data(), s.payload.size(), chunk));
            CHECK(chunk.kind == BlobKind::Layout);
            done |= asmb.add(1, chunk, 0.0, blob);
        }
        CHECK(done && blob.kind == BlobKind::Layout);
        fph::Layout parsed;
        std::vector<std::string> warnings;
        std::string error;
        CHECK(fph::parseLayoutText(std::string(blob.data.begin(), blob.data.end()), parsed, warnings, error));
        CHECK(parsed.version == 2 && parsed.sameWidgets(r.config.layout) && parsed.sameWidgets(modLayout(1)));
        // strangers and pending PCs get nothing
        LayoutGet stranger;
        stranger.pcId = pcId(50);
        r.send(PacketType::LayoutGet, encodeLayoutGet(stranger), pc, 2.0);
        CHECK(r.take().empty());
        r.hello(2, ep(ip(192, 168, 1, 77), 5), 2.0);  // pending
        r.take();
        get.pcId = pcId(2);
        r.send(PacketType::LayoutGet, encodeLayoutGet(get), ep(ip(192, 168, 1, 77), 5), 2.0);
        CHECK(r.take().empty());
    }
}

// ---- LayoutAuthority on its own ----

void testLayoutAuthority() {
    Config config;
    int saves = 0, applies = 0;
    bool saveOk = true;
    uint32_t savedVersion = 0, liveVersionDuringSave = 0;
    fph::Layout applied;
    LayoutAuthority auth(
        config,
        [&](const Config& next, std::string& error) {
            ++saves;
            savedVersion = next.layout.version;
            liveVersionDuringSave = config.layout.version;
            if (!saveOk) {
                error = "disk full";
                return false;
            }
            return true;
        },
        [&](const fph::Layout& l) {
            ++applies;
            applied = l;
        });
    CHECK(auth.version() == 1);

    // applied; the incoming layout's own version is ignored
    fph::Layout edit = modLayout(2);
    edit.version = 99;
    LayoutAuthority::Result r = auth.trySetLayout(1, edit);
    CHECK(r.applied() && r.outcome == LayoutAuthority::Outcome::Applied && r.version == 2);
    CHECK(auth.version() == 2 && config.layout.version == 2 && config.layout.sameWidgets(edit));
    CHECK(saves == 1 && applies == 1 && savedVersion == 2 && applied.version == 2 && applied.sameWidgets(edit));
    CHECK(liveVersionDuringSave == 1);  // saved from a copy before the live config changed
    CHECK(&auth.layout() == &config.layout);

    // stale: reports the current version, no save, no apply
    r = auth.trySetLayout(1, modLayout(4));
    CHECK(!r.applied() && r.outcome == LayoutAuthority::Outcome::Stale && r.version == 2);
    r = auth.trySetLayout(3, modLayout(4));  // a base from the future is stale too
    CHECK(r.outcome == LayoutAuthority::Outcome::Stale && r.version == 2);
    CHECK(saves == 1 && applies == 1 && config.layout.sameWidgets(edit));

    // save failed: nothing changes
    saveOk = false;
    {
        StderrCapture quiet(tempDir + "/quiet.txt");
        r = auth.trySetLayout(2, modLayout(5));
    }
    CHECK(!r.applied() && r.outcome == LayoutAuthority::Outcome::SaveFailed && r.version == 2 && r.error == "disk full");
    CHECK(auth.version() == 2 && config.layout.sameWidgets(edit) && applies == 1 && saves == 2);
    saveOk = true;
    r = auth.trySetLayout(2, modLayout(5));
    CHECK(r.applied() && r.version == 3 && applies == 2);

    // a config reload that replaces the layout is picked up (the authority only keeps a reference)
    config.layout.version = 40;
    CHECK(auth.version() == 40);
    CHECK(auth.trySetLayout(3, modLayout(1)).outcome == LayoutAuthority::Outcome::Stale);
    CHECK(auth.trySetLayout(40, modLayout(1)).version == 41);

    // no save / apply callbacks: still applies
    Config bare;
    LayoutAuthority plain(bare, nullptr, nullptr);
    CHECK(plain.trySetLayout(1, modLayout(1)).version == 2 && bare.layout.version == 2);
}

// ---- config: the "link" key ----

void testConfigLink() {
    const std::string dir = tempDir + "/cfg";
    const std::string path = dir + "/config.json";
    std::string error;
    std::vector<std::string> warnings;

    Config c;
    c.link.port = 50123;
    c.link.headsetId = "00112233445566778899aabbccddeeff";
    const std::string nasty = "Jack's \"PC\"\nline2 \\ back\ttab \xC3\xA9\xE2\x82\xAC";
    c.link.allowedPcs = {{"ffeeddccbbaa99887766554433221100", "Plain PC"},
                         {"0123456789abcdef0123456789abcdef", nasty}};
    CHECK(saveConfig(path, c, error));
    Config loaded;
    CHECK(loadConfig(path, loaded, warnings, error));  // the file still parses
    CHECK(loaded.link.port == 50123 && loaded.link.headsetId == c.link.headsetId);
    CHECK(loaded.link.allowedPcs.size() == 2);
    if (loaded.link.allowedPcs.size() == 2) {
        CHECK(loaded.link.allowedPcs[0].id == "ffeeddccbbaa99887766554433221100" &&
              loaded.link.allowedPcs[0].name == "Plain PC");
        CHECK(loaded.link.allowedPcs[1].id == "0123456789abcdef0123456789abcdef");
        // quotes, backslashes and UTF-8 survive; control characters (newline, tab) are dropped on write
        CHECK(loaded.link.allowedPcs[1].name == "Jack's \"PC\"line2 \\ backtab \xC3\xA9\xE2\x82\xAC");
    }
    // the rest of the config is untouched by the link key
    CHECK(loaded.layoutInFile && loaded.layout.sameWidgets(c.layout));
    // saving what was loaded gives the same thing back
    CHECK(saveConfig(dir + "/again.json", loaded, error));
    Config again;
    CHECK(loadConfig(dir + "/again.json", again, warnings, error) && again.link.allowedPcs.size() == 2 &&
          again.link.allowedPcs[1].name == loaded.link.allowedPcs[1].name);

    // defaults: no "link" key at all
    writeFile(dir + "/nolink.json", "{ \"alpha\": 0.5 }");
    Config none;
    warnings.clear();
    CHECK(loadConfig(dir + "/nolink.json", none, warnings, error));
    CHECK(none.link.port == 47630 && none.link.headsetId.empty() && none.link.allowedPcs.empty());
    // an empty allowed list round-trips
    Config empty;
    CHECK(saveConfig(dir + "/empty.json", empty, error));
    Config emptyBack;
    CHECK(loadConfig(dir + "/empty.json", emptyBack, warnings, error) && emptyBack.link.allowedPcs.empty() &&
          emptyBack.link.port == 47630);

    // bad entries skipped with warnings, the rest kept
    writeFile(dir + "/bad.json", R"({
  "link": {
    "port": 80,
    "headset_id": "NOT-A-HEX-ID",
    "bogus": 1,
    "allowed_pcs": [
      { "id": "zz" },
      { "name": "no id" },
      5,
      "text",
      { "id": "0123456789ABCDEF0123456789ABCDEF", "name": "upper case" },
      { "id": 12345, "name": "number id" },
      { "id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "name": "good" },
      { "id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" }
    ]
  }
})");
    Config bad;
    warnings.clear();
    CHECK(loadConfig(dir + "/bad.json", bad, warnings, error));
    CHECK(bad.link.headsetId.empty());  // not 32 lowercase hex: dropped, a new one is made at start
    CHECK(bad.link.port >= 1024);       // out of range is rounded into range
    CHECK(bad.link.allowedPcs.size() == 2);
    if (bad.link.allowedPcs.size() == 2) {
        CHECK(bad.link.allowedPcs[0].id == std::string(32, 'a') && bad.link.allowedPcs[0].name == "good");
        CHECK(bad.link.allowedPcs[1].id == std::string(32, 'b') && bad.link.allowedPcs[1].name.empty());
    }
    CHECK(warnings.size() >= 6);

    // wrong shapes
    writeFile(dir + "/shape1.json", R"({ "link": 5 })");
    Config s1;
    warnings.clear();
    CHECK(loadConfig(dir + "/shape1.json", s1, warnings, error) && !warnings.empty() && s1.link.allowedPcs.empty());
    writeFile(dir + "/shape2.json", R"({ "link": { "allowed_pcs": "nope", "port": "x" } })");
    Config s2;
    warnings.clear();
    CHECK(loadConfig(dir + "/shape2.json", s2, warnings, error) && warnings.size() >= 2 && s2.link.port == 47630);
}

}  // namespace

int main() {
    char pattern[] = "/tmp/fph-t06-tests/link-XXXXXX";
    fph::makeDirectories("/tmp/fph-t06-tests");
    const char* dir = mkdtemp(pattern);
    if (dir == nullptr) {
        std::printf("mkdtemp failed\n");
        return 1;
    }
    tempDir = dir;
    testAllowList();
    testPcMetrics();
    testPcMetricsUnknownIdsFromNewerPc();
    testParseVrlink();
    testReadVrlink();
    testLease();
    testLayoutSet();
    testLayoutAuthority();
    testConfigLink();
    const std::string cleanup = "rm -rf '" + tempDir + "'";
    if (std::system(cleanup.c_str()) != 0) std::printf("(could not remove %s)\n", tempDir.c_str());
    if (failures == 0) std::printf("link-server-test: all passed\n");
    else std::printf("link-server-test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
