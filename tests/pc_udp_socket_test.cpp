// pc/udp_socket over loopback only: send/receive, and receiveAll surviving the ICMP "port unreachable" reset
// (WSAECONNRESET) that an earlier send to a dead port leaves on the next recvfrom. No traffic leaves this machine.
#include "link_client.h"
#include "link_wire.h"
#include "udp_socket.h"

#include <ws2tcpip.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;
using namespace fph::link;
using namespace fph::pc;

constexpr uint32_t kLoopback = 0x7F000001u;

class NullEnv : public LinkClientEnv {
public:
    void send(const Endpoint&, const std::vector<uint8_t>&) override {}
    std::vector<uint32_t> broadcastAddresses() override { return {}; }
};

/** A plain loopback socket bound to an ephemeral port (non-blocking when asked). */
struct RawSocket {
    SOCKET s = INVALID_SOCKET;
    uint16_t port = 0;
    RawSocket() {
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(kLoopback);
        ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        int len = sizeof(a);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        DWORD timeoutMs = 1000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    }
    ~RawSocket() { close(); }
    void close() {
        if (s != INVALID_SOCKET) ::closesocket(s);
        s = INVALID_SOCKET;
    }
    void sendTo(uint16_t toPort, const std::vector<uint8_t>& data) {
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(toPort);
        a.sin_addr.s_addr = htonl(kLoopback);
        ::sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                 reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }
};

std::vector<uint8_t> helloAckPacket() {
    HelloAck a;
    a.headsetId = Id128 {};
    a.name = "loop";
    a.status = PeerStatus::Pending;
    return encodePacket(PacketType::HelloAck, 1, encodeHelloAck(a));
}

int receiveWithin(UdpSocket& sock, LinkClient& client, int ms) {
    int total = 0;
    for (int waited = 0; waited <= ms && total == 0; waited += 10) {
        total += sock.receiveAll(client, 1.0);
        if (total == 0) Sleep(10);
    }
    return total;
}

}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // interfaceBroadcasts always ends with the limited broadcast, and never repeats an address
    {
        const std::vector<uint32_t> b = interfaceBroadcasts();
        bool limited = false;
        for (size_t i = 0; i < b.size(); ++i) {
            limited |= b[i] == 0xFFFFFFFFu;
            for (size_t j = i + 1; j < b.size(); ++j) CHECK(b[i] != b[j]);
        }
        CHECK(limited);
    }

    UdpSocket sock;
    CHECK(!sock.isOpen());
    std::string error;
    CHECK(sock.open(error));
    CHECK(error.empty());
    CHECK(sock.isOpen());
    CHECK(sock.localPort() != 0);

    NullEnv env;
    LinkClient client(env, Id128 {}, "loop");

    // Nothing queued: receiveAll returns at once with 0
    CHECK(sock.receiveAll(client, 0.0) == 0);

    // send -> a second socket gets the datagram
    RawSocket peer;
    const std::vector<uint8_t> hello = encodePacket(PacketType::Hello, 7, {1, 2, 3});
    sock.send(Endpoint {kLoopback, peer.port}, hello);
    char buf[2048];
    const int got = ::recv(peer.s, buf, sizeof(buf), 0);
    CHECK(got == static_cast<int>(hello.size()));
    CHECK(got > 0 && std::vector<uint8_t>(buf, buf + got) == hello);
    CHECK(sock.sendErrors() == 0);

    // A datagram to a port nobody listens on (bind, learn the port, close) provokes ICMP port unreachable
    uint16_t deadPort = 0;
    {
        RawSocket dead;
        deadPort = dead.port;
    }
    for (int i = 0; i < 3; ++i) sock.send(Endpoint {kLoopback, deadPort}, hello);
    Sleep(100);

    // A real datagram from the peer after those: it must still arrive, and receiveAll must not give up
    peer.sendTo(sock.localPort(), helloAckPacket());
    Sleep(50);
    const int handled = receiveWithin(sock, client, 1000);
    CHECK(handled == 1);
    CHECK(client.malformed() == 0);
    CHECK(sock.receiveAll(client, 1.0) == 0);

    // Again, with the dead-port sends interleaved after the real datagram is queued
    peer.sendTo(sock.localPort(), helloAckPacket());
    sock.send(Endpoint {kLoopback, deadPort}, hello);
    Sleep(100);
    peer.sendTo(sock.localPort(), helloAckPacket());
    Sleep(50);
    int total = receiveWithin(sock, client, 1000);
    total += sock.receiveAll(client, 1.0);
    CHECK(total == 2);

    // Garbage datagrams reach the client and are counted as malformed, not fatal
    peer.sendTo(sock.localPort(), {9, 9, 9});
    Sleep(50);
    CHECK(receiveWithin(sock, client, 1000) == 1);
    CHECK(client.malformed() == 1);

    // After close: sends and reads are harmless
    sock.close();
    CHECK(!sock.isOpen());
    sock.send(Endpoint {kLoopback, peer.port}, hello);
    CHECK(sock.receiveAll(client, 1.0) == 0);
    CHECK(sock.localPort() == 0);

    // Reopen works
    CHECK(sock.open(error));
    CHECK(sock.localPort() != 0);

    WSACleanup();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc udp socket: all checks passed\n");
    return 0;
}
