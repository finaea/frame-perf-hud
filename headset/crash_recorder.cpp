#include "crash_recorder.h"

#include "atomic_file.h"
#include "session_log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace crashrec {

namespace {

constexpr char kMagic[8] = {'F', 'P', 'H', 'C', 'R', 'S', 'H', '1'};

/** Ring header: magic, record size, record count. */
struct Header {
    char magic[8];
    uint32_t recordSize;
    uint32_t records;
};
static_assert(sizeof(Header) == kHeaderSize, "header size is part of the file format");

float value(const fph::MetricStore& store, uint16_t id) {
    return static_cast<float>(store.f32Or(id, std::numeric_limits<double>::quiet_NaN()));
}

std::string csvNum(float v, int decimals) {
    if (!(v == v)) return "";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(v));
    return buffer;
}

bool fileExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

std::vector<fph::SourceId> kitSources() {
    using fph::SourceId;
    return {SourceId::Battery, SourceId::Power, SourceId::Temps, SourceId::Fan, SourceId::Throttle, SourceId::Uptime};
}

KitRecord kitRecord(const fph::MetricStore& store, uint64_t seq, int64_t unixTime) {
    using namespace fph::metric;
    KitRecord r;
    r.seq = seq;
    r.unixTime = unixTime;
    r.batteryPct = value(store, kBatteryPct);
    r.batteryV = value(store, kBatteryVoltageV);
    r.batteryA = value(store, kBatteryCurrentMa) / 1000.0f;
    r.batteryW = value(store, kBatteryPowerW);
    r.vphW = value(store, kPowerVphW);
    r.cpuC = value(store, kTempCpuC);
    r.gpuC = value(store, kTempGpuC);
    r.batteryC = value(store, kTempBatteryC);
    r.fanRpm = value(store, kFanRpm);
    r.throttle = (store.intOr(kThrottleCpu, 0) != 0 ? 1u : 0u) | (store.intOr(kThrottleGpu, 0) != 0 ? 2u : 0u) |
                 (store.intOr(kFrameThrottled, 0) != 0 ? 4u : 0u);
    r.uptimeS = static_cast<int32_t>(store.intOr(kUptimeS, -1));
    return r;
}

std::string kitCsv(std::vector<KitRecord> records) {
    records.erase(std::remove_if(records.begin(), records.end(), [](const KitRecord& r) { return r.seq == 0; }),
                  records.end());
    std::sort(records.begin(), records.end(), [](const KitRecord& a, const KitRecord& b) { return a.seq < b.seq; });
    std::string out =
        "unix,time,battery_pct,battery_v,battery_a,battery_w,vph_w,cpu_c,gpu_c,battery_c,fan_rpm,throttle_cpu,"
        "throttle_gpu,throttle_app,uptime_s\n";
    for (const KitRecord& r : records) {
        out += std::to_string(r.unixTime) + "," + fph::slog::clockTime(r.unixTime) + "," + csvNum(r.batteryPct, 1) + "," +
               csvNum(r.batteryV, 3) + "," + csvNum(r.batteryA, 3) + "," + csvNum(r.batteryW, 2) + "," +
               csvNum(r.vphW, 2) + "," + csvNum(r.cpuC, 1) + "," + csvNum(r.gpuC, 1) + "," + csvNum(r.batteryC, 1) +
               "," + csvNum(r.fanRpm, 0) + "," + std::to_string(r.throttle & 1) + "," +
               std::to_string((r.throttle >> 1) & 1) + "," + std::to_string((r.throttle >> 2) & 1) + "," +
               (r.uptimeS >= 0 ? std::to_string(r.uptimeS) : std::string()) + "\n";
    }
    return out;
}

std::vector<KitRecord> readRing(const std::string& path) {
    std::vector<KitRecord> out;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return out;
    Header h {};
    if (std::fread(&h, sizeof(h), 1, file) == 1 && std::memcmp(h.magic, kMagic, sizeof(kMagic)) == 0 &&
        h.recordSize == kRecordSize && h.records > 0 && h.records <= 100000) {
        out.resize(h.records);
        const size_t n = std::fread(out.data(), kRecordSize, h.records, file);
        out.resize(n);  // a short file (cut by the power loss) keeps what's there
    }
    std::fclose(file);
    return out;
}

