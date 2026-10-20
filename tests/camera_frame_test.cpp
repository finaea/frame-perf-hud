// T10 core/camera_frame: luma copy, dark-parity filter, frame pick, recheck gate, rate gate, tile geometry, BLOB
// payload. Platform-free.
#include "camera_frame.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
using namespace fph::cam;

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kSentinel = 0xEE;

/** A pitched plane whose pixels never equal the sentinel; the padding is all sentinel. */
Bytes makePlane(int w, int h, size_t pitch) {
    Bytes src(pitch * static_cast<size_t>(h), kSentinel);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) src[static_cast<size_t>(y) * pitch + x] = static_cast<uint8_t>((x + y * 3) % 200);
    return src;
}

uint8_t pix(int x, int y) { return static_cast<uint8_t>((x + y * 3) % 200); }

// ---------------------------------------------------------------------------------------------- copyLuma

void testCopyPitch() {
    const int w = 1056, h = 1024;
    const size_t pitch = 1152;
    const Bytes src = makePlane(w, h, pitch);
    Bytes dst(static_cast<size_t>(w) * h, 0);
    copyLuma(dst.data(), src.data(), w, h, pitch, 1, false);
    bool exact = true, leak = false;
    for (int y = 0; y < h && exact; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = dst[static_cast<size_t>(y) * w + x];
            if (v == kSentinel) leak = true;
            if (v != pix(x, y)) {
                exact = false;
                break;
            }
        }
    CHECK(exact);
    CHECK(!leak);

    // flipV: row y of the source is row h-1-y of the output
    Bytes flipped(static_cast<size_t>(w) * h, 0);
    copyLuma(flipped.data(), src.data(), w, h, pitch, 1, true);
    bool flipOk = true;
    for (int y = 0; y < h && flipOk; ++y)
        for (int x = 0; x < w; ++x)
            if (flipped[static_cast<size_t>(h - 1 - y) * w + x] != pix(x, y)) {
                flipOk = false;
                break;
            }
    CHECK(flipOk);
}

void testCopyDownscale() {
    const int w = 1056, h = 1024;
    const size_t pitch = 1152;
    const Bytes src = makePlane(w, h, pitch);
    const int ow = w / 2, oh = h / 2;
    Bytes dst(static_cast<size_t>(ow) * oh, 0);
    copyLuma(dst.data(), src.data(), w, h, pitch, 2, false);
    bool ok = true, leak = false;
    for (int y = 0; y < oh && ok; ++y)
        for (int x = 0; x < ow; ++x) {
            const int sum = pix(2 * x, 2 * y) + pix(2 * x + 1, 2 * y) + pix(2 * x, 2 * y + 1) + pix(2 * x + 1, 2 * y + 1);
            const uint8_t v = dst[static_cast<size_t>(y) * ow + x];
            if (v == kSentinel) leak = true;
            if (v != static_cast<uint8_t>((sum + 2) / 4)) {
                ok = false;
                break;
            }
        }
    CHECK(ok);
    CHECK(!leak);

    Bytes flipped(static_cast<size_t>(ow) * oh, 0);
    copyLuma(flipped.data(), src.data(), w, h, pitch, 2, true);
    bool flipOk = true;
    for (int y = 0; y < oh && flipOk; ++y)
        for (int x = 0; x < ow; ++x)
            if (flipped[static_cast<size_t>(oh - 1 - y) * ow + x] != dst[static_cast<size_t>(y) * ow + x]) {
                flipOk = false;
                break;
            }
    CHECK(flipOk);
}

void testDownscaleRounding() {
    // 2x2 blocks: {0,0,0,1} -> 0 (0.25), {0,0,1,1} -> 1 (0.5 rounds up), {1,1,1,0} -> 1, {255 x4} -> 255,
    // {255,255,255,254} -> 255 (254.75), {0,0,0,255}->64 (63.75)
    const uint8_t src[2 * 12] = {0, 0, 0, 0, 1, 1, 255, 255, 255, 255, 0,   0,    //
                                 0, 1, 1, 1, 1, 0, 255, 255, 254, 255, 255, 255};
    // columns pairs: (0,0|0,1) (0,0|1,1) (1,1|1,0) (255,255|255,255) (255,255|254,255) (0,0|255,255)
    uint8_t dst[6] = {};
    copyLuma(dst, src, 12, 2, 12, 2, false);
    CHECK(dst[0] == 0);
    CHECK(dst[1] == 1);
    CHECK(dst[2] == 1);
    CHECK(dst[3] == 255);
    CHECK(dst[4] == 255);
    CHECK(dst[5] == 128);  // (0+0+255+255+2)>>2 = 128

    const uint8_t src2[4] = {0, 0, 0, 255};
    uint8_t d2 = 0;
    copyLuma(&d2, src2, 2, 2, 2, 2, false);
    CHECK(d2 == 64);  // (255+2)>>2 = 64
}

size_t gDropLen = 0;
const void* gDropPtr = nullptr;
int gDropCalls = 0;
void fakeDrop(const void* p, size_t len) {
    gDropPtr = p;
    gDropLen = len;
    ++gDropCalls;
}

