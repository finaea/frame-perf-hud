// T11: the crash recorder (off by default). While it's on, a 1 Hz kit (battery, vph, temps, fan, throttle, uptime)
// goes into a fixed 600-record ring file, written in place and fsync'd every 5 s, next to a "running" marker. A
// clean exit removes the marker; finding it at the next start means the agent (or the headset) died, and the ring
// is kept as a CSV in crashes/ (last 5).
#pragma once

#include "metric_registry.h"
#include "metric_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace crashrec {

constexpr int kRecords = 600;         ///< 10 minutes at 1 Hz
constexpr double kSyncSec = 5.0;      ///< without it a power cut loses up to 30 s (dirty_expire_centisecs 3000)
constexpr uint32_t kRecordSize = 64;  ///< bytes per record on disk
constexpr uint32_t kHeaderSize = 16;

/** One second of the kit, as stored in the ring (little-endian, the Frame's layout). */
struct KitRecord {
    uint64_t seq = 0;  ///< 1, 2, ...; 0 = slot never written
    int64_t unixTime = 0;
    float batteryPct = 0, batteryV = 0, batteryA = 0, batteryW = 0;
    float vphW = 0, cpuC = 0, gpuC = 0, batteryC = 0, fanRpm = 0;
    uint32_t throttle = 0;  ///< bit 0 CPU, bit 1 GPU, bit 2 SteamVR throttling the app
    int32_t uptimeS = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(KitRecord) == kRecordSize, "the ring's record size is part of the file format");

/** @return the sources the kit reads (the scheduler pins them while the recorder is on) */
std::vector<fph::SourceId> kitSources();

/** @return the kit's values now (NaN where a value is N/A) */
KitRecord kitRecord(const fph::MetricStore& store, uint64_t seq, int64_t unixTime);

/** @return the records as CSV, in seq order, empty slots left out */
std::string kitCsv(std::vector<KitRecord> records);

/** @return the ring file's records (all slots, empty ones with seq 0); empty if missing or not a ring */
std::vector<KitRecord> readRing(const std::string& path);

class CrashRecorder {
public:
    /** @param stateDir ~/.local/state/frame-perf-hud: the ring and marker go in crash/, kept logs in crashes/ */
    explicit CrashRecorder(std::string stateDir);
    ~CrashRecorder();
    CrashRecorder(const CrashRecorder&) = delete;
    CrashRecorder& operator=(const CrashRecorder&) = delete;

    /**
     * At start (before setEnabled): if the marker is there, the last run ended uncleanly; the ring is copied to
     * crashes/<time of its last record>.csv and the marker removed.
     * @param log one line for the journal ("" if nothing happened)
     * @return "Crash log saved (hh:mm)" (the last record's local time), or "" if there was no crash log
     */
    std::string recover(std::string& log);

    /**
     * Starts (a fresh ring + the marker) or stops (sync, marker removed) recording.
     * @return false with error if the files can't be made (the recorder stays off)
     */
    bool setEnabled(bool on, std::string& error);
    bool enabled() const { return fd_ >= 0; }

    /** Writes the second's record in place (call once a second while enabled); fsyncs every kSyncSec. */
    void tick(const fph::MetricStore& store, double now, int64_t unixTime);

    /** Clean exit: the same as setEnabled(false). */
    void shutdown();

    uint64_t written() const { return seq_; }
    std::string ringPath() const;
    std::string markerPath() const;
    std::string crashesDir() const;

private:
    std::string stateDir_;
    int fd_ = -1;
    uint64_t seq_ = 0;
    double lastSync_ = 0;
};

}  // namespace crashrec
