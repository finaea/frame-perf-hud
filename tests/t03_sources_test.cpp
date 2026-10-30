// T03 sources: /proc process scan (fixture trees), Wi-Fi signal/channel helpers, stream link classification,
// vrlink journal lines and vrlink process detection. Linux only (symlinks, /proc layout).
#include "process_scan.h"
#include "stream_link.h"
#include "wifi_link.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

namespace fs = std::filesystem;

bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) < eps;
}

// ---- fixture /proc tree ----

class Fixture {
public:
    Fixture() {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base != nullptr ? base : "/tmp") + "/fph-t03-XXXXXX";
        if (::mkdtemp(pattern.data()) == nullptr) {
            std::printf("FAIL: mkdtemp\n");
            std::exit(2);
        }
        root_ = pattern;
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    const std::string& root() const { return root_; }

    void write(const std::string& relative, const std::string& content) {
        const fs::path path = fs::path(root_) / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }
    void mkdir(const std::string& relative) { fs::create_directories(fs::path(root_) / relative); }
    void remove(const std::string& relative) {
        std::error_code ec;
        fs::remove_all(fs::path(root_) / relative, ec);
    }
    void symlink(const std::string& target, const std::string& relative) {
        const fs::path path = fs::path(root_) / relative;
        fs::create_directories(path.parent_path());
        std::error_code ec;
        fs::remove(path, ec);
        fs::create_symlink(target, path);
    }

private:
    std::string root_;
};

/** A full /proc/[pid]/stat line (52 fields' worth is unnecessary: we stop after rsslim). */
std::string statLine(int pid, const std::string& comm, unsigned long utime, unsigned long stime,
                     unsigned long starttime, long rssPages) {
    std::string s = std::to_string(pid) + " (" + comm + ") S";
    // fields 4..13: ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
    s += " 1 1 1 0 -1 4194304 10 0 0 0";
    s += " " + std::to_string(utime) + " " + std::to_string(stime);  // 14, 15
    s += " 0 0 20 0 1 0";                                            // 16..21 (cutime cstime prio nice threads itreal)
    s += " " + std::to_string(starttime);                            // 22
    s += " 1000000";                                                 // 23 vsize
    s += " " + std::to_string(rssPages);                             // 24
    s += " 18446744073709551615\n";                                  // 25 rsslim
    return s;
}

void writeProc(Fixture& f, int pid, const std::string& comm, unsigned long utime, unsigned long stime,
               unsigned long starttime, long rssPages) {
    f.write(std::to_string(pid) + "/stat", statLine(pid, comm, utime, stime, starttime, rssPages));
}

// ---- parseProcStat ----

void parseNormal() {
    const std::string line = statLine(42, "bash", 7, 3, 999, 256);
    ProcStat s;
    CHECK(parseProcStat(line.data(), line.size(), s));
    CHECK(s.comm == "bash");
    CHECK(s.utime == 7);
    CHECK(s.stime == 3);
    CHECK(s.starttime == 999);
    CHECK(s.rssPages == 256);
}

void parseOddComm() {
    for (const char* comm : {"Web Content", "a) b", "evil) S 1 2 3 (", "(x)"}) {
        const std::string line = statLine(7, comm, 11, 22, 333, 44);
        ProcStat s;
        CHECK(parseProcStat(line.data(), line.size(), s));
        CHECK(s.comm == comm);
        CHECK(s.utime == 11 && s.stime == 22 && s.starttime == 333 && s.rssPages == 44);
    }
}

void parseMalformed() {
    ProcStat s;
    CHECK(!parseProcStat("", 0, s));
    CHECK(!parseProcStat("123 no parens here", 18, s));
    CHECK(!parseProcStat("123 (open only", 14, s));
    const std::string full = statLine(1, "x", 1, 1, 1, 1);
    // cut off before field 24 (rss): too few fields
    const size_t cut = full.find(" 1000000");
    CHECK(!parseProcStat(full.data(), cut, s));
}

// ---- displayName ----

