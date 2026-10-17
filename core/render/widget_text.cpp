#include "widget_text.h"

namespace fph {

const WidgetText& widgetText() {
    static const WidgetText kEnglish = {
        "N/A: ",
        // Titles
        "Frame",
        "CPU",
        "GPU & RAM",
        "Power",
        "Battery",
        "Temp",
        "CPU usage items",
        "RAM usage items",
        "Dongle",
        "Home Wi-Fi",
        "Streaming",
        "General",
        "Headset camera",
        "Eye camera",
        // Frame
        "Reproj ",
        "Dropped ",
        // CPU
        "max core ",
        "capped from ",
        // GPU & RAM
        "busy (estimate)",
        "Clock ",
        "RAM ",
        // Power
        "Rails ",
        "Unmeasured",
        "CPU",
        "GPU",
        "NPU",
        // Battery
        "Real gauge %. Steam shows a higher number",
        "Charge",
        "Into battery",
        "From battery",
        "To full",
        "To empty",
        "Health",
        "Cycles",
        "Charger",
        "Set current",
        "USB",
        "Unplugged",
        "PD offered",
        "Arriving",
        "Sag",
        " high",
        "Charging",
        "On battery",
        "Full",
        "Plugged",
        "Idle",
        // Temp
        "Fan ",
        "Thermal limit ",
        "DDR",
        "Battery",
        "Display",
        "Exhaust",
        "Heatsink",
        "NPU",
        "Camera",
        "Video",
        "Wi-Fi",
        "Flash",
        "nRF",
        "Connector",
        // Process lists
        "CPU % of one core",
        "RSS",
        "Measuring...",
        // Network
        "Not connected",
        "Link ",
        "IP ",
        // General
        "Controllers",
        "L ",
        "R ",
        "Time",
        "Uptime",
        "App",
        "none (SteamVR home)",
        "HUD",
        "Power-off",
        "Crash",
        // Throttle badge
        "Throttled to 1/",
        " of ",
    };
    return kEnglish;
}

}  // namespace fph