CrashRecorder::CrashRecorder(std::string stateDir) : stateDir_(std::move(stateDir)) {}

CrashRecorder::~CrashRecorder() {
    if (fd_ >= 0) ::close(fd_);  // without removing the marker: only shutdown() is a clean exit
}

std::string CrashRecorder::ringPath() const {
    return stateDir_ + "/crash/ring.bin";
}

std::string CrashRecorder::markerPath() const {
    return stateDir_ + "/crash/running";
}

std::string CrashRecorder::crashesDir() const {
    return stateDir_ + "/crashes";
}

std::string CrashRecorder::recover(std::string& log) {
    log.clear();
    if (!fileExists(markerPath())) return "";
    std::vector<KitRecord> records = readRing(ringPath());
    const KitRecord* last = nullptr;
    for (const KitRecord& r : records) {
        if (r.seq != 0 && (last == nullptr || r.seq > last->seq)) last = &r;
    }
    std::string note;
    if (last == nullptr) {
        log = "the last run didn't end cleanly, but its crash ring is empty";
    } else {
        const int64_t lastUnix = last->unixTime;
        const std::string path = crashesDir() + "/" + fph::slog::fileTime(lastUnix) + "_crash.csv";
        std::string error;
        if (fph::atomicWriteFile(path, kitCsv(records), error)) {
            fph::slog::pruneByCount(crashesDir(), fph::slog::kKeepCrashLogs);
            note = "Crash log saved (" + fph::slog::clockTime(lastUnix).substr(0, 5) + ")";
            log = "the last run didn't end cleanly: its crash ring went to " + path;
        } else {
            log = "the last run didn't end cleanly, but its crash ring couldn't be kept: " + error;
        }
    }
    ::unlink(markerPath().c_str());
    return note;
}

bool CrashRecorder::setEnabled(bool on, std::string& error) {
    if (on == enabled()) return true;
    if (!on) {
        ::fdatasync(fd_);
        ::close(fd_);
        fd_ = -1;
        ::unlink(markerPath().c_str());
        return true;
    }
    if (!fph::makeDirectories(stateDir_ + "/crash")) {
        error = "can't create " + stateDir_ + "/crash";
        return false;
    }
    const int fd = ::open(ringPath().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't open " + ringPath() + ": " + std::strerror(errno);
        return false;
    }
    // A fresh ring: every slot zero (seq 0 = empty), so an old run's records never mix in
    Header h {};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.recordSize = kRecordSize;
    h.records = kRecords;
    const off_t size = kHeaderSize + static_cast<off_t>(kRecordSize) * kRecords;
    if (::ftruncate(fd, 0) != 0 || ::ftruncate(fd, size) != 0 || ::pwrite(fd, &h, sizeof(h), 0) != sizeof(h) ||
        ::fdatasync(fd) != 0) {
        error = "can't set up " + ringPath() + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    const int marker = ::open(markerPath().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (marker < 0) {
        error = "can't create " + markerPath() + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::fsync(marker);
    ::close(marker);
    fd_ = fd;
    seq_ = 0;
    lastSync_ = 0;
    return true;
}

void CrashRecorder::tick(const fph::MetricStore& store, double now, int64_t unixTime) {
    if (fd_ < 0) return;
    const KitRecord r = kitRecord(store, ++seq_, unixTime);
    const off_t at = kHeaderSize + static_cast<off_t>((seq_ - 1) % kRecords) * kRecordSize;
    if (::pwrite(fd_, &r, sizeof(r), at) != static_cast<ssize_t>(sizeof(r))) {
        std::fprintf(stderr, "[crash] Writing the ring failed: %s\n", std::strerror(errno));
    }
    if (now - lastSync_ >= kSyncSec) {
        ::fdatasync(fd_);
        lastSync_ = now;
    }
}

void CrashRecorder::shutdown() {
    std::string error;
    setEnabled(false, error);
}

}  // namespace crashrec
