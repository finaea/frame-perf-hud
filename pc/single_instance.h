// One app per Windows session: a named mutex. A second launch finds the first one's window and asks it to come
// forward, then exits.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fph {
namespace pc {

/** The mutex name ("Local\\" = per logon session, so two Windows users each get one). */
extern const wchar_t* const kInstanceMutexName;
/** The main window's class name, which the second launch looks for. */
extern const wchar_t* const kMainWindowClass;
/** Registered message the second launch posts: "show yourself". */
unsigned showMessage();

class SingleInstance {
public:
    /** @param name the mutex name (tests pass their own) */
    explicit SingleInstance(const wchar_t* name = kInstanceMutexName);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /** @return true if this process is the first (it holds the mutex until destroyed) */
    bool isFirst() const { return first_; }

private:
    HANDLE mutex_ = nullptr;
    bool first_ = false;
};

/**
 * The second launch: lets the first one take the foreground and posts showMessage() to its window.
 * @return true if a window was found
 */
bool activateFirstInstance(const wchar_t* windowClass = kMainWindowClass);

}  // namespace pc
}  // namespace fph
