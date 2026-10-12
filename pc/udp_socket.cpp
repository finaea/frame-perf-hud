#include "udp_socket.h"

#include "log.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h>

#include <algorithm>

namespace fph {
namespace pc {

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(std::string& error, HWND notifyWindow, unsigned message) {
    close();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    wsaStarted_ = true;
    s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ == INVALID_SOCKET) {
        error = "socket() failed: " + std::to_string(WSAGetLastError());
        close();
        return false;
    }
    BOOL one = TRUE;
    ::setsockopt(s_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    // Without this, an ICMP port unreachable (no headset at a remembered IP) makes the next recvfrom fail
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "bind() failed: " + std::to_string(WSAGetLastError());
        close();
        return false;
    }
    if (notifyWindow != nullptr) {
        // Also makes the socket non-blocking
        if (WSAAsyncSelect(s_, notifyWindow, message, FD_READ) != 0) {
            error = "WSAAsyncSelect failed: " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
    } else {
        u_long nonBlocking = 1;
        ioctlsocket(s_, FIONBIO, &nonBlocking);
    }
    return true;
}

void UdpSocket::close() {
    if (s_ != INVALID_SOCKET) {
        ::closesocket(s_);
        s_ = INVALID_SOCKET;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

void UdpSocket::send(const Endpoint& to, const std::vector<uint8_t>& datagram) {
    if (s_ == INVALID_SOCKET || datagram.empty()) return;
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(to.ip);
    if (::sendto(s_, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ++sendErrors_;
    }
}

int UdpSocket::receiveAll(LinkClient& client, double now) {
    if (s_ == INVALID_SOCKET) return 0;
    int handled = 0;
    char buffer[2048];
    for (int i = 0; i < 256; ++i) {
        sockaddr_in addr {};
        int len = sizeof(addr);
        const int n = ::recvfrom(s_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&addr), &len);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            // WSAECONNRESET / WSAEMSGSIZE etc.: that datagram is gone, keep reading
            ++receiveErrors_;
            continue;
        }
        Endpoint from;
        from.ip = ntohl(addr.sin_addr.s_addr);
        from.port = ntohs(addr.sin_port);
        client.handlePacket(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(n), from, now);
        ++handled;
    }
    return handled;
}

uint16_t UdpSocket::localPort() const {
    sockaddr_in addr {};
    int len = sizeof(addr);
    if (s_ == INVALID_SOCKET || ::getsockname(s_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

std::vector<uint32_t> interfaceBroadcasts() {
    std::vector<uint32_t> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                a->IfType == IF_TYPE_TUNNEL) {
                continue;
            }
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const uint32_t ip = ntohl(sin->sin_addr.s_addr);
                const unsigned prefix = u->OnLinkPrefixLength;
                if (prefix == 0 || prefix >= 31) continue;
                if ((ip >> 16) == 0xA9FE) continue;  // 169.254/16: no DHCP answer, nothing to find there
                const uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
                out.push_back((ip & mask) | ~mask);
            }
        }
    }
    out.push_back(0xFFFFFFFFu);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<uint32_t> SocketEnv::broadcastAddresses() {
    const unsigned long long now = GetTickCount64();
    if (cached_.empty() || now - cachedAt_ > 10000) {
        cached_ = interfaceBroadcasts();
        cachedAt_ = now;
    }
    return cached_;
}

}  // namespace pc
}  // namespace fph
