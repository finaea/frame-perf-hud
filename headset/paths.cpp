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
    return envOr("XDG_DATA_HOME", envOr("HOME", ".") + "/.local/share");
}

std::string debugDumpPath() {
    return runtimeDir() + "/" + kAppName + "-debug-" + std::to_string(::getuid()) + ".txt";
}

}  // namespace paths