void testCacheOps() {
    const Bytes src = makePlane(64, 8, 80);
    Bytes dst(64 * 8);
    CacheOps ops;
    ops.dropRange = fakeDrop;
    gDropCalls = 0;
    copyLuma(dst.data(), src.data(), 64, 8, 80, 1, false, ops);
    CHECK(gDropCalls == 1);
    CHECK(gDropPtr == src.data());
    CHECK(gDropLen == 80u * 7 + 64);  // exactly the rows' extent, no padding past the last row

    gDropCalls = 0;
    (void)sampleLevel(src.data(), 64, 8, 80, ops, 33, 4);
    CHECK(gDropCalls == 2);  // rows 0 and 4
}

void testSampleLevel() {
    Bytes flat(100 * 100, 77);
    CHECK(std::fabs(sampleLevel(flat.data(), 100, 100, 100) - 77.0) < 1e-9);
    // padding must not be sampled: width 40 pitch 100, padding 255
    Bytes padded(100 * 64, 255);
    for (int y = 0; y < 64; ++y) std::memset(&padded[static_cast<size_t>(y) * 100], 10, 40);
    CHECK(std::fabs(sampleLevel(padded.data(), 40, 64, 100, {}, 3, 2) - 10.0) < 1e-9);
    CHECK(sampleLevel(flat.data(), 0, 100, 100) == 0.0);
    // steps below 1 are clamped
    CHECK(std::fabs(sampleLevel(flat.data(), 10, 10, 100, {}, 0, 0) - 77.0) < 1e-9);
}

void testGreyToRgba() {
    const uint8_t g[3] = {0, 100, 255};
    uint8_t out[12] = {};
    greyToRgba(out, g, 3);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255);
    CHECK(out[4] == 100 && out[5] == 100 && out[6] == 100 && out[7] == 255);
    CHECK(out[8] == 255 && out[11] == 255);
}

// ---------------------------------------------------------------------------------------------- dark filter

void testDarkParity() {
    DarkParityFilter f;
    CHECK(!f.known());
    CHECK(!f.isDark(0) && !f.isDark(1));
    f.observe(10, 55.0);  // even only
    CHECK(!f.known());
    CHECK(!f.isDark(10) && !f.isDark(11));
    f.observe(11, 0.5);  // odd -> now known: odd is dark
    CHECK(f.known());
    CHECK(f.isDark(11) && f.isDark(1) && f.isDark(0xFFFFFFFFu));
    CHECK(!f.isDark(10) && !f.isDark(0) && !f.isDark(0xFFFFFFFEu));
    CHECK(std::fabs(f.level(0) - 55.0) < 1e-9);
    CHECK(std::fabs(f.level(1) - 0.5) < 1e-9);

    // the second camera has the opposite parity
    DarkParityFilter g;
    g.observe(4, 0.5);
    g.observe(5, 55.0);
    CHECK(g.isDark(4) && g.isDark(6));
    CHECK(!g.isDark(5) && !g.isDark(7));

    // similar levels: nothing dark (0.5 ratio: 50 vs 55)
    DarkParityFilter s;
    s.observe(0, 50.0);
    s.observe(1, 55.0);
    CHECK(s.known());
    CHECK(!s.isDark(0) && !s.isDark(1));
    // exactly at the ratio is not dark (strict <)
    DarkParityFilter e;
    e.observe(0, 25.0);
    e.observe(1, 50.0);
    CHECK(!e.isDark(0));

    // smoothing: 0.8 old + 0.2 new
    DarkParityFilter m;
    m.observe(0, 100.0);
    m.observe(0, 0.0);
    CHECK(std::fabs(m.level(0) - 80.0) < 1e-9);

    // learns a flip: after enough frames of the other pattern the other parity becomes dark
    DarkParityFilter l;
    l.observe(0, 0.5);
    l.observe(1, 55.0);
    CHECK(l.isDark(0));
    for (uint32_t k = 0; k < 60; ++k) {
        l.observe(2 * k, 55.0);
        l.observe(2 * k + 1, 0.5);
    }
    CHECK(l.isDark(1) && !l.isDark(0));

    f.reset();
    CHECK(!f.known());
    CHECK(!f.isDark(11));
    CHECK(f.level(0) == 0.0 && f.level(1) == 0.0);
}

// ---------------------------------------------------------------------------------------------- pickFrame

DarkParityFilter evenDark() {
    DarkParityFilter f;
    f.observe(0, 0.5);
    f.observe(1, 55.0);
    return f;
}

