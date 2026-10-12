#include "link_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace fph::link;

namespace {

/** Most chunks a LAYOUT_SET blob may have (~19 KB; the largest layout is ~7 KB). */
constexpr uint16_t kMaxLayoutSetChunks = 16;

/** @return the first 8 hex chars, enough to tell PCs apart in the log */
std::string shortId(const std::string& hexId) {
    return hexId.substr(0, 8);
}

/** @return the name without control chars, cut to kMaxNameBytes */
std::string cleanName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
    }
    out = cutUtf8(out, kMaxNameBytes);
    return out.empty() ? "unnamed PC" : out;
}

}  // namespace

std::string LinkEndpoint::text() const {
    return ipv4ToText(ip) + ":" + std::to_string(port);
}

// ---- LinkServer ----

LinkServer::LinkServer(Config& config, LayoutAuthority& layout, LinkHostEnv& env, std::string name)
    : config_(config), layout_(layout), env_(env), name_(std::move(name)) {
    if (!idFromHex(config_.link.headsetId, headsetId_)) {
        headsetId_ = randomId();
        config_.link.headsetId = idToHex(headsetId_);
        headsetIdIsNew_ = true;  // the owner saves it (LinkService), unless the config file is unreadable
    }
}

void LinkServer::sendPacket(const LinkEndpoint& to, PacketType type, const std::vector<uint8_t>& payload) {
    const std::vector<uint8_t> datagram = encodePacket(type, seq_++, payload);
    if (!datagram.empty()) env_.send(to, datagram);
}

void LinkServer::sendBlob(const LinkEndpoint& to, BlobKind kind, uint8_t stream, const std::vector<uint8_t>& data) {
    for (const std::vector<uint8_t>& chunk : splitBlob(nextBlobId_++, kind, stream, data)) {
        sendPacket(to, PacketType::Blob, chunk);
    }
}

void LinkServer::sendAnswer(const LinkEndpoint& to, const Answer& answer) {
    if (answer.applied) {
        LayoutAck ack;
        ack.requestId = answer.requestId;
        ack.version = answer.version;
        sendPacket(to, PacketType::LayoutAck, encodeLayoutAck(ack));
    } else {
        LayoutReject reject;
        reject.requestId = answer.requestId;
        reject.currentVersion = answer.version;
        reject.reason = answer.reason;
        sendPacket(to, PacketType::LayoutReject, encodeLayoutReject(reject));
    }
}

void LinkServer::handlePacket(const uint8_t* data, size_t size, const LinkEndpoint& from, double now) {
    Header header;
    const uint8_t* payload = nullptr;
    const DecodeError error = decodePacket(data, size, header, payload);
    if (error != DecodeError::Ok) {
        ++malformed_;
        return;
    }
    bool ok = true;
    switch (header.type) {
        case PacketType::Hello: {
            Hello m;
            ok = decodeHello(payload, header.length, m);
            if (ok) onHello(m, from, now);
            break;
        }
        case PacketType::Subscribe: {
            Subscribe m;
            ok = decodeSubscribe(payload, header.length, m);
            if (ok) onSubscribe(m, from, now);
            break;
        }
        case PacketType::LayoutGet: {
            LayoutGet m;
            ok = decodeLayoutGet(payload, header.length, m);
            if (ok) onLayoutGet(m, from, now);
            break;
        }
        case PacketType::Blob: {
            BlobChunk chunk;
            ok = decodeBlobChunk(payload, header.length, chunk);
            // PCs only send LAYOUT_SET blobs; anything else (or an oversized one) is dropped
            if (!ok || chunk.kind != BlobKind::LayoutSet || chunk.count > kMaxLayoutSetChunks) {
                ok = false;
                break;
            }
            Blob blob;
            if (blobs_.add(from.key(), chunk, now, blob)) {
                LayoutSet m;
                ok = decodeLayoutSet(blob.data.data(), blob.data.size(), m);
                if (ok) onLayoutSet(m, from, now);
            }
            break;
        }
        case PacketType::Bye: {
            Bye m;
            ok = decodeBye(payload, header.length, m);
            if (ok) onBye(m, now);
            break;
        }
        case PacketType::PcMetrics:
            break;  // ticket 08
        default:
            ok = false;  // headset -> PC types, or unknown
            break;
    }
    if (!ok) ++malformed_;
}

