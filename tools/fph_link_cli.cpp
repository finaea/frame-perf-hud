// fph-link-cli: a tiny client for the headset link (ticket 06), so the protocol can be tested before the PC app
// (ticket 07). Builds on Windows (Winsock) and Linux; links fph-core only.
//
//   fph-link-cli hello HOST...                 HELLO each host ("broadcast" = 255.255.255.255), print the answers
//   fph-link-cli watch HOST [--seconds N] [--lists] [--all]
//                                              subscribe and print every METRICS snapshot; re-HELLO on
//                                              unknown_subscriber, BYE or 6 s of silence
//   fph-link-cli layout-get HOST               print the headset's layout JSON
//   fph-link-cli layout-set HOST (FILE | --touch) [--base N] [--drop-first] [--dup] [--request ID]
//                                              compare-and-set a layout, resending every 1 s until ACK / REJECT
//   fph-link-cli bye HOST
// Options: --port P (47630), --id HEX (else a saved one), --name NAME (else the computer name),
//          --id-file PATH (where the saved id lives)
#include "layout.h"
#include "link_wire.h"
#include "metric_registry.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static const SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle kNoSocket = -1;
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace fph::link;

namespace {

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Endpoint {
    uint32_t ip = 0;  ///< host order
    uint16_t port = 0;
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    std::string text() const { return ipv4ToText(ip) + ":" + std::to_string(port); }
    uint64_t key() const { return static_cast<uint64_t>(ip) << 16 | port; }
};

/** One UDP socket on an ephemeral port, used for everything (replies stay inside the stateful firewall). */
class Udp {
public:
    bool open() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == kNoSocket) return false;
        int one = 1;
        ::setsockopt(s_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return ::bind(s_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    ~Udp() {
        if (s_ != kNoSocket) {
#ifdef _WIN32
            ::closesocket(s_);
            WSACleanup();
#else
            ::close(s_);
#endif
        }
    }
    void send(const Endpoint& to, const std::vector<uint8_t>& data) {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(to.port);
        addr.sin_addr.s_addr = htonl(to.ip);
        ::sendto(s_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
    /** Waits up to timeout for one datagram. @return false on timeout */
    bool receive(double timeout, std::vector<uint8_t>& data, Endpoint& from) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s_, &set);
        timeval tv {};
        timeout = std::max(0.0, timeout);
        tv.tv_sec = static_cast<long>(timeout);
        tv.tv_usec = static_cast<long>((timeout - static_cast<double>(tv.tv_sec)) * 1e6);
        if (::select(static_cast<int>(s_ + 1), &set, nullptr, nullptr, &tv) <= 0) return false;
        char buffer[2048];
        sockaddr_in addr {};
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        const int n = static_cast<int>(
            ::recvfrom(s_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&addr), &len));
        if (n < 0) return false;  // e.g. Windows' WSAECONNRESET after an ICMP port unreachable: no headset there
        data.assign(buffer, buffer + n);
        from.ip = ntohl(addr.sin_addr.s_addr);
        from.port = ntohs(addr.sin_port);
        return true;
    }

private:
    SocketHandle s_ = kNoSocket;
};

struct Cli {
    std::string command;
    std::vector<std::string> hosts;
    std::string file;
    uint16_t port = kDefaultPort;
    std::string idHex;
    std::string name;
    std::string idFile;
    double seconds = 0;  ///< watch: 0 = until Ctrl+C
    bool lists = false;
    bool all = false;
    bool touch = false;
    bool dropFirst = false;
    bool dup = false;
    long long base = -1;
    long long request = -1;
};

std::string computerName() {
#ifdef _WIN32
    const char* n = std::getenv("COMPUTERNAME");
#else
    char buffer[256] = {};
    const char* n = ::gethostname(buffer, sizeof(buffer) - 1) == 0 ? buffer : nullptr;
#endif
    return n != nullptr && n[0] ? std::string(n) : "fph-link-cli";
}

std::string defaultIdFile() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return std::string(home != nullptr ? home : ".") + "/.fph-link-cli-id";
}

