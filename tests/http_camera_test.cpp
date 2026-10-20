// HTTP camera (14) headset/http_camera: the reader thread against a tiny local MJPEG server (mouthcam's format).
//   - the request it sends, Connecting -> Live, a tile frame (half size, full size, turned) and the relayed JPEG
//   - server gone -> Unreachable within seconds, server back on the same port -> Live again
//   - a 404 -> NotMjpeg, a bad address -> Unreachable, nothing wanted -> Off / NotWorn (and no connection)
// Linux only (sockets, eventfd).
#include "http_camera.h"

#include <turbojpeg.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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

using fph::cam::CameraState;
using Bytes = std::vector<uint8_t>;

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/** @return true as soon as cond() holds, false after `seconds` */
template <typename F>
bool waitFor(double seconds, F cond) {
    const double end = nowSec() + seconds;
    while (nowSec() < end) {
        if (cond()) return true;
        sleepMs(10);
    }
    return cond();
}

/** A grey JPEG, white on the left half, black on the right. */
Bytes makeJpeg(int w, int h) {
    Bytes grey(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) grey[static_cast<size_t>(y) * w + x] = x < w / 2 ? 255 : 0;
    tjhandle tj = tjInitCompress();
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    const int rc = tjCompress2(tj, grey.data(), w, 0, h, TJPF_GRAY, &buf, &size, TJSAMP_GRAY, 95, TJFLAG_FASTDCT);
    Bytes out;
    if (rc == 0) out.assign(buf, buf + size);
    tjFree(buf);
    tjDestroy(tj);
    return out;
}

/** A local HTTP server: mouthcam's stream (a part with Content-Length every 20 ms) or a canned answer. */
class TestServer {
public:
    enum class Mode { Mjpeg, NotFound };

    ~TestServer() { stop(); }