const std::vector<uint32_t>& LinkServer::hostIps(double now) {
    if (now - hostIpsAt_ >= kHostIpCacheSec) {
        hostIps_ = env_.streamHostIps();
        hostIpsAt_ = now;
        ++hostIpLookups_;
    }
    return hostIps_;
}

bool LinkServer::isAllowedId(const std::string& hexId) const {
    for (const LinkConfig::AllowedPc& pc : config_.link.allowedPcs) {
        if (pc.id == hexId) return true;
    }
    return false;
}

bool LinkServer::isDenied(const std::string& hexId) const {
    return std::find(denied_.begin(), denied_.end(), hexId) != denied_.end();
}

void LinkServer::onHello(const Hello& m, const LinkEndpoint& from, double now) {
    const std::string hex = idToHex(m.pcId);
    // A HELLO starts (or restarts) the conversation: a restarted PC app may reuse its address:port with blob ids
    // from 1 again, which would read as older than the ones completed before
    blobs_.forgetPeer(from.key());
    const bool isNew = peers_.find(hex) == peers_.end();
    Peer& peer = peers_[hex];
    if (isNew) {
        peer.id = m.pcId;
        peer.firstSeen = now;
    }
    const PeerStatus before = isNew ? PeerStatus::UnknownSubscriber : peer.status;
    peer.name = cleanName(m.pcName);
    peer.address = from;
    peer.lastSeen = now;

    // Allow-list, in order: denied this run, allowed_pcs, Steam Link host IP (looked up now, cached 5 s)
    peer.autoAllowed = false;
    if (isDenied(hex)) {
        peer.status = PeerStatus::Denied;
    } else if (isAllowedId(hex)) {
        peer.status = PeerStatus::Allowed;
    } else {
        const std::vector<uint32_t>& hosts = hostIps(now);
        if (std::find(hosts.begin(), hosts.end(), from.ip) != hosts.end()) {
            peer.status = PeerStatus::Allowed;
            peer.autoAllowed = true;
        } else {
            peer.status = PeerStatus::Pending;
        }
    }
    if (peer.status != before) {
        std::fprintf(stderr, "[link] PC '%s' (%s) at %s: %s%s\n", peer.name.c_str(), shortId(hex).c_str(),
                     from.text().c_str(), peerStatusName(peer.status),
                     peer.autoAllowed ? " (Steam Link host)" : "");
        if (before == PeerStatus::Pending || peer.status == PeerStatus::Pending) ++uiRevision_;
        if (peer.status != PeerStatus::Allowed && peer.leased) dropLease(peer, "no longer allowed");
    }

    HelloAck ack;
    ack.headsetId = headsetId_;
    ack.name = name_;
    ack.status = peer.status;
    ack.addresses = env_.localAddresses();
    ack.layoutVersion = layout_.version();
    ack.seenIp = from.ip;
    ack.seenPort = from.port;
    sendPacket(from, PacketType::HelloAck, encodeHelloAck(ack));
}

