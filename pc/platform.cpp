#include "platform.h"

#include "win_text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace fph {
namespace pc {

const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValue = L"frame-perf-hud";

std::string defaultDataDir() {
    PWSTR path = nullptr;
    std::string out;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) == S_OK && path != nullptr) {
        out = narrow(path) + "\\frame-perf-hud";
    }
    CoTaskMemFree(path);
    return out;
}

std::string computerName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size) && size > 0) return narrow(std::wstring(buffer, size));
    return "Windows PC";
}

std::string exePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) return narrow(std::wstring(buffer.data(), n));
        buffer.resize(buffer.size() * 2);
    }
}

namespace {

std::wstring runCommand() {
    return L"\"" + widen(exePath()) + L"\" --tray";
}

}  // namespace

bool autostartEnabled(const wchar_t* key, const wchar_t* value) {
    wchar_t data[2048] = {};
    DWORD size = sizeof(data) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS) return false;
    return _wcsicmp(data, runCommand().c_str()) == 0;
}

bool setAutostart(bool enable, const wchar_t* key, const wchar_t* value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG rc;
    if (enable) {
        const std::wstring command = runCommand();
        rc = RegSetValueExW(k, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, value);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

}  // namespace pc
}  // namespace fph
