#include "link_agent.h"

#include "scheduler.h"
#include "sensor_bridge.h"
#include "sensors.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kBindRetrySec = 5.0;
constexpr double kTickSec = 0.5;

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** @return the headset's name for HELLO_ACK: its hostname */
std::string headsetName() {
    char host[256] = {};
    if (::gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') return "Steam Frame";
    return host;
}

/** ProcessItem rows (what the widgets draw) on the wire. */
fph::link::ProcessListWire toWire(const std::vector<fph::ProcessItem>& rows, uint64_t generation, bool cpuValid) {
    fph::link::ProcessListWire out;
    out.generation = generation;
    out.cpuValid = cpuValid;
    out.rows.reserve(rows.size());
    for (const fph::ProcessItem& item : rows) {
        fph::link::ProcessRowWire row;
        row.name = item.name;
        row.pid = item.pid;
        row.count = static_cast<uint16_t>(std::min(item.count, 65535));
        row.cpuCorePct = static_cast<float>(item.cpuCorePct);
        row.rssMiB = static_cast<float>(item.rssMiB);
        out.rows.push_back(std::move(row));
    }
    return out;
}

}  // namespace

// ---- AgentLinkEnv ----

AgentLinkEnv::AgentLinkEnv(Config& config, ConfigWatcher& watcher, fph::MetricStore& store,
                           const SensorBridge& bridge, LinkSocket& socket)
    : config_(config), watcher_(watcher), store_(store), bridge_(bridge), socket_(socket) {}

void AgentLinkEnv::send(const LinkEndpoint& to, const std::vector<uint8_t>& datagram) {
    socket_.send(to, datagram);
}

std::vector<uint32_t> AgentLinkEnv::streamHostIps() {
    return readVrlinkHostIps();
}

std::vector<fph::link::LocalAddress> AgentLinkEnv::localAddresses() {
    return readLocalAddresses();
}

fph::link::LinkState AgentLinkEnv::linkState() {
    const fph::SessionInfo& s = bridge_.session();
    fph::link::LinkState state;
    state.sessionId = s.id;
    state.sessionStartUnix = s.startUnix;
    state.appKey = s.appKey;
    state.appName = s.appName;
    return state;
}

std::vector<fph::link::WireValue> AgentLinkEnv::metricValues() {
    // The active metrics: those owned by a widget in the layout (the PC draws the same layout)
    std::array<bool, fph::kWidgetTypeCount> enabled {};
    for (fph::WidgetType type : fph::enabledWidgets(config_.layout)) {
        const size_t index = static_cast<size_t>(type);
        if (index < enabled.size()) enabled[index] = true;
    }
    // Streaming also consumes Network's adapter IPs for link names, even without a Network card.
    if (enabled[static_cast<size_t>(fph::WidgetType::Streaming)]) {
        enabled[static_cast<size_t>(fph::WidgetType::Network)] = true;
    }
    std::vector<fph::link::WireValue> values;
    for (const fph::MetricDef& def : store_.defs()) {
        const size_t owner = static_cast<size_t>(def.owner);
        if (owner >= enabled.size() || !enabled[owner]) continue;
        if (const fph::MetricValue* value = store_.get(def.id)) values.push_back(fph::link::toWire(def.id, *value));
    }
    return values;
}

uint64_t AgentLinkEnv::processLists(fph::link::ProcessListWire& cpu, fph::link::ProcessListWire& ram) {
    const fph::ProcessListsView view = bridge_.processListsView();
    if (view.generation == 0) return 0;
    cpu = toWire(view.byCpu, view.generation, view.cpuValid);
    ram = toWire(view.byRam, view.generation, true);
    return view.generation;
}

bool AgentLinkEnv::saveConfig(std::string& error) {
    return watcher_.save(config_, error);
}

// ---- LinkService ----

LinkService::LinkService(Config& config, ConfigWatcher& watcher, LayoutAuthority& layout,
                         fph::MetricStore& store, const SensorBridge& bridge)
    : config_(config), env_(config, watcher, store, bridge, socket_), server_(config, layout, env_, headsetName()) {
    if (!server_.headsetIdIsNew()) return;
    // An unreadable config is left alone at startup (T04): the id then lives in memory until the next save
    std::string error;
    if (watcher.lastLoadFailed()) {
        std::fprintf(stderr, "[link] New headset id %.8s (not saved: the config file couldn't be read)\n",
                     config_.link.headsetId.c_str());
    } else if (watcher.save(config_, error)) {
        std::fprintf(stderr, "[link] New headset id %.8s saved\n", config_.link.headsetId.c_str());
    } else {
        std::fprintf(stderr, "[link] New headset id %.8s, but saving it failed: %s\n", config_.link.headsetId.c_str(),
                     error.c_str());
    }
}

