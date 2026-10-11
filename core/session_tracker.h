// Headset session identity: which game is running, and a session id that changes only when the game does.
// Fed by the OpenVR scene app (queried on connect and on VREvent_SceneApplicationChanged). Platform-free,
// so the PC app can follow its own scene app with the same rules.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace fph {

/** What the current session is. */
struct SessionInfo {
    int32_t id = 0;            ///< 0 = no session yet (no scene app seen since start)
    std::string appKey;        ///< full key, e.g. "steam.app.438100" (not cut; the metric is)
    std::string appName;       ///< e.g. "VRChat"; may be empty if SteamVR doesn't know it
    int64_t startUnix = 0;     ///< wall-clock seconds when the session started
};

/**
 * Applies the session rules:
 * - a new session starts only when a non-empty app key differs from the current session's key,
 * - an empty key (pid 0: the compositor is showing, e.g. between loading screens) never ends or splits the
 *   current session, so a flicker A → none → A stays one session,
 * - the app name may be filled in later for the same key without starting a new session.
 */
class SessionTracker {
public:
    /** @return a new non-zero session id */
    using IdSource = std::function<int32_t()>;

    /** Uses random non-zero ids. */
    SessionTracker();
    /** @param ids where session ids come from (tests) */
    explicit SessionTracker(IdSource ids);

    /**
     * Feeds the current scene app.
     * @param appKey key from GetApplicationKeyByProcessId, empty when there is no scene app
     * @param appName display name, may be empty
     * @param nowUnix wall-clock seconds
     * @return true if this started a new session
     */
    bool update(const std::string& appKey, const std::string& appName, int64_t nowUnix);

    /** @return the current session (id 0 before the first non-empty key) */
    const SessionInfo& current() const { return current_; }

private:
    IdSource ids_;
    SessionInfo current_;
};

}  // namespace fph
