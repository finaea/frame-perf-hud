// T10: finding and reading the eye cameras' frames. They aren't V4L2 devices: the frames only appear in the udmabuf
// arenas XRService shares with the eye tracker (the helper hands them over), 512×400 8-bit grey, one frame per slot
// of a ring whose offsets move every session. This is FrameEyeCameraFeed's framestream ring discovery and "ring is
// live" check, ported from C (framestream.c:566-1306, :1466-1497, :1526-1708).
//
//   framestream, Copyright (c) 2026 Curtis English, MIT License (see THIRD_PARTY_LICENSES.md).
//   https://github.com/Curtis-VL/FrameEyeCameraFeed, commit 309c41d.
//
// Changes from framestream: no globals (one EyeRingFinder per arena set), the cache operations are passed in, the
// discovery's waits go through a callback that keeps the agent's camera thread serving (and can abort), and
// polling adds a tear guard: a slot is copied only once its fingerprint held still for one poll, and the copy is
// kept only if the fingerprint still matches afterwards.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace eyering {

constexpr int kEyeStride = 512;   ///< bytes per row (= width)
constexpr int kEyeHeight = 400;
constexpr size_t kEyeFrameBytes = static_cast<size_t>(kEyeStride) * kEyeHeight;
constexpr long kEyeSlot = 262144;  ///< nominal spacing; the real one is measured (262208 on Jack's Frame)
constexpr size_t kScanBlock = 65536;
constexpr int kMaxArenas = 8;
constexpr int kMaxBlocks = 1024;
constexpr int kMaxRing = 32;

/** One mapped arena (read-only). */
struct Arena {
    const uint8_t* map = nullptr;
    size_t size = 0;
};

/** The ring: one origin, one pitch, N slots. */
struct Ring {
    int arena = 0;
    int slots = 0;
    long pitch = kEyeSlot;
    size_t off[kMaxRing] = {};
    uint64_t fp[kMaxRing] = {};
    int eye[kMaxRing] = {};  ///< which physical eye (0 / 1) each slot holds; 0 = the eye further left in the image
    bool bothEyes = false;
};

/** Cache maintenance (camcache), so tests can run without it. */
struct Cache {
    void (*dropLine)(const void*) = nullptr;
    void (*barrier)() = nullptr;
};

/**
 * Sampled FNV-1a over len bytes (512 samples, or `samples`), with each sampled line dropped from the cache first so
 * the samples come from memory (framestream.c:287-307).
 */
uint64_t fingerprint(const uint8_t* p, size_t len, const Cache& cache, size_t samples = 512);

/** framestream.c:599-630: lit fraction 0.20-0.96 and roughness ≤ 15 (real eye frames vs recycled heap). */
bool looksLikeEyeFrame(const uint8_t* p);

/**
 * framestream.c:767-844: the frame's first row (a frame is 400 lit rows after a run of ≥ 8 dark ones), searched
 * ±560 rows around hint.
 * @return false if none
 */
bool findFrameStart(const uint8_t* base, size_t limit, size_t hint, size_t* out);

class EyeRingFinder {
public:
    /**
     * Called between discovery's samples (~every 4 ms). The agent keeps the helper socket and the outward cams
     * served there. @return false to abort the discovery (stop requested, eyes no longer wanted, arenas gone)
     */
    using Wait = std::function<bool()>;

    explicit EyeRingFinder(Cache cache = {}) : cache_(cache) {}

    /** New arenas (after the helper's EYE message): forgets the change tracking. */
    void setArenas(const std::vector<Arena>& arenas);
    void clear() { arenas_.clear(); }
    const std::vector<Arena>& arenas() const { return arenas_; }

    /** Change tracking: fingerprints every 64 KiB block and marks the ones that changed since the last call. */
    void noteBlockChanges();
    void clearBlockChanges();
    /** noteBlockChanges + clearBlockChanges: only changes from now on count. */
    void resetBlockChanges();

    /**
     * framestream.c:897-1306: candidates in changed memory that look like eye frames → the arena with the most →
     * kept only if refreshed ≥ 5 times in 1 s (the "ring is live" check) → spacing measured → ring laid out → slots
     * that don't refresh within 0.6 s dropped → eyes assigned per slot.
     * @param wait called between samples during the ~1.6 s of checks
     * @return true with out filled; false if there's no live ring (or wait aborted)
     */
    bool discover(Ring& out, const Wait& wait, double (*now)());

private:
    struct Track {
        uint64_t blockfp[kMaxBlocks] = {};
        unsigned char changed[kMaxBlocks] = {};
    };
    Cache cache_;
    std::vector<Arena> arenas_;
    std::vector<Track> tracks_;
};

