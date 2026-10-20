// T10: the camera widgets' shared pieces (headset and PC). Platform-free: camera ids and link stream ids, the
// widget's tile geometry, the outward cams' frame copy + dark-frame filter, the post-copy recheck gate, and the
// camera BLOB's payload. The headset's camera thread (headset/camera_service.*) does the I/O around them.
#pragma once

#include "layout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fph {
namespace cam {

// ------------------------------------------------------------------------------------------------ ids

/**
 * Camera ids, used as the link's BLOB stream id and in SUBSCRIBE wants bits 8-15 (bit 8 + id).
 * 0-3 = the outward cams (/dev/video0, 3, 9, 13: the helper's cam index), 4-5 = the two eye cams, 6 = the HTTP
 * camera widget's stream (headset/http_camera.*, not the helper).
 */
constexpr uint8_t kOutwardCount = 4;
constexpr uint8_t kEyeBase = 4;
constexpr uint8_t kHttpCamera = 6;
constexpr uint8_t kCameraCount = 7;
/** The helper's cameras (0-5): what CameraService handles. */
constexpr uint8_t kHelperCameraMask = 0x3F;

inline bool isOutward(uint8_t id) { return id < kOutwardCount; }
inline bool isEye(uint8_t id) { return id >= kEyeBase && id < kEyeBase + 2; }

/** @return the SUBSCRIBE wants bit for a camera */
inline uint32_t wantBit(uint8_t id) { return 1u << (8 + id); }
/**
 * Every camera's wants bit. Bit 14 (HTTP camera) is only ever set for a headset whose layout holds the widget (the
 * PC asks for camerasInLayout): an older headset's SUBSCRIBE decoder doesn't expect the fps byte then and would
 * reject the whole SUBSCRIBE, but an older headset can't hold widget 14 either.
 */
constexpr uint32_t kWantCameraMask = 0x7Fu << 8;
/**
 * SUBSCRIBE wants bit 31: this SUBSCRIBE only registers the address camera frames go to (the PC's dongle path);
 * it doesn't renew or move the lease. See LinkServer::onSubscribe.
 */
constexpr uint32_t kWantCameraPathOnly = 1u << 31;

/** @return a 7-bit mask (bit = camera id) from SUBSCRIBE wants */
inline uint8_t camerasFromWants(uint32_t wants) { return static_cast<uint8_t>((wants >> 8) & 0x7F); }

/** @return "video0" ... "video13", "Eye 1", "Eye 2", "HTTP camera" */
const char* cameraName(uint8_t id);

/**
 * The cameras a layout shows: each Headset camera widget's outward mask, both eyes for an Eye camera widget, id 6
 * for an HTTP camera widget.
 * @return a 7-bit mask (bit = camera id)
 */
uint8_t camerasInLayout(const Layout& layout);

/** @return true if any Headset camera widget in the layout asks for full-size frames (the helper's outward cams) */
bool fullSizeInLayout(const Layout& layout);

/**
 * The HTTP camera widget that's live: the first one in column order (layoutGrid's order). Any later one only shows
 * "Only one HTTP camera at a time".
 * @return it, or nullptr when the layout has none
 */
const WidgetInstance* httpCameraIn(const Layout& layout);

/**
 * The headset frame rate per camera: the highest `fps` of the widgets showing it (two widgets showing one camera
 * share its frames, so it runs at the faster one's rate). 0 for a camera no widget shows.
 */
void displayFpsInLayout(const Layout& layout, double (&fps)[kCameraCount]);

// ------------------------------------------------------------------------------------------------ state

/** What a camera tile shows when there's no live frame. Stable: it goes over the link. */
enum class CameraState : uint8_t {
    Live = 0,            ///< frames are coming
    Starting = 1,        ///< helper starting / ring being located
    NotWorn = 2,         ///< the headset is in standby: the cameras pause
    HelperMissing = 3,   ///< the helper isn't installed (or lost its capability)
    HelperOutdated = 4,  ///< the installed helper speaks another protocol version
    Unavailable = 5,     ///< SteamVR / XRService / the eye tracker isn't running, or this cam isn't there
    Off = 6,             ///< nothing asked for it
    // HTTP camera (id 6)
    Connecting = 7,      ///< connecting / waiting for the first frame
    Unreachable = 8,     ///< the connection failed or dropped (retried every second)
    NotMjpeg = 9,        ///< it answered, but not with a multipart/x-mixed-replace stream
};

/**
 * @param hostPort the HTTP camera's "host:port" (httpUrlHostPort), for its own states
 * @return the tile's text ("Headset not worn", "Camera helper not installed: run `sudo ...`", ...)
 */
std::string cameraStateText(CameraState state, const std::string& hostPort = "");

// ------------------------------------------------------------------------------------------------ tiles

/** Where one camera's picture sits in a camera widget's card (card-relative px, unscaled). */
struct Tile {
    uint8_t camera = 0;
    int x = 0;       ///< image area left
    int y = 0;       ///< image area top
    int w = 0;
    int h = 0;
    int labelY = 0;  ///< baseline row of the camera's label, above the image area
};

/**
 * @return the native size of a camera's frames (outward 0/1: 1056×1024, 2/3: 640×480, eyes 512×400; the HTTP camera
 *         is taken as 640×480, its tile's aspect before rotation)
 */
void nativeSize(uint8_t camera, int& width, int& height);

/**
 * The tiles of a Headset camera (12), Eye camera (13) or HTTP camera (14) widget, in id order. One outward cam fills
 * the card's width; two or more sit two per row; the eyes sit side by side. The HTTP camera is one tile across the
 * card, 4:3, or 3:4 when turned a quarter (other aspects letterbox through fitFrame).
 * @return empty for other types, and for an HTTP camera that isn't the live one (httpCameraExtra)
 */
std::vector<Tile> cameraTiles(const WidgetInstance& w);

/** @return the card height of a camera widget (fixed per settings, so the grid only moves on layout changes) */
int cameraWidgetHeight(const WidgetInstance& w);

/** A rectangle in px. */
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
};

