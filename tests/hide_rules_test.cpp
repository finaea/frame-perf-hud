// R2 Hide system: glob + rule matching, the rules file (seed, reload, malformed, stat throttle), the default rules
// against a real process list captured on the Frame, and the scanner dropping hidden pids. Linux only.
#include "hide_rules.h"
#include "process_scan.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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

class TempDir {
public:
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base != nullptr ? base : "/tmp") + "/fph-hide-XXXXXX";
        if (::mkdtemp(pattern.data()) == nullptr) {
            std::printf("FAIL: mkdtemp\n");
            std::exit(2);
        }
        root_ = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& root() const { return root_; }
    void write(const std::string& relative, const std::string& content) const {
        const fs::path path = fs::path(root_) / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

private:
    std::string root_;
};

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** Moves the file's mtime forward so a rewrite always looks changed, whatever the filesystem's granularity. */
void touchForward(const std::string& path, long seconds) {
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = ::time(nullptr) + seconds;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, path.c_str(), times, 0);
}

void testGlob() {
    CHECK(globMatch("vrserver", "vrserver"));
    CHECK(!globMatch("vrserver", "vrserver2"));
    CHECK(!globMatch("vrserver", "vrserve"));
    CHECK(globMatch("steam-runtime-*", "steam-runtime-launcher"));
    CHECK(globMatch("steam-runtime-*", "steam-runtime-"));  // trailing * matches nothing too
    CHECK(!globMatch("steam-runtime-*", "steam-runtim"));
    CHECK(globMatch("vr?erver", "vrserver"));
    CHECK(!globMatch("vr?erver", "vrerver"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("*", "anything/at/all"));
    CHECK(globMatch("/system.slice/*", "/system.slice/a/b/c.service"));  // * spans '/'
    CHECK(globMatch("*/session.slice/ibus-*", "/user.slice/u/session.slice/ibus-x.service"));
    CHECK(!globMatch("/system.slice/*", "/user.slice/system.slice/x"));
    CHECK(globMatch("a*b*c", "aXXbYYc"));
    CHECK(!globMatch("a*b*c", "aXXbYY"));
}

void testRuleMatches() {
    const HideRule byName {"vrserver", "", ""};
    CHECK(hideRuleMatches(byName, "vrserver", "", ""));            // comm
    CHECK(hideRuleMatches(byName, "VrThread", "vrserver", ""));    // exe basename
    CHECK(!hideRuleMatches(byName, "other", "other", "/system.slice/x"));
    const HideRule byCgroup {"", "/system.slice/*", ""};
    CHECK(hideRuleMatches(byCgroup, "x", "", "/system.slice/a.service"));
    CHECK(!hideRuleMatches(byCgroup, "x", "", "/user.slice/a"));
    CHECK(!hideRuleMatches(byCgroup, "x", "", ""));  // unknown cgroup never matches a cgroup rule
    const HideRule both {"sshd", "/user.slice/*", ""};
    CHECK(hideRuleMatches(both, "sshd", "", "/user.slice/s.scope"));
    CHECK(!hideRuleMatches(both, "sshd", "", "/system.slice/sshd.service"));
    CHECK(!hideRuleMatches(both, "bash", "", "/user.slice/s.scope"));
    CHECK(!hideRuleMatches(HideRule {}, "x", "x", "/x"));  // a rule with neither matches nothing

    CHECK(cgroupV2Path("0::/user.slice/a.scope\n") == "/user.slice/a.scope");
    CHECK(cgroupV2Path("12:cpu:/x\n0::/y.service\n") == "/y.service");
    CHECK(cgroupV2Path("12:cpu:/x\n").empty());
}

void testSeedAndParse() {
    TempDir dir;
    const std::string path = dir.root() + "/sub/hide_rules.json";
    HideRules rules(path, 0);
    std::string error;
    CHECK(rules.seedIfMissing(error));
    std::vector<HideRule> parsed;
    CHECK(parseHideRules(slurp(path), parsed, error));
    CHECK(parsed.size() == defaultHideRules().size());
    bool same = parsed.size() == defaultHideRules().size();
    for (size_t i = 0; same && i < parsed.size(); ++i) {
        same = parsed[i].name == defaultHideRules()[i].name && parsed[i].cgroup == defaultHideRules()[i].cgroup;
    }
    CHECK(same);

    // an existing file, even a broken one, is left alone
    dir.write("broken.json", "{ not json");
    HideRules broken(dir.root() + "/broken.json", 0);
    CHECK(broken.seedIfMissing(error));
    CHECK(slurp(dir.root() + "/broken.json") == "{ not json");

    // parse failures
    CHECK(!parseHideRules("{\"hide\": 5}", parsed, error));
    CHECK(!parseHideRules("{\"hide\": [{\"note\": \"x\"}]}", parsed, error));  // neither name nor cgroup
    CHECK(!parseHideRules("{\"hide\": [{\"name\": 3}]}", parsed, error));
}

void testRefresh() {
    TempDir dir;
    const std::string path = dir.root() + "/rules.json";
    dir.write("rules.json", "{\"hide\": [{\"name\": \"alpha\"}]}");
    HideRules rules(path, 0);
    CHECK(rules.refresh(1));
    CHECK(rules.rules().size() == 1 && rules.hides("alpha", "", ""));
    CHECK(!rules.hides("beta", "", ""));
    const uint64_t gen = rules.generation();
    CHECK(!rules.refresh(2));  // unchanged
    CHECK(rules.generation() == gen);

    // an edit (new content + mtime forward) is picked up
    dir.write("rules.json", "{\"hide\": [{\"name\": \"beta\"}, {\"cgroup\": \"/x/*\"}]}");
    touchForward(path, 100);
    CHECK(rules.refresh(3));
    CHECK(rules.rules().size() == 2 && rules.hides("beta", "", "") && !rules.hides("alpha", "", ""));
    CHECK(rules.generation() == gen + 1);

    // malformed: last good rules and generation stay
    dir.write("rules.json", "{\"hide\": [");
    touchForward(path, 200);
    CHECK(!rules.refresh(4));
    CHECK(rules.rules().size() == 2 && rules.hides("beta", "", ""));
    CHECK(rules.generation() == gen + 1);

    // deleted: rules in use are kept
    std::remove(path.c_str());
    CHECK(!rules.refresh(5));
    CHECK(rules.hides("beta", "", ""));
}

void testRefreshThrottle() {
    TempDir dir;
    const std::string path = dir.root() + "/rules.json";
    dir.write("rules.json", "{\"hide\": [{\"name\": \"alpha\"}]}");
    HideRules rules(path, 5.0);
    CHECK(rules.refresh(100));  // the first look always happens
    dir.write("rules.json", "{\"hide\": [{\"name\": \"beta\"}]}");
    touchForward(path, 100);
    CHECK(!rules.refresh(102));  // within checkSeconds: not even a stat
    CHECK(rules.hides("alpha", "", ""));
    CHECK(rules.refresh(105.5));
    CHECK(rules.hides("beta", "", "") && !rules.hides("alpha", "", ""));
}

// ---- the default rules against the process list captured on the Frame (2026-10-17) ----

struct Captured {
    std::string comm, exe, cgroup;
};

std::vector<Captured> loadCaptured(const std::string& file) {
    std::vector<Captured> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t a = line.find('|');
        const size_t b = line.find('|', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        out.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
    }
    return out;
}

void testDefaultsAgainstFrameList(const std::string& fixtureDir) {
    const std::vector<Captured> procs = loadCaptured(fixtureDir + "/frame_processes_2026-10-17.txt");
    CHECK(procs.size() > 100);
    TempDir dir;
    HideRules rules(dir.root() + "/hide_rules.json", 0);  // no file: the built-in defaults
    std::set<std::string> visible;
    for (const Captured& p : procs) {
        if (!rules.hides(p.comm, p.exe, p.cgroup)) visible.insert(p.comm);
    }
    const std::set<std::string> want = {"bash",     "cmake",        "make",           "grep",
                                        "sort",     "journalctl",   "systemctl",      "eyecam-rec",
                                        "frame-perf-hud", "frameeyeosc-pan", "frameeyeosc"};
    CHECK(visible == want);
    if (visible != want) {
        for (const std::string& c : visible) {
            if (want.count(c) == 0) std::printf("  unexpectedly visible: %s\n", c.c_str());
        }
        for (const std::string& c : want) {
            if (visible.count(c) == 0) std::printf("  unexpectedly hidden: %s\n", c.c_str());
        }
    }

    // a game inside steam.service stays visible; SteamVR's own pieces don't
    const std::string steamService =
        "/user.slice/user-1000.slice/user@1000.service/session.slice/steam.service";
    CHECK(!rules.hides("Idol Showdown", "IdolShowdown.x86_64", steamService));
    CHECK(rules.hides("vrserver", "vrserver", steamService));
}

// ---- the scanner drops hidden pids before ranking ----

void writeProc(const TempDir& f, int pid, const std::string& comm, long rssPages, const std::string& cgroup,
               unsigned long ticks = 0) {
    std::string s = std::to_string(pid) + " (" + comm + ") S 1 1 1 0 -1 4194304 10 0 0 0 " + std::to_string(ticks) +
                    " 0 0 0 20 0 1 0 " + std::to_string(5000 + pid) + " 1000000 " + std::to_string(rssPages) +
                    " 18446744073709551615\n";
    f.write(std::to_string(pid) + "/stat", s);
    f.write(std::to_string(pid) + "/cgroup", "0::" + cgroup + "\n");
}

void testScannerHidesSystem() {
    TempDir proc;
    TempDir rulesDir;
    // 4096 B pages: 51200 pages = 200 MiB
    writeProc(proc, 100, "weirdsvc", 51200, "/system.slice/weird.service");      // biggest, hidden by cgroup
    writeProc(proc, 200, "appa", 12800, "/user.slice/user-1000.slice/session-1.scope");  // 50 MiB
    writeProc(proc, 300, "appb", 10240, "/user.slice/user-1000.slice/session-1.scope");  // 40 MiB
    writeProc(proc, 400, "appc", 7680, "/user.slice/user-1000.slice/session-1.scope");   // 30 MiB
    writeProc(proc, 500, "vrserver", 25600, "/user.slice/user-1000.slice/session-2.scope");  // hidden by name

    HideRules rules(rulesDir.root() + "/hide_rules.json", 0);
    std::string error;
    CHECK(rules.seedIfMissing(error));
    ProcessScanner scanner(proc.root(), 100, 4096);
    scanner.setHideRules(&rules);

    ProcessListOptions opts;
    opts.ramMax = 2;
    opts.cpuMax = 2;

    // without the option nothing is dropped
    const ProcessLists& plain = scanner.scan(10.0, opts);
    CHECK(plain.byRam.size() == 2);
    CHECK(plain.byRam[0].name == "weirdsvc" && plain.byRam[1].name == "vrserver");

    // with it, the hidden pids are gone and the list still fills its rows from the rest
    opts.ramHideSystem = true;
    const ProcessLists& hidden = scanner.scan(11.0, opts);
    CHECK(hidden.byRam.size() == 2);
    CHECK(hidden.byRam.size() == 2 && hidden.byRam[0].name == "appa" && hidden.byRam[1].name == "appb");
    CHECK(hidden.scanned == 5);

    // the CPU list has its own switch: ramHideSystem alone leaves it whole
    writeProc(proc, 100, "weirdsvc", 51200, "/system.slice/weird.service", 500);
    writeProc(proc, 200, "appa", 12800, "/user.slice/user-1000.slice/session-1.scope", 100);
    const ProcessLists& cpu = scanner.scan(12.0, opts);
    CHECK(cpu.cpuValid && !cpu.byCpu.empty() && cpu.byCpu[0].name == "weirdsvc");
    opts.cpuHideSystem = true;
    opts.ramHideSystem = false;
    writeProc(proc, 100, "weirdsvc", 51200, "/system.slice/weird.service", 1000);
    writeProc(proc, 200, "appa", 12800, "/user.slice/user-1000.slice/session-1.scope", 200);
    const ProcessLists& cpuHidden = scanner.scan(13.0, opts);
    CHECK(cpuHidden.cpuValid && !cpuHidden.byCpu.empty() && cpuHidden.byCpu[0].name == "appa");
    CHECK(cpuHidden.byRam[0].name == "weirdsvc");  // RAM list not hiding
}

// A hidden launcher that execs the game keeps its pid and starttime: the game must still show (review must-fix)
void testExecKeepsPidButNotVerdict() {
    TempDir proc;
    TempDir rulesDir;
    const std::string steam = "/user.slice/user-1000.slice/user@1000.service/session.slice/steam.service";
    writeProc(proc, 600, "steam-launch-wrapper", 2560, steam);
    HideRules rules(rulesDir.root() + "/hide_rules.json", 0);
    std::string error;
    CHECK(rules.seedIfMissing(error));
    ProcessScanner scanner(proc.root(), 100, 4096);
    scanner.setHideRules(&rules);
    ProcessListOptions opts;
    opts.ramMax = 5;
    opts.ramHideSystem = true;
    CHECK(scanner.scan(10.0, opts).byRam.empty());  // the launcher is hidden (and now cached)
    writeProc(proc, 600, "IdolShowdown", 51200, steam);  // exec: same pid, same starttime, new comm
    const ProcessLists& after = scanner.scan(11.0, opts);
    CHECK(after.byRam.size() == 1 && after.byRam[0].name == "IdolShowdown");
}

}  // namespace

void testEscapedFieldsRoundTrip() {
    const std::string nasty = std::string("\t\n\x01") + "\"q\"\\";
    HideRule rule;
    rule.name = "n" + nasty;
    rule.cgroup = "/c" + nasty;
    rule.note = "note" + nasty;
    const std::string json = hideRulesToJson({rule});
    std::vector<HideRule> parsed;
    std::string error;
    CHECK(parseHideRules(json, parsed, error));
    CHECK(parsed.size() == 1);
    if (parsed.size() == 1) {
        CHECK(parsed[0].name == rule.name);
        CHECK(parsed[0].cgroup == rule.cgroup);
        CHECK(parsed[0].note == rule.note);
    }
}

int main(int argc, char** argv) {
    const std::string fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    testGlob();
    testRuleMatches();
    testSeedAndParse();
    testEscapedFieldsRoundTrip();
    testRefresh();
    testRefreshThrottle();
    testDefaultsAgainstFrameList(fixtures);
    testScannerHidesSystem();
    testExecKeepsPidButNotVerdict();
    if (failures == 0) std::printf("hide-rules-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
