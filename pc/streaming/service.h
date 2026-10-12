#pragma once
#include "reader.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace fph {
namespace pc {
namespace streaming {
// Owns all OpenVR and process handles on one worker. UI only transfers immutable snapshots.
class Service {
public:
    explicit Service(bool forceHeaderMismatch = false);
    ~Service();
    void setDemand(bool enabled);
    bool take(MetricStore& store, double now);
    void stop();

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::thread worker_;
    std::atomic<bool> demand_{false}, stop_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<link::WireValue> snapshot_;
    uint64_t generation_ = 0, taken_ = 0;
    void run();
};
} // namespace streaming
} // namespace pc
} // namespace fph
