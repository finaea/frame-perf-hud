// pc/link_client: the PC side of the link driven by a fake headset built on core/link_wire (06's LinkServer is
// Linux-only). Covers discovery, the lease phases, rediscovery + blob assembler resets, layout self-heal and edits.
#include "link_client.h"
#include "link_wire.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <set>
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
using namespace fph::pc;

using Bytes = std::vector<uint8_t>;

constexpr uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d;
}

const Endpoint kHeadset {ip(10, 1, 2, 3), kDefaultPort};
const Id128 kPcId = [] {
    Id128 id {};
    for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(0xA0 + i);
    return id;
}();
const Id128 kHeadsetId = [] {
    Id128 id {};
    for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(0x10 + i);
    return id;
}();

struct Sent {
    Endpoint to;
    PacketType type;
    Bytes payload;
};

class FakeEnv : public LinkClientEnv {
public:
    std::vector<Sent> sent;
    std::vector<uint32_t> bcast;

    void send(const Endpoint& to, const Bytes& datagram) override {
        Header h;
        const uint8_t* payload = nullptr;
        if (decodePacket(datagram.data(), datagram.size(), h, payload) != DecodeError::Ok) {
            std::printf("FAIL: client sent an undecodable datagram\n");
            ++failures;
            return;
        }
        sent.push_back({to, h.type, Bytes(payload, payload + h.length)});
    }
    std::vector<uint32_t> broadcastAddresses() override { return bcast; }

    size_t count(PacketType t) const {
        size_t n = 0;
        for (const Sent& s : sent) n += s.type == t ? 1 : 0;
        return n;
    }
    std::vector<Sent> ofType(PacketType t) const {
        std::vector<Sent> out;
        for (const Sent& s : sent) {
            if (s.type == t) out.push_back(s);
        }
        return out;
    }
};

uint32_t g_seq = 1;

Bytes pkt(PacketType type, const Bytes& payload) {
    return encodePacket(type, g_seq++, payload);
}

void feed(LinkClient& c, const Bytes& datagram, const Endpoint& from, double now) {
    c.handlePacket(datagram.data(), datagram.size(), from, now);
}

Bytes helloAck(PeerStatus status, uint32_t layoutVersion, const Id128& id = kHeadsetId) {
    HelloAck a;
    a.headsetId = id;
    a.name = "Frame";
    a.status = status;
    a.layoutVersion = layoutVersion;
    a.addresses.push_back({"wlan0", ip(192, 168, 1, 47)});
    a.addresses.push_back({"wlanap", ip(10, 35, 78, 1)});
    return pkt(PacketType::HelloAck, encodeHelloAck(a));
}

Bytes subscribeAck(PeerStatus status) {
    SubscribeAck a;
    a.status = status;
    return pkt(PacketType::SubscribeAck, encodeSubscribeAck(a));
}

void feedMetrics(LinkClient& c, uint32_t layoutVersion, double now) {
    LinkState st;
    st.layoutVersion = layoutVersion;
    for (const Bytes& p : encodeMetrics(1000, 1, st, {})) feed(c, pkt(PacketType::Metrics, p), kHeadset, now);
}

/** Feeds a complete blob (every chunk). */
void feedBlob(LinkClient& c, uint32_t blobId, BlobKind kind, uint8_t stream, const Bytes& data, double now) {
    for (const Bytes& p : splitBlob(blobId, kind, stream, data)) feed(c, pkt(PacketType::Blob, p), kHeadset, now);
}

void feedLayout(LinkClient& c, uint32_t blobId, const Layout& layout, double now) {
    const std::string json = layoutToJson(layout, 0);
    feedBlob(c, blobId, BlobKind::Layout, 0, Bytes(json.begin(), json.end()), now);
}

Layout layoutV(uint32_t version) {
    Layout l = defaultLayout();
    l.version = version;
    return l;
}

/** An edit of defaultLayout: the first widget of column 0 removed. */
Layout edited(const Layout& l) {
    Layout e = l;
    e.columns[0].widgets.erase(e.columns[0].widgets.begin());
    return e;
}

/** A subscribed client holding the headset's live layout (version 3, blob id 5). Time ends at 1.0. */
struct Rig {
    FakeEnv env;
    LinkClient client;
    double now = 0.0;
    Layout live = layoutV(3);

    Rig() : client(env, kPcId, "TestPC") {}

    void connect() {
        client.tick(now);
        feed(client, helloAck(PeerStatus::Allowed, 3), kHeadset, now);
        client.tick(now);
        now += 0.5;
        feedLayout(client, 5, live, now);
        now += 0.5;
    }
    /** Advances in 0.25 s steps, with the headset answering the lease so silence never trips. */
    void run(double seconds) {
        const double end = now + seconds;
        while (now < end - 1e-9) {
            now += 0.25;
            feed(client, subscribeAck(PeerStatus::Allowed), kHeadset, now);
            client.tick(now);
        }
    }
};

