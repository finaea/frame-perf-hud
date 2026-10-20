// HTTP camera (14): see http_camera.h
#include "http_camera.h"

#include "mjpeg_stream.h"

#include <turbojpeg.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

using fph::cam::CameraState;
namespace cam = fph::cam;

namespace {

constexpr double kConnectTimeoutSec = 1.0;
constexpr double kRetrySec = 1.0;      ///< after a failure (mouthcam stopped, restarting)
constexpr double kStallSec = 3.0;      ///< connected, but no bytes this long: reconnect
constexpr double kDebugEverySec = 5.0;
constexpr size_t kRecvChunk = 64 * 1024;

double monoNow() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

double threadCpu() {
    timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

}  // namespace

/** Everything the HTTP camera thread owns. */
class HttpCameraWorker {
public:
    explicit HttpCameraWorker(HttpCamera& svc) : svc_(svc), tj_(tjInitDecompress()) {
        cpuMark_ = threadCpu();
        wallMark_ = monoNow();
    }
    ~HttpCameraWorker() {
        disconnect();
        if (tj_) tjDestroy(tj_);
    }

    void loop();

private:
    HttpCamera& svc_;
    tjhandle tj_;
    HttpCameraWants wants_;
    fph::HttpUrl url_;
    bool urlOk_ = false;

    int fd_ = -1;
    bool connecting_ = false;
    double connectStarted_ = 0.0;
    double retryAt_ = 0.0;
    double lastData_ = 0.0;
    bool answered_ = false;  ///< this connection got any bytes
    fph::mjpeg::StreamParser parser_;
    std::vector<uint8_t> chunk_ = std::vector<uint8_t>(kRecvChunk);
    std::vector<uint8_t> jpeg_;
    std::vector<uint8_t> grey_;
    std::vector<uint8_t> turned_;
    cam::RateGate displayGate_ {15.0};
    cam::RateGate pcGate_ {10.0};
    uint32_t frameId_ = 0;

    // debug, per window
    std::string lastError_;
    uint64_t framesIn_ = 0, decoded_ = 0, relayed_ = 0, bytesIn_ = 0;
    double decodeMs_ = 0.0;
    int srcW_ = 0, srcH_ = 0;
    double cpuMark_ = 0.0, wallMark_ = 0.0, nextDebug_ = 0.0;

