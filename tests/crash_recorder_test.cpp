// T11: the crash recorder (headset/crash_recorder). Uses a throwaway state dir; never touches the real one.
#include "crash_recorder.h"
#include "session_log.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace crashrec;
namespace fs = std::filesystem;
namespace metric = fph::metric;

std::string tempDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path p = fs::temp_directory_path() / (std::string("fph-crash-") + tag + "-" + std::to_string(stamp));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string f;
    while (std::getline(ss, f, ',')) out.push_back(f);
    if (!line.empty() && line.back() == ',') out.push_back("");
    return out;
}

size_t countFiles(const std::string& dir) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
}

/** A store with a few kit values (the rest stays N/A). */
void fillKit(fph::MetricStore& store) {
    CHECK(store.setF32(metric::kBatteryPct, 74.5, 1.0));
    CHECK(store.setF32(metric::kBatteryVoltageV, 4.012, 1.0));
    CHECK(store.setF32(metric::kBatteryCurrentMa, -1500.0, 1.0));
    CHECK(store.setF32(metric::kBatteryPowerW, -6.02, 1.0));
    CHECK(store.setF32(metric::kPowerVphW, 6.25, 1.0));
    CHECK(store.setF32(metric::kTempCpuC, 61.0, 1.0));
    CHECK(store.setF32(metric::kTempGpuC, 55.0, 1.0));
    CHECK(store.setF32(metric::kTempBatteryC, 30.0, 1.0));
    CHECK(store.setF32(metric::kFanRpm, 3000.0, 1.0));
    CHECK(store.setI32(metric::kUptimeS, 4321, 1.0));
}

void kitRecordAndCsv() {
    fph::MetricStore store;
    KitRecord empty = kitRecord(store, 1, 1000);
    CHECK(empty.seq == 1 && empty.unixTime == 1000);
    CHECK(std::isnan(empty.batteryPct) && std::isnan(empty.vphW));
    CHECK(empty.throttle == 0);
    CHECK(empty.uptimeS == -1);

    fillKit(store);
    CHECK(store.setEnum(metric::kThrottleGpu, 1, 1.0));
    CHECK(store.setEnum(metric::kFrameThrottled, 1, 1.0));
    const KitRecord r = kitRecord(store, 7, 1000000);
    CHECK(r.seq == 7);
    CHECK(std::fabs(r.batteryPct - 74.5f) < 1e-3);
    CHECK(std::fabs(r.batteryA - -1.5f) < 1e-3);  // mA -> A
    CHECK(std::fabs(r.vphW - 6.25f) < 1e-3);
    CHECK(r.throttle == (2u | 4u));  // GPU + app, not CPU
    CHECK(r.uptimeS == 4321);

    // unsorted input, an empty slot (seq 0): sorted by seq, the empty one left out
    KitRecord a = kitRecord(store, 3, 1000002), b = kitRecord(store, 1, 1000000), c = kitRecord(store, 2, 1000001);
    KitRecord blank;
    const std::string csv = kitCsv({a, blank, b, c});
    const std::vector<std::string> rows = lines(csv);
    CHECK(rows.size() == 4);
    CHECK(rows.size() == 4 && rows[0].rfind("unix,time,battery_pct", 0) == 0);
    CHECK(rows.size() == 4 && rows[1].rfind("1000000,", 0) == 0);
    CHECK(rows.size() == 4 && rows[3].rfind("1000002,", 0) == 0);
    if (rows.size() == 4) {
        const std::vector<std::string> f = fields(rows[1]);
        CHECK(f.size() == 15);
        if (f.size() == 15) {
            CHECK(f[1] == fph::slog::clockTime(1000000));
            CHECK(f[2] == "74.5");
            CHECK(f[3] == "4.012");
            CHECK(f[4] == "-1.500");
            CHECK(f[6] == "6.25");
            CHECK(f[7] == "61.0");
            CHECK(f[8] == "55.0");
            CHECK(f[9] == "30.0");
            CHECK(f[10] == "3000");
            CHECK(f[11] == "0" && f[12] == "1" && f[13] == "1");
            CHECK(f[14] == "4321");
        }
    }
    // N/A values are empty cells
    const std::vector<std::string> naRows = lines(kitCsv({empty}));
    CHECK(naRows.size() == 2);
    if (naRows.size() == 2) {
        const std::vector<std::string> f = fields(naRows[1]);
        CHECK(f.size() == 15 && f[2].empty() && f[6].empty() && f[14].empty());
    }
    CHECK(!kitSources().empty());
}

