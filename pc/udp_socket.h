// The PC's one UDP socket (Winsock): ephemeral port, used for everything, so the headset's replies come back
// through Windows' stateful UDP filter with no inbound firewall rule. Non-blocking; the window gets FD_READ.
#pragma once

#include "link_client.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

namespace fph {
namespace pc {

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /**
     * Opens 0.0.0.0:0 with SO_BROADCAST, non-blocking, ICMP "port unreachable" resets turned off.
     * @param notifyWindow if set, the window gets `message` on FD_READ (WSAAsyncSelect)
     * @return false + error if it couldn't
     */
    bool open(std::string& error, HWND notifyWindow = nullptr, unsigned message = 0);
    void close();
    bool isOpen() const { return s_ != INVALID_SOCKET; }

    /** Sends one datagram; errors are counted, not reported. */
    void send(const Endpoint& to, const std::vector<uint8_t>& datagram);

    /**
     * Reads every queued datagram (at most 256 per call) into the client. WSAECONNRESET (an ICMP port unreachable
     * for an earlier send) and other per-datagram errors are skipped, never fatal.
     * @return datagrams handled
     */
    int receiveAll(LinkClient& client, double now);

    /** @return the bound local port (tests) */
    uint16_t localPort() const;

    uint64_t sendErrors() const { return sendErrors_; }
    uint64_t receiveErrors() const { return receiveErrors_; }

private:
    SOCKET s_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
    uint64_t sendErrors_ = 0;
    uint64_t receiveErrors_ = 0;
};

/**
 * @return each up IPv4 interface's directed broadcast address (from GetAdaptersAddresses; loopback and tunnels
 *         skipped), plus 255.255.255.255. Host order.
 */
std::vector<uint32_t> interfaceBroadcasts();

/** LinkClientEnv over a UdpSocket; the broadcast list is refreshed at most every 10 s (DHCP, cables, dongle). */
class SocketEnv : public LinkClientEnv {
public:
    explicit SocketEnv(UdpSocket& socket) : socket_(socket) {}
    void send(const Endpoint& to, const std::vector<uint8_t>& datagram) override { socket_.send(to, datagram); }
    std::vector<uint32_t> broadcastAddresses() override;

private:
    UdpSocket& socket_;
    std::vector<uint32_t> cached_;
    unsigned long long cachedAt_ = 0;
};

}  // namespace pc
}  // namespace fph
