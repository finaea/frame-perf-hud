// T10 headset/camera_eye_ring: eye-frame heuristics, frame-start search, ring discovery ("ring is live" check) and
// the tear-guarded RingReader, on synthetic arenas. Plain C++, no cache maintenance.
#include "camera_eye_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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

using namespace eyering;
using Bytes = std::vector<uint8_t>;

constexpr size_t kPitch = 262208;  // the real slot spacing on Jack's Frame: 512 KiB / 2 + 64
constexpr size_t kOrigin = 262144;
constexpr int kLive = 8;
constexpr size_t kArena = 5u * 1024 * 1024;

uint32_t gRng = 12345;
uint32_t rnd() {
    gRng = gRng * 1664525u + 1013904223u;
    return gRng >> 8;
}

/**
 * One fake eye frame (512x400): a bright disc (radius 140, ~30% of the pixels) on a dim but not dark background
 * (every row stays lit enough that the frame doesn't look like it starts mid-way), mild noise.
 * @param cx the disc's centre column: the eye's identity (left or right eye)
 * @param bg background level
 */
void paintFrame(uint8_t* dst, int cx, int bg) {
    for (int y = 0; y < kEyeHeight; ++y) {
        for (int x = 0; x < kEyeStride; ++x) {
            const int dx = x - cx, dy = y - 200;
            int v;
            if (dx * dx + dy * dy <= 140 * 140)
                v = 110 + static_cast<int>(rnd() % 7) - 3;
            else
                v = bg + static_cast<int>(rnd() % 3);
            dst[static_cast<size_t>(y) * kEyeStride + x] = static_cast<uint8_t>(v);
        }
    }
}

/** Rewrites a frame the way the camera refreshing it would: the sampled fingerprints change, the picture holds. */
void refresh(uint8_t* frame) {
    for (size_t i = 0; i < kEyeFrameBytes; i += 7) frame[i] ^= 1;
}

struct Synth {
    Bytes mem;
    int eyeCx[kLive];
    Synth() : mem(kArena, 0) {}
    uint8_t* slot(int k) { return mem.data() + kOrigin + static_cast<size_t>(k) * kPitch; }
    Arena arena() const { return Arena {mem.data(), mem.size()}; }
    void refreshLive() {
        for (int k = 0; k < kLive; ++k) refresh(slot(k));
    }
};

/**
 * 8 live slots (0-3 one eye, 4-7 another), a stale picture exactly where a 9th slot would be (the forward walk runs
 * onto it), and a stale pair far away. In the two-eye ring slot 1 has the darkest background so the reference slot (darkest frame edges)
 * is one whose successor shows the same eye: the spacing measurement compares the reference with its next slot.
 */
void build(Synth& s, bool twoEyes = true) {
    for (int k = 0; k < kLive; ++k) {
        const int cx = (k < 4 || !twoEyes) ? 170 : 340;
        paintFrame(s.slot(k), cx, (twoEyes && k == 1) ? 7 : 9);
    }
    paintFrame(s.slot(kLive), 340, 9);  // stale: one past the ring
    paintFrame(s.mem.data() + kOrigin + 12 * kPitch, 170, 9);
    paintFrame(s.mem.data() + kOrigin + 13 * kPitch, 170, 9);
}

double gNow = 0.0;
double fakeNow() { return gNow; }

struct Calls {
    int count = 0;
};

// ---------------------------------------------------------------------------------------------- heuristics

