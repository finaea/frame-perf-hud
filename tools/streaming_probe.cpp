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
namespace {
using fph::pc::streaming::Anchors;
std::vector<uint8_t> load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
void printAnchors(const Anchors& a) {
    std::printf("build=%u image=%x global=%x server_vtables=%zu data=%x+%x map=%x aftm=%x link_ping=%x uber_ping=%x "
                "stride=%x file=%x remote_getter=%x remote_address=%x layout=%s server=%s layout_how=%s L1=%d L2=%d "
                "L3=%d%s%s\n",
                a.timestamp, a.imageSize, a.serverGlobal, a.serverVtables.size(), a.dataRva, a.dataSize, a.mapOffset,
                a.aftmOffset, a.linkPingOffset, a.uberPingOffset, a.stride, a.fileOffset, a.remoteGetterRva,
                a.remoteAddressOffset, a.layout ? a.layout->name : "unknown", a.serverHow.c_str(),
                a.layoutHow.c_str(), a.l1, a.l2, a.l3, a.l3Note.empty() ? "" : " l3_note=", a.l3Note.c_str());
}
// The derived layout against the known layout with the same header. Returns true on an exact match.
bool printDerived(const Anchors& d) {
    if (!d.derived) {
        std::printf("derived: failed (%s)\n", d.l3Note.c_str());
        return false;
    }
    const auto& l = d.derived->layout;
    std::printf("derived: stride=%x file=%x ping=%x stamps=", l.stride, l.fileOffset, l.pingOffset);
    for (auto s : l.stamps)
        std::printf("%x ", s);
    for (const auto& known : fph::pc::streaming::kFrameLayouts)
        if (d.derived->header == known.header) {
            const bool same = l.stride == known.stride && l.fileOffset == known.fileOffset &&
                              l.pingOffset == known.pingOffset && l.stamps == known.stamps &&
                              d.derived->row == known.row;
            std::printf("vs %s: %s\n", known.name, same ? "exact match" : "DIFFERENT");
            return same;
        }
    std::printf("vs known: new header\n");
    return true;
}
} // namespace
int main(int argc, char** argv) {
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--anchors") == 0) {
        const auto dll = load(std::filesystem::u8path(argv[2]));
        const bool derive = argc == 4 && std::strcmp(argv[3], "--derive") == 0;
        if (argc == 4 && !derive)
            return 2;
        const auto a = fph::pc::streaming::findAnchors(dll, derive);
        printAnchors(a);
        if (derive && !printDerived(a))
            return 1;
        return a.l1 && a.l2 && a.l3 ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--anchors-all") == 0) {
        std::error_code error;
        int failed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(argv[2]), error)) {
            if (entry.path().extension() != ".dll")
                continue;
            const auto dll = load(entry.path());
            std::printf("%s: ", entry.path().filename().u8string().c_str());
            const auto a = fph::pc::streaming::findAnchors(dll);
            printAnchors(a);
            std::printf("  ");
            const bool same = printDerived(fph::pc::streaming::findAnchors(dll, true));
            failed += !(a.l1 && a.l2 && a.l3 && same);
        }
        if (error) {
            std::printf("cannot list %s\n", argv[2]);
            return 2;
        }
        return failed ? 1 : 0;
    }
    int seconds = 15;
    fph::pc::streaming::ServiceProbe probe;
    for (int j = 1; j < argc; ++j) {
        if (std::strcmp(argv[j], "--force-header-mismatch") == 0)
            probe.headerMismatch = true;
        else if (std::strcmp(argv[j], "--force-derive") == 0)
            probe.derive = true;
        else if (std::strcmp(argv[j], "--force-rtti-scan") == 0)
            probe.noCodePair = true;
        else if (std::strcmp(argv[j], "--seconds") == 0 && j + 1 < argc)
            seconds = std::atoi(argv[++j]);
        else
            return 2;
    }
    fph::MetricStore store;
    fph::pc::streaming::Service reader(probe);
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
