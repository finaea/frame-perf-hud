// T10 pc/camera_feed: Camera BLOB -> turbojpeg decode -> per-camera picture and state, staleness, the SUBSCRIBE
// wants bits, and painting a live picture into its tile.
#include "camera_feed.h"
#include "camera_frame.h"

#include <cairo.h>
#include <turbojpeg.h>

#include <algorithm>
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
using fph::pc::CameraFeed;

using Bytes = std::vector<uint8_t>;

/** A grey JPEG (TJSAMP_GRAY) of a horizontal gradient, or a flat level when gradient is false. */
Bytes makeJpeg(int w, int h, uint8_t level, bool gradient = false) {
    Bytes grey(static_cast<size_t>(w) * h, level);
    if (gradient)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) grey[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
    tjhandle tj = tjInitCompress();
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    const int rc = tjCompress2(tj, grey.data(), w, 0, h, TJPF_GRAY, &buf, &size, TJSAMP_GRAY, 90, TJFLAG_FASTDCT);
    Bytes out;
    if (rc == 0) out.assign(buf, buf + size);
    tjFree(buf);
    tjDestroy(tj);
    return out;
}

Bytes liveBlob(uint8_t camera, int w, int h, uint32_t frameId, const Bytes& jpeg) {
    BlobHeader hd;
    hd.camera = camera;
    hd.state = CameraState::Live;
    hd.width = static_cast<uint16_t>(w);
    hd.height = static_cast<uint16_t>(h);
    hd.frameId = frameId;
    return encodeCameraBlob(hd, jpeg.data(), jpeg.size());
}

Bytes stateBlob(uint8_t camera, CameraState state, uint32_t frameId = 0) {
    BlobHeader hd;
    hd.camera = camera;
    hd.state = state;
    hd.frameId = frameId;
    return encodeCameraBlob(hd, nullptr, 0);
}

WidgetInstance headset(uint8_t mask) {
    WidgetInstance w;
    w.type = WidgetType::HeadsetCamera;
    w.cameras = mask;
    return w;
}

void testTwoCameras() {
    CameraFeed feed;
    const Bytes j0 = makeJpeg(64, 48, 100, true);
    const Bytes j3 = makeJpeg(40, 30, 180);
    CHECK(!j0.empty() && !j3.empty());

    // nothing yet: Starting (discovery), no pictures
    CHECK(feed.state(0, 1.0) == CameraState::Starting);
    CHECK(feed.state(3, 1.0) == CameraState::Starting);
    CHECK(feed.width(0) == 0 && feed.height(0) == 0 && feed.frameId(0) == 0);
    CHECK(feed.decoded() == 0 && feed.rejected() == 0);

    const uint64_t rev0 = feed.revision();
    const Bytes b0 = liveBlob(0, 64, 48, 7, j0);
    CHECK(feed.onBlob(0, b0.data(), b0.size(), 10.0));
    CHECK(feed.revision() > rev0);
    const Bytes b3 = liveBlob(3, 40, 30, 2, j3);
    CHECK(feed.onBlob(3, b3.data(), b3.size(), 10.0));

    CHECK(feed.width(0) == 64 && feed.height(0) == 48 && feed.frameId(0) == 7);
    CHECK(feed.width(3) == 40 && feed.height(3) == 30 && feed.frameId(3) == 2);
    CHECK(feed.state(0, 10.1) == CameraState::Live);
    CHECK(feed.state(3, 10.1) == CameraState::Live);
    CHECK(feed.state(1, 10.1) == CameraState::Starting);  // untouched camera
    CHECK(feed.decoded() == 2 && feed.rejected() == 0);

    // a newer frame replaces the picture and bumps the revision
    const uint64_t rev1 = feed.revision();
    const Bytes b0b = liveBlob(0, 64, 48, 8, j0);
    CHECK(feed.onBlob(0, b0b.data(), b0b.size(), 10.2));
    CHECK(feed.frameId(0) == 8 && feed.revision() > rev1);

    // the picture changes size (full-size toggled on the headset)
    const Bytes big = makeJpeg(128, 96, 50);
    const Bytes b0c = liveBlob(0, 128, 96, 9, big);
    CHECK(feed.onBlob(0, b0c.data(), b0c.size(), 10.3));
    CHECK(feed.width(0) == 128 && feed.height(0) == 96);

    // out-of-range camera ids (6 is the HTTP camera now: a real camera, untouched so far)
    CHECK(feed.state(6, 10.3) == CameraState::Starting);
    CHECK(feed.state(7, 10.3) == CameraState::Off);
    CHECK(feed.state(255, 10.3) == CameraState::Off);
    CHECK(feed.width(7) == 0 && feed.height(200) == 0 && feed.frameId(9) == 0);
}