void ringFileFormat() {
    const std::string dir = tempDir("ring");
    {
        CrashRecorder rec(dir);
        std::string error;
        CHECK(!rec.enabled());
        CHECK(rec.setEnabled(true, error));
        CHECK(error.empty());
        CHECK(rec.enabled());
        CHECK(fs::exists(rec.ringPath()) && fs::exists(rec.markerPath()));
        CHECK(rec.ringPath() == dir + "/crash/ring.bin");
        CHECK(rec.markerPath() == dir + "/crash/running");
        CHECK(rec.crashesDir() == dir + "/crashes");
        CHECK(fs::file_size(rec.ringPath()) == kHeaderSize + static_cast<uintmax_t>(kRecords) * kRecordSize);
        const std::string head = readFile(rec.ringPath()).substr(0, 8);
        CHECK(head == "FPHCRSH1");
        CHECK(rec.setEnabled(true, error));  // already on: nothing changes
        CHECK(rec.written() == 0);
        // a fresh ring has every slot empty
        for (const KitRecord& r : readRing(rec.ringPath())) CHECK(r.seq == 0);
        CHECK(readRing(rec.ringPath()).size() == static_cast<size_t>(kRecords));

        fph::MetricStore store;
        fillKit(store);
        for (int i = 0; i < 650; ++i) rec.tick(store, i, 1000000 + i);
        CHECK(rec.written() == 650);
        const std::vector<KitRecord> ring = readRing(rec.ringPath());
        CHECK(ring.size() == static_cast<size_t>(kRecords));
        // slot 0..49 were overwritten by seq 601..650
        CHECK(ring[0].seq == 601 && ring[49].seq == 650 && ring[50].seq == 51 && ring[599].seq == 600);
        const std::vector<std::string> rows = lines(kitCsv(ring));
        CHECK(rows.size() == 601);  // header + 600
        if (rows.size() == 601) {
            CHECK(rows[1].rfind("1000050,", 0) == 0);  // oldest kept = seq 51
            CHECK(rows[600].rfind("1000649,", 0) == 0);
            for (size_t i = 2; i < rows.size(); ++i) {
                if (std::stoll(rows[i]) != std::stoll(rows[i - 1]) + 1) {
                    CHECK(false);  // seq order = time order, no gaps
                    break;
                }
            }
        }
        rec.shutdown();
        CHECK(!rec.enabled());
        CHECK(!fs::exists(rec.markerPath()));
        CHECK(fs::exists(rec.ringPath()));
        rec.tick(store, 1000, 2000000);  // off: ignored
        CHECK(rec.written() == 650);
    }
    fs::remove_all(dir);
}

void crashIsRecovered() {
    const std::string dir = tempDir("crash");
    const int64_t last = 1000649;
    {
        CrashRecorder rec(dir);
        std::string error;
        CHECK(rec.setEnabled(true, error));
        fph::MetricStore store;
        fillKit(store);
        for (int i = 0; i < 650; ++i) rec.tick(store, i, 1000000 + i);
        // destroyed without shutdown() = the process died
    }
    CHECK(fs::exists(dir + "/crash/running"));
    CrashRecorder next(dir);
    std::string log;
    const std::string note = next.recover(log);
    CHECK(note == "Crash log kept (" + fph::slog::clockTime(last).substr(0, 5) + ")");
    CHECK(note.size() == 22);
    CHECK(!log.empty());
    CHECK(!fs::exists(dir + "/crash/running"));
    const std::string csvPath = dir + "/crashes/" + fph::slog::fileTime(last) + "_crash.csv";
    CHECK(fs::exists(csvPath));
    const std::vector<std::string> rows = lines(readFile(csvPath));
    CHECK(rows.size() == 601);
    CHECK(rows.size() == 601 && rows[1].find("74.5") != std::string::npos);
    CHECK(countFiles(dir + "/crashes") == 1);
    // asking again: the marker is gone, nothing to recover
    CHECK(next.recover(log).empty());
    CHECK(log.empty());
    fs::remove_all(dir);
}

