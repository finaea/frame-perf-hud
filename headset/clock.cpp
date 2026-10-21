// 時計の文字列の実装。
#include "clock.h"

#include <cstdio>

std::string formatClock(const std::tm& local, int format) {
    char text[32];
    if (format == 12) {
        // 0 時と 12 時は 12 と書く（12:00 AM が真夜中、12:00 PM が正午）
        const int hour = local.tm_hour % 12;
        std::snprintf(text, sizeof(text), "%d:%02d %s", hour != 0 ? hour : 12, local.tm_min,
                      local.tm_hour < 12 ? "AM" : "PM");
    } else if (format == 24) {
        std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    } else {
        return "";
    }
    return text;
}

std::string localClock(int format) {
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    if (localtime_r(&now, &local) == nullptr) return "--:--";
    return formatClock(local, format);
}
