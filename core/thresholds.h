// 色を変えるしきい値。設定ファイル（headset/config.h）とウィジェットで共通に使う。
#pragma once

/**
 * 色を変えるしきい値。warn で黄色、crit で赤。
 */
struct Thresholds {
    double fpsWarnRatio = 0.95;    ///< アプリの fps がリフレッシュレートのこの割合を下回ったら黄
    double fpsCritRatio = 0.75;    ///< 同じく赤
    double frameWarnRatio = 0.9;   ///< GPU フレーム時間が目標時間のこの倍率を超えたら黄
    double frameCritRatio = 1.0;   ///< 同じく赤
    double reprojWarnPct = 5.0;    ///< 再投影の割合（%）
    double reprojCritPct = 20.0;
    double tempWarnC = 70.0;       ///< CPU・GPU 温度（℃）
    double tempCritC = 80.0;
    double batteryWarnPct = 30.0;  ///< バッテリー残量（%）。これ以下で色が変わる
    double batteryCritPct = 15.0;
    double powerWarnW = 13.0;      ///< 本体の消費電力（vph、W）
    double powerCritW = 16.0;
    double cpuWarnPct = 85.0;      ///< CPU 使用率（最も忙しいコア、%）
    double cpuCritPct = 97.0;
    double gpuWarnPct = 85.0;      ///< GPU 負荷の目安（%）
    double gpuCritPct = 95.0;
    double wifiWarnDbm = -70.0;    ///< 直通回線の電波（dBm）。これ以下で色が変わる
    double wifiCritDbm = -78.0;
    double controllerWarnPct = 20.0;  ///< コントローラーの電池（%）。これ以下で色が変わる
    double controllerCritPct = 10.0;
};