void testFingerprint() {
    Bytes a(kEyeFrameBytes, 1);
    const Cache none;
    const uint64_t f0 = fingerprint(a.data(), a.size(), none);
    CHECK(fingerprint(a.data(), a.size(), none) == f0);
    a[0] = 2;  // sampled (i = 0)
    CHECK(fingerprint(a.data(), a.size(), none) != f0);
    a[0] = 1;
    CHECK(fingerprint(a.data(), a.size(), none) == f0);
    a[1] = 9;  // between samples (step 400): not seen
    CHECK(fingerprint(a.data(), a.size(), none) == f0);
    // more samples see it eventually: step 1 sees every byte
    CHECK(fingerprint(a.data(), a.size(), none, a.size()) != fingerprint(Bytes(kEyeFrameBytes, 1).data(), a.size(), none, a.size()));

    // cache maintenance: dropLine for every sampled line, one barrier
    static int lines = 0, barriers = 0;
    Cache c;
    c.dropLine = [](const void*) { ++lines; };
    c.barrier = [] { ++barriers; };
    lines = barriers = 0;
    (void)fingerprint(a.data(), a.size(), c);
    CHECK(lines == 512 && barriers == 1);
    lines = 0;
    (void)fingerprint(a.data(), a.size(), c, 128);
    CHECK(lines == 128);
}

void testLooksLikeEyeFrame() {
    gRng = 1;
    Bytes left(kEyeFrameBytes), right(kEyeFrameBytes);
    paintFrame(left.data(), 170, 9);
    paintFrame(right.data(), 340, 9);
    CHECK(looksLikeEyeFrame(left.data()));
    CHECK(looksLikeEyeFrame(right.data()));

    // mostly black with a band of noise: lit fraction far under 0.20
    Bytes band(kEyeFrameBytes, 0);
    for (int y = 150; y < 175; ++y)
        for (int x = 0; x < kEyeStride; ++x) band[static_cast<size_t>(y) * kEyeStride + x] = static_cast<uint8_t>(rnd());
    CHECK(!looksLikeEyeFrame(band.data()));

    // pure noise: lit fraction ~0.93 but roughness ~85
    Bytes noise(kEyeFrameBytes);
    for (uint8_t& b : noise) b = static_cast<uint8_t>(rnd());
    CHECK(!looksLikeEyeFrame(noise.data()));

    // all black; all bright (lit fraction 1.0 > 0.96)
    CHECK(!looksLikeEyeFrame(Bytes(kEyeFrameBytes, 0).data()));
    CHECK(!looksLikeEyeFrame(Bytes(kEyeFrameBytes, 200).data()));
    // a smooth half-lit frame passes
    Bytes half(kEyeFrameBytes, 0);
    std::memset(half.data(), 120, kEyeFrameBytes / 2);
    CHECK(looksLikeEyeFrame(half.data()));
}

void testFindFrameStart() {
    gRng = 2;
    const size_t rows = 1001;
    Bytes buf(rows * kEyeStride, 0);
    // rows 0-19 dark, 20-419 frame 1, 420-459 dark, 460-859 frame 2, 860-1000 dark
    paintFrame(&buf[20 * kEyeStride], 170, 9);
    paintFrame(&buf[460 * kEyeStride], 340, 9);
    size_t out = 0;
    CHECK(findFrameStart(buf.data(), buf.size(), 200 * kEyeStride + 5, &out) && out == 20u * kEyeStride);
    CHECK(findFrameStart(buf.data(), buf.size(), 700 * kEyeStride, &out) && out == 460u * kEyeStride);
    CHECK(findFrameStart(buf.data(), buf.size(), 440 * kEyeStride, &out) && out == 460u * kEyeStride);  // nearest
    CHECK(findFrameStart(buf.data(), buf.size(), 30 * kEyeStride, &out) && out == 20u * kEyeStride);
    CHECK(findFrameStart(buf.data(), buf.size(), 400 * kEyeStride, &out) && out == 460u * kEyeStride);  // 60 vs 380

    // no dark run (uniform lit rows), all dark, and a window under 200 rows: nothing
    CHECK(!findFrameStart(Bytes(rows * kEyeStride, 100).data(), rows * kEyeStride, 500 * kEyeStride, &out));
    CHECK(!findFrameStart(Bytes(rows * kEyeStride, 0).data(), rows * kEyeStride, 500 * kEyeStride, &out));
    CHECK(!findFrameStart(buf.data(), 100 * kEyeStride, 50 * kEyeStride, &out));
    // a dark run shorter than 8 rows isn't a frame boundary
    Bytes shortRun(rows * kEyeStride, 100);
    std::memset(&shortRun[300 * kEyeStride], 0, 7 * kEyeStride);
    CHECK(!findFrameStart(shortRun.data(), shortRun.size(), 300 * kEyeStride, &out));
    std::memset(&shortRun[300 * kEyeStride], 0, 8 * kEyeStride);
    CHECK(findFrameStart(shortRun.data(), shortRun.size(), 300 * kEyeStride, &out) && out == 308u * kEyeStride);
}

