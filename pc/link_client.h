// The PC side of the headset link (ticket 07): discovery, the lease, layout sync and edits. Protocol logic only, no
// sockets, so tests drive it with a fake headset; LinkClientEnv is what it needs from the app (sending, the
// interfaces' broadcast addresses). Single-threaded: call from the UI thread.
//
// Phases: Discovery (HELLO every 2 s to remembered IPs, the manual IP and every interface's broadcast) ->
// Pending / Denied (HELLO every 2 s to that headset) -> Subscribed (SUBSCRIBE every 2 s). Back to Discovery, with
// the blob assembler reset, on SUBSCRIBE_ACK unknown_subscriber, BYE, or 6 s without a packet from the headset.
#pragma once

#include "camera_feed.h"  // T10
#include "layout.h"
#include "link_wire.h"
#include "metric_store.h"
#include "stream_metrics.h"
#include "widgets.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fph {
namespace pc {

/** An IPv4 endpoint, host byte order. */
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    /** @return "a.b.c.d:port" */
    std::string text() const;
    /** @return the blob assembler's peer key */
    uint64_t key() const { return static_cast<uint64_t>(ip) << 16 | port; }
};

/** What LinkClient needs from the app. */
class LinkClientEnv {
public:
    virtual ~LinkClientEnv() = default;
    /** Sends one datagram (fire and forget). */
    virtual void send(const Endpoint& to, const std::vector<uint8_t>& datagram) = 0;
    /**
     * @return where discovery broadcasts go: each up interface's directed broadcast (255.255.255.255 only leaves
     *         through the default interface on Windows), plus 255.255.255.255. Host order.
     */
    virtual std::vector<uint32_t> broadcastAddresses() = 0;
};

enum class LinkPhase {
    Discovery,   ///< "Looking for the headset…"
    Pending,     ///< "Approve this PC in the headset's dashboard"
    Denied,      ///< denied on the headset (until its agent restarts)
    Subscribed,  ///< live
};

/** @return "discovery" / "pending" / "denied" / "subscribed" */
const char* linkPhaseName(LinkPhase phase);

/** A headset that answered Allowed, for the settings' remembered list. */
struct HeadsetSeen {
    std::string idHex;
    std::string name;
    std::vector<uint32_t> ips;  ///< the address it answered from first, then its own addresses
};

class LinkClient {
public:
    /** A pending edit is resent this often (kLayoutResendSec). */
    static constexpr double kEditResendSec = link::kLayoutResendSec;

    /**
     * @param env the app
     * @param pcId this PC's id (settings.json)
     * @param pcName the computer name
     * @param port the headset's port
     */
    LinkClient(LinkClientEnv& env, const link::Id128& pcId, std::string pcName, uint16_t port = link::kDefaultPort);

    /**
     * Where discovery HELLOs go besides the broadcasts.
     * @param remembered remembered headsets' last IPs
     * @param manualIp the manual IP from Settings (0 = none)
     */
    void setDiscoveryTargets(const std::vector<uint32_t>& remembered, uint32_t manualIp);

    /** The cached layout, drawn (greyed) until the headset's arrives. */
    void setCachedLayout(const Layout& layout);

    /** Handles one received datagram; malformed ones are counted and dropped. */
    void handlePacket(const uint8_t* data, size_t size, const Endpoint& from, double now);

    /** Timers: HELLO / SUBSCRIBE / LAYOUT_GET / edit resends, silence. Call every ≤ 250 ms. */
    void tick(double now);

    /** Sends BYE to the headset (Exit). Safe in any phase. */
    void sayGoodbye();

    /** Sends a full PC snapshot, only while this headset has granted a lease. UI thread, 1 Hz. */
    void sendPcMetrics(const std::vector<link::WireValue>& values);

    /**
     * Starts a compare-and-set edit: shows `edited` right away and resends LAYOUT_SET every 1 s until ACK / REJECT
     * (across reconnects too). A reject restores the headset's layout and leaves a notice.
     * @param edited the new layout
     * @param baseVersion the layout version when the drag started
     * @return false (nothing sent) if not subscribed, no layout yet, or an edit is already in flight
     */
    bool submitEdit(const Layout& edited, uint32_t baseVersion, double now);

    // ---- state for the window ----
    LinkPhase phase() const { return phase_; }
    bool subscribed() const { return phase_ == LinkPhase::Subscribed; }
    /** @return true once a layout is known (from the cache or the headset) */
    bool haveLayout() const { return haveLayout_; }
    /** @return true if the shown layout came from this headset session (not just the cache) */
    bool layoutIsLive() const { return layoutLive_; }
    const Layout& layout() const { return layout_; }
    /** @return bumps whenever layout() changes (redraw, save the cache) */
    uint64_t layoutRevision() const { return layoutRevision_; }
    bool editPending() const { return pending_ != nullptr; }
    /** @return the newest layout version the headset reported */
    uint32_t headsetLayoutVersion() const { return headsetVersion_; }

