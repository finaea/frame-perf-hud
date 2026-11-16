// pc/streaming/reader: parseFrame (field offsets, the 0-500ms bounds gate, u32 wrap), FrameWindow (144-tail
// dedupe by Present ID, an incomplete record picked up on a later tick, the 1 s received-time window so a
// stall reads as "no frames" rather than a failure), and Reader::tick against a fake Memory + a directly
// constructed Anchors: the golden path, a header-mismatch Anchors (only L3 unavailable), a runtime RTTI
// mismatch on the live Uber object (only L2 unavailable), and an owner-pointer change (a new SVLServer:
// resets() counts it, and the layer waits for the new baseline to actually advance before publishing again,
// not just for its index to differ from the old, unrelated one).
//
// findAnchors() itself (the byte-pattern / disassembly search) is exercised against the real DLL by the
// implementer; the native anchors values are known-good, so these tests build Anchors by hand instead of
// reconstructing a synthetic PE file.
//
// A custom store includes every output touched by the fixture; fail-closed ranges also use the compiled
// global registry, so the layer failures assert the shipped reason codes as well as missing values.
#include "metric_registry.h"
#include "metric_store.h"
#include "reason.h"
#include "stream_metrics.h"
#include "streaming/reader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

using namespace fph;
using namespace fph::pc::streaming;

// ==================== parseFrame ====================

std::vector<uint8_t> frameBytes(const uint32_t (&t)[13], int32_t pingUs) {
    std::vector<uint8_t> raw(kFrameStride, 0);
    std::memcpy(raw.data(), t, sizeof(t));
    std::memcpy(raw.data() + 0x38, &pingUs, sizeof(pingUs));
    return raw;
}

/** A self-consistent, complete frame: each stage a plausible small delay, ping 2 ms. */
void baseFields(uint32_t (&t)[13], uint32_t wgpRelease = 1000, uint32_t id = 1) {
    t[0] = id;
    t[1] = wgpRelease;    // WGP Release
    t[2] = t[1] + 1000;   // App Submit            (game = 1.0 ms)
    t[3] = t[2] + 300;    // Encode Start
    t[4] = t[3] + 2600;   // Encode End             (encode = 2.6 ms)
    t[5] = t[4] + 170;    // Send Start
    t[6] = t[5] + 180;    // Send End               (send = 0.18 ms)
    t[7] = t[6] + 6000;   // Receive                (network = 6.0 ms)
    t[8] = t[7] + 70;     // Decode Start
    t[9] = t[8] + 7000;   // Decode End             (decode, incl. queue = 7.07 ms)
    t[10] = t[9] + 36000; // Submit Illumination    (slack = 36.0 ms)
    t[11] = t[10] + 500;  // Predicted Display (unused by parseFrame)
    t[12] = 0;            // Decode Query (unused)
}

void testParseFrameFieldMapping() {
    uint32_t t[13] = {};
    baseFields(t);
    const auto raw = frameBytes(t, 2470);
    Frame f;
    CHECK(parseFrame(raw.data(), f));
    CHECK(f.id == t[0] && f.illum == t[10]);
    // ms[] pairs exactly as the reader defines them: game, encode, send, network, decode, slack, total
    const unsigned pairs[7][2] = {{1, 2}, {3, 4}, {5, 6}, {6, 7}, {7, 9}, {9, 10}, {1, 10}};
    for (unsigned j = 0; j < 7; ++j) {
        const double expected = (t[pairs[j][1]] - t[pairs[j][0]]) / 1000.0;
        CHECK(std::fabs(f.ms[j] - expected) < 1e-9);
    }
    CHECK(std::fabs(f.ping - 2.47) < 1e-9);
}

void testParseFrameIncomplete() {
    // zeroing any of illum (10), decode end (9), encode end (4) or receive (7) marks the record "not done
    // yet" (its frame completes ~50 ms after capture): parseFrame must refuse it, not synthesize a partial
    for (int idx : {10, 9, 4, 7}) {
        uint32_t t[13] = {};
        baseFields(t);
        t[idx] = 0;
        Frame f;
        CHECK(!parseFrame(frameBytes(t, 2000).data(), f));
    }
    // the untouched base record is in fact valid (so the negative cases above are testing the right thing)
    uint32_t t[13] = {};
    baseFields(t);
    Frame f;
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
}

void testParseFrameBounds() {
    // a genuinely too-long stage (600 ms decode, no wraparound involved) is rejected
    {
        uint32_t t[13] = {};
        baseFields(t);
        t[9] = t[8] + 600000;
        t[10] = t[9] + 1000;
        t[11] = t[10] + 500;
        Frame f;
        CHECK(!parseFrame(frameBytes(t, 2000).data(), f));
    }
    // ping out of its 0-500 ms bound, both directions
    {
        uint32_t t[13] = {};
        baseFields(t);
        Frame f;
        CHECK(!parseFrame(frameBytes(t, -1).data(), f));
        CHECK(!parseFrame(frameBytes(t, 600000).data(), f));
        CHECK(parseFrame(frameBytes(t, 500000).data(), f)); // the bound itself is inclusive
    }
}

void testParseFrameU32Wrap() {
    // WGP Release just below the u32 wraparound; every later timestamp wraps past it. The true elapsed time
    // between stages is unchanged (1000 us of "game", 53320 us total) -- only the raw counters wrap.
    uint32_t t[13] = {};
    baseFields(t, /*wgpRelease=*/0xFFFFFF9Cu);
    CHECK(t[2] < t[1]); // confirms the fixture actually wrapped (App Submit reads numerically smaller)

    Frame f;
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(std::fabs(f.ms[0] - 1.0) < 1e-9);   // game: still 1.0 ms despite the wrap
    CHECK(std::fabs(f.ms[6] - 53.32) < 1e-6); // total: WGP Release -> Submit Illumination, also wrapped
}

void testParseFrameHoldDisplay() {
    // Decode Query 4 ms after Decode End: slack (36 ms) splits into hold 4 ms + display 32 ms
    uint32_t t[13] = {};
    baseFields(t);
    t[12] = t[9] + 4000;
    Frame f;
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(f.split);
    CHECK(std::fabs(f.ms[7] - 4.0) < 1e-9);
    CHECK(std::fabs(f.ms[8] - 32.0) < 1e-9);
    CHECK(std::fabs(f.ms[7] + f.ms[8] - f.ms[5]) < 1e-9); // hold + display == slack
    CHECK(std::fabs(f.ms[6] - 53.32) < 1e-6);             // the other stages are untouched

    // Decode Query == Decode End: hold is exactly 0, still a valid split
    t[12] = t[9];
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(f.split && f.ms[7] == 0.0 && std::fabs(f.ms[8] - 36.0) < 1e-9);
}

void testParseFrameNoDecodeQuery() {
    // Decode Query 0 (startup records): the frame is kept, only the split is dropped
    uint32_t t[13] = {};
    baseFields(t);
    t[12] = 0;
    Frame f;
    f.ms[7] = f.ms[8] = 99; // stale values must not survive
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(!f.split);
    CHECK(f.ms[7] == 0.0 && f.ms[8] == 0.0);
    CHECK(std::fabs(f.ms[5] - 36.0) < 1e-9);
    CHECK(std::fabs(f.ms[6] - 53.32) < 1e-6);
    CHECK(std::fabs(f.ping - 2.0) < 1e-9);

    // a Decode Query nowhere near this frame (500 ms+ either side) is also "no split", not a rejected frame
    for (uint32_t off : {600000u, static_cast<uint32_t>(-600000)}) {
        baseFields(t);
        t[12] = t[9] + off;
        Frame g;
        CHECK(parseFrame(frameBytes(t, 2000).data(), g));
        CHECK(!g.split);
        CHECK(std::fabs(g.ms[5] - 36.0) < 1e-9);
    }
}

void testParseFrameNegativeHold() {
    // Decode Query 3 ms BEFORE Decode End (the frame missed its slot): hold is signed, display absorbs it
    uint32_t t[13] = {};
    baseFields(t);
    t[12] = t[9] - 3000;
    Frame f;
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(f.split);
    CHECK(std::fabs(f.ms[7] - (-3.0)) < 1e-9);
    CHECK(std::fabs(f.ms[8] - 39.0) < 1e-9);
    CHECK(std::fabs(f.ms[7] + f.ms[8] - f.ms[5]) < 1e-9);
}

void testParseFrameHoldDisplayU32Wrap() {
    // t[9] just under the wrap, t[12] and t[10] past it: the deltas must come out as true elapsed time
    uint32_t t[13] = {};
    baseFields(t, /*wgpRelease=*/0xFFFFFE00u - 17320u); // Decode End lands at 0xFFFFFE00
    t[12] = t[9] + 4000;                                // wrapped
    CHECK(t[9] == 0xFFFFFE00u && t[12] < t[9] && t[10] < t[9]);
    Frame f;
    CHECK(parseFrame(frameBytes(t, 2000).data(), f));
    CHECK(f.split);
    CHECK(std::fabs(f.ms[7] - 4.0) < 1e-9);
    CHECK(std::fabs(f.ms[8] - 32.0) < 1e-9);

    // negative hold across the wrap: t[12] numerically BIG, t[9] small
    uint32_t u[13] = {};
    baseFields(u, 1000);
    u[9] = 2000;
    u[8] = u[9] - 100;
    u[7] = u[8] - 50;
    u[6] = u[7] - 1000;
    u[5] = u[6] - 100;
    u[4] = u[5] - 100;
    u[3] = u[4] - 100;
    u[2] = u[3] - 100;
    u[1] = u[2] - 100;
    u[10] = u[9] + 36000;
    u[12] = u[9] - 3000; // wraps below 0
    CHECK(u[12] > u[9]);
    Frame g;
    CHECK(parseFrame(frameBytes(u, 2000).data(), g));
    CHECK(g.split);
    CHECK(std::fabs(g.ms[7] - (-3.0)) < 1e-9);
    CHECK(std::fabs(g.ms[8] - 39.0) < 1e-9);
}

// ==================== parseFrame: gpu-stamps layout ====================

/** gpu-stamps record with the offsets written out as literals here, deliberately NOT read from
 *  kFrameLayouts[1], so a wrong table entry makes parseFrame disagree with this fixture instead of
 *  agreeing with itself. Stride 0x98, ping 0x50. The two GPU-done stamps (0x1c, 0x20) and the problem flags
 *  (0x90) get nonzero junk: parseFrame must ignore them. */
std::vector<uint8_t> gpuFrameBytes(const uint32_t (&t)[13], int32_t pingUs) {
    static const uint32_t offsets[12] = {0x14, 0x18, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48};
    std::vector<uint8_t> raw(0x98, 0);
    std::memcpy(raw.data(), &t[0], sizeof(t[0]));
    for (size_t j = 0; j < 12; ++j)
        std::memcpy(raw.data() + offsets[j], &t[j + 1], sizeof(t[j + 1]));
    std::memcpy(raw.data() + 0x50, &pingUs, sizeof(pingUs));
    const uint32_t junk = 0x12345678u;
    std::memcpy(raw.data() + 0x1c, &junk, sizeof(junk));
    std::memcpy(raw.data() + 0x20, &junk, sizeof(junk));
    std::memcpy(raw.data() + 0x90, &junk, sizeof(junk));
    return raw;
}

/** Record bytes for either known layout, built from literal offsets (frameBytes() for legacy, gpuFrameBytes()
 *  for gpu-stamps), never from the FrameLayout table under test. */
std::vector<uint8_t> frameBytesForLayout(const FrameLayout& layout, const uint32_t (&t)[13], int32_t pingUs) {
    return &layout == &kFrameLayouts[1] ? gpuFrameBytes(t, pingUs) : frameBytes(t, pingUs);
}

/** The table itself, checked against the known values (independent of any fixture). */
void testFrameLayoutTableValues() {
    const FrameLayout& legacy = kFrameLayouts[0];
    CHECK(legacy.stride == 0x78 && legacy.fileOffset == 0x88030 && legacy.indexOffset == 0x88000);
    CHECK(legacy.pingOffset == 0x38);
    const uint32_t legacyStamps[12] = {0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30};
    for (size_t j = 0; j < 12; ++j)
        CHECK(legacy.stamps[j] == legacyStamps[j]);

    const FrameLayout& gpu = kFrameLayouts[1];
    CHECK(gpu.stride == 0x98 && gpu.fileOffset == 0xa8080 && gpu.indexOffset == 0xa8050);
    CHECK(gpu.pingOffset == 0x50);
    const uint32_t gpuStamps[12] = {0x14, 0x18, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48};
    for (size_t j = 0; j < 12; ++j)
        CHECK(gpu.stamps[j] == gpuStamps[j]);
    // 0x1c/0x20 (GPU-done stamps) and 0x90 (flags) are intentionally not mapped
    for (size_t j = 0; j < 12; ++j)
        CHECK(gpu.stamps[j] != 0x1c && gpu.stamps[j] != 0x20 && gpu.stamps[j] != 0x90);
}

void testParseFrameFieldMappingGpuStamps() {
    const auto& layout = kFrameLayouts[1];
    uint32_t t[13] = {};
    baseFields(t);
    const auto raw = frameBytesForLayout(layout, t, 2470);
    Frame f;
    CHECK(parseFrame(raw.data(), f, layout));
    CHECK(f.id == t[0] && f.illum == t[10]);
    const unsigned pairs[7][2] = {{1, 2}, {3, 4}, {5, 6}, {6, 7}, {7, 9}, {9, 10}, {1, 10}};
    for (unsigned j = 0; j < 7; ++j) {
        const double expected = (t[pairs[j][1]] - t[pairs[j][0]]) / 1000.0;
        CHECK(std::fabs(f.ms[j] - expected) < 1e-9);
    }
    CHECK(std::fabs(f.ping - 2.47) < 1e-9);
}

void testParseFrameIncompleteGpuStamps() {
    const auto& layout = kFrameLayouts[1];
    // same "not done yet" fields as the legacy case, just at the gpu-stamps byte offsets
    for (int idx : {10, 9, 4, 7}) {
        uint32_t t[13] = {};
        baseFields(t);
        t[idx] = 0;
        Frame f;
        CHECK(!parseFrame(frameBytesForLayout(layout, t, 2000).data(), f, layout));
    }
    uint32_t t[13] = {};
    baseFields(t);
    Frame f;
    CHECK(parseFrame(frameBytesForLayout(layout, t, 2000).data(), f, layout));
}

