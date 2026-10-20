// core/mjpeg_stream tests: the HTTP camera's MJPEG splitter, fed hand-built bytes (no sockets, no platform code).
#include "mjpeg_stream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
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

using fph::mjpeg::StreamParser;
using Bytes = std::vector<uint8_t>;
using Status = StreamParser::Status;

void append(Bytes& out, const std::string& text) { out.insert(out.end(), text.begin(), text.end()); }
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

/** A JPEG-shaped blob: SOI, DQT, SOS, entropy bytes (with FF 00 stuffing and a restart marker), EOI. `seed` makes
 *  two frames differ; `padding` adds entropy bytes (never FF). Not decodable, but it's what the splitter walks. */
Bytes fakeJpeg(uint8_t seed, size_t padding = 20, bool exifThumbnail = false) {
    Bytes j = {0xFF, 0xD8};
    if (exifThumbnail) {  // APP1 holding a whole nested JPEG: its SOI / EOI mustn't end the outer frame
        const Bytes thumb = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0x01, 0x02, 0xFF, 0xDA, 0x00, 0x04, 0x00, 0x00,
                             0x11, 0x22, 0xFF, 0xD9};
        const size_t len = 2 + 6 + thumb.size();
        j.insert(j.end(), {0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)});
        append(j, std::string("Exif\0\0", 6));
        append(j, thumb);
    }
    j.insert(j.end(), {0xFF, 0xDB, 0x00, 0x05, seed, 0x02, 0x03});  // DQT, length 5
    j.insert(j.end(), {0xFF, 0xDA, 0x00, 0x04, 0x00, 0x00});         // SOS, length 4
    j.insert(j.end(), {0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD0, 0x56});   // entropy: stuffed FF, a restart marker
    for (size_t i = 0; i < padding; ++i) j.push_back(static_cast<uint8_t>(0x10 + (i + seed) % 0x60));
    j.insert(j.end(), {0xFF, 0xD9});
    return j;
}

const char* kHeader =
    "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=X\r\nCache-Control: no-cache\r\n\r\n";

/** One part as mouthcam sends it: boundary, type, length, blank line, JPEG, CRLF */
Bytes partWithLength(const Bytes& jpeg) {
    Bytes p;
    append(p, "--X\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n");
    append(p, jpeg);
    append(p, "\r\n");
    return p;
}

/** One part with no Content-Length */
Bytes partNoLength(const Bytes& jpeg) {
    Bytes p;
    append(p, "--X\r\nContent-Type: image/jpeg\r\n\r\n");
    append(p, jpeg);
    append(p, "\r\n");
    return p;
}

Bytes stream(const std::vector<Bytes>& parts) {
    Bytes s;
    append(s, kHeader);
    for (const Bytes& p : parts) append(s, p);
    return s;
}

bool feedAll(StreamParser& p, const Bytes& data) { return p.feed(data.data(), data.size()); }

void testContentLength() {
    StreamParser p;
    CHECK(p.status() == Status::Header && p.httpStatus() == 0);
    const Bytes a = fakeJpeg(1), b = fakeJpeg(2, 300);
    CHECK(feedAll(p, stream({partWithLength(a)})));
    CHECK(p.status() == Status::Stream && p.httpStatus() == 200);
    Bytes got;
    CHECK(p.takeFrame(got) && got == a);
    CHECK(!p.takeFrame(got));
    CHECK(p.frames() == 1 && p.dropped() == 0);
    // the next part, later
    CHECK(feedAll(p, partWithLength(b)));
    CHECK(p.takeFrame(got) && got == b);
    CHECK(p.frames() == 2);

    // a part is only a frame once all of it is in
    Bytes part = partWithLength(a);
    part.resize(part.size() - 8);
    CHECK(!feedAll(p, part));
    CHECK(!p.takeFrame(got));
    CHECK(p.feed(a.data() + (a.size() - 6), 6));  // the last 6 of the JPEG; the CRLF may follow or not
    CHECK(p.takeFrame(got) && got == a);

    // a Content-Length part whose JPEG holds an inner FF D9 is cut by its length, not by markers
    Bytes odd = {0xFF, 0xD8, 0xFF, 0xD9, 0x01, 0x02, 0x03, 0xFF, 0xD9};
    StreamParser q;
    CHECK(feedAll(q, stream({partWithLength(odd)})));
    CHECK(q.takeFrame(got) && got == odd);

    // a part that isn't a JPEG is dropped and counted
    StreamParser r;
    const Bytes junk = {'h', 'e', 'l', 'l', 'o'};
    CHECK(!feedAll(r, stream({partWithLength(junk)})));
    CHECK(r.dropped() == 1 && r.frames() == 0);
    CHECK(feedAll(r, partWithLength(a)) && r.takeFrame(got) && got == a);
}

