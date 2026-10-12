// T10: see camera_eye_ring.h. Ported from framestream.c (MIT, Copyright (c) 2026 Curtis English); the comments
// that explain the measurements are framestream's.
#include "camera_eye_ring.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace eyering {

namespace {

constexpr int kStride = kEyeStride;
constexpr int kHeight = kEyeHeight;

}  // namespace

uint64_t fingerprint(const uint8_t* p, size_t len, const Cache& cache, size_t samples) {
    uint64_t h = 1469598103934665603ULL;
    size_t step = samples ? len / samples : len;
    if (step == 0) step = 1;
    // make sure the samples come from memory, not a stale cache line
    if (cache.dropLine) {
        for (size_t i = 0; i < len; i += step) cache.dropLine(p + i);
        if (cache.barrier) cache.barrier();
    }
    for (size_t i = 0; i < len; i += step) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/*
 * Does this look like an eye frame?
 *
 * Measured on real frames versus the memory this used to lock onto by mistake:
 *
 *                       real eye frames     recycled heap
 *   lit fraction        0.27 - 0.34         0.09 - 0.15
 *   local roughness     6.3 - 7.2           3.5 - 3.8 (and 38 for noise)
 *
 * The lit fraction is what separates them. Roughness only bounds pure noise.
 */
static void frameStats(const uint8_t* p, double* litfrac, double* smooth) {
    long lit = 0, n = 0, nr = 0;
    double rough = 0;
    for (unsigned y = 0; y + 1 < kHeight; y += 4) {
        for (unsigned x = 0; x + 1 < kStride; x += 4) {
            const int v = p[static_cast<size_t>(y) * kStride + x];
            n++;
            if (v > 16) {
                lit++;
                rough += std::abs(v - p[static_cast<size_t>(y + 1) * kStride + x]);
                rough += std::abs(v - p[static_cast<size_t>(y) * kStride + x + 1]);
                nr += 2;
            }
        }
    }
    *litfrac = n ? static_cast<double>(lit) / n : 0;
    *smooth = nr ? rough / nr : 1e9;
}

bool looksLikeEyeFrame(const uint8_t* p) {
    double litfrac, smooth;
    frameStats(p, &litfrac, &smooth);
    return litfrac >= 0.20 && litfrac <= 0.96 && smooth <= 15.0;
}

/* average of each column - the shape used for all the alignment maths below */
static void columnProfile(const uint8_t* p, float* out) {
    for (unsigned x = 0; x < kStride; x++) {
        unsigned acc = 0;
        for (unsigned y = 0; y < kHeight; y += 8) acc += p[static_cast<size_t>(y) * kStride + x];
        out[x] = static_cast<float>(acc) / (kHeight / 8);
    }
}

static double profileDistance(const float* a, const float* b, unsigned rot) {
    double d = 0;
    for (unsigned x = 0; x < kStride; x += 2) d += std::fabs(a[(x + rot) % kStride] - b[x]);
    return d / (kStride / 2);
}

/* how far a has to be rotated to line up with b */
static unsigned bestRotation(const float* a, const float* b, double* dist) {
    unsigned best = 0;
    double bestd = 1e18;
    for (unsigned r = 0; r < kStride; r++) {
        const double d = profileDistance(a, b, r);
        if (d < bestd) {
            bestd = d;
            best = r;
        }
    }
    if (dist) *dist = bestd;
    return best;
}

/* mean brightness of the columns at the two frame edges */
static double edgeDarkness(const uint8_t* p) {
    double acc = 0;
    long n = 0;
    for (unsigned y = 0; y < kHeight; y += 4) {
        const uint8_t* r = p + static_cast<size_t>(y) * kStride;
        for (unsigned x = 0; x < 24; x++) {
            acc += r[x] + r[kStride - 1 - x];
            n += 2;
        }
    }
    return n ? acc / n : 1e9;
}

/*
 * Find where the picture starts, in rows: a frame is 400 lit rows followed by padding, so it begins on the first
 * lit row after a run of dark ones. A candidate from the 4 KiB scan grid can sit anywhere inside a frame, so this
 * searches a wide window.
 */
bool findFrameStart(const uint8_t* base, size_t limit, size_t hint, size_t* out) {
    const long hintRow = static_cast<long>(hint / kStride);
    const long maxrow = static_cast<long>(limit / kStride) - 1;
    long lo = hintRow - 560;
    long hi = hintRow + 560;
    if (lo < 0) lo = 0;
    if (hi > maxrow) hi = maxrow;
    if (hi - lo < 200) return false;
    int nrows = static_cast<int>(hi - lo + 1);
    if (nrows > 1200) nrows = 1200;
    double mean[1200];
    double total = 0;
    for (int k = 0; k < nrows; k++) {
        const uint8_t* r = base + static_cast<size_t>(lo + k) * kStride;
        double acc = 0;
        for (unsigned x = 0; x < kStride; x += 8) acc += r[x];
        mean[k] = acc / (kStride / 8);
        total += mean[k];
    }
    const double avg = total / nrows;
    double dark = avg * 0.18;
    if (dark < 1.5) dark = 1.5;
    long best = -1;
    long bestd = 1L << 30;
    int run = 0;
    for (int k = 0; k < nrows; k++) {
        if (mean[k] <= dark) {
            run++;
            continue;
        }
        if (run >= 8) {
            const long row = lo + k;
            const long d = std::labs(row - hintRow);
            if (d < bestd) {
                bestd = d;
                best = row;
            }
        }
        run = 0;
    }
    if (best < 0) return false;
    *out = static_cast<size_t>(best) * kStride;
    return true;
}

// ------------------------------------------------------------------------------------------------ finder

void EyeRingFinder::setArenas(const std::vector<Arena>& arenas) {
    arenas_.assign(arenas.begin(), arenas.begin() + std::min<size_t>(arenas.size(), kMaxArenas));
    tracks_.assign(arenas_.size(), Track {});
}

void EyeRingFinder::noteBlockChanges() {
    for (size_t a = 0; a < arenas_.size(); a++) {
        size_t b = 0;
        for (size_t o = 0; o + kScanBlock <= arenas_[a].size && b < kMaxBlocks; o += kScanBlock, b++) {
            const uint64_t h = fingerprint(arenas_[a].map + o, kScanBlock, cache_);
            if (h != tracks_[a].blockfp[b]) {
                tracks_[a].blockfp[b] = h;
                tracks_[a].changed[b] = 1;
            }
        }
    }
}

void EyeRingFinder::clearBlockChanges() {
    for (Track& t : tracks_) std::memset(t.changed, 0, sizeof(t.changed));
}

void EyeRingFinder::resetBlockChanges() {
    noteBlockChanges();
    clearBlockChanges();
}

bool EyeRingFinder::discover(Ring& r, const Wait& wait, double (*now)()) {
    const size_t need = kEyeFrameBytes;
    const int narenas = static_cast<int>(arenas_.size());
    if (narenas == 0) return false;

    // ---- 1. rough candidates: regions being written that look like pictures
    struct Cand {
        int arena;
        size_t off;
    };
    Cand cand[32];
    int ncand = 0;
    for (int a = 0; a < narenas && ncand < 32; a++) {
        const Arena& ar = arenas_[static_cast<size_t>(a)];
        for (size_t off = 0; off + need <= ar.size; off += 4096) {
            const size_t blk = off / kScanBlock;
            if (blk >= static_cast<size_t>(kMaxBlocks) || !tracks_[static_cast<size_t>(a)].changed[blk]) continue;
            if (!looksLikeEyeFrame(ar.map + off)) continue;
            size_t start;
            if (!findFrameStart(ar.map, ar.size, off, &start)) continue;
            if (start + need > ar.size) continue;
            bool dup = false;
            for (int k = 0; k < ncand; k++)
                if (cand[k].arena == a && (start > cand[k].off ? start - cand[k].off : cand[k].off - start) < need / 2)
                    dup = true;
            if (!dup && ncand < 32) {
                cand[ncand].arena = a;
                cand[ncand].off = start;
                ncand++;
            }
            off += need - 4096;
        }
    }
    if (ncand < 2) return false;

    // keep them in memory order
    std::sort(cand, cand + ncand, [](const Cand& x, const Cand& y) {
        return x.arena != y.arena ? x.arena < y.arena : x.off < y.off;
    });

    // Keep only the arena holding the most candidates, and work entirely within it from here on. (Both shared
    // buffers see writes and they are different sizes: an offset from one read against the other runs off the end.)
    int a = cand[0].arena;
    {
        int count[kMaxArenas] = {};
        for (int k = 0; k < ncand; k++) count[cand[k].arena]++;
        for (int k = 0; k < narenas; k++)
            if (count[k] > count[a]) a = k;
        int keep = 0;
        for (int k = 0; k < ncand; k++)
            if (cand[k].arena == a) cand[keep++] = cand[k];
        ncand = keep;
    }
    if (ncand < 2) return false;
    const Arena& ar = arenas_[static_cast<size_t>(a)];

    // ---- 2. verify they are actually being refilled, not just stale pictures ("the ring is live")
    {
        uint64_t fp[32];
        int hits[32];
        for (int k = 0; k < ncand; k++) {
            fp[k] = fingerprint(ar.map + cand[k].off, need, cache_);
            hits[k] = 0;
        }
        const double t0 = now();
        while (now() - t0 < 1.0) {
            for (int k = 0; k < ncand; k++) {
                const uint64_t f = fingerprint(ar.map + cand[k].off, need, cache_);
                if (f != fp[k]) {
                    fp[k] = f;
                    hits[k]++;
                }
            }
            if (!wait()) return false;
        }
        int keep = 0;
        for (int k = 0; k < ncand; k++)
            if (hits[k] >= 5) cand[keep++] = cand[k];
        ncand = keep;
    }
    if (ncand < 2) return false;

    // ---- 3. pick the reference: the slot whose frame edges are darkest (least likely wrapped round the edge)
    int ref = -1;
    double refd = 1e18;
    for (int k = 0; k < ncand; k++) {
        if (cand[k].off + need > ar.size) continue;
        const double d = edgeDarkness(ar.map + cand[k].off);
        if (d < refd) {
            refd = d;
            ref = k;
        }
    }
    if (ref < 0) return false;
    const size_t base = cand[ref].off;

    // ---- 4. measure the spacing: on a 256 KiB grid each slot comes out rotated a little further; that rotation is
    // exactly how much bigger than 256 KiB the real spacing is
    float pref[kStride], pnext[kStride];
    columnProfile(ar.map + base, pref);
    long pitch = kEyeSlot;
    if (base + kEyeSlot + need <= ar.size) {
        columnProfile(ar.map + base + kEyeSlot, pnext);
        double d;
        const unsigned rot = bestRotation(pnext, pref, &d);
        long adj = static_cast<long>(rot);  // a rotation past halfway is really a small negative one
        if (adj > kStride / 2) adj -= kStride;
        pitch = kEyeSlot + adj;
    }

    // ---- 5. lay the ring out on that spacing
    long first = static_cast<long>(base);
    int n = 0;
    while (first - pitch >= 0 && n < kMaxRing && static_cast<size_t>(first - pitch) + need <= ar.size) {
        if (!looksLikeEyeFrame(ar.map + first - pitch)) break;
        first -= pitch;
        n++;
    }
    n = 0;
    for (long o = first; o + static_cast<long>(need) + kStride <= static_cast<long>(ar.size) && n < kMaxRing;
         o += pitch) {
        if (!looksLikeEyeFrame(ar.map + o)) break;
        r.off[n] = static_cast<size_t>(o);
        r.fp[n] = fingerprint(ar.map + o, need, cache_);
        n++;
    }
    if (n < 2) return false;

    // Settle the horizontal framing over the whole ring: shifting the base by d < 512 rotates the columns without
    // changing the rows read, so keep the d that leaves the frame edges darkest across all slots.
    {
        long bestd = 0;
        double bestv = 1e18;
        for (long d = 0; d < kStride; d++) {
            double v = 0;
            int c = 0;
            for (int k = 0; k < n; k++) {
                if (r.off[k] + static_cast<size_t>(d) + need > ar.size) continue;
                v += edgeDarkness(ar.map + r.off[k] + d);
                c++;
            }
            if (c && v / c < bestv) {
                bestv = v / c;
                bestd = d;
            }
        }
        if (bestd) {
            int keep = 0;
            for (int k = 0; k < n; k++)
                if (r.off[k] + static_cast<size_t>(bestd) + need <= ar.size)
                    r.off[keep++] = r.off[k] + static_cast<size_t>(bestd);
            n = keep;
            if (n < 2) return false;
            for (int k = 0; k < n; k++) r.fp[k] = fingerprint(ar.map + r.off[k], need, cache_);
        }
    }

    // Drop anything not actually being refilled: the forward walk can run one past the end of the ring onto a
    // stale frame, which would never update but would still be counted and grouped.
    {
        uint64_t before[kMaxRing];
        for (int k = 0; k < n; k++) before[k] = fingerprint(ar.map + r.off[k], need, cache_);
        bool seen[kMaxRing] = {};
        const double t1 = now();
        while (now() - t1 < 0.6) {
            for (int k = 0; k < n; k++)
                if (fingerprint(ar.map + r.off[k], need, cache_) != before[k]) seen[k] = true;
            if (!wait()) return false;
        }
        int keep = 0;
        for (int k = 0; k < n; k++)
            if (seen[k]) r.off[keep++] = r.off[k];
        if (keep >= 2) {
            n = keep;
            for (int k = 0; k < n; k++) r.fp[k] = fingerprint(ar.map + r.off[k], need, cache_);
        }
    }
    r.arena = a;
    r.slots = n;
    r.pitch = pitch;

    // ---- 6. which eye each slot belongs to: slots of the same eye match closely once aligned, the other eye is an
    // order of magnitude further away. Decided once per slot, so a stream never flickers between eyes.
    static float prof[kMaxRing][kStride];  // camera thread only
    for (int k = 0; k < n; k++) columnProfile(ar.map + r.off[k], prof[k]);
    double dist[kMaxRing];
    double dmin = 1e18, dmax = -1e18;
    for (int k = 0; k < n; k++) {
        dist[k] = profileDistance(prof[k], prof[0], 0);
        dmin = std::min(dmin, dist[k]);
        dmax = std::max(dmax, dist[k]);
    }
    const double split = (dmin + dmax) / 2;
    for (int k = 0; k < n; k++) r.eye[k] = dist[k] > split ? 1 : 0;

    // eye 0 is the one sitting further left, so the mapping is stable
    double cx[2] = {0, 0};
    int cn[2] = {0, 0};
    for (int k = 0; k < n; k++) {
        double num = 0, den = 0;
        for (unsigned x = 0; x < kStride; x++) {
            num += static_cast<double>(prof[k][x]) * x;
            den += prof[k][x];
        }
        if (den > 1) {
            cx[r.eye[k]] += num / den;
            cn[r.eye[k]]++;
        }
    }
    if (cn[0] && cn[1] && (cx[0] / cn[0]) > (cx[1] / cn[1]))
        for (int k = 0; k < n; k++) r.eye[k] ^= 1;
    r.bothEyes = dmax - dmin >= 0.8;
    if (!r.bothEyes)
        for (int k = 0; k < n; k++) r.eye[k] = 0;  // only one eye visible in this ring
    return true;
}

// ------------------------------------------------------------------------------------------------ reader

void RingReader::lock(const Ring& ring, const Arena& arena, double now) {
    ring_ = ring;
    arena_ = arena;
    locked_ = true;
    lastAny_ = now;
    refreshes_ = 0;
    for (int k = 0; k < ring_.slots; k++) {
        fp_[k] = fingerprint(arena_.map + ring_.off[k], kEyeFrameBytes, cache_, kPollSamples);
        changedLast_[k] = false;
    }
}

void RingReader::poll(double now, int settled[2]) {
    settled[0] = settled[1] = -1;
    if (!locked_) return;
    for (int k = 0; k < ring_.slots; k++) {
        const uint64_t f = fingerprint(arena_.map + ring_.off[k], kEyeFrameBytes, cache_, kPollSamples);
        if (f != fp_[k]) {
            fp_[k] = f;
            changedLast_[k] = true;  // fresh: it may still be being written
            lastAny_ = now;
            ++refreshes_;
            continue;
        }
        if (changedLast_[k]) {  // changed last poll, still since: written
            changedLast_[k] = false;
            const int eye = ring_.eye[k] & 1;
            settled[eye] = k;  // several settling at once (a slow poll): the later slot in ring order wins
        }
    }
}

bool RingReader::copySlot(int slot, uint8_t* dst, bool flipV) {
    if (!locked_ || slot < 0 || slot >= ring_.slots) return false;
    const uint8_t* src = arena_.map + ring_.off[slot];
    if (dropRange_) dropRange_(src, kEyeFrameBytes);  // the whole frame from memory, not just the sampled lines
    for (int y = 0; y < kEyeHeight; ++y) {
        const int oy = flipV ? kEyeHeight - 1 - y : y;
        std::memcpy(dst + static_cast<size_t>(oy) * kEyeStride, src + static_cast<size_t>(y) * kEyeStride, kEyeStride);
    }
    const uint64_t after = fingerprint(src, kEyeFrameBytes, cache_, kPollSamples);
    if (after != fp_[slot]) {
        fp_[slot] = after;  // rewritten under us: drop this copy; it settles again on the next quiet poll
        changedLast_[slot] = true;
        return false;
    }
    return true;
}

}  // namespace eyering
