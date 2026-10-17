// Warning thresholds (yellow / red), upstream's values. Shared by the headset's config file and the widgets.
#pragma once

/**
 * Thresholds that colour values: warn = yellow, crit = red.
 */
struct Thresholds {
    double fpsWarnRatio = 0.95;    ///< app fps below this share of the refresh rate is yellow
    double fpsCritRatio = 0.75;    ///< same, red
    double frameWarnRatio = 0.9;   ///< GPU / CPU frame time above this multiple of the target is yellow
    double frameCritRatio = 1.0;   ///< same, red
    double reprojWarnPct = 5.0;    ///< reprojected frames (%)
    double reprojCritPct = 20.0;
    double tempWarnC = 70.0;       ///< CPU / GPU temperature (°C)
    double tempCritC = 80.0;
    double batteryWarnPct = 30.0;  ///< battery charge (%); at or below this it changes colour
    double batteryCritPct = 15.0;
    double powerWarnW = 13.0;      ///< headset draw (vph, W)
    double powerCritW = 16.0;
    double cpuWarnPct = 85.0;      ///< CPU load (busiest core, %)
    double cpuCritPct = 97.0;
    double gpuWarnPct = 85.0;      ///< GPU load (%): frame GPU ms × app fps
    double gpuCritPct = 95.0;
    double wifiWarnDbm = -70.0;    ///< link signal (dBm); at or below this it changes colour
    double wifiCritDbm = -78.0;
    double controllerWarnPct = 20.0;  ///< controller battery (%); at or below this it changes colour
    double controllerCritPct = 10.0;
};