    /** T10: the camera pictures (Camera BLOBs, from the lease path or the dongle path) */
    CameraFeed& cameras() { return cameras_; }
    const CameraFeed& cameras() const { return cameras_; }
    /** T10: camera frames per second asked for in SUBSCRIBE (settings.json camera_fps, 1-30) */
    void setCameraFps(int fps) { cameraFps_ = fps < 1 ? 1 : (fps > 30 ? 30 : fps); }
    /** T10: @return the headset's dongle-path address camera frames are asked for at (0 = none) */
    uint32_t cameraPathIp() const { return cameraPathIp_; }

    /** T11: the headset's session from the last METRICS' LINK_STATE (kept across reconnects; id 0 = none yet) */
    const link::LinkState& headsetState() const { return headsetState_; }
    /** T11: monotonic time of the last METRICS (< 0 = never) */
    double lastMetricsAt() const { return lastMetricsAt_; }

    MetricStore& store() { return store_; }
    const MetricStore& store() const { return store_; }
    const ProcessListsView& processes() const { return processes_; }

    /** @return the headset's HELLO_ACK (valid once phase != Discovery) */
    const link::HelloAck& headset() const { return headset_; }
    Endpoint headsetEndpoint() const { return headsetEp_; }
    /** @return true if some answer was dropped because the headset speaks another protocol version */
    bool versionMismatch() const { return versionMismatch_; }

    /**
     * @return the user-facing notice left by the last reject ("Layout changed in the headset, reloaded. Your last
     *         move wasn't applied"), then clears it; empty if none
     */
    std::string takeNotice();

    /** @return true once per newly Allowed headset (remember it in settings) */
    bool takeHeadsetSeen(HeadsetSeen& out);

    /** @return one paragraph for logs */
    std::string debugText(double now) const;

    // ---- counters (tests, logs) ----
    uint64_t hellosSent() const { return hellosSent_; }
    uint64_t subscribesSent() const { return subscribesSent_; }
    uint64_t layoutGetsSent() const { return layoutGetsSent_; }
    uint64_t editCopiesSent() const { return editCopiesSent_; }
    uint64_t rediscoveries() const { return rediscoveries_; }
    uint64_t malformed() const { return malformed_; }
    uint64_t unknownMetricValues() const { return unknownMetricValues_; }
    /** @return how many times the blob assembler was reset (every return to discovery) */
    uint64_t assemblerResets() const { return assemblerResets_; }

private:
    struct PendingEdit {
        uint32_t requestId = 0;
        uint32_t baseVersion = 0;
        std::vector<uint8_t> blob;  ///< encodeLayoutSet
        Layout before;              ///< restored on reject
        double nextSend = 0.0;
        int copies = 0;
    };

    LinkClientEnv& env_;
    link::Id128 pcId_ {};
    std::string pcName_;
    uint16_t port_;
    std::vector<uint32_t> remembered_;
    uint32_t manualIp_ = 0;

    LinkPhase phase_ = LinkPhase::Discovery;
    link::HelloAck headset_;
    Endpoint headsetEp_;
    uint32_t lastHeadsetIp_ = 0;  ///< kept across rediscovery as one more target
    double lastHeard_ = 0.0;
    double nextHello_ = 0.0;
    double nextSubscribe_ = 0.0;
    double nextLayoutGet_ = 0.0;
    uint32_t seq_ = 0;
    uint32_t nextBlobId_ = 1;
    link::BlobAssembler blobs_;
    bool versionMismatch_ = false;

    Layout layout_;
    bool haveLayout_ = false;
    bool layoutLive_ = false;
    uint64_t layoutRevision_ = 0;
    uint32_t headsetVersion_ = 0;
    bool needLayout_ = false;
    std::unique_ptr<PendingEdit> pending_;
    std::string notice_;
    bool seenFresh_ = false;
    HeadsetSeen seen_;

    MetricStore store_;
    ProcessListsView processes_;
    link::LinkState headsetState_;  ///< T11
    double lastMetricsAt_ = -1.0;   ///< T11
    CameraFeed cameras_;          ///< T10
    int cameraFps_ = 10;          ///< T10
    uint32_t cameraPathIp_ = 0;   ///< T10: the headset's wlanap address, when it isn't the lease path

    uint64_t hellosSent_ = 0;
    uint64_t subscribesSent_ = 0;
    uint64_t layoutGetsSent_ = 0;
    uint64_t editCopiesSent_ = 0;
    uint64_t rediscoveries_ = 0;
    uint64_t malformed_ = 0;
    uint64_t unknownMetricValues_ = 0;
    uint64_t assemblerResets_ = 0;

    void sendPacket(const Endpoint& to, link::PacketType type, const std::vector<uint8_t>& payload);
    void sendHello(const Endpoint& to);
    void sendSubscribe(double now);
    void sendEditCopy(double now);
    uint32_t wants() const;
    void backToDiscovery(double now, const char* why);
    void onHelloAck(const link::HelloAck& ack, const Endpoint& from, double now);
    void onSubscribeAck(const link::SubscribeAck& ack, double now);
    void onMetrics(const link::Metrics& m, double now);
    void onBlob(const link::Blob& blob, double now);
    void onLayoutAck(const link::LayoutAck& ack, double now);
    void onLayoutReject(const link::LayoutReject& reject, double now);
    void noteHeadsetVersion(uint32_t version);
    void setLayout(const Layout& layout, bool live);
};

}  // namespace pc
}  // namespace fph