void LinkServer::onSubscribe(const Subscribe& m, const LinkEndpoint& from, double now) {
    SubscribeAck ack;
    auto it = peers_.find(idToHex(m.pcId));
    if (it == peers_.end()) {
        // Never said HELLO to this process (e.g. SteamVR restarted us): the PC goes back to discovery
        ack.status = PeerStatus::UnknownSubscriber;
        sendPacket(from, PacketType::SubscribeAck, encodeSubscribeAck(ack));
        return;
    }
    Peer& peer = it->second;
    peer.lastSeen = now;
    peer.address = from;
    ack.status = peer.status;
    if (peer.status == PeerStatus::Allowed) {
        if (!peer.leased || peer.leaseAddress != from) {
            std::fprintf(stderr, "[link] PC '%s' (%s) %s from %s (wants 0x%x)\n", peer.name.c_str(),
                         shortId(it->first).c_str(), peer.leased ? "moved its subscription" : "subscribed",
                         from.text().c_str(), m.wants);
        }
        if (!peer.leased) peer.sentListGeneration = 0;  // a new lease gets the current lists
        peer.leased = true;
        peer.leaseUntil = now + kLeaseSec;
        peer.wants = m.wants;
        peer.leaseAddress = from;
    }
    sendPacket(from, PacketType::SubscribeAck, encodeSubscribeAck(ack));
}

void LinkServer::onLayoutGet(const LayoutGet& m, const LinkEndpoint& from, double now) {
    auto it = peers_.find(idToHex(m.pcId));
    if (it == peers_.end() || it->second.status != PeerStatus::Allowed) return;  // no layout for strangers
    it->second.lastSeen = now;
    const std::string json = fph::layoutToJson(layout_.layout(), 0);
    sendBlob(from, BlobKind::Layout, 0, std::vector<uint8_t>(json.begin(), json.end()));
}

void LinkServer::onLayoutSet(const LayoutSet& m, const LinkEndpoint& from, double now) {
    const std::string hex = idToHex(m.pcId);
    auto it = peers_.find(hex);
    Answer answer;
    answer.requestId = m.requestId;
    answer.version = layout_.version();
    if (it == peers_.end()) {
        answer.reason = RejectReason::UnknownSubscriber;
        sendAnswer(from, answer);
        return;
    }
    Peer& peer = it->second;
    peer.lastSeen = now;
    if (peer.status == PeerStatus::Denied) return;
    if (peer.status != PeerStatus::Allowed) {
        answer.reason = RejectReason::NotAllowed;
        sendAnswer(from, answer);
        return;
    }
    // A resend of a request already answered: the same answer again, never applied twice
    for (const Answer& old : peer.answers) {
        if (old.requestId == m.requestId) {
            sendAnswer(from, old);
            return;
        }
    }
    fph::Layout layout;
    std::vector<std::string> warnings;
    std::string error;
    if (!fph::parseLayoutText(m.layoutJson, layout, warnings, error)) {
        std::fprintf(stderr, "[link] PC '%s' sent a layout that can't be read: %s\n", peer.name.c_str(),
                     error.c_str());
        answer.reason = RejectReason::InvalidLayout;
    } else {
        for (const std::string& w : warnings) std::fprintf(stderr, "[link] layout from '%s': %s\n", peer.name.c_str(), w.c_str());
        const LayoutAuthority::Result result = layout_.trySetLayout(m.baseVersion, layout);
        answer.version = result.version;
        answer.applied = result.applied();
        if (result.outcome == LayoutAuthority::Outcome::Stale) answer.reason = RejectReason::Stale;
        if (result.outcome == LayoutAuthority::Outcome::SaveFailed) answer.reason = RejectReason::SaveFailed;
        if (answer.applied) {
            ++layoutApplies_;
            std::fprintf(stderr, "[link] Layout edit %u from PC '%s' applied: version %u\n", m.requestId,
                         peer.name.c_str(), answer.version);
        } else {
            std::fprintf(stderr, "[link] Layout edit %u from PC '%s' rejected (%s): base %u, current %u\n",
                         m.requestId, peer.name.c_str(), rejectReasonName(answer.reason), m.baseVersion,
                         answer.version);
        }
    }
    peer.answers.push_back(answer);
    while (peer.answers.size() > kLayoutAnswerMemory) peer.answers.pop_front();
    sendAnswer(from, answer);
}

