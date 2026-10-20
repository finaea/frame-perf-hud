// T10: see camera_frame.h
#include "camera_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fph {
namespace cam {

namespace {

// Card geometry (card-relative px). Matches core/render/widgets.cpp: 496 px card, content 6 px in, title
// baseline at 23 px.
constexpr int kContentX = 6;
constexpr int kContentW = 484;
constexpr int kTilesTop = 30;
constexpr int kLabelH = 20;  ///< label row above each image
constexpr int kGap = 8;
constexpr int kBottom = 10;
constexpr int kHalfW = (kContentW - kGap) / 2;  // 238

int roundDiv(int a, int b) {
    return (a + b / 2) / b;
}

}  // namespace

const char* cameraName(uint8_t id) {
    static const char* const kNames[kCameraCount] = {"video0", "video3", "video9", "video13", "Eye 1", "Eye 2",
                                                      "HTTP camera"};
    return id < kCameraCount ? kNames[id] : "?";
}

uint8_t camerasInLayout(const Layout& layout) {
    uint8_t mask = 0;
    for (const LayoutColumn& column : layout.columns) {
        for (const WidgetInstance& w : column.widgets) {
            if (w.type == WidgetType::HeadsetCamera) mask |= static_cast<uint8_t>(w.cameras & 0x0F);
            if (w.type == WidgetType::EyeCamera) mask |= static_cast<uint8_t>(0x3u << kEyeBase);
            if (w.type == WidgetType::HttpCamera) mask |= static_cast<uint8_t>(1u << kHttpCamera);
        }
    }
    return mask;
}

const WidgetInstance* httpCameraIn(const Layout& layout) {
    for (const LayoutColumn& column : layout.columns)
        for (const WidgetInstance& w : column.widgets)
            if (w.type == WidgetType::HttpCamera) return &w;
    return nullptr;
}

bool fullSizeInLayout(const Layout& layout) {
    for (const LayoutColumn& column : layout.columns)
        for (const WidgetInstance& w : column.widgets)
            if (w.type == WidgetType::HeadsetCamera && w.cameraFullSize) return true;
    return false;
}

void displayFpsInLayout(const Layout& layout, double (&fps)[kCameraCount]) {
    for (double& f : fps) f = 0.0;
    for (const LayoutColumn& column : layout.columns) {
        for (const WidgetInstance& w : column.widgets) {
            uint8_t mask = 0;
            if (w.type == WidgetType::HeadsetCamera) mask = static_cast<uint8_t>(w.cameras & 0x0F ? w.cameras & 0x0F : 0x01);
            if (w.type == WidgetType::EyeCamera) mask = static_cast<uint8_t>(0x3u << kEyeBase);
            for (uint8_t c = 0; c < kCameraCount; ++c)
                if (mask & (1u << c)) fps[c] = std::max(fps[c], static_cast<double>(w.cameraFps));
        }
    }
    if (const WidgetInstance* http = httpCameraIn(layout)) fps[kHttpCamera] = http->cameraFps;  // only the live one
}

std::string cameraStateText(CameraState state, const std::string& hostPort) {
    switch (state) {
        case CameraState::Live: return "";
        case CameraState::Starting: return "Starting the camera...";
        case CameraState::NotWorn: return "Headset not worn";
        case CameraState::HelperMissing:
            return "Camera helper not installed: run `sudo ~/.local/share/frame-perf-hud/install-camera-helper.sh`";
        case CameraState::HelperOutdated:
            return "Camera helper is outdated: run `sudo ~/.local/share/frame-perf-hud/install-camera-helper.sh`";
        case CameraState::Unavailable: return "Camera unavailable (is SteamVR running?)";
        case CameraState::Off: return "Camera off";
        case CameraState::Connecting: return "Connecting to " + hostPort + "...";
        case CameraState::Unreachable: return "Can't reach " + hostPort + ". Is the stream running?";
        case CameraState::NotMjpeg: return hostPort + " isn't an MJPEG stream";
    }
    return "";
}

// ------------------------------------------------------------------------------------------------ tiles

void nativeSize(uint8_t camera, int& width, int& height) {
    if (camera < 2) {
        width = 1056;
        height = 1024;
    } else if (camera < kOutwardCount || camera == kHttpCamera) {
        width = 640;
        height = 480;
    } else {
        width = 512;
        height = 400;
    }
}

std::vector<Tile> cameraTiles(const WidgetInstance& w) {
    std::vector<uint8_t> ids;
    if (w.type == WidgetType::HeadsetCamera) {
        for (uint8_t c = 0; c < kOutwardCount; ++c)
            if (w.cameras & (1u << c)) ids.push_back(c);
        if (ids.empty()) ids.push_back(0);  // a hand-edited 0 mask still shows something
    } else if (w.type == WidgetType::EyeCamera) {
        ids = {kEyeBase, static_cast<uint8_t>(kEyeBase + 1)};
    } else if (w.type == WidgetType::HttpCamera && !w.httpCameraExtra) {
        Tile t;
        t.camera = kHttpCamera;
        // Size: a share of the card's width, centred (a turned 3:4 picture at full width is very tall)
        t.w = roundDiv(kContentW * cameraSizeNearest(w.cameraSize), 100);
        t.x = kContentX + (kContentW - t.w) / 2;
        t.labelY = kTilesTop + 14;
        t.y = kTilesTop + kLabelH;
        t.h = (w.rotation & 1) ? roundDiv(t.w * 4, 3) : roundDiv(t.w * 3, 4);
        return {t};
    } else {
        return {};
    }
    std::vector<Tile> tiles;
    if (ids.size() == 1) {
        int nw = 0, nh = 0;
        nativeSize(ids[0], nw, nh);
        Tile t;
        t.camera = ids[0];
        t.x = kContentX;
        t.labelY = kTilesTop + 14;
        t.y = kTilesTop + kLabelH;
        t.w = kContentW;
        t.h = roundDiv(kContentW * nh, nw);
        tiles.push_back(t);
        return tiles;
    }
    // Two per row; every tile the same size (the first camera's aspect: 1056x1024 or the eyes' 512x400)
    int nw = 0, nh = 0;
    nativeSize(ids[0], nw, nh);
    const int h = roundDiv(kHalfW * nh, nw);
    for (size_t k = 0; k < ids.size(); ++k) {
        const int row = static_cast<int>(k / 2);
        const int col = static_cast<int>(k % 2);
        const int rowTop = kTilesTop + row * (kLabelH + h + kGap);
        Tile t;
        t.camera = ids[k];
        t.x = kContentX + col * (kHalfW + kGap);
        t.labelY = rowTop + 14;
        t.y = rowTop + kLabelH;
        t.w = kHalfW;
        t.h = h;
        tiles.push_back(t);
    }
    return tiles;
}

int cameraWidgetHeight(const WidgetInstance& w) {
    WidgetInstance live = w;
    live.httpCameraExtra = false;  // the "only one" card keeps the height its own settings give it
    const std::vector<Tile> tiles = cameraTiles(live);
    int bottom = kTilesTop;
    for (const Tile& t : tiles) bottom = std::max(bottom, t.y + t.h);
    return bottom + kBottom;
}

Rect fitFrame(const Tile& tile, int frameW, int frameH) {
    Rect r {static_cast<double>(tile.x), static_cast<double>(tile.y), static_cast<double>(tile.w),
            static_cast<double>(tile.h)};
    if (frameW <= 0 || frameH <= 0 || tile.w <= 0 || tile.h <= 0) return r;
    const double scale = std::min(double(tile.w) / frameW, double(tile.h) / frameH);
    r.w = frameW * scale;
    r.h = frameH * scale;
    r.x = tile.x + (tile.w - r.w) / 2.0;
    r.y = tile.y + (tile.h - r.h) / 2.0;
    return r;
}

// ------------------------------------------------------------------------------------------------ frames

void copyLuma(uint8_t* dst, const uint8_t* src, int width, int height, size_t pitch, int downscale, bool flipV,
              const CacheOps& cache) {
    if (width <= 0 || height <= 0) return;
    if (cache.dropRange) cache.dropRange(src, pitch * static_cast<size_t>(height - 1) + static_cast<size_t>(width));
    if (downscale <= 1) {
        for (int y = 0; y < height; ++y) {
            const int oy = flipV ? height - 1 - y : y;
            std::memcpy(dst + static_cast<size_t>(oy) * width, src + static_cast<size_t>(y) * pitch,
                        static_cast<size_t>(width));
        }
        return;
    }
    const int ow = width / 2;
    const int oh = height / 2;
    for (int y = 0; y < oh; ++y) {
        const uint8_t* a = src + static_cast<size_t>(2 * y) * pitch;
        const uint8_t* b = a + pitch;
        uint8_t* d = dst + static_cast<size_t>(flipV ? oh - 1 - y : y) * ow;
        for (int x = 0; x < ow; ++x) {
            d[x] = static_cast<uint8_t>((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
        }
    }
}

double sampleLevel(const uint8_t* src, int width, int height, size_t pitch, const CacheOps& cache, int stepX,
                   int stepY) {
    if (width <= 0 || height <= 0) return 0.0;
    stepX = std::max(1, stepX);
    stepY = std::max(1, stepY);
    if (cache.dropRange) {
        // only the sampled rows (1 in stepY), so this stays far cheaper than the copy
        for (int y = 0; y < height; y += stepY) cache.dropRange(src + static_cast<size_t>(y) * pitch, width);
    }
    uint64_t sum = 0, n = 0;
    for (int y = 0; y < height; y += stepY) {
        const uint8_t* row = src + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < width; x += stepX) {
            sum += row[x];
            ++n;
        }
    }
    return n ? double(sum) / double(n) : 0.0;
}

void greyToRgba(uint8_t* dst, const uint8_t* grey, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t v = grey[i];
        dst[4 * i] = v;
        dst[4 * i + 1] = v;
        dst[4 * i + 2] = v;
        dst[4 * i + 3] = 255;
    }
}

void rotateGrey(uint8_t* dst, const uint8_t* src, int width, int height, int turns) {
    turns &= 3;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (turns == 0) {
        std::memcpy(dst, src, n);
        return;
    }
    if (turns == 2) {
        for (size_t i = 0; i < n; ++i) dst[i] = src[n - 1 - i];
        return;
    }
    // A quarter turn: the output is height wide and width tall (toggles/src/preview.cpp rotateInto)
    const int ow = height;
    for (int yo = 0; yo < width; ++yo) {
        uint8_t* row = dst + static_cast<size_t>(yo) * ow;
        for (int xo = 0; xo < ow; ++xo) {
            const int xi = turns == 1 ? yo : width - 1 - yo;
            const int yi = turns == 1 ? height - 1 - xo : xo;
            row[xo] = src[static_cast<size_t>(yi) * width + xi];
        }
    }
}

void DarkParityFilter::observe(uint32_t seq, double level) {
    const int p = static_cast<int>(seq & 1u);
    level_[p] = have_[p] ? 0.8 * level_[p] + 0.2 * level : level;  // camviewer.py:262
    have_[p] = true;
}

bool DarkParityFilter::isDark(uint32_t seq) const {
    if (!known()) return false;
    const int p = static_cast<int>(seq & 1u);
    return level_[p] < kDarkRatio * level_[p ^ 1];
}

void DarkParityFilter::reset() {
    have_[0] = have_[1] = false;
    level_[0] = level_[1] = 0.0;
}

int pickFrame(const std::vector<FrameEntry>& entries, const DarkParityFilter& filter, bool haveShown,
              uint32_t lastShownSeq) {
    for (int k = static_cast<int>(entries.size()) - 1; k >= 0; --k) {
        const FrameEntry& e = entries[static_cast<size_t>(k)];
        if (haveShown && static_cast<int32_t>(e.seq - lastShownSeq) <= 0) return -1;  // oldest first: nothing newer
        if (!filter.isDark(e.seq)) return k;
    }
    return -1;
}

void RecheckGate::onCam(uint32_t gen) {
    haveGen_ = true;
    gen_ = gen;
    inFlight_ = false;
}

void RecheckGate::onGone() {
    haveGen_ = false;
    inFlight_ = false;
}

bool RecheckGate::canCopy(double now) const {
    return haveGen_ && (!inFlight_ || now - startedAt_ > kAnswerTimeoutSec);
}

void RecheckGate::begin(uint32_t index, uint32_t seq, double now) {
    inFlight_ = true;
    index_ = index;
    seq_ = seq;
    startedAt_ = now;
}

RecheckGate::Verdict RecheckGate::onRecheck(uint32_t gen, uint32_t index, uint32_t seq, uint8_t result) {
    if (!inFlight_ || !haveGen_ || gen != gen_ || index != index_ || seq != seq_) return Verdict::Ignore;
    inFlight_ = false;
    if (result == kKeep) return Verdict::Keep;
    if (result == kStale) haveGen_ = false;  // a fresh CAM follows; until then nothing is copied
    return Verdict::Discard;
}

bool RateGate::take(double t) {
    if (interval_ <= 0.0) return true;
    if (t < next_ - interval_ / 4.0) return false;
    // after a gap, restart the schedule instead of bursting to catch up
    next_ = (next_ < t - interval_ ? t : next_) + interval_;
    return true;
}

// ------------------------------------------------------------------------------------------------ link

std::vector<uint8_t> encodeCameraBlob(const BlobHeader& h, const uint8_t* jpeg, size_t jpegSize) {
    const bool withJpeg = h.state == CameraState::Live && jpeg != nullptr;
    std::vector<uint8_t> out(BlobHeader::kSize + (withJpeg ? jpegSize : 0), 0);
    out[0] = BlobHeader::kVersion;
    out[1] = h.camera;
    out[2] = static_cast<uint8_t>(h.state);
    // out[3] reserved
    out[4] = static_cast<uint8_t>(h.width);
    out[5] = static_cast<uint8_t>(h.width >> 8);
    out[6] = static_cast<uint8_t>(h.height);
    out[7] = static_cast<uint8_t>(h.height >> 8);
    for (int k = 0; k < 4; ++k) out[8 + k] = static_cast<uint8_t>(h.frameId >> (8 * k));
    if (withJpeg && jpegSize) std::memcpy(out.data() + BlobHeader::kSize, jpeg, jpegSize);
    return out;
}

bool decodeCameraBlob(const uint8_t* data, size_t size, BlobHeader& h, const uint8_t*& jpeg, size_t& jpegSize) {
    jpeg = nullptr;
    jpegSize = 0;
    if (data == nullptr || size < BlobHeader::kSize || data[0] != BlobHeader::kVersion) return false;
    h.camera = data[1];
    if (h.camera >= kCameraCount || data[2] > static_cast<uint8_t>(CameraState::NotMjpeg)) return false;
    h.state = static_cast<CameraState>(data[2]);
    h.width = static_cast<uint16_t>(data[4] | data[5] << 8);
    h.height = static_cast<uint16_t>(data[6] | data[7] << 8);
    h.frameId = static_cast<uint32_t>(data[8]) | static_cast<uint32_t>(data[9]) << 8 |
                static_cast<uint32_t>(data[10]) << 16 | static_cast<uint32_t>(data[11]) << 24;
    if (h.state == CameraState::Live) {
        if (size == BlobHeader::kSize || h.width == 0 || h.height == 0) return false;
        jpeg = data + BlobHeader::kSize;
        jpegSize = size - BlobHeader::kSize;
    } else if (size != BlobHeader::kSize) {
        return false;
    }
    return true;
}

}  // namespace cam
}  // namespace fph
