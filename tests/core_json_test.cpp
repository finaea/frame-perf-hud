// core/json: jsonString escaping, and that parseJson gives the original string back (no platform code).
#include "json.h"
#include "utf8.h"

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

/** @return the string value parseJson gives for the document {"k": <json>, "after": 7}, "!" if it fails or "after" is lost */
std::string parsedText(const std::string& json) {
    JsonValue value;
    std::string error;
    if (!parseJson("{\"k\": " + json + ", \"after\": 7}", value, error)) return "!";
    const JsonValue* k = value.get("k");
    const JsonValue* after = value.get("after");
    if (k == nullptr || !k->isString() || after == nullptr || !after->isNumber() || after->number != 7) return "!";
    return k->text;
}

bool parses(const std::string& json) {
    JsonValue value;
    std::string error;
    return parseJson(json, value, error);
}

void testSurrogates() {
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(parsedText(R"("\ud800")") == fffd);                  // lone high
    CHECK(parsedText(R"("\udc00")") == fffd);                  // lone low
    CHECK(parsedText(R"("\ud800\u0041")") == fffd + "A");      // high + non-low: the next \u stands alone
    CHECK(parsedText(R"("x\ud800y")") == "x" + fffd + "y");    // high followed by plain text
    CHECK(parsedText(R"("\ud800\ud800")") == fffd + fffd);
    CHECK(parsedText(R"("\ud83d\ude00")") == "\xF0\x9F\x98\x80");  // a real pair
    CHECK(parsedText(R"("\ud800\ud83d\ude00")") == fffd + "\xF0\x9F\x98\x80");
}

void testRawUtf8() {
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(parsedText("\"a\xFF" "b\"") == "a" + fffd + "b");
    CHECK(parsedText("\"\xC0\xAF\"") == fffd);               // overlong "/": one broken sequence
    CHECK(parsedText("\"\xE3\x81\"") == fffd);               // truncated three-byte character
    CHECK(parsedText("\"\xE3\x81z\"") == fffd + "z");
    // valid text is unchanged
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    CHECK(parsedText("\"" + japanese + "\"") == japanese);
    CHECK(parsedText("\"\xF0\x9F\x98\x80\"") == "\xF0\x9F\x98\x80");
    // keys and later members still load after a broken string
    JsonValue value;
    std::string error;
    CHECK(parseJson("{\"a\": \"\xFF\", \"b\": 2}", value, error));
    CHECK(value.get("b") != nullptr && value.get("b")->number == 2);
}

void testTruncated() {
    CHECK(!parses("\"abc"));
    CHECK(!parses("{\"a\": \"abc"));
    CHECK(!parses("{\"a\": 1"));
    CHECK(!parses("{\"a\": 1,"));
    CHECK(!parses("{\"a\""));
    CHECK(!parses("[1, 2"));
    CHECK(!parses("[1,"));
    CHECK(!parses("[\"abc\\"));
    CHECK(!parses(R"("\u12)"));
    CHECK(!parses("\"abc\\"));
    CHECK(!parses(R"("\ud800\u12)"));
    CHECK(!parses(""));
}

void testDepthLimit() {
    // kMaxDepth = 32 in core/json.cpp: the top-level value is depth 0, so 33 nested (empty) arrays are accepted
    // (the innermost is at depth 32) and 34 are rejected
    auto nested = [](int n) { return std::string(static_cast<size_t>(n), '[') + std::string(static_cast<size_t>(n), ']'); };
    CHECK(parses(nested(33)));
    CHECK(!parses(nested(34)));
    // objects count the same way
    auto nestedObj = [](int n) {
        std::string s;
        for (int i = 0; i < n; ++i) s += "{\"a\":";
        s += "1";
        for (int i = 0; i < n; ++i) s += "}";
        return s;
    };
    CHECK(parses(nestedObj(32)));   // the value 1 is at depth 32
    CHECK(!parses(nestedObj(33)));  // the value 1 would be at depth 33
    JsonValue value;
    std::string error;
    CHECK(!parseJson(nested(34), value, error) && !error.empty());
}

void testUtf8Helpers() {
    using namespace fph;
    const std::string valid = "plain \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80 \xC3\xA9";
    CHECK(isValidUtf8(valid));
    std::string copy = valid;
    CHECK(!makeValidUtf8(copy));
    CHECK(copy == valid);
    CHECK(isValidUtf8(""));

    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(!isValidUtf8("\xED\xA0\x80"));  // an encoded surrogate
    std::string surrogate = "a\xED\xA0\x80z";
    CHECK(makeValidUtf8(surrogate));
    CHECK(isValidUtf8(surrogate));
    CHECK(surrogate == "a" + fffd + "z");
    CHECK(!isValidUtf8("\xF4\x90\x80\x80"));  // past U+10FFFF
    CHECK(isValidUtf8("\xF4\x8F\xBF\xBF"));   // U+10FFFF itself
    CHECK(!isValidUtf8("\xC0\xAF"));          // overlong
    CHECK(!isValidUtf8("\x80"));              // stray continuation byte
    CHECK(!isValidUtf8("\xE3\x81"));          // cut off by the end
    CHECK(utf8CharLength(valid, 0) == 1);
    CHECK(utf8CharLength(valid, 6) == 3);

    // one U+FFFD per broken sequence: the bad byte and the continuation bytes after it
    std::string cut = "ab\xE6\x97";
    CHECK(makeValidUtf8(cut));
    CHECK(cut == "ab" + fffd);
    std::string mixed = "\xFF" "x\xE3\x81y";
    CHECK(makeValidUtf8(mixed));
    CHECK(mixed == fffd + "x" + fffd + "y");
}

}  // namespace

int main() {
    testEscapes();
    testRoundTrip();
    testSurrogates();
    testRawUtf8();
    testTruncated();
    testDepthLimit();
    testUtf8Helpers();
    if (failures == 0) std::printf("core-json-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