    /** @param port 0 = any (read it back with port()) */
    bool start(Mode mode, const Bytes& jpeg, int port = 0) {
        mode_ = mode;
        jpeg_ = jpeg;
        listen_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_ < 0) return false;
        const int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_, 4) != 0) {
            ::close(listen_);
            listen_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    /** Closes the listener and every client, like mouthcam being switched off. */
    void stop() {
        if (!thread_.joinable()) return;
        stop_ = true;
        thread_.join();
    }

    int port() const { return port_; }
    int connections() const { return connections_.load(); }
    std::string firstRequest() {
        std::lock_guard<std::mutex> lock(lock_);
        return request_;
    }

private:
    struct Client {
        int fd;
        std::string request;
        bool answered = false;
    };

    Mode mode_ = Mode::Mjpeg;
    Bytes jpeg_;
    int listen_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_ {false};
    std::atomic<int> connections_ {0};
    std::mutex lock_;
    std::string request_;

    void send(Client& c, const std::string& text) { ::send(c.fd, text.data(), text.size(), MSG_NOSIGNAL); }

    void run() {
        std::vector<Client> clients;
        double nextFrame = 0.0;
        while (!stop_) {
            std::vector<pollfd> fds = {{listen_, POLLIN, 0}};
            for (const Client& c : clients) fds.push_back({c.fd, POLLIN, 0});
            ::poll(fds.data(), fds.size(), 10);
            if (fds[0].revents & POLLIN) {
                const int fd = ::accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0) {
                    ++connections_;
                    clients.push_back({fd, "", false});
                }
            }
            for (size_t i = 0; i < clients.size() && i + 1 < fds.size(); ++i) {
                if (!(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                char buf[1024];
                const ssize_t n = ::recv(clients[i].fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n == 0) {  // the client left
                    ::close(clients[i].fd);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
                if (n < 0) continue;
                clients[i].request.append(buf, static_cast<size_t>(n));
                if (!clients[i].answered && clients[i].request.find("\r\n\r\n") != std::string::npos) {
                    clients[i].answered = true;
                    {
                        std::lock_guard<std::mutex> lock(lock_);
                        if (request_.empty()) request_ = clients[i].request;
                    }
                    if (mode_ == Mode::NotFound) {
                        send(clients[i], "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nnot here\n");
                        ::shutdown(clients[i].fd, SHUT_RDWR);
                    } else {
                        send(clients[i],
                             "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=X\r\n\r\n");
                    }
                }
            }
            if (mode_ == Mode::Mjpeg && nowSec() >= nextFrame) {
                nextFrame = nowSec() + 0.02;
                for (Client& c : clients) {
                    if (!c.answered) continue;
                    send(c, "--X\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg_.size()) +
                                "\r\n\r\n");
                    ::send(c.fd, jpeg_.data(), jpeg_.size(), MSG_NOSIGNAL);
                    send(c, "\r\n");
                }
            }
        }
        for (const Client& c : clients) ::close(c.fd);
        ::close(listen_);
        listen_ = -1;
    }
};

std::string urlFor(int port, const char* path = "/preview") {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

/** @return the newest tile frame that arrives within `seconds`, with the requested size if given */
bool newestImage(HttpCamera& camera, CameraImage& out, double seconds, int width = 0, int height = 0) {
    return waitFor(seconds, [&] {
        CameraImage image;
        bool got = false;
        while (camera.takeImage(image)) got = true;  // only the newest matters
        if (got && (width == 0 || (image.width == width && image.height == height))) {
            out = std::move(image);
            return true;
        }
        return false;
    });
}

void testStreamAndReconnect() {
    const Bytes jpeg = makeJpeg(64, 48);
    CHECK(!jpeg.empty());
    TestServer server;
    CHECK(server.start(TestServer::Mode::Mjpeg, jpeg));
    const int port = server.port();

    HttpCamera camera;
    CHECK(camera.state() == CameraState::Off);
    CHECK(camera.start());
    CHECK(camera.eventFd() >= 0);
    CHECK(camera.debugText().find("HTTP camera") != std::string::npos);

    // nothing wanted: nothing connects
    sleepMs(100);
    CHECK(server.connections() == 0 && camera.state() == CameraState::Off);

    HttpCameraWants wants;
    wants.url = urlFor(port);
    wants.display = true;
    wants.displayFps = 30;
    camera.setWants(wants);
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::Live; }));
    CHECK(server.connections() == 1);
    const std::string request = server.firstRequest();
    CHECK(request.compare(0, 22, "GET /preview HTTP/1.0\r") == 0);
    CHECK(request.find("Host: 127.0.0.1:" + std::to_string(port) + "\r\n") != std::string::npos);

    // the eventfd says something is there
    pollfd p {camera.eventFd(), POLLIN, 0};
    CHECK(::poll(&p, 1, 2000) == 1);
    camera.clearEvent();

    // a tile frame: half size by default, left white / right black, opaque
    CameraImage image;
    CHECK(newestImage(camera, image, 3.0));
    CHECK(image.width == 32 && image.height == 24);
    CHECK(image.rgba.size() == static_cast<size_t>(32 * 24 * 4));
    CHECK(image.frameId > 0);
    if (image.rgba.size() == static_cast<size_t>(32 * 24 * 4)) {
        CHECK(image.rgba[(5 * 32 + 4) * 4] > 230 && image.rgba[(5 * 32 + 27) * 4] < 25);
        CHECK(image.rgba[3] == 255);
    }
    CHECK(camera.stateRevision() > 1);

    // no JPEG for the PC unless one asked
    CameraJpeg none;
    while (camera.takeJpeg(none)) {
    }
    sleepMs(150);
    CHECK(!camera.takeJpeg(none));

    // full size, same connection
    wants.fullSize = true;
    camera.setWants(wants);
    CHECK(newestImage(camera, image, 3.0, 64, 48));
    CHECK(image.rgba.size() == static_cast<size_t>(64 * 48 * 4));
    CHECK(server.connections() == 1);

    // rotation 1 (clockwise) swaps the size: the white left half goes to the top
    wants.fullSize = false;
    wants.rotation = 1;
    camera.setWants(wants);
    CHECK(newestImage(camera, image, 3.0, 24, 32));
    CHECK(image.rgba.size() == static_cast<size_t>(24 * 32 * 4));
    if (image.rgba.size() == static_cast<size_t>(24 * 32 * 4)) {
        CHECK(image.rgba[(4 * 24 + 12) * 4] > 230 && image.rgba[(27 * 24 + 12) * 4] < 25);
    }
    wants.rotation = 3;  // the white half at the bottom
    camera.setWants(wants);
    CHECK(newestImage(camera, image, 3.0, 24, 32));
    if (image.rgba.size() == static_cast<size_t>(24 * 32 * 4)) {
        CHECK(image.rgba[(4 * 24 + 12) * 4] < 25 && image.rgba[(27 * 24 + 12) * 4] > 230);
    }
    CHECK(server.connections() == 1);  // none of that reconnected

    // the server goes away: Unreachable within 5 s
    server.stop();
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::Unreachable; }));

    // and is back on the same port: Live again (the retry is every second)
    TestServer again;
    CHECK(again.start(TestServer::Mode::Mjpeg, jpeg, port));
    CHECK(waitFor(6.0, [&] { return camera.state() == CameraState::Live; }));
    CHECK(newestImage(camera, image, 3.0, 24, 32));
    CHECK(again.connections() >= 1);

    // switch everything off: the connection is dropped, state says why
    wants.display = false;
    wants.worn = false;
    camera.setWants(wants);
    CHECK(waitFor(3.0, [&] { return camera.state() == CameraState::NotWorn; }));
    wants.worn = true;
    camera.setWants(wants);
    CHECK(waitFor(3.0, [&] { return camera.state() == CameraState::Off; }));
    const int connections = again.connections();
    sleepMs(1500);
    CHECK(again.connections() == connections);  // not reconnecting while nothing is wanted

    camera.stop();
    camera.stop();  // twice is fine
}

