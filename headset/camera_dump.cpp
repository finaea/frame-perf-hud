// T10: see camera_dump.h
#include "camera_dump.h"

#include "camera_service.h"

#include <poll.h>

#include <chrono>
#include <cstdio>
#include <ctime>

namespace {

double nowSec() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

double processCpu() {
    timespec ts {};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

bool writePgm(const std::string& path, const CameraImage& image) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%d %d\n255\n", image.width, image.height);
    for (size_t i = 0; i < size_t(image.width) * image.height; ++i) std::fputc(image.rgba[4 * i], f);
    return std::fclose(f) == 0;
}

}  // namespace

int runCameraDump(const CameraDumpOptions& options, const volatile std::sig_atomic_t& stop) {
    CameraService service;
    if (!service.start()) {
        std::fprintf(stderr, "can't start the camera thread\n");
        return 1;
    }
    CameraWants wants;
    wants.display = options.display ? options.cameras : 0;
    wants.pc = options.pc ? options.cameras : 0;
    wants.fullSize = options.fullSize;
    wants.worn = options.worn;  // the agent takes this from the HMD activity level; the dump doesn't read OpenVR
    service.setWants(wants);
    std::printf("camera dump: cameras 0x%02x, display %d, pc %d, full size %d, %.0f s\n", options.cameras,
                int(options.display), int(options.pc), int(options.fullSize), options.seconds);

    const double t0 = nowSec();
    const double cpu0 = processCpu();
    double nextPrint = t0 + 2.0;
    uint64_t revision = 0;
    unsigned images[fph::cam::kCameraCount] = {}, jpegs[fph::cam::kCameraCount] = {};
    size_t jpegBytes[fph::cam::kCameraCount] = {};
    bool saved[fph::cam::kCameraCount] = {};
    while (!stop && nowSec() - t0 < options.seconds) {
        pollfd p {service.eventFd(), POLLIN, 0};
        ::poll(&p, 1, 200);
        service.clearEvent();
        for (uint8_t c = 0; c < fph::cam::kCameraCount; ++c) {
            CameraImage image;
            if (service.takeImage(c, image)) {
                ++images[c];
                if (!saved[c] && !options.snapshotDir.empty()) {
                    const std::string path = options.snapshotDir + "/" + fph::cam::cameraName(c) + ".pgm";
                    saved[c] = writePgm(path, image);
                    std::printf("[%6.2f] %s: first frame %dx%d -> %s\n", nowSec() - t0, fph::cam::cameraName(c),
                                image.width, image.height, saved[c] ? path.c_str() : "(write failed)");
                }
            }
            CameraJpeg jpeg;
            if (service.takeJpeg(c, jpeg)) {
                ++jpegs[c];
                jpegBytes[c] += jpeg.data.size();
            }
        }
        if (service.stateRevision() != revision) {
            revision = service.stateRevision();
            std::printf("[%6.2f] states:", nowSec() - t0);
            for (uint8_t c = 0; c < fph::cam::kCameraCount; ++c)
                if (options.cameras & (1u << c))
                    std::printf(" %s=%d", fph::cam::cameraName(c), int(service.state(c)));
            std::printf("\n");
        }
        if (nowSec() >= nextPrint) {
            const double span = nowSec() - (nextPrint - 2.0);
            nextPrint += 2.0;
            std::printf("[%6.2f] frames/s:", nowSec() - t0);
            for (uint8_t c = 0; c < fph::cam::kCameraCount; ++c) {
                if (!(options.cameras & (1u << c))) continue;
                std::printf(" %s %.1f", fph::cam::cameraName(c), images[c] / span);
                if (options.pc) std::printf(" (jpeg %.1f/s, %.1f kB avg)", jpegs[c] / span,
                                            jpegs[c] ? jpegBytes[c] / 1024.0 / jpegs[c] : 0.0);
                images[c] = jpegs[c] = 0;
                jpegBytes[c] = 0;
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    const double wall = nowSec() - t0;
    std::printf("%s", service.debugText().c_str());
    std::printf("process CPU %.2f%% of one core over %.1f s\n", 100.0 * (processCpu() - cpu0) / wall, wall);
    service.stop();
    return 0;
}