void testParseFrameBoundsGpuStamps() {
    const auto& layout = kFrameLayouts[1];
    {
        uint32_t t[13] = {};
        baseFields(t);
        t[9] = t[8] + 600000;
        t[10] = t[9] + 1000;
        t[11] = t[10] + 500;
        Frame f;
        CHECK(!parseFrame(frameBytesForLayout(layout, t, 2000).data(), f, layout));
    }
    {
        uint32_t t[13] = {};
        baseFields(t);
        Frame f;
        CHECK(!parseFrame(frameBytesForLayout(layout, t, -1).data(), f, layout));
        CHECK(!parseFrame(frameBytesForLayout(layout, t, 600000).data(), f, layout));
        CHECK(parseFrame(frameBytesForLayout(layout, t, 500000).data(), f, layout)); // the bound itself is inclusive
    }
}

void testParseFrameU32WrapGpuStamps() {
    const auto& layout = kFrameLayouts[1];
    uint32_t t[13] = {};
    baseFields(t, /*wgpRelease=*/0xFFFFFF9Cu);
    CHECK(t[2] < t[1]); // confirms the fixture actually wrapped

    Frame f;
    CHECK(parseFrame(frameBytesForLayout(layout, t, 2000).data(), f, layout));
    CHECK(std::fabs(f.ms[0] - 1.0) < 1e-9);
    CHECK(std::fabs(f.ms[6] - 53.32) < 1e-6);
}

// ==================== FrameWindow ====================

/** A full 144-slot tail: slots [0, validCount) are complete, distinct, closely-spaced records; the rest
 *  (if validCount < kTailFrames) stay all-zero, i.e. "not done yet". `base` offsets every WGP Release (and so
 *  every derived illumination timestamp) by a fixed amount, so a caller can place the whole tail straddling
 *  the u32 wraparound. */
std::vector<uint8_t> tail144(uint32_t startId, int validCount, uint32_t stepUs, uint32_t base = 0) {
    std::vector<uint8_t> raw(kTailFrames * kFrameStride, 0);
    for (int i = 0; i < validCount && i < static_cast<int>(kTailFrames); ++i) {
        uint32_t t[13] = {};
        baseFields(t, base + static_cast<uint32_t>(i) * stepUs, startId + static_cast<uint32_t>(i));
        std::memcpy(raw.data() + static_cast<size_t>(i) * kFrameStride, t, sizeof(t));
        const int32_t ping = 2000;
        std::memcpy(raw.data() + static_cast<size_t>(i) * kFrameStride + 0x38, &ping, sizeof(ping));
    }
    return raw;
}

/** Zeros one field (by its parseFrame index, e.g. 10 = Submit Illumination) of one slot in an existing tail. */
void clearField(std::vector<uint8_t>& tail, int slot, int fieldIndex) {
    const uint32_t zero = 0;
    std::memcpy(tail.data() + static_cast<size_t>(slot) * kFrameStride + static_cast<size_t>(fieldIndex) * 4, &zero,
                sizeof(zero));
}

void testFrameWindowDedupeAndLateCompletion() {
    FrameWindow w;
    CHECK(w.published() == 0);

    // 143 of 144 slots complete; the last one hasn't gotten its illumination timestamp yet
    const auto tail = tail144(100, 143, 1000);
    w.add(tail, 0.0);
    CHECK(w.published() == 143);
    CHECK(w.recent(0.0).size() == 143);

    // re-reading the identical tail (the ring hasn't moved): no new publishes, Present IDs already seen
    w.add(tail, 0.1);
    CHECK(w.published() == 143);

    // the last slot completes on a later re-read: picked up now, exactly once
    const auto tail2 = tail144(100, 144, 1000);
    w.add(tail2, 0.2);
    CHECK(w.published() == 144);
    CHECK(w.recent(0.2).size() == 144);
    w.add(tail2, 0.3);
    CHECK(w.published() == 144); // still just the one new id; no double count
}

void testFrameWindowWrongSizeIgnored() {
    FrameWindow w;
    std::vector<uint8_t> tooShort(kFrameStride, 0);
    w.add(tooShort, 0.0);
    CHECK(w.published() == 0);
    CHECK(w.recent(0.0).empty());
}

void testFrameWindowStallReadsAsNoFrames() {
    FrameWindow w;
    const auto tail = tail144(500, 144, 1000);
    w.add(tail, 0.0);
    CHECK(w.recent(0.0).size() == 144);

    // the ring hasn't advanced, but it's well under 1 s since these were received: still "recent"
    w.add(tail, 0.5);
    CHECK(w.published() == 144);
    CHECK(w.recent(0.5).size() == 144);

    // past 1 s since receipt, with nothing new arriving: the window empties -- a stall, not a failure.
    // published() keeps the historical total; it isn't "unpublished".
    w.add(tail, 1.1);
    CHECK(w.recent(1.1).empty());
    CHECK(w.published() == 144);
}

/** T08 review finding: an interior record (not the last slot) that finishes its illumination late must not
 *  move the "newest seen" reference backwards just because it happens to be appended last. The newest
 *  illumination has to be tracked independently of insertion order (with a wrap-aware comparison), not read
 *  off samples_.back(). Mirrors the review's own reproduction: tail144(100,144,1000) with slot 120 (id 220)
 *  cleared on the first read, completed on the second; without the fix, recent() lost every frame newer than
 *  220 (144 published, only 121 retained). */
void testFrameWindowInteriorLateCompletion() {
    FrameWindow w;
    auto firstRead = tail144(100, 144, 1000);
    clearField(firstRead, 120, 10); // id 220 isn't done yet on this read
    w.add(firstRead, 0.0);
    CHECK(w.published() == 143);
    CHECK(w.recent(0.0).size() == 143);

    // slot 120 completes on the next read. Every other id is already seen_, so it alone gets appended --
    // last by insertion order, even though its illumination sits in the *middle* of the span.
    const auto secondRead = tail144(100, 144, 1000);
    w.add(secondRead, 0.2);
    CHECK(w.published() == 144); // exactly the one new id (220), no dupes

    const auto window = w.recent(0.2);
    CHECK(window.size() == 144); // every completed frame stays in the window, not just the 121 up to id 220
    bool sawId220 = false, sawNewestId243 = false;
    for (const Frame& f : window) {
        sawId220 |= f.id == 220;
        sawNewestId243 |= f.id == 243; // slot 143: the genuinely newest frame, by far the latest illumination
    }
    CHECK(sawId220 && sawNewestId243);

    // re-adding the same complete tail again publishes nothing new and doesn't shrink the window
    w.add(secondRead, 0.3);
    CHECK(w.published() == 144);
    CHECK(w.recent(0.3).size() == 144);
}

/** Same scenario, but the whole tail straddles the u32 wraparound: slot 0's illumination is numerically huge
 *  (close to 0xFFFFFFFF) and slot 143's has wrapped around to a small number, even though slot 143 is the
 *  later frame. A naive "biggest number wins" newest-tracker would get this backwards. */
void testFrameWindowInteriorLateCompletionWraps() {
    constexpr uint32_t kWrapBase = 0xFFFFFFFFu - 70000u; // the 144 x 1000 us span straddles the wrap
    FrameWindow w;
    auto firstRead = tail144(300, 144, 1000, kWrapBase);
    clearField(firstRead, 120, 10); // id 420
    w.add(firstRead, 0.0);
    CHECK(w.published() == 143);

    const auto secondRead = tail144(300, 144, 1000, kWrapBase);
    w.add(secondRead, 0.2);
    CHECK(w.published() == 144);

    const auto window = w.recent(0.2);
    CHECK(window.size() == 144);
    bool sawNewestId443 = false;
    for (const Frame& f : window)
        sawNewestId443 |= f.id == 443; // slot 143: past the wrap, numerically small
    CHECK(sawNewestId443);
}

// ==================== Reader: fake Memory + Anchors ====================

constexpr uint64_t kModuleBase = 0x0000000010000000ULL;
constexpr uint64_t kBufSize = 0x00600000ULL; // 6 MiB: module RTTI area + two independent "server" worlds

/** A flat, sparse-enough backing store for one contiguous fake address space starting at kModuleBase. */
class FakeMemory : public fph::pc::streaming::Memory {
public:
    FakeMemory() : buf_(kBufSize, 0) {}
    bool read(uint64_t address, void* dest, size_t bytes) override {
        if (address < kModuleBase)
            return false;
        const uint64_t off = address - kModuleBase;
        if (off > buf_.size() || bytes > buf_.size() - off)
            return false;
        std::memcpy(dest, buf_.data() + off, bytes);
        return true;
    }
    template <class T> void poke(uint64_t address, T v) {
        write(address, &v, sizeof(v));
    }
    void write(uint64_t address, const void* src, size_t bytes) {
        std::memcpy(buf_.data() + (address - kModuleBase), src, bytes);
    }
    void writeCString(uint64_t address, const std::string& s) {
        write(address, s.data(), s.size() + 1);
    }

private:
    std::vector<uint8_t> buf_;
};

// ---- the fake module: PE header + one RTTI chain per type (shared by every instance of that type) ----

constexpr uint64_t kVtServer = kModuleBase + 0x1000, kColServer = kModuleBase + 0x1100, kTdServer = 0x1200;
constexpr uint64_t kVtUber = kModuleBase + 0x1300, kColUber = kModuleBase + 0x1400, kTdUber = 0x1500;
constexpr uint64_t kVtLink = kModuleBase + 0x1600, kColLink = kModuleBase + 0x1700, kTdLink = 0x1800;
constexpr uint64_t kVtBogus = kModuleBase + 0x1900, kColBogus = kModuleBase + 0x1A00, kTdBogus = 0x1B00;
constexpr uint32_t kFakeImageSize = 0x02000000; // 32 MiB: generous bound for the RTTI checks

void writePeHeader(FakeMemory& mem, uint32_t timestamp, uint32_t imageSize) {
    uint8_t dos[64] = {};
    dos[0] = 'M';
    dos[1] = 'Z';
    const uint32_t lfanew = 0x80;
    std::memcpy(dos + 0x3c, &lfanew, 4);
    mem.write(kModuleBase, dos, sizeof(dos));
    uint8_t nt[88] = {};
    nt[0] = 'P';
    nt[1] = 'E';
    nt[4] = 0x64;
    nt[5] = 0x86; // machine = AMD64
    std::memcpy(nt + 8, &timestamp, 4);
    nt[24] = 0x0b;
    nt[25] = 0x02; // magic = 0x20b (PE32+)
    std::memcpy(nt + 80, &imageSize, 4);
    mem.write(kModuleBase + lfanew, nt, sizeof(nt));
}

/** One RTTI chain (vtable -> complete object locator -> type descriptor name), MSVC x64 layout. Shared by
 *  every object of that type: real C++ objects of the same class share one vtable too. */
void writeRttiType(FakeMemory& mem, uint64_t vt, uint64_t col, uint32_t td, const std::string& name) {
    mem.poke<uint32_t>(col + 0, 1); // signature (1 = x64)
    mem.poke<uint32_t>(col + 12, td);
    mem.poke<uint64_t>(vt - 8, col);
    uint8_t zero[16] = {};
    mem.write(kModuleBase + td, zero, sizeof(zero));
    mem.writeCString(kModuleBase + td + 16, name);
}

// The remote-address getter: a tiny, fixed instruction shape at module+remoteGetterRva --
// `lea rdx,[rcx+offset]; movzx eax,word ptr[rdx]; cmp eax,2` -- with the embedded offset immediate required to
// equal anchors.remoteAddressOffset. 0x120 is the real offset this build's anchors derived.
constexpr uint32_t kRemoteGetterRva = 0x1c00;
constexpr uint32_t kRemoteAddressOffset = 0x120;

void writeRemoteGetterCode(FakeMemory& mem, uint32_t rva, uint32_t embeddedOffset) {
    uint8_t code[14] = {0x48, 0x8d, 0x91, 0, 0, 0, 0, 0x0f, 0xb7, 0x02, 0x66, 0x83, 0xf8, 0x02};
    std::memcpy(code + 3, &embeddedOffset, sizeof(embeddedOffset));
    mem.write(kModuleBase + rva, code, sizeof(code));
}

/** A sockaddr-shaped 16 bytes at link+offset: family (u16), port (u16, only its zero-ness matters), then the
 *  IPv4 octets in the order they'd print ("o1.o2.o3.o4"), matching how Windows lays out a real sockaddr_in
 *  regardless of host endianness. */
void writeRemoteAddress(FakeMemory& mem, uint64_t link, uint32_t offset, uint16_t family, uint16_t port, uint8_t o1,
                        uint8_t o2, uint8_t o3, uint8_t o4) {
    uint8_t addr[16] = {};
    std::memcpy(addr + 0, &family, sizeof(family));
    std::memcpy(addr + 2, &port, sizeof(port));
    addr[4] = o1;
    addr[5] = o2;
    addr[6] = o3;
    addr[7] = o4;
    mem.write(link + offset, addr, sizeof(addr));
}

void setupModule(FakeMemory& mem, uint32_t timestamp) {
    writePeHeader(mem, timestamp, kFakeImageSize);
    writeRttiType(mem, kVtServer, kColServer, kTdServer, ".?AVSVLServer@@");
    writeRttiType(mem, kVtUber, kColUber, kTdUber, ".?AVSVLDataLinkUber@@");
    writeRttiType(mem, kVtLink, kColLink, kTdLink, ".?AVSVLDataLinkTransferUDP@@");
    writeRttiType(mem, kVtBogus, kColBogus, kTdBogus, "bogus");
    writeRemoteGetterCode(mem, kRemoteGetterRva, kRemoteAddressOffset); // the real (unmodified) getter shape
}

void setObjectRtti(FakeMemory& mem, uint64_t obj, uint64_t vt) {
    mem.poke<uint64_t>(obj, vt);
}

constexpr uint64_t kServerGlobalSlot = 0x2000; // module+this holds a pointer to the *current* SVLServer
constexpr uint32_t kMapOffset = 0x200;         // srv+this -> the stats map's head sentinel

// ---- one SVLServer "world": map, Uber, two links, the AFTM tail and the bandwidth ring tail ----

/** @param node must have at least 0x80 scratch bytes free right after it (for keys >= 16 bytes, which can't
 *      use the inline SSO form and need a "heap" pointer instead -- e.g. "nFramesSkippedPS" is exactly 16) */