/** Reassembles the LAYOUT_SET blob copies sent since `from`; returns one entry per blob id. */
struct EditCopy {
    uint32_t blobId = 0;
    LayoutSet set;
};
std::vector<EditCopy> editCopies(const FakeEnv& env, size_t from = 0) {
    std::map<uint32_t, std::map<uint16_t, Bytes>> parts;
    std::map<uint32_t, uint16_t> counts;
    for (size_t i = from; i < env.sent.size(); ++i) {
        const Sent& s = env.sent[i];
        if (s.type != PacketType::Blob) continue;
        BlobChunk ch;
        if (!decodeBlobChunk(s.payload.data(), s.payload.size(), ch) || ch.kind != BlobKind::LayoutSet) continue;
        parts[ch.blobId][ch.index] = Bytes(ch.data, ch.data + ch.size);
        counts[ch.blobId] = ch.count;
    }
    std::vector<EditCopy> out;
    for (auto& [id, chunks] : parts) {
        if (chunks.size() != counts[id]) continue;
        Bytes all;
        for (auto& [idx, b] : chunks) all.insert(all.end(), b.begin(), b.end());
        EditCopy c;
        c.blobId = id;
        if (decodeLayoutSet(all.data(), all.size(), c.set)) out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

void testDiscoveryTargets() {
    FakeEnv env;
    env.bcast = {ip(192, 168, 1, 255), 0xFFFFFFFFu, ip(10, 1, 2, 3)};  // the last one repeats a remembered IP
    LinkClient c(env, kPcId, "TestPC");
    c.setDiscoveryTargets({ip(10, 1, 2, 3), ip(10, 35, 78, 1)}, ip(172, 16, 0, 9));
    c.tick(0.0);
    CHECK(c.phase() == LinkPhase::Discovery);
    // remembered x2, manual, 2 distinct broadcasts (the repeated IP is sent once)
    CHECK(c.hellosSent() == 5);
    CHECK(env.count(PacketType::Hello) == 5);
    bool toManual = false, toBroadcast = false, toRemembered = false;
    for (const Sent& s : env.ofType(PacketType::Hello)) {
        CHECK(s.to.port == kDefaultPort);
        toManual |= s.to.ip == ip(172, 16, 0, 9);
        toBroadcast |= s.to.ip == 0xFFFFFFFFu;
        toRemembered |= s.to.ip == ip(10, 35, 78, 1);
        Hello h;
        CHECK(decodeHello(s.payload.data(), s.payload.size(), h));
        CHECK(h.pcId == kPcId);
        CHECK(h.pcName == "TestPC");
    }
    CHECK(toManual && toBroadcast && toRemembered);
    c.tick(1.0);
    c.tick(1.9);
    CHECK(c.hellosSent() == 5);  // not before 2 s
    c.tick(2.0);
    CHECK(c.hellosSent() == 10);
    c.tick(3.9);
    CHECK(c.hellosSent() == 10);
    c.tick(4.0);
    CHECK(c.hellosSent() == 15);
    CHECK(c.subscribesSent() == 0);
}

void testPendingThenAllowed() {
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    c.setDiscoveryTargets({}, ip(10, 1, 2, 3));
    c.tick(0.0);
    CHECK(c.hellosSent() == 1);
    feed(c, helloAck(PeerStatus::Pending, 1), kHeadset, 0.1);
    CHECK(c.phase() == LinkPhase::Pending);
    CHECK(!c.subscribed());
    HeadsetSeen seen;
    CHECK(!c.takeHeadsetSeen(seen));  // only Allowed counts
    env.sent.clear();
    c.tick(1.0);
    CHECK(env.sent.empty());
    c.tick(2.1);
    CHECK(env.sent.size() == 1 && env.sent[0].type == PacketType::Hello && env.sent[0].to == kHeadset);  // only to it
    env.sent.clear();
    c.tick(3.0);
    CHECK(env.sent.empty());
    c.tick(4.1);
    CHECK(env.count(PacketType::Hello) == 1);
    CHECK(env.count(PacketType::Subscribe) == 0);

    feed(c, helloAck(PeerStatus::Allowed, 1), kHeadset, 4.2);
    CHECK(c.phase() == LinkPhase::Subscribed);
    CHECK(c.subscribed());
    CHECK(c.headset().name == "Frame");
    CHECK(c.headsetEndpoint() == kHeadset);
    CHECK(env.count(PacketType::Subscribe) == 1);  // right away
    Subscribe sub;
    const Sent s = env.ofType(PacketType::Subscribe).back();
    CHECK(decodeSubscribe(s.payload.data(), s.payload.size(), sub) && sub.pcId == kPcId);
    CHECK(s.to == kHeadset);

    CHECK(c.takeHeadsetSeen(seen));
    CHECK(seen.idHex == idToHex(kHeadsetId));
    CHECK(seen.name == "Frame");
    CHECK(!seen.ips.empty() && seen.ips[0] == kHeadset.ip);  // the answering address first
    CHECK(seen.ips.size() == 3);                             // then its own addresses
    CHECK(!c.takeHeadsetSeen(seen));                         // once

    // SUBSCRIBE every 2 s
    c.tick(5.0);
    c.tick(6.1);
    CHECK(env.count(PacketType::Subscribe) == 1);
    feed(c, subscribeAck(PeerStatus::Allowed), kHeadset, 6.15);
    c.tick(6.2);
    CHECK(env.count(PacketType::Subscribe) == 2);
    c.tick(7.0);
    CHECK(env.count(PacketType::Subscribe) == 2);
    c.tick(8.3);
    CHECK(env.count(PacketType::Subscribe) == 3);
}

void testDeniedAndWrongSource() {
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    c.setDiscoveryTargets({}, kHeadset.ip);
    c.tick(0.0);
    feed(c, helloAck(PeerStatus::Denied, 1), kHeadset, 0.1);
    CHECK(c.phase() == LinkPhase::Denied);
    // A different headset answering from elsewhere is ignored while we talk to this one
    feed(c, helloAck(PeerStatus::Allowed, 1, Id128 {}), Endpoint {ip(10, 9, 9, 9), kDefaultPort}, 0.2);
    CHECK(c.phase() == LinkPhase::Denied);
    // Garbage / malformed
    const uint64_t before = c.malformed();
    const uint8_t junk[] = {1, 2, 3};
    c.handlePacket(junk, sizeof(junk), kHeadset, 0.3);
    CHECK(c.malformed() == before + 1);
    // An older protocol's HELLO_ACK
    Bytes old = helloAck(PeerStatus::Allowed, 1);
    old[3] = static_cast<uint8_t>(kProtocolVersion + 1);
    CHECK(!c.versionMismatch());
    feed(c, old, kHeadset, 0.4);
    CHECK(c.versionMismatch());
    CHECK(c.phase() == LinkPhase::Denied);
}

void testBye_UnknownSubscriber_Silence() {
    enum Case { kUnknownSub, kBye, kSilence };
    for (int which : {kUnknownSub, kBye, kSilence}) {
        Rig r;
        r.connect();  // layout blob id 5 completed
        CHECK(r.client.subscribed() && r.client.layoutIsLive());
        CHECK(r.client.layout().version == 3);
        CHECK(r.client.assemblerResets() == 0);
        const double heard = r.now;
        const size_t mark = r.env.sent.size();
        if (which == kUnknownSub) {
            feed(r.client, subscribeAck(PeerStatus::UnknownSubscriber), kHeadset, heard);
        } else if (which == kBye) {
            Bye b;
            b.id = kHeadsetId;
            feed(r.client, pkt(PacketType::Bye, encodeBye(b)), kHeadset, heard);
        } else {
            // The last packet was 0.5 s ago: 5.9 s after it is fine, past 6 s it drops
            const double lastPacket = heard - 0.5;
            r.client.tick(lastPacket + 5.9);
            CHECK(r.client.subscribed());
            r.client.tick(lastPacket + 6.01);
        }
        const double t = which == kSilence ? heard - 0.5 + 6.01 : heard;
        CHECK(r.client.phase() == LinkPhase::Discovery);
        CHECK(r.client.assemblerResets() == 1);
        CHECK(r.client.rediscoveries() == 1);
        CHECK(!r.client.layoutIsLive());  // shown greyed until the headset's arrives again

        // HELLOs resume at once, also to the headset we just lost
        r.client.tick(t);
        bool helloToOld = false;
        for (size_t i = mark; i < r.env.sent.size(); ++i) {
            helloToOld |= r.env.sent[i].type == PacketType::Hello && r.env.sent[i].to.ip == kHeadset.ip;
        }
        CHECK(helloToOld);

        // Re-subscribe; the restarted headset numbers blobs from 1 again: a blob id 1 must be accepted now
        feed(r.client, helloAck(PeerStatus::Allowed, 4), kHeadset, t + 0.1);
        CHECK(r.client.subscribed());
        feedLayout(r.client, 1, layoutV(4), t + 0.2);
        CHECK(r.client.layoutIsLive());
        CHECK(r.client.layout().version == 4);
    }
}

void testLayoutBlobIdRestartWithoutReset() {
    // Sanity for the test above: with no rediscovery an older blob id is dropped
    Rig r;
    r.connect();
    feedLayout(r.client, 1, layoutV(9), r.now);
    CHECK(r.client.layout().version == 3);
}

void testLayoutGetSelfHeal() {
    Rig r;
    r.client.tick(0.0);
    feed(r.client, helloAck(PeerStatus::Allowed, 3), kHeadset, 0.0);
    // No layout yet: LAYOUT_GET every 1 s until a Layout blob arrives
    r.client.tick(0.0);
    CHECK(r.env.count(PacketType::LayoutGet) == 1);
    r.client.tick(0.5);
    CHECK(r.env.count(PacketType::LayoutGet) == 1);
    r.client.tick(1.0);
    CHECK(r.env.count(PacketType::LayoutGet) == 2);
    r.client.tick(2.0);
    CHECK(r.env.count(PacketType::LayoutGet) == 3);
    LayoutGet g;
    const Sent s = r.env.ofType(PacketType::LayoutGet).back();
    CHECK(decodeLayoutGet(s.payload.data(), s.payload.size(), g) && g.pcId == kPcId);
    feedLayout(r.client, 5, layoutV(3), 2.1);
    CHECK(r.client.layoutIsLive() && r.client.layout().version == 3);
    r.now = 2.1;
    r.run(4.0);
    CHECK(r.env.count(PacketType::LayoutGet) == 3);  // stopped

    // METRICS announcing another version: ask again every 1 s until we hold it
    const size_t base = r.env.count(PacketType::LayoutGet);
    feedMetrics(r.client, 3, r.now);  // same version: nothing
    r.client.tick(r.now);
    CHECK(r.env.count(PacketType::LayoutGet) == base);
    feedMetrics(r.client, 7, r.now);
    CHECK(r.client.headsetLayoutVersion() == 7);
    r.client.tick(r.now);
    CHECK(r.env.count(PacketType::LayoutGet) == base + 1);
    r.run(1.0);
    CHECK(r.env.count(PacketType::LayoutGet) == base + 2);
    r.run(1.0);
    CHECK(r.env.count(PacketType::LayoutGet) == base + 3);
    feedLayout(r.client, 6, layoutV(7), r.now);
    CHECK(r.client.layout().version == 7);
    const uint64_t rev = r.client.layoutRevision();
    r.run(3.0);
    CHECK(r.env.count(PacketType::LayoutGet) == base + 3);
    CHECK(r.client.layoutRevision() == rev);
}

void testWantsFollowLayout() {
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    Layout l;
    l.columns.resize(1);
    WidgetInstance w;
    w.type = WidgetType::CpuItems;
    l.columns[0].widgets.push_back(w);
    c.setCachedLayout(l);
    CHECK(c.haveLayout() && !c.layoutIsLive());
    c.setDiscoveryTargets({}, kHeadset.ip);
    c.tick(0.0);
    feed(c, helloAck(PeerStatus::Allowed, 1), kHeadset, 0.0);
    Subscribe sub;
    const Sent s = env.ofType(PacketType::Subscribe).back();
    CHECK(decodeSubscribe(s.payload.data(), s.payload.size(), sub));
    CHECK(sub.wants == (kWantMetrics | kWantCpuList));
}

void testEditAck() {
    Rig r;
    r.connect();
    const Layout before = r.client.layout();
    const Layout e = edited(before);
    CHECK(!r.client.editPending());
    const size_t mark = r.env.sent.size();
    CHECK(r.client.submitEdit(e, 3, r.now));
    CHECK(r.client.editPending());
    CHECK(r.client.editCopiesSent() == 1);
    CHECK(r.client.layout().sameWidgets(e));  // shown right away
    CHECK(r.client.layout().version == 3);
    CHECK(!r.client.submitEdit(before, 3, r.now));  // one at a time
    CHECK(r.client.editCopiesSent() == 1);
    CHECK(r.client.layout().sameWidgets(e));

    std::vector<EditCopy> first = editCopies(r.env, mark);
    CHECK(first.size() == 1);
    CHECK(first[0].set.pcId == kPcId);
    CHECK(first[0].set.baseVersion == 3);
    CHECK(first[0].set.requestId != 0);
    Layout sent;
    std::vector<std::string> w;
    std::string err;
    CHECK(parseLayoutText(first[0].set.layoutJson, sent, w, err));
    CHECK(sent.sameWidgets(e));

    // Resent every 1 s: same request id, new blob id
    const size_t mark2 = r.env.sent.size();
    r.run(0.75);
    CHECK(editCopies(r.env, mark2).empty());
    r.run(0.25);
    r.run(1.0);
    std::vector<EditCopy> more = editCopies(r.env, mark2);
    CHECK(more.size() == 2);
    for (const EditCopy& c : more) {
        CHECK(c.set.requestId == first[0].set.requestId);
        CHECK(c.blobId != first[0].blobId);
        CHECK(c.set.baseVersion == 3);
    }
    if (more.size() == 2) CHECK(more[0].blobId != more[1].blobId);
    CHECK(r.client.editCopiesSent() == 3);

    // A Layout blob arriving mid-edit doesn't override what's shown
    feedLayout(r.client, 8, layoutV(3), r.now);
    CHECK(r.client.layout().sameWidgets(e));

    // An ACK for another request is ignored
    LayoutAck wrong;
    wrong.requestId = first[0].set.requestId + 1;
    wrong.version = 9;
    feed(r.client, pkt(PacketType::LayoutAck, encodeLayoutAck(wrong)), kHeadset, r.now);
    CHECK(r.client.editPending());

    LayoutAck ack;
    ack.requestId = first[0].set.requestId;
    ack.version = 4;
    feed(r.client, pkt(PacketType::LayoutAck, encodeLayoutAck(ack)), kHeadset, r.now);
    CHECK(!r.client.editPending());
    CHECK(r.client.layout().version == 4);
    CHECK(r.client.layout().sameWidgets(e));
    CHECK(r.client.layoutIsLive());
    CHECK(r.client.takeNotice().empty());
    const size_t copies = r.client.editCopiesSent();
    const size_t gets = r.env.count(PacketType::LayoutGet);
    r.run(3.0);
    CHECK(r.client.editCopiesSent() == copies);  // no more resends
    CHECK(r.env.count(PacketType::LayoutGet) == gets);

    // And the next edit is allowed again
    CHECK(r.client.submitEdit(before, 4, r.now));
}

void testEditRejectStale() {
    Rig r;
    r.connect();
    const Layout before = r.client.layout();
    CHECK(r.client.submitEdit(edited(before), 2, r.now));
    const uint32_t req = editCopies(r.env).back().set.requestId;
    LayoutReject rj;
    rj.requestId = req;
    rj.currentVersion = 5;
    rj.reason = RejectReason::Stale;
    const uint64_t rev = r.client.layoutRevision();
    feed(r.client, pkt(PacketType::LayoutReject, encodeLayoutReject(rj)), kHeadset, r.now);
    CHECK(!r.client.editPending());
    CHECK(r.client.layout().sameWidgets(before));  // restored
    CHECK(r.client.layout().version == before.version);
    CHECK(r.client.layoutRevision() > rev);
    const std::string notice = r.client.takeNotice();
    CHECK(notice.find("wasn't applied") != std::string::npos);
    CHECK(r.client.takeNotice().empty());  // cleared by reading
    CHECK(r.client.headsetLayoutVersion() == 5);

    // LAYOUT_GET follows, and the headset's new layout lands
    const size_t gets = r.env.count(PacketType::LayoutGet);
    r.client.tick(r.now);
    CHECK(r.env.count(PacketType::LayoutGet) == gets + 1);
    Layout theirs = layoutV(5);
    theirs.columns.pop_back();
    feedLayout(r.client, 9, theirs, r.now);
    CHECK(r.client.layout().version == 5);
    CHECK(r.client.layout().sameWidgets(theirs));
    // No more edit copies
    const uint64_t copies = r.client.editCopiesSent();
    r.run(3.0);
    CHECK(r.client.editCopiesSent() == copies);
}

void testEditRejectOtherReasons() {
    Rig r;
    r.connect();
    const Layout before = r.client.layout();
    CHECK(r.client.submitEdit(edited(before), 3, r.now));
    LayoutReject rj;
    rj.requestId = editCopies(r.env).back().set.requestId;
    rj.currentVersion = 3;
    rj.reason = RejectReason::SaveFailed;
    feed(r.client, pkt(PacketType::LayoutReject, encodeLayoutReject(rj)), kHeadset, r.now);
    CHECK(!r.client.editPending());
    CHECK(r.client.layout().sameWidgets(before));
    CHECK(!r.client.takeNotice().empty());
}

void testPendingEditSurvivesRediscovery(bool viaBye) {
    Rig r;
    r.connect();
    const Layout before = r.client.layout();
    const Layout e = edited(before);
    CHECK(r.client.submitEdit(e, 3, r.now));
    const EditCopy firstCopy = editCopies(r.env).back();
    if (viaBye) {
        Bye b;
        b.id = kHeadsetId;
        feed(r.client, pkt(PacketType::Bye, encodeBye(b)), kHeadset, r.now);
    } else {
        LayoutReject rj;
        rj.requestId = firstCopy.set.requestId;
        rj.currentVersion = 3;
        rj.reason = RejectReason::UnknownSubscriber;
        feed(r.client, pkt(PacketType::LayoutReject, encodeLayoutReject(rj)), kHeadset, r.now);
    }
    CHECK(r.client.phase() == LinkPhase::Discovery);
    CHECK(r.client.editPending());
    CHECK(r.client.assemblerResets() == 1);
    CHECK(r.client.layout().sameWidgets(e));
    CHECK(!r.client.submitEdit(before, 3, r.now));  // not subscribed

    // Nothing resent while looking for the headset
    const size_t copies = r.client.editCopiesSent();
    r.client.tick(r.now + 1.1);
    CHECK(r.client.editCopiesSent() == copies);

    // Re-subscribed: the edit goes out again right away (same request id, new blob id)
    const double t = r.now + 1.2;
    const size_t mark = r.env.sent.size();
    feed(r.client, helloAck(PeerStatus::Allowed, 3), kHeadset, t);
    r.client.tick(t);
    CHECK(r.client.subscribed());
    CHECK(r.env.count(PacketType::Subscribe) >= 2);
    std::vector<EditCopy> again = editCopies(r.env, mark);
    CHECK(again.size() == 1);
    if (!again.empty()) {
        CHECK(again[0].set.requestId == firstCopy.set.requestId);
        CHECK(again[0].blobId != firstCopy.blobId);
        CHECK(again[0].set.baseVersion == 3);
    }
    CHECK(r.client.editCopiesSent() == copies + 1);
    // ... and an ACK finishes it
    LayoutAck ack;
    ack.requestId = firstCopy.set.requestId;
    ack.version = 4;
    feed(r.client, pkt(PacketType::LayoutAck, encodeLayoutAck(ack)), kHeadset, t);
    CHECK(!r.client.editPending());
    CHECK(r.client.layout().version == 4);
}

void testSubmitEditPreconditions() {
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    c.setCachedLayout(layoutV(1));
    CHECK(!c.submitEdit(edited(layoutV(1)), 1, 0.0));  // not subscribed
    CHECK(env.sent.empty());
    c.setDiscoveryTargets({}, kHeadset.ip);
    c.tick(0.0);
    feed(c, helloAck(PeerStatus::Allowed, 1), kHeadset, 0.0);
    CHECK(c.subscribed());

    FakeEnv env2;
    LinkClient c2(env2, kPcId, "TestPC");
    c2.setDiscoveryTargets({}, kHeadset.ip);
    c2.tick(0.0);
    feed(c2, helloAck(PeerStatus::Allowed, 1), kHeadset, 0.0);
    CHECK(!c2.submitEdit(edited(layoutV(1)), 1, 0.0));  // subscribed but no layout yet
    CHECK(env2.count(PacketType::Blob) == 0);
}

void testProcessListBlob() {
    Rig r;
    r.connect();
    ProcessListWire list;
    list.generation = 1;
    list.cpuValid = true;
    ProcessRowWire row;
    row.name = "game.exe";
    row.pid = 42;
    row.cpuCorePct = 80.0f;
    row.rssMiB = 512.0f;
    list.rows.push_back(row);
    const Bytes data = encodeProcessList(list);
    feedBlob(r.client, 6, BlobKind::ProcessList, kStreamCpuList, data, r.now);
    CHECK(r.client.processes().byCpu.size() == 1);
    CHECK(r.client.processes().byCpu[0].name == "game.exe");
    CHECK(r.client.processes().cpuValid);
    CHECK(r.client.processes().byRam.empty());
}

void testMultiChunkEdit() {
    Rig r;
    r.connect();
    Layout big = r.client.layout();
    big.columns.clear();
    for (int c = 0; c < 3; ++c) {
        LayoutColumn col;
        for (int i = 0; i < 16; ++i) {
            WidgetInstance w;
            w.type = WidgetType::CpuItems;
            w.maxItems = 5 + static_cast<uint32_t>(i);
            col.widgets.push_back(w);
        }
        big.columns.push_back(col);
    }
    const size_t mark = r.env.sent.size();
    CHECK(r.client.submitEdit(big, 3, r.now));
    r.run(2.0);  // copies at 0, 1, 2 s
    CHECK(r.client.editCopiesSent() == 3);

    std::map<uint32_t, std::vector<uint16_t>> chunks;  // blob id -> chunk indexes seen
    std::map<uint32_t, uint16_t> counts;
    for (size_t i = mark; i < r.env.sent.size(); ++i) {
        const Sent& sp = r.env.sent[i];
        if (sp.type != PacketType::Blob) continue;
        BlobChunk ch;
        CHECK(decodeBlobChunk(sp.payload.data(), sp.payload.size(), ch));
        if (ch.kind != BlobKind::LayoutSet) continue;
        chunks[ch.blobId].push_back(ch.index);
        counts[ch.blobId] = ch.count;
    }
    CHECK(chunks.size() == 3);  // one new blob id per copy
    for (const auto& [id, idx] : chunks) {
        CHECK(counts[id] > 1);
        CHECK(idx.size() == counts[id]);  // every chunk of the copy shares that id, each sent once
        for (size_t k = 0; k < idx.size(); ++k) CHECK(idx[k] == k);
    }
    const std::vector<EditCopy> copies = editCopies(r.env, mark);
    CHECK(copies.size() == 3);
    for (const EditCopy& c : copies) {
        CHECK(c.set.layoutJson.size() > 1186);
        CHECK(c.set.requestId == copies[0].set.requestId);
        CHECK(c.set.baseVersion == 3);
        CHECK(c.set.pcId == kPcId);
        CHECK(c.set.layoutJson == copies[0].set.layoutJson);
    }
    CHECK(copies.size() == 3 && copies[0].blobId != copies[1].blobId && copies[1].blobId != copies[2].blobId);
}

void testDeniedThenAllowed() {
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    c.setDiscoveryTargets({}, kHeadset.ip);
    c.tick(0.0);
    feed(c, helloAck(PeerStatus::Denied, 1), kHeadset, 0.1);
    CHECK(c.phase() == LinkPhase::Denied);
    // HELLO keeps going every 2 s to that headset
    env.sent.clear();
    c.tick(2.2);
    CHECK(env.sent.size() == 1 && env.sent[0].type == PacketType::Hello && env.sent[0].to == kHeadset);
    HeadsetSeen seen;
    CHECK(!c.takeHeadsetSeen(seen));
    // Approved later: same endpoint, same id
    feed(c, helloAck(PeerStatus::Allowed, 1), kHeadset, 2.3);
    CHECK(c.phase() == LinkPhase::Subscribed);
    CHECK(env.count(PacketType::Subscribe) == 1);
    CHECK(env.ofType(PacketType::Subscribe)[0].to == kHeadset);
    CHECK(c.takeHeadsetSeen(seen) && seen.idHex == idToHex(kHeadsetId));
}

void testGoodbye() {
    Rig r;
    r.connect();
    r.client.sayGoodbye();
    CHECK(r.env.count(PacketType::Bye) == 1);
    Bye b;
    const Sent s = r.env.ofType(PacketType::Bye).back();
    CHECK(decodeBye(s.payload.data(), s.payload.size(), b) && b.id == kPcId);
    // Not talking to anyone: nothing to say
    FakeEnv env;
    LinkClient c(env, kPcId, "TestPC");
    c.sayGoodbye();
    CHECK(env.sent.empty());
}

// ---- Mixed-version tolerance: a METRICS packet from a headset build with newer ids than this PC build
// knows (or vice versa) must retain every known value and never count as malformed -- only the unknown ids
// get skipped (link_client.cpp's own local counter; decodeMetrics itself doesn't consult the registry) ----

void testOnMetricsSkipsUnknownIdsKeepsKnownOnes() {
    Rig r;
    r.connect();
    const uint64_t malformedBefore = r.client.malformed();
    const uint64_t unknownBefore = r.client.unknownMetricValues();

    WireValue known1;
    known1.id = metric::kBatteryPct;
    known1.type = WireType::F32;
    known1.ok = true;
    known1.f32 = 55.0f;

    WireValue unknownOk;
    unknownOk.id = 65000;  // not in this build's registry at all
    unknownOk.type = WireType::F32;
    unknownOk.ok = true;
    unknownOk.f32 = 1.0f;

    WireValue unknownNa;
    unknownNa.id = 65001;
    unknownNa.type = WireType::I32;
    unknownNa.ok = false;
    unknownNa.reason = Reason::NotSampledYet;

    WireValue known2;
    known2.id = metric::kTempCpuC;
    known2.type = WireType::F32;
    known2.ok = true;
    known2.f32 = 42.0f;

    LinkState st;
    st.layoutVersion = r.live.version;
    const auto parts = encodeMetrics(1000, 1, st, {known1, unknownOk, unknownNa, known2});
    for (const Bytes& p : parts) feed(r.client, pkt(PacketType::Metrics, p), kHeadset, r.now);

    CHECK(r.client.malformed() == malformedBefore);          // never malformed just for an unknown id
    CHECK(r.client.unknownMetricValues() == unknownBefore + 2);
    // the known entries, on either side of the unknown ones, are retained regardless
    CHECK(r.client.store().f32Or(metric::kBatteryPct, -1) == 55.0f);
    CHECK(r.client.store().f32Or(metric::kTempCpuC, -1) == 42.0f);
}

// ---- R3: CONTROL_STATE / CONTROL_ACTION / CONTROL_RESULT ----

Bytes controlStateBytes(uint32_t revision, uint32_t layoutVersion, uint8_t flags = 0) {
    ControlState s;
    s.revision = revision;
    s.layoutVersion = layoutVersion;
    s.flags = flags;
    s.activeId = "profile-1";
    s.profiles = {{"profile-1", "Gaming"}, {"profile-2", "Work"}};
    s.status = "ok";
    return encodeControlState(s);
}

Bytes controlResultBytes(uint32_t requestId, ControlOutcome outcome, uint32_t version, const std::string& message) {
    ControlResult m;
    m.requestId = requestId;
    m.outcome = outcome;
    m.layoutVersion = version;
    m.message = message;
    return encodeControlResult(m);
}

ControlAction actionOf(const Sent& s) {
    ControlAction a;
    CHECK(decodeControlAction(s.payload.data(), s.payload.size(), a));
    return a;
}

void testControlClient() {
    {
        // nothing before a CONTROL_STATE: no state, sendControl refuses and sends nothing
        Rig r;
        r.connect();
        CHECK(r.client.controlState() == nullptr && r.client.controlRevision() == 0);
        const size_t mark = r.env.sent.size();
        CHECK(!r.client.sendControl(ControlKind::RecordOn, "", "", false, 0, r.now));
        CHECK(r.env.sent.size() == mark && !r.client.controlInFlight());
        // not subscribed, state or not: refuses
        Rig cold;
        feed(cold.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, 0.0);
        CHECK(!cold.client.subscribed() && !cold.client.sendControl(ControlKind::RecordOn, "", "", false, 0, 0.1));
        CHECK(cold.env.count(PacketType::ControlAction) == 0);
    }
    {
        // a state: kept, revision bumps on new content only (not on the revision field alone)
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3, kControlUnsaved)), kHeadset, r.now);
        const ControlState* st = r.client.controlState();
        CHECK(st != nullptr && st->revision == 1 && st->layoutVersion == 3 && st->profiles.size() == 2 &&
              st->activeId == "profile-1" && (st->flags & kControlUnsaved) != 0);
        const uint64_t rev1 = r.client.controlRevision();
        CHECK(rev1 == 1);
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3, kControlUnsaved)), kHeadset, r.now + 0.1);
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(9, 3, kControlUnsaved)), kHeadset, r.now + 0.2);
        CHECK(r.client.controlRevision() == rev1 && r.client.controlState()->revision == 9);  // 2 s repeats don't redraw
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(10, 3, kControlUnsaved | kControlRecording)),
             kHeadset, r.now + 0.3);
        CHECK(r.client.controlRevision() == rev1 + 1);
        CHECK(r.client.malformed() == 0 && r.client.subscribed());
    }
    {
        // sendControl: fields, one in flight, resend every 1 s, matching RESULT finishes it
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, r.now);
        const uint64_t revBefore = r.client.controlRevision();
        const size_t mark = r.env.sent.size();
        const double t0 = r.now;
        CHECK(r.client.sendControl(ControlKind::ProfileUse, "profile-2", "", true, 3, t0));
        CHECK(r.client.controlInFlight() && r.client.controlRevision() > revBefore);
        std::vector<Sent> acts = r.env.ofType(PacketType::ControlAction);
        CHECK(acts.size() == 1 && acts[0].to == kHeadset && r.env.sent.size() == mark + 1);
        const ControlAction first = actionOf(acts[0]);
        CHECK(first.pcId == kPcId && first.kind == ControlKind::ProfileUse && first.profileId == "profile-2" &&
              (first.flags & kControlConfirmed) != 0 && first.layoutVersion == 3 && first.requestId != 0);
        // a second one while in flight: refused, nothing sent
        CHECK(!r.client.sendControl(ControlKind::RecordOn, "", "", false, 0, t0 + 0.1));
        CHECK(r.env.count(PacketType::ControlAction) == 1);
        // resends: not before 1 s, then once per second, the same request id and fields
        r.now = t0;
        auto stepTo = [&](double t) {
            while (r.now < t - 1e-9) {
                r.now += 0.25;
                feed(r.client, subscribeAck(PeerStatus::Allowed), kHeadset, r.now);
                r.client.tick(r.now);
            }
        };
        stepTo(t0 + 0.75);
        CHECK(r.env.count(PacketType::ControlAction) == 1);
        stepTo(t0 + 1.0);
        CHECK(r.env.count(PacketType::ControlAction) == 2);
        stepTo(t0 + 1.75);
        CHECK(r.env.count(PacketType::ControlAction) == 2);
        stepTo(t0 + 2.0);
        CHECK(r.env.count(PacketType::ControlAction) == 3);
        acts = r.env.ofType(PacketType::ControlAction);
        for (const Sent& s : acts) {
            const ControlAction a = actionOf(s);
            CHECK(a.requestId == first.requestId && a.kind == first.kind && a.profileId == first.profileId &&
                  a.flags == first.flags && a.layoutVersion == first.layoutVersion);
        }
        // a result for another request id is ignored: still in flight, nothing to take
        feed(r.client, pkt(PacketType::ControlResult, controlResultBytes(first.requestId + 5, ControlOutcome::Done, 3, "x")),
             kHeadset, r.now);
        ControlResult res;
        ControlKind kind = ControlKind::ProfileNew;
        CHECK(r.client.controlInFlight() && !r.client.takeControlResult(res, kind));
        // the matching one finishes it
        const uint64_t revMid = r.client.controlRevision();
        feed(r.client, pkt(PacketType::ControlResult, controlResultBytes(first.requestId, ControlOutcome::NeedsConfirm, 4, "Lose?")),
             kHeadset, r.now);
        CHECK(!r.client.controlInFlight() && r.client.controlRevision() > revMid);
        CHECK(r.client.takeControlResult(res, kind));
        CHECK(res.requestId == first.requestId && res.outcome == ControlOutcome::NeedsConfirm && res.layoutVersion == 4 &&
              res.message == "Lose?" && kind == ControlKind::ProfileUse);
        CHECK(!r.client.takeControlResult(res, kind));  // once
        // no more resends, and a late duplicate answer is harmless
        const size_t sentNow = r.env.count(PacketType::ControlAction);
        stepTo(r.now + 3.0);
        CHECK(r.env.count(PacketType::ControlAction) == sentNow);
        feed(r.client, pkt(PacketType::ControlResult, controlResultBytes(first.requestId, ControlOutcome::Done, 4, "dup")),
             kHeadset, r.now);
        CHECK(!r.client.takeControlResult(res, kind) && !r.client.controlInFlight());
        // the next action gets a new request id
        CHECK(r.client.sendControl(ControlKind::RecordOn, "", "", false, 0, r.now));
        const ControlAction second = actionOf(r.env.ofType(PacketType::ControlAction).back());
        CHECK(second.requestId != first.requestId && (second.flags & kControlConfirmed) == 0 &&
              second.kind == ControlKind::RecordOn);
        CHECK(r.client.malformed() == 0);
    }
    {
        // no answer: given up after 5 s with "No answer from the headset"
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, r.now);
        const double t0 = r.now;
        CHECK(r.client.sendControl(ControlKind::ProfileSave, "", "", false, 3, t0));
        for (double t = t0 + 0.25; t <= t0 + 5.0 + 1e-9; t += 0.25) {
            r.now = t;
            feed(r.client, subscribeAck(PeerStatus::Allowed), kHeadset, t);
            r.client.tick(t);
        }
        CHECK(r.client.controlInFlight());  // exactly 5 s: still waiting (it gives up past 5 s)
        CHECK(r.env.count(PacketType::ControlAction) == 6);  // t0, +1 .. +5
        r.now = t0 + 5.25;
        feed(r.client, subscribeAck(PeerStatus::Allowed), kHeadset, r.now);
        r.client.tick(r.now);
        CHECK(!r.client.controlInFlight());
        ControlResult res;
        ControlKind kind = ControlKind::ProfileNew;
        CHECK(r.client.takeControlResult(res, kind));
        CHECK(res.outcome == ControlOutcome::Failed && res.message == "No answer from the headset" &&
              kind == ControlKind::ProfileSave);
        // and it can send again
        CHECK(r.client.sendControl(ControlKind::ProfileSave, "", "", false, 3, r.now));
    }
    {
        // losing the subscription while waiting fails it at once (the BYE / silence case)
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, r.now);
        CHECK(r.client.sendControl(ControlKind::RecordOn, "", "", false, 0, r.now));
        feed(r.client, subscribeAck(PeerStatus::UnknownSubscriber), kHeadset, r.now + 0.1);
        CHECK(!r.client.subscribed());
        r.client.tick(r.now + 0.2);
        CHECK(!r.client.controlInFlight());
        ControlResult res;
        ControlKind kind = ControlKind::ProfileNew;
        CHECK(r.client.takeControlResult(res, kind) && res.outcome == ControlOutcome::Failed &&
              res.message == "No answer from the headset");
    }
    {
        // version skew: unknown packet types and CONTROL_* with extra trailing bytes neither count as malformed nor
        // drop the link
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, r.now);
        const uint64_t malformed = r.client.malformed();
        const uint64_t rev = r.client.controlRevision();
        feed(r.client, pkt(static_cast<PacketType>(99), Bytes {1, 2, 3, 4}), kHeadset, r.now + 0.1);
        feed(r.client, pkt(static_cast<PacketType>(200), Bytes {}), kHeadset, r.now + 0.1);
        CHECK(r.client.malformed() == malformed && r.client.subscribed());
        Bytes newer = controlStateBytes(2, 3, kControlRecording);
        for (uint8_t b : {0xAA, 0xBB, 0xCC, 0xDD}) newer.push_back(b);  // a newer headset's appended fields
        feed(r.client, pkt(PacketType::ControlState, newer), kHeadset, r.now + 0.2);
        CHECK(r.client.malformed() == malformed && r.client.subscribed());
        CHECK(r.client.controlState() != nullptr && r.client.controlState()->revision == 2 &&
              (r.client.controlState()->flags & kControlRecording) != 0 && r.client.controlRevision() == rev + 1);
        // a RESULT with trailing bytes and an outcome from the future reads as Failed
        CHECK(r.client.sendControl(ControlKind::RecordOn, "", "", false, 0, r.now + 0.3));
        const uint32_t id = actionOf(r.env.ofType(PacketType::ControlAction).back()).requestId;
        Bytes future = controlResultBytes(id, static_cast<ControlOutcome>(77), 3, "newer");
        future.push_back(0x55);
        feed(r.client, pkt(PacketType::ControlResult, future), kHeadset, r.now + 0.4);
        ControlResult res;
        ControlKind kind = ControlKind::ProfileNew;
        CHECK(r.client.takeControlResult(res, kind) && res.outcome == ControlOutcome::Failed && res.message == "newer");
        CHECK(r.client.malformed() == malformed && r.client.subscribed());
        // truly short ones are malformed, though
        Bytes shortState = controlStateBytes(3, 3);
        shortState.resize(shortState.size() - 1);
        feed(r.client, pkt(PacketType::ControlState, shortState), kHeadset, r.now + 0.5);
        CHECK(r.client.malformed() == malformed + 1 && r.client.controlState()->revision == 2);
    }
}