/** @return the largest rect of the frame's aspect inside the tile, centred ("contain") */
Rect fitFrame(const Tile& tile, int frameW, int frameH);

// ------------------------------------------------------------------------------------------------ frames

/**
 * Optional cache maintenance before reading a buffer the camera hardware wrote (the headset passes dc civac over
 * the range + dsb ish, camviewer/cachecopy.c:19-34; tests pass nothing).
 */
struct CacheOps {
    void (*dropRange)(const void* p, size_t len) = nullptr;  ///< clean+invalidate [p, p+len) and barrier
};

/**
 * Copies one luma plane out of a camera buffer: rows of `width` bytes, `pitch` apart (NV12's Y plane or GREY).
 * With downscale 2 each output pixel is the 2×2 box average. Cache maintenance covers the source range first.
 * @param dst (width / downscale) × (height / downscale) bytes
 * @param flipV write the rows bottom-up
 */
void copyLuma(uint8_t* dst, const uint8_t* src, int width, int height, size_t pitch, int downscale, bool flipV,
              const CacheOps& cache = {});

/**
 * Mean of a sparse grid of samples (every stepX-th byte of every stepY-th row), for the dark-frame filter. Only the
 * sampled lines get cache maintenance, so it's cheap enough to run before deciding what to copy.
 */
double sampleLevel(const uint8_t* src, int width, int height, size_t pitch, const CacheOps& cache = {},
                   int stepX = 33, int stepY = 32);

/** Grey → opaque RGBA. */
void greyToRgba(uint8_t* dst, const uint8_t* grey, size_t pixels);

/**
 * Turns a grey image clockwise by quarter turns (Toggle's preview rotation, so 270 here = 270 in Baballonia).
 * @param dst width × height bytes (0, 2 turns) or height × width (1, 3 turns); not src
 * @param turns 0-3 (taken mod 4)
 */
void rotateGrey(uint8_t* dst, const uint8_t* src, int width, int height, int turns);

/**
 * The outward cams alternate a normal exposure with a near-black one (~68 fps, camviewer.py:274-280). This learns
 * which sequence parity is dark from brightness (DARK_RATIO 0.5, camviewer.py:55): a parity whose running level is
 * under half the other's is dark. Until both parities have a level, or when they look alike, nothing is dark.
 */
class DarkParityFilter {
public:
    static constexpr double kDarkRatio = 0.5;

    /** Feeds one frame's level (sampleLevel) under its V4L2 sequence number. */
    void observe(uint32_t seq, double level);
    /** @return true if frames with this sequence's parity are the dark ones */
    bool isDark(uint32_t seq) const;
    /** @return true once both parities have a level */
    bool known() const { return have_[0] && have_[1]; }
    double level(int parity) const { return level_[parity & 1]; }
    void reset();

private:
    double level_[2] = {0.0, 0.0};
    bool have_[2] = {false, false};
};

/**
 * The newest sequence shown for one outward stream, so a frame is never shown twice or out of order. It belongs to
 * the buffers it was learned on: a fresh CAM (new gen) or CAM_GONE resets it, since a restarted stream's sequence
 * numbers may start low again (they'd all look old otherwise).
 */
