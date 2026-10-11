#include "stream_link.h"

#include <dirent.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

double orZero(double v) {
    return std::isnan(v) ? 0.0 : v;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

}  // namespace

unsigned classifyLinkMode(bool vrlinkRunning, double dongleRxMbps, double homeRxMbps, double thresholdMbps) {
    if (!vrlinkRunning) return 0;
    const bool dongle = orZero(dongleRxMbps) >= thresholdMbps;
    const bool home = orZero(homeRxMbps) >= thresholdMbps;
    if (dongle && home) return 3;
    if (dongle) return 1;
    if (home) return 2;
    return 4;
}

double dongleSharePct(double dongleRxMbps, double homeRxMbps, double thresholdMbps) {
    const double d = orZero(dongleRxMbps);
    const double total = d + orZero(homeRxMbps);
    if (total < thresholdMbps) return std::nan("");
    return d / total * 100.0;
}

bool isVrlinkExe(const std::string& exeTarget) {
    std::string_view path = exeTarget;
    // A replaced binary reads "<path> (deleted)"
    constexpr std::string_view kDeleted = " (deleted)";
    if (path.size() > kDeleted.size() && path.substr(path.size() - kDeleted.size()) == kDeleted) {
        path.remove_suffix(kDeleted.size());
    }
    const size_t slash = path.rfind('/');
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return base == "vrlink";
}

std::string vrlinkAppHint(const char* cmdline, size_t length) {
    // Arguments are NUL-separated; the value follows the "-A" argument
    bool next = false;
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && cmdline[i] != '\0') continue;
        const std::string_view arg(cmdline + start, i - start);
        if (next) return arg.empty() ? "" : "steam.app." + std::string(arg);
        next = arg == "-A";
        start = i + 1;
    }
    return "";
}

// ---- VrlinkWatch ----

VrlinkWatch::VrlinkWatch(std::string procRoot) : root_(std::move(procRoot)) {}

bool VrlinkWatch::isVrlink(int pid) const {
    char path[512];
    char target[512];
    std::snprintf(path, sizeof(path), "%s/%d/exe", root_.c_str(), pid);
    const ssize_t n = ::readlink(path, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    return isVrlinkExe(target);
}

bool VrlinkWatch::running(double now) {
    if (pid_ != 0 && isVrlink(pid_)) return true;
    pid_ = 0;
    if (lastScan_ >= 0 && now - lastScan_ < kRescanIntervalSec) return false;
    lastScan_ = now;
    DIR* dir = ::opendir(root_.c_str());
    if (dir == nullptr) return false;
    char path[512];
    char comm[64];
    while (dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '1' || name[0] > '9') continue;
        // Cheap filter first: vrlink's comm starts with "vrlink" ("vrlink" or its renamed "vrlinkrunthread")
        std::snprintf(path, sizeof(path), "%s/%s/comm", root_.c_str(), name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) continue;  // exited mid-scan
        const bool got = std::fgets(comm, sizeof(comm), file) != nullptr;
        std::fclose(file);
        if (!got || std::strncmp(comm, "vrlink", 6) != 0) continue;
        const int pid = std::atoi(name);
        if (isVrlink(pid)) {
            pid_ = pid;
            break;
        }
    }
    ::closedir(dir);
    return pid_ != 0;
}

// ---- journal lines ----

VrlinkEvent classifyVrlinkLine(const char* message, size_t length, int& packets) {
    packets = 0;
    const std::string_view m(message, length);
    if (contains(m, "SVLFEC: substantive retry (")) {
        const size_t at = m.find("retry (") + 7;
        packets = std::atoi(std::string(m.substr(at, 12)).c_str());
        return VrlinkEvent::FecRetry;
    }
    if (contains(m, "SVLFEC: frame ") && contains(m, "was entirely dropped")) return VrlinkEvent::FrameDropped;
    if (contains(m, "[SVLFEC] received late metadata")) return VrlinkEvent::LateMetadata;
    if (contains(m, "too many packets were missing") && contains(m, "Aborting stream")) return VrlinkEvent::StreamAbort;
    if (contains(m, "ExperiencedAnUnrecoverableError")) return VrlinkEvent::Unrecoverable;
    if (contains(m, "[OMITTING additional logs")) return VrlinkEvent::LogOmitted;
    return VrlinkEvent::None;
}

void VrlinkCounts::add(VrlinkEvent event, int packets) {
    switch (event) {
        case VrlinkEvent::FecRetry:
            ++fecRetries;
            if (packets > 0) fecRetryPackets += packets;
            break;
        case VrlinkEvent::FrameDropped: ++framesDropped; break;
        case VrlinkEvent::LateMetadata: ++lateMetadata; break;
        case VrlinkEvent::StreamAbort: ++streamAborts; break;
        case VrlinkEvent::Unrecoverable: ++unrecoverable; break;
        case VrlinkEvent::LogOmitted: ++logOmitted; break;
        case VrlinkEvent::None: break;
    }
}

// ---- VrlinkJournal ----

VrlinkJournal::~VrlinkJournal() {
    close();
}

void VrlinkJournal::close() {
    if (journal_ != nullptr) sd_journal_close(journal_);
    journal_ = nullptr;
}

bool VrlinkJournal::open(std::string& error) {
    int r = sd_journal_open(&journal_, SD_JOURNAL_CURRENT_USER);
    if (r < 0) {
        journal_ = nullptr;
        error = std::string("sd_journal_open: ") + std::strerror(-r);
        return false;
    }
    r = sd_journal_add_match(journal_, "SYSLOG_IDENTIFIER=vrlink", 0);
    if (r < 0) {
        error = std::string("sd_journal_add_match: ") + std::strerror(-r);
        close();
        return false;
    }
    // Sets up inotify, so sd_journal_process() notices appended and rotated files
    (void)sd_journal_get_fd(journal_);
    if (!cursor_.empty() && sd_journal_seek_cursor(journal_, cursor_.c_str()) >= 0) {
        // Step onto the last entry already read, so next() returns the one after it. If that entry is gone
        // (rotated away), we landed on a newer one we haven't counted: step back so next() returns it
        if (sd_journal_next(journal_) > 0 && sd_journal_test_cursor(journal_, cursor_.c_str()) <= 0) {
            sd_journal_previous(journal_);
        }
        return true;
    }
    // First open: count only what happens from now on
    sd_journal_seek_tail(journal_);
    sd_journal_previous(journal_);
    return true;
}

bool VrlinkJournal::poll(std::string& error) {
    if (journal_ == nullptr && !open(error)) return false;
    sd_journal_process(journal_);
    bool readAny = false;
    for (int i = 0; i < kMaxEntriesPerPoll; ++i) {
        const int r = sd_journal_next(journal_);
        if (r == 0) break;
        if (r < 0) {
            error = std::string("sd_journal_next: ") + std::strerror(-r);
            close();  // reopened from the saved cursor next time
            return false;
        }
        readAny = true;
        const void* data = nullptr;
        size_t length = 0;
        if (sd_journal_get_data(journal_, "MESSAGE", &data, &length) >= 0 && length > 8) {
            int packets = 0;
            // data is "MESSAGE=<text>"
            counts_.add(classifyVrlinkLine(static_cast<const char*>(data) + 8, length - 8, packets), packets);
        }
    }
    // next() returning 0 doesn't move, so we're still on the last entry read
    char* cursor = nullptr;
    if (readAny && sd_journal_get_cursor(journal_, &cursor) >= 0 && cursor != nullptr) {
        cursor_ = cursor;
        std::free(cursor);
    }
    return true;
}
