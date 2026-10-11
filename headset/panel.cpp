// パネル描画の実装。数字 + 直近の折れ線グラフ + しきい値による色分け。
#include "panel.h"

#include "clock.h"
#include "draw.h"
#include "i18n.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// 画像の大きさとレイアウト（px）
constexpr int kWidth = 512;
constexpr int kPad = 14;
constexpr int kRowHeight = 92;
constexpr int kRowGap = 18;                   // 段のカードの間が空くように（カードの上下のはみ出しより広く）
constexpr int kLeftWidth = 188;               // 数字の列の幅
constexpr int kGraphX = kPad + kLeftWidth + 8;
constexpr int kGraphWidth = kWidth - kPad - kGraphX;
constexpr int kTextLineHeight = 26;
constexpr int kTextRows = 3;                  // 下の文字の段（CPU・無線・電池）。時計を出すときはもう 1 行
constexpr int kTextTopGap = 18;               // 温度の段と、下の文字の段の間
// 時計を出さないときの高さ（434）と、時計の行を足した高さ（460）。テクスチャはいつも 460 で作り、
// 時計を出さないときは上の 434 だけを使う（VrOverlay::setPanelVisibleHeight）
constexpr int kHeightNoClock = kPad + 3 * kRowHeight + 2 * kRowGap + kTextTopGap + kTextRows * kTextLineHeight + 12;
constexpr int kHeight = kHeightNoClock + kTextLineHeight;
constexpr double kCardInsetX = 8;             // 段のカードの左右（パネルの端から）
constexpr double kCardAbove = 6;              // 段のカードが中身より上に出る分
constexpr double kCardBelow = 4;              // 段のカードが中身より下に出る分
constexpr int kTextTop = kPad + 3 * kRowHeight + 2 * kRowGap + kTextTopGap;  // 下の文字の段の上端

/** 値の状態。 */
enum class Level { Normal, Warn, Crit };

/**
 * 値としきい値から状態を決める。
 * @param value 値（NaN なら Normal）
 * @param warn 黄にするしきい値
 * @param crit 赤にするしきい値
 * @param lowerIsWorse true なら値が小さいほど悪い（バッテリー残量など）
 * @return 状態
 */
Level levelOf(double value, double warn, double crit, bool lowerIsWorse = false) {
    if (std::isnan(value)) return Level::Normal;
    if (lowerIsWorse) {
        if (value <= crit) return Level::Crit;
        if (value <= warn) return Level::Warn;
        return Level::Normal;
    }
    if (value >= crit) return Level::Crit;
    if (value >= warn) return Level::Warn;
    return Level::Normal;
}

/**
 * 状態に合った文字色を返す。
 * @param level 状態
 * @param normal ふだんの色
 * @return 色
 */
Color colorOf(Level level, Color normal = kText) {
    switch (level) {
        case Level::Warn: return kWarn;
        case Level::Crit: return kDanger;
        default: return normal;
    }
}

/**
 * 2 つの状態の悪いほうを返す。
 * @param a 状態
 * @param b 状態
 * @return 悪いほう
 */