/** The PC id: --id, else the saved one, else a new one (saved). */
Id128 pcId(const Cli& cli) {
    Id128 id {};
    if (!cli.idHex.empty()) {
        if (idFromHex(cli.idHex, id)) return id;
        std::fprintf(stderr, "--id must be 32 hex chars\n");
        std::exit(2);
    }
    const std::string path = cli.idFile.empty() ? defaultIdFile() : cli.idFile;
    std::ifstream in(path);
    std::string text;
    if (in >> text && idFromHex(text, id)) return id;
    id = randomId();
    std::ofstream(path) << idToHex(id) << "\n";
    std::fprintf(stderr, "New PC id %s saved in %s\n", idToHex(id).c_str(), path.c_str());
    return id;
}

bool resolveHost(const std::string& host, uint16_t port, Endpoint& out) {
    out.port = port;
    if (host == "broadcast") {
        out.ip = 0xFFFFFFFFu;
        return true;
    }
    if (ipv4FromText(host, out.ip)) return true;
    std::fprintf(stderr, "%s: give an IPv4 address (or \"broadcast\")\n", host.c_str());
    return false;
}

/** The session: one socket, one headset, seq numbers, blob reassembly. */
struct Session {
    Udp udp;
    Cli cli;
    Id128 id {};
    uint32_t seq = 0;
    uint32_t blobId = 1;
    BlobAssembler blobs;

    void send(const Endpoint& to, PacketType type, const std::vector<uint8_t>& payload) {
        udp.send(to, encodePacket(type, seq++, payload));
    }
    void sendHello(const Endpoint& to) {
        Hello hello;
        hello.pcId = id;
        hello.pcName = cli.name;
        send(to, PacketType::Hello, encodeHello(hello));
    }
    void sendSubscribe(const Endpoint& to) {
        Subscribe sub;
        sub.pcId = id;
        sub.wants = kWantMetrics | (cli.lists ? kWantCpuList | kWantRamList : 0);
        send(to, PacketType::Subscribe, encodeSubscribe(sub));
    }
};

void printHelloAck(const HelloAck& ack, const Endpoint& from) {
    std::printf("HELLO_ACK from %s: headset '%s' id %s, protocol %u, status %s, layout version %u, sees us as %s:%u\n",
                from.text().c_str(), ack.name.c_str(), idToHex(ack.headsetId).c_str(), ack.protocol,
                peerStatusName(ack.status), ack.layoutVersion, ipv4ToText(ack.seenIp).c_str(), ack.seenPort);
    for (const LocalAddress& a : ack.addresses) std::printf("    %s %s\n", a.ifName.c_str(), ipv4ToText(a.ip).c_str());
    std::fflush(stdout);
}

/**
 * HELLOs until an Allowed HELLO_ACK (repeating every 2 s, also while pending), or the timeout.
 * @param headset updated to the answering address (a broadcast gets a unicast answer)
 */
bool helloUntilAllowed(Session& s, Endpoint& headset, double timeout, uint32_t* layoutVersion = nullptr) {
    // Back to discovery: a restarted headset numbers its blobs from 1 again, so forget what was completed before
    s.blobs = BlobAssembler();
    const double end = nowSec() + timeout;
    double nextHello = 0;
    PeerStatus last = PeerStatus::UnknownSubscriber;
    while (nowSec() < end) {
        if (nowSec() >= nextHello) {
            s.sendHello(headset);
            nextHello = nowSec() + kHelloRepeatSec;
        }
        std::vector<uint8_t> data;
        Endpoint from;
        if (!s.udp.receive(std::min(nextHello, end) - nowSec(), data, from)) continue;
        Header h;
        const uint8_t* payload = nullptr;
        HelloAck ack;
        if (decodePacket(data.data(), data.size(), h, payload) != DecodeError::Ok || h.type != PacketType::HelloAck ||
            !decodeHelloAck(payload, h.length, ack)) {
            continue;
        }
        if (ack.status != last) printHelloAck(ack, from);
        last = ack.status;
        if (ack.status == PeerStatus::Allowed) {
            headset = from;
            if (layoutVersion != nullptr) *layoutVersion = ack.layoutVersion;
            return true;
        }
        if (ack.status == PeerStatus::Pending) {
            std::printf("Pending: approve this PC ('%s') in the headset's dashboard\n", s.cli.name.c_str());
            std::fflush(stdout);
        }
    }
    std::printf("No allowed answer within %.0f s\n", timeout);
    return false;
}