// ---------------------------------------------------------------------------------------------- discovery

void primeFinder(EyeRingFinder& f, Synth& s) {
    f.setArenas({s.arena()});
    f.resetBlockChanges();  // baseline: only changes from now on count
    s.refreshLive();
    refresh(s.slot(kLive));
    refresh(s.mem.data() + kOrigin + 12 * kPitch);
    refresh(s.mem.data() + kOrigin + 13 * kPitch);
    f.noteBlockChanges();
}

void testDiscover() {
    gRng = 3;
    Synth s;
    build(s);
    EyeRingFinder f;
    primeFinder(f, s);

    gNow = 100.0;
    Calls calls;
    Ring ring;
    const bool ok = f.discover(
        ring,
        [&] {
            ++calls.count;
            gNow += 0.004;
            s.refreshLive();  // the live slots keep refreshing; the stale pictures never do
            return true;
        },
        fakeNow);
    CHECK(ok);
    CHECK(calls.count > 100);  // ~1.6 s of waits at 4 ms
    if (!ok) return;

    CHECK(ring.arena == 0);
    CHECK(ring.slots == kLive);  // the stale picture one past the end is walked onto and dropped
    CHECK(ring.pitch == static_cast<long>(kPitch));
    CHECK(ring.bothEyes);
    int eye0 = 0, eye1 = 0;
    for (int k = 0; k < ring.slots; ++k) (ring.eye[k] ? eye1 : eye0)++;
    CHECK(eye0 == 4 && eye1 == 4);
    for (int k = 1; k < 4; ++k) CHECK(ring.eye[k] == ring.eye[0]);
    for (int k = 5; k < 8; ++k) CHECK(ring.eye[k] == ring.eye[4]);
    CHECK(ring.eye[0] != ring.eye[4]);
    CHECK(ring.eye[0] == 0 && ring.eye[4] == 1);  // eye 0 is the one further left in the image
    for (int k = 1; k < ring.slots; ++k) CHECK(static_cast<long>(ring.off[k] - ring.off[k - 1]) == ring.pitch);
    // the ring lies on the live slots only: it starts at the first slot (+ at most a row of alignment) and ends
    // before the stale picture
    CHECK(ring.off[0] >= kOrigin - kEyeStride && ring.off[0] <= kOrigin + 2 * kEyeStride);
    CHECK(ring.off[ring.slots - 1] < kOrigin + kLive * kPitch);
    for (int k = 0; k < ring.slots; ++k) {
        CHECK(ring.off[k] + kEyeFrameBytes <= s.mem.size());
        CHECK(ring.fp[k] != 0);
    }
}

void testDiscoverOneEye() {
    gRng = 4;
    Synth s;
    build(s, false);
    EyeRingFinder f;
    primeFinder(f, s);
    gNow = 10.0;
    Ring ring;
    const bool ok = f.discover(ring, [&] { gNow += 0.004; s.refreshLive(); return true; }, fakeNow);
    CHECK(ok);
    if (!ok) return;
    CHECK(ring.slots == kLive);
    CHECK(!ring.bothEyes);
    for (int k = 0; k < ring.slots; ++k) CHECK(ring.eye[k] == 0);
}

void testDiscoverStaleOnly() {
    gRng = 5;
    Synth s;
    build(s);
    EyeRingFinder f;
    primeFinder(f, s);
    gNow = 0.0;
    Ring ring;
    // picture-like regions that changed once but are never refreshed: not a live ring
    CHECK(!f.discover(ring, [&] { gNow += 0.004; return true; }, fakeNow));
    CHECK(gNow > 0.9);  // it did sit through the liveness window
}