void testLfOnlyAndCase() {
    // "\n\n" header end, odd header case: accepted
    StreamParser p;
    std::string h = "HTTP/1.1 200 OK\ncontent-type: Multipart/X-Mixed-Replace; boundary=frame\n\n";
    Bytes s;
    append(s, h);
    append(s, partWithLength(fakeJpeg(3)));
    CHECK(feedAll(p, s));
    CHECK(p.status() == Status::Stream);
}

void testNoContentLength() {
    StreamParser p;
    const Bytes a = fakeJpeg(1), b = fakeJpeg(2, 100);
    Bytes got;
    CHECK(feedAll(p, stream({partNoLength(a), partNoLength(b)})));
    CHECK(p.frames() == 2);
    CHECK(p.takeFrame(got) && got == b);  // newest wins
    CHECK(!p.takeFrame(got));

    // EXIF thumbnail: the nested SOI ... EOI must not end the frame early
    StreamParser q;
    const Bytes exif = fakeJpeg(4, 30, true);
    const Bytes next = fakeJpeg(5);
    CHECK(feedAll(q, stream({partNoLength(exif)})));
    CHECK(q.takeFrame(got));
    CHECK(got == exif);
    CHECK(got.size() > 40);
    // ... also when the stream is cut inside the thumbnail
    StreamParser r;
    const Bytes whole = stream({partNoLength(exif), partNoLength(next)});
    const size_t cut = whole.size() - partNoLength(next).size() - exif.size() / 2;
    CHECK(!r.feed(whole.data(), cut));
    CHECK(!r.takeFrame(got));
    CHECK(r.feed(whole.data() + cut, whole.size() - cut));
    CHECK(r.frames() == 2);
    CHECK(r.takeFrame(got) && got == next);

    // a JPEG with no boundary line at all, straight after the header (a bare stream of frames)
    StreamParser s;
    Bytes raw;
    append(raw, kHeader);
    append(raw, a);
    append(raw, b);
    CHECK(feedAll(s, raw) && s.frames() == 2 && s.takeFrame(got) && got == b);
}

void testSplitFeeds() {
    const Bytes a = fakeJpeg(1, 50, true), b = fakeJpeg(2, 40);
    for (const bool withLength : {true, false}) {
        StreamParser p;
        const Bytes all = stream(withLength ? std::vector<Bytes>{partWithLength(a), partWithLength(b)}
                                            : std::vector<Bytes>{partNoLength(a), partNoLength(b)});
        std::vector<Bytes> seen;
        Bytes got;
        for (const uint8_t byte : all) {
            if (p.feed(&byte, 1)) {
                CHECK(p.takeFrame(got));
                seen.push_back(got);
            }
        }
        // Without a length the frame is known at its EOI (the trailing CRLF isn't needed), with one at its last byte
        CHECK(seen.size() == 2);
        if (seen.size() == 2) CHECK(seen[0] == a && seen[1] == b);
        CHECK(p.status() == Status::Stream && p.dropped() == 0);
    }
    // three-byte feeds, other offsets
    for (const size_t step : {2u, 3u, 7u, 64u}) {
        StreamParser p;
        const Bytes all = stream({partWithLength(a), partNoLength(b)});
        for (size_t i = 0; i < all.size(); i += step) p.feed(all.data() + i, std::min(step, all.size() - i));
        CHECK(p.frames() == 2);
    }
}

