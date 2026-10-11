// フレーム時間の集計結果と、グラフ用の時系列バッファ。
#pragma once

#include <cstdint>
#include <deque>
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
};

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

/**
 * 時刻つきの値を一定時間ぶんだけ持つ時系列。グラフ描画に使う。
 */
class History {
public:
    /** 時刻と値の組。 */
    struct Point {
        double t;  ///< 秒（単調増加の時計）
        double v;  ///< 値（NaN は線を途切れさせる）
    };

    /**
     * 値を追加し、古いものを捨てる。
     * @param t 時刻（秒）
     * @param v 値
     * @param keepSeconds 残す秒数
     */
    void push(double t, double v, double keepSeconds) {
        points_.push_back({t, v});
        while (!points_.empty() && points_.front().t < t - keepSeconds) points_.pop_front();
    }

    /** @return 保持している点 */
    const std::deque<Point>& points() const { return points_; }

    /**
     * 保持している値の最大（NaN は無視）。
     * @return 最大値。値が無ければ NaN
     */
    double maxValue() const {
        double best = std::numeric_limits<double>::quiet_NaN();
        for (const auto& p : points_) {
            if (p.v == p.v && (best != best || p.v > best)) best = p.v;
        }
        return best;
    }

    /**
     * 保持している値の最小（NaN は無視）。
     * @return 最小値。値が無ければ NaN
     */
    double minValue() const {
        double best = std::numeric_limits<double>::quiet_NaN();
        for (const auto& p : points_) {
            if (p.v == p.v && (best != best || p.v < best)) best = p.v;
        }
        return best;
    }

    /** 全部捨てる。 */
    void clear() { points_.clear(); }

private:
    std::deque<Point> points_;
};