int cmdHello(Session& s) {
    std::vector<Endpoint> targets;
    for (const std::string& h : s.cli.hosts) {
        Endpoint e;
        if (!resolveHost(h, s.cli.port, e)) return 2;
        targets.push_back(e);
    }
    for (const Endpoint& e : targets) {
        s.sendHello(e);
        std::printf("HELLO -> %s\n", e.text().c_str());
    }
    std::fflush(stdout);
    int answers = 0;
    const double end = nowSec() + 3.0;
    while (nowSec() < end) {
        std::vector<uint8_t> data;
        Endpoint from;
        if (!s.udp.receive(end - nowSec(), data, from)) continue;
        Header h;
        const uint8_t* payload = nullptr;
        HelloAck ack;
        const DecodeError e = decodePacket(data.data(), data.size(), h, payload);
        if (e == DecodeError::Ok && h.type == PacketType::HelloAck && decodeHelloAck(payload, h.length, ack)) {
            printHelloAck(ack, from);
            ++answers;
        }
    }
    std::printf("%d answer(s)\n", answers);
    return answers > 0 ? 0 : 1;
}

std::string valueText(const WireValue& v) {
    if (!v.ok) return std::string("N/A (") + fph::reasonText(v.reason) + ")";
    char buffer[64];
    switch (v.type) {
        case fph::WireType::F32: std::snprintf(buffer, sizeof(buffer), "%.2f", v.f32); return buffer;
        case fph::WireType::I32: return std::to_string(v.i32);
        case fph::WireType::Enum: return std::to_string(v.enumValue);
        case fph::WireType::ShortString: return "\"" + v.text + "\"";
    }
    return "?";
}