void displayNameCases() {
    CHECK(displayName("vrlinkrunthread", "vrlink") == "vrlink");
    CHECK(displayName("at-spi-bus-laun", "at-spi-bus-launcher") == "at-spi-bus-launcher");
    CHECK(displayName("run_vrlink.sh", "bash") == "run_vrlink.sh");
    CHECK(displayName("com.vrchat.oculus", "app_process64") == "com.vrchat.oculus");
    CHECK(displayName("comm", "") == "comm");
    CHECK(displayName("same", "same") == "same");
}

// ---- ProcessScanner: exe-derived names, sum-same-name groups by the displayed name ----

void scannerDisplayNameSumAndSplit() {
    Fixture f;
    // the main thread was renamed ("vrlinkrunthread"); exe resolves to the real binary
    writeProc(f, 10, "vrlinkrunthread", 0, 0, 1000, 256);
    f.symlink("/usr/bin/vrlink", "10/exe");
    // another vrlink process whose comm is already the exe's name
    writeProc(f, 20, "vrlink", 0, 0, 2000, 256);
    f.symlink("/usr/bin/vrlink", "20/exe");
    // a script: comm is the script, exe is the interpreter it keeps its own name
    writeProc(f, 30, "run_vrlink.sh", 0, 0, 3000, 256);
    f.symlink("/usr/bin/bash", "30/exe");

    ProcessScanner scanner(f.root(), 100, 4096);
    ProcessListOptions opts;
    opts.sumSameName = true;
    const ProcessLists& summed = scanner.scan(1.0, opts);
    CHECK(summed.byRam.size() == 2);  // the two vrlink rows merge; the script stays its own row
    int vrlinkCount = -1;
    for (const ProcessRow& r : summed.byRam) {
        if (r.name == "vrlink") vrlinkCount = r.count;
        CHECK(r.name != "run_vrlink.sh" || r.count == 1);
    }
    CHECK(vrlinkCount == 2);

    opts.sumSameName = false;
    const ProcessLists& split = scanner.scan(2.0, opts);
    CHECK(split.byRam.size() == 3);
    int vrlinkRows = 0;
    for (const ProcessRow& r : split.byRam) {
        if (r.name == "vrlink") ++vrlinkRows;
    }
    CHECK(vrlinkRows == 2);  // both pids show under "vrlink", kept as separate rows
}

// ---- topRows ----

ProcessRow row(const std::string& name, int pid, double cpu, double rss) {
    ProcessRow r;
    r.name = name;
    r.pid = pid;
    r.cpuCorePct = cpu;
    r.rssMiB = rss;
    return r;
}

void topRowsOrdering() {
    const std::vector<ProcessRow> rows = {row("a", 1, 5, 100), row("b", 2, 50, 10), row("c", 3, 20, 300),
                                          row("d", 4, 1, 200)};
    const auto cpu = topRows(rows, false, 3, true);
    CHECK(cpu.size() == 3);
    CHECK(cpu[0].name == "b" && cpu[1].name == "c" && cpu[2].name == "a");
    const auto ram = topRows(rows, false, 3, false);
    CHECK(ram.size() == 3);
    CHECK(ram[0].name == "c" && ram[1].name == "d" && ram[2].name == "a");
    // fewer rows than max: all, still sorted
    const auto all = topRows(rows, false, 10, true);
    CHECK(all.size() == 4 && all[3].name == "d");
    CHECK(topRows(rows, false, 0, true).empty());
}

void topRowsTieBreak() {
    // equal keys: name, then pid
    const std::vector<ProcessRow> rows = {row("z", 1, 10, 10), row("a", 9, 10, 10), row("a", 3, 10, 10)};
    const auto out = topRows(rows, false, 3, true);
    CHECK(out[0].name == "a" && out[0].pid == 3);
    CHECK(out[1].name == "a" && out[1].pid == 9);
    CHECK(out[2].name == "z");
    // the same with the partial-sort path (max < size)
    const auto cut = topRows(rows, false, 2, false);
    CHECK(cut.size() == 2 && cut[0].pid == 3 && cut[1].pid == 9);
}

