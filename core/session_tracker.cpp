#include "session_tracker.h"

#include <random>

namespace fph {

namespace {

int32_t randomSessionId() {
    static std::mt19937 rng {std::random_device {}()};
    std::uniform_int_distribution<int32_t> dist(1, INT32_MAX);
    return dist(rng);
}

}  // namespace

SessionTracker::SessionTracker() : ids_(randomSessionId) {}

SessionTracker::SessionTracker(IdSource ids) : ids_(std::move(ids)) {}

bool SessionTracker::update(const std::string& appKey, const std::string& appName, int64_t nowUnix) {
    if (appKey.empty()) {
        // T11: no scene app (the game quit, or the compositor between loading screens): the session is parked from
        // the moment its app left, and ends if nothing comes back in time
        if (current_.id != 0 && current_.leftUnix == 0) current_.leftUnix = nowUnix;
        expire(nowUnix);
        return false;
    }
    if (current_.id != 0 && appKey == current_.appKey) {
        current_.leftUnix = 0;  // still here, or back in time: the no-app stretch is a gap in the same session
        if (!appName.empty()) current_.appName = appName;
        return false;
    }
    if (previous_.id != 0 && appKey == previous_.appKey && nowUnix - current_.startUnix < kSessionDetourSec) {
        // T11: back to the previous app after a short detour: the detour is dropped, the old session goes on. Any
        // app counts as the detour on purpose, not just Home: Home's app key on the Frame isn't verified
        current_ = previous_;
        current_.leftUnix = 0;
        previous_ = SessionInfo();
        if (!appName.empty()) current_.appName = appName;
        return true;
    }
    previous_ = current_;
    int32_t id = ids_();
    // Never hand out 0 (= no session) or the id just used
    while (id == 0 || id == current_.id) id = ids_();
    current_.id = id;
    current_.appKey = appKey;
    current_.appName = appName;
    current_.startUnix = nowUnix;
    current_.leftUnix = 0;
    return true;
}

bool SessionTracker::expire(int64_t nowUnix) {
    if (current_.id == 0 || current_.leftUnix == 0 || nowUnix - current_.leftUnix < kSessionDetourSec) return false;
    // No app for a whole detour: the session is over, and the session before it can't come back either
    current_ = SessionInfo();
    previous_ = SessionInfo();
    return true;
}

}  // namespace fph
