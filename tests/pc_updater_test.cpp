// pc/updater: version compare, release JSON, SHA256SUMS lookup, URL allowlist, check throttle, exe swap, sha256.
#include "updater.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
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

using namespace fph::pc::update;
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

void testCompareVersions() {
    CHECK(compareVersions("0.4.0", "0.1.0") == 1);
    CHECK(compareVersions("0.1.0", "0.4.0") == -1);
    CHECK(compareVersions("v0.4.0", "0.4.0") == 0);
    CHECK(compareVersions("V0.4.0", "0.1.0") == 1);
    CHECK(compareVersions("0.4.0-rc1", "0.4.0") == 0);
    CHECK(compareVersions("0.10.0", "0.9.9") == 1);
    CHECK(compareVersions("1.0.0", "0.99.99") == 1);
    CHECK(compareVersions("0.4.0", "0.4.0") == 0);
    CHECK(compareVersions("", "0.4.0") == 0);
    CHECK(compareVersions("abc", "0.4.0") == 0);
    CHECK(compareVersions("0.4.0", "abc") == 0);
    CHECK(compareVersions("1..2", "0.1.0") == 0);
}

const char* kReleaseJson = R"({
  "url": "https://api.github.com/repos/finaea/frame-perf-hud/releases/1",
  "html_url": "https://github.com/finaea/frame-perf-hud/releases/tag/v0.4.0",
  "tag_name": "v0.4.0",
  "name": "Frame Perf HUD 0.4.0",
  "draft": false,
  "prerelease": false,
  "assets": [
    {"name": "frame-perf-hud-0.4.0.tar.gz", "size": 1234,
     "browser_download_url": "https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/frame-perf-hud-0.4.0.tar.gz"},
    {"name": "FramePerfHud-0.4.0.exe", "size": 5678,
     "browser_download_url": "https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/FramePerfHud-0.4.0.exe"},
    {"name": "SHA256SUMS", "size": 200,
     "browser_download_url": "https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/SHA256SUMS"}
  ]
})";

void testParseRelease() {
    Release r;
    std::string err;
    CHECK(parseRelease(kReleaseJson, r, err));
    CHECK(r.tag == "v0.4.0");
    CHECK(r.version == "0.4.0");
    CHECK(r.page == "https://github.com/finaea/frame-perf-hud/releases/tag/v0.4.0");
    CHECK(!r.draft && !r.prerelease);
    CHECK(r.asset == "FramePerfHud-0.4.0.exe");
    CHECK(r.assetUrl == "https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/FramePerfHud-0.4.0.exe");
    CHECK(r.sumsUrl == "https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/SHA256SUMS");
    CHECK(assetName("0.4.0") == "FramePerfHud-0.4.0.exe");

    // no exe on the release
    const char* noExe = R"({"tag_name":"v0.4.0","assets":[
        {"name":"frame-perf-hud-0.4.0.tar.gz","browser_download_url":"https://github.com/a"},
        {"name":"SHA256SUMS","browser_download_url":"https://github.com/s"}]})";
    Release a;
    CHECK(parseRelease(noExe, a, err));
    CHECK(a.assetUrl.empty());
    CHECK(a.sumsUrl == "https://github.com/s");

    // no SHA256SUMS
    const char* noSums = R"({"tag_name":"v0.4.0","assets":[
        {"name":"FramePerfHud-0.4.0.exe","browser_download_url":"https://github.com/e"}]})";
    Release b;
    CHECK(parseRelease(noSums, b, err));
    CHECK(b.assetUrl == "https://github.com/e");
    CHECK(b.sumsUrl.empty());

    // flags
    Release c;
    CHECK(parseRelease(R"({"tag_name":"v0.5.0","draft":true,"prerelease":true})", c, err));
    CHECK(c.draft && c.prerelease);
    CHECK(c.assetUrl.empty() && c.sumsUrl.empty());

    // rejects
    Release d;
    CHECK(!parseRelease(R"({"name":"x","assets":[]})", d, err));
    CHECK(!parseRelease("not json at all", d, err));
    CHECK(!parseRelease("", d, err));
    CHECK(!parseRelease(R"({"tag_name":""})", d, err));
    CHECK(!parseRelease(R"({"tag_name":5})", d, err));
}