void topRowsSumSameNameValues() {
    const std::vector<ProcessRow> rows = {row("chrome", 1, 10, 100), row("chrome", 2, 15, 200), row("chrome", 3, 5, 50),
                                          row("vim", 4, 29, 20)};
    const auto cpu = topRows(rows, true, 5, true);
    CHECK(cpu.size() == 2);
    CHECK(cpu[0].name == "chrome");
    CHECK(cpu[0].pid == 0);
    CHECK(cpu[0].count == 3);
    CHECK(near(cpu[0].cpuCorePct, 30.0));
    CHECK(near(cpu[0].rssMiB, 350.0));
    CHECK(cpu[1].name == "vim");
    const auto ram = topRows(rows, true, 1, false);
    CHECK(ram.size() == 1 && ram[0].name == "chrome");
}

// ---- ProcessScanner ----

void scannerRamAndFirstScan() {
    Fixture f;
    writeProc(f, 100, "busy", 0, 0, 5000, 1024);   // 4 MiB at 4096 B pages
    writeProc(f, 200, "big", 0, 0, 6000, 25600);   // 100 MiB
    writeProc(f, 300, "idle", 0, 0, 7000, 256);    // 1 MiB
    f.mkdir("self");                                // non-pid dirs are ignored
    f.mkdir("12abc");
    f.mkdir("0");
    f.write("cpuinfo", "x");

    ProcessScanner scanner(f.root(), 100, 4096);
    ProcessListOptions opts;
    const ProcessLists& l = scanner.scan(10.0, opts);
    CHECK(!l.cpuValid);
    CHECK(l.byCpu.empty());
    CHECK(l.scanned == 3);
    CHECK(l.vanished == 0);
    CHECK(l.byRam.size() == 3);
    CHECK(l.byRam[0].name == "big" && near(l.byRam[0].rssMiB, 100.0));
    CHECK(l.byRam[1].name == "busy" && near(l.byRam[1].rssMiB, 4.0));
    CHECK(l.byRam[2].name == "idle" && near(l.byRam[2].rssMiB, 1.0));
}

void scannerCpuFromTwoScans() {
    Fixture f;
    writeProc(f, 100, "busy", 100, 0, 5000, 1024);
    writeProc(f, 200, "calm", 10, 0, 6000, 1024);
    writeProc(f, 300, "idle", 5, 5, 7000, 1024);

    ProcessScanner scanner(f.root(), 100, 4096);
    ProcessListOptions opts;
    scanner.scan(10.0, opts);
    // 1 s later: busy +50 ticks (utime 30 + stime 20), calm +10, idle +0
    writeProc(f, 100, "busy", 130, 20, 5000, 1024);
    writeProc(f, 200, "calm", 20, 0, 6000, 1024);
    const ProcessLists& l = scanner.scan(11.0, opts);
    CHECK(l.cpuValid);
    CHECK(l.byCpu.size() == 3);
    CHECK(l.byCpu[0].name == "busy" && near(l.byCpu[0].cpuCorePct, 50.0));
    CHECK(l.byCpu[1].name == "calm" && near(l.byCpu[1].cpuCorePct, 10.0));
    CHECK(l.byCpu[2].name == "idle" && near(l.byCpu[2].cpuCorePct, 0.0));

    // an interval of 2 s halves the percentage
    writeProc(f, 100, "busy", 230, 20, 5000, 1024);  // +100 ticks over 2 s = 50%
    const ProcessLists& l2 = scanner.scan(13.0, opts);
    CHECK(near(l2.byCpu[0].cpuCorePct, 50.0));

    // more than one core: 300 ticks in 1 s = 300%
    writeProc(f, 100, "busy", 530, 20, 5000, 1024);
    const ProcessLists& l3 = scanner.scan(14.0, opts);
    CHECK(near(l3.byCpu[0].cpuCorePct, 300.0));
}

