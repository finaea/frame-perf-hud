// T11 review M1: the Logs page's Record switch shows the writer's real state (start, stop, failed start, shutdown).
#include "session_service.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

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

std::string tempDir(const char* tag) {
    const fs::path p = fs::temp_directory_path() / (std::string("fph-svc-") + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

fph::SessionInfo game() {
    fph::SessionInfo s;
    s.id = 5;
    s.appKey = "steam.app.438100";
    s.appName = "VRChat";
    s.startUnix = 1792200000;
    return s;
}

size_t recordFiles(const std::string& dir) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir + "/saves", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().find("_record.csv") != std::string::npos) ++n;
    }
    return n;
}

void recordSwitchFollowsWriter() {
    const std::string dir = tempDir("record");
    SessionService s(dir);
    CHECK(!s.ui().recording);
    uint64_t rev = s.ui().revision;
    s.setRecord(true, game(), 1.0);
    CHECK(s.recording());
    CHECK(s.ui().recording);  // the switch shows On while rows go to disk
    CHECK(s.ui().revision != rev);
    CHECK(recordFiles(dir) == 1);
    rev = s.ui().revision;
    s.setRecord(false, game(), 2.0);
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    CHECK(s.ui().revision != rev);
    // shutdown while recording turns it off too
    s.setRecord(true, game(), 3.0);
    CHECK(s.ui().recording);
    s.shutdown();
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    fs::remove_all(dir);
}

void failedStartStaysOff() {
    // a state dir that can't be created: the switch must stay Off
    SessionService s("/proc/fph-no-such-dir");
    s.setRecord(true, game(), 1.0);
    CHECK(!s.recording());
    CHECK(!s.ui().recording);
    CHECK(s.ui().recordStatus.rfind("Record failed", 0) == 0);
}

}  // namespace

int main() {
    recordSwitchFollowsWriter();
    failedStartStaysOff();
    if (failures == 0) std::printf("session_service_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
