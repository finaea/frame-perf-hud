// T10 headset/camera_service: the mailbox / eventfd surface, without ever running the camera helper.
//   - start() with no wants spawns no process (nothing touches the helper or /dev/video*), eventFd() is valid,
//     the take calls are empty, state() is Off, stop() joins promptly.
//   - post() / setState() (private; opened up with `#define private public` for this file only, no product
//     change) feed the mailbox: latest frame wins, one slot per camera, the eventfd signals.
// This test never calls setWants() with a camera, so the helper is never started.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#define private public
#include "camera_service.h"
#undef private

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph::cam;

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @return how many processes have us as their parent (a spawned helper would show here) */
int childProcesses() {
    const pid_t me = getpid();
    int n = 0;
    DIR* d = opendir("/proc");
    if (!d) return -1;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string path = std::string("/proc/") + e->d_name + "/stat";
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        char buf[512];
        const size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[len] = 0;
        const char* p = std::strrchr(buf, ')');  // comm may contain spaces
        if (!p) continue;
        char state;
        int ppid = 0;
        if (std::sscanf(p + 1, " %c %d", &state, &ppid) == 2 && ppid == me) ++n;
    }
    closedir(d);
    return n;
}

bool readable(int fd) {
    pollfd p {fd, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

void testIdle() {
    CameraService svc;
    CHECK(svc.eventFd() < 0);  // not started yet
    CHECK(svc.state(0) == CameraState::Off);
    CHECK(svc.stateRevision() >= 1);

    const int childrenBefore = childProcesses();
    CHECK(svc.start());
    CHECK(svc.start());  // once: a second call is a no-op
    CHECK(svc.eventFd() >= 0);
    CHECK(svc.wakeFd_ >= 0 && svc.wakeFd_ != svc.eventFd());

    // let the thread run its startup; with nothing wanted it must stay idle
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(childProcesses() == childrenBefore);  // no helper spawned
    CHECK(!readable(svc.eventFd()));            // and no event raised by merely starting

    for (uint8_t c = 0; c < kCameraCount; ++c) {
        CameraImage img;
        CameraJpeg jpg;
        CHECK(!svc.takeImage(c, img));
        CHECK(!svc.takeJpeg(c, jpg));
        CHECK(svc.state(c) == CameraState::Off);
    }
    CameraImage img;
    CameraJpeg jpg;
    CHECK(!svc.takeImage(6, img) && !svc.takeImage(255, img));
    CHECK(!svc.takeJpeg(6, jpg) && !svc.takeJpeg(255, jpg));
    CHECK(svc.state(6) == CameraState::Off && svc.state(255) == CameraState::Off);
    CHECK(svc.debugText().find("Cameras") != std::string::npos);

    // setting the same (empty) wants repeatedly is cheap and starts nothing
    CameraWants none;
    CHECK(none.any() == 0);
    svc.setWants(none);
    svc.setWants(none);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(childProcesses() == childrenBefore);

    const double t0 = nowSec();
    svc.stop();
    CHECK(nowSec() - t0 < 1.0);
    svc.stop();  // idempotent
    CHECK(childProcesses() == childrenBefore);
}

void testStopPromptlyWithoutSleepBound() {
    // stop() right after start(), before the thread has necessarily reached its poll
    for (int i = 0; i < 20; ++i) {
        CameraService svc;
        CHECK(svc.start());
        const double t0 = nowSec();
        svc.stop();
        CHECK(nowSec() - t0 < 1.0);
    }
    // destructor stops it too
    const double t1 = nowSec();
    {
        CameraService svc;
        CHECK(svc.start());
    }
    CHECK(nowSec() - t1 < 1.0);
    // never started: destructor and stop are harmless
    {
        CameraService svc;
        svc.stop();
    }
}

void testRestart() {
    CameraService svc;
    CHECK(svc.start());
    svc.stop();
    CHECK(svc.start());  // after stop the service can start again
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const double t0 = nowSec();
    svc.stop();
    CHECK(nowSec() - t0 < 1.0);
}

void testMailbox() {
    // start() makes the eventfds; stopping the thread leaves them, and nothing else touches the mailbox or the
    // states while this test plays the camera thread (the running thread resets its states to Off by itself)
    CameraService svc;
    CHECK(svc.start());
    svc.stop();
    const int fd = svc.eventFd();
    CHECK(fd >= 0);
    CHECK(!readable(fd));

    // an image: signals, is taken once, content intact
    CameraImage a;
    a.width = 4;
    a.height = 2;
    a.rgba.assign(4 * 2 * 4, 0x55);
    a.frameId = 11;
    svc.post(2, &a, nullptr);
    CHECK(readable(fd));
    CameraImage got;
    CHECK(!svc.takeImage(1, got));  // another camera's box is empty
    CameraJpeg none;
    CHECK(!svc.takeJpeg(2, none));  // no jpeg was posted for it
    CHECK(svc.takeImage(2, got));
    CHECK(got.width == 4 && got.height == 2 && got.frameId == 11 && got.rgba.size() == 32 && got.rgba[5] == 0x55);
    CHECK(!svc.takeImage(2, got));  // taken
    svc.clearEvent();
    CHECK(!readable(fd));

    // latest wins: two posts before a take leave only the second
    CameraImage b1, b2;
    b1.width = 1;
    b1.height = 1;
    b1.rgba = {1, 1, 1, 255};
    b1.frameId = 1;
    b2.width = 1;
    b2.height = 1;
    b2.rgba = {2, 2, 2, 255};
    b2.frameId = 2;
    svc.post(0, &b1, nullptr);
    svc.post(0, &b2, nullptr);
    CHECK(svc.takeImage(0, got));
    CHECK(got.frameId == 2 && got.rgba[0] == 2);
    CHECK(!svc.takeImage(0, got));

    // jpeg and image boxes are independent, per camera
    auto makeJpeg = [](uint32_t id) {
        CameraJpeg j;
        j.width = 528;
        j.height = 512;
        j.frameId = id;
        j.data = {0xFF, 0xD8, 0xFF, 0xD9};
        return j;
    };
    CameraJpeg j3 = makeJpeg(5), j4 = makeJpeg(6);
    CameraImage c;
    c.width = 2;
    c.height = 2;
    c.rgba.assign(16, 9);
    c.frameId = 6;
    svc.post(3, &c, &j3);  // both at once
    svc.post(4, nullptr, &j4);
    CameraJpeg gotJ;
    CHECK(!svc.takeImage(4, got));
    CHECK(svc.takeJpeg(4, gotJ));
    CHECK(gotJ.width == 528 && gotJ.height == 512 && gotJ.frameId == 6 && gotJ.data.size() == 4);
    CHECK(!svc.takeJpeg(4, gotJ));
    CHECK(svc.takeImage(3, got) && got.frameId == 6);
    CHECK(svc.takeJpeg(3, gotJ) && gotJ.frameId == 5 && gotJ.data.size() == 4);

    // bad posts: out of range camera, nothing at all
    CameraImage d;
    d.rgba.assign(4, 1);
    svc.post(6, &d, nullptr);
    svc.post(200, &d, nullptr);
    svc.post(1, nullptr, nullptr);
    for (uint8_t k = 0; k < kCameraCount; ++k) {
        CHECK(!svc.takeImage(k, got));
        CHECK(!svc.takeJpeg(k, gotJ));
    }

    // state: a change bumps the revision and signals; the same state again doesn't
    svc.clearEvent();
    CHECK(!readable(fd));
    const uint64_t rev = svc.stateRevision();
    svc.setState(1, CameraState::NotWorn);
    CHECK(svc.state(1) == CameraState::NotWorn);
    CHECK(svc.stateRevision() == rev + 1);
    CHECK(readable(fd));
    svc.clearEvent();
    svc.setState(1, CameraState::NotWorn);
    CHECK(svc.stateRevision() == rev + 1 && !readable(fd));
    svc.setState(1, CameraState::Live);
    CHECK(svc.stateRevision() == rev + 2);
    svc.setState(9, CameraState::Live);  // out of range: ignored
    CHECK(svc.stateRevision() == rev + 2);
    CHECK(svc.state(0) == CameraState::Off);
}

void testConcurrentPostTake() {
    // one thread posting as fast as it can, the main thread taking: every frame taken is a whole one (a monotonic
    // frame id and a matching payload), never a torn mix
    CameraService svc;
    CHECK(svc.start());
    svc.stop();
    std::atomic<bool> go {true};
    std::thread poster([&] {
        uint32_t id = 0;
        while (go.load()) {
            CameraImage img;
            img.width = 8;
            img.height = 8;
            img.frameId = ++id;
            img.rgba.assign(8 * 8 * 4, static_cast<uint8_t>(id));
            svc.post(0, &img, nullptr);
        }
    });
    uint32_t last = 0;
    int taken = 0;
    bool torn = false, backwards = false;
    const double end = nowSec() + 0.3;
    CameraImage img;
    while (nowSec() < end) {
        if (svc.takeImage(0, img)) {
            ++taken;
            if (img.frameId <= last) backwards = true;
            last = img.frameId;
            for (uint8_t v : img.rgba)
                if (v != static_cast<uint8_t>(img.frameId)) torn = true;
        }
    }
    go.store(false);
    poster.join();
    CHECK(taken > 10);
    CHECK(!torn);
    CHECK(!backwards);
}

}  // namespace

int main() {
    testIdle();
    testStopPromptlyWithoutSleepBound();
    testRestart();
    testMailbox();
    testConcurrentPostTake();
    if (failures == 0) std::printf("camera_service_mailbox_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