void scannerCpuMaxAndSum() {
    Fixture f;
    for (int i = 0; i < 5; ++i) writeProc(f, 100 + i, "worker", 0, 0, 1000 + i, 256);
    writeProc(f, 200, "solo", 0, 0, 2000, 256);
    ProcessScanner scanner(f.root(), 100, 4096);
    ProcessListOptions opts;
    opts.cpuMax = 2;
    opts.ramMax = 3;
    scanner.scan(0.5, opts);
    for (int i = 0; i < 5; ++i) writeProc(f, 100 + i, "worker", 10, 0, 1000 + i, 256);
    writeProc(f, 200, "solo", 30, 0, 2000, 256);
    const ProcessLists& l = scanner.scan(1.5, opts);
    CHECK(l.byCpu.size() == 2);
    CHECK(l.byCpu[0].name == "solo");
    CHECK(l.byRam.size() == 3);

}

void scannerSumSameNameOrder() {
    Fixture f;
    for (int i = 0; i < 5; ++i) writeProc(f, 100 + i, "worker", 0, 0, 1000 + i, 256);
    writeProc(f, 200, "solo", 0, 0, 2000, 256);
    ProcessScanner scanner(f.root(), 100, 4096);
    ProcessListOptions opts;
    opts.sumSameName = true;
    scanner.scan(0.0 + 1.0, opts);
    for (int i = 0; i < 5; ++i) writeProc(f, 100 + i, "worker", 10, 0, 1000 + i, 256);  // 10% each, 50% total
    writeProc(f, 200, "solo", 30, 0, 2000, 256);                                         // 30%
    const ProcessLists& l = scanner.scan(2.0, opts);
    CHECK(l.byCpu.size() == 2);
    CHECK(l.byCpu[0].name == "worker" && l.byCpu[0].count == 5 && l.byCpu[0].pid == 0);
    CHECK(near(l.byCpu[0].cpuCorePct, 50.0));
    CHECK(near(l.byCpu[0].rssMiB, 5.0));
    CHECK(l.byCpu[1].name == "solo");
}

void scannerVanishedPids() {
    Fixture f;
    writeProc(f, 100, "ok", 0, 0, 1000, 256);
    f.mkdir("300");                          // pid dir, no stat file (exited between readdir and open)
    f.write("301/stat", "");                 // empty read
    f.write("302/stat", "302 (cut) S 1 1");  // truncated
    f.write("303/stat", "garbage with no parens\n");
    ProcessScanner scanner(f.root(), 100, 4096);
    const ProcessLists& l = scanner.scan(1.0, ProcessListOptions());
    CHECK(l.scanned == 1);
    CHECK(l.vanished == 4);
    CHECK(l.byRam.size() == 1 && l.byRam[0].name == "ok");

    // a pid that was there on the first scan and is gone on the second: no crash, not listed
    f.remove("100");
    writeProc(f, 101, "new", 0, 0, 1100, 256);
    const ProcessLists& l2 = scanner.scan(2.0, ProcessListOptions());
    CHECK(l2.cpuValid);
    CHECK(l2.byCpu.size() == 1 && l2.byCpu[0].name == "new" && near(l2.byCpu[0].cpuCorePct, 0.0));  // no baseline: 0%
}

void scannerReusedPid() {
    Fixture f;
    writeProc(f, 100, "old", 1000, 0, 5000, 256);
    ProcessScanner scanner(f.root(), 100, 4096);
    scanner.scan(1.0, ProcessListOptions());
    // same pid, another process (different starttime) with fewer ticks, then one with more ticks
    writeProc(f, 100, "reborn", 50, 0, 9000, 256);
    const ProcessLists& l = scanner.scan(2.0, ProcessListOptions());
    CHECK(l.byCpu.size() == 1 && near(l.byCpu[0].cpuCorePct, 0.0));
    writeProc(f, 100, "reborn2", 5000000, 0, 9500, 256);  // would read as a huge % if the baseline were reused
    const ProcessLists& l2 = scanner.scan(3.0, ProcessListOptions());
    CHECK(l2.byCpu.size() == 1 && near(l2.byCpu[0].cpuCorePct, 0.0));
}