void LinkServer::onBye(const Bye& m, double now) {
    auto it = peers_.find(idToHex(m.id));
    if (it == peers_.end()) return;
    it->second.lastSeen = now;
    if (it->second.leased) dropLease(it->second, "said BYE");
}

void LinkServer::dropLease(Peer& peer, const char* why) {
    peer.leased = false;
    peer.wants = 0;
    std::fprintf(stderr, "[link] PC '%s' (%s) lease ended: %s\n", peer.name.c_str(),
                 shortId(idToHex(peer.id)).c_str(), why);
}

bool LinkServer::hasLease() const {
    for (const auto& kv : peers_) {
        if (kv.second.leased) return true;
    }
    return false;
}

void LinkServer::tick(double now) {
    blobs_.expire(now);
    if (peers_.empty()) return;  // idle: nothing to expire, nothing to send
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& peer = it->second;
        if (peer.leased && now > peer.leaseUntil) dropLease(peer, "not renewed for 6 s");
        if (!peer.leased && now - peer.lastSeen > kPeerIdleSec) {
            if (peer.status == PeerStatus::Pending) ++uiRevision_;
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    // One snapshot per tick, shared by every lease holder
    bool wantMetrics = false, wantLists = false;
    for (const auto& kv : peers_) {
        if (!kv.second.leased) continue;
        wantMetrics |= (kv.second.wants & kWantMetrics) != 0;
        wantLists |= (kv.second.wants & (kWantCpuList | kWantRamList)) != 0;
    }
    std::vector<std::vector<uint8_t>> metricParts;
    if (wantMetrics) {
        LinkState state = env_.linkState();
        state.layoutVersion = layout_.version();
        const uint64_t ms = static_cast<uint64_t>(std::llround(now * 1000.0));
        metricParts = encodeMetrics(ms, ++snapshotId_, state, env_.metricValues());
    }
    ProcessListWire cpu, ram;
    const uint64_t generation = wantLists ? env_.processLists(cpu, ram) : 0;
    std::vector<uint8_t> cpuBlob, ramBlob;
    for (auto& kv : peers_) {
        Peer& peer = kv.second;
        if (!peer.leased) continue;
        if (peer.wants & kWantMetrics) {
            for (const std::vector<uint8_t>& part : metricParts) sendPacket(peer.leaseAddress, PacketType::Metrics, part);
            ++metricsSent_;
        }
        if (generation != 0 && generation != peer.sentListGeneration) {
            if (peer.wants & kWantCpuList) {
                if (cpuBlob.empty()) cpuBlob = encodeProcessList(cpu);
                sendBlob(peer.leaseAddress, BlobKind::ProcessList, kStreamCpuList, cpuBlob);
            }
            if (peer.wants & kWantRamList) {
                if (ramBlob.empty()) ramBlob = encodeProcessList(ram);
                sendBlob(peer.leaseAddress, BlobKind::ProcessList, kStreamRamList, ramBlob);
            }
            peer.sentListGeneration = generation;
        }
    }
}

void LinkServer::sayGoodbye() {
    Bye bye;
    bye.id = headsetId_;
    for (auto& kv : peers_) {
        if (kv.second.leased) sendPacket(kv.second.leaseAddress, PacketType::Bye, encodeBye(bye));
    }
}

LinkUiState LinkServer::uiState() const {
    LinkUiState state;
    std::vector<const Peer*> pending;
    for (const auto& kv : peers_) {
        if (kv.second.status == PeerStatus::Pending) pending.push_back(&kv.second);
    }
    std::sort(pending.begin(), pending.end(), [](const Peer* a, const Peer* b) { return a->firstSeen < b->firstSeen; });
    for (const Peer* p : pending) state.pending.push_back({idToHex(p->id), p->name});
    state.knownPcs = config_.link.allowedPcs.size();
    state.revision = uiRevision_ * 1000003u + state.knownPcs;  // also moves when a reload changes the list
    return state;
}

void LinkServer::allow(const std::string& id) {
    auto it = peers_.find(id);
    if (it == peers_.end() || it->second.status != PeerStatus::Pending) return;
    it->second.status = PeerStatus::Allowed;
    if (!isAllowedId(id)) config_.link.allowedPcs.push_back({id, it->second.name});
    ++uiRevision_;
    std::string error;
    if (!env_.saveConfig(error)) {
        std::fprintf(stderr, "[link] Allowed PC '%s' for now, but saving failed: %s\n", it->second.name.c_str(),
                     error.c_str());
    } else {
        std::fprintf(stderr, "[link] Allowed PC '%s' (%s)\n", it->second.name.c_str(), shortId(id).c_str());
    }
}

void LinkServer::deny(const std::string& id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    if (!isDenied(id)) denied_.push_back(id);
    if (it->second.leased) dropLease(it->second, "denied");
    if (it->second.status == PeerStatus::Pending) ++uiRevision_;
    it->second.status = PeerStatus::Denied;
    std::fprintf(stderr, "[link] Denied PC '%s' (%s) until the HUD restarts\n", it->second.name.c_str(),
                 shortId(id).c_str());
}

void LinkServer::forgetAll() {
    const size_t n = config_.link.allowedPcs.size();
    config_.link.allowedPcs.clear();
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (it->second.status == PeerStatus::Allowed && !it->second.autoAllowed) {
            if (it->second.leased) dropLease(it->second, "PCs forgotten");
            it = peers_.erase(it);  // its next SUBSCRIBE gets unknown_subscriber, so it asks again
        } else {
            ++it;
        }
    }
    ++uiRevision_;
    std::string error;
    if (!env_.saveConfig(error)) std::fprintf(stderr, "[link] Forgot %zu PC(s), but saving failed: %s\n", n, error.c_str());
    else std::fprintf(stderr, "[link] Forgot %zu allowed PC(s)\n", n);
}

