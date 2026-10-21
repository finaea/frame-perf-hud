// formatRate: the Network widget's ↓/↑ text (kbps below 1 Mbps, Mbps above).
#include "widgets.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(double mbps, const char* value, const char* unit) {
    const fph::RateText t = fph::formatRate(mbps);
    if (t.value != value || t.unit != unit) {
        std::printf("FAIL formatRate(%g) = \"%s%s\", want \"%s%s\"\n", mbps, t.value.c_str(), t.unit.c_str(), value, unit);
        ++failures;
    }
}

}  // namespace

int main() {
    expect(0.0, "0", " kbps");
    expect(0.0004, "0", " kbps");
    expect(0.024, "24", " kbps");
    expect(0.9994, "999", " kbps");
    expect(0.9995, "1.0", " Mbps");  // rounds to 1000 kbps: say 1.0 Mbps, never "1000 kbps"
    expect(1.0, "1.0", " Mbps");
    expect(4.56, "4.6", " Mbps");
    expect(99.9, "99.9", " Mbps");
    expect(123.4, "123", " Mbps");
    expect(-0.001, "0", " kbps");  // a counter glitch never shows a negative rate
    expect(std::nan(""), "--", " Mbps");
    if (failures == 0) std::printf("render-rate-text: all passed\n");
    return failures == 0 ? 0 : 1;
}
