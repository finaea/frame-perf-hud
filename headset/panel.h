// cairo でパネル画像を描く。
#pragma once

#include "config.h"
#include "history.h"
#include "sensors.h"

#include <cstdint>
#include <string>
#include <vector>

class FontSet;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/**
 * パネルに出す内容（最新値とグラフ用の履歴）。
 */
struct PanelState {
    SensorSample sensors;
    FrameStats frame;
    bool vrConnected = false;
    double now = 0.0;  ///< 最後に更新した時刻（秒）
    History appFps;
    History powerW;
    History cpuTempC;
    History gpuTempC;
    History gpuBusyPct;
    ControllerStatus controllers;  ///< 左右のコントローラーの電池（VR から 10 秒おきに入れる）

    /**
     * 最新値を入れ替え、グラフ用の履歴に追加する。
     * @param time 現在時刻（秒）
     * @param sample センサーの読み取り結果
     * @param frameStats フレーム時間の集計
     * @param connected SteamVR につながっているか
     * @param keepSeconds 履歴に残す秒数
     */
    void update(double time, const SensorSample& sample, const FrameStats& frameStats, bool connected,
                double keepSeconds);
};

/**
 * 性能パネルの画像を描く係。
 */
class PanelRenderer {
public:
    /**
     * @param fonts 使うフォント（このオブジェクトより長く生きていること）
     */
    explicit PanelRenderer(const FontSet& fonts);
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;

    /**
     * パネルを描く。
     * @param state 表示する内容
     * @param config 設定（しきい値とグラフの秒数を使う）
     */
    void render(const PanelState& state, const Config& config);

    /**
     * 描いた画像を OpenVR 用の非乗算済み RGBA に変換して返す。
     * @return width() * height() * 4 バイト
     */
    const std::vector<uint8_t>& toRgba();

    /**
     * 描いた画像のうち、見えている部分（visibleHeight()）を PNG で保存する。
     * @param path 保存先
     * @return 保存できたら true
     */
    bool writePng(const std::string& path) const;

    /** @return 画像の幅（px） */
    int width() const;
    /** @return 画像（テクスチャ）の高さ（px）。時計を出すときの高さで、いつも同じ */
    int height() const;
    /** @return 最後に描いたときの、見えている部分の高さ（px）。時計を出さないときは時計の 1 行ぶん低い */
    int visibleHeight() const { return visibleHeight_; }

    /**
     * 見えている部分の高さ。
     * @param clock 時計を出すか
     * @return px（時計ありで 460、なしで 434）
     */
    static int heightFor(bool clock);

private:
    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    cairo_surface_t* staticLayers_[2] = {nullptr, nullptr};  ///< 動かない部分（地・段のカード・グラフの台）。[0] 時計なし、[1] 時計あり
    std::vector<uint8_t> rgba_;
    int visibleHeight_ = 0;  ///< 最後に描いたときの、見えている部分の高さ（px）

    /**
     * 動かない部分を描いた画像を作る（起動時に時計あり・なしの 2 枚）。
     * @param clock 時計の行を含めるか
     * @return 画像（呼ぶ側で破棄する）
     */
    cairo_surface_t* buildStaticLayer(bool clock) const;
};