void testPickFrame() {
    const DarkParityFilter none;
    const DarkParityFilter dark = evenDark();
    CHECK(pickFrame({}, none, false, 0) == -1);

    // newest non-dark: even dark, odd lit
    std::vector<FrameEntry> e = {{0, 10}, {1, 11}, {2, 12}};
    CHECK(pickFrame(e, dark, false, 0) == 1);
    CHECK(pickFrame(e, none, false, 0) == 2);  // nothing known: newest wins

    // respects lastShownSeq
    CHECK(pickFrame(e, none, true, 11) == 2);
    CHECK(pickFrame(e, none, true, 12) == -1);
    CHECK(pickFrame(e, dark, true, 10) == 1);
    CHECK(pickFrame(e, dark, true, 11) == -1);  // 12 is dark, 11 already shown
    CHECK(pickFrame(e, none, true, 13) == -1);
    // haveShown false ignores lastShownSeq
    CHECK(pickFrame(e, none, false, 100) == 2);

    // only dark
    std::vector<FrameEntry> onlyDark = {{0, 10}, {1, 12}};
    CHECK(pickFrame(onlyDark, dark, false, 0) == -1);

    // wraparound
    std::vector<FrameEntry> w = {{0, 0xFFFFFFFEu}, {1, 0xFFFFFFFFu}, {2, 0u}, {3, 1u}};
    CHECK(pickFrame(w, none, true, 0xFFFFFFFEu) == 3);
    CHECK(pickFrame(w, none, true, 0xFFFFFFFFu) == 3);
    CHECK(pickFrame(w, none, true, 0u) == 3);
    CHECK(pickFrame(w, none, true, 1u) == -1);
    CHECK(pickFrame(w, dark, true, 0xFFFFFFFEu) == 3);  // seq 1 odd = lit
    std::vector<FrameEntry> w2 = {{0, 0xFFFFFFFEu}, {1, 0xFFFFFFFFu}};
    CHECK(pickFrame(w2, none, true, 0xFFFFFFFFu) == -1);
    CHECK(pickFrame(w2, none, true, 0xFFFFFFFEu) == 1);
}

// ---------------------------------------------------------------------------------------------- RecheckGate

void testRecheckGate() {
    using V = RecheckGate::Verdict;
    RecheckGate g;
    CHECK(!g.canCopy(0.0));  // no CAM yet
    CHECK(!g.currentGen(1));
    CHECK(g.onRecheck(1, 0, 0, 0) == V::Ignore);  // nothing in flight

    g.onCam(5);
    CHECK(g.currentGen(5) && !g.currentGen(4));
    CHECK(g.canCopy(0.0));
    g.begin(3, 100, 10.0);
    CHECK(g.inFlight());
    CHECK(!g.canCopy(10.5));
    CHECK(!g.canCopy(11.0));  // exactly 1 s: not yet
    CHECK(g.canCopy(11.01));  // a lost answer frees it after 1 s

    // answers that aren't this copy are ignored and leave it in flight
    CHECK(g.onRecheck(4, 3, 100, 0) == V::Ignore);  // old gen
    CHECK(g.onRecheck(5, 2, 100, 0) == V::Ignore);  // other index
    CHECK(g.onRecheck(5, 3, 101, 0) == V::Ignore);  // other seq
    CHECK(g.inFlight());
    CHECK(g.onRecheck(5, 3, 100, RecheckGate::kKeep) == V::Keep);
    CHECK(!g.inFlight());
    CHECK(g.canCopy(10.1));
    CHECK(g.onRecheck(5, 3, 100, 0) == V::Ignore);  // already answered

    // NEWER(1) / REQUEUED(2) / INVALID(4) discard and keep the gen
    for (uint8_t code : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
        g.begin(7, 200, 20.0);
        CHECK(g.onRecheck(5, 7, 200, code) == V::Discard);
        CHECK(!g.inFlight());
        CHECK(g.currentGen(5));
        CHECK(g.canCopy(20.0));
    }

    // STALE discards and blocks copying until a new CAM
    g.begin(7, 201, 30.0);
    CHECK(g.onRecheck(5, 7, 201, RecheckGate::kStale) == V::Discard);
    CHECK(!g.inFlight());
    CHECK(!g.canCopy(31.0));
    CHECK(!g.currentGen(5));
    g.onCam(6);
    CHECK(g.canCopy(31.0));
    CHECK(g.currentGen(6));
    // a late answer under the old gen can't publish
    CHECK(g.onRecheck(5, 7, 201, 0) == V::Ignore);

    // onCam drops the copy in flight
    g.begin(1, 1, 40.0);
    g.onCam(7);
    CHECK(!g.inFlight());
    CHECK(g.onRecheck(7, 1, 1, 0) == V::Ignore);
    CHECK(g.onRecheck(6, 1, 1, 0) == V::Ignore);

    // onGone drops it and the gen
    g.begin(1, 2, 50.0);
    g.onGone();
    CHECK(!g.inFlight());
    CHECK(!g.canCopy(50.0));
    CHECK(!g.currentGen(7));
    CHECK(g.onRecheck(7, 1, 2, 0) == V::Ignore);

    // same (gen, index, seq) after a new CAM of the same gen number: still only valid with a fresh begin
    g.onCam(7);
    CHECK(g.onRecheck(7, 1, 2, 0) == V::Ignore);
}

// ---------------------------------------------------------------------------------------------- RateGate

