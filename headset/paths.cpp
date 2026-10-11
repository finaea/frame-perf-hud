#include "paths.h"

#include <unistd.h>

#include <cstdlib>
#include <string>

namespace paths {

namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

std::string home() {
    return envOr("HOME", ".");
}

/** @return the directory holding the running binary, or "" if /proc/self/exe can't be read */
std::string exeDir() {
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return "";
    std::string path(buffer, static_cast<size_t>(n));
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

bool readable(const std::string& path) {
    return ::access(path.c_str(), R_OK) == 0;
}

}  // namespace

std::string runtimeDir() {
    // Same place whether started by systemd, by Steam (+ in the dashboard) or over SSH
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return runtime;
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime;
    return "/tmp";
}

std::string dataHome() {
    return envOr("XDG_DATA_HOME", home() + "/.local/share");
}

std::string stateDir() {
    return envOr("XDG_STATE_HOME", home() + "/.local/state") + "/" + kAppName;
}

std::string debugDumpPath() {
    return runtimeDir() + "/" + kAppName + "-debug-" + std::to_string(::getuid()) + ".txt";
}

std::string resolveFontPath(const std::string& configured, bool bold) {
    if (!configured.empty()) return configured;
    const std::string file = bold ? "NotoSans-Bold.ttf" : "NotoSans-Regular.ttf";
    const std::string dir = exeDir();
    const std::string candidates[] = {
        dataHome() + "/" + kAppName + "/fonts/" + file,  // installed by install.sh
        dir + "/../assets/fonts/" + file,                // dev build in <repo>/build
        dir + "/assets/fonts/" + file,                   // run from the unpacked release
    };
    for (const std::string& path : candidates) {
        if (readable(path)) return path;
    }
    return "";
}

}  // namespace paths
