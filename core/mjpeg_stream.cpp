// HTTP camera (14): see mjpeg_stream.h
#include "mjpeg_stream.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace fph {
namespace mjpeg {

namespace {

constexpr size_t kNeedMore = 0;
constexpr size_t kNotJpeg = static_cast<size_t>(-1);

/** @return the first `needle` in [from, to), or `to` */
size_t find(const std::vector<uint8_t>& hay, const char* needle, size_t from, size_t to) {
    const size_t n = std::strlen(needle);
    if (to > hay.size()) to = hay.size();
    for (size_t i = from; i + n <= to; ++i) {
        if (std::memcmp(hay.data() + i, needle, n) == 0) return i;
    }
    return to;
}

/** @return where a lower-case needle starts in text (case-insensitive), or npos */
size_t findNoCase(const std::string& text, const char* needle) {
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(needle);
}

/** @return the first SOI (FF D8 FF) in [from, size), or size */
size_t findSoi(const std::vector<uint8_t>& b, size_t from) {
    for (size_t i = from; i + 2 < b.size(); ++i) {
        if (b[i] == 0xFF && b[i + 1] == 0xD8 && b[i + 2] == 0xFF) return i;
    }
    return b.size();
}

}  // namespace

void StreamParser::reset() {
    *this = StreamParser();
}

bool StreamParser::feed(const uint8_t* data, size_t size) {
    if (status_ == Status::NotMjpeg || size == 0) return false;
    if (skip_ > 0) {  // the rest of an oversize part
        const size_t n = std::min(skip_, size);
        skip_ -= n;
        data += n;
        size -= n;
        if (size == 0) return false;
    }
    buf_.insert(buf_.end(), data, data + size);
    if (status_ == Status::Header && !parseHeader()) return false;
    bool found = false;
    while (status_ == Status::Stream && skip_ == 0 && parsePart(found)) {
    }
    compact();
    return found;
}

bool StreamParser::takeFrame(std::vector<uint8_t>& out) {
    if (!fresh_) return false;
    std::swap(out, frame_);
    fresh_ = false;
    return true;
}

bool StreamParser::parseHeader() {
    size_t end = find(buf_, "\r\n\r\n", 0, buf_.size());
    size_t bodyAt = end + 4;
    if (end == buf_.size()) {
        end = find(buf_, "\n\n", 0, buf_.size());
        bodyAt = end + 2;
    }
    if (end == buf_.size()) {
        if (buf_.size() > kMaxHeader) {
            status_ = Status::NotMjpeg;
            error_ = "no HTTP header";
        }
        return false;
    }
    const std::string header(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(end));
    // "HTTP/1.0 200 OK"
    if (header.compare(0, 7, "HTTP/1.") != 0 || header.size() < 12 || header[8] != ' ') {
        status_ = Status::NotMjpeg;
        error_ = "not an HTTP answer";
        return false;
    }
    httpStatus_ = std::atoi(header.c_str() + 9);
    if (httpStatus_ != 200) {
        status_ = Status::NotMjpeg;
        error_ = "HTTP " + std::to_string(httpStatus_);
        return false;
    }
    const size_t type = findNoCase(header, "\ncontent-type:");
    const size_t lineEnd = type == std::string::npos ? std::string::npos : header.find('\n', type + 1);
    const std::string value = type == std::string::npos ? "" : header.substr(type, lineEnd - type);
    if (findNoCase(value, "multipart/x-mixed-replace") == std::string::npos) {
        status_ = Status::NotMjpeg;
        error_ = type == std::string::npos ? "no Content-Type" : "Content-Type isn't multipart/x-mixed-replace";
        return false;
    }
    status_ = Status::Stream;
    pos_ = bodyAt;
    return true;
}

bool StreamParser::parsePart(bool& found) {
    const size_t size = buf_.size();
    if (pos_ >= size) return false;
    const size_t soi = findSoi(buf_, pos_);
    // A part header ("--boundary", Content-Type, Content-Length, blank line) before the next JPEG
    const size_t limit = std::min(size, pos_ + kMaxPartHeader + 4);
    const size_t end = find(buf_, "\r\n\r\n", pos_, limit);
    if (end < limit && end < soi) {  // find() returns `limit` when there's none
        const std::string header(buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                 buf_.begin() + static_cast<std::ptrdiff_t>(end));
        const size_t cl = findNoCase(header, "content-length:");
        const long n = cl == std::string::npos ? 0 : std::strtol(header.c_str() + cl + 15, nullptr, 10);
        const size_t at = end + 4;
        if (n <= 0) {  // no length: the JPEG's markers decide where it ends
            pos_ = at;
            return true;
        }
        const size_t length = static_cast<size_t>(n);
        if (length > kMaxFrame) {
            ++dropped_;
            const size_t here = std::min(length, size - at);
            pos_ = at + here;
            skip_ = length - here;
            return skip_ == 0;  // all of it was here: the next part may be too
        }
        if (size - at < length) return false;  // not all here yet
        if (length >= 2 && buf_[at] == 0xFF && buf_[at + 1] == 0xD8) {
            publish(at, length);
            found = true;
        } else {
            ++dropped_;
        }
        pos_ = at + length;
        return true;
    }
    if (soi < size) {
        if (soi != pos_) walkAt_ = 0;  // a walk saved for another SOI doesn't apply
        const size_t length = jpegLength(soi);
        if (length != kNeedMore) walkAt_ = 0;
        if (length == kNotJpeg) {
            ++dropped_;
            pos_ = soi + 2;
            return true;
        }
        if (length == kNeedMore) {
            if (size - soi > kMaxFrame) {  // no end in sight: give up on it, look for the next one
                ++dropped_;
                walkAt_ = 0;
                pos_ = soi + 2;
                return true;
            }
            pos_ = soi;
            return false;
        }
        publish(soi, length);
        found = true;
        pos_ = soi + length;
        return true;
    }
    // Neither a part header nor a JPEG: garbage. Keep a few bytes, they may start a marker or a header
    if (size - pos_ > kMaxPartHeader + 4) {
        ++dropped_;
        pos_ = size - 3;
    }
    return false;
}

size_t StreamParser::jpegLength(size_t at) {
    const std::vector<uint8_t>& b = buf_;
    const size_t size = b.size();
    // Resume where the last feed stopped, so a big frame arriving in small packets is walked once
    size_t p = walkAt_ > 0 ? at + walkAt_ : at + 2;
    bool entropy = walkAt_ > 0 && walkEntropy_;
    const auto needMore = [&]() {
        walkAt_ = p - at;
        walkEntropy_ = entropy;
        return kNeedMore;
    };
    for (;;) {
        if (entropy) {
            // SOS: entropy-coded data up to the next marker (FF 00 is a stuffed byte, FF D0-D7 a restart)
            for (;;) {
                if (p - at > kMaxFrame) return kNeedMore;  // the caller drops it
                if (p + 1 >= size) return needMore();
                if (b[p] == 0xFF && b[p + 1] != 0x00 && !(b[p + 1] >= 0xD0 && b[p + 1] <= 0xD7)) break;
                p += b[p] == 0xFF ? 2 : 1;
            }
            entropy = false;
        }
        if (p - at > kMaxFrame) return kNeedMore;  // the caller drops it
        if (p + 1 >= size) return needMore();
        if (b[p] != 0xFF) return kNotJpeg;
        while (b[p + 1] == 0xFF) {  // fill bytes
            if (++p + 1 >= size) return needMore();
        }
        const uint8_t m = b[p + 1];
        if (m == 0xD9) return p + 2 - at;  // EOI
        if (m == 0x00 || m == 0xD8) return kNotJpeg;
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {  // standalone markers
            p += 2;
            continue;
        }
        if (p + 4 > size) return needMore();
        const size_t length = static_cast<size_t>(b[p + 2]) << 8 | b[p + 3];
        if (length < 2) return kNotJpeg;
        p += 2 + length;  // a segment (an APP1 with an EXIF thumbnail is skipped whole)
        entropy = m == 0xDA;
    }
}

void StreamParser::publish(size_t at, size_t length) {
    frame_.assign(buf_.begin() + static_cast<std::ptrdiff_t>(at),
                  buf_.begin() + static_cast<std::ptrdiff_t>(at + length));
    fresh_ = true;
    ++frames_;
}

void StreamParser::compact() {
    if (pos_ == 0) return;
    pos_ = std::min(pos_, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
}

}  // namespace mjpeg
}  // namespace fph
