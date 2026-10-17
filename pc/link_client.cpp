#include "link_client.h"

#include "log.h"

#include <algorithm>
#include <cstdio>
#include <random>

namespace fph {
namespace pc {

using namespace fph::link;

std::string Endpoint::text() const {
    return ipv4ToText(ip) + ":" + std::to_string(port);
}

const char* linkPhaseName(LinkPhase phase) {
    switch (phase) {
        case LinkPhase::Discovery: return "discovery";
        case LinkPhase::Pending: return "pending";
        case LinkPhase::Denied: return "denied";
        case LinkPhase::Subscribed: return "subscribed";
    }
    return "?";
}

namespace {

uint32_t randomRequestId() {
    static std::mt19937 rng(std::random_device {}());
    uint32_t id = 0;
    while (id == 0) id = rng();
    return id;
}

}  // namespace

LinkClient::LinkClient(LinkClientEnv& env, const Id128& pcId, std::string pcName, uint16_t port)
    : env_(env), pcId_(pcId), pcName_(std::move(pcName)), port_(port) {}

void LinkClient::setDiscoveryTargets(const std::vector<uint32_t>& remembered, uint32_t manualIp) {
    remembered_ = remembered;
    manualIp_ = manualIp;
    // A new manual IP should be tried now, not at the next 2 s mark
    if (phase_ == LinkPhase::Discovery) nextHello_ = 0.0;
}

void LinkClient::setCachedLayout(const Layout& layout) {
    if (layoutLive_) return;
    setLayout(layout, false);
}

void LinkClient::setLayout(const Layout& layout, bool live) {
    layout_ = layout;
    haveLayout_ = true;
    layoutLive_ = live;
    ++layoutRevision_;
}

void LinkClient::sendPacket(const Endpoint& to, PacketType type, const std::vector<uint8_t>& payload) {
    env_.send(to, encodePacket(type, seq_++, payload));
}

void LinkClient::sendHello(const Endpoint& to) {
    Hello hello;
    hello.pcId = pcId_;
    hello.pcName = pcName_;
    sendPacket(to, PacketType::Hello, encodeHello(hello));
    ++hellosSent_;
}

uint32_t LinkClient::wants() const {
    uint32_t w = kWantMetrics;
    for (const LayoutColumn& c : layout_.columns) {
        for (const WidgetInstance& i : c.widgets) {
            if (i.type == WidgetType::CpuItems) w |= kWantCpuList;
            if (i.type == WidgetType::RamItems) w |= kWantRamList;
        }
    }
    return w | CameraFeed::wantsFor(layout_);  // T10: the cameras of this layout's camera widgets
}

void LinkClient::sendSubscribe(double now) {
    Subscribe sub;
    sub.pcId = pcId_;
    sub.wants = wants();
    sub.cameraFps = static_cast<uint8_t>(cameraFps_);
    sendPacket(headsetEp_, PacketType::Subscribe, encodeSubscribe(sub));
    // T10: camera frames prefer the dongle path. When the headset's dongle AP address isn't the path we subscribed
    // on, register it for camera frames only (the lease and metrics stay where they are). If this PC isn't on the
    // dongle's network the copy just goes nowhere, and the frames come over the lease path.
    cameraPathIp_ = 0;
    if (sub.wants & cam::kWantCameraMask) {
        for (const LocalAddress& a : headset_.addresses) {
            if (a.ifName == "wlanap" && a.ip != 0 && a.ip != headsetEp_.ip) cameraPathIp_ = a.ip;
        }
    }
    if (cameraPathIp_ != 0) {
        Subscribe path = sub;
        path.wants |= cam::kWantCameraPathOnly;
        sendPacket(Endpoint {cameraPathIp_, port_}, PacketType::Subscribe, encodeSubscribe(path));
    }
    ++subscribesSent_;
    nextSubscribe_ = now + kSubscribeEverySec;
}

void LinkClient::sendEditCopy(double now) {
    // Same request id every copy, a new blob id each time (the headset's assembler drops a completed id)
    for (const std::vector<uint8_t>& chunk : splitBlob(nextBlobId_++, BlobKind::LayoutSet, 0, pending_->blob)) {
        sendPacket(headsetEp_, PacketType::Blob, chunk);
    }
    ++pending_->copies;
    ++editCopiesSent_;
    pending_->nextSend = now + kEditResendSec;
}

void LinkClient::backToDiscovery(double now, const char* why) {
    logLine("[link] back to discovery (%s), was %s with %s", why, linkPhaseName(phase_), headsetEp_.text().c_str());
    if (headsetEp_.ip != 0) lastHeadsetIp_ = headsetEp_.ip;
    phase_ = LinkPhase::Discovery;
    headsetEp_ = Endpoint();
    // A restarted headset numbers its blobs from 1 again: forget what was completed before
    blobs_ = BlobAssembler();
    cameras_.reset();  // T10
    cameraPathIp_ = 0;
    ++assemblerResets_;
    ++rediscoveries_;
    layoutLive_ = false;
    nextHello_ = now;  // right away
}

void LinkClient::tick(double now) {
    blobs_.expire(now);
    if (phase_ != LinkPhase::Discovery && now - lastHeard_ > kLeaseSec) {
        backToDiscovery(now, "6 s of silence");
    }
    switch (phase_) {
        case LinkPhase::Discovery:
            if (now >= nextHello_) {
                std::vector<uint32_t> targets = remembered_;
                if (manualIp_ != 0) targets.push_back(manualIp_);
                if (lastHeadsetIp_ != 0) targets.push_back(lastHeadsetIp_);
                for (uint32_t b : env_.broadcastAddresses()) targets.push_back(b);
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                for (uint32_t ip : targets) {
                    if (ip != 0) sendHello(Endpoint {ip, port_});
                }
                nextHello_ = now + kHelloRepeatSec;
            }
            break;
        case LinkPhase::Pending:
        case LinkPhase::Denied:
            if (now >= nextHello_) {
                sendHello(headsetEp_);
                nextHello_ = now + kHelloRepeatSec;
            }
            break;
        case LinkPhase::Subscribed:
            if (now >= nextSubscribe_) sendSubscribe(now);
            if (pending_ != nullptr && now >= pending_->nextSend) {
                sendEditCopy(now);
                logLine("[link] LAYOUT_SET request %u resent (copy %d)", pending_->requestId, pending_->copies);
            }
            if (pending_ == nullptr && needLayout_ && now >= nextLayoutGet_) {
                LayoutGet get;
                get.pcId = pcId_;
                sendPacket(headsetEp_, PacketType::LayoutGet, encodeLayoutGet(get));
                ++layoutGetsSent_;
                nextLayoutGet_ = now + kLayoutResendSec;
            }
            break;
    }
}

void LinkClient::sayGoodbye() {
    if (headsetEp_.ip == 0) return;
    Bye bye;
    bye.id = pcId_;
    sendPacket(headsetEp_, PacketType::Bye, encodeBye(bye));
    logLine("[link] BYE -> %s", headsetEp_.text().c_str());
}

bool LinkClient::submitEdit(const Layout& edited, uint32_t baseVersion, double now) {
    if (phase_ != LinkPhase::Subscribed || !haveLayout_ || pending_ != nullptr) return false;
    pending_.reset(new PendingEdit());
    pending_->requestId = randomRequestId();
    pending_->baseVersion = baseVersion;
    pending_->before = layout_;
    LayoutSet set;
    set.pcId = pcId_;
    set.requestId = pending_->requestId;
    set.baseVersion = baseVersion;
    set.layoutJson = layoutToJson(edited, 0);
    pending_->blob = encodeLayoutSet(set);
    Layout shown = edited;
    shown.version = layout_.version;
    setLayout(shown, layoutLive_);
    sendEditCopy(now);
    logLine("[link] LAYOUT_SET request %u, base %u, %zu bytes", set.requestId, baseVersion, set.layoutJson.size());
    return true;
}

std::string LinkClient::takeNotice() {
    std::string n;
    n.swap(notice_);
    return n;
}

bool LinkClient::takeHeadsetSeen(HeadsetSeen& out) {
    if (!seenFresh_) return false;
    seenFresh_ = false;
    out = seen_;
    return true;
}

void LinkClient::handlePacket(const uint8_t* data, size_t size, const Endpoint& from, double now) {
    Header h;
    const uint8_t* payload = nullptr;
    const DecodeError e = decodePacket(data, size, h, payload);
    if (e != DecodeError::Ok) {
        if (e == DecodeError::BadVersion && h.type == PacketType::HelloAck) {
            if (!versionMismatch_) logLine("[link] %s speaks protocol %u, we speak %u", from.text().c_str(), h.version,
                                           kProtocolVersion);
            versionMismatch_ = true;
        }
        ++malformed_;
        return;
    }
    if (h.type == PacketType::HelloAck) {
        HelloAck ack;
        if (!decodeHelloAck(payload, h.length, ack)) {
            ++malformed_;
            return;
        }
        onHelloAck(ack, from, now);
        return;
    }
    // Everything else only counts from the headset we're talking to (T10: camera BLOBs also from its dongle path)
    const bool cameraPath = cameraPathIp_ != 0 && from.ip == cameraPathIp_ && from.port == port_ &&
                            h.type == PacketType::Blob;
    if (phase_ == LinkPhase::Discovery || (from != headsetEp_ && !cameraPath)) return;
    if (!cameraPath) lastHeard_ = now;
    switch (h.type) {
        case PacketType::SubscribeAck: {
            SubscribeAck ack;
            if (decodeSubscribeAck(payload, h.length, ack)) onSubscribeAck(ack, now);
            else ++malformed_;
            break;
        }
        case PacketType::Metrics: {
            Metrics m;
            if (decodeMetrics(payload, h.length, m)) onMetrics(m, now);
            else ++malformed_;
            break;
        }
        case PacketType::Blob: {
            BlobChunk chunk;
            Blob blob;
            if (!decodeBlobChunk(payload, h.length, chunk)) {
                ++malformed_;
                break;
            }
            if (blobs_.add(from.key(), chunk, now, blob)) onBlob(blob, now);
            break;
        }
        case PacketType::LayoutAck: {
            LayoutAck ack;
            if (decodeLayoutAck(payload, h.length, ack)) onLayoutAck(ack, now);
            else ++malformed_;
            break;
        }
        case PacketType::LayoutReject: {
            LayoutReject reject;
            if (decodeLayoutReject(payload, h.length, reject)) onLayoutReject(reject, now);
            else ++malformed_;
            break;
        }
        case PacketType::Bye:
            backToDiscovery(now, "BYE from the headset");
            break;
        default:
            break;
    }
}

void LinkClient::onHelloAck(const HelloAck& ack, const Endpoint& from, double now) {
    if (phase_ == LinkPhase::Subscribed) return;  // a late answer to an old broadcast
    if (phase_ != LinkPhase::Discovery) {
        // Pending / Denied: only our headset's answers count, from where we talk to it
        if (from != headsetEp_ || ack.headsetId != headset_.headsetId) return;
    }
    lastHeard_ = now;
    const bool newHeadset = phase_ == LinkPhase::Discovery;
    headset_ = ack;
    headsetEp_ = from;
    noteHeadsetVersion(ack.layoutVersion);
    switch (ack.status) {
        case PeerStatus::Allowed: {
            logLine("[link] subscribed to '%s' (%s) at %s, layout v%u", ack.name.c_str(), idToHex(ack.headsetId).c_str(),
                    from.text().c_str(), ack.layoutVersion);
            phase_ = LinkPhase::Subscribed;
            sendSubscribe(now);
            nextLayoutGet_ = now;
            if (pending_ != nullptr) pending_->nextSend = now;  // an edit that outlived the reconnect: resend now
            seen_.idHex = idToHex(ack.headsetId);
            seen_.name = ack.name;
            seen_.ips.clear();
            seen_.ips.push_back(from.ip);
            for (const LocalAddress& a : ack.addresses) {
                if (a.ip != 0 && std::find(seen_.ips.begin(), seen_.ips.end(), a.ip) == seen_.ips.end()) {
                    seen_.ips.push_back(a.ip);
                }
            }
            seenFresh_ = true;
            break;
        }
        case PeerStatus::Pending:
            if (phase_ != LinkPhase::Pending || newHeadset) {
                logLine("[link] '%s' at %s: pending approval", ack.name.c_str(), from.text().c_str());
            }
            phase_ = LinkPhase::Pending;
            nextHello_ = now + kHelloRepeatSec;
            break;
        case PeerStatus::Denied:
        case PeerStatus::UnknownSubscriber:
            if (phase_ != LinkPhase::Denied || newHeadset) {
                logLine("[link] '%s' at %s: %s", ack.name.c_str(), from.text().c_str(), peerStatusName(ack.status));
            }
            phase_ = LinkPhase::Denied;
            nextHello_ = now + kHelloRepeatSec;
            break;
    }
}

void LinkClient::onSubscribeAck(const SubscribeAck& ack, double now) {
    switch (ack.status) {
        case PeerStatus::Allowed: break;
        case PeerStatus::UnknownSubscriber: backToDiscovery(now, "unknown_subscriber"); break;
        case PeerStatus::Pending:
            logLine("[link] lease answered pending (the headset forgot this PC)");
            phase_ = LinkPhase::Pending;
            nextHello_ = now;
            break;
        case PeerStatus::Denied:
            logLine("[link] lease answered denied");
            phase_ = LinkPhase::Denied;
            nextHello_ = now;
            break;
    }
}

void LinkClient::noteHeadsetVersion(uint32_t version) {
    headsetVersion_ = version;
    needLayout_ = !haveLayout_ || !layoutLive_ || layout_.version != version;
}

void LinkClient::onMetrics(const Metrics& m, double now) {
    uint32_t skipped = 0;
    for (const WireValue& v : m.values) {
        if (isPcStreamMetric(v.id)) continue; // The PC owns these; never replace its values with an echo.
        if (!storeWire(store_, v, now)) ++skipped;
    }
    if (skipped) {
        unknownMetricValues_ += skipped;
        logLine("[link] METRICS skipped %u unknown/incompatible metric ids (total %llu)", skipped,
                static_cast<unsigned long long>(unknownMetricValues_));
    }
    if (pending_ == nullptr) noteHeadsetVersion(m.state.layoutVersion);
    headsetState_ = m.state;  // T11: the PC copy of the session logs follows the headset's session
    lastMetricsAt_ = now;
}

void LinkClient::sendPcMetrics(const std::vector<link::WireValue>& values) {
    if (!subscribed()) return;
    PcMetrics m; m.pcId=pcId_; m.snapshotId=seq_+1; m.values=values;
    const auto payload=encodePcMetrics(m);
    if(!payload.empty())sendPacket(headsetEp_,PacketType::PcMetrics,payload);
}

void LinkClient::onBlob(const Blob& blob, double now) {
    (void)now;
    if (blob.kind == BlobKind::Layout) {
        if (pending_ != nullptr) return;  // the edit's answer decides what's shown
        Layout fresh;
        std::vector<std::string> warnings;
        std::string error;
        const std::string text(blob.data.begin(), blob.data.end());
        if (!parseLayoutText(text, fresh, warnings, error)) {
            logLine("[link] unreadable layout from the headset: %s", error.c_str());
            return;
        }
        for (const std::string& w : warnings) logLine("[link] layout: %s", w.c_str());
        const bool changed = !layoutLive_ || !haveLayout_ || fresh.version != layout_.version ||
                             !fresh.sameWidgets(layout_);
        if (changed) {
            logLine("[link] layout v%u from the headset", fresh.version);
            setLayout(fresh, true);
        }
        // A newer version may already be announced: keep asking until we hold it
        needLayout_ = fresh.version != headsetVersion_ && newerId(headsetVersion_, fresh.version);
        if (!needLayout_) headsetVersion_ = fresh.version;
    } else if (blob.kind == BlobKind::Camera) {  // T10
        cameras_.onBlob(blob.stream, blob.data.data(), blob.data.size(), now);
    } else if (blob.kind == BlobKind::ProcessList) {
        ProcessListWire list;
        if (!decodeProcessList(blob.data.data(), blob.data.size(), list)) return;
        std::vector<ProcessItem> rows;
        rows.reserve(list.rows.size());
        for (const ProcessRowWire& r : list.rows) {
            ProcessItem item;
            item.name = r.name;
            item.pid = r.pid;
            item.count = r.count;
            item.cpuCorePct = r.cpuCorePct;
            item.rssMiB = r.rssMiB;
            rows.push_back(item);
        }
        if (blob.stream == kStreamCpuList) {
            processes_.byCpu = std::move(rows);
            processes_.cpuValid = list.cpuValid;
        } else {
            processes_.byRam = std::move(rows);
        }
        ++processes_.generation;
    }
}

void LinkClient::onLayoutAck(const LayoutAck& ack, double now) {
    (void)now;
    if (pending_ == nullptr || ack.requestId != pending_->requestId) return;
    logLine("[link] LAYOUT_ACK request %u: version %u (after %d copies)", ack.requestId, ack.version, pending_->copies);
    Layout applied = layout_;
    applied.version = ack.version;
    pending_.reset();
    setLayout(applied, true);
    noteHeadsetVersion(ack.version);
}

void LinkClient::onLayoutReject(const LayoutReject& reject, double now) {
    if (pending_ == nullptr || reject.requestId != pending_->requestId) return;
    logLine("[link] LAYOUT_REJECT request %u: %s, headset at v%u", reject.requestId, rejectReasonName(reject.reason),
            reject.currentVersion);
    if (reject.reason == RejectReason::UnknownSubscriber) {
        // The headset restarted under us: HELLO again and keep resending the same edit once subscribed
        backToDiscovery(now, "LAYOUT_SET answered unknown_subscriber");
        return;
    }
    const Layout before = pending_->before;
    pending_.reset();
    setLayout(before, layoutLive_);
    switch (reject.reason) {
        case RejectReason::Stale:
            notice_ = "Layout changed in the headset, reloaded. Your last move wasn't applied.";
            break;
        case RejectReason::NotAllowed:
            notice_ = "This PC isn't approved in the headset. Your last move wasn't applied.";
            break;
        default:
            notice_ = "The headset couldn't apply your last move.";
            break;
    }
    // Reload whatever the headset has now
    headsetVersion_ = reject.currentVersion;
    needLayout_ = true;
    nextLayoutGet_ = now;
}

std::string LinkClient::debugText(double now) const {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "phase %s, headset '%s' at %s, heard %.1f s ago, layout v%u (%s), headset v%u, edit %s, "
                  "HELLO %llu, SUBSCRIBE %llu, LAYOUT_GET %llu, edit copies %llu, rediscoveries %llu, malformed %llu, skipped unknown metric ids %llu",
                  linkPhaseName(phase_), headset_.name.c_str(), headsetEp_.text().c_str(),
                  lastHeard_ > 0 ? now - lastHeard_ : -1.0,
                  layout_.version, layoutLive_ ? "live" : "cached", headsetVersion_,
                  pending_ != nullptr ? "pending" : "none", static_cast<unsigned long long>(hellosSent_),
                  static_cast<unsigned long long>(subscribesSent_), static_cast<unsigned long long>(layoutGetsSent_),
                  static_cast<unsigned long long>(editCopiesSent_), static_cast<unsigned long long>(rediscoveries_),
                  static_cast<unsigned long long>(malformed_), static_cast<unsigned long long>(unknownMetricValues_));
    return buffer;
}

}  // namespace pc
}  // namespace fph