// Lane F review: request ids differ across launches (the headset remembers answers per PC id across a BYE / HELLO,
// so ids restarting at 1 would replay an old answer), and losing the connection clears the headset's tab state
void testControlReviewFixes() {
    std::set<uint32_t> ids;
    for (int launch = 0; launch < 20; ++launch) {  // the same PC id, a new LinkClient each time = a new app launch
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3)), kHeadset, r.now);
        CHECK(r.client.sendControl(launch % 2 ? ControlKind::RecordOff : ControlKind::RecordOn, "", "", false, 3, r.now));
        const std::vector<Sent> acts = r.env.ofType(PacketType::ControlAction);
        CHECK(acts.size() == 1);
        if (acts.size() == 1) ids.insert(actionOf(acts[0]).requestId);
    }
    CHECK(ids.size() == 20);  // no launch reused another's id (ids.count(1) == 20 before the fix)
    CHECK(ids.count(0) == 0);
    {
        Rig r;
        r.connect();
        feed(r.client, pkt(PacketType::ControlState, controlStateBytes(1, 3, kControlUnsaved)), kHeadset, r.now);
        CHECK(r.client.controlState() != nullptr);
        const uint64_t rev = r.client.controlRevision();
        // 7 s of silence: back to discovery, and the old headset's state goes with it (an older HUD found next
        // must show "update it", not this one's buttons)
        r.now += 7.0;
        r.client.tick(r.now);
        CHECK(!r.client.subscribed());
        CHECK(r.client.controlState() == nullptr);
        CHECK(r.client.controlRevision() > rev);
        CHECK(!r.client.sendControl(ControlKind::RecordOn, "", "", false, 3, r.now));
    }
}

}  // namespace

int main() {
    testDiscoveryTargets();
    testPendingThenAllowed();
    testDeniedAndWrongSource();
    testBye_UnknownSubscriber_Silence();
    testLayoutBlobIdRestartWithoutReset();
    testLayoutGetSelfHeal();
    testWantsFollowLayout();
    testEditAck();
    testEditRejectStale();
    testEditRejectOtherReasons();
    testPendingEditSurvivesRediscovery(true);
    testPendingEditSurvivesRediscovery(false);
    testSubmitEditPreconditions();
    testProcessListBlob();
    testMultiChunkEdit();
    testDeniedThenAllowed();
    testGoodbye();
    testOnMetricsSkipsUnknownIdsKeepsKnownOnes();
    testControlClient();       // R3
    testControlReviewFixes();  // R3 lane F review
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc link client: all checks passed\n");
    return 0;
}