std::string LinkServer::debugText(double now) const {
    std::string text = "== Link ==\nheadset id " + shortId(config_.link.headsetId) + "…, layout version " +
                       std::to_string(layout_.version()) + ", allowed_pcs " +
                       std::to_string(config_.link.allowedPcs.size()) + ", malformed packets " +
                       std::to_string(malformed_) + ", METRICS snapshots sent " + std::to_string(metricsSent_) +
                       ", layout edits applied " + std::to_string(layoutApplies_) + ", vrlink lookups " +
                       std::to_string(hostIpLookups_) + ", blobs in progress " +
                       std::to_string(blobs_.inProgress()) + "\n";
    for (const auto& kv : peers_) {
        const Peer& p = kv.second;
        char line[256];
        std::snprintf(line, sizeof(line), "PC '%s' %s at %s: %s%s, %s, seen %.1f s ago\n", p.name.c_str(),
                      shortId(kv.first).c_str(), p.address.text().c_str(), peerStatusName(p.status),
                      p.autoAllowed ? " (Steam Link host)" : "",
                      p.leased ? ("lease " + std::to_string(p.leaseUntil - now).substr(0, 4) + " s left").c_str()
                               : "no lease",
                      now - p.lastSeen);
        text += line;
    }
    return text;
}

// ---- LinkSocket ----

LinkSocket::~LinkSocket() {
    close();
}