void testRateGate() {
    RateGate r(10.0);  // 0.1 s
    CHECK(r.take(0.0));
    CHECK(!r.take(0.05));
    CHECK(!r.ready(0.05));
    CHECK(r.ready(0.08));  // a quarter interval of slack: due at 0.1, ok from 0.075
    CHECK(!r.take(0.07));
    CHECK(r.take(0.08));
    // schedule, not "since the last frame": next due 0.2 even though 0.08 was taken
    CHECK(std::fabs(r.nextDue() - 0.2) < 1e-9);
    CHECK(!r.take(0.12));
    CHECK(r.take(0.2));

    // after a gap it restarts instead of bursting
    CHECK(r.take(5.0));
    CHECK(std::fabs(r.nextDue() - 5.1) < 1e-9);
    CHECK(!r.take(5.02));

    // a steady 100 Hz poll for 10 s gets ~10 fps (no more, and not far under)
    RateGate s(10.0);
    int n = 0;
    for (int k = 0; k < 1000; ++k)
        if (s.take(k * 0.01)) ++n;
    CHECK(n >= 98 && n <= 101);

    // fps 0 or negative: unlimited
    RateGate u(0.0);
    CHECK(u.take(0.0) && u.take(0.0) && u.ready(0.0));
    u.setFps(-3.0);
    CHECK(u.take(1.0) && u.take(1.0));
    u.setFps(1.0);
    CHECK(u.take(10.0));
    CHECK(!u.take(10.5));
    CHECK(u.take(11.0));
}

// ---------------------------------------------------------------------------------------------- tiles

WidgetInstance headset(uint8_t mask) {
    WidgetInstance w;
    w.type = WidgetType::HeadsetCamera;
    w.cameras = mask;
    return w;
}

WidgetInstance eyes() {
    WidgetInstance w;
    w.type = WidgetType::EyeCamera;
    return w;
}

bool overlap(const Tile& a, const Tile& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void checkInsideCard(const WidgetInstance& w) {
    const std::vector<Tile> tiles = cameraTiles(w);
    const int height = cameraWidgetHeight(w);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& t = tiles[i];
        CHECK(t.x >= 0 && t.x + t.w <= 496);
        CHECK(t.y >= 0 && t.y + t.h < height);
        CHECK(t.w > 0 && t.h > 0);
        CHECK(t.labelY < t.y && t.labelY >= 0);
        for (size_t j = i + 1; j < tiles.size(); ++j) CHECK(!overlap(t, tiles[j]));
    }
}

void testTiles() {
    // one outward cam: full content width, aspect kept
    {
        const std::vector<Tile> t = cameraTiles(headset(0x01));
        CHECK(t.size() == 1);
        CHECK(t[0].camera == 0);
        CHECK(t[0].w == 484);
        CHECK(std::abs(t[0].h - 484.0 * 1024 / 1056) <= 1.0);
        checkInsideCard(headset(0x01));
        CHECK(cameraWidgetHeight(headset(0x01)) > t[0].y + t[0].h);
    }
    // a 640x480 cam
    {
        const std::vector<Tile> t = cameraTiles(headset(0x04));
        CHECK(t.size() == 1 && t[0].camera == 2);
        CHECK(t[0].w == 484);
        CHECK(std::abs(t[0].h - 484.0 * 480 / 640) <= 1.0);
    }
    // 2-4 cams: two per row, same size, id order
    for (uint8_t mask : {uint8_t(0x03), uint8_t(0x05), uint8_t(0x07), uint8_t(0x0F), uint8_t(0x0C), uint8_t(0x0E)}) {
        const WidgetInstance w = headset(mask);
        const std::vector<Tile> t = cameraTiles(w);
        int want = 0;
        for (int b = 0; b < 4; ++b) want += (mask >> b) & 1;
        CHECK(static_cast<int>(t.size()) == want);
        uint8_t prev = 0;
        bool first = true;
        for (const Tile& tile : t) {
            CHECK(tile.w == t[0].w && tile.h == t[0].h);
            CHECK(tile.w < 484);
            if (!first) CHECK(tile.camera > prev);
            prev = tile.camera;
            first = false;
        }
        CHECK(t[0].x < t[1].x && t[0].y == t[1].y);  // first row holds two
        if (t.size() > 2) {
            CHECK(t[2].x == t[0].x && t[2].y > t[0].y);
            if (t.size() > 3) CHECK(t[3].x == t[1].x && t[3].y == t[2].y);
        }
        checkInsideCard(w);
    }
    // heights grow with rows
    CHECK(cameraWidgetHeight(headset(0x0F)) > cameraWidgetHeight(headset(0x03)));
    CHECK(cameraWidgetHeight(headset(0x07)) == cameraWidgetHeight(headset(0x0F)));
    // a hand-edited 0 mask still shows camera 0
    {
        const std::vector<Tile> t = cameraTiles(headset(0x00));
        CHECK(t.size() == 1 && t[0].camera == 0);
        // bits above the outward cams are ignored
        const std::vector<Tile> t2 = cameraTiles(headset(0xF1));
        CHECK(t2.size() == 1 && t2[0].camera == 0);
    }
    // eyes: side by side
    {
        const std::vector<Tile> t = cameraTiles(eyes());
        CHECK(t.size() == 2);
        CHECK(t[0].camera == 4 && t[1].camera == 5);
        CHECK(t[0].y == t[1].y && t[0].x < t[1].x);
        CHECK(t[0].w == t[1].w && t[0].h == t[1].h);
        CHECK(std::abs(t[0].h - t[0].w * 400.0 / 512) <= 1.0);
        checkInsideCard(eyes());
    }
    // other types
    {
        WidgetInstance o;
        o.type = WidgetType::None;
        CHECK(cameraTiles(o).empty());
    }
}