class ShownWatermark {
public:
    void reset() { have_ = false; seq_ = 0; }
    void shown(uint32_t seq) { have_ = true; seq_ = seq; }
    bool have() const { return have_; }
    uint32_t seq() const { return seq_; }

private:
    bool have_ = false;
    uint32_t seq_ = 0;
};

/** One finished buffer from the helper's FRAMES report. */
struct FrameEntry {
    uint32_t index = 0;
    uint32_t seq = 0;
};

/**
 * Which entry to copy: the newest one that isn't dark and is newer than the last one shown. Entries are oldest first.
 * @return its position in entries, or -1
 */
int pickFrame(const std::vector<FrameEntry>& entries, const DarkParityFilter& filter, bool haveShown,
              uint32_t lastShownSeq);
inline int pickFrame(const std::vector<FrameEntry>& entries, const DarkParityFilter& filter, const ShownWatermark& w) {
    return pickFrame(entries, filter, w.have(), w.seq());
}

/**
 * The post-copy recheck (review R9): a copied outward frame is shown only if the helper answers KEEP for the same
 * (gen, buffer index, sequence). One copy in flight per camera. A new CAM (new gen), CAM_GONE or a helper restart
 * forgets the copy in flight, so a late answer can never publish a frame from old buffers.
 */
class RecheckGate {
public:
    enum class Verdict {
        Keep,     ///< publish the copy
        Discard,  ///< it was this copy, and it isn't good (NEWER / REQUEUED / STALE / INVALID)
        Ignore,   ///< not the copy in flight (old gen, other buffer, nothing in flight)
    };

    /** A CAM message: these buffers (gen) are now current, and nothing is in flight. */
    void onCam(uint32_t gen);
    /** CAM_GONE / helper gone: no buffers, nothing in flight. */
    void onGone();

    /** @return true if FRAMES with this gen describe the current buffers */
    bool currentGen(uint32_t gen) const { return haveGen_ && gen == gen_; }
    /** @return true if a copy may start (current buffers, nothing in flight; a lost answer frees it after 1 s) */
    bool canCopy(double now) const;
    /** A copy of (index, seq) was made at `now` and the recheck sent. */
    void begin(uint32_t index, uint32_t seq, double now);
    /** The helper's RECHECK answer. */
    Verdict onRecheck(uint32_t gen, uint32_t index, uint32_t seq, uint8_t result);
    bool inFlight() const { return inFlight_; }

    /** The helper's result codes (fph_camgrab_proto.h), repeated so core stays platform-free. */
    static constexpr uint8_t kKeep = 0;
    static constexpr uint8_t kStale = 3;
    static constexpr double kAnswerTimeoutSec = 1.0;

private:
    bool haveGen_ = false;
    uint32_t gen_ = 0;
    bool inFlight_ = false;
    uint32_t index_ = 0;
    uint32_t seq_ = 0;
    double startedAt_ = 0.0;
};

/**
 * Output rate limit with a schedule rather than "since the last frame", allowing a quarter interval of slack
 * (framestream.c rate_ok): frames are only noticed on a poll, so a strict test lands well under the target.
 */
class RateGate {
public:
    explicit RateGate(double fps = 15.0) { setFps(fps); }
    void setFps(double fps) { interval_ = fps > 0 ? 1.0 / fps : 0.0; }
    /** @return true (and schedules the next one) if a frame may go out at t */
    bool take(double t);
    /** @return true if take(t) would return true (nothing scheduled) */
    bool ready(double t) const { return interval_ <= 0.0 || t >= next_ - interval_ / 4.0; }
    /** @return when the next frame is due */
    double nextDue() const { return next_; }

private:
    double interval_ = 0.0;
    double next_ = 0.0;
};

// ------------------------------------------------------------------------------------------------ link

/** The camera BLOB's header (then JPEG bytes when state is Live). Little-endian. */
struct BlobHeader {
    static constexpr uint8_t kVersion = 1;
    static constexpr size_t kSize = 12;
    uint8_t camera = 0;
    CameraState state = CameraState::Live;
    uint16_t width = 0;   ///< JPEG size (0 when not Live)
    uint16_t height = 0;
    uint32_t frameId = 0;  ///< bumps per frame sent for this camera
};

/** @return header + jpeg as a Camera BLOB's data (split with link::splitBlob, stream id = camera) */
std::vector<uint8_t> encodeCameraBlob(const BlobHeader& header, const uint8_t* jpeg, size_t jpegSize);

/**
 * @param jpeg points into data (nullptr, 0 when not Live)
 * @return false if malformed or another version
 */
bool decodeCameraBlob(const uint8_t* data, size_t size, BlobHeader& header, const uint8_t*& jpeg, size_t& jpegSize);

}  // namespace cam
}  // namespace fph
