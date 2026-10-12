// T10: camera BLOBs over the link's reassembly. Two cameras' frames interleaved into one BlobAssembler for the same
// peer must both complete; a newer frame replaces an incomplete older one; a lost chunk costs only that frame.
#include "camera_frame.h"
#include "link_wire.h"

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
using namespace fph::link;

using Bytes = std::vector<uint8_t>;
using Chunks = std::vector<Bytes>;

/** A camera BLOB with a pseudo-random "JPEG" of the given size. */
Bytes makeCameraBlob(uint8_t camera, size_t jpegSize, uint32_t frameId, uint32_t seed) {
    Bytes jpeg(jpegSize);
    uint32_t s = seed * 2654435761u + 1;
    for (uint8_t& b : jpeg) {
        s = s * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(s >> 24);
    }
    cam::BlobHeader h;
    h.camera = camera;
    h.state = cam::CameraState::Live;
    h.width = 528;
    h.height = 512;
    h.frameId = frameId;
    return cam::encodeCameraBlob(h, jpeg.data(), jpeg.size());
}

/** Feeds payloads one by one. @return the completed blob, if any chunk completed one */
struct Feeder {
    BlobAssembler asmb;
    std::vector<Blob> done;
    double now = 0.0;

    void feed(uint64_t peer, const Bytes& payload) {
        BlobChunk c;
        CHECK(decodeBlobChunk(payload.data(), payload.size(), c));
        Blob b;
        if (asmb.add(peer, c, now, b)) done.push_back(std::move(b));
    }
};

const Blob* find(const std::vector<Blob>& v, uint8_t stream) {
    for (const Blob& b : v)
        if (b.stream == stream) return &b;
    return nullptr;
}

void testInterleavedCameras() {
    const Bytes a = makeCameraBlob(0, 40000, 1, 1);
    const Bytes b = makeCameraBlob(2, 25000, 1, 2);
    const Chunks ca = splitBlob(7, BlobKind::Camera, 0, a);
    const Chunks cb = splitBlob(7, BlobKind::Camera, 2, b);  // the same blob id on another stream
    CHECK(ca.size() == (a.size() + kBlobChunkData - 1) / kBlobChunkData);
    CHECK(cb.size() == (b.size() + kBlobChunkData - 1) / kBlobChunkData);
    CHECK(ca.size() > 30 && cb.size() > 15);

    Feeder f;
    size_t i = 0, j = 0;
    while (i < ca.size() || j < cb.size()) {
        if (i < ca.size()) f.feed(1, ca[i++]);
        if (j < cb.size()) f.feed(1, cb[j++]);
        f.now += 0.0001;
    }
    CHECK(f.done.size() == 2);
    const Blob* ra = find(f.done, 0);
    const Blob* rb = find(f.done, 2);
    CHECK(ra && ra->data == a && ra->kind == BlobKind::Camera && ra->blobId == 7);
    CHECK(rb && rb->data == b && rb->kind == BlobKind::Camera && rb->blobId == 7);
    CHECK(f.asmb.inProgress() == 0);

    // and the completed data decodes as camera blobs of the right camera
    cam::BlobHeader h;
    const uint8_t* jpeg = nullptr;
    size_t jn = 0;
    if (ra) {
        CHECK(cam::decodeCameraBlob(ra->data.data(), ra->data.size(), h, jpeg, jn));
        CHECK(h.camera == 0 && jn == 40000);
    }
    if (rb) {
        CHECK(cam::decodeCameraBlob(rb->data.data(), rb->data.size(), h, jpeg, jn));
        CHECK(h.camera == 2 && jn == 25000);
    }
}

void testAllSixInterleaved() {
    Feeder f;
    std::vector<Bytes> blobs;
    std::vector<Chunks> chunks;
    for (uint8_t c = 0; c < cam::kCameraCount; ++c) {
        blobs.push_back(makeCameraBlob(c, 9000 + 3000u * c, 5, 10 + c));
        chunks.push_back(splitBlob(5, BlobKind::Camera, c, blobs.back()));
    }
    bool more = true;
    for (size_t k = 0; more; ++k) {
        more = false;
        for (uint8_t c = 0; c < cam::kCameraCount; ++c) {
            if (k < chunks[c].size()) {
                f.feed(9, chunks[c][chunks[c].size() - 1 - k]);  // reversed order: out-of-order chunks still complete
                more = true;
            }
        }
    }
    CHECK(f.done.size() == cam::kCameraCount);
    for (uint8_t c = 0; c < cam::kCameraCount; ++c) {
        const Blob* r = find(f.done, c);
        CHECK(r && r->data == blobs[c]);
    }
}

