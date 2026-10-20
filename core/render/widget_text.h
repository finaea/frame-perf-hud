// UI strings for the widgets. Drawing code never contains UI text; it looks it up here.
// English only for now, like headset/i18n.* (other languages are welcome, see README).
#pragma once

namespace fph {

struct WidgetText {
    const char* naPrefix;  ///< before a reason: "N/A: "
    // Titles
    const char* frame;
    const char* cpu;
    const char* gpuRam;
    const char* power;
    const char* battery;
    const char* temp;
    const char* cpuItems;
    const char* ramItems;
    const char* networkDongle;
    const char* networkHome;
    const char* streaming;
    const char* general;
    const char* headsetCamera;
    const char* eyeCamera;
    const char* httpCamera;
    const char* httpCameraOnlyOne;  ///< on every HTTP camera card after the first
    // Frame
    const char* reproj;   ///< followed by %
    const char* dropped;  ///< followed by a count
    // CPU
    const char* maxCore;  ///< followed by %
    const char* capped;   ///< cluster clock capped below the hardware max (Playtime); followed by that max (GHz)
    // GPU & RAM
    const char* gpuLoad;  ///< after the big %: frame GPU ms × app fps, not the whole GPU (core/gpu_load.h)
    const char* clock;
    const char* ram;
    // Power
    const char* rails;       ///< sum of measured rails, followed by W
    const char* unmeasured;  ///< vph − rails
    const char* railCpu;
    const char* railGpu;
    const char* railNpu;
    // Battery
    const char* batteryNote;  ///< the gauge % vs Steam's number
    const char* charge;
    const char* flowIn;       ///< W into the battery
    const char* flowOut;      ///< W out of it
    const char* toFull;
    const char* toEmpty;
    const char* health;
    const char* cycles;
    const char* charger;
    const char* setCurrent;
    const char* usb;
    const char* unplugged;
    const char* pdOffered;
    const char* arriving;
    const char* sag;
    const char* sagHigh;  ///< after the sag value when ≥ 10% of the offered V
    const char* statusCharging;
    const char* statusDischarging;
    const char* statusFull;
    const char* statusPlugged;
    const char* statusIdle;
    // Temp
    const char* fan;
    const char* thermalLimit;  ///< badge, followed by CPU / GPU / CPU+GPU
    const char* tDdr;
    const char* tBattery;
    const char* tDisplay;
    const char* tExhaust;
    const char* tHeatsink;
    const char* tNpu;
    const char* tCamera;
    const char* tVideo;
    const char* tWifi;
    const char* tFlash;
    const char* tNrf;
    const char* tConnector;
    // Process lists
    const char* pctOfCore;  ///< CPU column header: % of one core
    const char* rss;        ///< RAM column header
    const char* measuring;  ///< first scan, no CPU interval yet
    // Network
    const char* notConnected;
    const char* link;  ///< PHY rate, followed by tx/rx
    const char* ip;
    // General
    const char* controllers;
    const char* left;
    const char* right;
    const char* time;
    const char* uptime;
    const char* app;
    const char* noApp;  ///< the compositor is showing
    const char* hudCost;  ///< frame-perf-hud's own cost
    const char* powerOff;  ///< T11: the last power-off reason
    const char* crashLog;  ///< T11: a crash log was kept at start
    // Throttle badge: "Throttled to 1/N of H Hz"
    const char* throttledTo;
    const char* throttledOf;
};

/** @return the English strings (static) */
const WidgetText& widgetText();

}  // namespace fph
