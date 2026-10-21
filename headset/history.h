// フレーム時間の集計結果と、グラフ用の時系列バッファ。
#pragma once

#include "series.h"  // History: in core/ so the widgets share it

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

/**
 * 1 回の更新間隔ぶんのフレーム時間の集計。
 */
struct FrameStats {
    bool valid = false;           ///< OpenVR からフレームの情報が取れたら true
    bool haveTimes = false;       ///< この間隔に新しいフレームがあり、ms の値が入っている
    double appFps = std::numeric_limits<double>::quiet_NaN();  ///< アプリが新しく出して、表示されたフレームの数（毎秒）
    double displayHz = 0.0;       ///< HMD のリフレッシュレート
    double targetMs = 0.0;        ///< 1 フレームの目標時間（1000 / Hz）
    double gpuMs = 0.0;           ///< GPU 時間の平均（m_flTotalRenderGpuMs）
    double gpuMaxMs = 0.0;        ///< GPU 時間の最大
    double cpuMs = 0.0;           ///< CPU 時間の平均（アプリ + コンポジタ）
    double cpuMaxMs = 0.0;
    uint32_t frames = 0;          ///< 集計したフレーム数
    double reprojectedPct = 0.0;  ///< 再投影の割合（直近 1 秒の表示回数に対する、同じフレームの 2 回目以降の %）
    uint32_t dropped = 0;         ///< 落ちたフレーム数（直近 1 秒ぶん、m_nNumDroppedFrames の合計）
    int throttledFrames = 0;      ///< SteamVR がアプリを抑えている段数（1 = リフレッシュレートの半分、2 = 1/3）
    double gpuBusyPct = std::numeric_limits<double>::quiet_NaN();  ///< GPU 負荷の目安（estimateGpuBusyPct）
};

/**
 * GPU 負荷の目安を、SteamVR のフレーム計測から出す（1 枚あたりの GPU 時間 × 1 秒あたりの枚数）。
 * カーネルの DRM fdinfo（drm-engine-gpu・drm-cycles-gpu）は SteamOS 0.5.6 の msm では重いゲームの分を
 * 正しく数えないので使わない。数えるのは VR のアプリとコンポジタの分だけ。
 * @param gpuMs 1 枚あたりの GPU 時間の平均（ms、m_flTotalRenderGpuMs）
 * @param fps アプリの新しいフレームの数（毎秒）
 * @return 使用率（%、0〜100）。どちらかが取れなければ NaN
 * @example
 * estimateGpuBusyPct(16.5, 58.6) // 96.69（Half-Life: Alyx を 72Hz で動かしたとき）
 */
inline double estimateGpuBusyPct(double gpuMs, double fps) {
    if (!std::isfinite(gpuMs) || !std::isfinite(fps) || gpuMs < 0 || fps < 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // GPU の仕事が重なって次のフレームの分まで食い込むと 100% を超えるので止める
    return std::clamp(gpuMs * fps / 10.0, 0.0, 100.0);
}

/**
 * コントローラー 1 本ぶんの電池。
 */
struct ControllerBattery {
    bool present = false;                                         ///< つながっている
    double pct = std::numeric_limits<double>::quiet_NaN();        ///< 残量（%）。取れなければ NaN
    bool charging = false;                                        ///< 充電中
};

/**
 * 左右のコントローラーの電池。
 */
struct ControllerStatus {
    ControllerBattery left;
    ControllerBattery right;
};
