#include "updater.h"

#include "atomic_file.h"  // makeDirectories
#include "json.h"
#include "log.h"
#include "win_text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

namespace fph {
namespace pc {
namespace update {

namespace {

/** The numeric x.y.z core of a version (vendor/frame-updater/cpp/update_check.cpp: parseVersion, same rules). */
bool parseVersion(const std::string& text, long parts[3]) {
    std::string core = text;
    if (!core.empty() && (core[0] == 'v' || core[0] == 'V')) core.erase(0, 1);
    const size_t suffix = core.find_first_of("-+");
    if (suffix != std::string::npos) core.erase(suffix);
    parts[0] = parts[1] = parts[2] = 0;
    int index = 0;
    size_t digits = 0;
    for (const char c : core) {
        if (c == '.') {
            if (digits == 0 || ++index > 2) return false;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 9) return false;
            parts[index] = parts[index] * 10 + (c - '0');
        } else {
            return false;
        }
    }
    return digits > 0;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int64_t unixNow() {
    return static_cast<int64_t>(std::time(nullptr));
}

/** One GET. The body goes to `body` or, if `file` isn't empty, to that file. */
struct HttpResult {
    int status = 0;
    std::string error;  ///< "" if the request completed (any status)
};

HttpResult httpGet(const std::string& url, const std::string& version, bool githubApi, bool insecure, std::string* body,
                   const std::string& file, uint64_t maxBytes) {
    HttpResult r;
    if (!urlAllowed(url, insecure)) {
        r.error = "refusing to contact " + url;
        return r;
    }
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    parts.dwExtraInfoLength = 1;  // the query stays on the path
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        r.error = "bad URL " + url;
        return r;
    }
    const std::wstring target = std::wstring(path, parts.dwUrlPathLength) +
                                (parts.lpszExtraInfo ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : L"");
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring agent = widen("frame-perf-hud/" + version);  // GitHub's API wants a User-Agent
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    HANDLE out = INVALID_HANDLE_VALUE;
    const auto fail = [&](const std::string& what) {
        char text[64];
        std::snprintf(text, sizeof(text), " (error %lu)", GetLastError());
        r.error = what + text;
    };
    do {
        if (session == nullptr) { fail("WinHttpOpen"); break; }
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
        connect = WinHttpConnect(session, host, parts.nPort, 0);
        if (connect == nullptr) { fail("can't reach " + narrow(host)); break; }
        request = WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) { fail("WinHttpOpenRequest"); break; }
        const wchar_t* headers = githubApi ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                           : WINHTTP_NO_ADDITIONAL_HEADERS;
        if (!WinHttpSendRequest(request, headers, githubApi ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            fail("can't reach " + narrow(host));
            break;
        }
        // Redirects were followed (https -> http never is): the answer must still come from an allowed place
        DWORD urlBytes = 0;
        WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &urlBytes);
        std::wstring finalUrl(urlBytes / sizeof(wchar_t) + 1, L'\0');
        if (urlBytes > 0 && WinHttpQueryOption(request, WINHTTP_OPTION_URL, &finalUrl[0], &urlBytes)) {
            finalUrl.resize(wcslen(finalUrl.c_str()));
            if (!urlAllowed(narrow(finalUrl), insecure)) {
                r.error = "redirected to " + narrow(finalUrl);
                break;
            }
        }
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        if (status != 200) break;  // the caller words it
        if (!file.empty()) {
            out = CreateFileW(widen(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
            if (out == INVALID_HANDLE_VALUE) { fail("can't write " + file); break; }
        }
        uint64_t total = 0;
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
                fail("the download broke off");
                break;
            }
            if (got == 0) break;
            total += got;
            if (total > maxBytes) {
                r.error = "the answer is too big";
                break;
            }
            if (out != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (!WriteFile(out, buffer.data(), got, &written, nullptr) || written != got) {
                    fail("can't write " + file);
                    break;
                }
            } else if (body != nullptr) {
                body->append(buffer.data(), got);
            }
        }
    } while (false);
    if (out != INVALID_HANDLE_VALUE) {
        if (!r.error.empty() || r.status != 200 || !FlushFileBuffers(out)) {
            CloseHandle(out);
            DeleteFileW(widen(file).c_str());
            if (r.error.empty() && r.status == 200) r.error = "can't write " + file;
        } else {
            CloseHandle(out);
        }
    }
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connect != nullptr) WinHttpCloseHandle(connect);
    if (session != nullptr) WinHttpCloseHandle(session);
    return r;
}

std::string statusError(int status) {
    if (status == 404) return "no published release";
    if (status == 403 || status == 429) return "GitHub refused the request (probably the hourly limit)";
    return "GitHub answered HTTP " + std::to_string(status);
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    long x[3];
    long y[3];
    if (!parseVersion(a, x) || !parseVersion(b, y)) return 0;
    for (int i = 0; i < 3; ++i) {
        if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    }
    return 0;
}

std::string assetName(const std::string& version) {
    return "FramePerfHud-" + version + ".exe";
}

bool parseRelease(const std::string& json, Release& out, std::string& error) {
    JsonValue root;
    if (!parseJson(json, root, error)) return false;
    const JsonValue* tag = root.get("tag_name");
    if (!root.isObject() || tag == nullptr || !tag->isString() || tag->text.empty()) {
        error = "GitHub's answer has no tag_name";
        return false;
    }
    Release r;
    r.tag = tag->text;
    r.version = (r.tag[0] == 'v' || r.tag[0] == 'V') ? r.tag.substr(1) : r.tag;
    if (const JsonValue* page = root.get("html_url"); page != nullptr && page->isString()) r.page = page->text;
    if (const JsonValue* d = root.get("draft"); d != nullptr && d->isBool()) r.draft = d->boolean;
    if (const JsonValue* p = root.get("prerelease"); p != nullptr && p->isBool()) r.prerelease = p->boolean;
    r.asset = assetName(r.version);
    if (const JsonValue* assets = root.get("assets"); assets != nullptr && assets->type == JsonValue::Type::Array) {
        for (const JsonValue& item : assets->items) {
            const JsonValue* name = item.get("name");
            const JsonValue* url = item.get("browser_download_url");
            if (name == nullptr || url == nullptr || !name->isString() || !url->isString()) continue;
            if (name->text == r.asset) r.assetUrl = url->text;
            if (name->text == "SHA256SUMS") r.sumsUrl = url->text;
        }
    }
    out = r;
    return true;
}

bool findChecksum(const std::string& sums, const std::string& name, std::string& hex) {
    size_t start = 0;
    while (start < sums.size()) {
        size_t end = sums.find('\n', start);
        if (end == std::string::npos) end = sums.size();
        std::string line = sums.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t space = line.find_first_of(" \t");
        if (space == std::string::npos) continue;
        const std::string sum = line.substr(0, space);
        size_t at = line.find_first_not_of(" \t", space);
        if (at == std::string::npos) continue;
        if (line[at] == '*') ++at;  // binary-mode marker
        if (line.substr(at) != name) continue;
        // First matching line decides, like frame-update.sh's awk ... exit
        if (sum.size() != 64 || sum.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
        hex = lower(sum);
        return true;
    }
    return false;
}

bool urlAllowed(const std::string& url, bool insecure) {
    const bool https = url.rfind("https://", 0) == 0;
    if (insecure) return https || url.rfind("http://", 0) == 0;
    if (!https) return false;
    std::string host = url.substr(8);
    host = host.substr(0, host.find_first_of("/?#"));
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos) return false;
    if (host == "github.com" || host == "api.github.com" || host == "codeload.github.com") return true;
    const std::string suffix = ".githubusercontent.com";
    return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool checkDue(bool enabled, int64_t lastCheckedAt, int64_t lastFailedAt, int64_t now) {
    if (!enabled) return false;
    if (lastFailedAt > 0 && now >= lastFailedAt && now - lastFailedAt < kRetryAfterFailSec) return false;
    // A clock set back (checkedAt in the future) checks again rather than waiting for it
    return lastCheckedAt <= 0 || now < lastCheckedAt || now - lastCheckedAt >= kCheckIntervalSec;
}

std::string oldExePath(const std::string& exe) {
    return exe + ".old";
}

bool swapInExe(const std::string& exe, const std::string& newFile, std::string& error) {
    const std::wstring current = widen(exe);
    const std::wstring old = widen(oldExePath(exe));
    if (!DeleteFileW(old.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = "can't delete the previous " + oldExePath(exe) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!MoveFileExW(current.c_str(), old.c_str(), 0)) {
        error = "can't rename " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!MoveFileExW(widen(newFile).c_str(), current.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        error = "can't put the new exe in place (error " + std::to_string(GetLastError()) + ")";
        MoveFileExW(old.c_str(), current.c_str(), 0);  // undo: the old exe keeps its name
        return false;
    }
    return true;
}

bool undoSwap(const std::string& exe, std::string& error) {
    if (!MoveFileExW(widen(oldExePath(exe)).c_str(), widen(exe).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "can't put the old exe back (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool removeOldExe(const std::string& exe) {
    return DeleteFileW(widen(oldExePath(exe)).c_str()) != FALSE;
}

bool folderWritable(const std::string& dir) {
    const std::string probe = dir + "\\.fph-write-test-" + std::to_string(GetCurrentProcessId());
    HANDLE h = CreateFileW(widen(probe).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string sha256File(const std::string& path, std::string& error) {
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "can't read " + path;
        return "";
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<unsigned char> buffer(256 * 1024);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        ok = BCryptHashData(hash, buffer.data(), got, 0) >= 0;
    }
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (!ok) {
        error = "can't hash " + path;
        return "";
    }
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    for (unsigned char b : digest) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    return hex;
}

std::string describe(const View& v) {
    switch (v.state) {
        case State::Idle: return "Not checked yet (" + v.current + ")";
        case State::Checking: return "Checking for updates…";
        case State::UpToDate: return "Up to date (" + v.current + ")";
        case State::Available:
            return "Version " + v.latest + " is available" +
                   (v.installable ? "" : " (download it from the release page)");
        case State::Downloading: return "Downloading " + v.latest + "…";
        case State::Ready: return "Installing " + v.latest + "…";
        case State::CheckFailed: return "Couldn't check for updates: " + v.error;
        case State::InstallFailed:
            return std::string(v.changed ? "The update failed: " : "The update failed (nothing was changed): ") + v.error;
    }
    return "";
}

UpdaterConfig configFromEnvironment(const std::string& current, const std::string& downloadDir) {
    UpdaterConfig c;
    c.current = current;
    c.downloadDir = downloadDir;
    if (const char* api = std::getenv("FPH_UPDATE_API_URL"); api != nullptr && api[0] != '\0') c.apiBase = api;
    if (const char* insecure = std::getenv("FPH_UPDATE_INSECURE"); insecure != nullptr && insecure[0] == '1') {
        c.insecure = true;
    }
    return c;
}

// ---- the checker ----

struct Updater::Shared {
    std::mutex mutex;
    View view;
    uint64_t revision = 1;
    bool busy = false;
    bool checkedPending = false;
    int64_t failedAt = 0;  ///< an automatic check failed (Unix s)

    template <typename F>
    void update(F&& f) {
        std::lock_guard<std::mutex> lock(mutex);
        f(view);
        ++revision;
    }
};

Updater::Updater(UpdaterConfig config) : config_(std::move(config)), shared_(std::make_shared<Shared>()) {
    shared_->view.current = config_.current;
}

Updater::~Updater() = default;  // a running worker keeps its own reference to shared_

void Updater::restore(int64_t checkedAt, const std::string& latest, const std::string& page) {
    shared_->update([&](View& v) {
        v.checkedAt = checkedAt;
        if (latest.empty() || checkedAt <= 0) return;
        v.latest = latest;
        v.page = page;
        const bool newer = compareVersions(latest, v.current) > 0;
        v.state = newer ? State::Available : State::UpToDate;
        v.installable = newer;  // Install asks GitHub again and says so if the exe is missing
    });
}

void Updater::tick(bool enabled, int64_t now) {
    int64_t checkedAt = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->busy) return;
        checkedAt = shared_->view.checkedAt;
        lastFailedAt_ = shared_->failedAt;
    }
    if (checkDue(enabled, checkedAt, lastFailedAt_, now)) start(false);
}

void Updater::checkNow() {
    start(false);
}

void Updater::install() {
    start(true);
}

void Updater::installFailed(const std::string& error, bool changed) {
    shared_->update([&](View& v) {
        v.state = State::InstallFailed;
        v.error = error;
        v.changed = changed;
        v.readyPath.clear();
    });
}

View Updater::view() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->view;
}

uint64_t Updater::revision() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->revision;
}

bool Updater::takeChecked(int64_t& checkedAt, std::string& latest, std::string& page) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (!shared_->checkedPending) return false;
    shared_->checkedPending = false;
    checkedAt = shared_->view.checkedAt;
    latest = shared_->view.latest;
    page = shared_->view.page;
    return true;
}

void Updater::start(bool installJob) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->busy) return;
        shared_->busy = true;
        shared_->view.state = installJob ? State::Downloading : State::Checking;
        shared_->view.error.clear();
        shared_->view.changed = false;
        ++shared_->revision;
    }
    const UpdaterConfig config = config_;
    std::shared_ptr<Shared> shared = shared_;
    std::thread([config, shared, installJob]() {
        const auto finish = [&](State state, const std::string& error) {
            shared->update([&](View& v) {
                v.state = state;
                v.error = error;
            });
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->busy = false;
        };
        const std::string apiUrl = config.apiBase + "/repos/" + kRepo + "/releases/latest";
        std::string json;
        const HttpResult answer = httpGet(apiUrl, config.current, true, config.insecure, &json, "", 1 << 20);
        Release release;
        std::string error = answer.error;
        if (error.empty() && answer.status != 200) error = statusError(answer.status);
        if (error.empty() && !parseRelease(json, release, error)) error = "can't read GitHub's answer: " + error;
        if (!error.empty()) {
            logLine("[update] check failed: %s", error.c_str());
            if (!installJob) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->failedAt = unixNow();
            }
            finish(installJob ? State::InstallFailed : State::CheckFailed, error);
            return;
        }
        // A draft or prerelease as "latest" counts as no news (frame-update.sh does the same)
        const bool usable = !release.draft && !release.prerelease;
        const bool newer = usable && compareVersions(release.version, config.current) > 0;
        const bool installable = newer && !release.assetUrl.empty() && !release.sumsUrl.empty();
        logLine("[update] latest release %s (%s): %s", release.tag.c_str(), release.page.c_str(),
                newer ? (installable ? "newer" : "newer, no exe / SHA256SUMS on it") : "not newer");
        shared->update([&](View& v) {
            if (usable) {
                v.latest = release.version;
                v.page = release.page;
            }
            v.installable = installable;
            v.checkedAt = unixNow();
        });
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->checkedPending = true;
            shared->failedAt = 0;
        }
        if (!installJob) {
            finish(newer ? State::Available : State::UpToDate, "");
            return;
        }
        if (!newer) {
            finish(State::UpToDate, "");
            return;
        }
        if (!installable) {
            finish(State::InstallFailed,
                   release.assetUrl.empty() ? "the release has no " + release.asset + ": update by hand"
                                            : "the release has no SHA256SUMS: update by hand");
            return;
        }
        // SHA256SUMS first: no line for the exe, no download
        std::string sums;
        HttpResult got = httpGet(release.sumsUrl, config.current, false, config.insecure, &sums, "", 1 << 20);
        if (got.error.empty() && got.status != 200) got.error = "SHA256SUMS: HTTP " + std::to_string(got.status);
        std::string expected;
        if (got.error.empty() && !findChecksum(sums, release.asset, expected)) {
            got.error = "SHA256SUMS has no entry for " + release.asset;
        }
        if (!got.error.empty()) {
            logLine("[update] %s", got.error.c_str());
            finish(State::InstallFailed, got.error);
            return;
        }
        makeDirectories(config.downloadDir);
        const std::string target = config.downloadDir + "\\" + release.asset;
        const std::string part = target + ".part";
        got = httpGet(release.assetUrl, config.current, false, config.insecure, nullptr, part, 256ull << 20);
        if (got.error.empty() && got.status != 200) got.error = release.asset + ": HTTP " + std::to_string(got.status);
        std::string actual;
        if (got.error.empty()) actual = sha256File(part, got.error);
        if (got.error.empty() && actual != expected) {
            got.error = release.asset + " has SHA-256 " + actual + ", SHA256SUMS says " + expected;
        }
        if (got.error.empty() &&
            !MoveFileExW(widen(part).c_str(), widen(target).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            got.error = "can't keep the download in " + config.downloadDir;
        }
        if (!got.error.empty()) {
            DeleteFileW(widen(part).c_str());
            logLine("[update] %s", got.error.c_str());
            finish(State::InstallFailed, got.error);
            return;
        }
        logLine("[update] %s downloaded and verified (sha256 %s)", target.c_str(), actual.c_str());
        shared->update([&](View& v) { v.readyPath = target; });
        finish(State::Ready, "");
    }).detach();
}

}  // namespace update
}  // namespace pc
}  // namespace fph
