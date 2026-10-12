// pc/settings: settings.json round trip, first run, a broken file, atomic save, the remembered-headsets list.
#include "link_wire.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
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

using namespace fph;
using namespace fph::pc;
namespace fs = std::filesystem;

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeAll(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string hexId(char c) {
    return std::string(32, c);
}

Settings fullSettings() {
    Settings s;
    s.pcId = hexId('a');
    s.manualIp = "192.168.1.47";
    s.onTop = true;
    s.trayHintShown = true;
    s.cameraFps = 15;
    s.window.valid = true;
    s.window.x = -1200;
    s.window.y = 40;
    s.window.width = 900;
    s.window.height = 700;
    s.window.maximized = true;
    s.headsets.push_back({hexId('b'), "Frame \"one\"", {"192.168.1.47", "10.35.78.1"}});
    s.headsets.push_back({hexId('c'), "Frame two ü", {"10.0.0.2"}});
    s.headsets.push_back({hexId('d'), "", {}});
    return s;
}

void testRoundTrip() {
    const Settings s = fullSettings();
    Settings back;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(settingsFromJson(settingsToJson(s), back, warnings, error));
    CHECK(warnings.empty());
    CHECK(back == s);
    CHECK(back.headsets.size() == 3);
    CHECK(back.headsets[0].name == "Frame \"one\"");
    CHECK(back.headsets[1].name == "Frame two ü");
    CHECK(back.headsets[0].ips.size() == 2 && back.headsets[0].ips[1] == "10.35.78.1");
    CHECK(back.window.x == -1200 && back.window.maximized);
    // defaults too
    Settings d;
    d.pcId = hexId('e');
    Settings dback;
    CHECK(settingsFromJson(settingsToJson(d), dback, warnings, error));
    CHECK(dback == d);
}

void testTolerance() {
    Settings out;
    std::vector<std::string> warnings;
    std::string error;
    const std::string text =
        "{\"pc_id\": \"" + hexId('f') + "\", \"manual_ip\": \"not an ip\", \"camera_fps\": 99, \"future_key\": [1,2],"
        " \"window\": {\"valid\": true, \"x\": 1, \"y\": 2, \"width\": 50, \"height\": 50},"
        " \"headsets\": [{\"id\": \"short\", \"name\": \"x\"}, {\"id\": \"" + hexId('1') + "\", \"name\": \"ok\","
        " \"ips\": [\"1.2.3.4\", \"bogus\", \"1.2.3.5\", \"1.2.3.6\", \"1.2.3.7\", \"1.2.3.8\"]}]}";
    CHECK(settingsFromJson(text, out, warnings, error));
    CHECK(out.pcId == hexId('f'));
    CHECK(out.manualIp.empty());
    CHECK(!warnings.empty());
    CHECK(out.cameraFps == 30);          // clamped
    CHECK(!out.window.valid);            // too small to be a real window
    CHECK(out.headsets.size() == 1);     // the bad id is skipped
    CHECK(out.headsets[0].ips.size() == kMaxIpsPerHeadset);
    CHECK(out.headsets[0].ips[1] == "1.2.3.5");

    Settings keep = fullSettings();
    CHECK(!settingsFromJson("[1,2]", keep, warnings, error));
    CHECK(!settingsFromJson("{oops", keep, warnings, error));
    CHECK(keep == fullSettings());  // untouched on failure
}

void testLoadSave(const fs::path& dir) {
    const fs::path file = dir / "settings.json";
    const std::string path = file.string();
    std::vector<std::string> warnings;
    Settings s;
    bool created = false;

    // Missing file: defaults + a new id
    CHECK(loadSettings(path, s, created, warnings) == LoadResult::Missing);
    CHECK(created);
    link::Id128 parsed {};
    CHECK(link::idFromHex(s.pcId, parsed));
    const std::string firstId = s.pcId;
    Settings s2;
    bool created2 = false;
    CHECK(loadSettings(path, s2, created2, warnings) == LoadResult::Missing);
    CHECK(s2.pcId != firstId);  // random each time until saved

    // Save, then it loads back identical and keeps its id
    std::string error;
    Settings full = fullSettings();
    CHECK(saveSettings(path, full, error));
    CHECK(error.empty());
    CHECK(fs::exists(file));
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    CHECK(names.size() == 1);  // no temp file left behind
    Settings loaded;
    CHECK(loadSettings(path, loaded, created, warnings) == LoadResult::Loaded);
    CHECK(!created);
    CHECK(loaded == full);

    // Overwrite
    full.manualIp = "10.0.0.7";
    full.onTop = false;
    CHECK(saveSettings(path, full, error));
    CHECK(loadSettings(path, loaded, created, warnings) == LoadResult::Loaded);
    CHECK(loaded == full);
    names.clear();
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    CHECK(names.size() == 1);

    // A file without a pc_id: loads, and a new id is made
    writeAll(file, "{\"manual_ip\": \"1.1.1.1\"}");
    CHECK(loadSettings(path, loaded, created, warnings) == LoadResult::Loaded);
    CHECK(created && loaded.pcId.size() == 32 && loaded.manualIp == "1.1.1.1");

    // Broken: moved aside, defaults + a new id
    const std::string garbage = "{ this is not json";
    writeAll(file, garbage);
    warnings.clear();
    CHECK(loadSettings(path, loaded, created, warnings) == LoadResult::Broken);
    CHECK(created);
    CHECK(link::idFromHex(loaded.pcId, parsed));
    CHECK(loaded.headsets.empty() && loaded.manualIp.empty());
    CHECK(!warnings.empty());
    CHECK(!fs::exists(file));
    CHECK(fs::exists(dir / "settings.json.broken"));
    CHECK(readAll(dir / "settings.json.broken") == garbage);
    // After saving, the next load is fine and the .broken file stays
    CHECK(saveSettings(path, loaded, error));
    Settings again;
    CHECK(loadSettings(path, again, created, warnings) == LoadResult::Loaded);
    CHECK(again == loaded);
    CHECK(fs::exists(dir / "settings.json.broken"));

    // A second broken file replaces the first .broken
    writeAll(file, "[]");
    CHECK(loadSettings(path, loaded, created, warnings) == LoadResult::Broken);
    CHECK(readAll(dir / "settings.json.broken") == "[]");
}

void testRemember() {
    Settings s;
    CHECK(rememberHeadset(s, hexId('1'), "A", {"1.1.1.1", "2.2.2.2"}));
    CHECK(s.headsets.size() == 1);
    CHECK(!rememberHeadset(s, hexId('1'), "A", {"1.1.1.1", "2.2.2.2"}));  // nothing new
    CHECK(rememberHeadset(s, hexId('2'), "B", {"3.3.3.3"}));
    CHECK(s.headsets[0].id == hexId('2'));  // newest first
    CHECK(rememberHeadset(s, hexId('1'), "A", {"2.2.2.2", "4.4.4.4"}));
    CHECK(s.headsets.size() == 2 && s.headsets[0].id == hexId('1'));
    // new IPs first, older ones after, deduped
    CHECK((s.headsets[0].ips == std::vector<std::string> {"2.2.2.2", "4.4.4.4", "1.1.1.1"}));
    // IPs capped, invalid ones dropped
    CHECK(rememberHeadset(s, hexId('1'), "A", {"bad", "5.5.5.5", "6.6.6.6", "7.7.7.7", "8.8.8.8"}));
    CHECK(s.headsets[0].ips.size() == kMaxIpsPerHeadset);
    CHECK(s.headsets[0].ips[0] == "5.5.5.5");
    // Headsets capped, oldest dropped
    for (int i = 0; i < 12; ++i) rememberHeadset(s, hexId(static_cast<char>('a' + i)), "n", {"9.9.9.9"});
    CHECK(s.headsets.size() == kMaxRememberedHeadsets);
    CHECK(s.headsets[0].id == hexId('l'));
    // rememberedIps flattens without repeats
    Settings t;
    rememberHeadset(t, hexId('1'), "A", {"1.1.1.1", "2.2.2.2"});
    rememberHeadset(t, hexId('2'), "B", {"2.2.2.2", "3.3.3.3"});
    CHECK((rememberedIps(t) == std::vector<std::string> {"2.2.2.2", "3.3.3.3", "1.1.1.1"}));
}

}  // namespace

int main() {
    testRoundTrip();
    testTolerance();
    testRemember();

    const fs::path dir = fs::temp_directory_path() / ("fph-pc-settings-test-" + std::to_string(std::rand()) + "-" +
                                                      std::to_string(static_cast<long long>(std::time(nullptr))));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    testLoadSave(dir);
    fs::remove_all(dir, ec);

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc settings: all checks passed\n");
    return 0;
}
