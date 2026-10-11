// Wireless (dongle and home Wi-Fi) readings.
#include "wifi_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/** Collects one GET_STATION dump: the first station only (MAC addresses aren't read). */
struct StationResult {
    bool managed = false;  ///< station side (home Wi-Fi): changes the signal choice
    bool found = false;
    double signalDbm = kNaN;
    double txLinkMbps = kNaN;
    double rxLinkMbps = kNaN;
};

/** Collects one GET_INTERFACE dump: the first AP and the first station interface. */
struct InterfaceResult {
    unsigned apIndex = 0;
    std::string apName;
    int apFreq = 0;
    int apWidth = 0;
    unsigned stationIndex = 0;
    std::string stationName;
    int stationFreq = 0;
    int stationWidth = 0;
};

/**
 * Link rate (Mbps) from a nested NL80211_STA_INFO_TX_BITRATE / RX_BITRATE.
 * @return Mbps, NaN if missing
 */
double parseBitrate(nlattr* attr) {
    if (attr == nullptr) return kNaN;
    nlattr* rate[NL80211_RATE_INFO_MAX + 1] = {};
    if (nla_parse_nested(rate, NL80211_RATE_INFO_MAX, attr, nullptr) != 0) return kNaN;
    // Both in units of 100 kbit/s
    if (rate[NL80211_RATE_INFO_BITRATE32] != nullptr) return nla_get_u32(rate[NL80211_RATE_INFO_BITRATE32]) / 10.0;
    if (rate[NL80211_RATE_INFO_BITRATE] != nullptr) return nla_get_u16(rate[NL80211_RATE_INFO_BITRATE]) / 10.0;
    return kNaN;
}

/** @return a signed dBm attribute, NaN if absent */
double signalAttr(nlattr** info, int key) {
    if (info[key] == nullptr) return kNaN;
    return static_cast<int8_t>(nla_get_u8(info[key]));
}

void parseAttributes(nl_msg* message, nlattr** attrs) {
    auto* header = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(message)));
    nla_parse(attrs, NL80211_ATTR_MAX, genlmsg_attrdata(header, 0), genlmsg_attrlen(header, 0), nullptr);
}

int onStation(nl_msg* message, void* arg) {
    auto* result = static_cast<StationResult*>(arg);
    if (result->found) return NL_SKIP;
    nlattr* attrs[NL80211_ATTR_MAX + 1] = {};
    parseAttributes(message, attrs);
    if (attrs[NL80211_ATTR_STA_INFO] == nullptr) return NL_SKIP;
    nlattr* info[NL80211_STA_INFO_MAX + 1] = {};
    if (nla_parse_nested(info, NL80211_STA_INFO_MAX, attrs[NL80211_ATTR_STA_INFO], nullptr) != 0) return NL_SKIP;
    result->found = true;
    SignalReadings r;
    r.signal = signalAttr(info, NL80211_STA_INFO_SIGNAL);
    r.signalAvg = signalAttr(info, NL80211_STA_INFO_SIGNAL_AVG);
    r.beaconSignalAvg = signalAttr(info, NL80211_STA_INFO_BEACON_SIGNAL_AVG);
    r.ackSignal = signalAttr(info, NL80211_STA_INFO_ACK_SIGNAL);
    r.ackSignalAvg = signalAttr(info, NL80211_STA_INFO_ACK_SIGNAL_AVG);
    result->signalDbm = chooseSignalDbm(r, result->managed);
    result->txLinkMbps = parseBitrate(info[NL80211_STA_INFO_TX_BITRATE]);
    result->rxLinkMbps = parseBitrate(info[NL80211_STA_INFO_RX_BITRATE]);
    return NL_SKIP;
}

int onInterface(nl_msg* message, void* arg) {
    auto* result = static_cast<InterfaceResult*>(arg);
    nlattr* attrs[NL80211_ATTR_MAX + 1] = {};
    parseAttributes(message, attrs);
    // Interfaces without a netdev (P2P-device) have no IFINDEX
    if (attrs[NL80211_ATTR_IFTYPE] == nullptr || attrs[NL80211_ATTR_IFINDEX] == nullptr ||
        attrs[NL80211_ATTR_IFNAME] == nullptr) {
        return NL_SKIP;
    }
    const unsigned type = nla_get_u32(attrs[NL80211_ATTR_IFTYPE]);
    const unsigned index = nla_get_u32(attrs[NL80211_ATTR_IFINDEX]);
    const char* name = nla_get_string(attrs[NL80211_ATTR_IFNAME]);
    // Only present while the interface is on a channel
    const int freq = attrs[NL80211_ATTR_WIPHY_FREQ] != nullptr ? static_cast<int>(nla_get_u32(attrs[NL80211_ATTR_WIPHY_FREQ])) : 0;
    const int width = attrs[NL80211_ATTR_CHANNEL_WIDTH] != nullptr
                          ? widthMhzFromNl80211(nla_get_u32(attrs[NL80211_ATTR_CHANNEL_WIDTH]))
                          : 0;
    if (type == NL80211_IFTYPE_AP && result->apIndex == 0) {
        result->apIndex = index;
        result->apName = name;
        result->apFreq = freq;
        result->apWidth = width;
    } else if (type == NL80211_IFTYPE_STATION && result->stationIndex == 0) {
        result->stationIndex = index;
        result->stationName = name;
        result->stationFreq = freq;
        result->stationWidth = width;
    }
    return NL_SKIP;
}