void testRelayAndErrors() {
    const Bytes jpeg = makeJpeg(64, 48);
    TestServer server;
    CHECK(server.start(TestServer::Mode::Mjpeg, jpeg));

    HttpCamera camera;
    CHECK(camera.start());
    HttpCameraWants wants;
    wants.url = urlFor(server.port(), "/some/other/path?x=1");
    wants.pc = true;  // a PC asked, the HUD isn't shown
    wants.pcFps = 30;
    camera.setWants(wants);
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::Live; }));
    CameraJpeg out;
    CHECK(waitFor(3.0, [&] { return camera.takeJpeg(out); }));
    CHECK(out.data == jpeg);  // untouched
    CHECK(out.width == 64 && out.height == 48 && out.frameId > 0);
    CHECK(server.firstRequest().compare(0, 33, "GET /some/other/path?x=1 HTTP/1.0") == 0);
    CameraImage image;
    CHECK(!camera.takeImage(image));  // display was off: no tile decode
    // the newest only: the next one has a newer id
    const uint32_t id = out.frameId;
    CHECK(waitFor(3.0, [&] { return camera.takeJpeg(out) && out.frameId > id; }));

    // a server that says 404: NotMjpeg
    TestServer notFound;
    CHECK(notFound.start(TestServer::Mode::NotFound, jpeg));
    wants.url = urlFor(notFound.port());
    camera.setWants(wants);
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::NotMjpeg; }));

    // a bad address: nothing to connect to
    wants.url = "http://";
    camera.setWants(wants);
    CHECK(waitFor(3.0, [&] { return camera.state() == CameraState::Unreachable; }));

    // nobody listening: Unreachable
    int dead = 0;
    {
        TestServer temp;
        CHECK(temp.start(TestServer::Mode::Mjpeg, jpeg));
        dead = temp.port();
    }
    wants.url = urlFor(dead);
    camera.setWants(wants);
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::Unreachable; }));

    // it recovers when the real one is back (new address)
    wants.url = urlFor(server.port());
    camera.setWants(wants);
    CHECK(waitFor(5.0, [&] { return camera.state() == CameraState::Live; }));

    camera.stop();
}

}  // namespace

int main() {
    testStreamAndReconnect();
    testRelayAndErrors();
    if (failures == 0) std::printf("http-camera-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