bool LinkSocket::open(uint16_t port, std::string& error) {
    close();
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "bind UDP " + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

void LinkSocket::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void LinkSocket::send(const LinkEndpoint& to, const std::vector<uint8_t>& datagram) {
    if (fd_ < 0) return;
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(to.ip);
    if (::sendto(fd_, datagram.data(), datagram.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
        ++sendErrors_;
    }
}

void LinkSocket::receiveAll(LinkServer& server, double now) {
    if (fd_ < 0) return;
    uint8_t buffer[2048];
    for (int i = 0; i < 256; ++i) {
        sockaddr_in addr {};
        socklen_t len = sizeof(addr);
        const ssize_t n = ::recvfrom(fd_, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&addr), &len);
        if (n < 0) return;  // EAGAIN: drained (other errors: try again on the next wake)
        if (addr.sin_family != AF_INET) continue;
        LinkEndpoint from;
        from.ip = ntohl(addr.sin_addr.s_addr);
        from.port = ntohs(addr.sin_port);
        server.handlePacket(buffer, static_cast<size_t>(n), from, now);
    }
}

// ---- vrlink host IPs ----

std::vector<uint32_t> parseVrlinkHostIps(const char* cmdline, size_t length) {
    std::vector<uint32_t> out;
    const auto addValue = [&out](const std::string& value) {
        // Accept "ip", "ip:port" and comma / semicolon separated lists of those
        size_t start = 0;
        while (start <= value.size()) {
            size_t end = value.find_first_of(",;", start);
            if (end == std::string::npos) end = value.size();
            std::string token = value.substr(start, end - start);
            const size_t colon = token.find(':');
            if (colon != std::string::npos) token.resize(colon);
            uint32_t ip = 0;
            if (ipv4FromText(token, ip) && std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
            start = end + 1;
        }
    };
    bool next = false;
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && cmdline[i] != '\0') continue;
        const std::string arg(cmdline + start, i - start);
        if (next) {
            addValue(arg);
            next = false;
        } else if (arg == "-c") {
            next = true;
        } else if (arg.rfind("-c=", 0) == 0) {
            addValue(arg.substr(3));
        }
        start = i + 1;
    }
    return out;
}

std::vector<uint32_t> readVrlinkHostIps(const std::string& procRoot) {
    DIR* dir = ::opendir(procRoot.c_str());
    if (dir == nullptr) return {};
    std::vector<uint32_t> ips;
    char path[512];
    while (dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '1' || name[0] > '9') continue;
        // Cheap filter first: vrlink's comm starts with "vrlink" (it renames its main thread "vrlinkrunthread")
        std::snprintf(path, sizeof(path), "%s/%s/comm", procRoot.c_str(), name);
        char comm[32] = {};
        const int commFd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (commFd < 0) continue;
        const ssize_t commLen = ::read(commFd, comm, sizeof(comm) - 1);
        ::close(commFd);
        if (commLen < 6 || std::strncmp(comm, "vrlink", 6) != 0) continue;
        // Then the exe's basename
        std::snprintf(path, sizeof(path), "%s/%s/exe", procRoot.c_str(), name);
        char target[512];
        const ssize_t n = ::readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        std::string exe(target);
        const std::string deleted = " (deleted)";
        if (exe.size() > deleted.size() && exe.compare(exe.size() - deleted.size(), deleted.size(), deleted) == 0) {
            exe.resize(exe.size() - deleted.size());
        }
        const size_t slash = exe.rfind('/');
        if (exe.substr(slash == std::string::npos ? 0 : slash + 1) != "vrlink") continue;
        // The cmdline holds a secret (-e): it's parsed here and never logged or kept
        std::snprintf(path, sizeof(path), "%s/%s/cmdline", procRoot.c_str(), name);
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        std::vector<char> cmdline(16384);
        size_t length = 0;
        ssize_t got = 0;
        while (length < cmdline.size() && (got = ::read(fd, cmdline.data() + length, cmdline.size() - length)) > 0) {
            length += static_cast<size_t>(got);
        }
        ::close(fd);
        for (uint32_t ip : parseVrlinkHostIps(cmdline.data(), length)) {
            if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
        }
        std::fill(cmdline.begin(), cmdline.end(), '\0');
    }
    ::closedir(dir);
    return ips;
}

std::vector<LocalAddress> readLocalAddresses() {
    std::vector<LocalAddress> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        LocalAddress address;
        address.ifName = a->ifa_name != nullptr ? a->ifa_name : "";
        address.ip = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
        out.push_back(address);
    }
    ::freeifaddrs(list);
    return out;
}
