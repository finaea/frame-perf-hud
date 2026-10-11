// core/ session tracker tests: the session id changes only when the app key does (CTest runs them; no platform code).
#include "session_tracker.h"

#include <cstdio>

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

/** Hands out 1, 2, 3, ... */
SessionTracker::IdSource counter() {
    return [n = 0]() mutable { return static_cast<int32_t>(++n); };
}

void firstEmptyKeyGivesNoSession() {
    SessionTracker t(counter());
    CHECK(!t.update("", "", 100));
    CHECK(t.current().id == 0);
    CHECK(t.current().appKey.empty());
    CHECK(!t.update("", "ignored", 101));
    CHECK(t.current().id == 0);
}

void sameKeyKeepsSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    const int32_t id = t.current().id;
    CHECK(id == 1);
    CHECK(!t.update("steam.app.1", "A", 200));
    CHECK(t.current().id == id);
    CHECK(t.current().startUnix == 100);
}

void compositorFlickerStaysOneSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(!t.update("", "", 110));
    CHECK(t.current().id == 1);
    CHECK(t.current().appKey == "steam.app.1");  // an empty key doesn't clear the session
    CHECK(!t.update("steam.app.1", "A", 120));
    CHECK(t.current().id == 1);
    CHECK(t.current().startUnix == 100);
}

void otherKeyStartsNewSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.2", "B", 300));
    CHECK(t.current().id == 2);
    CHECK(t.current().appKey == "steam.app.2");
    CHECK(t.current().appName == "B");
    CHECK(t.current().startUnix == 300);
    // and back to A is a new session again, not a resume
    CHECK(t.update("steam.app.1", "A", 400));
    CHECK(t.current().id == 3);
    CHECK(t.current().startUnix == 400);
}

void nameFilledInLater() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "", 100));
    CHECK(t.current().appName.empty());
    CHECK(!t.update("steam.app.1", "A", 150));
    CHECK(t.current().id == 1);
    CHECK(t.current().appName == "A");
    CHECK(t.current().startUnix == 100);
    // a later empty name doesn't wipe it
    CHECK(!t.update("steam.app.1", "", 160));
    CHECK(t.current().appName == "A");
}

void idSourceZeroAndRepeatSkipped() {
    // The source returns 0 and then repeats the previous id: neither may be used
    int calls = 0;
    const int32_t seq[] = {5, 0, 5, 5, 7};
    SessionTracker t([&]() { return seq[calls++]; });
    CHECK(t.update("a", "", 1));
    CHECK(t.current().id == 5);
    CHECK(t.update("b", "", 2));
    CHECK(t.current().id == 7);
}

void defaultCtorGivesNonZeroIds() {
    SessionTracker t;
    CHECK(t.update("a", "", 1));
    CHECK(t.current().id != 0);
}

}  // namespace

int main() {
    firstEmptyKeyGivesNoSession();
    sameKeyKeepsSession();
    compositorFlickerStaysOneSession();
    otherKeyStartsNewSession();
    nameFilledInLater();
    idSourceZeroAndRepeatSkipped();
    defaultCtorGivesNonZeroIds();
    if (failures == 0) std::printf("core_session_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
