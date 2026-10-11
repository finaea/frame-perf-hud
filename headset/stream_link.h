// Headset side of the stream (part of widget 10): which link carries it, and vrlink's own error lines.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct sd_journal;

/** A link counts as carrying the stream above this receive rate. The stream runs at ~150-330 Mbit/s. */
constexpr double kStreamLinkThresholdMbps = 5.0;

/**
 * Link mode from per-interface receive rates. NaN rates count as 0.
 * @return an fph::StreamLinkMode value (core/metric_registry.h): 0 none, 1 dongle, 2 Wi-Fi, 3 multi, 4 idle
 */
unsigned classifyLinkMode(bool vrlinkRunning, double dongleRxMbps, double homeRxMbps,
                          double thresholdMbps = kStreamLinkThresholdMbps);

/**
 * @return the dongle's share of the received bytes (%), NaN when the two together are under the threshold
 */
double dongleSharePct(double dongleRxMbps, double homeRxMbps, double thresholdMbps = kStreamLinkThresholdMbps);

/**
 * @param exeTarget readlink of /proc/[pid]/exe
 * @return true if it's the vrlink binary. Its comm can't be used: vrlink renames its main thread
 *         ("vrlinkrunthread" on the Frame, 2026-10-11), and that rename is what /proc/[pid]/comm shows
 */
bool isVrlinkExe(const std::string& exeTarget);

/** The headset's scene app key while PC-streaming: vrlink itself (2026-10-11). */
constexpr const char* kVrlinkAppKey = "vrlink.client";

/**
 * vrlink's "-A <appid>" from its NUL-separated /proc/[pid]/cmdline, as "steam.app.<appid>".
 * @return "" if there's no -A
 */
std::string vrlinkAppHint(const char* cmdline, size_t length);

/** Whether vrlink runs. Remembers its pid; rescans /proc only when that pid is gone, at most every 5 s. */
class VrlinkWatch {
public:
    /** @param procRoot "/proc", or a fixture tree in tests */
    explicit VrlinkWatch(std::string procRoot = "/proc");

    /**
     * @param now monotonic seconds
     * @return true if vrlink runs
     */
    bool running(double now);

    /** @return the pid found last, 0 if none */
    int pid() const { return pid_; }

    static constexpr double kRescanIntervalSec = 5.0;

private:
    std::string root_;
    int pid_ = 0;
    double lastScan_ = -1.0;

    bool isVrlink(int pid) const;
};

/** Kinds of vrlink journal line this counts. */
enum class VrlinkEvent {
    None,
    FecRetry,       ///< "SVLFEC: substantive retry (N packets) due to frame skip (...)"
    FrameDropped,   ///< "SVLFEC: frame N was entirely dropped, initializing and re-requesting"
    LateMetadata,   ///< "[SVLFEC] received late metadata for frame [ N ]"
    StreamAbort,    ///< "ERROR: ... too many packets were missing even to retransmit.  Aborting stream."
    Unrecoverable,  ///< "Fault, SVLFEC Received ExperiencedAnUnrecoverableError()"
    LogOmitted,     ///< "[OMITTING additional logs for N seconds]" (vrlink's own rate limit)
};

/**
 * Classifies one MESSAGE= value. Patterns are from the Frame's user journal, 2026-10-11 (3 days of lines).
 * @param message the message text
 * @param length bytes in message
 * @param packets for FecRetry: N, else 0
 */
VrlinkEvent classifyVrlinkLine(const char* message, size_t length, int& packets);

/** Counts since the agent started. */
struct VrlinkCounts {
    int32_t fecRetries = 0;
    int32_t fecRetryPackets = 0;
    int32_t framesDropped = 0;
    int32_t lateMetadata = 0;
    int32_t streamAborts = 0;
    int32_t unrecoverable = 0;
    int32_t logOmitted = 0;

    /** Adds one classified line. */
    void add(VrlinkEvent event, int packets);
};

/**
 * Follows the user journal (SD_JOURNAL_CURRENT_USER, SYSLOG_IDENTIFIER=vrlink) with a cursor.
 * The first open starts at the tail, so history from before the agent started isn't counted.
 */
class VrlinkJournal {
public:
    VrlinkJournal() = default;
    ~VrlinkJournal();
    VrlinkJournal(const VrlinkJournal&) = delete;
    VrlinkJournal& operator=(const VrlinkJournal&) = delete;

    /**
     * Reads the entries since the last call (at most kMaxEntriesPerPoll; the rest next time).
     * @param error why it couldn't read, when it returns false
     * @return false if the journal can't be opened or read (it retries next call)
     */
    bool poll(std::string& error);

    const VrlinkCounts& counts() const { return counts_; }

    static constexpr int kMaxEntriesPerPoll = 2000;

private:
    sd_journal* journal_ = nullptr;
    std::string cursor_;  ///< last entry read: a reopen continues from here
    VrlinkCounts counts_;

    bool open(std::string& error);
    void close();
};
