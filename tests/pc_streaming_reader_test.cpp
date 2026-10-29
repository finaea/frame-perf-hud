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
    testReadLinkEndpointValid();
    testReadLinkEndpointGetterShapeChanged();
    testReadLinkEndpointBadFamily();
    testReadLinkEndpointZeroPort();
    testReadLinkEndpointInsaneIp();
    testReadLinkEndpointWrongObjectType();
    testReaderEndpointSuccessPopulatesAddress();
    testReaderEndpointFailureDoesNotAffectL2Timing();
    if (failures == 0) {
        std::printf("pc streaming reader: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