int cmdWatch(Session& s) {
    Endpoint headset;
    if (!resolveHost(s.cli.hosts[0], s.cli.port, headset)) return 2;
    const Endpoint configured = headset;
    const double start = nowSec();
    const auto timeLeft = [&]() { return s.cli.seconds > 0 ? start + s.cli.seconds - nowSec() : 1e9; };
    while (timeLeft() > 0) {
        headset = configured;
        if (!helloUntilAllowed(s, headset, std::min(30.0, timeLeft()))) continue;
        std::printf("[%.1f s] subscribed to %s\n", nowSec() - start, headset.text().c_str());
        std::fflush(stdout);
        double nextSubscribe = 0;
        double lastHeard = nowSec();
        uint32_t lastSnapshot = 0;
        size_t snapshotValues = 0;
        int snapshotParts = 0;
        bool rehello = false;
        while (!rehello && timeLeft() > 0) {
            if (nowSec() >= nextSubscribe) {
                s.sendSubscribe(headset);
                nextSubscribe = nowSec() + kSubscribeEverySec;
            }
            if (nowSec() - lastHeard > kLeaseSec) {
                std::printf("[%.1f s] silence for %.0f s: back to discovery\n", nowSec() - start, kLeaseSec);
                rehello = true;
                break;
            }
            std::vector<uint8_t> data;
            Endpoint from;
            if (!s.udp.receive(std::min(nextSubscribe - nowSec(), 0.5), data, from)) continue;
            Header h;
            const uint8_t* payload = nullptr;
            if (decodePacket(data.data(), data.size(), h, payload) != DecodeError::Ok) {
                std::printf("dropped a malformed packet from %s\n", from.text().c_str());
                continue;
            }
            lastHeard = nowSec();
            if (h.type == PacketType::SubscribeAck) {
                SubscribeAck ack;
                if (decodeSubscribeAck(payload, h.length, ack) && ack.status != PeerStatus::Allowed) {
                    std::printf("[%.1f s] SUBSCRIBE_ACK %s: back to discovery\n", nowSec() - start,
                                peerStatusName(ack.status));
                    rehello = true;
                }
            } else if (h.type == PacketType::Bye) {
                std::printf("[%.1f s] BYE from the headset: back to discovery\n", nowSec() - start);
                rehello = true;
            } else if (h.type == PacketType::Metrics) {
                Metrics m;
                if (!decodeMetrics(payload, h.length, m)) {
                    std::printf("bad METRICS\n");
                    continue;
                }
                if (m.snapshotId != lastSnapshot) {
                    lastSnapshot = m.snapshotId;
                    snapshotValues = 0;
                    snapshotParts = 0;
                }
                snapshotValues += m.values.size();
                ++snapshotParts;
                int ok = 0;
                for (const WireValue& v : m.values) ok += v.ok ? 1 : 0;
                std::printf("[%.1f s] METRICS #%u part %u/%u: %zu values (%d ok) | layout v%u, session %d '%s' (%s) | "
                            "headset t=%.1f s\n",
                            nowSec() - start, m.snapshotId, m.part + 1, m.partCount, m.values.size(), ok,
                            m.state.layoutVersion, m.state.sessionId, m.state.appName.c_str(), m.state.appKey.c_str(),
                            m.headsetMs / 1000.0);
                for (const WireValue& v : m.values) {
                    const bool headline = v.id == fph::metric::kCpuUsagePct || v.id == fph::metric::kBatteryPct ||
                                          v.id == fph::metric::kPowerVphW || v.id == fph::metric::kFrameAppFps;
                    if (!s.cli.all && !headline) continue;
                    const fph::MetricDef* def = fph::findMetric(v.id);
                    std::printf("    %5u %-28s %s %s\n", v.id, def != nullptr ? def->name : "?", valueText(v).c_str(),
                                def != nullptr && v.ok ? def->unit : "");
                }
                std::fflush(stdout);
            } else if (h.type == PacketType::Blob) {
                BlobChunk chunk;
                Blob blob;
                if (decodeBlobChunk(payload, h.length, chunk) && s.blobs.add(from.key(), chunk, nowSec(), blob) &&
                    blob.kind == BlobKind::ProcessList) {
                    ProcessListWire list;
                    if (decodeProcessList(blob.data.data(), blob.data.size(), list)) {
                        std::printf("[%.1f s] %s list (scan %llu, %zu rows):", nowSec() - start,
                                    blob.stream == kStreamCpuList ? "CPU" : "RAM",
                                    static_cast<unsigned long long>(list.generation), list.rows.size());
                        for (size_t i = 0; i < list.rows.size() && i < 3; ++i) {
                            const ProcessRowWire& r = list.rows[i];
                            std::printf(" %s %.1f%%/%.0fMiB;", r.name.c_str(), r.cpuCorePct, r.rssMiB);
                        }
                        std::printf("\n");
                        std::fflush(stdout);
                    }
                }
            }
            s.blobs.expire(nowSec());
        }
    }
    // --seconds ran out: end the lease now instead of letting it expire (Ctrl+C / kill leaves it to expire)
    Bye bye;
    bye.id = s.id;
    s.send(headset, PacketType::Bye, encodeBye(bye));
    return 0;
}

/** LAYOUT_GET until the Layout blob arrives. @return false on timeout */
bool fetchLayout(Session& s, const Endpoint& headset, std::string& json, double timeout = 10.0) {
    const double end = nowSec() + timeout;
    double nextGet = 0;
    while (nowSec() < end) {
        if (nowSec() >= nextGet) {
            LayoutGet get;
            get.pcId = s.id;
            s.send(headset, PacketType::LayoutGet, encodeLayoutGet(get));
            nextGet = nowSec() + kLayoutResendSec;
        }
        std::vector<uint8_t> data;
        Endpoint from;
        if (!s.udp.receive(nextGet - nowSec(), data, from)) continue;
        Header h;
        const uint8_t* payload = nullptr;
        BlobChunk chunk;
        Blob blob;
        if (decodePacket(data.data(), data.size(), h, payload) == DecodeError::Ok && h.type == PacketType::Blob &&
            decodeBlobChunk(payload, h.length, chunk) && s.blobs.add(from.key(), chunk, nowSec(), blob) &&
            blob.kind == BlobKind::Layout) {
            json.assign(blob.data.begin(), blob.data.end());
            return true;
        }
    }
    return false;
}