void testFindChecksum() {
    const std::string tarHash(64, 'a');
    const std::string exeHash = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    const std::string lowerExe = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::string hex;

    const std::string twoSpace = tarHash + "  frame-perf-hud-0.4.0.tar.gz\n" + exeHash + "  FramePerfHud-0.4.0.exe\n";
    CHECK(findChecksum(twoSpace, "FramePerfHud-0.4.0.exe", hex));
    CHECK(hex == lowerExe);
    CHECK(findChecksum(twoSpace, "frame-perf-hud-0.4.0.tar.gz", hex));
    CHECK(hex == tarHash);

    const std::string star = tarHash + " *frame-perf-hud-0.4.0.tar.gz\n" + lowerExe + " *FramePerfHud-0.4.0.exe";
    hex.clear();
    CHECK(findChecksum(star, "FramePerfHud-0.4.0.exe", hex));
    CHECK(hex == lowerExe);

    const std::string crlf = tarHash + "  frame-perf-hud-0.4.0.tar.gz\r\n" + exeHash + "  FramePerfHud-0.4.0.exe\r\n";
    hex.clear();
    CHECK(findChecksum(crlf, "FramePerfHud-0.4.0.exe", hex));
    CHECK(hex == lowerExe);

    // no line for the name
    CHECK(!findChecksum(twoSpace, "FramePerfHud-0.5.0.exe", hex));
    CHECK(!findChecksum("", "FramePerfHud-0.4.0.exe", hex));

    // malformed hash
    CHECK(!findChecksum(std::string(63, 'a') + "  FramePerfHud-0.4.0.exe\n", "FramePerfHud-0.4.0.exe", hex));
    CHECK(!findChecksum(std::string(63, 'a') + "g  FramePerfHud-0.4.0.exe\n", "FramePerfHud-0.4.0.exe", hex));
    CHECK(!findChecksum(std::string(65, 'a') + "  FramePerfHud-0.4.0.exe\n", "FramePerfHud-0.4.0.exe", hex));

    // a name that only ends with the wanted one must not match
    const std::string suffixed = exeHash + "  x-FramePerfHud-0.4.0.exe\n";
    CHECK(!findChecksum(suffixed, "FramePerfHud-0.4.0.exe", hex));
    // ...and a prefix of another name doesn't either
    CHECK(!findChecksum(exeHash + "  FramePerfHud-0.4.0.exe.sig\n", "FramePerfHud-0.4.0.exe", hex));
}

void testUrlAllowed() {
    CHECK(urlAllowed("https://github.com/finaea/frame-perf-hud/releases/download/v0.4.0/x.exe", false));
    CHECK(urlAllowed("https://api.github.com/repos/finaea/frame-perf-hud/releases/latest", false));
    CHECK(urlAllowed("https://objects.githubusercontent.com/abc?x=1", false));
    CHECK(urlAllowed("https://release-assets.githubusercontent.com/abc", false));
    CHECK(urlAllowed("https://codeload.github.com/finaea/frame-perf-hud/tar.gz/v0.4.0", false));

    CHECK(!urlAllowed("http://github.com/x", false));
    CHECK(!urlAllowed("https://evil.com/x", false));
    CHECK(!urlAllowed("https://github.com.evil.com/x", false));
    CHECK(!urlAllowed("https://githubusercontent.com/x", false));
    CHECK(!urlAllowed("https://evil.com/github.com", false));
    CHECK(!urlAllowed("https://github.com@evil.com/x", false));
    CHECK(!urlAllowed("ftp://github.com/x", false));
    CHECK(!urlAllowed("", false));

    CHECK(urlAllowed("http://127.0.0.1:8080/x", true));
    CHECK(urlAllowed("https://evil.com/x", true));
    CHECK(!urlAllowed("ftp://127.0.0.1/x", true));
}

