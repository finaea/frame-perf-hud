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

}  // namespace

int main() {
    testEscapes();
    testRoundTrip();
    if (failures == 0) std::printf("core-json-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
