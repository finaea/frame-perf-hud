// T09: the agent's side of the camera helper (helper/fph-camgrab.c): the install check, starting and stopping
// it, decoding its messages, and the respawn limit. The camera thread (T10) drives it; until then
// --camera-helper-dump exercises it end to end.
//
// The helper is the only privileged piece and every change to it needs sudo again, so the logic lives here.
#pragma once

#include "fph_camgrab_proto.h"

#include <sys/types.h>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camhelper {

constexpr const char* kInstallDir = "/home/.frame-perf-hud";
constexpr const char* kHelperPath = "/home/.frame-perf-hud/fph-camgrab";
/** Where install.sh puts the installer, for the "not installed" message */
constexpr const char* kInstallerHint = "~/.local/share/frame-perf-hud/install-camera-helper.sh";

// ------------------------------------------------------------------------------------------------ install check

enum class InstallState {
    Ok,
    Missing,        ///< no file (never installed, or uninstalled)
    NotRegular,     ///< a symlink, directory, ...
    WrongOwner,     ///< not root:root
    WrongMode,      ///< not exactly 0755
    NoCapability,   ///< no cap_sys_ptrace=ep (e.g. the file was replaced, which clears it)
    UnsafeDir,      ///< the directory isn't root-owned or is writable by others
};

/** What lstat says about a path (pure input for judgeInstall, so tests needn't touch /home). */
struct FileFacts {
    bool exists = false;
    bool regular = false;
    bool directory = false;
    uid_t uid = 0;
    gid_t gid = 0;
    mode_t mode = 0;  ///< permission bits (& 07777)
};

/**
 * Parses a security.capability xattr (struct vfs_cap_data, revision 1, 2 or 3).
 * @return true if CAP_SYS_PTRACE is permitted and the effective flag is set, for the initial user namespace
 */
bool capXattrHasPtraceEffective(const uint8_t* data, size_t len);

/** The decision, from facts already read. */
InstallState judgeInstall(const FileFacts& dir, const FileFacts& file, const std::vector<uint8_t>& capXattr);

/** Reads the facts from disk and judges them. */
InstallState checkInstall(const char* dir = kInstallDir, const char* path = kHelperPath);

/** @return a short English description ("ok", "not installed", "lost its capability", ...) */
const char* installStateText(InstallState state);

/** @return "Camera helper not installed: run `sudo ~/.local/share/frame-perf-hud/install-camera-helper.sh`" */
std::string notInstalledMessage();

// ------------------------------------------------------------------------------------------------ respawn limit

/** At most one start per minGapSec (5 s): the helper exits whenever XRService does, and SteamVR may flap. */
class RespawnLimiter {
public:
    explicit RespawnLimiter(double minGapSec = 5.0) : minGap_(minGapSec) {}
    /** @return true (and records the start) if a start is allowed at nowSec (a monotonic clock) */
    bool tryStart(double nowSec);
    /** @return the earliest time the next start is allowed */
    double nextAllowed() const { return started_ ? last_ + minGap_ : 0.0; }

private:
    double minGap_;
    double last_ = 0.0;
    bool started_ = false;
};

// ------------------------------------------------------------------------------------------------ outward retry

/**
 * When to re-send OUTWARD_ON. A cam that isn't there yet (SteamVR still starting: no XRService, or not every
 * /dev/video node open) comes back as CAM_GONE and the helper stays up without looking again, so the agent
 * re-sends ON every intervalSec while any cam is missing for a retryable reason (not OFF, not DENIED).
 * CAM and CAM_GONE can arrive in either order. ON rediscovers all four cams and replaces their fds, so once
 * at least one cam is up the retries are capped (a node that never appears doesn't churn the others forever).
 */
class OutwardRetry {
public:
    explicit OutwardRetry(double intervalSec = 2.0, int maxPartialRetries = 30)
        : interval_(intervalSec), maxPartial_(maxPartialRetries) {}

    /** After (re)spawning the helper and sending ON: forget everything. */
    void reset();
    void onCam(uint8_t cam, double nowSec);
    void onGone(uint8_t cam, uint8_t reason, double nowSec);
    /** @return true if the caller should send OUTWARD_ON now (counts as one attempt) */
    bool due(double nowSec);
    /** @return when the next retry is due, 0 if none is pending (for poll timeouts) */
    double nextAt() const { return nextAt_; }
    /** @return true once the cap was hit with some cams still missing */
    bool gaveUp() const { return gaveUp_; }

private:
    enum class CamState : uint8_t { Unknown, Present, Missing, Final };  // Final = OFF / DENIED: don't retry
    void reschedule(double nowSec);