void testDifferentPeers() {
    // two PCs (peers) receiving the same stream don't cancel each other either
    const Bytes a = makeCameraBlob(0, 12000, 1, 3);
    const Chunks ca = splitBlob(3, BlobKind::Camera, 0, a);
    Feeder f;
    for (size_t k = 0; k < ca.size(); ++k) {
        f.feed(1, ca[k]);
        f.feed(2, ca[k]);
    }
    CHECK(f.done.size() == 2);
    CHECK(f.done[0].data == a && f.done[1].data == a);
    CHECK(f.done[0].peer != f.done[1].peer);
}

void testNewerReplacesIncomplete() {
    const Bytes older = makeCameraBlob(1, 30000, 1, 4);
    const Bytes newer = makeCameraBlob(1, 20000, 2, 5);
    const Chunks co = splitBlob(10, BlobKind::Camera, 1, older);
    const Chunks cn = splitBlob(11, BlobKind::Camera, 1, newer);

    Feeder f;
    for (size_t k = 0; k + 3 < co.size(); ++k) f.feed(1, co[k]);  // most of it, then the newer one starts
    CHECK(f.done.empty());
    CHECK(f.asmb.inProgress() == 1);
    f.feed(1, cn[0]);
    CHECK(f.asmb.inProgress() == 1);
    // late chunks of the older blob are dropped and don't disturb the newer one
    const uint64_t droppedBefore = f.asmb.dropped();
    f.feed(1, co[co.size() - 1]);
    CHECK(f.asmb.dropped() > droppedBefore);
    for (size_t k = 1; k < cn.size(); ++k) f.feed(1, cn[k]);
    CHECK(f.done.size() == 1);
    CHECK(f.done.size() == 1 && f.done[0].blobId == 11 && f.done[0].data == newer);

    // and the older blob arriving complete after that is a late duplicate: never delivered
    for (const Bytes& c : co) f.feed(1, c);
    CHECK(f.done.size() == 1);
}

void testLostChunk() {
    const Bytes lost = makeCameraBlob(3, 30000, 1, 6);
    const Bytes next = makeCameraBlob(3, 30000, 2, 7);
    const Chunks cl = splitBlob(20, BlobKind::Camera, 3, lost);
    const Chunks cx = splitBlob(21, BlobKind::Camera, 3, next);
    Feeder f;
    for (size_t k = 0; k < cl.size(); ++k)
        if (k != cl.size() / 2) f.feed(1, cl[k]);
    CHECK(f.done.empty());
    CHECK(f.asmb.inProgress() == 1);
    f.now += 0.01;
    for (const Bytes& c : cx) f.feed(1, c);  // the next frame completes
    CHECK(f.done.size() == 1);
    CHECK(f.done.size() == 1 && f.done[0].blobId == 21 && f.done[0].data == next);
    CHECK(f.asmb.inProgress() == 0);

    // without a newer frame the incomplete one expires after kBlobExpirySec and never completes
    Feeder g;
    for (size_t k = 0; k < cl.size(); ++k)
        if (k != 3) g.feed(1, cl[k]);
    CHECK(g.asmb.inProgress() == 1);
    g.now += kBlobExpirySec + 0.1;
    g.asmb.expire(g.now);
    CHECK(g.asmb.inProgress() == 0);
    CHECK(g.done.empty());
    // the missing chunk arriving after expiry doesn't finish it
    g.feed(1, cl[3]);
    CHECK(g.done.empty());
}

void testNonLiveIsOneChunk() {
    cam::BlobHeader h;
    h.camera = 4;
    h.state = cam::CameraState::NotWorn;
    const Bytes b = cam::encodeCameraBlob(h, nullptr, 0);
    const Chunks c = splitBlob(1, BlobKind::Camera, 4, b);
    CHECK(c.size() == 1);
    Feeder f;
    f.feed(1, c[0]);
    CHECK(f.done.size() == 1 && f.done[0].data == b && f.done[0].stream == 4);
}

void testTooBig() {
    // over kMaxBlobChunks chunks: refused up front
    const Bytes huge(static_cast<size_t>(kMaxBlobChunks) * kBlobChunkData + 1, 0x11);
    CHECK(splitBlob(1, BlobKind::Camera, 0, huge).empty());
    const Bytes max(static_cast<size_t>(kMaxBlobChunks) * kBlobChunkData, 0x11);
    CHECK(splitBlob(1, BlobKind::Camera, 0, max).size() == kMaxBlobChunks);
}

}  // namespace

int main() {
    testInterleavedCameras();
    testAllSixInterleaved();
    testDifferentPeers();
    testNewerReplacesIncomplete();
    testLostChunk();
    testNonLiveIsOneChunk();
    testTooBig();
    if (failures == 0) std::printf("camera_blob_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