void testDiscoverAbort() {
    gRng = 6;
    Synth s;
    build(s);
    EyeRingFinder f;
    primeFinder(f, s);
    gNow = 0.0;
    Calls calls;
    Ring ring;
    CHECK(!f.discover(ring, [&] { ++calls.count; gNow += 0.004; s.refreshLive(); return false; }, fakeNow));
    CHECK(calls.count == 1);

    // abort in the second phase (after the liveness window passed)
    primeFinder(f, s);
    gNow = 0.0;
    calls.count = 0;
    CHECK(!f.discover(ring,
                      [&] {
                          ++calls.count;
                          gNow += 0.004;
                          s.refreshLive();
                          return gNow < 1.2;  // ok through the 1 s window, aborts in the 0.6 s one
                      },
                      fakeNow));
    CHECK(calls.count > 250 && calls.count < 320);
}

void testDiscoverNothing() {
    EyeRingFinder f;
    Ring ring;
    gNow = 0.0;
    CHECK(!f.discover(ring, [] { return true; }, fakeNow));  // no arenas

    Synth s;  // all zeros
    f.setArenas({s.arena()});
    f.resetBlockChanges();
    s.mem[1000] = 5;
    f.noteBlockChanges();
    CHECK(!f.discover(ring, [] { return true; }, fakeNow));  // changed memory, but no pictures

    // pictures that didn't change since the baseline aren't candidates
    gRng = 7;
    Synth t;
    build(t);
    EyeRingFinder g;
    g.setArenas({t.arena()});
    g.resetBlockChanges();
    g.noteBlockChanges();  // nothing changed
    CHECK(!g.discover(ring, [] { return true; }, fakeNow));
}

// ---------------------------------------------------------------------------------------------- reader

Ring manualRing() {
    Ring r;
    r.arena = 0;
    r.slots = 4;
    r.pitch = static_cast<long>(kPitch);
    for (int k = 0; k < 4; ++k) {
        r.off[k] = kOrigin + static_cast<size_t>(k) * kPitch;
        r.eye[k] = k < 2 ? 0 : 1;
    }
    r.bothEyes = true;
    return r;
}

void testRingReader() {
    gRng = 8;
    Synth s;
    for (int k = 0; k < 4; ++k) paintFrame(s.slot(k), k < 2 ? 170 : 340, 9);
    const Ring ring = manualRing();
    RingReader reader;
    CHECK(!reader.locked());
    int settled[2];
    reader.poll(0.0, settled);
    CHECK(settled[0] == -1 && settled[1] == -1);

    reader.lock(ring, s.arena(), 1.0);
    CHECK(reader.locked());
    CHECK(reader.ring().slots == 4);
    reader.poll(1.1, settled);
    CHECK(settled[0] == -1 && settled[1] == -1);
    CHECK(reader.refreshes() == 0);

    // slot 1 rewritten: fresh on this poll, settled only after one quiet poll
    refresh(s.slot(1));
    reader.poll(1.2, settled);
    CHECK(settled[0] == -1 && settled[1] == -1);
    CHECK(reader.refreshes() == 1);
    CHECK(reader.lastChange() == 1.2);
    reader.poll(1.3, settled);
    CHECK(settled[0] == 1 && settled[1] == -1);
    CHECK(reader.lastChange() == 1.2);  // that quiet poll isn't a change
    reader.poll(1.4, settled);
    CHECK(settled[0] == -1 && settled[1] == -1);  // reported once

    // a slot still being written (changes on two polls running) isn't settled until it holds still
    refresh(s.slot(2));
    reader.poll(1.5, settled);
    refresh(s.slot(2));
    reader.poll(1.6, settled);
    CHECK(settled[1] == -1);
    reader.poll(1.7, settled);
    CHECK(settled[1] == 2 && settled[0] == -1);

    // both eyes settle on one poll: one slot per eye
    refresh(s.slot(0));
    refresh(s.slot(3));
    reader.poll(1.8, settled);
    reader.poll(1.9, settled);
    CHECK(settled[0] == 0 && settled[1] == 3);

    // two slots of the same eye settling together: the later slot in ring order wins
    refresh(s.slot(0));
    refresh(s.slot(1));
    reader.poll(2.0, settled);
    reader.poll(2.1, settled);
    CHECK(settled[0] == 1);

    // copy of a settled slot: exact, and flipV reverses the rows
    refresh(s.slot(1));
    reader.poll(2.2, settled);
    reader.poll(2.3, settled);
    CHECK(settled[0] == 1);
    Bytes dst(kEyeFrameBytes, 0);
    CHECK(reader.copySlot(1, dst.data(), false));
    CHECK(std::memcmp(dst.data(), s.slot(1), kEyeFrameBytes) == 0);
    Bytes flipped(kEyeFrameBytes, 0);
    CHECK(reader.copySlot(1, flipped.data(), true));
    bool flipOk = true;
    for (int y = 0; y < kEyeHeight && flipOk; ++y)
        if (std::memcmp(&flipped[static_cast<size_t>(kEyeHeight - 1 - y) * kEyeStride], s.slot(1) + static_cast<size_t>(y) * kEyeStride,
                        kEyeStride) != 0)
            flipOk = false;
    CHECK(flipOk);

    // bounds
    CHECK(!reader.copySlot(-1, dst.data(), false));
    CHECK(!reader.copySlot(4, dst.data(), false));

    reader.unlock();
    CHECK(!reader.locked());
    CHECK(!reader.copySlot(1, dst.data(), false));
    reader.poll(3.0, settled);
    CHECK(settled[0] == -1 && settled[1] == -1);
}

