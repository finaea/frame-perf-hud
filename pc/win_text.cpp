#include "win_text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fph {
namespace pc {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace pc
}  // namespace fph
