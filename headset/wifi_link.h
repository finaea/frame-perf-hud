// Wireless state of both links: the Steam Link dongle (the interface where the Frame is an AP, `wlanap`) and
// home Wi-Fi (the interface where the Frame is a station, `wlan0`). nl80211 is read directly with libnl
// (no `iw` process). Interfaces are told apart by nl80211 type (AP / station), not by name. MAC addresses
// and SSIDs are never read out.
#pragma once

#include <string>

struct nl_sock;

/**
 * Upstream's single combined link (the upstream panel's Wi-Fi line). NaN where there's no value.
 * The dongle if it has a peer, otherwise the home Wi-Fi access point.
 */
struct WifiInfo {
    bool interfaceUp = false;   ///< the dongle AP or a home Wi-Fi station interface exists
    bool connected = false;     ///< one of them has a peer
    bool homeWifi = false;      ///< true = home Wi-Fi, false = dongle
    double signalDbm;           ///< dBm (dongle: ack signal; home Wi-Fi: signal)
    double txLinkMbps;          ///< PHY rate Frame → peer (PC / AP)
    double rxLinkMbps;          ///< PHY rate peer → Frame
    double rxMbps;              ///< actually received (peer → Frame; the video goes this way)
    double txMbps;              ///< actually sent

    WifiInfo();
};

/** One interface (widget 9 instance). NaN / 0 / empty where there's no value. */
struct AdapterInfo {
    bool present = false;    ///< the interface exists
    bool connected = false;  ///< it has a peer (a station on the AP, or the access point we're on)
    std::string ifName;
    std::string ip;          ///< first IPv4 address, empty if none
    double rxMbps;           ///< from /sys/class/net/<if>/statistics, NaN on the first read
    double txMbps;
    double txLinkMbps;
    double rxLinkMbps;
    double signalDbm;
    int freqMhz = 0;   ///< control channel; 0 = unknown
    int widthMhz = 0;  ///< 0 = unknown

    AdapterInfo();
};

/** Both interfaces from one read. */
struct WifiAdapters {
    AdapterInfo dongle;  ///< nl80211 type AP
    AdapterInfo home;    ///< nl80211 type station
};

/** Signal readings of one station, NaN where nl80211 didn't send the attribute. */
struct SignalReadings {
    double signal;           ///< NL80211_STA_INFO_SIGNAL (iw "signal")
    double signalAvg;        ///< NL80211_STA_INFO_SIGNAL_AVG ("signal avg")
    double beaconSignalAvg;  ///< NL80211_STA_INFO_BEACON_SIGNAL_AVG ("beacon signal avg")
    double ackSignal;        ///< NL80211_STA_INFO_ACK_SIGNAL ("last ack signal")
    double ackSignalAvg;     ///< NL80211_STA_INFO_ACK_SIGNAL_AVG ("avg ack signal")

    SignalReadings();
};

/**
 * Picks one signal value. 0 dBm counts as "not measured" and is skipped.
 * - AP side (dongle): `signal` reads 0 dBm on the Frame, so avg ack signal, then last ack signal, then the rest.
 * - Station side (home Wi-Fi): `signal`, like the OS shows it, then beacon avg, signal avg, avg ack signal.
 *   On the Frame: signal −49, beacon avg −48, avg ack −59, signal avg 0.
 * @param r the readings
 * @param managed true for the station side
 * @return dBm, NaN if none is usable
 */
double chooseSignalDbm(const SignalReadings& r, bool managed);

/** @return the channel number for a frequency (2.4 / 5 / 6 / 60 GHz), 0 if unknown */
int channelFromFreq(int freqMhz);

/**
 * @return the band for a frequency, as an fph::WifiBand value (core/metric_registry.h): 0 unknown, 1 2.4 GHz,
 *         2 5 GHz, 3 6 GHz, 4 60 GHz. A plain number so this file needs nothing from core/.
 */
unsigned bandFromFreq(int freqMhz);

/**
 * @param nl80211Width an enum nl80211_chan_width value
 * @return the width in MHz, 0 if unknown
 */
int widthMhzFromNl80211(unsigned nl80211Width);

/** @return upstream's combined link: the dongle if it has a peer, else home Wi-Fi */
WifiInfo combineAdapters(const WifiAdapters& adapters);

/**
 * Byte counters of one interface → Mbps since the previous call. Keeps its files open.
 */
class InterfaceCounters {
public:
    InterfaceCounters();
    ~InterfaceCounters();
    InterfaceCounters(const InterfaceCounters&) = delete;
    InterfaceCounters& operator=(const InterfaceCounters&) = delete;

    /**
     * Reads rx_bytes / tx_bytes. A changed name reopens the files and restarts the delta.
     * @param name interface name (empty: nothing to read, rates go NaN)
     * @param now monotonic seconds
     */
    void update(const std::string& name, double now);

    /** Starts a fresh delta (after a pause): the next update gives NaN. */
    void reset();

    double rxMbps() const { return rxMbps_; }
    double txMbps() const { return txMbps_; }

private:
    std::string name_;
    int rxFd_ = -1;
    int txFd_ = -1;
    double lastTime_ = -1.0;
    unsigned long long lastRx_ = 0;
    unsigned long long lastTx_ = 0;
    double rxMbps_;
    double txMbps_;

    void close();
};

/**
 * Holds one nl80211 socket and queries interfaces and stations. Never writes (no settings changes).
 */
class WifiLink {
public:
    WifiLink() = default;
    ~WifiLink();
    WifiLink(const WifiLink&) = delete;
    WifiLink& operator=(const WifiLink&) = delete;

    /**
     * Reads both interfaces. Byte counters every call; the radio side (nl80211 interfaces + stations,
     * IP addresses) at most every kRadioIntervalSec, cached in between, or at once if an interface vanished.
     * @param now monotonic seconds
     * @return both adapters
     */
    WifiAdapters readAdapters(double now);

    /** Starts fresh byte-counter deltas (after a pause). */
    void resetCounters();

    /** @return the dongle / home interface name found by the last radio read ("" before the first, or if none) */
    const std::string& dongleName() const { return radio_.dongle.ifName; }
    const std::string& homeName() const { return radio_.home.ifName; }

    /**
     * Upstream's combined read (kept for Sensors::readWifi).
     * @param now monotonic seconds
     */
    WifiInfo read(double now) { return combineAdapters(readAdapters(now)); }

    /** The radio side is re-read at most this often (seconds). Just under 2 s, so a 2 s cadence never skips. */
    static constexpr double kRadioIntervalSec = 1.9;

private:
    nl_sock* socket_ = nullptr;
    int family_ = -1;
    double lastRadio_ = -1.0;
    unsigned apIndex_ = 0;       ///< 0 = none
    unsigned stationIndex_ = 0;  ///< 0 = none
    WifiAdapters radio_;         ///< last radio read (rates not filled in)
    InterfaceCounters apCounters_;
    InterfaceCounters stationCounters_;

    bool ensureSocket();
    bool dump(int command, unsigned ifindex, int (*callback)(struct nl_msg*, void*), void* arg);
    void readRadio();
};