void cleanShutdownIsNotACrash() {
    const std::string dir = tempDir("clean");
    {
        CrashRecorder rec(dir);
        std::string error;
        CHECK(rec.setEnabled(true, error));
        fph::MetricStore store;
        fillKit(store);
        for (int i = 0; i < 10; ++i) rec.tick(store, i, 3000 + i);
        rec.shutdown();
    }
    CrashRecorder next(dir);
    std::string log;
    CHECK(next.recover(log).empty());
    CHECK(log.empty());
    CHECK(!fs::exists(dir + "/crashes"));
    // never enabled at all: nothing either
    const std::string dir2 = tempDir("never");
    CrashRecorder none(dir2);
    CHECK(none.recover(log).empty());
    // setEnabled(false) on a recorder that's off is fine
    std::string error;
    CHECK(none.setEnabled(false, error));
    fs::remove_all(dir);
    fs::remove_all(dir2);
}

void crashBeforeFirstTick() {
    const std::string dir = tempDir("empty");
    {
        CrashRecorder rec(dir);
        std::string error;
        CHECK(rec.setEnabled(true, error));
    }  // died before the first second
    CrashRecorder next(dir);
    std::string log;
    CHECK(next.recover(log).empty());  // an empty ring is no crash log
    CHECK(!log.empty());
    CHECK(!fs::exists(dir + "/crash/running"));
    CHECK(!fs::exists(dir + "/crashes"));
    fs::remove_all(dir);
}

void keepsFiveCrashLogs() {
    const std::string dir = tempDir("keep");
    fph::MetricStore store;
    fillKit(store);
    std::string log;
    for (int k = 0; k < 8; ++k) {
        CrashRecorder rec(dir);
        rec.recover(log);  // the previous run's crash
        std::string error;
        CHECK(rec.setEnabled(true, error));
        for (int i = 0; i < 3; ++i) rec.tick(store, i, 2000000 + k * 100000 + i);
    }
    CrashRecorder last(dir);
    CHECK(!last.recover(log).empty());
    CHECK(countFiles(dir + "/crashes") == static_cast<size_t>(fph::slog::kKeepCrashLogs));
    // the newest 5 stay: runs 3..7 (last record of run k is 2000002 + k * 100000)
    CHECK(fs::exists(dir + "/crashes/" + fph::slog::fileTime(2000002 + 7 * 100000) + "_crash.csv"));
    CHECK(fs::exists(dir + "/crashes/" + fph::slog::fileTime(2000002 + 3 * 100000) + "_crash.csv"));
    CHECK(!fs::exists(dir + "/crashes/" + fph::slog::fileTime(2000002 + 2 * 100000) + "_crash.csv"));
    CHECK(!fs::exists(dir + "/crashes/" + fph::slog::fileTime(2000002) + "_crash.csv"));
    fs::remove_all(dir);
}

void readRingEdgeCases() {
    const std::string dir = tempDir("edge");
    CHECK(readRing(dir + "/missing.bin").empty());
    {
        std::ofstream(dir + "/garbage.bin", std::ios::binary) << "this is not a ring file at all, no magic here";
    }
    CHECK(readRing(dir + "/garbage.bin").empty());
    // a ring cut short by a power loss keeps the records that are there
    CrashRecorder rec(dir);
    std::string error;
    CHECK(rec.setEnabled(true, error));
    fph::MetricStore store;
    fillKit(store);
    for (int i = 0; i < 5; ++i) rec.tick(store, i, 4000 + i);
    fs::resize_file(rec.ringPath(), kHeaderSize + 3 * kRecordSize);
    const std::vector<KitRecord> cut = readRing(rec.ringPath());
    CHECK(cut.size() == 3);
    CHECK(cut.size() == 3 && cut[0].seq == 1 && cut[2].seq == 3);
    rec.shutdown();
    fs::remove_all(dir);
}

void setEnabledFailsCleanly() {
    CrashRecorder rec("/proc/fph-no-such-dir/state");
    std::string error;
    CHECK(!rec.setEnabled(true, error));
    CHECK(!error.empty());
    CHECK(!rec.enabled());
}

}  // namespace

int main() {
    kitRecordAndCsv();
    ringFileFormat();
    crashIsRecovered();
    cleanShutdownIsNotACrash();
    crashBeforeFirstTick();
    keepsFiveCrashLogs();
    readRingEdgeCases();
    setEnabledFailsCleanly();
    if (failures == 0) std::printf("crash_recorder_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