void testNativeSize() {
    int w = 0, h = 0;
    nativeSize(0, w, h);
    CHECK(w == 1056 && h == 1024);
    nativeSize(1, w, h);
    CHECK(w == 1056 && h == 1024);
    nativeSize(2, w, h);
    CHECK(w == 640 && h == 480);
    nativeSize(3, w, h);
    CHECK(w == 640 && h == 480);
    nativeSize(4, w, h);
    CHECK(w == 512 && h == 400);
    nativeSize(5, w, h);
    CHECK(w == 512 && h == 400);
}

void testFitFrame() {
    Tile t;
    t.x = 6;
    t.y = 50;
    t.w = 238;
    t.h = 179;
    // wider frame than tile: fills the width, centred vertically
    Rect r = fitFrame(t, 640, 240);
    CHECK(std::fabs(r.w - 238.0) < 1e-9);
    CHECK(std::fabs(r.h - 238.0 * 240 / 640) < 1e-9);
    CHECK(std::fabs(r.x - 6.0) < 1e-9);
    CHECK(std::fabs(r.y - (50 + (179 - r.h) / 2)) < 1e-9);
    // taller frame: fills the height, centred horizontally
    r = fitFrame(t, 1056, 1024);
    CHECK(std::fabs(r.h - 179.0) < 1e-9);
    CHECK(r.w < 238.0);
    CHECK(std::fabs(r.w / r.h - 1056.0 / 1024) < 1e-9);
    CHECK(std::fabs(r.x - (6 + (238 - r.w) / 2)) < 1e-9);
    CHECK(std::fabs(r.y - 50.0) < 1e-9);
    // same aspect: exact
    r = fitFrame(t, 238 * 2, 179 * 2);
    CHECK(std::fabs(r.w - 238) < 1e-9 && std::fabs(r.h - 179) < 1e-9);
    // an upscale is allowed (contain, not "down only")
    r = fitFrame(t, 10, 10);
    CHECK(std::fabs(r.h - 179.0) < 1e-9);
    // degenerate frames return the tile
    r = fitFrame(t, 0, 0);
    CHECK(r.x == 6 && r.y == 50 && r.w == 238 && r.h == 179);
}

// ---------------------------------------------------------------------------------------------- ids / layout

void testIdsAndLayout() {
    CHECK(wantBit(0) == 1u << 8 && wantBit(5) == 1u << 13);
    CHECK(kWantCameraMask == 0x7F00u);
    CHECK(wantBit(kHttpCamera) == 1u << 14 && kCameraCount == 7);
    CHECK((kWantCameraPathOnly & kWantCameraMask) == 0);
    CHECK(camerasFromWants(wantBit(1) | wantBit(4) | 0x1u | kWantCameraPathOnly) == ((1u << 1) | (1u << 4)));
    CHECK(isOutward(3) && !isOutward(4) && isEye(4) && isEye(5) && !isEye(6));
    CHECK(std::strcmp(cameraName(0), "video0") == 0 && std::strcmp(cameraName(3), "video13") == 0);
    CHECK(std::strcmp(cameraName(4), "Eye 1") == 0 && std::strcmp(cameraName(6), "HTTP camera") == 0);
    CHECK(std::strcmp(cameraName(7), "?") == 0 && !isOutward(6));
    CHECK(camerasFromWants(wantBit(6)) == (1u << 6) && camerasFromWants(1u << 15) == 0);

    Layout l;
    LayoutColumn c0, c1;
    c0.widgets.push_back(headset(0x05));
    c1.widgets.push_back(eyes());
    WidgetInstance full = headset(0x02);
    full.cameraFullSize = true;
    c1.widgets.push_back(full);
    l.columns = {c0, c1};
    CHECK(camerasInLayout(l) == (0x05 | 0x02 | 0x30));
    CHECK(fullSizeInLayout(l));
    l.columns[1].widgets.pop_back();
    CHECK(camerasInLayout(l) == (0x05 | 0x30));
    CHECK(!fullSizeInLayout(l));
    CHECK(camerasInLayout(Layout {}) == 0);

    CHECK(cameraStateText(CameraState::Live).empty());
    CHECK(cameraStateText(CameraState::NotWorn) == "Headset not worn");
    CHECK(cameraStateText(CameraState::HelperMissing).find("install-camera-helper") != std::string::npos);
    CHECK(!cameraStateText(CameraState::Starting).empty());
    CHECK(!cameraStateText(CameraState::Off).empty());
    // the HTTP camera's own states name the address
    CHECK(cameraStateText(CameraState::Connecting, "127.0.0.1:8082").find("127.0.0.1:8082") != std::string::npos);
    CHECK(cameraStateText(CameraState::Unreachable, "127.0.0.1:8082").find("127.0.0.1:8082") != std::string::npos);
    CHECK(cameraStateText(CameraState::NotMjpeg, "cam.local:80").find("cam.local:80") != std::string::npos);
    CHECK(cameraStateText(CameraState::Unreachable, "cam.local:80") == "Can't reach cam.local:80. Is the stream running?");
    CHECK(cameraStateText(CameraState::Unreachable, "cam.local:80").find("mouthcam") == std::string::npos);
    CHECK(cameraStateText(CameraState::NotMjpeg, "cam.local:80").find("MJPEG") != std::string::npos);
    CHECK(static_cast<int>(CameraState::Connecting) == 7 && static_cast<int>(CameraState::Unreachable) == 8 &&
          static_cast<int>(CameraState::NotMjpeg) == 9);  // they go over the link
}

