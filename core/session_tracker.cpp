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
    if (appKey.empty()) return false;  // the compositor is showing: keep the current session
    if (current_.id != 0 && appKey == current_.appKey) {
        if (!appName.empty()) current_.appName = appName;
        return false;
    }
    if (previous_.id != 0 && appKey == previous_.appKey && nowUnix - current_.startUnix < kSessionDetourSec) {
        // T11: back to the previous app after a short detour: the detour is dropped, the old session goes on. Any
        // app counts as the detour on purpose, not just Home: Home's app key on the Frame isn't verified
        current_ = previous_;
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
    return true;
}

}  // namespace fph