void testStale() {
    CameraFeed feed;
    const Bytes j = makeJpeg(32, 32, 90);
    const Bytes b = liveBlob(1, 32, 32, 1, j);
    CHECK(feed.onBlob(1, b.data(), b.size(), 100.0));
    CHECK(feed.state(1, 100.0) == CameraState::Live);
    CHECK(feed.state(1, 100.0 + CameraFeed::kStaleSec - 0.01) == CameraState::Live);
    // by time alone, before any tick
    CHECK(feed.state(1, 100.0 + CameraFeed::kStaleSec) == CameraState::Starting);

    // tick before the threshold changes nothing
    uint64_t rev = feed.revision();
    feed.tick(100.0 + CameraFeed::kStaleSec - 0.1);
    CHECK(feed.revision() == rev);
    CHECK(feed.state(1, 100.5) == CameraState::Live);

    // tick after: Starting, revision bumps once
    feed.tick(100.0 + CameraFeed::kStaleSec + 0.01);
    CHECK(feed.revision() > rev);
    CHECK(feed.state(1, 100.0 + CameraFeed::kStaleSec + 0.01) == CameraState::Starting);
    rev = feed.revision();
    feed.tick(110.0);
    CHECK(feed.revision() == rev);  // already stale: no more bumps

    // a fresh frame brings it back
    CHECK(feed.onBlob(1, b.data(), b.size(), 120.0));
    CHECK(feed.state(1, 120.0) == CameraState::Live);
}

void testStateBlobs() {
    CameraFeed feed;
    const Bytes j = makeJpeg(32, 32, 90);
    const Bytes live = liveBlob(2, 32, 32, 1, j);
    CHECK(feed.onBlob(2, live.data(), live.size(), 5.0));
    CHECK(feed.state(2, 5.0) == CameraState::Live);

    // the headset says it isn't worn: don't keep showing the last picture
    const Bytes nw = stateBlob(2, CameraState::NotWorn);
    const uint64_t rev = feed.revision();
    CHECK(feed.onBlob(2, nw.data(), nw.size(), 5.1));
    CHECK(feed.revision() > rev);
    CHECK(feed.state(2, 5.1) == CameraState::NotWorn);
    CHECK(feed.state(2, 5.2) == CameraState::NotWorn);  // and it stays, no matter the time
    CHECK(feed.state(2, 500.0) == CameraState::NotWorn);

    // every non-Live state is reported as sent
    for (CameraState s : {CameraState::Starting, CameraState::HelperMissing, CameraState::HelperOutdated,
                          CameraState::Unavailable, CameraState::Off}) {
        const Bytes b = stateBlob(4, s);
        CHECK(feed.onBlob(4, b.data(), b.size(), 6.0));
        CHECK(feed.state(4, 6.0) == s);
    }

    // the same state again changes nothing visible
    const Bytes un = stateBlob(5, CameraState::Unavailable);
    CHECK(feed.onBlob(5, un.data(), un.size(), 7.0));
    const uint64_t rev2 = feed.revision();
    CHECK(feed.onBlob(5, un.data(), un.size(), 7.5));
    CHECK(feed.revision() == rev2);

    // frames come again: Live
    CHECK(feed.onBlob(2, live.data(), live.size(), 8.0));
    CHECK(feed.state(2, 8.0) == CameraState::Live);
    // a state blob doesn't count as a decoded frame
    CHECK(feed.decoded() == 2);
}