int cmdLayoutGet(Session& s) {
    Endpoint headset;
    if (!resolveHost(s.cli.hosts[0], s.cli.port, headset) || !helloUntilAllowed(s, headset, 20.0)) return 1;
    std::string json;
    if (!fetchLayout(s, headset, json)) {
        std::printf("No layout within 10 s\n");
        return 1;
    }
    std::printf("%s\n", json.c_str());
    return 0;
}

int cmdLayoutSet(Session& s) {
    Endpoint headset;
    if (!resolveHost(s.cli.hosts[0], s.cli.port, headset) || !helloUntilAllowed(s, headset, 20.0)) return 1;
    std::string json;
    fph::Layout layout;
    std::vector<std::string> warnings;
    std::string error;
    if (s.cli.touch) {
        // A harmless visible edit: flip "sum same name" on the first process list widget (or add a General widget)
        if (!fetchLayout(s, headset, json) || !fph::parseLayoutText(json, layout, warnings, error)) {
            std::printf("Couldn't fetch the current layout\n");
            return 1;
        }
        bool done = false;
        for (fph::LayoutColumn& c : layout.columns) {
            for (fph::WidgetInstance& w : c.widgets) {
                if (!done && (w.type == fph::WidgetType::CpuItems || w.type == fph::WidgetType::RamItems)) {
                    w.sumSameName = !w.sumSameName;
                    done = true;
                }
            }
        }
        if (!done) {
            if (layout.columns.empty()) layout.columns.resize(1);
            fph::WidgetInstance general;
            general.type = fph::WidgetType::General;
            layout.columns[0].widgets.push_back(general);
        }
        if (s.cli.base < 0) s.cli.base = layout.version;
    } else {
        std::ifstream in(s.cli.file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        if (!in || !fph::parseLayoutText(text.str(), layout, warnings, error)) {
            std::printf("%s: %s\n", s.cli.file.c_str(), in ? error.c_str() : "can't read");
            return 2;
        }
        if (s.cli.base < 0) {
            std::string current;
            fph::Layout currentLayout;
            if (!fetchLayout(s, headset, current) || !fph::parseLayoutText(current, currentLayout, warnings, error)) {
                std::printf("Couldn't fetch the current version (give --base N)\n");
                return 1;
            }
            s.cli.base = currentLayout.version;
        }
    }
    LayoutSet set;
    set.pcId = s.id;
    std::random_device rd;
    set.requestId = s.cli.request >= 0 ? static_cast<uint32_t>(s.cli.request) : rd();
    set.baseVersion = static_cast<uint32_t>(s.cli.base);
    set.layoutJson = fph::layoutToJson(layout, 0);
    const std::vector<uint8_t> blob = encodeLayoutSet(set);
    std::printf("LAYOUT_SET request %u, base %u, %zu bytes of JSON%s\n", set.requestId, set.baseVersion,
                set.layoutJson.size(), s.cli.dropFirst ? " (dropping the first copy)" : "");
    std::fflush(stdout);

    int copies = 0;
    int answersWanted = s.cli.dup ? 2 : 1;
    int answers = 0;
    int result = 1;
    const double end = nowSec() + 15.0;
    double nextSend = 0;
    while (nowSec() < end && answers < answersWanted) {
        if (nowSec() >= nextSend) {
            ++copies;
            if (s.cli.dropFirst && copies == 1) {
                std::printf("  copy 1: not sent (simulated loss)\n");
            } else {
                // Every resend is a new blob (new blob id), same request id
                for (const std::vector<uint8_t>& chunk : splitBlob(s.blobId++, BlobKind::LayoutSet, 0, blob)) {
                    s.send(headset, PacketType::Blob, chunk);
                }
                std::printf("  copy %d sent\n", copies);
            }
            std::fflush(stdout);
            nextSend = nowSec() + kLayoutResendSec;
        }
        std::vector<uint8_t> data;
        Endpoint from;
        if (!s.udp.receive(nextSend - nowSec(), data, from)) continue;
        Header h;
        const uint8_t* payload = nullptr;
        if (decodePacket(data.data(), data.size(), h, payload) != DecodeError::Ok) continue;
        if (h.type == PacketType::LayoutAck) {
            LayoutAck ack;
            if (decodeLayoutAck(payload, h.length, ack) && ack.requestId == set.requestId) {
                std::printf("LAYOUT_ACK request %u: new version %u (after %d copies)\n", ack.requestId, ack.version,
                            copies);
                result = 0;
                ++answers;
                if (s.cli.dup && answers == 1) {
                    nextSend = 0;  // send the same request once more: it must get the same answer, not apply again
                    std::printf("  --dup: resending the same request after the answer\n");
                }
            }
        } else if (h.type == PacketType::LayoutReject) {
            LayoutReject reject;
            if (decodeLayoutReject(payload, h.length, reject) && reject.requestId == set.requestId) {
                std::printf("LAYOUT_REJECT request %u: %s, current version %u (after %d copies)\n", reject.requestId,
                            rejectReasonName(reject.reason), reject.currentVersion, copies);
                result = 3;
                ++answers;
                if (s.cli.dup && answers == 1) {
                    nextSend = 0;
                    std::printf("  --dup: resending the same request after the answer\n");
                }
            }
        }
        std::fflush(stdout);
    }
    if (answers == 0) std::printf("No answer within 15 s\n");
    return answers == 0 ? 1 : result;
}

int cmdBye(Session& s) {
    Endpoint headset;
    if (!resolveHost(s.cli.hosts[0], s.cli.port, headset)) return 2;
    Bye bye;
    bye.id = s.id;
    s.send(headset, PacketType::Bye, encodeBye(bye));
    std::printf("BYE -> %s\n", headset.text().c_str());
    return 0;
}

void usage() {
    std::printf(
        "Usage: fph-link-cli COMMAND ... [--port P] [--id HEX] [--name NAME] [--id-file PATH]\n"
        "  hello HOST...            HELLO each host (\"broadcast\" = 255.255.255.255) and print the answers (3 s)\n"
        "  watch HOST [--seconds N] [--lists] [--all]\n"
        "                           subscribe and print METRICS (--all: every value, --lists: process lists too)\n"
        "  layout-get HOST          print the layout JSON\n"
        "  layout-set HOST (FILE | --touch) [--base N] [--request ID] [--drop-first] [--dup]\n"
        "                           compare-and-set a layout (--touch: flip one process list's sum-same-name).\n"
        "                           --drop-first skips the first copy (simulated loss); --dup resends the same\n"
        "                           request after the answer (it must get the same answer, not apply again)\n"
        "  bye HOST                 end the lease now\n");
}

}  // namespace