// ---------------------------------------------------------------------------------------------- HTTP camera

WidgetInstance httpCam(uint8_t rotation = 0, uint8_t fps = 15) {
    WidgetInstance w;
    w.type = WidgetType::HttpCamera;
    w.rotation = rotation;
    w.cameraFps = fps;
    return w;
}

void testHttpCameraLayoutHelpers() {
    Layout l;
    LayoutColumn c0, c1;
    c0.widgets.push_back(headset(0x01));
    CHECK(httpCameraIn(l) == nullptr);
    l.columns = {c0};
    CHECK(httpCameraIn(l) == nullptr);
    CHECK(camerasInLayout(l) == 0x01);

    // the first one in column order is the live one, not the one with the lowest fps or the last
    WidgetInstance first = httpCam(1, 10), second = httpCam(2, 30);
    first.url = "http://first:1/";
    second.url = "http://second:2/";
    c1.widgets = {first};
    LayoutColumn c2;
    c2.widgets = {headset(0x02), second};
    l.columns = {c0, c1, c2};
    const WidgetInstance* live = httpCameraIn(l);
    CHECK(live != nullptr && live->url == "http://first:1/" && live == &l.columns[1].widgets[0]);
    CHECK(camerasInLayout(l) == (0x01 | 0x02 | (1u << 6)));

    // fps: only the live one counts
    double fps[kCameraCount];
    displayFpsInLayout(l, fps);
    CHECK(fps[6] == 10);
    CHECK(fps[0] == 15 && fps[1] == 15 && fps[4] == 0 && fps[5] == 0);
    l.columns[1].widgets.clear();
    CHECK(httpCameraIn(l)->url == "http://second:2/");
    displayFpsInLayout(l, fps);
    CHECK(fps[6] == 30);
    // none: 0
    l.columns[2].widgets.pop_back();
    displayFpsInLayout(l, fps);
    CHECK(fps[6] == 0 && httpCameraIn(l) == nullptr);

    // an HTTP camera alone sets bit 6 only
    Layout only;
    only.columns.resize(1);
    only.columns[0].widgets = {httpCam()};
    CHECK(camerasInLayout(only) == (1u << 6));
    // full size is about the helper's cams (the HTTP camera's is per widget)
    WidgetInstance big = httpCam();
    big.cameraFullSize = true;
    only.columns[0].widgets = {big};
    CHECK(!fullSizeInLayout(only));
}

void testHttpCameraTiles() {
    for (const uint8_t rotation : {0, 1, 2, 3}) {
        const WidgetInstance w = httpCam(rotation);
        const std::vector<Tile> t = cameraTiles(w);
        CHECK(t.size() == 1);
        if (t.size() != 1) continue;
        CHECK(t[0].camera == kHttpCamera && t[0].w == 484);
        if (rotation & 1) {
            CHECK(t[0].h == 645);  // 3:4 portrait: 484 * 4 / 3
        } else {
            CHECK(t[0].h == 363);  // 4:3: 484 * 3 / 4
        }
        checkInsideCard(w);
        CHECK(cameraWidgetHeight(w) > t[0].y + t[0].h);
    }
    CHECK(cameraWidgetHeight(httpCam(1)) > cameraWidgetHeight(httpCam(0)));
    CHECK(cameraWidgetHeight(httpCam(3)) == cameraWidgetHeight(httpCam(1)));
    CHECK(cameraWidgetHeight(httpCam(2)) == cameraWidgetHeight(httpCam(0)));

    // an extra one (layoutGrid marks it): no tile, but the card is as tall as the live one
    for (const uint8_t rotation : {0, 1}) {
        WidgetInstance extra = httpCam(rotation);
        extra.httpCameraExtra = true;
        CHECK(cameraTiles(extra).empty());
        CHECK(cameraWidgetHeight(extra) == cameraWidgetHeight(httpCam(rotation)));
    }
    int w = 0, h = 0;
    nativeSize(kHttpCamera, w, h);
    CHECK(w == 640 && h == 480);
}