void testRejects() {
    CameraFeed feed;
    const Bytes j = makeJpeg(32, 32, 90);
    const Bytes good = liveBlob(0, 32, 32, 1, j);
    CHECK(feed.onBlob(0, good.data(), good.size(), 1.0));
    const uint64_t decoded = feed.decoded();

    // the header's camera isn't the BLOB's stream
    CHECK(!feed.onBlob(1, good.data(), good.size(), 1.1));
    CHECK(feed.rejected() == 1);
    CHECK(feed.width(1) == 0);
    CHECK(feed.state(1, 1.1) == CameraState::Starting);
    CHECK(!feed.onBlob(6, good.data(), good.size(), 1.1));
    CHECK(feed.rejected() == 2);
    CHECK(!feed.onBlob(7, good.data(), good.size(), 1.1));  // no such camera, whatever the header says
    CHECK(feed.rejected() == 3);

    // corrupt JPEG with a valid header
    Bytes garbage(200);
    for (size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<uint8_t>(i * 7 + 3);
    const Bytes badJpeg = liveBlob(0, 32, 32, 2, garbage);
    CHECK(!feed.onBlob(0, badJpeg.data(), badJpeg.size(), 1.2));
    CHECK(feed.rejected() == 4);
    CHECK(feed.frameId(0) == 1);  // nothing changed
    CHECK(feed.state(0, 1.2) == CameraState::Live);

    // a JPEG header cut off after the SOI
    const Bytes tiny = {0xFF, 0xD8, 0xFF};
    const Bytes tinyBlob = liveBlob(0, 32, 32, 3, tiny);
    CHECK(!feed.onBlob(0, tinyBlob.data(), tinyBlob.size(), 1.3));

    // header size that disagrees with the JPEG's
    const Bytes wrongDims = liveBlob(0, 64, 64, 4, j);
    CHECK(!feed.onBlob(0, wrongDims.data(), wrongDims.size(), 1.4));
    CHECK(feed.width(0) == 32 && feed.frameId(0) == 1);

    // oversized picture (over 4096 px) refused even when header and JPEG agree
    const Bytes wide = makeJpeg(4100, 8, 10);
    const Bytes wideBlob = liveBlob(0, 4100, 8, 5, wide);
    CHECK(!feed.onBlob(0, wideBlob.data(), wideBlob.size(), 1.5));

    // malformed blobs
    Bytes bad = good;
    bad[0] = 9;  // another version
    CHECK(!feed.onBlob(0, bad.data(), bad.size(), 1.6));
    CHECK(!feed.onBlob(0, good.data(), 5, 1.6));
    CHECK(!feed.onBlob(0, nullptr, 0, 1.6));
    CHECK(feed.decoded() == decoded);
    CHECK(feed.rejected() >= 10);
}

void testReset() {
    CameraFeed feed;
    const Bytes j = makeJpeg(32, 32, 90);
    const Bytes b = liveBlob(0, 32, 32, 40, j);
    CHECK(feed.onBlob(0, b.data(), b.size(), 1.0));
    const uint64_t rev = feed.revision();
    feed.reset();
    CHECK(feed.revision() > rev);
    CHECK(feed.width(0) == 0 && feed.frameId(0) == 0);
    CHECK(feed.state(0, 1.0) == CameraState::Starting);
    // frame ids restart at the headset: an old, higher id doesn't block a new picture
    const Bytes again = liveBlob(0, 32, 32, 1, j);
    CHECK(feed.onBlob(0, again.data(), again.size(), 2.0));
    CHECK(feed.frameId(0) == 1);
}

void testWantsFor() {
    Layout l;
    CHECK(CameraFeed::wantsFor(l) == 0);
    LayoutColumn c0, c1;
    c0.widgets.push_back(headset(0x05));
    WidgetInstance eyes;
    eyes.type = WidgetType::EyeCamera;
    c1.widgets.push_back(eyes);
    l.columns = {c0, c1};
    CHECK(CameraFeed::wantsFor(l) == (wantBit(0) | wantBit(2) | wantBit(4) | wantBit(5)));
    CHECK((CameraFeed::wantsFor(l) & ~kWantCameraMask) == 0);

    // the second widget adds its cameras; duplicates don't matter
    l.columns[0].widgets.push_back(headset(0x0F));
    CHECK(CameraFeed::wantsFor(l) == (wantBit(0) | wantBit(1) | wantBit(2) | wantBit(3) | wantBit(4) | wantBit(5)));

    // other widgets want nothing
    Layout o;
    LayoutColumn oc;
    WidgetInstance other;
    other.type = WidgetType::None;
    oc.widgets.push_back(other);
    o.columns = {oc};
    CHECK(CameraFeed::wantsFor(o) == 0);
}

void testPaint() {
    CameraFeed feed;
    // 66x64 flat grey 128: same aspect as video0's 1056x1024, so it fills the whole tile
    const Bytes j = makeJpeg(66, 64, 128);
    const Bytes b = liveBlob(0, 66, 64, 1, j);
    CHECK(feed.onBlob(0, b.data(), b.size(), 10.0));

    WidgetInstance w = headset(0x01);
    const std::vector<Tile> tiles = cameraTiles(w);
    CHECK(tiles.size() == 1);
    const Tile t = tiles[0];

    GridGeometry geometry;
    WidgetSlot slot;
    slot.widget = w;
    slot.x = 0;
    slot.y = 0;
    slot.w = 496;
    slot.h = cameraWidgetHeight(w);
    geometry.slots.push_back(slot);
    geometry.width = 512;
    geometry.height = slot.h + 20;

    auto render = [&](double now, double scale, int& pxCentre, int& pxCorner, int& pxOutside) {
        const int W = static_cast<int>(512 * scale), H = static_cast<int>(geometry.height * scale);
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        feed.paint(cr, geometry, scale, now);
        cairo_surface_flush(surf);
        const unsigned char* d = cairo_image_surface_get_data(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        auto at = [&](double x, double y) {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(d + static_cast<size_t>(y * scale) * stride + static_cast<size_t>(x * scale) * 4);
            return static_cast<int>(v & 0xFF);
        };
        pxCentre = at(t.x + t.w / 2.0, t.y + t.h / 2.0);
        pxCorner = at(t.x + 12, t.y + 12);  // inside the bilinear edge blend
        pxOutside = at(2, 2);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    };

    int centre = 0, corner = 0, outside = 0;
    render(10.1, 1.0, centre, corner, outside);
    CHECK(centre >= 120 && centre <= 136);
    CHECK(corner >= 120 && corner <= 136);
    CHECK(outside == 0);  // the card's own area outside the tile is the panel's job, not the feed's

    render(10.1, 0.5, centre, corner, outside);  // the window scales the panel
    CHECK(centre >= 120 && centre <= 136);
    CHECK(outside == 0);

    // stale: nothing painted
    render(10.0 + CameraFeed::kStaleSec + 1.0, 1.0, centre, corner, outside);
    CHECK(centre == 0 && corner == 0);

    // a non-Live state: nothing painted
    const Bytes nw = stateBlob(0, CameraState::NotWorn);
    CHECK(feed.onBlob(0, nw.data(), nw.size(), 10.2));
    render(10.3, 1.0, centre, corner, outside);
    CHECK(centre == 0);

    // letterbox: a wide picture in the same tile leaves bars above and below
    const Bytes wide = makeJpeg(128, 32, 200);
    const Bytes wb = liveBlob(0, 128, 32, 2, wide);
    CHECK(feed.onBlob(0, wb.data(), wb.size(), 11.0));
    render(11.1, 1.0, centre, corner, outside);
    CHECK(centre >= 192 && centre <= 208);
    CHECK(corner == 0);  // inside the tile but in the bar
}

/** A grey JPEG, white (255) on the left half and black on the right, or white on top and black below. */
Bytes makeHalvesJpeg(int w, int h, bool leftRight) {
    Bytes grey(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) grey[static_cast<size_t>(y) * w + x] = (leftRight ? x < w / 2 : y < h / 2) ? 255 : 0;
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

WidgetInstance httpCamera(uint8_t rotation) {
    WidgetInstance w;
    w.type = WidgetType::HttpCamera;
    w.rotation = rotation;
    return w;
}

/** Paints one widget alone on a black panel and returns the blue channel at (fx, fy), fractions of its tile. */
int paintedAt(const CameraFeed& feed, const WidgetInstance& w, double now, double fx, double fy) {
    GridGeometry geometry;
    WidgetSlot slot;
    slot.widget = w;
    slot.w = 496;
    slot.h = cameraWidgetHeight(w);
    geometry.slots.push_back(slot);
    geometry.width = 512;
    geometry.height = slot.h + 20;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 512, geometry.height);
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    feed.paint(cr, geometry, 1.0, now);
    cairo_surface_flush(surf);
    const unsigned char* d = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);
    int v;
    const std::vector<Tile> tiles = cameraTiles(w);
    if (tiles.empty()) {  // no tile laid out (an extra HTTP camera): the brightest pixel anywhere
        v = 0;
        for (int y = 0; y < geometry.height; ++y)
            for (int x = 0; x < 512; ++x) v = std::max(v, static_cast<int>(d[y * stride + x * 4]));
    } else {
        const Tile& t = tiles[0];
        v = d[static_cast<int>(t.y + t.h * fy) * stride + static_cast<int>(t.x + t.w * fx) * 4];
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return v;
}

void testHttpCameraBlobs() {
    CameraFeed feed;
    // camera 6 takes a BLOB like any other
    const Bytes j = makeJpeg(64, 48, 100, true);
    const Bytes b = liveBlob(6, 64, 48, 3, j);
    CHECK(feed.state(6, 1.0) == CameraState::Starting);
    CHECK(feed.onBlob(6, b.data(), b.size(), 1.0));
    CHECK(feed.width(6) == 64 && feed.height(6) == 48 && feed.frameId(6) == 3);
    CHECK(feed.state(6, 1.1) == CameraState::Live);
    CHECK(feed.state(0, 1.1) == CameraState::Starting);  // the others are untouched
    CHECK(feed.decoded() == 1 && feed.rejected() == 0);

    // the HTTP camera's own states come through
    for (const CameraState s : {CameraState::Connecting, CameraState::Unreachable, CameraState::NotMjpeg}) {
        const Bytes sb = stateBlob(6, s);
        CHECK(feed.onBlob(6, sb.data(), sb.size(), 2.0));
        CHECK(feed.state(6, 2.1) == s);
    }
    // a state only a newer headset would send (10) is refused
    Bytes bad = stateBlob(6, CameraState::NotMjpeg);
    bad[2] = 10;
    CHECK(!feed.onBlob(6, bad.data(), bad.size(), 2.2));
    CHECK(feed.state(6, 2.3) == CameraState::NotMjpeg);

    // wants: an HTTP camera widget asks for bit 14
    Layout l;
    l.columns.resize(1);
    l.columns[0].widgets = {httpCamera(1)};
    CHECK(CameraFeed::wantsFor(l) == wantBit(6));
    CHECK(CameraFeed::wantsFor(l) == (1u << 14));
}

void testHttpCameraPaintRotation() {
    CameraFeed feed;
    // 64x48, white on the left, black on the right, relayed as the camera sent it
    const Bytes j = makeHalvesJpeg(64, 48, true);
    CHECK(!j.empty());
    const Bytes b = liveBlob(6, 64, 48, 1, j);
    CHECK(feed.onBlob(6, b.data(), b.size(), 10.0));

    // rotation 0: as is. Sample a quarter in from each side.
    CHECK(paintedAt(feed, httpCamera(0), 10.1, 0.25, 0.5) > 235);
    CHECK(paintedAt(feed, httpCamera(0), 10.1, 0.75, 0.5) < 20);
    // rotation 1 (clockwise): the left side goes to the top, the tile is portrait
    CHECK(paintedAt(feed, httpCamera(1), 10.1, 0.5, 0.25) > 235);
    CHECK(paintedAt(feed, httpCamera(1), 10.1, 0.5, 0.75) < 20);
    // rotation 2: mirrored left to right
    CHECK(paintedAt(feed, httpCamera(2), 10.1, 0.25, 0.5) < 20);
    CHECK(paintedAt(feed, httpCamera(2), 10.1, 0.75, 0.5) > 235);
    // rotation 3: the left side goes to the bottom
    CHECK(paintedAt(feed, httpCamera(3), 10.1, 0.5, 0.25) < 20);
    CHECK(paintedAt(feed, httpCamera(3), 10.1, 0.5, 0.75) > 235);

    // a picture whose top is white, rotation 1: the top goes to the right
    CameraFeed feed2;
    const Bytes tj = makeHalvesJpeg(64, 48, false);
    const Bytes tb = liveBlob(6, 64, 48, 1, tj);
    CHECK(feed2.onBlob(6, tb.data(), tb.size(), 10.0));
    CHECK(paintedAt(feed2, httpCamera(1), 10.1, 0.75, 0.5) > 235);
    CHECK(paintedAt(feed2, httpCamera(1), 10.1, 0.25, 0.5) < 20);
    CHECK(paintedAt(feed2, httpCamera(0), 10.1, 0.5, 0.25) > 235);
    CHECK(paintedAt(feed2, httpCamera(0), 10.1, 0.5, 0.75) < 20);

    // the turn is the HTTP camera widget's: a helper camera widget ignores the rotation field
    WidgetInstance headsetTurned = headset(0x01);
    headsetTurned.rotation = 1;
    CameraFeed feed3;
    const Bytes hb = liveBlob(0, 64, 48, 1, j);
    CHECK(feed3.onBlob(0, hb.data(), hb.size(), 10.0));
    CHECK(paintedAt(feed3, headsetTurned, 10.1, 0.1, 0.5) == paintedAt(feed3, headset(0x01), 10.1, 0.1, 0.5));

    // stale: nothing painted; an extra HTTP camera: nothing painted either
    CHECK(paintedAt(feed, httpCamera(1), 10.0 + CameraFeed::kStaleSec + 1.0, 0.5, 0.25) == 0);
    WidgetInstance extra = httpCamera(0);
    extra.httpCameraExtra = true;
    CHECK(paintedAt(feed, extra, 10.1, 0.25, 0.5) == 0);
}

}  // namespace

int main() {
    testTwoCameras();
    testStale();
    testStateBlobs();
    testRejects();
    testReset();
    testWantsFor();
    testPaint();
    testHttpCameraBlobs();
    testHttpCameraPaintRotation();
    if (failures == 0) std::printf("pc_camera_feed_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