    void applyWants(const HttpCameraWants& w, double t);
    void startConnect(double t);
    void onConnected(double t);
    void readSome(double t);
    void fail(double t, CameraState state, const std::string& why);
    void disconnect();
    void handleFrame(double t);
    void writeDebug(double t);
};

void HttpCameraWorker::applyWants(const HttpCameraWants& w, double t) {
    const bool reconnect = w.url != wants_.url || w.active() != wants_.active();
    wants_ = w;
    displayGate_.setFps(w.displayFps);
    pcGate_.setFps(w.pcFps);
    if (reconnect) {
        disconnect();
        retryAt_ = t;
        urlOk_ = fph::parseHttpUrl(w.url, url_);
        if (w.active()) svc_.setState(CameraState::Connecting);
    }
}

void HttpCameraWorker::startConnect(double t) {
    if (!urlOk_) {
        fail(t, CameraState::Unreachable, "bad address");
        return;
    }
    // A name lookup blocks this thread only (numeric addresses, like mouthcam's 127.0.0.1, don't look anything up)
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(url_.port);
    if (::getaddrinfo(url_.host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        fail(t, CameraState::Unreachable, "can't resolve " + url_.host);
        return;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        ::freeaddrinfo(found);
        fail(t, CameraState::Unreachable, std::string("socket: ") + std::strerror(errno));
        return;
    }
    const int rc = ::connect(fd_, found->ai_addr, found->ai_addrlen);
    ::freeaddrinfo(found);
    if (rc != 0 && errno != EINPROGRESS) {
        fail(t, CameraState::Unreachable, std::string("connect: ") + std::strerror(errno));
        return;
    }
    connecting_ = true;
    connectStarted_ = t;
}

void HttpCameraWorker::onConnected(double t) {
    connecting_ = false;
    int soError = 0;
    socklen_t len = sizeof(soError);
    ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soError, &len);
    if (soError != 0) {
        fail(t, CameraState::Unreachable, std::string("connect: ") + std::strerror(soError));
        return;
    }
    const std::string host = url_.port == 80 ? url_.host : url_.host + ":" + std::to_string(url_.port);
    const std::string request = "GET " + url_.path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
    if (::send(fd_, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
        fail(t, CameraState::Unreachable, "couldn't send the request");
        return;
    }
    parser_.reset();
    lastData_ = t;
    answered_ = false;
}

void HttpCameraWorker::readSome(double t) {
    for (int k = 0; k < 16 && fd_ >= 0; ++k) {  // drain what's there, but come back to the loop now and then
        const ssize_t n = ::recv(fd_, chunk_.data(), chunk_.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (n <= 0) {
            // closed before a whole HTTP header came: something answered, but not an MJPEG server (ssh, ...)
            const bool notHttp = n == 0 && parser_.status() == fph::mjpeg::StreamParser::Status::Header && answered_;
            fail(t, notHttp ? CameraState::NotMjpeg : CameraState::Unreachable,
                 n == 0 ? "the server closed the stream" : std::strerror(errno));
            return;
        }
        lastData_ = t;
        answered_ = true;
        bytesIn_ += static_cast<uint64_t>(n);
        const bool found = parser_.feed(chunk_.data(), static_cast<size_t>(n));
        if (parser_.status() == fph::mjpeg::StreamParser::Status::NotMjpeg) {
            fail(t, CameraState::NotMjpeg, parser_.error());
            return;
        }
        if (found && parser_.takeFrame(jpeg_)) handleFrame(t);
    }
}

void HttpCameraWorker::handleFrame(double t) {
    ++framesIn_;
    svc_.setState(CameraState::Live);
    lastError_.clear();
    const bool forDisplay = wants_.display && displayGate_.take(t);
    const bool forPc = wants_.pc && pcGate_.take(t);
    if (!forDisplay && !forPc) return;
    int w = 0, h = 0, sub = 0, colorspace = 0;
    if (!tj_ || tjDecompressHeader3(tj_, jpeg_.data(), static_cast<unsigned long>(jpeg_.size()), &w, &h, &sub,
                                    &colorspace) != 0 ||
        w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        lastError_ = "a frame isn't a JPEG TurboJPEG can read";
        return;
    }
    srcW_ = w;
    srcH_ = h;
    ++frameId_;
    if (forDisplay) {
        const double t0 = monoNow();
        // Half size by default: TurboJPEG's scaled decode skips most of the IDCT work
        const tjscalingfactor factor = wants_.fullSize ? tjscalingfactor {1, 1} : tjscalingfactor {1, 2};
        const int sw = TJSCALED(w, factor);
        const int sh = TJSCALED(h, factor);
        grey_.resize(static_cast<size_t>(sw) * sh);
        if (tjDecompress2(tj_, jpeg_.data(), static_cast<unsigned long>(jpeg_.size()), grey_.data(), sw, sw, sh,
                          TJPF_GRAY, TJFLAG_FASTDCT) == 0) {
            const uint8_t* src = grey_.data();
            CameraImage image;
            image.width = sw;
            image.height = sh;
            if (wants_.rotation & 3) {
                turned_.resize(grey_.size());
                cam::rotateGrey(turned_.data(), grey_.data(), sw, sh, wants_.rotation);
                src = turned_.data();
                if (wants_.rotation & 1) std::swap(image.width, image.height);
            }
            image.rgba.resize(grey_.size() * 4);
            cam::greyToRgba(image.rgba.data(), src, grey_.size());
            image.frameId = frameId_;
            svc_.post(&image, nullptr);
            ++decoded_;
            decodeMs_ += (monoNow() - t0) * 1e3;
        } else {
            lastError_ = std::string("JPEG: ") + tjGetErrorStr2(tj_);
        }
    }
    if (forPc) {  // as it came: the PC decodes and turns it
        CameraJpeg out;
        out.width = static_cast<uint16_t>(w);
        out.height = static_cast<uint16_t>(h);
        out.data = jpeg_;
        out.frameId = frameId_;
        svc_.post(nullptr, &out);
        ++relayed_;
    }
}

void HttpCameraWorker::fail(double t, CameraState state, const std::string& why) {
    disconnect();
    lastError_ = why;
    retryAt_ = t + kRetrySec;
    if (wants_.active()) svc_.setState(state);
}

void HttpCameraWorker::disconnect() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    connecting_ = false;
}

void HttpCameraWorker::writeDebug(double t) {
    const double cpu = threadCpu();
    const double wall = t - wallMark_;
    if (wall <= 0.0) return;
    char line[640];
    std::snprintf(line, sizeof(line),
                  "== HTTP camera ==\n%s | %s | wants display %d (%.0f fps%s, %d°) pc %d (%.0f fps)\n"
                  "frames in %.1f/s (%dx%d), decoded %.1f/s (%.2f ms each), relayed %.1f/s, %.0f kB/s | parser frames "
                  "%llu dropped %llu | thread CPU %.2f%% of one core over %.1f s%s%s\n",
                  wants_.url.c_str(), fd_ >= 0 ? (connecting_ ? "connecting" : "connected") : "disconnected",
                  int(wants_.display), wants_.displayFps, wants_.fullSize ? ", full res" : "", wants_.rotation * 90,
                  int(wants_.pc), wants_.pcFps, framesIn_ / wall, srcW_, srcH_, decoded_ / wall,
                  decoded_ ? decodeMs_ / decoded_ : 0.0, relayed_ / wall, bytesIn_ / 1024.0 / wall,
                  static_cast<unsigned long long>(parser_.frames()), static_cast<unsigned long long>(parser_.dropped()),
                  100.0 * (cpu - cpuMark_) / wall, wall, lastError_.empty() ? "" : " | last error: ",
                  lastError_.c_str());
    {
        std::lock_guard<std::mutex> lock(svc_.debugLock_);
        svc_.debug_ = line;
        HttpCamera::Counters& c = svc_.counters_;
        c.framesIn += framesIn_;
        c.decoded += decoded_;
        c.relayed += relayed_;
        c.decodeMs += decodeMs_;
        c.bytesIn += bytesIn_;
    }
    framesIn_ = decoded_ = relayed_ = bytesIn_ = 0;
    decodeMs_ = 0.0;
    cpuMark_ = cpu;
    wallMark_ = t;
}

void HttpCameraWorker::loop() {
    while (!svc_.stop_.load()) {
        double t = monoNow();
        bool changed = false;
        const HttpCameraWants w = svc_.readWants(changed);
        if (changed) applyWants(w, t);
        if (!wants_.active()) {
            disconnect();
            nextDebug_ = 0.0;
            svc_.setState(wants_.url.empty() || wants_.worn ? CameraState::Off : CameraState::NotWorn);
        } else if (fd_ < 0 && t >= retryAt_) {
            startConnect(t);
        } else if (connecting_ && t - connectStarted_ > kConnectTimeoutSec) {
            fail(t, CameraState::Unreachable, "connect: timed out");
        } else if (fd_ >= 0 && !connecting_ && t - lastData_ > kStallSec) {
            fail(t, CameraState::Unreachable, "no data for 3 s");
        }
        if (wants_.active() && t >= nextDebug_) {
            if (nextDebug_ > 0.0) {
                writeDebug(t);
            } else {  // just turned on: the first window starts now
                wallMark_ = t;
                cpuMark_ = threadCpu();
            }
            nextDebug_ = t + kDebugEverySec;
        }

        double at = 1e18;
        if (wants_.active()) {
            at = nextDebug_;
            if (fd_ < 0) at = std::min(at, retryAt_);
            else if (connecting_) at = std::min(at, connectStarted_ + kConnectTimeoutSec);
            else at = std::min(at, lastData_ + kStallSec);
        }
        t = monoNow();
        const int timeoutMs = at >= 1e17 ? -1 : std::max(0, int(std::ceil((at - t) * 1000.0)));
        pollfd p[2] = {{svc_.wakeFd_, POLLIN, 0}, {fd_, static_cast<short>(connecting_ ? POLLOUT : POLLIN), 0}};
        const int n = ::poll(p, fd_ >= 0 ? 2 : 1, timeoutMs);
        if (n > 0 && (p[0].revents & POLLIN)) {
            uint64_t v;
            (void)!::read(svc_.wakeFd_, &v, sizeof(v));
        }
        if (n > 0 && fd_ >= 0 && p[1].revents) {
            t = monoNow();
            if (connecting_) onConnected(t);
            else readSome(t);
        }
    }
    disconnect();
}

// ------------------------------------------------------------------------------------------------ service

HttpCamera::HttpCamera() {
    state_.store(static_cast<uint8_t>(CameraState::Off));
}

HttpCamera::~HttpCamera() {
    stop();
    if (eventFd_ >= 0) ::close(eventFd_);
    if (wakeFd_ >= 0) ::close(wakeFd_);
}

bool HttpCamera::start() {
    if (thread_.joinable()) return true;
    if (eventFd_ < 0) eventFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd_ < 0 || wakeFd_ < 0) return false;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void HttpCamera::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    const uint64_t one = 1;
    (void)!::write(wakeFd_, &one, sizeof(one));
    thread_.join();
}

void HttpCamera::run() {
    HttpCameraWorker worker(*this);
    worker.loop();
}

void HttpCamera::clearEvent() {
    uint64_t v;
    if (eventFd_ >= 0) (void)!::read(eventFd_, &v, sizeof(v));
}

void HttpCamera::signal() {
    const uint64_t one = 1;
    if (eventFd_ >= 0) (void)!::write(eventFd_, &one, sizeof(one));
}

void HttpCamera::setWants(const HttpCameraWants& wants) {
    {
        std::lock_guard<std::mutex> lock(wantsLock_);
        if (wants == wants_) return;
        wants_ = wants;
        wantsDirty_ = true;
    }
    const uint64_t one = 1;
    if (wakeFd_ >= 0) (void)!::write(wakeFd_, &one, sizeof(one));
}

HttpCameraWants HttpCamera::readWants(bool& changed) {
    std::lock_guard<std::mutex> lock(wantsLock_);
    changed = wantsDirty_;
    wantsDirty_ = false;
    return wants_;
}

void HttpCamera::post(CameraImage* image, CameraJpeg* jpeg) {
    {
        std::lock_guard<std::mutex> lock(boxLock_);
        if (image) {
            image_ = std::move(*image);
            imageFresh_ = true;
        }
        if (jpeg) {
            jpeg_ = std::move(*jpeg);
            jpegFresh_ = true;
        }
    }
    signal();
}

bool HttpCamera::takeImage(CameraImage& out) {
    std::lock_guard<std::mutex> lock(boxLock_);
    if (!imageFresh_) return false;
    std::swap(out, image_);
    imageFresh_ = false;
    return true;
}

bool HttpCamera::takeJpeg(CameraJpeg& out) {
    std::lock_guard<std::mutex> lock(boxLock_);
    if (!jpegFresh_) return false;
    std::swap(out, jpeg_);
    jpegFresh_ = false;
    return true;
}

void HttpCamera::setState(CameraState state) {
    if (state_.exchange(static_cast<uint8_t>(state)) != static_cast<uint8_t>(state)) {
        stateRevision_.fetch_add(1);
        signal();
    }
}

std::string HttpCamera::debugText() const {
    std::lock_guard<std::mutex> lock(debugLock_);
    return debug_.empty() ? "== HTTP camera ==\noff\n" : debug_;
}

HttpCamera::Counters HttpCamera::counters() const {
    std::lock_guard<std::mutex> lock(debugLock_);
    return counters_;
}

// ------------------------------------------------------------------------------------------------ bench

int runHttpCameraBench(const HttpCameraBenchOptions& options, const volatile std::sig_atomic_t& stop) {
    HttpCamera camera;
    if (!camera.start()) {
        std::fprintf(stderr, "[http-camera] couldn't start the thread\n");
        return 1;
    }
    HttpCameraWants wants;
    wants.url = options.url;
    wants.display = true;
    wants.pc = options.pc;
    wants.displayFps = options.fps;
    wants.fullSize = options.fullSize;
    wants.rotation = options.rotation;
    camera.setWants(wants);
    std::fprintf(stderr, "[http-camera] reading %s for %.0f s (tile %.0f fps%s, %d°%s)\n", options.url.c_str(),
                 options.seconds, options.fps, options.fullSize ? ", full res" : "", options.rotation * 90,
                 options.pc ? ", relaying JPEGs" : "");
    timespec cpu0 {};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu0);
    const double start = monoNow();
    uint64_t images = 0, jpegs = 0, jpegBytes = 0;
    bool saved = options.snapshotDir.empty();
    std::string lastState;
    while (!stop && monoNow() - start < options.seconds) {
        pollfd p {camera.eventFd(), POLLIN, 0};
        ::poll(&p, 1, 200);
        camera.clearEvent();
        CameraImage image;
        if (camera.takeImage(image)) {
            ++images;
            if (!saved) {
                const std::string path = options.snapshotDir + "/http-camera.pgm";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(f, "P5\n%d %d\n255\n", image.width, image.height);
                    for (size_t i = 0; i < image.rgba.size(); i += 4) std::fputc(image.rgba[i], f);
                    std::fclose(f);
                    std::fprintf(stderr, "[http-camera] first frame %dx%d -> %s\n", image.width, image.height,
                                 path.c_str());
                }
                saved = true;
            }
        }
        CameraJpeg jpeg;
        if (camera.takeJpeg(jpeg)) {
            ++jpegs;
            jpegBytes += jpeg.data.size();
        }
        const std::string state = cam::cameraStateText(camera.state(), fph::httpUrlHostPort(options.url));
        if (state != lastState) {
            std::fprintf(stderr, "[http-camera] state %d %s\n", int(camera.state()), state.c_str());
            lastState = state;
        }
    }
    const double wall = monoNow() - start;
    timespec cpu1 {};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu1);
    const double cpu = double(cpu1.tv_sec - cpu0.tv_sec) + double(cpu1.tv_nsec - cpu0.tv_nsec) / 1e9;
    std::fputs(camera.debugText().c_str(), stderr);  // the last 5 s window
    camera.stop();
    const HttpCamera::Counters c = camera.counters();  // whole 5 s windows only
    std::fprintf(stderr,
                 "[http-camera] %.1f s: tile frames %.1f/s, JPEGs relayed %.1f/s (%.0f kB each) | windows: frames in "
                 "%llu, decoded %llu (%.2f ms each), %.0f kB/s in | process CPU %.2f%% of one core\n",
                 wall, images / wall, jpegs / wall, jpegs ? jpegBytes / 1024.0 / jpegs : 0.0,
                 static_cast<unsigned long long>(c.framesIn), static_cast<unsigned long long>(c.decoded),
                 c.decoded ? c.decodeMs / c.decoded : 0.0, c.bytesIn / 1024.0 / std::max(1.0, wall),
                 100.0 * cpu / wall);
    return images > 0 ? 0 : 1;
}
