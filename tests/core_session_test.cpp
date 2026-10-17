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

void detourWithin60sResumesSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.home", "Home", 130));  // Home for 30 s
    CHECK(t.current().id == 2);
    CHECK(t.update("steam.app.1", "A", 160));  // back after a 30 s detour: the old session goes on
    CHECK(t.current().id == 1);
    CHECK(t.current().appKey == "steam.app.1");
    CHECK(t.current().startUnix == 100);
    // a second short detour from the resumed session resumes it again
    CHECK(t.update("steam.app.home", "Home", 200));
    CHECK(t.update("steam.app.1", "A", 210));
    CHECK(t.current().id == 1);
    CHECK(t.current().startUnix == 100);
}

void detourOver60sIsNewSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.home", "Home", 130));
    CHECK(t.update("steam.app.1", "A", 191));  // 61 s later
    CHECK(t.current().id == 3);
    CHECK(t.current().startUnix == 191);
}

void detourBoundaryIs60s() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.home", "Home", 130));
    CHECK(t.update("steam.app.1", "A", 189));  // 59 s: resumes
    CHECK(t.current().id == 1);
    SessionTracker u(counter());
    CHECK(u.update("steam.app.1", "A", 100));
    CHECK(u.update("steam.app.home", "Home", 130));
    CHECK(u.update("steam.app.1", "A", 190));  // exactly 60 s: not a detour any more (< kSessionDetourSec)
    CHECK(u.current().id == 3);
}

void threeDifferentAppsAreThreeSessions() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.2", "B", 200));
    CHECK(t.update("steam.app.3", "C", 230));  // B ran 30 s, but C isn't A: no resume
    CHECK(t.current().id == 3);
    CHECK(t.current().appName == "C");
    CHECK(t.update("steam.app.1", "A", 240));  // A was two sessions ago: new session
    CHECK(t.current().id == 4);
}

void flickerInsideDetourKeepsDetour() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.home", "Home", 130));
    CHECK(!t.update("", "", 140));  // empty key changes nothing
    CHECK(t.current().id == 2);
    CHECK(t.update("steam.app.1", "A", 150));
    CHECK(t.current().id == 1);
}

void midGameFirstKeyIsASession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.9", "Mid", 5000));
    CHECK(t.current().id == 1);
    CHECK(t.current().appKey == "steam.app.9");
    CHECK(t.current().appName == "Mid");
    CHECK(t.current().startUnix == 5000);
    // nothing to resume to from the very first session: Home then the same game within 60 s resumes it
    CHECK(t.update("steam.app.home", "Home", 5100));
    CHECK(t.update("steam.app.9", "Mid", 5110));
    CHECK(t.current().id == 1);
}

void resumeUpdatesName() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "", 100));
    CHECK(t.update("steam.app.home", "Home", 130));
    CHECK(t.update("steam.app.1", "A", 140));
    CHECK(t.current().id == 1);
    CHECK(t.current().appName == "A");
}

// ---- T11 R3: no scene app is a detour ----

void noAppParksTheSessionAndBackKeepsIt() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.current().leftUnix == 0);
    CHECK(!t.update("", "", 130));
    CHECK(t.current().id == 1);
    CHECK(t.current().leftUnix == 130);
    CHECK(!t.update("", "", 140));  // still gone: leftUnix stays at the first time
    CHECK(t.current().leftUnix == 130);
    CHECK(t.current().appKey == "steam.app.1");
    CHECK(!t.update("steam.app.1", "A", 160));  // back: the no-app time is a gap
    CHECK(t.current().id == 1);
    CHECK(t.current().leftUnix == 0);
    CHECK(t.current().startUnix == 100);
}

void noAppForAMinuteEndsTheSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(!t.update("", "", 130));
    CHECK(!t.update("", "", 189));  // 59 s
    CHECK(t.current().id == 1);
    CHECK(!t.expire(189));
    CHECK(t.current().id == 1);
    CHECK(!t.update("", "", 190));  // 60 s: over
    CHECK(t.current().id == 0);
    CHECK(t.current().appKey.empty());
    // the same game afterwards is a NEW session, not a resume
    CHECK(t.update("steam.app.1", "A", 200));
    CHECK(t.current().id == 2);
    CHECK(t.current().startUnix == 200);
    CHECK(t.current().leftUnix == 0);
}

void expireWorksOnItsOwn() {
    SessionTracker t(counter());
    CHECK(!t.expire(50));  // nothing to expire
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(!t.expire(500));  // the app never left
    CHECK(t.current().id == 1);
    CHECK(!t.update("", "", 130));
    CHECK(!t.expire(189));
    CHECK(t.current().id == 1);
    CHECK(t.expire(190));
    CHECK(t.current().id == 0);
    CHECK(!t.expire(10000));  // already over
}

void noAppThenOtherAppThenBack() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(!t.update("", "", 130));
    CHECK(t.update("steam.app.2", "B", 150));  // another app: a new session, A is the detour's origin
    CHECK(t.current().id == 2);
    CHECK(t.update("steam.app.1", "A", 180));  // within 60 s of B's start: A resumes
    CHECK(t.current().id == 1);
    CHECK(t.current().startUnix == 100);
    CHECK(t.current().leftUnix == 0);

    SessionTracker u(counter());
    CHECK(u.update("steam.app.1", "A", 100));
    CHECK(!u.update("", "", 130));
    CHECK(u.update("steam.app.2", "B", 150));
    CHECK(u.update("steam.app.1", "A", 210));  // B ran 60 s: A is new
    CHECK(u.current().id == 3);
}

void expiredNoAppForgetsThePreviousSession() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(t.update("steam.app.2", "B", 130));
    CHECK(!t.update("", "", 140));
    CHECK(!t.update("", "", 200));  // B has been gone 60 s: over, and A can't come back either
    CHECK(t.current().id == 0);
    CHECK(t.update("steam.app.1", "A", 210));
    CHECK(t.current().id == 3);
}

void flickerKeepsTheId() {
    SessionTracker t(counter());
    CHECK(t.update("steam.app.1", "A", 100));
    CHECK(!t.update("", "", 110));
    CHECK(!t.update("steam.app.1", "A", 112));
    CHECK(t.current().id == 1);
    CHECK(t.current().leftUnix == 0);
    // and it can leave again later with a fresh leftUnix
    CHECK(!t.update("", "", 300));
    CHECK(t.current().leftUnix == 300);
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
    detourWithin60sResumesSession();
    detourOver60sIsNewSession();
    detourBoundaryIs60s();
    threeDifferentAppsAreThreeSessions();
    flickerInsideDetourKeepsDetour();
    midGameFirstKeyIsASession();
    resumeUpdatesName();
    noAppParksTheSessionAndBackKeepsIt();
    noAppForAMinuteEndsTheSession();
    expireWorksOnItsOwn();
    noAppThenOtherAppThenBack();
    expiredNoAppForgetsThePreviousSession();
    flickerKeepsTheId();
    if (failures == 0) std::printf("core_session_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