void testTornCopy() {
    gRng = 9;
    Synth s;
    for (int k = 0; k < 4; ++k) paintFrame(s.slot(k), k < 2 ? 170 : 340, 9);
    RingReader reader;
    reader.lock(manualRing(), s.arena(), 0.0);
    int settled[2];
    refresh(s.slot(2));
    reader.poll(0.1, settled);
    reader.poll(0.2, settled);
    CHECK(settled[1] == 2);

    // the camera rewrites the slot between the settled poll and the copy: the copy's re-fingerprint disagrees
    s.slot(2)[0] ^= 0x40;  // a sampled byte (offset 0)
    Bytes dst(kEyeFrameBytes, 0);
    CHECK(!reader.copySlot(2, dst.data(), false));

    // the slot is reported again (after holding still for a poll) and then copies fine
    int got = -1;
    for (int i = 0; i < 3 && got < 0; ++i) {
        reader.poll(0.3 + 0.1 * i, settled);
        got = settled[1];
    }
    CHECK(got == 2);
    CHECK(reader.copySlot(2, dst.data(), false));
    CHECK(std::memcmp(dst.data(), s.slot(2), kEyeFrameBytes) == 0);
    CHECK(reader.refreshes() >= 1);

    // the same tear with flipV
    refresh(s.slot(3));
    reader.poll(1.0, settled);
    reader.poll(1.1, settled);
    CHECK(settled[1] == 3);
    s.slot(3)[1600] ^= 0x40;  // a sampled byte (step 1600)
    CHECK(!reader.copySlot(3, dst.data(), true));
}

static int gDropRange = 0;
static size_t gDropLen = 0;
void dropRange(const void*, size_t len) {
    ++gDropRange;
    gDropLen = len;
}

void testReaderCacheHook() {
    gRng = 10;
    Synth s;
    for (int k = 0; k < 4; ++k) paintFrame(s.slot(k), 170, 9);
    RingReader reader(Cache {}, dropRange);
    reader.lock(manualRing(), s.arena(), 0.0);
    Bytes dst(kEyeFrameBytes);
    CHECK(reader.copySlot(0, dst.data(), false));
    CHECK(gDropRange == 1 && gDropLen == kEyeFrameBytes);
}

}  // namespace

int main() {
    testFingerprint();
    testLooksLikeEyeFrame();
    testFindFrameStart();
    testDiscover();
    testDiscoverOneEye();
    testDiscoverStaleOnly();
    testDiscoverAbort();
    testDiscoverNothing();
    testRingReader();
    testTornCopy();
    testReaderCacheHook();
    if (failures == 0) std::printf("camera_eye_ring_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
