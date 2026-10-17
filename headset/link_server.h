// The headset side of the PC link (ticket 06): discovery, allow-list, leases, METRICS / process lists out,
// layout sync in. LinkServer is the protocol logic with no sockets, so tests drive it directly; LinkHostEnv is
// everything it needs from the agent (sending, the metric store, the vrlink host IPs, saving the config).
// LinkSocket is the non-blocking UDP socket the main loop polls. Single-threaded: call from the main loop.
#pragma once

#include "config.h"
#include "layout_authority.h"
#include "link_ui.h"
#include "link_wire.h"
#include "stream_metrics.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

/** An IPv4 endpoint, host byte order. */
struct LinkEndpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool operator==(const LinkEndpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const LinkEndpoint& o) const { return !(*this == o); }
    /** @return "a.b.c.d:port" */
    std::string text() const;
    /** @return a key for the blob assembler */
    uint64_t key() const { return static_cast<uint64_t>(ip) << 16 | port; }
};

/** T10: one PC's camera subscription: where its frames go, which cameras (bit = camera id), how fast. */
struct LinkCameraTarget {
    LinkEndpoint to;
    uint8_t cameras = 0;
    double fps = 10.0;
};

/** What LinkServer needs from the agent. */
class LinkHostEnv {
public:
    virtual ~LinkHostEnv() = default;
    /** Sends one datagram (fire and forget). */
    virtual void send(const LinkEndpoint& to, const std::vector<uint8_t>& datagram) = 0;
    /** @return the Steam Link host IPs: the -c args of the running vrlink (empty if it isn't running). Uncached */
    virtual std::vector<uint32_t> streamHostIps() = 0;
    /** @return the headset's own IPv4 addresses (for HELLO_ACK) */
    virtual std::vector<fph::link::LocalAddress> localAddresses() = 0;
    /** @return session identity for LINK_STATE (the layout version is filled in by the server) */
    virtual fph::link::LinkState linkState() = 0;
    /** @return the active metrics' values (the enabled widgets' metrics) */
    virtual std::vector<fph::link::WireValue> metricValues() = 0;
    /**
     * @param cpu the CPU list
     * @param ram the RAM list
     * @return the scan generation (bumped on every scan); 0 = no lists yet
     */
    virtual uint64_t processLists(fph::link::ProcessListWire& cpu, fph::link::ProcessListWire& ram) = 0;
    /** Saves the config (allowed_pcs changed). @return false + error if it failed */
    virtual bool saveConfig(std::string& error) = 0;
    virtual void pcMetrics(const fph::PcMetrics&, double) {}
    /** T10: bytes waiting in the socket's send queue (camera frames back off so control packets always fit) */
    virtual size_t sendQueued() { return 0; }
    /**
     * R3: what the PC window's Profiles / Logs tabs show. The revision must bump whenever the content changes.
     * @return false if this agent has none (then no CONTROL_STATE is sent)
     */
    virtual bool controlState(fph::link::ControlState&) { return false; }
    /**
     * R3: carries out a leased PC's Profiles / Logs button, with the same guards as the headset's own dashboard
     * (fph::link::controlActionConfirmed for the second tap). Called at most once per request id.
     */
    virtual fph::link::ControlResult controlAction(const fph::link::ControlAction&, double) {
        fph::link::ControlResult r;
        r.outcome = fph::link::ControlOutcome::Unsupported;
        return r;
    }
};

class LinkServer {
public:
    /** How long the vrlink -c lookup is cached. */
    static constexpr double kHostIpCacheSec = 5.0;
    /** A PC with no lease is forgotten this long after its last packet (then SUBSCRIBE gets unknown_subscriber). */
    static constexpr double kPeerIdleSec = 15.0;

    /**
     * @param config the live config (link.allowed_pcs, link.headset_id)
     * @param layout the layout compare-and-set
     * @param env the agent
     * @param name this headset's name for HELLO_ACK
     */
    LinkServer(Config& config, LayoutAuthority& layout, LinkHostEnv& env, std::string name);

    /** Handles one received datagram. Malformed ones are counted and dropped. */
    void handlePacket(const uint8_t* data, size_t size, const LinkEndpoint& from, double now);

    /**
     * The metrics deadline (2 Hz): expires leases, peers and blobs, then sends METRICS (+ new process lists) to
     * every lease holder. Sends nothing and does no work beyond a few comparisons when nobody holds a lease.
     */
    void tick(double now);