void writeMapNode(FakeMemory& mem, uint64_t node, uint64_t left, uint64_t right, const std::string& key, double value) {
    mem.poke<uint64_t>(node + 0, left);
    mem.poke<uint64_t>(node + 0x10, right);
    mem.poke<uint8_t>(node + 0x19, 0); // not a sentinel
    if (key.size() < 16) {
        uint8_t sso[16] = {};
        std::memcpy(sso, key.data(), key.size());
        mem.write(node + 0x20, sso, sizeof(sso));
        mem.poke<uint64_t>(node + 0x30, static_cast<uint64_t>(key.size()));
        mem.poke<uint64_t>(node + 0x38, static_cast<uint64_t>(15)); // SSO capacity (< 16)
    } else {
        const uint64_t heap = node + 0x80; // scratch area between this node and the next
        mem.write(heap, key.data(), key.size());
        mem.poke<uint64_t>(node + 0x20, heap);
        mem.poke<uint64_t>(node + 0x30, static_cast<uint64_t>(key.size()));
        mem.poke<uint64_t>(node + 0x38, static_cast<uint64_t>(key.size()));
    }
    mem.poke<double>(node + 0x40, value);
}

struct StatKv {
    const char* key;
    double value;
};

void writeMap(FakeMemory& mem, uint64_t srv, uint64_t head, uint64_t rootBase, const std::vector<StatKv>& kvs) {
    mem.poke<uint64_t>(srv + kMapOffset, head);
    mem.poke<uint64_t>(srv + kMapOffset + 8, static_cast<uint64_t>(kvs.size())); // MSVC std::map: {_Myhead, _Mysize}
    mem.poke<uint64_t>(head + 8, rootBase);
    // a simple right-chained list (left always the sentinel) satisfies walkMap's stack traversal fully.
    // Nodes are spaced 0x100 apart so each has 0x80 bytes of its own scratch space after it, for keys that
    // need the heap-string form (see writeMapNode).
    for (size_t i = 0; i < kvs.size(); ++i) {
        const uint64_t node = rootBase + i * 0x100;
        const uint64_t right = (i + 1 < kvs.size()) ? rootBase + (i + 1) * 0x100 : head;
        writeMapNode(mem, node, head, right, kvs[i].key, kvs[i].value);
    }
}

/** Decode Query offset after Decode End (us) written into every fixture record; 0 = no Decode Query. */
uint32_t gFixtureDecodeQueryUs = 0;

void writeAftmRecord(FakeMemory& mem, uint64_t addr, uint32_t presentId, uint32_t wgpRelease) {
    uint32_t t[13] = {};
    baseFields(t, wgpRelease, presentId);
    if (gFixtureDecodeQueryUs != 0)
        t[12] = t[9] + gFixtureDecodeQueryUs;
    mem.write(addr, t, sizeof(t));
    const int32_t ping = 2000; // matches the sPingMS below for the L3 cross-check
    mem.poke<int32_t>(addr + 0x38, ping);
}

void writeBandwidthRecord(FakeMemory& mem, uint64_t addr, uint64_t timestampUs) {
    const int32_t fields[7] = {150000, 120000, 15000, 2000, 5000, 3000, 0}; // wire,video,fec,logic,audio,retry
    mem.write(addr, fields, sizeof(fields));
    mem.poke<uint64_t>(addr + 0x40, timestampUs);
}

/** µs spacing used for both the AFTM and bandwidth tails: 144 records at this cadence span exactly 2 s, so
 *  the latest 1 s (the window Reader/FrameWindow actually publish) is exactly the last 72 of them -- the
 *  same shape as the real 72 Hz stream the spike measured. */
constexpr uint32_t kFixtureStepUs = 14000;
constexpr uint32_t kFixtureRecentCount = 72;
uint32_t gFixtureStepUs = kFixtureStepUs; // a test may set another cadence before buildWorld, then restore it

struct World {
    uint64_t srv = 0, uber = 0, link0 = 0, link1 = 0, bw = 0;
};

/**
 * Builds one complete, self-consistent SVLServer world at `base` (added to kModuleBase), with its AFTM tail
 * ending at ring index `index` (so a read of the last 144 records needs slots [index-144, index)), plus
 * `extraTail` further slots so a later test can advance the index without rewriting the world.
 */
World buildWorld(FakeMemory& mem, uint64_t base, uint32_t index, uint32_t extraTail, uint32_t idStart) {
    World w;
    w.srv = kModuleBase + base;
    const uint64_t head = kModuleBase + base + 0x90000;
    const uint64_t root = kModuleBase + base + 0x90100;
    w.uber = kModuleBase + base + 0xA0000;
    w.link0 = kModuleBase + base + 0xA1000;
    w.link1 = kModuleBase + base + 0xA2000;
    w.bw = kModuleBase + base + 0xB0000;

    setObjectRtti(mem, w.srv, kVtServer);
    setObjectRtti(mem, w.uber, kVtUber);
    setObjectRtti(mem, w.link0, kVtLink);
    setObjectRtti(mem, w.link1, kVtLink);

    mem.poke<uint64_t>(w.srv + 0x188, w.uber);
    mem.poke<uint64_t>(w.srv + 0x1f0, w.bw);
    mem.poke<uint32_t>(w.srv + 0x1e8, 8192); // bandwidth ring size: must read back exactly this
    mem.poke<uint32_t>(w.srv + 0x1ec, 144);  // write index chosen so the 144-tail starts at bw+0

    uint64_t linksArr[16] = {};
    linksArr[0] = w.link0;
    linksArr[1] = w.link1;
    mem.write(w.uber + 0x388, linksArr, sizeof(linksArr));
    mem.poke<uint32_t>(w.link0 + 0x24, 434);
    mem.poke<uint32_t>(w.link0 + 0x2c, 11700); // µs
    mem.poke<uint32_t>(w.link1 + 0x24, 625);
    mem.poke<uint32_t>(w.link1 + 0x2c, 9200); // 434 + 625 = 1059 = sRXpps below (the L2 cross-check)

    writeMap(mem, w.srv, head, root,
             {{"sPingMS", 2.0},
              {"dbgEncTimeMS", 2.6},
              {"txTimeMS", 0.18},
              {"dEstLinkBW", 232.0},
              {"rexmitReq", 4670},
              {"rexmitSent", 4013},
              {"nFramesSkippedPS", 0},
              {"dFrmsLate", 0},
              {"encResets", 3},
              {"sRXpps", 1059}});

    const uint32_t start = (index - kTailFrames) & 0xfff;
    const uint32_t count = kTailFrames + extraTail;
    for (uint32_t i = 0; i < count; ++i) {
        writeAftmRecord(mem, w.srv + static_cast<uint64_t>(start + i) * kFrameStride, idStart + i, i * gFixtureStepUs);
        writeBandwidthRecord(mem, w.bw + static_cast<uint64_t>(i) * 0xa8, i * gFixtureStepUs);
    }
    mem.poke<uint16_t>(w.srv + 0x88000, static_cast<uint16_t>(index));
    return w;
}

void setActiveServer(FakeMemory& mem, uint64_t srv) {
    mem.poke<uint64_t>(kModuleBase + kServerGlobalSlot, srv);
}

Anchors goldenAnchors(uint32_t timestamp) {
    Anchors a;
    a.timestamp = timestamp;
    a.imageSize = kFakeImageSize;
    a.serverGlobal = static_cast<uint32_t>(kServerGlobalSlot);
    a.mapOffset = kMapOffset;
    a.aftmOffset = 0;
    a.linkPingOffset = 0x2c;
    a.uberPingOffset = 0x488;
    a.stride = kFrameStride;
    a.fileOffset = 0x88030;
    a.remoteGetterRva = kRemoteGetterRva;
    a.remoteAddressOffset = kRemoteAddressOffset;
    a.l1 = a.l2 = a.l3 = true;
    return a;
}

bool notOk(const MetricStore& s, uint16_t id) {
    const MetricValue* v = s.get(id);
    return v != nullptr && !v->ok;
}
bool isOkF32(const MetricStore& s, uint16_t id, double expected, double eps) {
    const MetricValue* v = s.get(id);
    return v != nullptr && v->ok && v->type == WireType::F32 && std::fabs(v->f32 - expected) < eps;
}

/** Every output Reader::tick touches, including notes and the independent build field. */
std::vector<MetricDef> readerMetricDefs() {
    std::vector<MetricDef> defs;
    const auto add = [&](uint16_t id, WireType t) {
        defs.push_back({id, "x", "", WidgetType::Streaming, SourceId::None, 1000, t});
    };
    for (uint16_t id = 1021; id <= 1067; ++id)
        add(id, WireType::F32);
    add(metric::kPcStreamBuild, WireType::ShortString);
    add(metric::kPcStreamL1Note, WireType::ShortString);
    add(metric::kPcStreamL2Note, WireType::ShortString);
    add(metric::kPcStreamL3Note, WireType::ShortString);
    for (uint16_t id = 1090; id <= 1093; ++id)
        add(id, WireType::ShortString); // Link0..3Address
    return defs;
}

std::string noteOf(const MetricStore& s, uint16_t id) {
    const MetricValue* v = s.get(id);
    return v != nullptr ? v->text : std::string();
}

void testReaderGoldenPath() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    setActiveServer(mem, buildWorld(mem, 0x10000, /*index=*/300, /*extraTail=*/0, /*idStart=*/0).srv);

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    CHECK(reader.resets() == 0);
    CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamNamedEncode, 2.6, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEstimatedBandwidth, 232.0, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink1Ping, 9.2, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Share, 100.0 * 434 / 1059, 1e-4));
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEncodeAvg, 2.6, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamTotalAvg, 53.32, 1e-3));
    // no record carries a Decode Query: slack stays, hold/display go N/A (NoFrames), the rest still publishes
    CHECK(isOkF32(store, metric::kPcStreamSlackAvg, 36.0, 1e-6));
    for (uint16_t id = 1054; id <= 1057; ++id) {
        CHECK(notOk(store, id));
        CHECK(store.get(id)->reason == Reason::NoFrames);
    }
}

void testReaderPublishesHoldDisplay() {
    gFixtureDecodeQueryUs = 4000;
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    setActiveServer(mem, buildWorld(mem, 0x10000, /*index=*/300, /*extraTail=*/0, /*idStart=*/0).srv);

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    gFixtureDecodeQueryUs = 0;

    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamSlackAvg, 36.0, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamSlackP95, 36.0, 1e-6));
    CHECK(isOkF32(store, 1054, 4.0, 1e-6));  // HoldAvg
    CHECK(isOkF32(store, 1055, 4.0, 1e-6));  // HoldP95
    CHECK(isOkF32(store, 1056, 32.0, 1e-6)); // DisplayAvg
    CHECK(isOkF32(store, 1057, 32.0, 1e-6)); // DisplayP95
    CHECK(isOkF32(store, metric::kPcStreamTotalAvg, 53.32, 1e-3));
}

void testReaderHeaderMismatchOnlyL3() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    setActiveServer(mem, buildWorld(mem, 0x10000, 300, 0, 0).srv);

    Anchors a = goldenAnchors(kBuild);
    a.l3 = false;
    a.l3Note = "CSV header/stride mismatch"; // details and build travel in separate short-string fields

    Reader reader(mem, kModuleBase, a);
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    // L1 and L2 are unaffected by an L3-only anchors failure
    CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    // L3 rows stay N/A, carrying the build note
    CHECK(noteOf(store, metric::kPcStreamL3Note) == a.l3Note);
    CHECK(noteOf(store, metric::kPcStreamBuild) == std::to_string(kBuild));
    CHECK(notOk(store, metric::kPcStreamFps));
    CHECK(notOk(store, metric::kPcStreamEncodeAvg));
    CHECK(notOk(store, metric::kPcStreamWireMbps));
    CHECK(store.get(metric::kPcStreamFps)->reason == Reason::SteamVrChanged);
    CHECK(store.get(metric::kPcStreamEncodeAvg)->reason == Reason::SteamVrChanged);
}

void testReaderRttiMismatchOnlyL2() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    // the live Uber object's vtable drifted to something the cached RTTI name doesn't match (a struct-layout
    // change the static anchors never saw, as opposed to the header-mismatch case above)
    setObjectRtti(mem, w.uber, kVtBogus);

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
    CHECK(!noteOf(store, metric::kPcStreamL2Note).empty()); // "link RTTI/packets mismatch"
    CHECK(notOk(store, metric::kPcStreamLink0Ping));
    CHECK(notOk(store, metric::kPcStreamLink1Share));
    CHECK(store.get(metric::kPcStreamLink0Ping)->reason == Reason::SteamVrChanged);
    // L3 keeps working: the RTTI drift was specific to the Uber/link objects, not the frame ring
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

void testReaderOwnerChangeWaitsForNewBaseline() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, /*extraTail=*/1, 5000); // one spare slot for index+1
    setActiveServer(mem, w1.srv);

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());

    reader.tick(0.0, store);
    CHECK(reader.resets() == 0);
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    const uint64_t publishedBefore = reader.published();
    CHECK(publishedBefore > 0);

    // a stream reset rebuilds the objects inside the same PID: swap the live SVLServer pointer. L2 doesn't
    // depend on the AFTM ring advancing at all: it revalidates the new Uber/link owners on this very tick and
    // resumes immediately. L3 is the one that must wait for its own ring baseline to actually move forward,
    // not just for the index to read as a different (unrelated) number.
    setActiveServer(mem, w2.srv);
    reader.tick(1.0, store);
    CHECK(reader.resets() == 1);
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Share, 100.0 * 434 / 1059, 1e-4));
    CHECK(noteOf(store, metric::kPcStreamL3Note) == "stream reset; waiting");
    CHECK(notOk(store, metric::kPcStreamFps));
    CHECK(store.get(metric::kPcStreamFps)->reason == Reason::NoFrames);

    // the ring hasn't moved since: L3 is still waiting, L2 stays valid, no further reset counted
    reader.tick(2.0, store);
    CHECK(reader.resets() == 1);
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    CHECK(notOk(store, metric::kPcStreamFps));

    // now the new baseline actually advances (the ring moved forward by one): L3 publishing resumes too, with
    // a completely fresh dedupe/publish state (the old world's last-published Present IDs are gone)
    mem.poke<uint16_t>(w2.srv + 0x88000, static_cast<uint16_t>(501));
    reader.tick(3.0, store);
    CHECK(reader.resets() == 1); // resolving the wait is not itself another reset
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(reader.published() > 0);
}