Level worse(Level a, Level b) {
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

/**
 * 数値を文字列にする。NaN は "--"。
 * @param value 値
 * @param decimals 小数点以下の桁数
 * @return 文字列
 */
std::string num(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

/**
 * 3 桁ごとにカンマを入れた整数の文字列にする。NaN は "--"。
 * @param value 値
 * @return 文字列
 */
std::string grouped(double value) {
    if (std::isnan(value)) return "--";
    std::string digits = std::to_string(static_cast<long long>(std::lround(value)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}


/**
 * グラフの縦軸の範囲と置き場所。
 */
struct GraphArea {
    double x, y, w, h;   ///< 描く場所（px）
    double minV, maxV;   ///< 縦軸の範囲
    double now;          ///< 右端の時刻（秒）
    double seconds;      ///< 横軸の秒数

    /**
     * 時刻を x 座標にする。
     * @param t 時刻
     * @return x
     */
    double xOf(double t) const { return x + w - (now - t) / seconds * w; }

    /**
     * 値を y 座標にする（範囲外は端に寄せる）。
     * @param v 値
     * @return y
     */
    double yOf(double v) const {
        const double ratio = (std::clamp(v, minV, maxV) - minV) / (maxV - minV);
        return y + h - ratio * h;
    }
};

/**
 * グラフの背景と、左上の目盛りの文字を描く。
 * @param pen 描画道具
 * @param area グラフの場所
 * @param topLabel 左上に出す最大値の文字
 */
void drawGraphFrame(const Pen& pen, const GraphArea& area, const std::string& topLabel) {
    // グラフの台（地の色の角丸）は動かない部分として先に描いてあるので、ここでは目盛りの文字だけ
    pen.text(area.x + 6, area.y + 14, topLabel, 12, kTextMuted);
}

/**
 * 横向きの点線（目標値やしきい値）を描く。
 * @param pen 描画道具
 * @param area グラフの場所
 * @param value 線を引く値（範囲外なら描かない）
 * @param c 色
 */
void drawGuide(const Pen& pen, const GraphArea& area, double value, Color c) {
    if (std::isnan(value) || value <= area.minV || value >= area.maxV) return;
    const double y = std::round(area.yOf(value)) + 0.5;
    const double dashes[] = {4.0, 4.0};
    cairo_save(pen.cr);
    cairo_set_dash(pen.cr, dashes, 2, 0);
    cairo_set_line_width(pen.cr, 1.0);
    pen.color(c);  // コントラスト比は不透明な色で確かめているので、薄めない
    cairo_move_to(pen.cr, area.x + 4, y);
    cairo_line_to(pen.cr, area.x + area.w - 4, y);
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

/**
 * 折れ線を描く。NaN の点で線を途切れさせる。
 * @param pen 描画道具
 * @param area グラフの場所
 * @param history 描く時系列
 * @param c 線の色
 */
void drawSeries(const Pen& pen, const GraphArea& area, const History& history, Color c) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    cairo_set_line_width(pen.cr, 2.0);
    cairo_set_line_join(pen.cr, CAIRO_LINE_JOIN_ROUND);
    pen.color(c);
    bool drawing = false;
    for (const auto& p : history.points()) {
        if (std::isnan(p.v)) {
            drawing = false;
            continue;
        }
        const double x = area.xOf(p.t);
        const double y = area.yOf(p.v);
        if (drawing) {
            cairo_line_to(pen.cr, x, y);
        } else {
            cairo_move_to(pen.cr, x, y);
            drawing = true;
        }
    }
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

/**
 * 時系列を下から塗りつぶして描く（GPU 使用率のように背景として薄く出す用）。
 * @param pen 描画道具
 * @param area グラフの場所（縦軸はこの時系列の範囲）
 * @param history 描く時系列
 * @param c 色
 * @param alpha 塗りの不透明度
 */
void drawSeriesFill(const Pen& pen, const GraphArea& area, const History& history, Color c, double alpha) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    pen.color(c, alpha);
    const double baseY = area.y + area.h;
    bool open = false;
    double lastX = 0;
    for (const auto& p : history.points()) {
        const double x = area.xOf(p.t);
        if (std::isnan(p.v)) {
            if (open) {
                cairo_line_to(pen.cr, lastX, baseY);
                cairo_close_path(pen.cr);
                open = false;
            }
            continue;
        }
        if (!open) {
            cairo_move_to(pen.cr, x, baseY);
            open = true;
        }
        cairo_line_to(pen.cr, x, area.yOf(p.v));
        lastX = x;
    }
    if (open) {
        cairo_line_to(pen.cr, lastX, baseY);
        cairo_close_path(pen.cr);
    }
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
}

/**
 * 目立つ警告のバッジ（塗りの角丸に白い文字）を右上揃えで描く。
 * @param pen 描画道具
 * @param right 右端 x
 * @param top 上端 y
 * @param text 文字
 * @param c 塗りの色
 */
void drawBadge(const Pen& pen, double right, double top, const std::string& text, Color c,
               Color textColor = kOnAccent) {
    const double size = 14;
    const double width = pen.measure(text, size, true) + 16;
    pen.color(c, 0.95);
    pen.roundedRect(right - width, top, width, 22, 6);
    cairo_fill(pen.cr);
    pen.text(right - width + 8, top + 16, text, size, textColor, true);
}


/**
 * Draws a small up / down arrow (the bundled Noto Sans has no arrow glyphs, U+2190-2193).
 * @param pen drawing tools
 * @param x left edge
 * @param baseline text baseline
 * @param up true for up, false for down
 * @param c colour
 * @return width drawn (px)
 */
double drawArrow(const Pen& pen, double x, double baseline, bool up, Color c) {
    const double h = 11;
    const double top = baseline - h;
    const double cx = x + 4.5;
    pen.color(c);
    cairo_save(pen.cr);
    cairo_set_line_width(pen.cr, 1.6);
    cairo_move_to(pen.cr, cx, top + (up ? 3 : 0));
    cairo_line_to(pen.cr, cx, baseline - (up ? 0 : 3));
    cairo_stroke(pen.cr);
    // Arrow head
    const double tipY = up ? top : baseline;
    const double backY = up ? top + 4.5 : baseline - 4.5;
    cairo_move_to(pen.cr, cx, tipY);
    cairo_line_to(pen.cr, cx - 4, backY);
    cairo_line_to(pen.cr, cx + 4, backY);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
    return 11;
}

/**
 * 充電中を表す小さな稲妻を描く。
 * @param pen 描画道具
 * @param x 左端
 * @param baseline 文字のベースライン
 * @param c 色
 * @return 描いた幅（px）
 */
double drawBolt(const Pen& pen, double x, double baseline, Color c) {
    const double h = 13;
    const double top = baseline - h + 1;
    pen.color(c);
    cairo_move_to(pen.cr, x + 6, top);
    cairo_line_to(pen.cr, x + 1, top + h * 0.58);
    cairo_line_to(pen.cr, x + 4.5, top + h * 0.58);
    cairo_line_to(pen.cr, x + 3, top + h);
    cairo_line_to(pen.cr, x + 8.5, top + h * 0.38);
    cairo_line_to(pen.cr, x + 5, top + h * 0.38);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    return 10;
}

/** Wi-Fi アイコンで 4 本とも点く強さ（dBm）。3 本・2 本の境目は設定の黄・赤のしきい値を使う。 */
constexpr double kWifiFullBarsDbm = -60.0;

/**
 * 電波の強さから、Wi-Fi アイコンで点く本数（点 + 弧 3 本の 4 段階）を決める。
 * @param dbm 電波の強さ（dBm、NaN なら 0 本）
 * @param th しきい値（wifi_warn_dbm / wifi_crit_dbm）
 * @return 1〜4。分からなければ 0
 */
int wifiBars(double dbm, const Thresholds& th) {
    if (std::isnan(dbm)) return 0;
    if (dbm > kWifiFullBarsDbm) return 4;
    if (dbm > th.wifiWarnDbm) return 3;
    if (dbm > th.wifiCritDbm) return 2;
    return 1;
}

/**
 * 扇形の Wi-Fi アイコン（下の点 + 弧 3 本）を描く。点いていない弧は薄いグレー。
 * @param pen 描画道具
 * @param x 左端
 * @param baseline 文字のベースライン（アイコンの下端をここにそろえる）
 * @param bars 点く本数（0〜4）
 * @param c 点いている部分の色
 * @param crossed 斜線を入れる（未接続）
 * @return 描いた幅（px）
 */
double drawWifiIcon(const Pen& pen, double x, double baseline, int bars, Color c, bool crossed) {
    const double width = 26;
    const double cx = x + width / 2;
    const double cy = baseline;  // 扇のかなめ
    const double radii[] = {6.0, 10.5, 15.0};
    cairo_save(pen.cr);
    cairo_set_line_cap(pen.cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(pen.cr, 2.4);
    // かなめの点（1 本目）
    if (bars >= 1) pen.color(c); else pen.color(kTextMuted, 0.35);
    cairo_arc(pen.cr, cx, cy - 1.5, 2.2, 0, 2 * M_PI);
    cairo_fill(pen.cr);
    // 弧（2〜4 本目）: 真上を中心に左右 45 度
    for (int i = 0; i < 3; ++i) {
        if (bars >= i + 2) pen.color(c); else pen.color(kTextMuted, 0.35);
        cairo_new_sub_path(pen.cr);
        cairo_arc(pen.cr, cx, cy - 1.5, radii[i], -M_PI * 0.75, -M_PI * 0.25);
        cairo_stroke(pen.cr);
    }
    if (crossed) {
        pen.color(kTextMuted);
        cairo_set_line_width(pen.cr, 2.0);
        cairo_move_to(pen.cr, cx - 9, cy - 15);
        cairo_line_to(pen.cr, cx + 9, cy + 1);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
    return width;
}

/**
 * 大きな数字と単位を描く（例: "8.2" + "ms"）。
 * @param pen 描画道具
 * @param x 左端
 * @param y ベースライン
 * @param value 数字の文字列
 * @param unit 単位
 * @param c 色
 */
double drawBigValue(const Pen& pen, double x, double y, const std::string& value, const std::string& unit, Color c) {
    const double w = pen.text(x, y, value, 30, c, true);
    return w + 3 + pen.text(x + w + 3, y, unit, 16, c);
}

/**
 * フレームの段（アプリの fps が主役、GPU・CPU の ms は補足）を描く。
 * @param pen 描画道具
 * @param top 段の上端 y
 * @param state 表示内容
 * @param config 設定
 */
void drawFrameRow(const Pen& pen, double top, const PanelState& state, const Config& config) {
    const FrameStats& f = state.frame;
    const Thresholds& th = config.thresholds;
    const UiText& t = uiText(config.language);
    const double x = kPad;

    pen.text(x, top + 17, t.frame, 16, kTextMuted);
    // 見出しの右に GPU 使用率（目安）
    const double gpuPct = state.sensors.gpuBusyPct;
    const double pctRight = x + kLeftWidth - 4;
    const double pctWidth = pen.text(pctRight, top + 17, num(gpuPct, 0) + "%", 17,
                                     colorOf(levelOf(gpuPct, th.gpuWarnPct, th.gpuCritPct)), true, true);
    pen.text(pctRight - pctWidth - 4, top + 17, "GPU", 14, kGpu, false, true);

    if (!state.vrConnected || !f.valid) {
        drawBigValue(pen, x, top + 52, "--", "fps", kTextMuted);
        pen.text(x, top + 72, state.vrConnected ? t.waiting : t.noSteamVr, 15, kTextMuted);
    } else {
        // 主役: アプリが実際に出しているフレームの数（fps）と、表示のリフレッシュレート
        const double hz = f.displayHz;
        const Level fpsLevel = hz > 0 ? levelOf(f.appFps, hz * th.fpsWarnRatio, hz * th.fpsCritRatio, true)
                                      : Level::Normal;
        pen.dot(x + 4, top + 42, kAccent);
        const double bigWidth = drawBigValue(pen, x + 14, top + 52, num(f.appFps, 0), "fps", colorOf(fpsLevel));
        if (hz > 0) pen.text(x + 14 + bigWidth + 6, top + 52, "/ " + num(hz, 0) + "Hz", 14, kTextMuted);
        // 補足: GPU・CPU の 1 フレームあたりの時間（目標時間と比べて色を変える）
        const Level gpuLevel = levelOf(f.gpuMs, f.targetMs * th.frameWarnRatio, f.targetMs * th.frameCritRatio);
        const Level cpuLevel = levelOf(f.cpuMs, f.targetMs * th.frameWarnRatio, f.targetMs * th.frameCritRatio);
        double lineX = x + 14;
        lineX += pen.text(lineX, top + 72, "GPU " + (f.haveTimes ? num(f.gpuMs, 1) : std::string("--")), 15,
                          colorOf(gpuLevel, kTextMuted));
        lineX += pen.text(lineX, top + 72, "  CPU " + (f.haveTimes ? num(f.cpuMs, 1) : std::string("--")), 15,
                          colorOf(cpuLevel, kTextMuted));
        pen.text(lineX, top + 72, " ms", 13, kTextMuted);
        const Level reprojLevel = worse(levelOf(f.reprojectedPct, th.reprojWarnPct, th.reprojCritPct),
                                        f.dropped > 0 ? Level::Warn : Level::Normal);
        pen.text(x + 14, top + 90,
                 std::string(t.reproj) + num(f.reprojectedPct, 0) + "%  " + t.dropped + std::to_string(f.dropped), 15,
                 colorOf(reprojLevel, kTextMuted));
    }

    // 縦軸は 0 fps からリフレッシュレートの 25% 上まで（線と目盛りの文字が重ならないよう。fmax は NaN を無視する）
    const double hz = f.displayHz > 0 ? f.displayHz : 90.0;
    const double maxV = std::ceil(std::fmax(hz, state.appFps.maxValue()) * 1.25 / 10.0) * 10.0;
    const GraphArea area {kGraphX, top + 4, kGraphWidth, kRowHeight - 8, 0.0, maxV,
                          state.now, static_cast<double>(config.graphSeconds)};
    drawGraphFrame(pen, area, num(area.maxV, 0) + "fps");
    // GPU 使用率（0〜100%）を背景に薄く塗る
    GraphArea pctArea = area;
    pctArea.minV = 0.0;
    pctArea.maxV = 100.0;
    drawSeriesFill(pen, pctArea, state.gpuBusyPct, kGpu, kGpuFillAlpha);
    // リフレッシュレートの点線（ここに張り付いていれば落ちていない）
    drawGuide(pen, area, hz, kTextMuted);
    drawSeries(pen, area, state.appFps, kAccent);

    // SteamVR がアプリを抑えている（半分の速さで描かせて、残りのコマを再投影で埋めている）ときはバッジで知らせる
    if (state.vrConnected && f.valid && f.throttledFrames > 0) {
        drawBadge(pen, area.x + area.w - 4, area.y + 4,
                  throttleBadgeText(config.language, hz, f.throttledFrames), kWarn,
                  kOnAccent);
    }
}

/**
 * 消費電力の段を描く。
 * @param pen 描画道具
 * @param top 段の上端 y
 * @param state 表示内容
 * @param config 設定
 */
void drawPowerRow(const Pen& pen, double top, const PanelState& state, const Config& config) {
    const SensorSample& s = state.sensors;
    const Thresholds& th = config.thresholds;
    const UiText& t = uiText(config.language);
    const double x = kPad;

    pen.text(x, top + 17, t.power, 16, kTextMuted);
    pen.dot(x + 4, top + 42, kAccent);
    drawBigValue(pen, x + 14, top + 52, num(s.powerMainW, 2), "W",
                 colorOf(levelOf(s.powerMainW, th.powerWarnW, th.powerCritW)));
    pen.text(x + 14, top + 72, t.allChannels + num(s.powerSumW, 1) + "W", 15, kTextMuted);
    pen.text(x + 14, top + 90, t.fan + grouped(s.fanRpm) + "rpm", 15, kTextMuted);

    const double maxData = state.powerW.maxValue();
    const double maxV = std::isnan(maxData) ? 5.0 : std::max(2.0, std::ceil(maxData * 1.2));
    const GraphArea area {kGraphX, top + 4, kGraphWidth, kRowHeight - 8, 0.0, maxV, state.now,
                          static_cast<double>(config.graphSeconds)};
    drawGraphFrame(pen, area, num(maxV, 0) + "W");
    drawGuide(pen, area, th.powerWarnW, kWarn);
    drawGuide(pen, area, th.powerCritW, kDanger);
    drawSeries(pen, area, state.powerW, kAccent);
}

/**
 * 温度の段を描く。
 * @param pen 描画道具
 * @param top 段の上端 y
 * @param state 表示内容
 * @param config 設定
 */
void drawTempRow(const Pen& pen, double top, const PanelState& state, const Config& config) {
    const SensorSample& s = state.sensors;
    const Thresholds& th = config.thresholds;
    const UiText& t = uiText(config.language);
    const double x = kPad;

    pen.text(x, top + 17, t.temp, 16, kTextMuted);
    pen.dot(x + 4, top + 42, kAccent);
    drawBigValue(pen, x + 14, top + 52, num(s.cpuTempC, 0), t.cpuTempUnit,
                 colorOf(levelOf(s.cpuTempC, th.tempWarnC, th.tempCritC)));
    pen.dot(x + 4, top + 67, kGpu);
    const double gpuWidth = pen.text(x + 14, top + 72, "GPU " + num(s.gpuTempC, 0) + t.celsius, 15,
                                     colorOf(levelOf(s.gpuTempC, th.tempWarnC, th.tempCritC)));
    pen.text(x + 14 + gpuWidth + 10, top + 72, t.battTemp + num(s.batteryTempC, 0) + t.celsius, 15, kTextMuted);
    pen.text(x + 14, top + 90,
             t.display + num(s.displayTempC, 0) + "  " + t.exhaust + num(s.exhaustTempC, 0) + "  " + t.heatsink +
                 num(s.heatsinkTempC, 0) + t.celsius,
             14, kTextMuted);

    // 縦軸: 30℃ から「赤のしきい値 + 5℃」までを基本にして、はみ出したら広げる
    const double dataMin = std::fmin(state.cpuTempC.minValue(), state.gpuTempC.minValue());  // NaN は無視される
    const double dataMax = std::fmax(state.cpuTempC.maxValue(), state.gpuTempC.maxValue());
    const double minV = std::isnan(dataMin) ? 30.0 : std::min(30.0, std::floor(dataMin / 5.0) * 5.0 - 5.0);
    const double maxV = std::isnan(dataMax) ? th.tempCritC + 5.0 : std::max(th.tempCritC + 5.0, dataMax + 5.0);
    const GraphArea area {kGraphX, top + 4, kGraphWidth, kRowHeight - 8, minV, maxV, state.now,
                          static_cast<double>(config.graphSeconds)};
    drawGraphFrame(pen, area, num(maxV, 0) + t.celsius);
    drawGuide(pen, area, th.tempWarnC, kWarn);
    drawGuide(pen, area, th.tempCritC, kDanger);
    drawSeries(pen, area, state.gpuTempC, kGpu);
    drawSeries(pen, area, state.cpuTempC, kAccent);

    // 熱で制限がかかっていたら、グラフの右上に目立つバッジ
    if (s.throttleCpu || s.throttleGpu) {
        drawBadge(pen, area.x + area.w - 4, area.y + 4, thermalBadgeText(config.language, s.throttleCpu, s.throttleGpu),
                  kDanger);
    }
}

/**
 * コントローラー 1 本ぶんの電池を描く（例: "左手 80%" と充電中の稲妻）。
 * @param pen 描画道具
 * @param x 左端
 * @param y ベースライン
 * @param label "左手" / "右手"
 * @param battery 電池
 * @param th しきい値
 * @return 描いた幅（px）
 */
double drawControllerBattery(const Pen& pen, double x, double y, const char* label, const ControllerBattery& battery,
                             const Thresholds& th) {
    const double start = x;
    x += pen.text(x, y, std::string(label) + " ", 15, kTextMuted);
    if (!battery.present) return x - start + pen.text(x, y, "--", 15, kTextMuted);
    const Level level = levelOf(battery.pct, th.controllerWarnPct, th.controllerCritPct, true);
    x += pen.text(x, y, num(battery.pct, 0) + "%", 16, colorOf(level), true);
    if (battery.charging) x += 2 + drawBolt(pen, x + 2, y, kWarn);
    return x - start;
}

/**
 * 下の 3 行（CPU・GPU クロック / 直通回線 / 電池・コントローラー・メモリ）を描く。
 * @param pen 描画道具
 * @param top 1 行目の上端 y
 * @param state 表示内容
 * @param config 設定
 */
void drawTextRows(const Pen& pen, double top, const PanelState& state, const Config& config) {
    const SensorSample& s = state.sensors;
    const Thresholds& th = config.thresholds;
    const UiText& t = uiText(config.language);
    double x = kPad;
    const double y1 = top + 19;
    const double y2 = top + kTextLineHeight + 19;
    const double y3 = top + 2 * kTextLineHeight + 19;

    // 1 行目: CPU 使用率とクロック、GPU クロック
    x += pen.text(x, y1, "CPU ", 16, kTextMuted);
    x += pen.text(x, y1, num(s.cpuUsagePct, 0) + "%", 17, kText, true);
    x += pen.text(x, y1, t.maxCoreOpen, 15, kTextMuted);
    x += pen.text(x, y1, num(s.cpuMaxCorePct, 0) + "%", 15, colorOf(levelOf(s.cpuMaxCorePct, th.cpuWarnPct, th.cpuCritPct)));
    x += pen.text(x, y1, t.maxCoreClose, 15, kTextMuted);
    x += pen.text(x + 4, y1, num(s.cpuFreqMHz / 1000.0, 2) + "GHz", 16, kText) + 4;
    x += 14;
    x += pen.text(x, y1, "GPU ", 16, kTextMuted);
    pen.text(x, y1, num(s.gpuFreqMHz, 0) + "MHz", 16, kText);

    // 2 行目: 無線。直通回線に相手がいれば「直通」、いなければ家の Wi-Fi の接続先を「Wi-Fi」として出す
    x = kPad;
    const WifiInfo& w = s.wifi;
    const bool direct = w.interfaceUp && w.connected && !w.homeWifi;
    x += pen.text(x, y2, direct ? t.direct : t.homeWifi, 16, kTextMuted);
    x += 4;
    if (!w.interfaceUp || !w.connected) {
        x += drawWifiIcon(pen, x, y2, 0, kTextMuted, true);
        pen.text(x + 6, y2, t.notConnected, 16, kTextMuted);
    } else {
        // 電波の強さはアイコンの本数と色で（dBm の数字は右端に小さく）
        const Level signalLevel = levelOf(w.signalDbm, th.wifiWarnDbm, th.wifiCritDbm, true);
        x += drawWifiIcon(pen, x, y2, wifiBars(w.signalDbm, th), colorOf(signalLevel), false);
        x += 8;
        // 実際に流れている量（相手 → Frame。Steam Link なら映像はこちら）
        const double down = w.rxMbps;
        x += drawArrow(pen, x, y2, false, kTextMuted);
        x += pen.text(x, y2, num(down, std::isnan(down) || down >= 100 ? 0 : 1), 17, kText, true);
        x += 4;
        x += drawArrow(pen, x, y2, true, kTextMuted);
        x += pen.text(x, y2, num(w.txMbps, 1) + " Mbps", 14, kTextMuted);
        x += 12;
        pen.text(x, y2, t.linkRate + num(w.txLinkMbps, 0) + "/" + num(w.rxLinkMbps, 0), 14, kTextMuted);
        std::string signal = std::isnan(w.signalDbm) ? "--" : num(std::fabs(w.signalDbm), 0);
        if (!std::isnan(w.signalDbm) && w.signalDbm < 0) signal = "−" + signal;
        pen.text(kWidth - kPad, y2, signal + "dBm", 13, colorOf(signalLevel, kTextMuted), false, true);
    }

    // 3 行目: 本体の電池・コントローラー・メモリ
    x = kPad;
    x += pen.text(x, y3, t.battery, 16, kTextMuted);
    const Level batteryLevel = levelOf(s.batteryPct, th.batteryWarnPct, th.batteryCritPct, true);
    x += pen.text(x, y3, num(s.batteryPct, 0) + "%", 17, colorOf(batteryLevel), true);
    x += 6;
    x += pen.text(x, y3, batteryStatusText(config.language, s.batteryStatus, s.chargerOnline), 15, kTextMuted);
    x += 16;
    const ControllerStatus& c = state.controllers;
    if (!c.left.present && !c.right.present) {
        x += pen.text(x, y3, t.noControllers, 15, kTextMuted);
    } else {
        x += drawControllerBattery(pen, x, y3, t.leftHand, c.left, th);
        x += 10;
        x += drawControllerBattery(pen, x, y3, t.rightHand, c.right, th);
    }
    // メモリは右端にそろえる
    const double right = kWidth - kPad;
    const double memWidth = pen.text(right, y3, num(s.memUsedGiB, 1) + "/" + num(s.memTotalGiB, 0) + "GB", 15,
                                     kText, false, true);
    pen.text(right - memWidth - 4, y3, t.memory, 15, kTextMuted, false, true);
}

}  // namespace

void PanelState::update(double time, const SensorSample& sample, const FrameStats& frameStats, bool connected,
                        double keepSeconds) {
    now = time;
    sensors = sample;
    frame = frameStats;
    vrConnected = connected;
    const double nan = kNoValue;
    appFps.push(time, frameStats.valid ? frameStats.appFps : nan, keepSeconds);
    powerW.push(time, sample.powerMainW, keepSeconds);
    cpuTempC.push(time, sample.cpuTempC, keepSeconds);
    gpuTempC.push(time, sample.gpuTempC, keepSeconds);
    gpuBusyPct.push(time, sample.gpuBusyPct, keepSeconds);
}

PanelRenderer::PanelRenderer(const FontSet& fonts) : fonts_(fonts), visibleHeight_(kHeight) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    staticLayers_[0] = buildStaticLayer(false);
    staticLayers_[1] = buildStaticLayer(true);
    // VR では画素の並びが一定でないのでサブピクセルは使わない
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
}

PanelRenderer::~PanelRenderer() {
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
    for (cairo_surface_t* layer : staticLayers_) {
        if (layer != nullptr) cairo_surface_destroy(layer);
    }
}

cairo_surface_t* PanelRenderer::buildStaticLayer(bool clock) const {
    // 動かない部分（地・段のカード・グラフの台）は 1 回だけ描いて、毎回それを貼る（影つきのカードを毎回描かない）。
    // 時計を出さないときは地と下のカードを 1 行ぶん短くし、その下は透明のまま
    const int height = heightFor(clock);
    const int textRows = kTextRows + (clock ? 1 : 0);
    cairo_surface_t* layer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cairo_t* cr = cairo_create(layer);
    const Pen pen {cr, &fonts_};
    // 地（不透明。コントラスト比は不透明な地で計算している。全体の透けは設定の alpha でオーバーレイごと変える）
    pen.color(kBg);
    pen.roundedRect(0, 0, kWidth, height, 16);
    cairo_fill(cr);
    const double cardW = kWidth - 2 * kCardInsetX;
    for (int i = 0; i < 3; ++i) {
        const double top = kPad + i * (kRowHeight + kRowGap);
        drawCard(pen, kCardInsetX, top - kCardAbove, cardW, kRowHeight + kCardAbove + kCardBelow, 12, kCard, kCard, 0);
        // グラフの台（地の色で一段沈める）
        pen.color(kBg);
        pen.roundedRect(kGraphX, top + 4, kGraphWidth, kRowHeight - 8, 6);
        cairo_fill(cr);
    }
    drawCard(pen, kCardInsetX, kTextTop - kCardAbove, cardW, textRows * kTextLineHeight + kCardAbove + kCardBelow, 12,
             kCard, kCard, 0);
    cairo_destroy(cr);
    cairo_surface_flush(layer);
    return layer;
}

void PanelRenderer::render(const PanelState& state, const Config& config) {
    const Pen pen {cr_, &fonts_};
    const bool clock = config.clockFormat != 0;
    visibleHeight_ = heightFor(clock);

    // 先に描いておいた動かない部分をそのまま貼る（角の外と、時計を出さないときの下の 1 行ぶんは透明のまま）
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr_, staticLayers_[clock ? 1 : 0], 0, 0);
    cairo_paint(cr_);
    cairo_restore(cr_);
    if (state.sensors.throttleCpu || state.sensors.throttleGpu) {
        // 熱で制限がかかっている間は、パネル全体を赤い枠で囲んで目立たせる
        pen.color(kDanger);
        cairo_set_line_width(cr_, 4.0);
        pen.roundedRect(2, 2, kWidth - 4, visibleHeight_ - 4, 15);
        cairo_stroke(cr_);
    }

    double top = kPad;
    drawFrameRow(pen, top, state, config);
    top += kRowHeight + kRowGap;
    drawPowerRow(pen, top, state, config);
    top += kRowHeight + kRowGap;
    drawTempRow(pen, top, state, config);
    drawTextRows(pen, kTextTop, state, config);
    // 4 行目: 時計（本体の時刻。更新間隔ごとに描き直す）
    if (clock) {
        const double y = kTextTop + kTextRows * kTextLineHeight + 19;
        pen.text(kPad, y, uiText(config.language).clockLabel, 16, kTextMuted);
        pen.text(kWidth - kPad, y, localClock(config.clockFormat), 21, kText, true, true);
    }

    cairo_surface_flush(surface_);
}

const std::vector<uint8_t>& PanelRenderer::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool PanelRenderer::writePng(const std::string& path) const {
    // 見えている部分（時計を出さないときは上の 434）だけを書き出す
    cairo_surface_t* visible = cairo_surface_create_for_rectangle(surface_, 0, 0, kWidth, visibleHeight_);
    const bool ok = cairo_surface_write_to_png(visible, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(visible);
    return ok;
}

int PanelRenderer::width() const {
    return kWidth;
}

int PanelRenderer::height() const {
    return kHeight;
}

int PanelRenderer::heightFor(bool clock) {
    return clock ? kHeight : kHeightNoClock;
}
