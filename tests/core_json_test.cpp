// core/json: jsonString escaping, and that parseJson gives the original string back (no platform code).
#include "json.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

/** @return true if jsonString(text) parses back to exactly text */
bool roundTrips(const std::string& text) {
    JsonValue value;
    std::string error;
    if (!parseJson(jsonString(text), value, error)) return false;
    return value.isString() && value.text == text;
}

void testEscapes() {
    CHECK(jsonString("") == R"("")");
    CHECK(jsonString("plain") == R"("plain")");
    CHECK(jsonString(R"(a"b)") == R"("a\"b")");
    CHECK(jsonString(R"(a\b)") == R"("a\\b")");
    CHECK(jsonString("a\tb") == R"("a\u0009b")");
    CHECK(jsonString("a\nb") == R"("a\u000ab")");
    CHECK(jsonString(std::string("\x01")) == R"("\u0001")");
    CHECK(jsonString(std::string("\x1f")) == R"("\u001f")");
    CHECK(jsonString(std::string(1, '\0')) == R"("\u0000")");
    // 0x20 and up pass through: DEL, space, and UTF-8 bytes
    CHECK(jsonString("\x7f") == "\"\x7f\"");
    CHECK(jsonString(" ") == "\" \"");
    CHECK(jsonString("\xe6\x97\xa5\xe6\x9c\xac") == "\"\xe6\x97\xa5\xe6\x9c\xac\"");
}

void testRoundTrip() {
    CHECK(roundTrips(""));
    CHECK(roundTrips(R"(a"b\c)"));
    CHECK(roundTrips("a\tb\nc\r\x01 d\x1f"));
    CHECK(roundTrips(std::string("nul\0inside", 10)));
    CHECK(roundTrips("\x7f \xe6\x97\xa5\xe6\x9c\xac"));
    CHECK(roundTrips(R"(\"\\"\)"));
    std::string everyControl;
    for (int c = 0; c < 0x20; ++c) everyControl += static_cast<char>(c);
    CHECK(roundTrips(everyControl));
}

const std::string kFffd = "\xEF\xBF\xBD";  // U+FFFD

/** @return what parseJson makes of a one-string document; "<fail>" if it doesn't parse */
std::string parsedString(const std::string& json) {
    JsonValue value;
    std::string error;
    if (!parseJson(json, value, error) || !value.isString()) return "<fail>";
    return value.text;
}

bool parses(const std::string& json) {
    JsonValue value;
    std::string error;
    return parseJson(json, value, error);
}

// Unpaired surrogates and broken raw UTF-8 come out as U+FFFD; a valid pair stays one character
void testBadEncodings() {
    CHECK(parsedString(R"("\uD800")") == kFffd);       // lone high
    CHECK(parsedString(R"("\uDC00")") == kFffd);       // lone low
    CHECK(parsedString(R"("\uD800A")") == kFffd + "A");  // high, then not a low
    CHECK(parsedString(R"("\uD800\u0041")") == kFffd + "A");  // high, then an escaped non-low
    CHECK(parsedString(R"("\uD83D\uDE00")") == "\xF0\x9F\x98\x80");  // a valid pair, as escapes
    CHECK(parsedString("\"a\xFF" "b\"") == "a" + kFffd + "b");  // a byte that is never UTF-8
    CHECK(parsedString("\"\xC0\xAF\"") == kFffd + kFffd);      // overlong "/"
    CHECK(parsedString("\"\xE3\x81\"") == kFffd + kFffd);      // a 3-byte character cut short by the quote
    CHECK(parsedString("\"\xE6\x97\xA5\"") == "\xE6\x97\xA5");  // a valid one is untouched
    for (const std::string& json : {std::string(R"("\uD800")"), std::string("\"\xC0\xAF\""), std::string("\"\xE3\x81\"")}) {
        const std::string text = parsedString(json);
        CHECK(isValidUtf8(text.data(), text.size()));
    }
}

// Cut-off input fails instead of parsing something
void testTruncatedInput() {
    CHECK(!parses("\"ab"));
    CHECK(!parses("{\"a\":1"));
    CHECK(!parses("[1,2"));
    CHECK(!parses("\"\\u12"));
    CHECK(!parses("\"\\"));
    CHECK(parses("[1,2]") && parses("{\"a\":1}"));
}

// Nesting up to the parser's limit (32 levels) is fine, one more is refused
void testNestingDepth() {
    const auto nested = [](int levels) { return std::string(static_cast<size_t>(levels), '[') + "1" + std::string(static_cast<size_t>(levels), ']'); };
    CHECK(parses(nested(32)));
    CHECK(!parses(nested(33)));
}

void testUtf8Helpers() {
    const auto valid = [](const std::string& s) { return isValidUtf8(s.data(), s.size()); };
    CHECK(valid("") && valid("plain") && valid("\xE6\x97\xA5\xE6\x9C\xAC") && valid("\xF0\x9F\x98\x80"));
    CHECK(valid("\xF4\x8F\xBF\xBF"));                 // U+10FFFF
    CHECK(!valid("\xF4\x90\x80\x80"));                // past U+10FFFF
    CHECK(!valid("\xFF") && !valid("\x80") && !valid("\xC0\xAF") && !valid("\xE0\x80\xAF"));  // stray, continuation, overlong
    CHECK(!valid("\xED\xA0\x80"));                    // a surrogate in UTF-8
    CHECK(!valid("\xE3\x81") && !valid("a\xF0\x9F"));  // truncated
    std::string text = "a\xFF" "b\xC0\xAF" "c";
    makeValidUtf8(text);
    CHECK(text == "a" + kFffd + "b" + kFffd + kFffd + "c");
    CHECK(valid(text));
    std::string good = "ok \xE6\x97\xA5";
    makeValidUtf8(good);
    CHECK(good == "ok \xE6\x97\xA5");  // a valid string is left alone
}

}  // namespace

int main() {
    testEscapes();
    testRoundTrip();
    testBadEncodings();
    testTruncatedInput();
    testNestingDepth();
    testUtf8Helpers();
    if (failures == 0) std::printf("core-json-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