/**
 * T10: when an unlocked search does its work: change tracking every 250 ms and a discovery attempt every 1 s
 * (framestream's loop), backing off after 15 s without a ring to one attempt per 10 s with tracking only in the
 * 1.25 s before it (tracking is a pass over the arenas). The camera thread's work and its poll() wake-up both come
 * from here, so a deadline the work isn't doing yet never turns into a zero timeout (a busy spin).
 */
class SearchSchedule {
public:
    static constexpr double kNoteSec = 0.25;
    static constexpr double kScanSec = 1.0;
    static constexpr double kSlowAfterSec = 15.0;
    static constexpr double kSlowScanSec = 10.0;
    static constexpr double kSlowNoteLeadSec = 1.25;

    /** New arenas: the first attempt comes after one second of tracking. */
    void reset(double now) {
        since_ = now;
        lastScan_ = now;
        nextNote_ = 0.0;
    }
    /** The lock was lost: searching starts over (fast again), keeping the last attempt's time. */
    void restart(double now) { since_ = now; }

    bool slow(double now) const { return now - since_ > kSlowAfterSec; }
    double scanEvery(double now) const { return slow(now) ? kSlowScanSec : kScanSec; }
    double scanAt(double now) const { return lastScan_ + scanEvery(now); }
    /** @return when change tracking is next due (in slow mode, not before the lead-in to the next attempt) */
    double noteAt(double now) const {
        return slow(now) ? std::max(nextNote_, scanAt(now) - kSlowNoteLeadSec) : nextNote_;
    }
    bool noteDue(double now) const { return now >= noteAt(now); }
    bool scanDue(double now) const { return now >= scanAt(now); }
    void noted(double now) { nextNote_ = now + kNoteSec; }
    void scanned(double now) { lastScan_ = now; }
    /** @return the earliest deadline (for poll's timeout) */
    double nextWake(double now) const { return std::min(noteAt(now), scanAt(now)); }

private:
    double since_ = 0.0;
    double lastScan_ = -1e9;
    double nextNote_ = 0.0;
};

/**
 * Reads a locked ring: which slots refreshed, and a tear-guarded copy of a slot.
 */
class RingReader {
public:
    explicit RingReader(Cache cache = {}, void (*dropRange)(const void*, size_t) = nullptr)
        : cache_(cache), dropRange_(dropRange) {}

    /** Starts reading this ring (fingerprints re-taken with the poll's sample count). */
    void lock(const Ring& ring, const Arena& arena, double now);
    void unlock() { locked_ = false; }
    bool locked() const { return locked_; }
    const Ring& ring() const { return ring_; }

    /**
     * One poll: fingerprints every slot (a cheap 128-sample one). A slot whose fingerprint changed is "fresh"; one
     * that changed on the previous poll and held still since is "settled" (written, and not being written now).
     * @return per eye (0 / 1), the slot that settled on this poll, or -1. If both eyes share one stream (one eye
     *         visible) it's reported as eye 0
     */
    void poll(double now, int settled[2]);

    /**
     * Copies a settled slot (cache-maintained) and checks its fingerprint again afterwards.
     * @param dst kEyeFrameBytes
     * @param flipV write the rows bottom-up
     * @return false if the slot changed during the copy (discard: it may be torn)
     */
    bool copySlot(int slot, uint8_t* dst, bool flipV);

    /** @return when any slot last changed */
    double lastChange() const { return lastAny_; }
    /** @return slot refreshes since lock (debug) */
    uint64_t refreshes() const { return refreshes_; }

    static constexpr size_t kPollSamples = 128;

private:
    Cache cache_;
    void (*dropRange_)(const void*, size_t);
    bool locked_ = false;
    Ring ring_;
    Arena arena_;
    uint64_t fp_[kMaxRing] = {};
    bool changedLast_[kMaxRing] = {};
    double lastAny_ = 0.0;
    uint64_t refreshes_ = 0;
};

}  // namespace eyering
