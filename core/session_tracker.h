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
    int32_t id = 0;            ///< 0 = none (no scene app seen since start, or T11: it ended with no app)
    std::string appKey;        ///< full key, e.g. "steam.app.438100" (not cut; the metric is)
    std::string appName;       ///< e.g. "VRChat"; may be empty if SteamVR doesn't know it
    int64_t startUnix = 0;     ///< wall-clock seconds when the session started
    int64_t leftUnix = 0;      ///< T11: when its app went away (no scene app), 0 while it runs
};

/**
 * T11: a detour to another app shorter than this (SteamVR Home between two runs of a game) doesn't split it, and a
 * session with no app for this long is over.
 */
constexpr int64_t kSessionDetourSec = 60;

/**
 * Applies the session rules:
 * - a new session starts only when a non-empty app key differs from the current session's key,
 * - T11: going back to the previous app within kSessionDetourSec of leaving it resumes the previous session (same
 *   id and start), so game → Home → same game stays one game,
 * - T11: an empty key (pid 0: no scene app, the compositor is showing) parks the current session from that moment
 *   (leftUnix): the same app back within kSessionDetourSec goes on with the same session (a loading-screen flicker
 *   A → none → A stays one), another app is a detour as above, and no app for kSessionDetourSec ends it (id 0),
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

    /**
     * T11: ends the session once its app has been gone kSessionDetourSec (update() does this too; call it when the
     * scene app isn't re-queried, e.g. once a second).
     * @return true if this ended the session (current() is id 0 from now)
     */
    bool expire(int64_t nowUnix);

    /** @return the current session (id 0 before the first non-empty key and after expire()) */
    const SessionInfo& current() const { return current_; }

private:
    IdSource ids_;
    SessionInfo current_;
    SessionInfo previous_;  ///< T11: the session before current_, resumable while current_ is a short detour
};

}  // namespace fph