/** T08 review finding: a disabled L3 (static header/stride mismatch) permanently blocked L2's recovery after
 *  an owner change, because the shared "waiting" early-return never resolves when the AFTM index can never
 *  be read at all (anchors.l3 == false). L2 must come back on the very first post-reset tick regardless, and
 *  stay valid across further ticks even though the (disabled) ring index never advances. Mirrors the review's
 *  own reproduction (owner swap, ticks at t=11/12/22/42), except the ring index is never bumped here at all:
 *  that's the whole point -- L2's recovery must not depend on it in any way. */
void testReaderL2RecoversIndependentlyOfDisabledL3() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, 0, 5000); // index never changes after this
    setActiveServer(mem, w1.srv);

    Anchors a = goldenAnchors(kBuild);
    a.l3 = false;
    a.l3Note = "CSV header/stride mismatch";

    Reader reader(mem, kModuleBase, a);
    MetricStore store(readerMetricDefs());

    reader.tick(10.0, store);
    CHECK(reader.resets() == 0);
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL3Note) == a.l3Note);

    setActiveServer(mem, w2.srv);
    for (const double now : {11.0, 12.0, 22.0, 42.0}) {
        reader.tick(now, store);
        CHECK(reader.resets() == 1); // one owner change, counted once, not once per tick
        CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
        CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
        // L2 recovers on the very first post-reset tick and stays valid: it never waits on L3's ring index
        CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
        CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
        CHECK(isOkF32(store, metric::kPcStreamLink1Share, 100.0 * 625 / 1059, 1e-4));
        // L3 stays in its original disabled state throughout -- it never flips to "waiting" for a ring
        // advance that (with l3 statically off) can never happen
        CHECK(noteOf(store, metric::kPcStreamL3Note) == a.l3Note);
        CHECK(notOk(store, metric::kPcStreamFps));
    }
}

/** 2026-10-17: SteamVR restarted under a running app. The reader walked the stats map 3 s after vrserver
 *  started, when it held only sPingMS and sRXpps, and never walked it again because those two nodes stayed
 *  valid. dbgEncTimeMS never arrived, so L3's encode cross-check failed on every retry. The map's root stays
 *  put here, as it did live (no owner change was logged), so only the element count can trigger the re-walk. */
void testReaderRewalksWhenStatsMapGrows() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    const uint64_t head = w.srv + 0x90000, root = w.srv + 0x90100; // buildWorld's map, rewritten in place
    writeMap(mem, w.srv, head, root, {{"sPingMS", 2.0}, {"sRXpps", 1059}});

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
    CHECK(notOk(store, metric::kPcStreamNamedEncode));
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "missing dbgEncTimeMS +7"); // 8 of the 9 shown keys
    CHECK(noteOf(store, metric::kPcStreamL3Note) == "frame/bandwidth cross-check");

    // the encoder starts and the rest of the keys appear: L1 picks them up on the next tick. The two cached
    // nodes keep their address and key (a real insert never moves a node), so no cached key goes bad.
    writeMap(mem, w.srv, head, root,
             {{"sPingMS", 2.0},
              {"sRXpps", 1059},
              {"dbgEncTimeMS", 2.6},
              {"txTimeMS", 0.18},
              {"dEstLinkBW", 232.0},
              {"rexmitReq", 4670},
              {"rexmitSent", 4013},
              {"nFramesSkippedPS", 0},
              {"dFrmsLate", 0},
              {"encResets", 3}});
    reader.tick(10.5, store);
    CHECK(reader.resets() == 0);
    CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamNamedEncode, 2.6, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEstimatedBandwidth, 232.0, 1e-6));
    CHECK(noteOf(store, metric::kPcStreamL3Note) == "frame validation retry pending");

    // and L3 passes its cross-check on its next retry
    reader.tick(20.0, store);
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEncodeAvg, 2.6, 1e-6));
}

/** 2026-10-17, headset idling at ~4 Mbit/s: 63 of 8000 bandwidth records carried more audio than wire bytes.
 *  This one is copied from that capture (wire, video, fec, logic, audio, retry, dword 6). Audio only gets the
 *  absolute bound, so one such record in the last second must not fail L3. */
void testReaderIdleAudioAboveWireKeepsL3() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    const int32_t idle[7] = {8272, 216, 1024, 24, 8960, 0, 1904};
    mem.write(w.bw + 100 * 0xa8, idle, sizeof(idle)); // inside the last 1 s; its timestamp at +0x40 stays

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

/** Every other dword still has to fit inside its record's wire bytes: video above wire fails L3 closed. */
void testReaderNonAudioAboveWireFailsL3() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    const int32_t bad[7] = {8272, 9000, 1024, 24, 2000, 0, 1904};
    mem.write(w.bw + 100 * 0xa8, bad, sizeof(bad));

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(noteOf(store, metric::kPcStreamL3Note) == "frame/bandwidth cross-check");
    CHECK(notOk(store, metric::kPcStreamFps));
    CHECK(notOk(store, metric::kPcStreamWireMbps));
}

/** 2026-10-29: the headset switched 72 -> 90 Hz mid-session while OpenVR's display frequency (and the app's single
 *  read of it) still said 72, so a rate-vs-Hz term kept L3 off for the rest of the session. A 90 fps ring that
 *  passes every other cross-check must publish, whatever the HMD property says. */
void testReaderL3Valid90HzStream() {
    gFixtureStepUs = 11111; // 90 Hz: the latest 1 s of the 144-record tail holds 91 records
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    setActiveServer(mem, buildWorld(mem, 0x10000, 300, 0, 0).srv);
    gFixtureStepUs = kFixtureStepUs;

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, 91, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEncodeAvg, 2.6, 1e-6));
}

// ==================== Reader: gpu-stamps layout, and named-encode averaging ====================

constexpr uint32_t kLayoutAftmOffset = 0xffe0; // keep wrapped slot 0 clear of the server's fields

/** Generalized buildWorld: takes an arbitrary FrameLayout (stride + indexOffset) and wraps the frame-ring
 *  slot index through kFrameSlots, so an index near the wrap boundary exercises the real two-piece read
 *  Reader::tick performs (buildWorld() above never wraps: every index it's given is well under 4096). */
World buildWorldLayout(FakeMemory& mem, uint64_t base, const FrameLayout& layout, uint32_t index, uint32_t extraTail,
                      uint32_t idStart) {
    World w;
    w.srv = kModuleBase + base;
    const uint64_t head = kModuleBase + base + 0x90000;
    const uint64_t root = kModuleBase + base + 0x90100;
    w.uber = kModuleBase + base + 0xA0000;
    w.link0 = kModuleBase + base + 0xA1000;
    w.link1 = kModuleBase + base + 0xA2000;
    w.bw = kModuleBase + base + 0xB0000;

    setObjectRtti(mem, w.srv, kVtServer);
    setObjectRtti(mem, w.uber, kVtUber);
    setObjectRtti(mem, w.link0, kVtLink);
    setObjectRtti(mem, w.link1, kVtLink);

    mem.poke<uint64_t>(w.srv + 0x188, w.uber);
    mem.poke<uint64_t>(w.srv + 0x1f0, w.bw);
    mem.poke<uint32_t>(w.srv + 0x1e8, 8192);
    mem.poke<uint32_t>(w.srv + 0x1ec, 144);

    uint64_t linksArr[16] = {};
    linksArr[0] = w.link0;
    linksArr[1] = w.link1;
    mem.write(w.uber + 0x388, linksArr, sizeof(linksArr));
    mem.poke<uint32_t>(w.link0 + 0x24, 434);
    mem.poke<uint32_t>(w.link0 + 0x2c, 11700); // us
    mem.poke<uint32_t>(w.link1 + 0x24, 625);
    mem.poke<uint32_t>(w.link1 + 0x2c, 9200); // 434 + 625 = 1059 = sRXpps below

    writeMap(mem, w.srv, head, root,
             {{"sPingMS", 2.0},
              {"dbgEncTimeMS", 2.6},
              {"txTimeMS", 0.18},
              {"dEstLinkBW", 232.0},
              {"rexmitReq", 4670},
              {"rexmitSent", 4013},
              {"nFramesSkippedPS", 0},
              {"dFrmsLate", 0},
              {"encResets", 3},
              {"sRXpps", 1059}});

    const uint32_t start = (index - kTailFrames) & 0xfff;
    const uint32_t count = kTailFrames + extraTail;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = (start + i) & 0xfff; // wraps through the 4096-slot ring
        uint32_t t[13] = {};
        baseFields(t, i * gFixtureStepUs, idStart + i);
        if (gFixtureDecodeQueryUs != 0)
            t[12] = t[9] + gFixtureDecodeQueryUs;
        const auto raw = frameBytesForLayout(layout, t, 2000);
        mem.write(w.srv + kLayoutAftmOffset + static_cast<uint64_t>(slot) * layout.stride, raw.data(), raw.size());
        writeBandwidthRecord(mem, w.bw + static_cast<uint64_t>(i) * 0xa8, i * gFixtureStepUs);
    }
    mem.poke<uint16_t>(w.srv + kLayoutAftmOffset + layout.indexOffset, static_cast<uint16_t>(index));
    return w;
}

Anchors anchorsForLayout(uint32_t timestamp, const FrameLayout& layout) {
    Anchors a = goldenAnchors(timestamp);
    a.layout = &layout;
    a.aftmOffset = kLayoutAftmOffset;
    a.stride = layout.stride;
    a.fileOffset = layout.fileOffset;
    return a;
}

/** Reader::tick's own golden path, but on the gpu-stamps layout (stride 0x98, index at aftmOffset+0xa8050)
 *  and with the frame ring wrapped: index=100 puts the 144-record tail's start at (100-144)&0xfff=4052, so
 *  44 records come from the top of the ring and the other 100 wrap to slot 0 -- the two-piece read. Using the
 *  wrong stride or index offset here would misread every record (parseFrame would see garbage, not merely
 *  shifted-but-valid data), so a clean pass is real evidence the layout's fields are honored. */
void testReaderGpuStampsGoldenPathWithRingWrap() {
    const auto& layout = kFrameLayouts[1];
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    setActiveServer(mem, buildWorldLayout(mem, 0x10000, layout, /*index=*/100, /*extraTail=*/0, /*idStart=*/0).srv);

    Reader reader(mem, kModuleBase, anchorsForLayout(kBuild, layout));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    CHECK(reader.resets() == 0);
    CHECK(noteOf(store, metric::kPcStreamL1Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamNamedPing, 2.0, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamEncodeAvg, 2.6, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamTotalAvg, 53.32, 1e-3));
}

/** The "dbgEncTimeMS" map node's value field, for a world buildWorldLayout()/buildWorld() built: root
 *  (srv+0x90100) + node index 1 (insertion order: sPingMS, dbgEncTimeMS, ...) * 0x100 + 0x40 (the double
 *  value, per writeMapNode). Rewriting it in place changes what the *next* tick's named() read returns,
 *  without touching the node's address or key -- exactly how a real value update behaves. */
constexpr uint64_t kEncodeNodeValueOffset = 0x90100 + 0x100 + 0x40;

void setEncodeNamedValue(FakeMemory& mem, uint64_t srv, double value) {
    mem.poke<double>(srv + kEncodeNodeValueOffset, value);
}

/** Rewrites a world's frame-ring tail and bandwidth tail as the stream would look `generation` Service ticks
 *  (0.5 s each = 36 new records at the fixture's 14 ms cadence) after the initial build: the ring index
 *  advances by 36, and record n (global, = generation*36 + position) keeps the SAME Present ID and the SAME
 *  stamps in every generation that still contains it. So overlapping ids dedupe, only 36 are new per tick,
 *  and the recent window stays at kFixtureRecentCount instead of expiring (FrameWindow drops every record
 *  1 s after it was received) or doubling. Generation 0 is identical to buildWorldLayout()'s tail. */
void writeCadenceTail(FakeMemory& mem, const World& w, const FrameLayout& layout, uint32_t initialIndex,
                      uint32_t idStart, uint32_t generation) {
    const uint32_t index = (initialIndex + generation * 36) & 0xfff;
    const uint32_t start = (index - kTailFrames) & 0xfff;
    for (uint32_t i = 0; i < kTailFrames; ++i) {
        const uint32_t n = generation * 36 + i;
        uint32_t t[13] = {};
        baseFields(t, n * gFixtureStepUs, idStart + n);
        const auto raw = frameBytesForLayout(layout, t, 2000);
        mem.write(w.srv + kLayoutAftmOffset + static_cast<uint64_t>((start + i) & 0xfff) * layout.stride,
                  raw.data(), raw.size());
        writeBandwidthRecord(mem, w.bw + static_cast<uint64_t>(i) * 0xa8, n * gFixtureStepUs);
    }
    mem.poke<uint16_t>(w.srv + kLayoutAftmOffset + layout.indexOffset, static_cast<uint16_t>(index));
}

/** One world driven at the 0.5 s Service cadence: before every tick, the ring is advanced to that tick's
 *  generation (t * 2) and the named encode value is set. */
struct CadenceRun {
    FakeMemory mem;
    World w;
    const FrameLayout& layout;
    uint32_t initialIndex, idStart;
    Reader reader;
    MetricStore store;

    explicit CadenceRun(const FrameLayout& l, uint32_t index = 300, uint32_t ids = 0)
        : layout(l), initialIndex(index), idStart(ids), reader(mem, kModuleBase, anchorsForLayout(kKnownBuild, l)),
          store(readerMetricDefs()) {
        setupModule(mem, kKnownBuild);
        w = buildWorldLayout(mem, 0x10000, l, index, 0, ids);
        setActiveServer(mem, w.srv);
    }
    void tick(double t, double encodeMs) {
        writeCadenceTail(mem, w, layout, initialIndex, idStart, static_cast<uint32_t>(std::lround(t * 2)));
        setEncodeNamedValue(mem, w.srv, encodeMs);
        reader.tick(t, store);
    }
};

/** Named-encode (dbgEncTimeMS) oscillating +-1.5 ms around the true, AFTM-measured mean (2.6 ms, fixed by
 *  baseFields on every record) must still pass the L3 cross-check at the next periodic (10 s) recheck: the
 *  check averages the whole 5 s window, not the single latest sample. The very first tick gets exactly the
 *  true mean -- there's no warmup period, the check runs on tick 1 regardless of how few samples exist.
 *  Three-tick high/low bursts leave the last two samples high at the recheck: a one-second window or the
 *  latest sample would be 1.5 ms off and fail, while the five-second mean is exactly right. */
