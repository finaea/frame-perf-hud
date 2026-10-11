// Process lists for the CPU usage items and RAM usage items widgets (7 / 8): one /proc scan feeds both.
// Each process costs one read of /proc/[pid]/stat, which already carries comm, utime + stime and rss
// (the same counters /proc/[pid]/comm and /proc/[pid]/statm give), so ~510 opens per scan instead of ~1530.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/** The fields of /proc/[pid]/stat the lists use. */
struct ProcStat {
    std::string comm;        ///< between the first '(' and the last ')': it may hold spaces and parentheses
    uint64_t utime = 0;      ///< field 14, clock ticks
    uint64_t stime = 0;      ///< field 15, clock ticks
    uint64_t starttime = 0;  ///< field 22, clock ticks since boot (tells a reused pid apart)
    int64_t rssPages = 0;    ///< field 24
};

/**
 * Parses one /proc/[pid]/stat line.
 * @param text the file's contents (needn't be NUL-terminated)
 * @param length bytes in text
 * @param out the fields
 * @return false if the line is cut short or malformed
 */
bool parseProcStat(const char* text, size_t length, ProcStat& out);

/** One list row: a process, or with "sum same name" every process of that name. */
struct ProcessRow {
    std::string name;      ///< comm: the main thread's name, e.g. "vrlinkrunthread"
    std::string exe;       ///< T04: basename of /proc/[pid]/exe ("vrlink"); empty for summed rows, other users' processes
    int pid = 0;           ///< 0 for a summed row
    int count = 1;         ///< processes in this row
    double cpuCorePct = 0.0;   ///< % of one core over the last interval (top's convention: 4 busy cores = 400%)
    double rssMiB = 0.0;
};

/** Both lists, as of the last scan. */
struct ProcessLists {
    std::vector<ProcessRow> byCpu;  ///< highest CPU first; empty until the second scan after a (re)start
    std::vector<ProcessRow> byRam;  ///< highest RSS first
    bool cpuValid = false;          ///< byCpu has a real interval behind it
    int scanned = 0;                ///< processes read
    int vanished = 0;               ///< listed in /proc but gone (or unreadable) by the time we read them
    double scanMs = 0.0;            ///< wall time of the scan
};

/** Per-widget settings (ticket 04 wires them to the layout). */
struct ProcessListOptions {
    size_t cpuMax = 15;
    size_t ramMax = 15;
    bool sumSameName = false;  ///< one row per name, CPU and RSS summed (Halo's option); both lists unless
                               ///< ramSumSameName says otherwise
    int ramSumSameName = -1;   ///< T04: RAM list's own setting (0 / 1), -1 = same as sumSameName
};

/**
 * Ranks rows and keeps the top ones.
 * @param rows one row per process
 * @param sumSameName merge rows with the same name first
 * @param max rows to keep
 * @param byCpu sort by CPU (else by RSS); ties go by name, then pid, so equal rows don't swap every scan
 * @return at most max rows
 */
std::vector<ProcessRow> topRows(std::vector<ProcessRow> rows, bool sumSameName, size_t max, bool byCpu);

/** Scans /proc. Not thread-safe; the scheduler calls it from the main loop. */
class ProcessScanner {
public:
    /**
     * @param procRoot "/proc", or a fixture tree in tests
     * @param ticksPerSecond sysconf(_SC_CLK_TCK); 0 = ask the system
     * @param pageSize sysconf(_SC_PAGESIZE); 0 = ask the system
     */
    explicit ProcessScanner(std::string procRoot = "/proc", long ticksPerSecond = 0, long pageSize = 0);

    /** Forgets the CPU baseline (after a pause), so the next scan gives no CPU list. */
    void reset();

    /**
     * Reads every /proc/[pid]/stat and rebuilds both lists. A process that exits mid-scan is skipped.
     * @param now monotonic seconds
     * @param options list sizes and "sum same name"
     * @return the new lists
     */
    const ProcessLists& scan(double now, const ProcessListOptions& options);

    /** @return the lists from the last scan */
    const ProcessLists& lists() const { return lists_; }

private:
    struct Previous {
        uint64_t starttime = 0;
        uint64_t ticks = 0;  ///< utime + stime
    };

    std::string root_;
    long ticksPerSecond_;
    long pageSize_;
    std::unordered_map<int, Previous> previous_;
    struct ExeName {
        uint64_t starttime = 0;
        std::string name;
    };
    std::unordered_map<int, ExeName> exeNames_;  ///< T04: readlink cache for listed rows only (≤ 2 × max per scan)

    /** T04: fills ProcessRow::exe for the listed rows, from the cache or one readlink each. */
    void fillExeNames(std::vector<ProcessRow>& rows, const std::unordered_map<int, Previous>& current);
    double previousTime_ = -1.0;
    ProcessLists lists_;
};
