// pc/single_instance: the named mutex that keeps one app per session.
#include "single_instance.h"

#include <cstdio>
#include <memory>
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

using namespace fph::pc;

}  // namespace

int main() {
    // Our own names, so a running FramePerfHud can't interfere
    const std::wstring name = L"Local\\fph-pc-single-instance-test-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring other = name + L"-other";

    {
        auto a = std::make_unique<SingleInstance>(name.c_str());
        CHECK(a->isFirst());
        {
            SingleInstance b(name.c_str());
            CHECK(!b.isFirst());
            SingleInstance c(name.c_str());
            CHECK(!c.isFirst());
            SingleInstance different(other.c_str());  // another name is another app
            CHECK(different.isFirst());
        }
        // The second ones are gone; the first still holds it
        {
            SingleInstance d(name.c_str());
            CHECK(!d.isFirst());
        }
        a.reset();
        SingleInstance e(name.c_str());
        CHECK(e.isFirst());  // released with the first
    }

    CHECK(showMessage() != 0);
    CHECK(showMessage() == showMessage());
    // No window of this class: false, not a crash
    CHECK(!activateFirstInstance(L"fph-pc-test-no-such-window-class"));

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc single instance: all checks passed\n");
    return 0;
}