int main(int argc, char** argv) {
    Cli cli;
    if (argc < 2) {
        usage();
        return 2;
    }
    cli.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool next = i + 1 < argc;
        if (a == "--port" && next) cli.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--id" && next) cli.idHex = argv[++i];
        else if (a == "--name" && next) cli.name = argv[++i];
        else if (a == "--id-file" && next) cli.idFile = argv[++i];
        else if (a == "--seconds" && next) cli.seconds = std::atof(argv[++i]);
        else if (a == "--base" && next) cli.base = std::atoll(argv[++i]);
        else if (a == "--request" && next) cli.request = std::atoll(argv[++i]);
        else if (a == "--lists") cli.lists = true;
        else if (a == "--all") cli.all = true;
        else if (a == "--touch") cli.touch = true;
        else if (a == "--drop-first") cli.dropFirst = true;
        else if (a == "--dup") cli.dup = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "Unknown option %s\n", a.c_str());
            usage();
            return 2;
        } else if (cli.command == "layout-set" && !cli.hosts.empty()) {
            cli.file = a;
        } else {
            cli.hosts.push_back(a);
        }
    }
    if (cli.name.empty()) cli.name = computerName();
    if (cli.hosts.empty() || (cli.command == "layout-set" && cli.file.empty() && !cli.touch)) {
        usage();
        return 2;
    }
    Session s;
    s.cli = cli;
    if (!s.udp.open()) {
        std::fprintf(stderr, "Can't open a UDP socket\n");
        return 1;
    }
    s.id = pcId(cli);
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    if (cli.command == "hello") return cmdHello(s);
    if (cli.command == "watch") return cmdWatch(s);
    if (cli.command == "layout-get") return cmdLayoutGet(s);
    if (cli.command == "layout-set") return cmdLayoutSet(s);
    if (cli.command == "bye") return cmdBye(s);
    usage();
    return 2;
}