    double interval_;
    int maxPartial_;
    CamState state_[FPH_OUTWARD_CAMS] = {};
    double nextAt_ = 0.0;
    int partialRetries_ = 0;
    bool gaveUp_ = false;
};

// ------------------------------------------------------------------------------------------------ messages

/** Owned file descriptors (closed on destruction unless released). */
class FdList {
public:
    FdList() = default;
    ~FdList() { closeAll(); }
    FdList(const FdList&) = delete;
    FdList& operator=(const FdList&) = delete;
    FdList(FdList&& other) noexcept : fds_(std::move(other.fds_)) { other.fds_.clear(); }
    FdList& operator=(FdList&& other) noexcept;

    void add(int fd) { fds_.push_back(fd); }
    size_t size() const { return fds_.size(); }
    int operator[](size_t i) const { return fds_[i]; }
    /** Hands the fds over: the caller closes them. */
    std::vector<int> release();
    void closeAll();

private:
    std::vector<int> fds_;
};

/** One decoded helper message. Only the member for `type` is filled. */
struct Message {
    fph_msg_type type = FPH_MSG_ERROR;
    fph_msg_hello hello{};
    fph_msg_cam cam{};
    fph_msg_gone gone{};
    fph_msg_eye eye{};
    fph_msg_frames frames{};
    fph_msg_recheck recheck{};
    fph_msg_error error{};
    fph_msg_bye bye{};
    FdList fds;  ///< CAM: one per bufs[] entry; EYE: one per arenas[] entry; else empty
};

enum class DecodeError {
    None,
    TooShort,
    BadMagic,
    BadVersion,  ///< an older / newer helper is installed: reinstall it
    BadType,
    BadLength,
    BadFields,   ///< counts or ids out of range
    FdMismatch,  ///< fd count doesn't match the message
};

/**
 * Decodes one packet. Takes ownership of fds either way (on an error they're closed).
 * @return DecodeError::None and out filled, or the reason
 */
DecodeError decodeMessage(const void* data, size_t len, std::vector<int> fds, Message& out);

const char* decodeErrorText(DecodeError e);

/** @return a fixed-size command packet (what HelperProcess sends) */
fph_cmd makeCommand(fph_op op, uint8_t cam = 0, uint32_t index = 0, uint32_t seq = 0);

// ------------------------------------------------------------------------------------------------ the process

/**
 * One running helper. start() spawns it with a SOCK_SEQPACKET socketpair end as fd 3. The helper dies with
 * the thread that spawned it (PR_SET_PDEATHSIG), on socket EOF, and when XRService / eyetracking exits.
 */
class HelperProcess {
public:
    HelperProcess() = default;
    ~HelperProcess() { stop(); }
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;

    /** @param path the helper binary; @return false with *error set on failure */
    bool start(const char* path, std::string* error);
    /** Closes the socket (the helper exits on EOF), reaps it; SIGKILL if it's still there after 1 s. */
    void stop();

    bool running() const { return sock_ >= 0; }
    /** The socket to poll for POLLIN; -1 when not running. */
    int fd() const { return sock_; }
    pid_t pid() const { return pid_; }

    bool sendOutward(bool on);
    bool sendEye(bool on);
    bool sendRecheck(uint8_t cam, uint32_t index, uint32_t seq);

    enum class ReadResult { Message, WouldBlock, Closed, Bad };
    /** Non-blocking: one message. Closed means the helper exited (call stop(), then respawn via the limiter). */
    ReadResult read(Message& out, DecodeError* why = nullptr);

private:
    bool send(const fph_cmd& cmd);

    int sock_ = -1;
    pid_t pid_ = -1;
};

// ------------------------------------------------------------------------------------------------ debug

struct CameraHelperDumpOptions {
    double seconds = 20.0;
    bool outward = true;
    bool eye = false;
    std::string helperPath;  ///< empty = the installed one (another path is for testing the protocol only)
};

/**
 * --camera-helper-dump: checks the install, starts the helper, and prints what it hands over (fds, format,
 * per-second frame reports and recheck results), respawning it like T10 will. Read-only throughout.
 * @return exit code
 */
int runCameraHelperDump(const CameraHelperDumpOptions& options, const volatile std::sig_atomic_t& stop);

}  // namespace camhelper
