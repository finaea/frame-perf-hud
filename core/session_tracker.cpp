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
