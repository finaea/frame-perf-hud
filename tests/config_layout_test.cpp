// headset config + layout: missing / broken config.json, a broken "layout" key, and the atomic save under kill -9.
// POSIX only (fork, SIGKILL). Works in a temp dir; never touches ~/.config.
#include "atomic_file.h"
#include "config.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
using W = WidgetType;

std::string tempDir;

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

/** A layout that differs per n, so a torn or stale file is noticeable. */
Layout layoutFor(int n) {
    Layout layout = defaultLayout();
    layout.version = static_cast<uint32_t>(n);
    for (WidgetInstance& w : layout.columns[2].widgets) {
        if (w.type == W::CpuItems) w.maxItems = static_cast<uint32_t>(1 + n % 50);
    }
    return layout;
}

// ---- 2. config.json missing / unparsable / broken layout ----

void testLoad() {
    std::vector<std::string> warnings;
    std::string error;

    Config c;
    c.alpha = 0.5;  // would be reset to the default by a "missing file" load
    CHECK(loadConfig(tempDir + "/nope.json", c, warnings, error));
    CHECK(c.alpha == Config().alpha);
    CHECK(c.layout.sameWidgets(defaultLayout()));
    CHECK(!c.layoutInFile);

    // unparsable: false, and the caller's config is left alone
    writeFile(tempDir + "/garbage.json", "{ \"alpha\": 0.3, ");
    Config keep;
    keep.alpha = 0.7;
    error.clear();
    CHECK(!loadConfig(tempDir + "/garbage.json", keep, warnings, error));
    CHECK(!error.empty());
    CHECK(keep.alpha == 0.7);

    // a layout that isn't a layout: rest of the config kept, default layout, a warning
    writeFile(tempDir + "/badlayout.json", R"({ "alpha": 0.35, "layout": { "columns": 5 } })");
    Config bad;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/badlayout.json", bad, warnings, error));
    CHECK(bad.alpha == 0.35);
    CHECK(bad.layout.sameWidgets(defaultLayout()));
    CHECK(!bad.layoutInFile);
    CHECK(!warnings.empty());

    // a good custom layout in the file is used
    writeFile(tempDir + "/goodlayout.json",
              R"({ "layout": { "version": 5, "columns": [[{"type":"cpu_items","max_items":3}],[{"type":"temp"}]] } })");
    Config good;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/goodlayout.json", good, warnings, error));
    CHECK(good.layoutInFile);
    CHECK(good.layout.version == 5);
    CHECK(good.layout.columns.size() == 2);
    CHECK(good.layout.columns[0].widgets[0].type == W::CpuItems && good.layout.columns[0].widgets[0].maxItems == 3);

    // save + load round trip keeps the layout
    Config saved;
    saved.layout = layoutFor(7);
    CHECK(saveConfig(tempDir + "/sub/dir/roundtrip.json", saved, error));
    Config loaded;
    warnings.clear();
    CHECK(loadConfig(tempDir + "/sub/dir/roundtrip.json", loaded, warnings, error));
    CHECK(loaded.layoutInFile);
    CHECK(loaded.layout.version == 7);
    CHECK(loaded.layout.sameWidgets(saved.layout));
}

// ---- 3. Atomic save under kill -9 ----

/** The child: save over and over, forever (until the parent kills it). */
[[noreturn]] void saveLoop(const std::string& path) {
    Config config;
    config.fontPath = std::string(200 * 1024, 'x');  // a big file, so a kill lands mid-write now and then
    std::string error;
    for (int n = 1;; ++n) {
        config.layout = layoutFor(n);
        saveConfig(path, config, error);
    }
}

void testKillDuringSave() {
    const std::string path = tempDir + "/kill/config.json";
    std::string error;
    Config first;
    first.fontPath = std::string(200 * 1024, 'x');
    first.layout = layoutFor(1);
    CHECK(saveConfig(path, first, error));

    const int kKills = 200;
    int bad = 0;
    srand(12345);
    for (int k = 0; k < kKills; ++k) {
        const pid_t pid = fork();
        if (pid < 0) {
            CHECK(false && "fork failed");
            return;
        }
        if (pid == 0) saveLoop(path);
        struct timespec delay = {0, (1 + rand() % 25) * 1000000L};  // 1-25 ms
        nanosleep(&delay, nullptr);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);

        Config loaded;
        std::vector<std::string> warnings;
        std::string loadError;
        const bool ok = loadConfig(path, loaded, warnings, loadError);
        const bool intact = ok && loaded.layoutInFile && loaded.layout.columns.size() == 3 &&
                            loaded.layout.sameWidgets(layoutFor(static_cast<int>(loaded.layout.version))) &&
                            loaded.fontPath.size() == 200 * 1024;
        if (!intact) {
            ++bad;
            std::printf("kill %d: config.json not intact (load ok=%d, error=%s)\n", k, ok ? 1 : 0, loadError.c_str());
        }
    }
    CHECK(bad == 0);
    std::printf("kill -9 test: %d kills, %d broken files\n", kKills, bad);
}

}  // namespace

// ---- Review fix: an unreadable config is kept as .bad, not overwritten ----

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void testBrokenConfigKeptAsBad() {
    const std::string path = tempDir + "/broken/config.json";
    makeDirectories(tempDir + "/broken");
    const std::string original = "{ \"alpha\": 0.4, oops this isn't JSON\n";
    writeFile(path, original);
    writeFile(path + ".bad", "an older bad file");

    ConfigWatcher watcher(path);
    Config config = watcher.loadInitial();
    CHECK(watcher.lastLoadFailed());
    CHECK(readFile(path) == original);  // loading alone never touches it

    // What a dashboard action / second-launch toggle does: change something, save through the watcher
    config.visible = false;
    std::string error;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);  // the user's text, byte-identical (older .bad replaced)
    std::vector<std::string> warnings;
    Config reloaded;
    CHECK(loadConfig(path, reloaded, warnings, error));
    CHECK(!reloaded.visible && reloaded.layoutInFile);
    CHECK(!watcher.lastLoadFailed());

    // The next save just saves (the .bad copy stays as it was)
    config.alpha = 0.7;
    CHECK(watcher.save(config, error));
    CHECK(readFile(path + ".bad") == original);
}

int main() {
    char pattern[] = "/tmp/fph-t04-tests/cfg-XXXXXX";
    makeDirectories("/tmp/fph-t04-tests");
    const char* dir = mkdtemp(pattern);
    if (dir == nullptr) {
        std::printf("mkdtemp failed\n");
        return 1;
    }
    tempDir = dir;
    testLoad();
    testKillDuringSave();
    testBrokenConfigKeptAsBad();
    const std::string cleanup = "rm -rf '" + tempDir + "'";
    if (std::system(cleanup.c_str()) != 0) std::printf("(could not remove %s)\n", tempDir.c_str());
    if (failures == 0) std::printf("config-layout-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