void runEncodeOscillationPassesAtPeriodicCheck(const FrameLayout& layout) {
    CadenceRun r(layout);
    r.tick(0.0, 2.6);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));

    // 0.5 s ticks oscillating +-1.5 ms; the t=0 check armed a 10 s cooldown, so none are validated before t=10.
    for (int k = 1; k <= 20; ++k) {
        const double t = k * 0.5;
        r.tick(t, ((k - 1) / 3) % 2 ? 2.6 - 1.5 : 2.6 + 1.5);
        CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty());
        CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6)); // frames stay live, not "no frames"
    }
    CHECK(isOkF32(r.store, metric::kPcStreamEncodeAvg, 2.6, 1e-6)); // the *measured* average, unaffected by this
}

/** A consistent +2 ms bias (not oscillation) on dbgEncTimeMS must fail the same periodic check: the window
 *  average then sits 2 ms off the true mean, beyond the +-1 ms tolerance. */
void runEncodeConsistentOffsetFailsAtPeriodicCheck(const FrameLayout& layout) {
    CadenceRun r(layout);
    r.tick(0.0, 2.6);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty()); // first tick: the lone sample is exactly the mean

    for (int k = 1; k <= 19; ++k) {
        r.tick(k * 0.5, 2.6 + 2.0); // consistently 2 ms high, every tick in the window
        CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6)); // still enabled, frames live
    }
    r.tick(10.0, 2.6 + 2.0); // the recheck: frames are live (72), so the failure is the encode average
    CHECK(noteOf(r.store, metric::kPcStreamL3Note) == "frame/bandwidth cross-check");
    CHECK(notOk(r.store, metric::kPcStreamFps));
}

/** Samples more than 5 s stale must not count toward the window average: a wildly-off burst that fully ages
 *  out before the next periodic check leaves that check passing on the clean, recent samples alone. */
void runStaleEncodeSamplesExpire(const FrameLayout& layout) {
    CadenceRun r(layout);
    r.tick(0.0, 2.6);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty());

    for (int k = 1; k <= 9; ++k)
        r.tick(k * 0.5, 50.0); // wildly poisoned (t=0.5..4.5), but all >= 5 s stale by the t=10 recheck
    for (int k = 10; k <= 20; ++k)
        r.tick(k * 0.5, 2.6); // clean, inside the 5 s window at t=10
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

/** An owner change (stream reset) clears encodeSamples_ outright: a heavily poisoned history from the old
 *  SVLServer must not haunt the new one. Once the new baseline's ring advances, L3 resumes immediately on the
 *  fresh (2-sample) window -- the same "no warmup" rule as the very first tick of the reader's life. */
void runOwnerChangeClearsEncodeSamples(const FrameLayout& layout) {
    CadenceRun r(layout, 300, 1000);
    const World w2 = buildWorldLayout(r.mem, 0x300000, layout, 500, /*extraTail=*/1, 5000);

    r.tick(0.0, 2.6);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty());

    for (int k = 1; k <= 4; ++k) {
        r.tick(k * 0.5, 500.0); // badly poison world 1 (its ring keeps advancing) right before the swap
        CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    }

    setActiveServer(r.mem, w2.srv);
    setEncodeNamedValue(r.mem, w2.srv, 2.6);
    r.reader.tick(2.5, r.store);
    CHECK(r.reader.resets() == 1);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note) == "stream reset; waiting"); // ring hasn't advanced yet

    r.mem.poke<uint16_t>(w2.srv + kLayoutAftmOffset + layout.indexOffset, static_cast<uint16_t>(501));
    setEncodeNamedValue(r.mem, w2.srv, 2.6);
    r.reader.tick(3.0, r.store);
    CHECK(noteOf(r.store, metric::kPcStreamL3Note).empty()); // passes on 2 fresh samples, not poisoned by w1's 500 ms
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

// ==================== readLinkEndpoint ====================

void testReadLinkEndpointValid() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 10, 35, 78, 1);
    const Anchors a = goldenAnchors(kKnownBuild);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a) == "10.35.78.1");
}

void testReadLinkEndpointGetterShapeChanged() {
    // a build where the getter's embedded offset moved (or the shape otherwise changed): fail closed rather
    // than trust whatever happens to be at the old offset
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    writeRemoteGetterCode(mem, kRemoteGetterRva, kRemoteAddressOffset + 8); // overwrite: wrong embedded offset
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 10, 35, 78, 1);
    const Anchors a = goldenAnchors(kKnownBuild);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty());
}

void testReadLinkEndpointBadFamily() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 23 /* AF_INET6 */, 0x1234, 10, 35, 78, 1);
    const Anchors a = goldenAnchors(kKnownBuild);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty());
}

void testReadLinkEndpointZeroPort() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0, 10, 35, 78, 1);
    const Anchors a = goldenAnchors(kKnownBuild);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty());
}

void testReadLinkEndpointInsaneIp() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    const Anchors a = goldenAnchors(kKnownBuild);

    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 0, 0, 0, 0);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty()); // 0.0.0.0
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 127, 0, 0, 1);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty()); // loopback
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 224, 0, 0, 1);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a).empty()); // multicast / reserved
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 192, 168, 1, 47);
    CHECK(readLinkEndpoint(mem, w.link0, kModuleBase, a) == "192.168.1.47"); // sane: accepted
}

void testReadLinkEndpointWrongObjectType() {
    // a non-TransferUDP object (e.g. the Uber) must never be read as if it had a remote address, even if
    // something happens to sit at the same offset
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    writeRemoteAddress(mem, w.uber, kRemoteAddressOffset, 2, 0x1234, 10, 35, 78, 1);
    const Anchors a = goldenAnchors(kKnownBuild);
    CHECK(readLinkEndpoint(mem, w.uber, kModuleBase, a).empty());
}

// ==================== Reader::tick: endpoint publishing (ids 1090-1093) ====================

void testReaderEndpointSuccessPopulatesAddress() {
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    writeRemoteAddress(mem, w.link0, kRemoteAddressOffset, 2, 0x1234, 10, 35, 78, 1);
    writeRemoteAddress(mem, w.link1, kRemoteAddressOffset, 2, 0x1234, 192, 168, 1, 47);

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(noteOf(store, metric::kPcStreamLink0Address) == "10.35.78.1");
    CHECK(noteOf(store, metric::kPcStreamLink1Address) == "192.168.1.47");
}

void testReaderEndpointFailureDoesNotAffectL2Timing() {
    // the getter's backing-field signature changed (a SteamVR update moved the struct, say): the address
    // extraction must fail closed (N/A, so streamLinkName falls back to "Link N") without taking L2's own
    // ping/share timing down with it -- those come from a completely separate read.
    FakeMemory mem;
    constexpr uint32_t kBuild = kKnownBuild;
    setupModule(mem, kBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    writeRemoteGetterCode(mem, kRemoteGetterRva, kRemoteAddressOffset + 8); // "changed backing-field signature"

    Reader reader(mem, kModuleBase, goldenAnchors(kBuild));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);

    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamLink0Ping, 11.7, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink1Ping, 9.2, 1e-6));
    CHECK(isOkF32(store, metric::kPcStreamLink0Share, 100.0 * 434 / 1059, 1e-4));
    CHECK(isOkF32(store, metric::kPcStreamLink1Share, 100.0 * 625 / 1059, 1e-4));
    CHECK(notOk(store, metric::kPcStreamLink0Address));
    CHECK(notOk(store, metric::kPcStreamLink1Address));
}

// ==================== Reader: SVLServer found by an RTTI scan of .data ====================

constexpr uint32_t kScanDataRva = 0x3000, kScanDataSize = 0x200; // 64 qwords of fake .data
constexpr uint64_t kBogusObject = kModuleBase + 0x4000;          // an object whose RTTI name is "bogus"

/** Anchors with the code-pair global dropped (or kept) and the scan inputs a real findAnchors would fill. */
Anchors scanAnchors(uint32_t timestamp, bool codePair) {
    Anchors a = goldenAnchors(timestamp);
    if (!codePair)
        a.serverGlobal = 0;
    a.serverVtables = {static_cast<uint32_t>(kVtServer - kModuleBase)};
    a.dataRva = kScanDataRva;
    a.dataSize = kScanDataSize;
    return a;
}

void setDataGlobal(FakeMemory& mem, unsigned slot, uint64_t pointerValue) {
    mem.poke<uint64_t>(kModuleBase + kScanDataRva + slot * 8ull, pointerValue);
}

/** L1 fully live: no note, and a named value actually published. */
bool l1Live(const MetricStore& s) {
    return noteOf(s, metric::kPcStreamL1Note).empty() && isOkF32(s, metric::kPcStreamNamedPing, 2.0, 1e-6);
}

void testScanAgreeingGlobalsPublish() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setObjectRtti(mem, kBogusObject, kVtBogus);
    // three globals at the same server (lowest is slot 1), plus two pointers that are not an SVLServer
    setDataGlobal(mem, 32, w.srv);
    setDataGlobal(mem, 8, w.srv);
    setDataGlobal(mem, 1, w.srv);
    setDataGlobal(mem, 5, kBogusObject);
    setDataGlobal(mem, 6, w.uber);

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, false));
    MetricStore store(readerMetricDefs());
    CHECK(reader.serverHow().empty()); // nothing chosen before the first tick
    reader.tick(10.0, store);
    CHECK(reader.serverHow() == "RTTI scan (rva 3008)"); // the scan ran and the lowest hit was taken
    CHECK(l1Live(store));
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

void testScanDisagreeingGlobalsFailL1() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, 0, 5000);
    setDataGlobal(mem, 2, w1.srv);
    setDataGlobal(mem, 9, w2.srv);

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, false));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
    CHECK(reader.serverHow().empty());
    CHECK(notOk(store, metric::kPcStreamNamedPing));
    CHECK(notOk(store, metric::kPcStreamFps));
}

void testScanNoHitFailsL1() {
    // only non-servers in .data: a bogus-RTTI object and the Uber
    {
        FakeMemory mem;
        setupModule(mem, kKnownBuild);
        const World w = buildWorld(mem, 0x10000, 300, 0, 0);
        setObjectRtti(mem, kBogusObject, kVtBogus);
        setDataGlobal(mem, 3, kBogusObject);
        setDataGlobal(mem, 4, w.uber);
        Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, false));
        MetricStore store(readerMetricDefs());
        reader.tick(10.0, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
        CHECK(reader.serverHow().empty());
        CHECK(notOk(store, metric::kPcStreamNamedPing));
    }
    // a real server, but its vtable is not in the SVLServer vtable list
    {
        FakeMemory mem;
        setupModule(mem, kKnownBuild);
        const World w = buildWorld(mem, 0x10000, 300, 0, 0);
        setDataGlobal(mem, 3, w.srv);
        Anchors a = scanAnchors(kKnownBuild, false);
        a.serverVtables = {static_cast<uint32_t>(kVtLink - kModuleBase)};
        Reader reader(mem, kModuleBase, a);
        MetricStore store(readerMetricDefs());
        reader.tick(10.0, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
        CHECK(notOk(store, metric::kPcStreamNamedPing));
    }
    // a listed vtable whose RTTI name is not SVLServer is still refused
    {
        FakeMemory mem;
        setupModule(mem, kKnownBuild);
        buildWorld(mem, 0x10000, 300, 0, 0);
        setObjectRtti(mem, kBogusObject, kVtBogus);
        setDataGlobal(mem, 3, kBogusObject);
        Anchors a = scanAnchors(kKnownBuild, false);
        a.serverVtables.push_back(static_cast<uint32_t>(kVtBogus - kModuleBase));
        Reader reader(mem, kModuleBase, a);
        MetricStore store(readerMetricDefs());
        reader.tick(10.0, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
    }
}

/** The chosen global goes stale and another one points at a new server. The scan only runs every 10 s, so the
 *  ticks before the gate stay N/A; the first one past it recovers and counts the owner change. */
void testScanStalePointerRecoversAfterGate() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, /*extraTail=*/1, 5000);
    setObjectRtti(mem, kBogusObject, kVtBogus);
    setDataGlobal(mem, 0, w1.srv);

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, false));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store); // scan, gate now 20
    CHECK(reader.serverHow() == "RTTI scan (rva 3000)");
    CHECK(l1Live(store));

    setDataGlobal(mem, 0, kBogusObject); // the chosen global goes stale
    setDataGlobal(mem, 1, w2.srv);       // another global already points at the new server
    for (const double now : {11.0, 15.0, 19.5}) {
        reader.tick(now, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch"); // gated: no scan yet
        CHECK(reader.serverHow().empty());
        CHECK(notOk(store, metric::kPcStreamNamedPing));
    }
    CHECK(reader.resets() == 0);

    reader.tick(20.0, store); // gate open
    CHECK(reader.serverHow() == "RTTI scan (rva 3008)");
    CHECK(l1Live(store));
    CHECK(reader.resets() == 1);
    CHECK(noteOf(store, metric::kPcStreamL2Note).empty());
    CHECK(noteOf(store, metric::kPcStreamL3Note) == "stream reset; waiting"); // the owner change was seen

    mem.poke<uint16_t>(w2.srv + 0x88000, static_cast<uint16_t>(501));
    reader.tick(20.5, store);
    CHECK(noteOf(store, metric::kPcStreamL3Note).empty());
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

/** The chosen global is cleared and nothing else points at a server: an empty scan still re-arms the gate. */
void testScanEmptyScanRearmsGate() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 1000);
    setDataGlobal(mem, 0, w.srv);

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, false));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(l1Live(store));

    setDataGlobal(mem, 0, 0);
    reader.tick(11.0, store); // gated
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
    reader.tick(20.0, store); // scans, finds nothing: the gate moves to 30
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");

    setDataGlobal(mem, 1, w.srv); // a hit exists now, but the scan at 20 re-armed the gate
    reader.tick(21.0, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
    CHECK(reader.serverHow().empty());
    reader.tick(29.9, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");

    reader.tick(30.0, store);
    CHECK(reader.serverHow() == "RTTI scan (rva 3008)");
    CHECK(l1Live(store));
    CHECK(reader.resets() == 0); // same object as before, so not an owner change
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

void testScanAgreesWithCodePair() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv); // the code-pair global
    setDataGlobal(mem, 2, w.srv);
    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(reader.serverHow() == "code pair, RTTI scan agrees (rva 2000)");
    CHECK(l1Live(store));
    CHECK(isOkF32(store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
}

void testScanDisagreesWithCodePair() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, 0, 5000);
    setActiveServer(mem, w1.srv);
    setDataGlobal(mem, 2, w2.srv);
    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
    CHECK(reader.serverHow().empty());
    CHECK(notOk(store, metric::kPcStreamNamedPing));
}