    /** @return true if some PC holds a lease ("someone looking") */
    bool hasLease() const;

    /**
     * T10: every lease holder that asked for cameras: its camera address (the dongle path it registered with a
     * camera-path-only SUBSCRIBE within the lease time, else its lease address), the cameras and its fps.
     */
    std::vector<LinkCameraTarget> cameraTargets(double now) const;
    /**
     * T10: sends one camera frame / state as a Camera BLOB (stream id = camera), unless the socket's send queue is
     * already over kCameraQueueLimit: then the frame is skipped (the next one comes), so a burst of camera chunks
     * never crowds out METRICS / SUBSCRIBE_ACK on the same socket.
     * @return false if skipped
     */
    bool sendCameraBlob(const LinkEndpoint& to, uint8_t camera, const std::vector<uint8_t>& data);
    static constexpr size_t kCameraQueueLimit = 256 * 1024;
    uint64_t cameraFramesSkipped() const { return cameraSkipped_; }
    /** R3: CONTROL_STATE packets sent / CONTROL_ACTIONs carried out (tests, debug dump) */
    uint64_t controlStatesSent() const { return controlStatesSent_; }
    uint64_t controlActionsDone() const { return controlActionsDone_; }

    /** Sends BYE to every lease holder (shutdown). */
    void sayGoodbye();

    // ---- dashboard tab (ticket 05 draws it) ----

    /** @return pending PCs (oldest first), allowed_pcs count, and a revision that bumps on change */
    LinkUiState uiState() const;
    /** Allows a pending PC: adds it to allowed_pcs and saves the config. Unknown ids are ignored. */
    void allow(const std::string& id);
    /** Denies a pending PC until the agent restarts (in memory only). */
    void deny(const std::string& id);
    /** Clears allowed_pcs (saves), forgets every PC that isn't auto-allowed and drops its lease. */
    void forgetAll();

    /** @return true if the constructor made a new headset id (config.link.headset_id was empty): save the config */
    bool headsetIdIsNew() const { return headsetIdIsNew_; }

    /** @return one paragraph for the debug dump */
    std::string debugText(double now) const;

    // ---- counters (tests, debug dump) ----
    uint64_t malformed() const { return malformed_; }
    uint64_t unknownMetricValues() const { return unknownMetricValues_; }
    uint64_t layoutApplies() const { return layoutApplies_; }
    uint64_t metricsSent() const { return metricsSent_; }
    uint64_t hostIpLookups() const { return hostIpLookups_; }

private:
    struct Answer {
        uint32_t requestId = 0;
        bool applied = false;
        uint32_t version = 0;
        fph::link::RejectReason reason = fph::link::RejectReason::Stale;
    };

    struct Peer {
        fph::link::Id128 id {};
        std::string name;
        LinkEndpoint address;  ///< where its last packet came from
        fph::link::PeerStatus status = fph::link::PeerStatus::Pending;
        bool autoAllowed = false;  ///< allowed because its IP is a Steam Link host
        double firstSeen = 0.0;
        double lastSeen = 0.0;
        // Lease
        bool leased = false;
        double leaseUntil = 0.0;
        uint32_t wants = 0;
        LinkEndpoint leaseAddress;  ///< the last SUBSCRIBE's source: data goes here
        uint64_t sentListGeneration = 0;
        uint32_t pcSnapshot = 0;
        bool havePcSnapshot = false;
        uint8_t cameraFps = 0;           ///< T10: from SUBSCRIBE (0 = not given)
        LinkEndpoint cameraAddress;      ///< T10: the PC's dongle path, from a camera-path-only SUBSCRIBE
        double cameraAddressUntil = 0.0;
        std::deque<Answer> answers;  ///< last kLayoutAnswerMemory LAYOUT_SET answers
        // R3: CONTROL_STATE / CONTROL_ACTION
        bool controlSent = false;
        uint32_t controlRevision = 0;  ///< the last CONTROL_STATE revision sent
        double controlSentAt = 0.0;
        std::deque<fph::link::ControlResult> controlAnswers;  ///< last kLayoutAnswerMemory answers (a resend gets the same)
    };