void scannerReset() {
    Fixture f;
    writeProc(f, 100, "p", 0, 0, 5000, 256);
    ProcessScanner scanner(f.root(), 100, 4096);
    scanner.scan(1.0, ProcessListOptions());
    CHECK(scanner.scan(2.0, ProcessListOptions()).cpuValid);
    scanner.reset();
    const ProcessLists& l = scanner.scan(3.0, ProcessListOptions());
    CHECK(!l.cpuValid && l.byCpu.empty());
    CHECK(scanner.scan(4.0, ProcessListOptions()).cpuValid);
    CHECK(scanner.lists().cpuValid);
}

void scannerMissingRoot() {
    ProcessScanner scanner("/nonexistent/fph-t03", 100, 4096);
    const ProcessLists& l = scanner.scan(1.0, ProcessListOptions());
    CHECK(l.scanned == 0 && l.byRam.empty() && !l.cpuValid);
}

// ---- wifi_link.h pure functions ----

void signalChoice() {
    SignalReadings r;
    // the Frame's AP side: signal and signal avg read 0, only ack values are real
    r.signal = 0;
    r.signalAvg = 0;
    r.ackSignal = -46;
    r.ackSignalAvg = -45;
    CHECK(chooseSignalDbm(r, false) == -45);

    SignalReadings onlyAck;
    onlyAck.ackSignal = -46;
    CHECK(chooseSignalDbm(onlyAck, false) == -46);

    SignalReadings zeros;
    zeros.signal = 0;
    zeros.ackSignalAvg = 0;
    CHECK(std::isnan(chooseSignalDbm(zeros, false)));
    CHECK(std::isnan(chooseSignalDbm(SignalReadings(), false)));
    CHECK(std::isnan(chooseSignalDbm(SignalReadings(), true)));

    // AP order falls through to signal avg, then signal
    SignalReadings apFallback;
    apFallback.signal = -60;
    apFallback.signalAvg = -55;
    CHECK(chooseSignalDbm(apFallback, false) == -55);
    apFallback.signalAvg = 0;
    CHECK(chooseSignalDbm(apFallback, false) == -60);

    // managed side (home Wi-Fi on the Frame: signal -49, beacon avg -48, avg ack -59, signal avg 0)
    SignalReadings m;
    m.signal = -49;
    m.beaconSignalAvg = -48;
    m.ackSignalAvg = -59;
    m.signalAvg = 0;
    CHECK(chooseSignalDbm(m, true) == -49);
    m.signal = 0;
    CHECK(chooseSignalDbm(m, true) == -48);
    m.beaconSignalAvg = SignalReadings().beaconSignalAvg;  // NaN
    CHECK(chooseSignalDbm(m, true) == -59);
}

void channels() {
    CHECK(channelFromFreq(2412) == 1);
    CHECK(channelFromFreq(2437) == 6);
    CHECK(channelFromFreq(2472) == 13);
    CHECK(channelFromFreq(2484) == 14);
    CHECK(channelFromFreq(5180) == 36);
    CHECK(channelFromFreq(5500) == 100);
    CHECK(channelFromFreq(5825) == 165);
    CHECK(channelFromFreq(5935) == 2);
    CHECK(channelFromFreq(5955) == 1);
    CHECK(channelFromFreq(6135) == 37);
    CHECK(channelFromFreq(7115) == 233);
    CHECK(channelFromFreq(58320) == 1);
    CHECK(channelFromFreq(0) == 0);
    CHECK(channelFromFreq(1234) == 0);
}

void bands() {
    CHECK(bandFromFreq(2412) == 1);
    CHECK(bandFromFreq(2484) == 1);
    CHECK(bandFromFreq(5180) == 2);
    CHECK(bandFromFreq(5885) == 2);
    CHECK(bandFromFreq(5935) == 3);
    CHECK(bandFromFreq(6135) == 3);
    CHECK(bandFromFreq(58320) == 4);
    CHECK(bandFromFreq(0) == 0);
    CHECK(bandFromFreq(3000) == 0);
}

void widths() {
    CHECK(widthMhzFromNl80211(0) == 20);
    CHECK(widthMhzFromNl80211(1) == 20);
    CHECK(widthMhzFromNl80211(2) == 40);
    CHECK(widthMhzFromNl80211(3) == 80);
    CHECK(widthMhzFromNl80211(4) == 160);
    CHECK(widthMhzFromNl80211(5) == 160);
    CHECK(widthMhzFromNl80211(13) == 320);
    CHECK(widthMhzFromNl80211(99) == 0);
}