void testCheckDue() {
    const int64_t now = 1800000000;
    const int64_t hour = 3600;
    CHECK(!checkDue(false, 0, 0, now));
    CHECK(!checkDue(false, now - 48 * hour, 0, now));
    CHECK(checkDue(true, 0, 0, now));
    CHECK(!checkDue(true, now - 23 * hour, 0, now));
    CHECK(checkDue(true, now - 24 * hour, 0, now));
    CHECK(checkDue(true, now - 30 * hour, 0, now));
    CHECK(checkDue(true, now + 5 * hour, 0, now));  // clock set back
    CHECK(!checkDue(true, now - 48 * hour, now - 30 * 60, now));
    CHECK(checkDue(true, now - 48 * hour, now - 61 * 60, now));
}

void testSwap(const fs::path& dir) {
    const fs::path exe = dir / "FramePerfHud.exe";
    const fs::path fresh = dir / "new.exe";
    const fs::path old = oldExePath(exe.string());
    CHECK(old == fs::path(exe.string() + ".old"));
    std::string err;

    writeAll(exe, "old");
    writeAll(fresh, "new");
    CHECK(swapInExe(exe.string(), fresh.string(), err));
    CHECK(readAll(exe) == "new");
    CHECK(readAll(old) == "old");
    CHECK(!fs::exists(fresh));

    // a leftover .old is replaced
    writeAll(fresh, "newer");
    CHECK(swapInExe(exe.string(), fresh.string(), err));
    CHECK(readAll(exe) == "newer");
    CHECK(readAll(old) == "new");

    CHECK(removeOldExe(exe.string()));
    CHECK(!fs::exists(old));
    CHECK(!removeOldExe(exe.string()));

    // missing new file: rolled back, no .old left behind
    writeAll(exe, "keep");
    err.clear();
    CHECK(!swapInExe(exe.string(), (dir / "nope.exe").string(), err));
    CHECK(!err.empty());
    CHECK(readAll(exe) == "keep");
    CHECK(!fs::exists(old));

    // undoSwap: Windows refused the new exe, so the old one goes back
    writeAll(exe, "old");
    writeAll(fresh, "rejected");
    CHECK(swapInExe(exe.string(), fresh.string(), err));
    CHECK(readAll(exe) == "rejected");
    err.clear();
    CHECK(undoSwap(exe.string(), err));
    CHECK(readAll(exe) == "old");
    CHECK(!fs::exists(old));
    CHECK(!fs::exists(fresh));

    // undoSwap with no .old: refused, exe untouched
    err.clear();
    CHECK(!undoSwap(exe.string(), err));
    CHECK(!err.empty());
    CHECK(readAll(exe) == "old");

    // the state is clean after an undo: a fresh swap works again
    writeAll(fresh, "retry");
    err.clear();
    CHECK(swapInExe(exe.string(), fresh.string(), err));
    CHECK(readAll(exe) == "retry");
    CHECK(readAll(old) == "old");
    CHECK(removeOldExe(exe.string()));

    CHECK(folderWritable(dir.string()));
    CHECK(!folderWritable((dir / "does-not-exist").string()));
}

void testSha256(const fs::path& dir) {
    const fs::path f = dir / "abc.bin";
    writeAll(f, "abc");
    std::string err;
    CHECK(sha256File(f.string(), err) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(err.empty());
    err.clear();
    CHECK(sha256File((dir / "missing.bin").string(), err).empty());
    CHECK(!err.empty());

    const fs::path e = dir / "empty.bin";
    writeAll(e, "");
    CHECK(sha256File(e.string(), err) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void testDescribe() {
    View v;
    v.current = "0.1.0";
    v.latest = "0.4.0";
    v.error = "boom";
    for (State s : {State::Idle, State::Checking, State::UpToDate, State::Available, State::Downloading, State::Ready,
                    State::CheckFailed, State::InstallFailed}) {
        v.state = s;
        CHECK(!describe(v).empty());
    }
}

}  // namespace

int main() {
    testCompareVersions();
    testParseRelease();
    testFindChecksum();
    testUrlAllowed();
    testCheckDue();
    testDescribe();

    const fs::path dir = fs::temp_directory_path() / ("fph-pc-updater-test-" + std::to_string(std::rand()) + "-" +
                                                      std::to_string(static_cast<long long>(std::time(nullptr))));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    testSwap(dir);
    testSha256(dir);
    fs::remove_all(dir, ec);

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc updater: all checks passed\n");
    return 0;
}
