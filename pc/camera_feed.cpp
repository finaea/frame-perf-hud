// T10: see camera_feed.h
#include "camera_feed.h"

#include <turbojpeg.h>

#include <cstring>

namespace fph {
namespace pc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

CameraFeed::CameraFeed() : tj_(tjInitDecompress()) {}

CameraFeed::~CameraFeed() {
    reset();
    if (tj_) tjDestroy(static_cast<tjhandle>(tj_));
}

void CameraFeed::reset() {
    for (Cam& c : cams_) {
        if (c.surface) cairo_surface_destroy(c.surface);
        c = Cam {};
    }
    ++revision_;
}

bool CameraFeed::onBlob(uint8_t stream, const uint8_t* data, size_t size, double now) {
    cam::BlobHeader h;
    const uint8_t* jpeg = nullptr;
    size_t jpegSize = 0;
    if (!cam::decodeCameraBlob(data, size, h, jpeg, jpegSize) || h.camera != stream) {
        ++rejected_;
        return false;
    }
    Cam& c = cams_[h.camera];
    if (h.state != cam::CameraState::Live) {
        if (c.reported != h.state || c.live) ++revision_;
        c.reported = h.state;
        c.live = false;  // the headset says it has nothing: don't keep showing the last picture
        return true;
    }
    if (!tj_) return false;
    int w = 0, hgt = 0, sub = 0, colorspace = 0;
    if (tjDecompressHeader3(static_cast<tjhandle>(tj_), jpeg, static_cast<unsigned long>(jpegSize), &w, &hgt, &sub,
                            &colorspace) != 0 ||
        w != h.width || hgt != h.height || w <= 0 || hgt <= 0 || w > 4096 || hgt > 4096) {
        ++rejected_;
        return false;
    }
    grey_.resize(static_cast<size_t>(w) * hgt);
    if (tjDecompress2(static_cast<tjhandle>(tj_), jpeg, static_cast<unsigned long>(jpegSize), grey_.data(), w, w, hgt,
                      TJPF_GRAY, TJFLAG_FASTDCT) != 0) {
        ++rejected_;
        return false;
    }
    if (!c.surface || c.width != w || c.height != hgt) {
        if (c.surface) cairo_surface_destroy(c.surface);
        c.surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, hgt);
        c.width = w;
        c.height = hgt;
    }
    cairo_surface_flush(c.surface);
    unsigned char* px = cairo_image_surface_get_data(c.surface);
    const int stride = cairo_image_surface_get_stride(c.surface);
    for (int y = 0; y < hgt; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(px + static_cast<size_t>(y) * stride);
        const uint8_t* g = grey_.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) row[x] = 0xFF000000u | uint32_t(g[x]) << 16 | uint32_t(g[x]) << 8 | g[x];
    }
    cairo_surface_mark_dirty(c.surface);
    c.havePicture = true;
    c.frameId = h.frameId;
    c.at = now;
    c.live = true;
    c.reported = cam::CameraState::Live;
    ++decoded_;
    ++revision_;
    return true;
}

cam::CameraState CameraFeed::state(uint8_t camera, double now) const {
    if (camera >= cam::kCameraCount) return cam::CameraState::Off;
    const Cam& c = cams_[camera];
    if (c.live && c.havePicture && now - c.at < kStaleSec) return cam::CameraState::Live;
    return c.reported == cam::CameraState::Live ? cam::CameraState::Starting : c.reported;
}

void CameraFeed::tick(double now) {
    for (Cam& c : cams_) {
        if (c.live && now - c.at >= kStaleSec) {
            c.live = false;
            if (c.reported == cam::CameraState::Live) c.reported = cam::CameraState::Starting;
            ++revision_;
        }
    }
}

uint32_t CameraFeed::wantsFor(const Layout& layout) {
    const uint8_t mask = cam::camerasInLayout(layout);
    uint32_t wants = 0;
    for (uint8_t c = 0; c < cam::kCameraCount; ++c)
        if (mask & (1u << c)) wants |= cam::wantBit(c);
    return wants;
}

void CameraFeed::paint(cairo_t* cr, const GridGeometry& geometry, double scale, double now) const {
    for (const WidgetSlot& slot : geometry.slots) {
        if (!isCameraWidget(slot.widget.type)) continue;
        // HTTP camera: the headset relays its JPEGs as they came, so the picture is turned here, by the widget's
        // rotation (clockwise quarter turns, like the headset's tiles)
        const int turns = slot.widget.type == WidgetType::HttpCamera ? slot.widget.rotation & 3 : 0;
        for (const cam::Tile& tile : cam::cameraTiles(slot.widget)) {
            const Cam& c = cams_[tile.camera];
            if (!c.surface || state(tile.camera, now) != cam::CameraState::Live) continue;
            const int shownW = (turns & 1) ? c.height : c.width;
            const int shownH = (turns & 1) ? c.width : c.height;
            const cam::Rect r = cam::fitFrame(tile, shownW, shownH);
            cairo_save(cr);
            cairo_scale(cr, scale, scale);
            cairo_rectangle(cr, slot.x + r.x, slot.y + r.y, r.w, r.h);
            cairo_clip(cr);
            // about the rect's centre, then the picture's own size centred on it
            cairo_translate(cr, slot.x + r.x + r.w / 2, slot.y + r.y + r.h / 2);
            cairo_rotate(cr, turns * kPi / 2);
            cairo_scale(cr, r.w / shownW, r.h / shownH);
            cairo_translate(cr, -c.width / 2.0, -c.height / 2.0);
            cairo_set_source_surface(cr, c.surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
}

}  // namespace pc
}  // namespace fph