    Config& config_;
    LayoutAuthority& layout_;
    LinkHostEnv& env_;
    std::string name_;
    fph::link::Id128 headsetId_ {};
    bool headsetIdIsNew_ = false;
    std::map<std::string, Peer> peers_;  ///< by hex id
    std::vector<std::string> denied_;    ///< hex ids denied this run
    fph::link::BlobAssembler blobs_;
    uint32_t seq_ = 0;
    uint32_t nextBlobId_ = 1;
    uint32_t snapshotId_ = 0;
    uint64_t uiRevision_ = 1;
    std::vector<uint32_t> hostIps_;
    double hostIpsAt_ = -1e9;

    uint64_t malformed_ = 0;
    uint64_t unknownMetricValues_ = 0;
    uint64_t layoutApplies_ = 0;
    uint64_t metricsSent_ = 0;
    uint64_t hostIpLookups_ = 0;
    uint64_t cameraSkipped_ = 0;  ///< T10
    uint64_t controlStatesSent_ = 0;   ///< R3
    uint64_t controlActionsDone_ = 0;  ///< R3

    void sendPacket(const LinkEndpoint& to, fph::link::PacketType type, const std::vector<uint8_t>& payload);
    void sendBlob(const LinkEndpoint& to, fph::link::BlobKind kind, uint8_t stream, const std::vector<uint8_t>& data);
    void sendAnswer(const LinkEndpoint& to, const Answer& answer);

    void onHello(const fph::link::Hello& m, const LinkEndpoint& from, double now);
    void onSubscribe(const fph::link::Subscribe& m, const LinkEndpoint& from, double now);
    void onLayoutGet(const fph::link::LayoutGet& m, const LinkEndpoint& from, double now);
    void onLayoutSet(const fph::link::LayoutSet& m, const LinkEndpoint& from, double now);
    void onBye(const fph::link::Bye& m, double now);
    void onControlAction(const fph::link::ControlAction& m, const LinkEndpoint& from, double now);  // R3

    /** @return the Steam Link host IPs, looked up at most every kHostIpCacheSec */
    const std::vector<uint32_t>& hostIps(double now);
    bool isAllowedId(const std::string& hexId) const;
    bool isDenied(const std::string& hexId) const;
    void dropLease(Peer& peer, const char* why);
    void sendData(Peer& peer, double now);
};

/** The non-blocking UDP socket (IPv4, all interfaces). */
class LinkSocket {
public:
    LinkSocket() = default;
    ~LinkSocket();
    LinkSocket(const LinkSocket&) = delete;
    LinkSocket& operator=(const LinkSocket&) = delete;

    /**
     * Binds 0.0.0.0:port (SO_BROADCAST off; broadcasts to 255.255.255.255:port still arrive).
     * @return false + error if it couldn't
     */
    bool open(uint16_t port, std::string& error);
    void close();
    /** @return the fd for poll(), or -1 */
    int fd() const { return fd_; }
    /** Sends one datagram; errors are counted, not reported. */
    void send(const LinkEndpoint& to, const std::vector<uint8_t>& datagram);
    /** T10: @return bytes not yet sent from the socket's queue (SIOCOUTQ), 0 if unknown */
    size_t queuedBytes() const;
    /** Reads every queued datagram into the server (at most 256 per call, so a flood can't starve the loop). */
    void receiveAll(LinkServer& server, double now);
    uint64_t sendErrors() const { return sendErrors_; }

private:
    int fd_ = -1;
    uint64_t sendErrors_ = 0;
};

/**
 * The Steam Link host IPs from vrlink's NUL-separated /proc/[pid]/cmdline: the value after each "-c"
 * argument (also "-c=<ip>"; a ":port" suffix or comma-separated list is accepted). Anything that isn't an IPv4
 * address is skipped. Never logs or returns any other argument (vrlink's -e carries a secret).
 * @return host-order IPv4 addresses, deduplicated, in order
 */
std::vector<uint32_t> parseVrlinkHostIps(const char* cmdline, size_t length);

/**
 * Finds the running vrlink (by /proc/[pid]/exe) and parses its -c host IPs.
 * @param procRoot "/proc", or a fixture tree in tests
 * @return the IPs; empty if vrlink isn't running or its cmdline can't be read
 */
std::vector<uint32_t> readVrlinkHostIps(const std::string& procRoot = "/proc");

/** @return the up, non-loopback IPv4 addresses (getifaddrs) */
std::vector<fph::link::LocalAddress> readLocalAddresses();