void testHttpCameraSize() {
    // Size: the picture is a percent of the card's width, centred. 100% is the default (484 wide at x = 6).
    WidgetInstance w = httpCam(0);
    CHECK(w.cameraSize == 100);
    {
        const Tile t = cameraTiles(w)[0];
        CHECK(t.w == 484 && t.x == 6 && t.h == 363);
    }
    struct Case {
        uint8_t size, rotation;
        int w, x, h;
    };
    // w = round(484 * size / 100), x = 6 + (484 - w) / 2, h = w * 3/4 (0, 2) or w * 4/3 (1, 3), rounded
    const Case cases[] = {
        {40, 0, 194, 151, 146}, {40, 3, 194, 151, 259}, {40, 1, 194, 151, 259}, {40, 2, 194, 151, 146},
        {50, 0, 242, 127, 182}, {70, 0, 339, 78, 254},  {100, 3, 484, 6, 645},  {100, 0, 484, 6, 363},
    };
    for (const Case& c : cases) {
        w = httpCam(c.rotation);
        w.cameraSize = c.size;
        const std::vector<Tile> t = cameraTiles(w);
        CHECK(t.size() == 1);
        if (t.size() != 1) continue;
        if (t[0].w != c.w || t[0].x != c.x || t[0].h != c.h)
            std::printf("size %d rot %d: got w %d x %d h %d\n", c.size, c.rotation, t[0].w, t[0].x, t[0].h);
        CHECK(t[0].w == c.w && t[0].x == c.x && t[0].h == c.h);
        CHECK(t[0].camera == kHttpCamera);
        checkInsideCard(w);
        CHECK(cameraWidgetHeight(w) > t[0].y + t[0].h);
        // centred in the card's content width (6 .. 490)
        CHECK(std::abs((t[0].x - 6) - (490 - (t[0].x + t[0].w))) <= 1);
    }
    // the card is as tall as the picture needs: smaller picture, shorter card
    WidgetInstance big = httpCam(3), small = httpCam(3);
    small.cameraSize = 40;
    CHECK(cameraWidgetHeight(small) < cameraWidgetHeight(big));
    CHECK(cameraWidgetHeight(big) - cameraWidgetHeight(small) == cameraTiles(big)[0].h - cameraTiles(small)[0].h);
    // an extra one is as tall as a live one at the same size
    WidgetInstance extra = small;
    extra.httpCameraExtra = true;
    CHECK(cameraTiles(extra).empty() && cameraWidgetHeight(extra) == cameraWidgetHeight(small));
}

void testRotateGrey() {
    // 3 wide, 2 tall:   1 2 3
    //                   4 5 6
    const uint8_t src[6] = {1, 2, 3, 4, 5, 6};
    uint8_t dst[6];
    rotateGrey(dst, src, 3, 2, 0);
    CHECK(std::memcmp(dst, src, 6) == 0);
    // a quarter clockwise: 2 wide, 3 tall   4 1 / 5 2 / 6 3
    const uint8_t r1[6] = {4, 1, 5, 2, 6, 3};
    rotateGrey(dst, src, 3, 2, 1);
    CHECK(std::memcmp(dst, r1, 6) == 0);
    // half: 3 wide, 2 tall   6 5 4 / 3 2 1
    const uint8_t r2[6] = {6, 5, 4, 3, 2, 1};
    rotateGrey(dst, src, 3, 2, 2);
    CHECK(std::memcmp(dst, r2, 6) == 0);
    // three quarters: 2 wide, 3 tall   3 6 / 2 5 / 1 4
    const uint8_t r3[6] = {3, 6, 2, 5, 1, 4};
    rotateGrey(dst, src, 3, 2, 3);
    CHECK(std::memcmp(dst, r3, 6) == 0);
    // taken mod 4
    rotateGrey(dst, src, 3, 2, 5);
    CHECK(std::memcmp(dst, r1, 6) == 0);
    rotateGrey(dst, src, 3, 2, 4);
    CHECK(std::memcmp(dst, src, 6) == 0);
    // four quarters are the identity, on a bigger odd-sized image
    const int iw = 7, ih = 5;
    Bytes img(iw * ih), a(iw * ih), b(iw * ih);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i * 5 + 1);
    rotateGrey(a.data(), img.data(), iw, ih, 1);
    rotateGrey(b.data(), a.data(), ih, iw, 1);
    rotateGrey(a.data(), b.data(), iw, ih, 1);
    rotateGrey(b.data(), a.data(), ih, iw, 1);
    CHECK(b == img);
    // a column of one pixel and a row of one pixel
    const uint8_t col[3] = {7, 8, 9};
    uint8_t out3[3];
    rotateGrey(out3, col, 1, 3, 1);  // 1 wide 3 tall -> 3 wide 1 tall, clockwise: bottom to top becomes left to right
    CHECK(out3[0] == 9 && out3[1] == 8 && out3[2] == 7);
    rotateGrey(out3, col, 1, 3, 3);
    CHECK(out3[0] == 7 && out3[1] == 8 && out3[2] == 9);
}

// ---------------------------------------------------------------------------------------------- blob

