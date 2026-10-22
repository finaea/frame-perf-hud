// UTF-8 checks for text from outside: config / profile JSON, /proc process names (cut at 15 bytes, so possibly
// mid-character), the network. cairo_show_text with invalid UTF-8 puts its context into a permanent error, so the
// drawing code makes text valid first (Pen::text / Pen::measure), and the JSON parser too.
#pragma once

#include <cstddef>
#include <string>

namespace fph {

/**
 * @return the length (1-4) of the well-formed UTF-8 character at s[i], or 0 if it isn't one: a stray continuation
 *         byte, an overlong form, a surrogate, past U+10FFFF, or cut off by the end of the string
 */
inline size_t utf8CharLength(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    // the lead byte gives the length, its payload bits, and the smallest code point that length may carry
    const size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0) return 0;
    unsigned code = c & (0x7Fu >> n);
    const unsigned min = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) return 0;
        code = (code << 6) | (b & 0x3Fu);
    }
    if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
    return n;
}

/** @return true if the whole string is well-formed UTF-8 */
inline bool isValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8CharLength(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

/**
 * Replaces each broken sequence (a bad byte and the continuation bytes after it) with U+FFFD. Valid text is left
 * as is, without a copy.
 * @return true if it changed anything
 */
inline bool makeValidUtf8(std::string& s) {
    if (isValidUtf8(s)) return false;
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8CharLength(s, i);
        if (n > 0) {
            out.append(s, i, n);
            i += n;
            continue;
        }
        out += "\xEF\xBF\xBD";
        ++i;
        for (int k = 0; k < 3 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; ++k) ++i;
    }
    s.swap(out);
    return true;
}

}  // namespace fph
