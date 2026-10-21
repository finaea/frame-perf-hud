#include "widget_text.h"

#include <cstdio>

namespace fph {

namespace {

const WidgetText kJapanese = {
    WidgetLanguage::Ja,
    "N/A: ",  // TODO(ja)
    // Titles
    "フレーム",
    "CPU",
    "GPU & RAM",  // TODO(ja)
    "電力",
    "電池",
    "温度",
    "Network",  // TODO(ja)
    "General",  // TODO(ja)
    // Frame
    "計測待ち",
    "SteamVR 未接続",
    "再投影 ",
    "落ち ",
    // CPU
    "最大コア ",
    "big core ",  // TODO(ja)
    // GPU & RAM
    "load",  // TODO(ja)
    "Clock ",  // TODO(ja)
    "メモリ ",
    // Power
    "Rails ",  // TODO(ja)
    // Battery
    "Real gauge %. Steam shows a higher number",  // TODO(ja)
    "Into battery",  // TODO(ja)
    "From battery",  // TODO(ja)
    "充電中",
    "放電中",
    "満充電",
    "給電中",
    "待機",
    // Temp
    "℃ CPU",
    "℃",
    "ファン ",
    "熱で制限中 ",
    "CPU・GPU",
    "DDR",
    "電池",
    "画面",
    "排気",
    "放熱",
    // Network
    "直通",
    "Wi-Fi",
    "未接続",
    "リンク ",
    // General
    "Controllers",  // TODO(ja)
    "左手 ",
    "右手 ",
    "現在時刻",
};

const WidgetText kEnglish = {
    WidgetLanguage::En,
    "N/A: ",
    // Titles
    "Frame",
    "CPU",
    "GPU & RAM",
    "Power",
    "Battery",
    "Temp",
    "Network",
    "General",
    // Frame
    "Measuring",
    "No SteamVR",
    "Reproj ",
    "Dropped ",
    // CPU
    "max core ",
    "big core ",
    // GPU & RAM
    "load",
    "Clock ",
    "RAM ",
    // Power
    "Rails ",
    // Battery
    "Real gauge %. Steam shows a higher number",
    "Into battery",
    "From battery",
    "Charging",
    "On battery",
    "Full",
    "Plugged",
    "Idle",
    // Temp
    "°C CPU",
    "°C",
    "Fan ",
    "Thermal limit ",
    "CPU+GPU",
    "DDR",
    "Battery",
    "Display",
    "Exhaust",
    "Heatsink",
    // Network
    "Dongle",
    "Wi-Fi",
    "Not connected",
    "Link ",
    // General
    "Controllers",
    "L ",
    "R ",
    "Time",
};

}  // namespace

const WidgetText& widgetText(WidgetLanguage language) {
    return language == WidgetLanguage::En ? kEnglish : kJapanese;
}

std::string throttleBadgeText(const WidgetText& t, double hz, int throttled) {
    char hzText[16];
    std::snprintf(hzText, sizeof(hzText), "%.0f", hz);
    const std::string fraction = "1/" + std::to_string(throttled + 1);
    if (t.language == WidgetLanguage::En) return "Throttled to " + fraction + " of " + hzText + "Hz (reproj)";
    return std::string(hzText) + "Hz の" + (throttled == 1 ? std::string("半分") : fraction) + "に制限（再投影）";
}

}  // namespace fph