void testBlobRoundTrip() {
    Bytes jpeg(300);
    for (size_t i = 0; i < jpeg.size(); ++i) jpeg[i] = static_cast<uint8_t>(i * 31 + 7);
    BlobHeader h;
    h.camera = 3;
    h.state = CameraState::Live;
    h.width = 528;
    h.height = 512;
    h.frameId = 0x01020304u;
    const Bytes blob = encodeCameraBlob(h, jpeg.data(), jpeg.size());
    CHECK(blob.size() == BlobHeader::kSize + jpeg.size());
    CHECK(blob[0] == 1 && blob[1] == 3 && blob[2] == 0);
    CHECK(blob[4] == (528 & 0xFF) && blob[5] == (528 >> 8));
    CHECK(blob[8] == 0x04 && blob[11] == 0x01);  // little endian

    BlobHeader d;
    const uint8_t* j = nullptr;
    size_t jn = 0;
    CHECK(decodeCameraBlob(blob.data(), blob.size(), d, j, jn));
    CHECK(d.camera == 3 && d.state == CameraState::Live && d.width == 528 && d.height == 512 &&
          d.frameId == 0x01020304u);
    CHECK(jn == jpeg.size() && j == blob.data() + BlobHeader::kSize && std::memcmp(j, jpeg.data(), jn) == 0);

    // a non-Live state round-trips with no jpeg; any jpeg passed is dropped
    for (CameraState s : {CameraState::Starting, CameraState::NotWorn, CameraState::HelperMissing,
                          CameraState::HelperOutdated, CameraState::Unavailable, CameraState::Off,
                          CameraState::Connecting, CameraState::Unreachable, CameraState::NotMjpeg}) {
        BlobHeader n;
        n.camera = 5;
        n.state = s;
        n.frameId = 9;
        const Bytes b = encodeCameraBlob(n, jpeg.data(), jpeg.size());
        CHECK(b.size() == BlobHeader::kSize);
        BlobHeader dn;
        CHECK(decodeCameraBlob(b.data(), b.size(), dn, j, jn));
        CHECK(dn.camera == 5 && dn.state == s && dn.frameId == 9 && j == nullptr && jn == 0);
    }

    // max-size dims and ids
    BlobHeader big;
    big.camera = 0;
    big.width = 0xFFFF;
    big.height = 0xFFFF;
    big.frameId = 0xFFFFFFFFu;
    const Bytes bb = encodeCameraBlob(big, jpeg.data(), 1);
    CHECK(decodeCameraBlob(bb.data(), bb.size(), d, j, jn));
    CHECK(d.width == 0xFFFF && d.height == 0xFFFF && d.frameId == 0xFFFFFFFFu && jn == 1);
}

void testBlobMalformed() {
    Bytes jpeg(50, 0x42);
    BlobHeader h;
    h.camera = 1;
    h.state = CameraState::Live;
    h.width = 10;
    h.height = 10;
    h.frameId = 1;
    const Bytes good = encodeCameraBlob(h, jpeg.data(), jpeg.size());
    BlobHeader d;
    const uint8_t* j = nullptr;
    size_t jn = 0;
    CHECK(decodeCameraBlob(good.data(), good.size(), d, j, jn));

    Bytes b = good;
    b[0] = 2;  // another version
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));
    CHECK(j == nullptr && jn == 0);
    b = good;
    b[0] = 0;
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));

    b = good;
    b[1] = 7;  // camera >= kCameraCount
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));
    b[1] = 255;
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));
    b[1] = 6;  // the HTTP camera
    CHECK(decodeCameraBlob(b.data(), b.size(), d, j, jn) && d.camera == 6);
    b[1] = 5;
    CHECK(decodeCameraBlob(b.data(), b.size(), d, j, jn));

    b = good;
    b[2] = 10;  // unknown state (9 = NotMjpeg is the last)
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));
    b[2] = 255;
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));

    // too short / null
    CHECK(!decodeCameraBlob(good.data(), BlobHeader::kSize - 1, d, j, jn));
    CHECK(!decodeCameraBlob(good.data(), 0, d, j, jn));
    CHECK(!decodeCameraBlob(nullptr, 20, d, j, jn));

    // Live without a jpeg
    CHECK(!decodeCameraBlob(good.data(), BlobHeader::kSize, d, j, jn));
    // Live with zero dimensions
    b = good;
    b[4] = b[5] = 0;
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));
    b = good;
    b[6] = b[7] = 0;
    CHECK(!decodeCameraBlob(b.data(), b.size(), d, j, jn));

    // non-Live with trailing bytes
    BlobHeader n;
    n.camera = 2;
    n.state = CameraState::NotWorn;
    Bytes nb = encodeCameraBlob(n, nullptr, 0);
    CHECK(decodeCameraBlob(nb.data(), nb.size(), d, j, jn));
    nb.push_back(0);
    CHECK(!decodeCameraBlob(nb.data(), nb.size(), d, j, jn));

    // encode with Live state but no jpeg pointer makes a header-only blob the decoder rejects
    const Bytes noJpeg = encodeCameraBlob(h, nullptr, 0);
    CHECK(noJpeg.size() == BlobHeader::kSize);
    CHECK(!decodeCameraBlob(noJpeg.data(), noJpeg.size(), d, j, jn));
}

}  // namespace

int main() {
    testCopyPitch();
    testCopyDownscale();
    testDownscaleRounding();
    testCacheOps();
    testSampleLevel();
    testGreyToRgba();
    testDarkParity();
    testPickFrame();
    testRecheckGate();
    testRateGate();
    testTiles();
    testNativeSize();
    testFitFrame();
    testIdsAndLayout();
    testHttpCameraLayoutHelpers();
    testHttpCameraTiles();
    testHttpCameraSize();
    testRotateGrey();
    testBlobRoundTrip();
    testBlobMalformed();
    if (failures == 0) std::printf("camera_frame_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
