// Portable parser over a read-only memory interface; production uses ReadProcessMemory, tests use bytes.
#pragma once
#include "stream_metrics.h"
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace fph {
namespace pc {
namespace streaming {
constexpr uint32_t kKnownBuild = 1791581198;
constexpr size_t kFrameStride = 0x78, kFrameSlots = 4096, kTailFrames = 144;
extern const char kCsvHeader[];
extern const char kCsvRow[];
class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(uint64_t address, void* dest, size_t bytes) = 0;
    template <class T> bool get(uint64_t a, T& v) {
        return read(a, &v, sizeof(v));
    }
};
struct Anchors {
    uint32_t timestamp = 0, imageSize = 0;
    uint32_t serverGlobal = 0, mapOffset = 0, aftmOffset = 0, linkPingOffset = 0, uberPingOffset = 0;
    uint32_t stride = 0, fileOffset = 0;
    uint32_t remoteAddressOffset = 0, remoteGetterRva = 0;
    bool l1 = false, l2 = false, l3 = false;
    std::string l1Note, l2Note, l3Note;
};
Anchors findAnchors(const std::vector<uint8_t>& dll, const std::string& expectedHeader = kCsvHeader);
bool sameLoadedBuild(Memory& memory, uint64_t module, const Anchors& anchors);
bool rttiIs(Memory& memory, uint64_t obj, uint64_t module, uint32_t imageSize, const char* expected);
std::string readLinkEndpoint(Memory& memory, uint64_t link, uint64_t module, const Anchors& anchors);
struct Frame {
    uint32_t id = 0, illum = 0;
    std::array<double, 7> ms{}; // game, encode, send, network, decode (queue included), slack, total
    double ping = 0;
};
bool parseFrame(const uint8_t* bytes, Frame& frame);
class FrameWindow {
public:
    // Re-read the complete 144-record tail every tick; incomplete records never enter the dedupe set.
    void add(const std::vector<uint8_t>& tail, double now);
    void clear();
    std::vector<Frame> recent(double now) const;
    uint64_t published() const {
        return published_;
    }

private:
    struct Sample {
        Frame frame;
        double received;
    };
    std::deque<Sample> samples_;
    std::deque<uint32_t> ids_;
    std::set<uint32_t> seen_;
    uint64_t published_ = 0;
    uint32_t newestIllum_ = 0;
    bool haveNewest_ = false;
};
class Reader {
public:
    Reader(Memory& memory, uint64_t module, Anchors anchors);
    void tick(double now, double displayHz, MetricStore& out);
    uint64_t resets() const {
        return resets_;
    }
    uint64_t published() const {
        return frames_.published();
    }

private:
    Memory& memory_;
    uint64_t module_;
    Anchors anchors_;
    uint64_t server_ = 0, head_ = 0, root_ = 0, uber_ = 0, bw_ = 0, mapSize_ = 0, walkedSize_ = 0;
    std::array<uint64_t, 16> links_{};
    uint16_t index_ = 0;
    bool haveIndex_ = false, awaitAdvance_ = false;
    uint64_t resets_ = 0;
    std::map<std::string, uint64_t> nodes_;
    FrameWindow frames_;
    double retry1_ = 0, retry2_ = 0, retry3_ = 0, check2_ = 0, check3_ = 0;
    bool enabled2_ = false, enabled3_ = false;
    bool walkMap(uint64_t head);
    bool named(std::map<std::string, double>& values);
};
// Only these event names become counters. Raw lines are never returned, logged or sent.
struct LogEvents {
    uint32_t resets = 0, badLinks = 0, encoderReinits = 0, throttles = 0;
};
bool parseLogEvent(const std::string& line, LogEvents& events);
class LogParser {
public:
    void feed(const char* data, size_t size);
    void rewritten();
    const LogEvents& events() const {
        return events_;
    }

private:
    LogEvents events_;
    std::string partial_;
    bool dropping_ = false;
};
} // namespace streaming
} // namespace pc
} // namespace fph
