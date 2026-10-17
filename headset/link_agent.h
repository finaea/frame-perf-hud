// The PC link wired into the agent: LinkServer + its UDP socket + the agent-side LinkHostEnv (metric store,
// session, process lists, vrlink host IPs, config saves). main.cpp holds one LinkService; --serve-link runs
// one without OpenVR for testing.
#pragma once

#include "config.h"
#include "layout_authority.h"
#include "link_server.h"
#include "metric_store.h"

#include <csignal>
#include <functional>
#include <memory>
#include <string>

class SensorBridge;

/** LinkHostEnv backed by the running agent. */
class AgentLinkEnv : public LinkHostEnv {
public:
    AgentLinkEnv(Config& config, ConfigWatcher& watcher, fph::MetricStore& store, const SensorBridge& bridge,
                 LinkSocket& socket);

    void send(const LinkEndpoint& to, const std::vector<uint8_t>& datagram) override;
    std::vector<uint32_t> streamHostIps() override;
    std::vector<fph::link::LocalAddress> localAddresses() override;
    fph::link::LinkState linkState() override;
    std::vector<fph::link::WireValue> metricValues() override;
    uint64_t processLists(fph::link::ProcessListWire& cpu, fph::link::ProcessListWire& ram) override;
    bool saveConfig(std::string& error) override;
    void pcMetrics(const fph::PcMetrics& m, double now) override { pcStream_.apply(m, store_, now); }
    size_t sendQueued() override { return socket_.queuedBytes(); }  // T10
    void expirePcMetrics(double now) { pcStream_.expire(store_, now); }

    /** R3: the PC window's Profiles / Logs tabs, answered by the main loop (unset: no CONTROL_STATE, Unsupported). */
    using ControlStateFn = std::function<bool(fph::link::ControlState&)>;
    using ControlActionFn = std::function<fph::link::ControlResult(const fph::link::ControlAction&, double)>;
    void setControlHooks(ControlStateFn state, ControlActionFn action) {
        controlStateFn_ = std::move(state);
        controlActionFn_ = std::move(action);
    }
    bool controlState(fph::link::ControlState& out) override { return controlStateFn_ && controlStateFn_(out); }
    fph::link::ControlResult controlAction(const fph::link::ControlAction& a, double now) override {
        return controlActionFn_ ? controlActionFn_(a, now) : LinkHostEnv::controlAction(a, now);
    }

private:
    Config& config_;
    ConfigWatcher& watcher_;
    fph::MetricStore& store_;
    fph::PcStreamReceiver pcStream_;
    const SensorBridge& bridge_;
    LinkSocket& socket_;
    ControlStateFn controlStateFn_;      // R3
    ControlActionFn controlActionFn_;
};

/** Socket + env + server, for the main loop. */
class LinkService {
public:
    LinkService(Config& config, ConfigWatcher& watcher, LayoutAuthority& layout, fph::MetricStore& store,
                const SensorBridge& bridge);

    /** Opens the socket on config.link.port (logs if it can't; tick() retries every 5 s). */
    void start(double now);
    /** @return the socket fd for poll(), or -1 */
    int fd() const { return socket_.fd(); }
    /** Handles every queued datagram. Call whenever poll() says the fd is readable (cheap otherwise). */
    void receive(double now) { socket_.receiveAll(server_, now); }
    /** The metrics deadline (2 Hz): leases, METRICS out, bind retry. */
    void tick(double now);
    /** @return true while some PC holds a lease (DemandInputs::pcLease) */
    bool hasLease() const { return server_.hasLease(); }
    /** The protocol logic: uiState / allow / deny / forgetAll for the dashboard tab. */
    LinkServer& server() { return server_; }
    /** R3: the PC window's Profiles / Logs tabs (see AgentLinkEnv::setControlHooks). */
    void setControlHooks(AgentLinkEnv::ControlStateFn state, AgentLinkEnv::ControlActionFn action) {
        env_.setControlHooks(std::move(state), std::move(action));
    }
    /** For the debug dump. */
    std::string debugText(double now) const;
    /** BYE to every lease holder, then closes the socket. */
    void shutdown();

private:
    Config& config_;
    LinkSocket socket_;
    AgentLinkEnv env_;
    LinkServer server_;
    double nextBindAttempt_ = 0.0;
    std::string lastBindError_;
};

/** --serve-link: the link without the overlay (no OpenVR, no single-instance lock), for testing. */
struct ServeLinkOptions {
    std::string configPath;  ///< required, and must not be the default config (never touch the live one)
    int port = 0;            ///< 0 = config link.port
    double seconds = 0.0;    ///< 0 = until SIGINT / SIGTERM
};

/**
 * Runs sensors + scheduler + the link server, with "someone looking" = a PC lease.
 * @param wakeFd an eventfd the signal handlers poke (or -1)
 * @return exit code
 */
int runServeLink(const ServeLinkOptions& options, const volatile std::sig_atomic_t& stop, int wakeFd);
