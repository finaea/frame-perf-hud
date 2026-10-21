#include "atomic_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace frame_perf_overlay {

namespace {

/** @return the folder part of path ("" if it has none) */
std::string parentDir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

#ifdef _WIN32

/** @return path as UTF-16 for the W APIs */
std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string lastErrorText(const char* what) {
    return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

#endif

}  // namespace

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return true;
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/' && dir[pos] != '\\') continue;
        const std::string part = dir.substr(0, pos);
#ifdef _WIN32
        if (part.size() == 2 && part[1] == ':') continue;  // "C:"
        if (!CreateDirectoryW(widen(part).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#else
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
    }
    return true;
}

bool atomicWriteFile(const std::string& path, const std::string& content, std::string& error) {
    if (!makeDirectories(parentDir(path))) {
        error = "can't create the folder of " + path;
        return false;
    }
    const std::string temp = path + ".tmp";
#ifdef _WIN32
    const std::wstring wtemp = widen(temp);
    HANDLE file = CreateFileW(wtemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = lastErrorText("CreateFileW") + ": " + temp;
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size() && FlushFileBuffers(file);
    if (!ok) error = lastErrorText("WriteFile / FlushFileBuffers") + ": " + temp;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(wtemp.c_str());
        return false;
    }
    if (!MoveFileExW(wtemp.c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = lastErrorText("MoveFileExW") + ": " + path;
        DeleteFileW(wtemp.c_str());
        return false;
    }
    return true;
#else
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't open " + temp + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = "write failed: " + temp + ": " + std::strerror(errno);
            ::close(fd);
            ::unlink(temp.c_str());
            return false;
        }
        done += static_cast<size_t>(n);
    }
    // The data must be on disk before the rename makes it the config: otherwise a power cut right after
    // can leave a renamed but empty file
    if (::fsync(fd) != 0) {
        error = "fsync failed: " + temp + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    }
    ::close(fd);
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "rename failed: " + std::string(std::strerror(errno));
        ::unlink(temp.c_str());
        return false;
    }
    // Persist the rename itself (best effort: the file is already complete either way)
    const std::string dir = parentDir(path);
    const int dirFd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        (void)::fsync(dirFd);
        ::close(dirFd);
    }
    return true;
#endif
}

}  // namespace frame_perf_overlay
