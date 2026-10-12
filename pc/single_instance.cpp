#include "single_instance.h"

namespace fph {
namespace pc {

const wchar_t* const kInstanceMutexName = L"Local\\frame-perf-hud-pc-7c1e4b2a";
const wchar_t* const kMainWindowClass = L"FramePerfHudPcWindow";

unsigned showMessage() {
    static const unsigned message = RegisterWindowMessageW(L"frame-perf-hud-pc-show");
    return message;
}

SingleInstance::SingleInstance(const wchar_t* name) {
    mutex_ = CreateMutexW(nullptr, TRUE, name);
    // ERROR_ALREADY_EXISTS: someone holds it. A null handle (access denied by another user's object) counts as
    // "not first" too, so we never run two copies.
    first_ = mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_ != nullptr) {
        if (first_) ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
}

bool activateFirstInstance(const wchar_t* windowClass) {
    // The first instance may still be creating its window: try for ~2 s
    for (int i = 0; i < 20; ++i) {
        HWND hwnd = FindWindowW(windowClass, nullptr);
        if (hwnd != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(hwnd, showMessage(), 0, 0);
            return true;
        }
        Sleep(100);
    }
    return false;
}

}  // namespace pc
}  // namespace fph