void testCodePairAloneWithEmptyScanArea() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    setActiveServer(mem, buildWorld(mem, 0x10000, 300, 0, 0).srv);
    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true)); // scan inputs set, .data holds no server
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(reader.serverHow() == "code pair (rva 2000)");
    CHECK(l1Live(store));
}

/** The code-pair global goes stale while a .data global points at the server: the scan takes over after its gate. */
void testCodePairStaleFallsBackToScan() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w = buildWorld(mem, 0x10000, 300, 0, 0);
    setActiveServer(mem, w.srv);
    setDataGlobal(mem, 2, w.srv);
    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store);
    CHECK(reader.serverHow() == "code pair, RTTI scan agrees (rva 2000)");

    setActiveServer(mem, 0);
    reader.tick(11.0, store); // gated
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server RTTI/map mismatch");
    CHECK(reader.serverHow().empty());
    reader.tick(20.0, store);
    CHECK(reader.serverHow() == "RTTI scan (rva 3010)");
    CHECK(l1Live(store));
    CHECK(reader.resets() == 0);
}

// ==================== findAnchors on a synthetic PE ====================

/** A minimal PE32+ image: .text (rva 0x1000), .rdata (0x2000, optionally an SVLServer RTTI chain), .data (0x3000,
 *  virtual size 0x1000). `text` is copied to the start of .text. Hand-built, no bytes from a real driver. */
std::vector<uint8_t> buildPe(const std::vector<uint8_t>& text, bool rtti) {
    constexpr uint64_t kImageBase = 0x180000000ULL;
    std::vector<uint8_t> f(0xa00, 0);
    const auto w16 = [&](size_t o, uint16_t v) { std::memcpy(f.data() + o, &v, 2); };
    const auto w32 = [&](size_t o, uint32_t v) { std::memcpy(f.data() + o, &v, 4); };
    const auto w64 = [&](size_t o, uint64_t v) { std::memcpy(f.data() + o, &v, 8); };
    f[0] = 'M';
    f[1] = 'Z';
    w32(0x3c, 0x80);
    const size_t p = 0x80;
    w32(p, 0x4550);
    w16(p + 4, 0x8664);
    w16(p + 6, 3);
    w32(p + 8, 0x5ee00001);
    w16(p + 20, 0xf0);
    const size_t opt = p + 24;
    w16(opt, 0x20b);
    w64(opt + 24, kImageBase);
    w32(opt + 56, 0x5000);
    struct Sec {
        const char* name;
        uint32_t vsize, rva, rsize, raw;
    };
    const Sec secs[3] = {{".text", 0x200, 0x1000, 0x200, 0x400},
                         {".rdata", 0x200, 0x2000, 0x200, 0x600},
                         {".data", 0x1000, 0x3000, 0x200, 0x800}};
    for (size_t i = 0; i < 3; ++i) {
        const size_t s = opt + 0xf0 + i * 40;
        std::memcpy(f.data() + s, secs[i].name, std::strlen(secs[i].name));
        w32(s + 8, secs[i].vsize);
        w32(s + 12, secs[i].rva);
        w32(s + 16, secs[i].rsize);
        w32(s + 20, secs[i].raw);
    }
    std::memcpy(f.data() + 0x400, text.data(), text.size());
    if (rtti) {
        const std::string name = ".?AVSVLServer@@";
        std::memcpy(f.data() + 0x600 + 0x10, name.c_str(), name.size() + 1); // type descriptor at rva 0x2000
        const size_t col = 0x600 + 0x40;                                     // locator at rva 0x2040
        w32(col, 1);
        w32(col + 12, 0x2000);
        w32(col + 20, 0x2040);
        w64(0x600 + 0x80, kImageBase + 0x2040); // locator pointer; the vtable starts right after, rva 0x2088
    }
    return f;
}

/** `mov [rip+g1],r15` then `mov [rip+g2],rbx` back to back, the shape of the server global's store pair. */
void storePair(std::vector<uint8_t>& text, uint32_t g1, uint32_t g2) {
    const uint32_t rva = 0x1000 + static_cast<uint32_t>(text.size());
    const int32_t r1 = static_cast<int32_t>(g1) - static_cast<int32_t>(rva + 7);
    const int32_t r2 = static_cast<int32_t>(g2) - static_cast<int32_t>(rva + 14);
    const uint8_t a[3] = {0x4c, 0x89, 0x3d}, b[3] = {0x48, 0x89, 0x1d};
    text.insert(text.end(), a, a + 3);
    text.insert(text.end(), reinterpret_cast<const uint8_t*>(&r1), reinterpret_cast<const uint8_t*>(&r1) + 4);
    text.insert(text.end(), b, b + 3);
    text.insert(text.end(), reinterpret_cast<const uint8_t*>(&r2), reinterpret_cast<const uint8_t*>(&r2) + 4);
}

void testFindAnchorsCodePairWithoutGapRule() {
    // the second store is 0x7f0 bytes after the first (the old rule demanded exactly 0x40)
    {
        std::vector<uint8_t> text;
        storePair(text, 0x3010, 0x3800);
        const Anchors a = findAnchors(buildPe(text, true));
        CHECK(a.serverGlobal == 0x3010);
        CHECK(a.serverVtables.size() == 1 && a.serverVtables[0] == 0x2088);
        CHECK(a.dataRva == 0x3000 && a.dataSize == 0x1000);
        CHECK(a.serverHow == "code pair+RTTI scan");
    }
    // the second global below the first (build 1794602179 had -0xa18)
    {
        std::vector<uint8_t> text;
        storePair(text, 0x3f00, 0x3008);
        CHECK(findAnchors(buildPe(text, true)).serverGlobal == 0x3f00);
    }
    // no RTTI chain in the image: the pair alone still anchors, with no scan inputs
    {
        std::vector<uint8_t> text;
        storePair(text, 0x3010, 0x3800);
        const Anchors a = findAnchors(buildPe(text, false));
        CHECK(a.serverGlobal == 0x3010);
        CHECK(a.serverVtables.empty());
        CHECK(a.serverHow == "code pair");
    }
    // the global must sit inside .data
    {
        std::vector<uint8_t> text;
        storePair(text, 0x2100, 0x3800);
        const Anchors a = findAnchors(buildPe(text, true));
        CHECK(a.serverGlobal == 0);
        CHECK(a.serverHow == "RTTI scan");
    }
    // two different pairs are ambiguous, not "pick the first"
    {
        std::vector<uint8_t> text;
        storePair(text, 0x3010, 0x3800);
        text.push_back(0xcc);
        storePair(text, 0x3200, 0x3800);
        CHECK(findAnchors(buildPe(text, true)).serverGlobal == 0);
    }
}

// ==================== decodeCsvWriter / deriveLayout on a synthetic writer ====================

/** Hand-assembled in the writer's shape (no bytes from any real driver). */
struct Asm {
    std::vector<uint8_t> b;
    void u8(unsigned v) {
        b.push_back(static_cast<uint8_t>(v));
    }
    void i32(int32_t v) {
        for (unsigned k = 0; k < 4; ++k)
            u8(static_cast<unsigned>(v >> (8 * k)) & 0xff);
    }
    void rex(bool w, unsigned reg, unsigned base) {
        const unsigned r = 0x40 | (w ? 8 : 0) | (reg >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0);
        if (r != 0x40)
            u8(r);
    }
    // [base+disp] operand, base not rsp/r12
    void mem(unsigned regField, unsigned base, int32_t disp) {
        const bool small = disp >= -128 && disp <= 127;
        u8((small ? 0x40 : 0x80) | ((regField & 7) << 3) | (base & 7));
        if (small)
            u8(static_cast<uint8_t>(disp));
        else
            i32(disp);
    }
    // mov [rsp+disp], r32 (SIB form)
    void stackStore(unsigned regField, int32_t disp) {
        const bool small = disp >= -128 && disp <= 127;
        rex(false, regField, 4);
        u8(0x89);
        u8((small ? 0x40 : 0x80) | ((regField & 7) << 3) | 4);
        u8(0x24);
        if (small)
            u8(static_cast<uint8_t>(disp));
        else
            i32(disp);
    }
};

struct Col {
    std::string name, spec;
    int32_t off;
    bool direct; // loaded straight from [record+off] instead of from the stack copy
};

struct Writer {
    std::vector<uint8_t> code;
    size_t lea = 0;
};

constexpr uint32_t kWriterFileOffset = 0xa8080;

/** imul reg,rax,stride; add reg,rcx; movups/movaps copy of the record to the stack; then the printf argument setup:
 *  r8 = column 0, r9 = column 1, columns 2.. at [rsp+0x20+8k]; lea of the format string; mov rcx,[rsi+file]; call. */
/** Instructions mixed into the writer that change what an argument holds (or that the decoder cannot follow). The
 *  mutated column is Receive (7), read against Decode End (9). */
enum class Mut { None, Cmov, BranchSelect, Loop, IndexedStore, SkippableErrorPath, AfterLeaBranch, BranchIntoArgs };

Writer emitWriter(uint32_t stride, bool imm8, unsigned reg, const std::vector<Col>& cols, Mut mut = Mut::None) {
    constexpr int32_t kStack = -0x180;
    constexpr size_t kMutCol = 7, kOtherCol = 9;
    Asm a;
    size_t earlyOutAt = 0;
    if (mut == Mut::SkippableErrorPath) { // the real writer's early-out: test eax,eax; je <past the print>
        a.u8(0x85);
        a.u8(0xc0);
        a.u8(0x0f);
        a.u8(0x84);
        earlyOutAt = a.b.size();
        a.i32(0);
    }
    a.rex(true, reg, 0);
    a.u8(imm8 ? 0x6b : 0x69);
    a.u8(0xc0 | ((reg & 7) << 3));
    if (imm8)
        a.u8(stride);
    else
        a.i32(static_cast<int32_t>(stride));
    a.rex(true, reg, 0);
    a.u8(0x03);
    a.u8(0xc0 | ((reg & 7) << 3) | 1);
    for (uint32_t c = 0; c < stride; c += 16) {
        a.rex(false, 0, reg);
        a.u8(0x0f);
        a.u8(0x10);
        a.mem(0, reg, static_cast<int32_t>(c));
        a.u8(0x0f);
        a.u8(0x29);
        a.mem(0, 5, kStack + static_cast<int32_t>(c));
    }
    for (size_t k = 2; k < cols.size(); ++k)
        if (cols[k].direct) {
            a.rex(false, 11, reg);
            a.u8(0x8b);
            a.mem(11, reg, cols[k].off);
            if (mut == Mut::BranchSelect && k == kMutCol) {
                // r11d = Receive; if (r11d != edx (Decode End)) r11d = edx -- as a branch, not a cmov
                a.rex(false, 2, reg);
                a.u8(0x8b);
                a.mem(2, reg, cols[kOtherCol].off);
                for (const unsigned x : {0x41u, 0x39u, 0xd3u, 0x75u, 0x03u, 0x41u, 0x89u, 0xd3u})
                    a.u8(x); // cmp r11d,edx; jne +3; mov r11d,edx
            }
            a.stackStore(11, 0x20 + 8 * static_cast<int32_t>(k - 2));
        }
    size_t intoArgsAt = 0;
    if (mut == Mut::BranchIntoArgs) { // test eax,eax; je <start of the stack arguments>: skips the lea and r8/r9/FILE*
        for (const unsigned x : {0x85u, 0xc0u, 0x0fu, 0x84u})
            a.u8(x);
        intoArgsAt = a.b.size();
        a.i32(0);
    }
    if (mut == Mut::Loop)
        for (const unsigned x : {0x85u, 0xc0u, 0x75u, 0xfcu}) // test eax,eax; jne -4
            a.u8(x);
    if (mut == Mut::IndexedStore)
        for (const unsigned x : {0x89u, 0x44u, 0x8du, 0xc0u}) // mov [rbp+rcx*4-0x40],eax
            a.u8(x);
    if (mut == Mut::SkippableErrorPath) // test eax,eax; je +7; mov ecx,eax; call rel32
        for (const unsigned x : {0x85u, 0xc0u, 0x74u, 0x07u, 0x89u, 0xc1u, 0xe8u, 0u, 0u, 0u, 0u})
            a.u8(x);
    Writer w;
    w.lea = a.b.size();
    a.u8(0x48);
    a.u8(0x8d);
    a.u8(0x15);
    a.i32(0x1234);
    const auto load = [&](unsigned dst, const Col& c) {
        const unsigned base = c.direct ? reg : 5;
        a.rex(false, dst, base);
        a.u8(0x8b);
        a.mem(dst, base, c.direct ? c.off : kStack + c.off);
    };
    load(9, cols[1]);
    load(8, cols[0]);
    a.rex(true, 1, 6);
    a.u8(0x8b);
    a.mem(1, 6, static_cast<int32_t>(kWriterFileOffset));
    if (mut == Mut::BranchIntoArgs) {
        const int32_t rel = static_cast<int32_t>(a.b.size() - (intoArgsAt + 4));
        std::memcpy(a.b.data() + intoArgsAt, &rel, 4);
    }
    for (size_t k = cols.size() - 1; k >= 2; --k)
        if (!cols[k].direct) {
            a.u8(0x8b);
            a.mem(0, 5, kStack + cols[k].off);
            if (mut == Mut::AfterLeaBranch && k == kMutCol) {
                // after the lea: edx = Decode End; if (record flag != 0) eax = edx is SKIPPED, as a branch over a mov
                a.u8(0x8b);
                a.mem(2, 5, kStack + cols[kOtherCol].off);
                a.u8(0x83);
                a.mem(7, 5, kStack + cols[14].off); // cmp dword [rbp+flag],0
                for (const unsigned x : {0u, 0x75u, 0x02u, 0x89u, 0xd0u})
                    a.u8(x); // imm8 0; jne +2; mov eax,edx
            }
            if (mut == Mut::Cmov && k == kMutCol) {
                // eax = Receive; if (eax != edx (Decode End)) eax = edx -- a value-changing cmov
                a.u8(0x8b);
                a.mem(2, 5, kStack + cols[kOtherCol].off);
                for (const unsigned x : {0x39u, 0xd0u, 0x0fu, 0x45u, 0xc2u})
                    a.u8(x); // cmp eax,edx; cmovne eax,edx
            }
            a.stackStore(0, 0x20 + 8 * static_cast<int32_t>(k - 2));
        }
    a.u8(0xe8);
    a.i32(0x100);
    a.u8(0xc3);
    if (mut == Mut::SkippableErrorPath) { // the early-out jumps past everything, the call included
        const int32_t rel = static_cast<int32_t>(a.b.size() - (earlyOutAt + 4));
        std::memcpy(a.b.data() + earlyOutAt, &rel, 4);
    }
    w.code = a.b;
    return w;
}