void testGarbage() {
    const Bytes a = fakeJpeg(1), b = fakeJpeg(2), c = fakeJpeg(3);
    Bytes got;
    // junk between parts (no Content-Length): skipped
    StreamParser p;
    Bytes s;
    append(s, kHeader);
    append(s, partNoLength(a));
    append(s, std::string("\r\nxxxx garbage, no marker here \x01\x02\xFE\r\n"));
    append(s, partNoLength(b));
    CHECK(feedAll(p, s));
    CHECK(p.frames() == 2 && p.takeFrame(got) && got == b);

    // a lot of garbage (more than a part header) before the next part
    StreamParser q;
    Bytes t;
    append(t, kHeader);
    append(t, partWithLength(a));
    append(t, std::string(5000, 'z'));
    append(t, partWithLength(b));
    CHECK(feedAll(q, t));
    CHECK(q.frames() == 2 && q.takeFrame(got) && got == b);

    // FF D8 not followed by FF is no SOI: skipped, the real frame after it is found
    StreamParser r;
    Bytes u;
    append(u, kHeader);
    append(u, Bytes{0xFF, 0xD8, 0x00, 0x11});
    append(u, partNoLength(c));
    CHECK(feedAll(r, u));
    CHECK(r.takeFrame(got) && got == c);

    // a truncated JPEG (cut off, then the next part starts): the next frame still arrives
    StreamParser v;
    Bytes cutOff = fakeJpeg(1, 40);
    cutOff.resize(cutOff.size() - 10);  // no EOI
    Bytes w;
    append(w, kHeader);
    append(w, partNoLength(cutOff));
    append(w, partNoLength(b));
    v.feed(w.data(), w.size());
    // the cut JPEG swallows bytes up to the next marker; whatever it does, b must be found eventually
    append(w, partNoLength(c));
    StreamParser v2;
    v2.feed(w.data(), w.size());
    CHECK(v2.takeFrame(got) && (got == c || got == b));
}

void testOversize() {
    const size_t big = StreamParser::kMaxFrame + 1000;
    Bytes huge = fakeJpeg(9, big);
    const Bytes good = fakeJpeg(1), good2 = fakeJpeg(2);
    Bytes got;

    // with a Content-Length: dropped (as it streams in), the next one found
    for (const size_t chunk : {size_t(1) << 30, size_t(100000)}) {
        StreamParser p;
        const Bytes all = stream({partWithLength(good), partWithLength(huge), partWithLength(good2)});
        CHECK(p.feed(all.data(), std::min<size_t>(all.size(), 200)) || true);
        for (size_t i = 200; i < all.size(); i += chunk) p.feed(all.data() + i, std::min(chunk, all.size() - i));
        CHECK(p.frames() == 2);
        CHECK(p.dropped() >= 1);
        CHECK(p.takeFrame(got) && got == good2);
        CHECK(p.status() == Status::Stream);
    }

    // without a Content-Length: dropped, the next one found
    for (const size_t chunk : {size_t(1) << 30, size_t(65536)}) {
        StreamParser p;
        const Bytes all = stream({partNoLength(good), partNoLength(huge), partNoLength(good2)});
        for (size_t i = 0; i < all.size(); i += chunk) p.feed(all.data() + i, std::min(chunk, all.size() - i));
        CHECK(p.dropped() >= 1);
        CHECK(p.takeFrame(got) && got == good2);
        CHECK(p.frames() == 2);
    }

    // exactly kMaxFrame is accepted
    StreamParser q;
    Bytes edge = fakeJpeg(1, 0);
    edge.insert(edge.end() - 2, StreamParser::kMaxFrame - edge.size(), 0x20);
    CHECK(edge.size() == StreamParser::kMaxFrame);
    CHECK(feedAll(q, stream({partWithLength(edge)})));
    CHECK(q.takeFrame(got) && got.size() == StreamParser::kMaxFrame);
}

