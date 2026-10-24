// A time series for the graphs (History, moved from headset/history.h so the widgets share it).
#pragma once

#include <deque>
#include <limits>

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