AdapterInfo adapter(bool present, bool connected, double signal, double rx) {
    AdapterInfo a;
    a.present = present;
    a.connected = connected;
    a.signalDbm = signal;
    a.rxMbps = rx;
    return a;
}

void combine() {
    WifiAdapters both;
    both.dongle = adapter(true, true, -45, 200);
    both.home = adapter(true, true, -55, 10);
    WifiInfo i = combineAdapters(both);
    CHECK(i.interfaceUp && i.connected && !i.homeWifi);
    CHECK(i.signalDbm == -45 && i.rxMbps == 200);

    both.dongle.connected = false;
    i = combineAdapters(both);
    CHECK(i.connected && i.homeWifi && i.signalDbm == -55 && i.rxMbps == 10);

    both.home.connected = false;
    i = combineAdapters(both);
    CHECK(i.interfaceUp && !i.connected);
    CHECK(std::isnan(i.signalDbm));

    CHECK(!combineAdapters(WifiAdapters()).interfaceUp);
}

// ---- stream_link ----

void linkMode() {
    const double nan = std::nan("");
    CHECK(classifyLinkMode(false, 200, 0) == 0);  // vrlink not running: none, whatever the rates
    CHECK(classifyLinkMode(false, 200, 200) == 0);
    CHECK(classifyLinkMode(true, 200, 0) == 1);
    CHECK(classifyLinkMode(true, 0, 200) == 2);
    CHECK(classifyLinkMode(true, 100, 100) == 3);
    CHECK(classifyLinkMode(true, 1, 1) == 4);
    CHECK(classifyLinkMode(true, nan, nan) == 4);
    CHECK(classifyLinkMode(true, nan, 50) == 2);
    CHECK(classifyLinkMode(true, 50, nan) == 1);
    // the threshold itself counts as carrying the stream
    CHECK(classifyLinkMode(true, kStreamLinkThresholdMbps, 0) == 1);
    CHECK(classifyLinkMode(true, kStreamLinkThresholdMbps - 0.001, 0) == 4);
    CHECK(classifyLinkMode(true, 3, 0, 2.0) == 1);  // custom threshold
}

void donglShare() {
    CHECK(near(dongleSharePct(150, 50), 75.0));
    CHECK(near(dongleSharePct(100, 0), 100.0));
    CHECK(near(dongleSharePct(0, 100), 0.0));
    CHECK(near(dongleSharePct(std::nan(""), 100), 0.0));
    CHECK(std::isnan(dongleSharePct(2, 2)));  // together under the threshold
    CHECK(std::isnan(dongleSharePct(std::nan(""), std::nan(""))));
    CHECK(near(dongleSharePct(2.5, 2.5), 50.0));  // exactly at the threshold
}

void vrlinkExe() {
    CHECK(isVrlinkExe("/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink"));
    CHECK(isVrlinkExe("/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink (deleted)"));
    CHECK(isVrlinkExe("vrlink"));
    CHECK(!isVrlinkExe("/opt/steamvr/tools/vrlink/bin/linuxarm64/run_vrlink.sh"));
    CHECK(!isVrlinkExe("/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlinkrunthread"));
    CHECK(!isVrlinkExe("/usr/bin/bash"));
    CHECK(!isVrlinkExe(""));
}

