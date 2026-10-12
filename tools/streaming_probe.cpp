// Read-only live verification of the production reader, with a local forced-header gate option.
#include "stream_metrics.h"
#include "streaming/service.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--anchors") == 0) {
        std::ifstream in(std::filesystem::u8path(argv[2]), std::ios::binary);
        std::vector<uint8_t> dll((std::istreambuf_iterator<char>(in)), {});
        const auto a = fph::pc::streaming::findAnchors(dll);
        std::printf("build=%u image=%x global=%x map=%x aftm=%x link_ping=%x uber_ping=%x stride=%x file=%x "
                    "remote_getter=%x remote_address=%x L1=%d L2=%d L3=%d\n",
                    a.timestamp, a.imageSize, a.serverGlobal, a.mapOffset, a.aftmOffset, a.linkPingOffset,
                    a.uberPingOffset, a.stride, a.fileOffset, a.remoteGetterRva, a.remoteAddressOffset, a.l1, a.l2,
                    a.l3);
        return a.l1 && a.l2 && a.l3 ? 0 : 1;
    }
    int seconds = 15;
    bool mismatch = false;
    for (int j = 1; j < argc; ++j) {
        if (std::strcmp(argv[j], "--force-header-mismatch") == 0)
            mismatch = true;
        else if (std::strcmp(argv[j], "--seconds") == 0 && j + 1 < argc)
            seconds = std::atoi(argv[++j]);
        else
            return 2;
    }
    fph::MetricStore store;
    fph::pc::streaming::Service reader(mismatch);
    reader.setDemand(true);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (reader.take(store, now)) {
            const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            std::printf("SNAPSHOT unix_ms=%lld monotonic=%.3f\n", static_cast<long long>(unixMs), now);
            for (const auto& v : fph::pcStreamValues(store)) {
                const auto* d = fph::findMetric(v.id);
                std::printf("%u %s = ", v.id, d->name);
                if (!v.ok)
                    std::printf("N/A (%s)", fph::reasonText(v.reason));
                else if (v.type == fph::WireType::F32)
                    std::printf("%.3f", v.f32);
                else if (v.type == fph::WireType::I32)
                    std::printf("%d", v.i32);
                else
                    std::printf("%s", v.text.c_str());
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    reader.stop();
    return 0;
}
