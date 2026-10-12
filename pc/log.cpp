#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include "win_text.h"
#endif

namespace fph {
namespace pc {

namespace {
std::FILE* g_log = nullptr;
std::mutex g_logMutex;
}

bool openLog(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log != nullptr) std::fclose(g_log);
#ifdef _WIN32
    g_log = _wfopen(widen(path).c_str(), L"wb");
#else
    g_log = std::fopen(path.c_str(), "wb");
#endif
    return g_log != nullptr;
}

void logLine(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::FILE* out = g_log != nullptr ? g_log : stderr;
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(out, "%s ", stamp);
    va_list args;
    va_start(args, format);
    std::vfprintf(out, format, args);
    va_end(args);
    std::fputc('\n', out);
    std::fflush(out);
}

}  // namespace pc
}  // namespace fph