void vrlinkLines() {
    const auto classify = [](const std::string& line, int& packets) {
        return classifyVrlinkLine(line.data(), line.size(), packets);
    };
    int p = -1;
    CHECK(classify("- SVLFEC: substantive retry (19 packets) due to frame skip (skipped 14884, last sub 14883, group 2, "
                   "expected size 183931)",
                   p) == VrlinkEvent::FecRetry);
    CHECK(p == 19);
    CHECK(classify("- SVLFEC: frame 1234 was entirely dropped, initializing and re-requesting", p) ==
          VrlinkEvent::FrameDropped);
    CHECK(p == 0);
    CHECK(classify("- [SVLFEC] received late metadata for frame [ 5678 ]", p) == VrlinkEvent::LateMetadata);
    CHECK(classify("- ERROR: While trying to request retransmission of packet #42, too many packets were missing even "
                   "to retransmit.  Aborting stream.",
                   p) == VrlinkEvent::StreamAbort);
    CHECK(classify("- Fault, SVLFEC Received ExperiencedAnUnrecoverableError()", p) == VrlinkEvent::Unrecoverable);
    CHECK(classify("[OMITTING additional logs for 5 seconds]", p) == VrlinkEvent::LogOmitted);
    CHECK(classify("- SVLDataLink::HandleFECChange( 32 )", p) == VrlinkEvent::None);
    CHECK(classify("", p) == VrlinkEvent::None);
}

void vrlinkCounts() {
    VrlinkCounts c;
    c.add(VrlinkEvent::FecRetry, 19);
    c.add(VrlinkEvent::FecRetry, 5);
    c.add(VrlinkEvent::FecRetry, 0);  // no packet count parsed: counts as a retry, adds no packets
    c.add(VrlinkEvent::FrameDropped, 0);
    c.add(VrlinkEvent::LateMetadata, 0);
    c.add(VrlinkEvent::LateMetadata, 0);
    c.add(VrlinkEvent::StreamAbort, 0);
    c.add(VrlinkEvent::Unrecoverable, 0);
    c.add(VrlinkEvent::LogOmitted, 0);
    c.add(VrlinkEvent::None, 0);
    CHECK(c.fecRetries == 3 && c.fecRetryPackets == 24);
    CHECK(c.framesDropped == 1 && c.lateMetadata == 2);
    CHECK(c.streamAborts == 1 && c.unrecoverable == 1 && c.logOmitted == 1);
}

void vrlinkWatch() {
    const std::string vrlinkPath = "/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink";
    Fixture f;
    // a decoy: comm starts with "vrlink" but the exe is the wrapper script
    f.write("50/comm", "vrlink-helper\n");
    f.symlink("/opt/steamvr/tools/vrlink/bin/linuxarm64/run_vrlink.sh", "50/exe");
    f.write("60/comm", "bash\n");
    f.symlink("/usr/bin/bash", "60/exe");

    VrlinkWatch watch(f.root());
    CHECK(!watch.running(0.0));
    CHECK(watch.pid() == 0);

    // vrlink appears; its main thread was renamed, so comm is "vrlinkrunthread"
    f.write("70/comm", "vrlinkrunthread\n");
    f.symlink(vrlinkPath, "70/exe");
    CHECK(!watch.running(1.0));  // rescan is rate-limited to every 5 s
    CHECK(watch.running(5.0));
    CHECK(watch.pid() == 70);
    CHECK(watch.running(5.1));  // cheap path: the remembered pid

    // it exits
    f.remove("70");
    CHECK(!watch.running(5.2));  // pid gone, rescan was just done: not yet
    CHECK(watch.pid() == 0);
    CHECK(!watch.running(10.5));

    // and comes back under another pid
    f.write("80/comm", "vrlink\n");
    f.symlink(vrlinkPath + " (deleted)", "80/exe");
    CHECK(watch.running(16.0));
    CHECK(watch.pid() == 80);
}

}  // namespace

int main() {
    parseNormal();
    parseOddComm();
    parseMalformed();
    displayNameCases();
    scannerDisplayNameSumAndSplit();
    topRowsOrdering();
    topRowsTieBreak();
    topRowsSumSameNameValues();
    scannerRamAndFirstScan();
    scannerCpuFromTwoScans();
    scannerCpuMaxAndSum();
    scannerSumSameNameOrder();
    scannerVanishedPids();
    scannerReusedPid();
    scannerReset();
    scannerMissingRoot();
    signalChoice();
    channels();
    bands();
    widths();
    combine();
    linkMode();
    donglShare();
    vrlinkExe();
    vrlinkLines();
    vrlinkCounts();
    vrlinkWatch();
    if (failures == 0) std::printf("t03_sources_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