/** @return the interface's first IPv4 address ("" if none) */
std::string ipv4Of(const ifaddrs* list, const std::string& name) {
    for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET || name != a->ifa_name) continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr) return text;
    }
    return "";
}

bool readCounter(int fd, unsigned long long& value) {
    if (fd < 0) return false;
    char buffer[32];
    const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return false;
    buffer[n] = '\0';
    value = std::strtoull(buffer, nullptr, 10);
    return true;
}

}  // namespace

WifiInfo::WifiInfo() : signalDbm(kNaN), txLinkMbps(kNaN), rxLinkMbps(kNaN), rxMbps(kNaN), txMbps(kNaN) {}

AdapterInfo::AdapterInfo() : rxMbps(kNaN), txMbps(kNaN), txLinkMbps(kNaN), rxLinkMbps(kNaN), signalDbm(kNaN) {}

SignalReadings::SignalReadings()
    : signal(kNaN), signalAvg(kNaN), beaconSignalAvg(kNaN), ackSignal(kNaN), ackSignalAvg(kNaN) {}

double chooseSignalDbm(const SignalReadings& r, bool managed) {
    const double apOrder[] = {r.ackSignalAvg, r.ackSignal, r.signalAvg, r.signal};
    const double managedOrder[] = {r.signal, r.beaconSignalAvg, r.signalAvg, r.ackSignalAvg};
    for (const double dbm : managed ? managedOrder : apOrder) {
        if (!std::isnan(dbm) && dbm != 0.0) return dbm;
    }
    return kNaN;
}

int channelFromFreq(int f) {
    if (f == 2484) return 14;
    if (f >= 2412 && f < 2484) return (f - 2407) / 5;
    if (f == 5935) return 2;  // the one 6 GHz channel below 5950 MHz
    if (f >= 5955 && f <= 7115) return (f - 5950) / 5;
    if (f >= 4910 && f <= 5895) return (f - 5000) / 5;
    if (f >= 58320 && f <= 70200) return (f - 56160) / 2160;
    return 0;
}

unsigned bandFromFreq(int f) {
    if (f >= 2400 && f < 2500) return 1;
    if (f >= 4900 && f < 5925) return 2;
    if (f >= 5925 && f <= 7125) return 3;
    if (f >= 57000 && f <= 71000) return 4;
    return 0;
}

int widthMhzFromNl80211(unsigned w) {
    // enum nl80211_chan_width, by number so older headers without the newest names still build
    switch (w) {
        case 0: return 20;   // 20_NOHT
        case 1: return 20;
        case 2: return 40;
        case 3: return 80;
        case 4: return 160;  // 80P80
        case 5: return 160;
        case 6: return 5;
        case 7: return 10;
        case 8: return 1;
        case 9: return 2;
        case 10: return 4;
        case 11: return 8;
        case 12: return 16;
        case 13: return 320;
        default: return 0;
    }
}

WifiInfo combineAdapters(const WifiAdapters& a) {
    WifiInfo info;
    info.interfaceUp = a.dongle.present || a.home.present;
    const AdapterInfo* used = a.dongle.connected ? &a.dongle : (a.home.connected ? &a.home : nullptr);
    if (used == nullptr) return info;
    info.connected = true;
    info.homeWifi = used == &a.home;
    info.signalDbm = used->signalDbm;
    info.txLinkMbps = used->txLinkMbps;
    info.rxLinkMbps = used->rxLinkMbps;
    info.rxMbps = used->rxMbps;
    info.txMbps = used->txMbps;
    return info;
}

// ---- InterfaceCounters ----

InterfaceCounters::InterfaceCounters() : rxMbps_(kNaN), txMbps_(kNaN) {}

InterfaceCounters::~InterfaceCounters() {
    close();
}

void InterfaceCounters::close() {
    if (rxFd_ >= 0) ::close(rxFd_);
    if (txFd_ >= 0) ::close(txFd_);
    rxFd_ = -1;
    txFd_ = -1;
    reset();
}

void InterfaceCounters::reset() {
    lastTime_ = -1.0;
    rxMbps_ = kNaN;
    txMbps_ = kNaN;
}