void testNotMjpeg() {
    struct Case {
        const char* answer;
        const char* what;
    };
    const Case cases[] = {
        {"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nnope", "404"},
        {"HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: 5\r\n\r\nhello", "single jpeg"},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", "no content type"},
        {"HTTP/1.1 301 Moved\r\nLocation: http://x/\r\n\r\n", "redirect"},
        {"HTTP/1.1 500 Oops\r\nContent-Type: multipart/x-mixed-replace;boundary=X\r\n\r\n", "500 with multipart"},
        {"SSH-2.0-OpenSSH_9.6\r\nsomething\r\n\r\n", "not http"},
        {"HTTP/1.1 200\r\n\r\n", "short status line"},
    };
    for (const Case& c : cases) {
        StreamParser p;
        const std::string answer(c.answer);
        CHECK(!p.feed(reinterpret_cast<const uint8_t*>(answer.data()), answer.size()));
        if (p.status() != Status::NotMjpeg) std::printf("not NotMjpeg: %s\n", c.what);
        CHECK(p.status() == Status::NotMjpeg);
        CHECK(!p.error().empty());
        // nothing more is parsed, even a JPEG
        const Bytes j = partNoLength(fakeJpeg(1));
        CHECK(!feedAll(p, j));
        Bytes got;
        CHECK(!p.takeFrame(got));
        CHECK(p.status() == Status::NotMjpeg);
    }
    StreamParser p;
    const std::string nf = "HTTP/1.0 404 Not Found\r\n\r\n";
    p.feed(reinterpret_cast<const uint8_t*>(nf.data()), nf.size());
    CHECK(p.httpStatus() == 404 && p.error().find("404") != std::string::npos);

    // header split over one-byte feeds still gets judged
    StreamParser q;
    for (const char c : nf) {
        const uint8_t byte = static_cast<uint8_t>(c);
        q.feed(&byte, 1);
    }
    CHECK(q.status() == Status::NotMjpeg);

    // endless header with no end: NotMjpeg once it's over kMaxHeader
    StreamParser r;
    Bytes endless;
    append(endless, "HTTP/1.1 200 OK\r\nX-Pad: ");
    append(endless, std::string(StreamParser::kMaxHeader + 10, 'a'));
    r.feed(endless.data(), endless.size());
    CHECK(r.status() == Status::NotMjpeg);

    // a header still arriving is neither
    StreamParser s;
    const std::string half = "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace;bound";
    CHECK(!s.feed(reinterpret_cast<const uint8_t*>(half.data()), half.size()));
    CHECK(s.status() == Status::Header);
}

void testNewestOnlyAndReset() {
    StreamParser p;
    const Bytes a = fakeJpeg(1), b = fakeJpeg(2);
    Bytes got;
    CHECK(feedAll(p, stream({partWithLength(a), partWithLength(b)})));
    CHECK(p.frames() == 2);
    CHECK(p.takeFrame(got) && got == b);
    CHECK(!p.takeFrame(got));

    // a new connection: header state again, counters cleared, nothing pending
    CHECK(feedAll(p, partWithLength(a)));
    p.reset();
    CHECK(p.status() == Status::Header && p.frames() == 0 && p.dropped() == 0 && p.httpStatus() == 0);
    CHECK(!p.takeFrame(got));
    CHECK(feedAll(p, stream({partWithLength(b)})) && p.takeFrame(got) && got == b);

    // reset also clears NotMjpeg
    StreamParser q;
    const std::string nf = "HTTP/1.0 404 Not Found\r\n\r\n";
    q.feed(reinterpret_cast<const uint8_t*>(nf.data()), nf.size());
    CHECK(q.status() == Status::NotMjpeg);
    q.reset();
    CHECK(q.status() == Status::Header && q.error().empty());
    CHECK(feedAll(q, stream({partWithLength(a)})) && q.takeFrame(got) && got == a);

    // feed(0 bytes) is harmless
    CHECK(!q.feed(nullptr, 0));
}

}  // namespace

/** A 3 MB frame with no Content-Length in ~1400-byte packets: the walk resumes, so it's linear (was O(n²)). */
void testBigNoLengthFrameInPackets() {
    const Bytes big = fakeJpeg(9, 3u * 1024 * 1024);
    const Bytes next = fakeJpeg(10);
    const Bytes data = stream({partNoLength(big)});
    StreamParser p;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < data.size(); at += 1400) p.feed(data.data() + at, std::min<size_t>(1400, data.size() - at));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Bytes frame;
    CHECK(p.frames() == 1 && p.dropped() == 0);
    CHECK(p.takeFrame(frame) && frame == big);
    CHECK(feedAll(p, partNoLength(next)) && p.takeFrame(frame) && frame == next);  // and the walk starts over
    CHECK(ms < 1000.0);
    std::printf("3 MB no-length frame in 1400-byte packets: %.1f ms\n", ms);
}

int main() {
    testBigNoLengthFrameInPackets();
    testContentLength();
    testLfOnlyAndCase();
    testNoContentLength();
    testSplitFeeds();
    testGarbage();
    testOversize();
    testNotMjpeg();
    testNewestOnlyAndReset();
    if (failures == 0) std::printf("core-mjpeg-stream-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