const char* const kColumnNames[15] = {"Present ID",   "WGP Release", "App Submit",     "Encode Start",
                                      "Encode End",   "Send Start",  "Send End",       "Receive",
                                      "Decode Start", "Decode End",  "Submit Illumination", "Predicted Display",
                                      "Decode Query", "Ping At Send Start", "TX ID"};

/** The 15 columns with record offsets: in column order when shuffle is 0, else scattered (Present ID stays at 0). */
std::vector<Col> csvColumns(uint32_t stride, unsigned shuffle, const std::vector<unsigned>& direct,
                            bool predicted = true) {
    std::vector<Col> cols;
    for (unsigned k = 0; k < 15; ++k) {
        if (!predicted && k == 11)
            continue;
        const int32_t off = k == 0 ? 0
                            : shuffle == 0 ? static_cast<int32_t>(4 * k)
                                           : static_cast<int32_t>(4 * (1 + (k * shuffle) % (stride / 4 - 1)));
        const bool isDirect = std::find(direct.begin(), direct.end(), k) != direct.end();
        cols.push_back({kColumnNames[k], k == 13 || k == 14 ? "%d" : "%u", off, isDirect});
    }
    return cols;
}

int32_t colOffset(const std::vector<Col>& cols, const std::string& name) {
    for (const auto& c : cols)
        if (c.name == name)
            return c.off;
    return -1;
}

struct Derived {
    WriterShape shape;
    DerivedLayout d;
    std::string note;
    bool decoded = false, ok = false;
};

void deriveFrom(Derived& r, uint32_t stride, bool imm8, unsigned reg, const std::vector<Col>& cols,
                Mut mut = Mut::None) {
    const Writer w = emitWriter(stride, imm8, reg, cols, mut);
    std::string header, row;
    for (size_t k = 0; k < cols.size(); ++k) {
        header += (k ? "," : "") + cols[k].name;
        row += (k ? "," : "") + cols[k].spec;
    }
    header += "\n";
    row += "\n";
    r.decoded = decodeCsvWriter(w.code.data(), w.code.size(), w.lea, r.shape);
    r.ok = r.decoded && deriveLayout(header, row, r.shape, r.d, r.note);
}

/** The derived layout against the offsets the fixture chose, then a record built from those offsets (not from the
 *  layout) through parseFrame: the mapping has to be by column name. */
void checkMapped(const Derived& r, const std::vector<Col>& cols, uint32_t stride) {
    CHECK(r.decoded);
    CHECK(r.ok);
    CHECK(r.note.empty());
    if (!r.ok)
        return;
    CHECK(r.shape.stride == stride);
    CHECK(r.shape.fileOffset == kWriterFileOffset);
    CHECK(r.shape.args.size() == cols.size());
    for (size_t k = 0; k < cols.size() && k < r.shape.args.size(); ++k)
        CHECK(r.shape.args[k] == cols[k].off);
    const FrameLayout& l = r.d.layout;
    CHECK(l.stride == stride && l.fileOffset == kWriterFileOffset && l.indexOffset == 0);
    CHECK(std::strcmp(l.name, "derived") == 0);
    CHECK(l.header == r.d.header.c_str() && l.row == r.d.row.c_str());
    static const char* const stampNames[12] = {"WGP Release", "App Submit", "Encode Start", "Encode End",
                                               "Send Start",  "Send End",   "Receive",      "Decode Start",
                                               "Decode End",  "Submit Illumination", "Predicted Display",
                                               "Decode Query"};
    for (size_t j = 0; j < 12; ++j) {
        int32_t expected = colOffset(cols, stampNames[j]);
        if (expected < 0 && j == 10)
            expected = colOffset(cols, "Decode Query"); // Predicted Display absent: falls back
        CHECK(static_cast<int32_t>(l.stamps[j]) == expected);
    }
    CHECK(static_cast<int32_t>(l.pingOffset) == colOffset(cols, "Ping At Send Start"));

    uint32_t t[13] = {};
    baseFields(t);
    t[12] = t[9] + 4000; // Decode Query 4 ms after Decode End
    std::vector<uint8_t> raw(stride, 0);
    const char* const fieldNames[13] = {"Present ID",   "WGP Release", "App Submit",   "Encode Start",
                                        "Encode End",   "Send Start",  "Send End",     "Receive",
                                        "Decode Start", "Decode End",  "Submit Illumination", "Predicted Display",
                                        "Decode Query"};
    for (size_t j = 0; j < 13; ++j) {
        const int32_t off = colOffset(cols, fieldNames[j]);
        if (off >= 0)
            std::memcpy(raw.data() + off, &t[j], 4);
    }
    const int32_t ping = 2470;
    std::memcpy(raw.data() + colOffset(cols, "Ping At Send Start"), &ping, 4);
    Frame f;
    CHECK(parseFrame(raw.data(), f, l));
    CHECK(f.id == t[0] && f.illum == t[10]);
    const unsigned pairs[7][2] = {{1, 2}, {3, 4}, {5, 6}, {6, 7}, {7, 9}, {9, 10}, {1, 10}};
    for (unsigned j = 0; j < 7; ++j)
        CHECK(std::fabs(f.ms[j] - (t[pairs[j][1]] - t[pairs[j][0]]) / 1000.0) < 1e-9);
    CHECK(f.split && std::fabs(f.ms[7] - 4.0) < 1e-9 && std::fabs(f.ms[8] - 32.0) < 1e-9);
    CHECK(std::fabs(f.ping - 2.47) < 1e-9);
}

void testWriterDecodeImm32ShuffledRecordDirectLoads() {
    // imul rdi,rax,0xb0 (imm32 form); fields scattered through the record; Present ID's neighbours, the first stack
    // argument, a register argument and the last stack argument are all read straight from [rdi+disp]
    const auto cols = csvColumns(0xb0, 7, {1, 10, 12});
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols);
    checkMapped(r, cols, 0xb0);
    // the scatter really is scattered (guards against a fixture that happens to be in column order)
    CHECK(colOffset(cols, "Receive") < colOffset(cols, "Send End"));
}

void testWriterDecodeImm8OtherRegister() {
    // imul r10,rax,0x70 (imm8 form, a register that needs REX.R/B); no direct loads
    const auto cols = csvColumns(0x70, 11, {});
    Derived r;
    deriveFrom(r, 0x70, true, 10, cols);
    checkMapped(r, cols, 0x70);
}

void testWriterDecodeInOrderRegisterRbx() {
    const auto cols = csvColumns(0x100, 0, {13});
    Derived r;
    deriveFrom(r, 0x100, false, 3, cols);
    checkMapped(r, cols, 0x100);
}

void testWriterDecodeColumnOrderIsNotFieldOrder() {
    // the header lists the columns backwards (Present ID last), the record keeps its own order: mapping is by name
    auto cols = csvColumns(0xb0, 0, {});
    std::reverse(cols.begin(), cols.end());
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols);
    checkMapped(r, cols, 0xb0);
    CHECK(r.ok && r.d.layout.stamps[0] == 4 && r.d.layout.pingOffset == 52); // WGP at 4*1, Ping at 4*13
}

void testWriterDecodePredictedDisplayAbsent() {
    const auto cols = csvColumns(0xb0, 7, {}, /*predicted=*/false);
    CHECK(colOffset(cols, "Predicted Display") < 0);
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols);
    checkMapped(r, cols, 0xb0); // includes stamps[10] == Decode Query's offset
    CHECK(r.ok && r.d.layout.stamps[10] == r.d.layout.stamps[11]);
}

void testWriterDeriveFailsClosed() {
    // a needed column missing: false, with the column named; the writer itself still decodes
    for (const char* missing : {"Encode End", "Decode Query", "Ping At Send Start", "WGP Release", "Present ID"}) {
        auto cols = csvColumns(0xb0, 7, {});
        cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const Col& c) { return c.name == missing; }),
                   cols.end());
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols);
        CHECK(r.decoded);
        CHECK(!r.ok);
        CHECK(r.note == std::string("no column ") + missing);
    }
    // Present ID not at the record start
    {
        auto cols = csvColumns(0xb0, 7, {});
        cols[0].off = 8;
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols);
        CHECK(r.decoded && !r.ok);
        CHECK(r.note == "Present ID not at record start");
    }
    // a needed column that is not a 32-bit printf field
    {
        auto cols = csvColumns(0xb0, 7, {});
        cols[2].spec = "%llu";
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols);
        CHECK(r.decoded && !r.ok);
        CHECK(r.note == "App Submit not 32-bit");
    }
    // header and row disagree on the column count
    {
        WriterShape shape;
        shape.stride = 0xb0;
        DerivedLayout d;
        std::string note;
        CHECK(!deriveLayout("Present ID,WGP Release\n", "%u\n", shape, d, note));
        CHECK(note == "CSV header/row count mismatch");
    }
}

/** Writer instructions that change a value, or that the decoder cannot follow, must fail the derivation instead of
 *  mapping a column to the slot the value merely started in. */
void testWriterDecodeRejectsValueChangingCode() {
    const auto cols = csvColumns(0xb0, 7, {});
    {   // (a) cmovne between loading Receive and storing it: an opcode outside the decoded subset
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols, Mut::Cmov);
        CHECK(!r.decoded);
        CHECK(!r.ok);
    }
    {   // (c) a backward jump (a loop)
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols, Mut::Loop);
        CHECK(!r.decoded);
    }
    {   // (d) a store through an index register: unknown which slots it changes
        Derived r;
        deriveFrom(r, 0xb0, false, 7, cols, Mut::IndexedStore);
        CHECK(!r.decoded);
    }
}

void testWriterDecodeBranchSelectLosesProvenance() {
    // (b) Receive passes through `cmp; jne +3; mov r11d,edx` before the print: it decodes, but the argument may be
    // either slot, so it maps to nothing and deriveLayout names the column
    const auto cols = csvColumns(0xb0, 7, {7});
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols, Mut::BranchSelect);
    CHECK(r.decoded); // the shape itself is still recognised (stride, FILE*)
    CHECK(r.shape.stride == 0xb0 && r.shape.fileOffset == kWriterFileOffset);
    CHECK(r.shape.args.size() == cols.size());
    CHECK(r.shape.args.size() > 7 && r.shape.args[7] == -1);
    CHECK(r.shape.args.size() > 9 && r.shape.args[9] == cols[9].off); // untouched neighbours still map
    CHECK(!r.ok);
    CHECK(r.note == "Receive no offset");
}

void testWriterDecodeKeepsRealWritersEarlyOutAndErrorPath() {
    // (e) positive control: a forward je past the print at the start, and a skippable `test; je; mov; call` error path
    // before the lea. Neither changes an argument, so the mapping is exact.
    const auto cols = csvColumns(0xb0, 7, {1, 10});
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols, Mut::SkippableErrorPath);
    checkMapped(r, cols, 0xb0);
}

/** A scan that finds conflicting servers keeps the disagreement across the gated ticks, rescans at the gate, and only
 *  accepts a server once a scan finds them agreeing. */