void InterfaceCounters::update(const std::string& name, double now) {
    if (name != name_) {
        close();
        name_ = name;
    }
    if (name.empty()) return;
    if (rxFd_ < 0) {
        const std::string base = "/sys/class/net/" + name + "/statistics/";
        rxFd_ = ::open((base + "rx_bytes").c_str(), O_RDONLY | O_CLOEXEC);
        txFd_ = ::open((base + "tx_bytes").c_str(), O_RDONLY | O_CLOEXEC);
    }
    unsigned long long rx = 0;
    unsigned long long tx = 0;
    if (!readCounter(rxFd_, rx) || !readCounter(txFd_, tx)) {
        close();  // e.g. the interface was recreated: reopen next time
        return;
    }
    const double elapsed = now - lastTime_;
    if (lastTime_ >= 0 && elapsed > 0 && rx >= lastRx_ && tx >= lastTx_) {
        rxMbps_ = static_cast<double>(rx - lastRx_) * 8.0 / 1e6 / elapsed;
        txMbps_ = static_cast<double>(tx - lastTx_) * 8.0 / 1e6 / elapsed;
    } else {
        rxMbps_ = kNaN;
        txMbps_ = kNaN;
    }
    lastRx_ = rx;
    lastTx_ = tx;
    lastTime_ = now;
}

// ---- WifiLink ----

WifiLink::~WifiLink() {
    if (socket_ != nullptr) nl_socket_free(socket_);
}

bool WifiLink::ensureSocket() {
    if (socket_ != nullptr) return true;
    socket_ = nl_socket_alloc();
    if (socket_ == nullptr) return false;
    if (genl_connect(socket_) != 0 || (family_ = genl_ctrl_resolve(socket_, "nl80211")) < 0) {
        nl_socket_free(socket_);
        socket_ = nullptr;
        return false;
    }
    return true;
}

bool WifiLink::dump(int command, unsigned ifindex, int (*callback)(struct nl_msg*, void*), void* arg) {
    if (!ensureSocket()) return false;
    nl_msg* message = nlmsg_alloc();
    if (message == nullptr) return false;
    genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family_, 0, NLM_F_DUMP, static_cast<uint8_t>(command), 0);
    if (ifindex != 0) nla_put_u32(message, NL80211_ATTR_IFINDEX, ifindex);
    nl_socket_modify_cb(socket_, NL_CB_VALID, NL_CB_CUSTOM, callback, arg);
    const bool ok = nl_send_auto(socket_, message) >= 0 && nl_recvmsgs_default(socket_) >= 0;
    nlmsg_free(message);
    if (!ok) {
        // A broken socket is rebuilt on the next call
        nl_socket_free(socket_);
        socket_ = nullptr;
    }
    return ok;
}

void WifiLink::readRadio() {
    InterfaceResult found;
    if (!dump(NL80211_CMD_GET_INTERFACE, 0, onInterface, &found)) {
        // nl80211 unreachable: nothing is known about either link
        apIndex_ = 0;
        stationIndex_ = 0;
        radio_ = WifiAdapters();
        return;
    }
    apIndex_ = found.apIndex;
    stationIndex_ = found.stationIndex;
    WifiAdapters r;
    r.dongle.present = apIndex_ != 0;
    r.dongle.ifName = found.apName;
    r.dongle.freqMhz = found.apFreq;
    r.dongle.widthMhz = found.apWidth;
    r.home.present = stationIndex_ != 0;
    r.home.ifName = found.stationName;
    r.home.freqMhz = found.stationFreq;
    r.home.widthMhz = found.stationWidth;

    if (apIndex_ != 0) {
        StationResult s;
        if (dump(NL80211_CMD_GET_STATION, apIndex_, onStation, &s) && s.found) {
            r.dongle.connected = true;
            r.dongle.signalDbm = s.signalDbm;
            r.dongle.txLinkMbps = s.txLinkMbps;
            r.dongle.rxLinkMbps = s.rxLinkMbps;
        }
    }
    if (stationIndex_ != 0) {
        StationResult s;
        s.managed = true;
        if (dump(NL80211_CMD_GET_STATION, stationIndex_, onStation, &s) && s.found) {
            r.home.connected = true;
            r.home.signalDbm = s.signalDbm;
            r.home.txLinkMbps = s.txLinkMbps;
            r.home.rxLinkMbps = s.rxLinkMbps;
        }
    }
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        if (r.dongle.present) r.dongle.ip = ipv4Of(list, r.dongle.ifName);
        if (r.home.present) r.home.ip = ipv4Of(list, r.home.ifName);
        freeifaddrs(list);
    }
    radio_ = r;
}

void WifiLink::resetCounters() {
    apCounters_.reset();
    stationCounters_.reset();
}

WifiAdapters WifiLink::readAdapters(double now) {
    // An interface that vanished or was recreated (SteamVR rebuilds the dongle AP) forces a radio read now
    const bool gone = (apIndex_ != 0 && if_nametoindex(radio_.dongle.ifName.c_str()) != apIndex_) ||
                      (stationIndex_ != 0 && if_nametoindex(radio_.home.ifName.c_str()) != stationIndex_);
    if (lastRadio_ < 0 || now - lastRadio_ >= kRadioIntervalSec || gone) {
        lastRadio_ = now;
        readRadio();
    }
    apCounters_.update(radio_.dongle.ifName, now);
    stationCounters_.update(radio_.home.ifName, now);
    WifiAdapters out = radio_;
    out.dongle.rxMbps = apCounters_.rxMbps();
    out.dongle.txMbps = apCounters_.txMbps();
    out.home.rxMbps = stationCounters_.rxMbps();
    out.home.txMbps = stationCounters_.txMbps();
    return out;
}