void LinkService::start(double now) {
    std::string error;
    if (socket_.open(static_cast<uint16_t>(config_.link.port), error)) {
        std::fprintf(stderr, "[link] Listening on UDP %d\n", config_.link.port);
        lastBindError_.clear();
        return;
    }
    if (error != lastBindError_) std::fprintf(stderr, "[link] Can't listen: %s (retrying every 5 s)\n", error.c_str());
    lastBindError_ = error;
    nextBindAttempt_ = now + kBindRetrySec;
}

void LinkService::tick(double now) {
    env_.expirePcMetrics(now);
    if (socket_.fd() < 0 && now >= nextBindAttempt_) start(now);
    server_.tick(now);
}

std::string LinkService::debugText(double now) const {
    return server_.debugText(now) + "socket " + (socket_.fd() >= 0 ? "open" : "closed") + ", send errors " +
           std::to_string(socket_.sendErrors()) + ", camera frames skipped (send queue full) " +
           std::to_string(server_.cameraFramesSkipped()) + ", queued now " + std::to_string(socket_.queuedBytes()) +
           " B\n";  // T10
}

void LinkService::shutdown() {
    server_.sayGoodbye();
    socket_.close();
}

// ---- --serve-link ----

int runServeLink(const ServeLinkOptions& options, const volatile std::sig_atomic_t& stop, int wakeFd) {
    if (options.configPath.empty() || options.configPath == defaultConfigPath()) {
        std::fprintf(stderr, "--serve-link needs its own --config FILE (never the live %s)\n",
                     defaultConfigPath().c_str());
        return 2;
    }
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    if (options.port > 0) config.link.port = options.port;

    Sensors sensors;
    sensors.discover();
    fph::MetricStore store;
    fph::Scheduler scheduler;
    SensorBridge bridge(sensors, nullptr, store);
    bridge.registerSources(scheduler);
    const auto applyLayout = [&](const fph::Layout& layout) {
        scheduler.setEnabledWidgets(fph::enabledWidgets(layout));
        bridge.setProcessListOptions(SensorBridge::processOptionsFor(layout));
    };
    applyLayout(config.layout);
    LayoutAuthority authority(
        config, [&watcher](const Config& c, std::string& error) { return watcher.save(c, error); }, applyLayout);
    LinkService link(config, watcher, authority, store, bridge);
    const double start = monotonicSeconds();
    link.start(start);
    std::fprintf(stderr, "[serve-link] config %s, layout version %u; sources run only while a PC holds a lease\n",
                 options.configPath.c_str(), config.layout.version);

    double nextTick = start;
    bool looking = false;
    while (!stop) {
        const double now = monotonicSeconds();
        if (options.seconds > 0 && now - start >= options.seconds) break;
        link.receive(now);
        if (now >= nextTick) {
            nextTick = now + kTickSec;
            fph::DemandInputs demand;
            demand.pcLease = link.hasLease();
            if (demand.pcLease != looking) {
                std::fprintf(stderr, "[demand] %s\n", demand.pcLease ? "A PC holds a lease: reading sources"
                                                                     : "No lease: all sources paused");
                looking = demand.pcLease;
            }
            scheduler.setDemand(demand);
            scheduler.runDue(now);
            link.tick(now);
        } else {
            scheduler.runDue(now);
        }
        const double wakeAt = std::min(nextTick, scheduler.nextDue(monotonicSeconds()));
        const int timeoutMs =
            static_cast<int>(std::ceil(std::max(0.0, wakeAt - monotonicSeconds()) * 1000.0));
        struct pollfd fds[2] = {{link.fd(), POLLIN, 0}, {wakeFd, POLLIN, 0}};
        if (::poll(fds, 2, timeoutMs) > 0 && (fds[1].revents & POLLIN)) {
            uint64_t value = 0;
            (void)!::read(wakeFd, &value, sizeof(value));
        }
    }
    link.shutdown();
    std::fprintf(stderr, "[serve-link] stopped\n");
    return 0;
}