void testScanDisagreementIsSticky() {
    FakeMemory mem;
    setupModule(mem, kKnownBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, 0, 5000);
    setActiveServer(mem, w1.srv);  // code pair -> server 1
    setDataGlobal(mem, 2, w2.srv); // a .data global -> server 2

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store(readerMetricDefs());
    reader.tick(10.0, store); // scan: disagree, next scan at 20
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
    for (const double now : {10.5, 19.9, 20.0, 50.0}) { // gated ticks keep it; the scans at 20 and 50 still see it
        reader.tick(now, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
        CHECK(notOk(store, metric::kPcStreamNamedPing));
        CHECK(reader.serverHow().empty());
    }

    // resolve: the conflicting global now points at the code-pair server. The last scan was at 50, so not before 60.
    setDataGlobal(mem, 2, w1.srv);
    reader.tick(51.0, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
    CHECK(notOk(store, metric::kPcStreamNamedPing));
    reader.tick(59.9, store);
    CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
    reader.tick(60.0, store);
    CHECK(reader.serverHow() == "code pair, RTTI scan agrees (rva 2000)");
    CHECK(l1Live(store));

    // the same, resolved by clearing the conflicting global instead
    FakeMemory mem2;
    setupModule(mem2, kKnownBuild);
    const World v1 = buildWorld(mem2, 0x10000, 300, 0, 1000);
    const World v2 = buildWorld(mem2, 0x300000, 500, 0, 5000);
    setActiveServer(mem2, v1.srv);
    setDataGlobal(mem2, 2, v2.srv);
    Reader reader2(mem2, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store2(readerMetricDefs());
    reader2.tick(10.0, store2);
    reader2.tick(10.5, store2);
    CHECK(noteOf(store2, metric::kPcStreamL1Note) == "server globals disagree");
    setDataGlobal(mem2, 2, 0);
    reader2.tick(19.9, store2);
    CHECK(noteOf(store2, metric::kPcStreamL1Note) == "server globals disagree");
    reader2.tick(20.0, store2);
    CHECK(reader2.serverHow() == "code pair (rva 2000)");
    CHECK(l1Live(store2));

    // two scan hits and no code pair: the gated tick must keep saying "disagree", not fall back to "no server"
    FakeMemory mem3;
    setupModule(mem3, kKnownBuild);
    const World u1 = buildWorld(mem3, 0x10000, 300, 0, 1000);
    const World u2 = buildWorld(mem3, 0x300000, 500, 0, 5000);
    setDataGlobal(mem3, 2, u1.srv);
    setDataGlobal(mem3, 9, u2.srv);
    Reader reader3(mem3, kModuleBase, scanAnchors(kKnownBuild, false));
    MetricStore store3(readerMetricDefs());
    reader3.tick(10.0, store3);
    CHECK(noteOf(store3, metric::kPcStreamL1Note) == "server globals disagree");
    reader3.tick(10.5, store3);
    CHECK(noteOf(store3, metric::kPcStreamL1Note) == "server globals disagree");
    CHECK(notOk(store3, metric::kPcStreamNamedPing));
}

/** Refuses every read that touches the fake .data block, as a failed ReadProcessMemory would. */
class DataRefusingMemory : public FakeMemory {
public:
    bool refuse = false;
    bool read(uint64_t address, void* dest, size_t bytes) override {
        if (refuse && address < kModuleBase + kScanDataRva + kScanDataSize && address + bytes > kModuleBase + kScanDataRva)
            return false;
        return FakeMemory::read(address, dest, bytes);
    }
};

/** A rescan whose .data read fails is no evidence: the disagreement stays, the gate re-arms, nothing is cached. */
void testScanFailedReadKeepsDisagreement() {
    DataRefusingMemory mem;
    setupModule(mem, kKnownBuild);
    const World w1 = buildWorld(mem, 0x10000, 300, 0, 1000);
    const World w2 = buildWorld(mem, 0x300000, 500, 0, 5000);
    setActiveServer(mem, w1.srv);  // code pair -> server 1 (still readable throughout)
    setDataGlobal(mem, 2, w2.srv); // a .data global -> server 2

    Reader reader(mem, kModuleBase, scanAnchors(kKnownBuild, true));
    MetricStore store(readerMetricDefs());
    const auto stillDisagree = [&](double now) {
        reader.tick(now, store);
        CHECK(noteOf(store, metric::kPcStreamL1Note) == "server globals disagree");
        CHECK(notOk(store, metric::kPcStreamNamedPing));
        CHECK(reader.serverHow().empty());
    };
    stillDisagree(10.0); // scan ok: disagree, next scan 20

    mem.refuse = true;
    stillDisagree(20.0); // the scan is due, its read fails: no new evidence, the latch stays
    stillDisagree(20.5); // gated (next scan 30)
    mem.refuse = false;
    stillDisagree(29.9); // still gated
    stillDisagree(30.0); // a good scan, the conflict is still there

    setDataGlobal(mem, 2, w1.srv); // resolved; the scan at 30 re-armed the gate to 40
    stillDisagree(35.0);
    stillDisagree(39.9);
    reader.tick(40.0, store);
    CHECK(reader.serverHow() == "code pair, RTTI scan agrees (rva 2000)");
    CHECK(l1Live(store));
}

/** Branches after the lea: a conditional skip of an assignment on the way to the print makes that argument depend on a
 *  flag, so it must not map to either slot. */
void testWriterDecodeBranchAfterLeaLosesProvenance() {
    const auto cols = csvColumns(0xb0, 7, {});
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols, Mut::AfterLeaBranch);
    CHECK(r.decoded);
    CHECK(r.shape.args.size() == cols.size());
    CHECK(r.shape.args.size() > 7 && r.shape.args[7] == -1);
    CHECK(r.shape.args.size() > 9 && r.shape.args[9] == cols[9].off); // the neighbour it was compared with is intact
    CHECK(!r.ok);
    CHECK(r.note == "Receive no offset");
}

/** A branch before the lea that lands between the lea and the print skips the lea and the register arguments: they
 *  must not map to fields (the stack arguments after the landing point still do). */
void testWriterDecodeBranchIntoArgumentSetup() {
    const auto cols = csvColumns(0xb0, 7, {});
    Derived r;
    deriveFrom(r, 0xb0, false, 7, cols, Mut::BranchIntoArgs);
    CHECK(r.decoded);
    CHECK(r.shape.args.size() == cols.size());
    CHECK(r.shape.args.size() > 1 && r.shape.args[0] == -1 && r.shape.args[1] == -1); // r8, r9 may be skipped
    CHECK(r.shape.args.size() > 2 && r.shape.args[2] == cols[2].off);                  // after the landing point
    CHECK(r.shape.fileOffset == 0);                                                    // the FILE* load may be skipped too
    CHECK(!r.ok);
    CHECK(r.note == "Present ID no offset");
}

// ==================== Reader: write index found at runtime (derived layout) ====================

constexpr uint32_t kIndexRingBytes = kFrameSlots * kFrameStride; // 0x78000: where the u16 hunt starts
constexpr uint32_t kRealIndexAt = kIndexRingBytes + 0x2468;      // from the ring start, nowhere a known layout uses
constexpr uint32_t kDecoyIndexAt = kIndexRingBytes + 0x0400;
constexpr uint64_t kPingNodeValueOffset = 0x90100 + 0x40;        // sPingMS is the first map node

/** A derived layout with the legacy record shape (so frameBytes-style fixtures apply) and no known index offset. */
std::shared_ptr<DerivedLayout> legacyShapedDerived() {
    auto d = std::make_shared<DerivedLayout>();
    d->header = kFrameLayouts[0].header;
    d->row = kFrameLayouts[0].row;
    d->layout = kFrameLayouts[0];
    d->layout.name = "derived";
    d->layout.header = d->header.c_str();
    d->layout.row = d->row.c_str();
    d->layout.indexOffset = 0;
    return d;
}

/** A world at index 300 whose real write index (and decoys) live outside any known layout's spot. */
struct IndexRun {
    FakeMemory mem;
    std::shared_ptr<DerivedLayout> derived;
    World w;
    std::unique_ptr<Reader> reader;
    MetricStore store;
    uint32_t trackerAt = 0; // a second u16 that advances with the real index (ambiguity case)

    explicit IndexRun(uint32_t secondTracker = 0)
        : derived(legacyShapedDerived()), store(readerMetricDefs()), trackerAt(secondTracker) {
        setupModule(mem, kKnownBuild);
        // ring at srv+kLayoutAftmOffset (clear of the server's own fields); leaves a stale 300 at the legacy index
        // spot: a decoy
        w = buildWorldLayout(mem, 0x10000, kFrameLayouts[0], 300, 0, 1000);
        setActiveServer(mem, w.srv);
        mem.poke<uint16_t>(ringAt(kDecoyIndexAt), 300); // equals the expected value but never moves
        mem.poke<uint16_t>(ringAt(kRealIndexAt), 300);
        if (trackerAt)
            mem.poke<uint16_t>(ringAt(trackerAt), 300);
        Anchors a = anchorsForLayout(kKnownBuild, derived->layout);
        a.layout = &derived->layout;
        a.derived = derived;
        a.layoutHow = "derived";
        reader = std::make_unique<Reader>(mem, kModuleBase, a);
    }
    /** The ring moves on to `newIndex` (> 300): the newest record lands at slot newIndex-1, then the index follows. */
    uint64_t ringAt(uint32_t offset) const {
        return w.srv + kLayoutAftmOffset + offset;
    }
    void advance(uint32_t newIndex) {
        const uint32_t slot = newIndex - 1, i = slot - 156; // the world wrote slots 156..299
        uint32_t t[13] = {};
        baseFields(t, i * gFixtureStepUs, 1000 + i);
        const auto raw = frameBytesForLayout(kFrameLayouts[0], t, 2000);
        mem.write(ringAt(slot * kFrameStride), raw.data(), raw.size());
        mem.poke<uint16_t>(ringAt(kRealIndexAt), static_cast<uint16_t>(newIndex));
        if (trackerAt)
            mem.poke<uint16_t>(ringAt(trackerAt), static_cast<uint16_t>(newIndex));
    }
    void tick(double now) {
        reader->tick(now, store);
    }
    std::string l3() const {
        return noteOf(store, metric::kPcStreamL3Note);
    }
};

void testIndexFinderPicksTheMovingIndexNotTheDecoy() {
    IndexRun r;
    r.tick(10.0); // candidates collected: the real index and both decoys all read 300
    CHECK(r.l3() == "finding write index");
    CHECK(r.reader->indexOffset() == 0);
    CHECK(notOk(r.store, metric::kPcStreamFps));
    CHECK(noteOf(r.store, metric::kPcStreamL1Note).empty() && noteOf(r.store, metric::kPcStreamL2Note).empty());

    r.advance(301);
    r.tick(10.5); // expected value changed: only the moving one survives
    CHECK(r.l3() == "write index found");
    CHECK(r.reader->indexOffset() == kRealIndexAt);
    CHECK(notOk(r.store, metric::kPcStreamFps)); // the finding tick itself publishes nothing

    r.tick(11.0); // first normal tick on the found index
    CHECK(r.l3().empty());
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(isOkF32(r.store, metric::kPcStreamEncodeAvg, 2.6, 1e-6));
    CHECK(r.reader->indexOffset() == kRealIndexAt);

    r.advance(302);
    r.tick(11.5);
    CHECK(r.l3().empty());
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6));
    CHECK(r.reader->resets() == 0);
}

void testIndexFinderAmbiguousStaysUnknown() {
    IndexRun r(/*secondTracker=*/kRealIndexAt + 0x100); // two u16s advance in lockstep
    r.tick(10.0);
    CHECK(r.l3() == "finding write index");
    r.advance(301);
    r.tick(10.5); // two survivors: no pick
    CHECK(r.l3() == "finding write index");
    CHECK(r.reader->indexOffset() == 0);
    r.advance(302);
    r.tick(11.0);
    CHECK(r.l3() == "finding write index");
    CHECK(r.reader->indexOffset() == 0);
    CHECK(notOk(r.store, metric::kPcStreamFps));
}

void testIndexFinderResetsWhenCrossCheckFails() {
    IndexRun r;
    r.tick(10.0);
    r.advance(301);
    r.tick(10.5);
    CHECK(r.l3() == "write index found");
    CHECK(r.reader->indexOffset() == kRealIndexAt);

    // the first tick on the found index runs the cross-check: a sPingMS the frames disagree with fails it
    r.mem.poke<double>(r.w.srv + kPingNodeValueOffset, 9.0);
    r.tick(11.0);
    CHECK(r.l3() == "frame/bandwidth cross-check");
    CHECK(r.reader->indexOffset() == 0);
    CHECK(notOk(r.store, metric::kPcStreamFps));
}

void testIndexFinderResetsWhenWindowEmpty() {
    IndexRun r;
    r.tick(10.0);
    r.advance(301);
    r.tick(10.5);
    r.tick(11.0);
    CHECK(r.l3().empty());
    CHECK(isOkF32(r.store, metric::kPcStreamFps, kFixtureRecentCount, 1e-6)); // live on the found index
    CHECK(r.reader->indexOffset() == kRealIndexAt);

    // the index now points at an empty part of the ring (a forward jump under 2048, so not an owner change)
    r.mem.poke<uint16_t>(r.ringAt(kRealIndexAt), 1802);
    r.tick(13.0);
    CHECK(r.reader->resets() == 0);
    CHECK(r.l3() == "no frames");
    CHECK(r.reader->indexOffset() == 0);
    CHECK(notOk(r.store, metric::kPcStreamFps));

    r.tick(13.5); // hunting again
    CHECK(r.l3() == "finding write index");
}

} // namespace

int main() {
    testParseFrameFieldMapping();
    testParseFrameIncomplete();
    testParseFrameBounds();
    testParseFrameU32Wrap();
    testParseFrameHoldDisplay();
    testParseFrameNoDecodeQuery();
    testParseFrameNegativeHold();
    testParseFrameHoldDisplayU32Wrap();
    testFrameLayoutTableValues();
    testParseFrameFieldMappingGpuStamps();
    testParseFrameIncompleteGpuStamps();
    testParseFrameBoundsGpuStamps();
    testParseFrameU32WrapGpuStamps();
    testFrameWindowDedupeAndLateCompletion();
    testFrameWindowWrongSizeIgnored();
    testFrameWindowStallReadsAsNoFrames();
    testFrameWindowInteriorLateCompletion();
    testFrameWindowInteriorLateCompletionWraps();
    testReaderGoldenPath();
    testReaderPublishesHoldDisplay();
    testReaderHeaderMismatchOnlyL3();
    testReaderRttiMismatchOnlyL2();
    testReaderOwnerChangeWaitsForNewBaseline();
    testReaderL2RecoversIndependentlyOfDisabledL3();
    testReaderRewalksWhenStatsMapGrows();
    testReaderIdleAudioAboveWireKeepsL3();
    testReaderNonAudioAboveWireFailsL3();
    testReaderL3Valid90HzStream();
    testReaderGpuStampsGoldenPathWithRingWrap();
    for (const auto& layout : kFrameLayouts) {
        runEncodeOscillationPassesAtPeriodicCheck(layout);
        runEncodeConsistentOffsetFailsAtPeriodicCheck(layout);
        runStaleEncodeSamplesExpire(layout);
        runOwnerChangeClearsEncodeSamples(layout);
    }
    testReadLinkEndpointValid();
    testReadLinkEndpointGetterShapeChanged();
    testReadLinkEndpointBadFamily();
    testReadLinkEndpointZeroPort();
    testReadLinkEndpointInsaneIp();
    testReadLinkEndpointWrongObjectType();
    testReaderEndpointSuccessPopulatesAddress();
    testReaderEndpointFailureDoesNotAffectL2Timing();
    testScanAgreeingGlobalsPublish();
    testScanDisagreeingGlobalsFailL1();
    testScanNoHitFailsL1();
    testScanStalePointerRecoversAfterGate();
    testScanEmptyScanRearmsGate();
    testScanAgreesWithCodePair();
    testScanDisagreesWithCodePair();
    testCodePairAloneWithEmptyScanArea();
    testCodePairStaleFallsBackToScan();
    testFindAnchorsCodePairWithoutGapRule();
    testWriterDecodeImm32ShuffledRecordDirectLoads();
    testWriterDecodeImm8OtherRegister();
    testWriterDecodeInOrderRegisterRbx();
    testWriterDecodeColumnOrderIsNotFieldOrder();
    testWriterDecodePredictedDisplayAbsent();
    testWriterDeriveFailsClosed();
    testWriterDecodeRejectsValueChangingCode();
    testWriterDecodeBranchSelectLosesProvenance();
    testWriterDecodeKeepsRealWritersEarlyOutAndErrorPath();
    testScanDisagreementIsSticky();
    testScanFailedReadKeepsDisagreement();
    testWriterDecodeBranchAfterLeaLosesProvenance();
    testWriterDecodeBranchIntoArgumentSetup();
    testIndexFinderPicksTheMovingIndexNotTheDecoy();
    testIndexFinderAmbiguousStaysUnknown();
    testIndexFinderResetsWhenCrossCheckFails();
    testIndexFinderResetsWhenWindowEmpty();
    if (failures == 0) {
        std::printf("pc streaming reader: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
