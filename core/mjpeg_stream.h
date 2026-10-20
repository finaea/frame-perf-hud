// HTTP camera (14): splits an MJPEG-over-HTTP answer (`multipart/x-mixed-replace`) into JPEG frames. Platform-free,
// fed with whatever recv() returned; the headset's HTTP camera thread (headset/http_camera.*) does the socket.
// - The answer must be `HTTP/1.x 200` with a multipart/x-mixed-replace Content-Type, else it's NotMjpeg.
// - A part with a Content-Length (mouthcam sends one) is cut by its length. Without one, the JPEG is found by its
//   markers: SOI, then the segments are walked (so an EXIF thumbnail's own SOI / EOI can't end it early), then the
//   entropy-coded data up to EOI.
// - A frame over kMaxFrame bytes is dropped, and so is anything that isn't a JPEG; the parser then looks for the
//   next part. Only the newest complete frame is kept: a slow reader skips frames rather than lagging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fph {
namespace mjpeg {

class StreamParser {
public:
    static constexpr size_t kMaxFrame = 4u * 1024 * 1024;
    static constexpr size_t kMaxHeader = 16 * 1024;   ///< the HTTP answer's header
    static constexpr size_t kMaxPartHeader = 1024;    ///< one part's header ("--boundary", Content-Type, -Length)

    enum class Status {
        Header,    ///< waiting for the answer's header
        Stream,    ///< a multipart stream: frames come out
        NotMjpeg,  ///< not a 200 multipart/x-mixed-replace answer (nothing more is parsed)
    };

    /** Feeds bytes as they came off the socket. @return true if at least one new complete frame was found */
    bool feed(const uint8_t* data, size_t size);

    /** Takes the newest complete frame not taken yet. @return false if there's none */
    bool takeFrame(std::vector<uint8_t>& out);

    Status status() const { return status_; }
    /** @return the answer's status code (0 until the header is in) */
    int httpStatus() const { return httpStatus_; }
    /** @return why it's NotMjpeg, for the debug dump */
    const std::string& error() const { return error_; }

    uint64_t frames() const { return frames_; }    ///< complete frames found
    uint64_t dropped() const { return dropped_; }  ///< oversize frames and runs of garbage skipped

    /** For a new connection. */
    void reset();

private:
    Status status_ = Status::Header;
    int httpStatus_ = 0;
    std::string error_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;           ///< parsed up to here in buf_
    size_t skip_ = 0;          ///< bytes of an oversize Content-Length part still to throw away
    size_t walkAt_ = 0;        ///< a JPEG at pos_ walked this far from its SOI on an earlier feed (0 = none)
    bool walkEntropy_ = false; ///< ... and stopped inside entropy-coded data
    std::vector<uint8_t> frame_;
    bool fresh_ = false;
    uint64_t frames_ = 0;
    uint64_t dropped_ = 0;

    bool parseHeader();
    /** @return true if it made progress (call again), false if it needs more bytes */
    bool parsePart(bool& found);
    /**
     * Walks a JPEG starting at `at` (its SOI), from where the last call stopped when it needed more bytes.
     * @return its length, 0 if more bytes are needed, SIZE_MAX if it isn't a JPEG
     */
    size_t jpegLength(size_t at);
    void publish(size_t at, size_t length);
    void compact();
};

}  // namespace mjpeg
}  // namespace fph
