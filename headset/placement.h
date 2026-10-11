// 手首（コントローラー）に固定するときの計算（位置と向き・目から見た角度でのフェード・トラッキングの確かめ）と、
// オーバーレイの透明度・表示の反映を失敗したらやり直すための状態。OpenVR を呼ばない純粋な計算だけを置く（テストしやすいように）。
#pragma once

#include "config.h"
#include "openvr.h"

#include <cmath>
#include <cstdint>

/** フェードの途中（見え方が変わっている間）に手首のパネルを確かめる間隔（秒）。 */
constexpr double kWristPollFadeSec = 1.0 / 30.0;
/** 見えきっている・消えきっているときに手首のパネルを確かめる間隔（秒）。トラッキングが外れたのに気づく速さもこれで決まる。 */
constexpr double kWristPollSteadySec = 0.1;
/** フェードの 1 回の計算で進める時間の上限（秒）。粗い間隔のあとでも、いきなり大きく変わらないようにする。 */
constexpr double kWristFadeMaxStepSec = kWristPollFadeSec;

/**
 * 手首のパネルの、コントローラーから見た変換（3×4。左 3 列が回転、右端の列が位置）を作る。
 * 回転は頭のパネルと同じ panelRotation（パネル自身の軸で yaw → pitch → roll、正で面が右・上・面を見て反時計回り）を、
 * 「手首の上に寝かせた向き」を基準にしてかける: R = Rx(−90°) · panelRotation(yaw, pitch + 90, roll)。
 * pitch −90（既定）で基準そのもの = 面がコントローラーの +y を向き、パネルの上側がコントローラーの −z を向く。
 * 基準のまわりで回すので、既定の向きのまわりで左右・上下・回転のボタンがそれぞれ別の軸で効く（ジンバルロックにならない）。
 * @param pose 手首の位置と向き
 * @return コントローラー基準の変換
 */
vr::HmdMatrix34_t wristTransform(const WristPose& pose);

/**
 * 変換を合成する（parent · local。どちらも 3×4 で、4 行目は (0, 0, 0, 1) とみなす）。
 * @param parent 親の変換（例: コントローラーの姿勢）
 * @param local 親から見た変換（例: wristTransform）
 * @return 合成した変換
 */
vr::HmdMatrix34_t composeTransform(const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& local);

/**
 * パネルの面が目の方を向いている度合いから、見せる割合を決める。面の向き（+z）と「パネルから頭への向き」の角度が
 * fadeEndDeg − 30° までは 1、fadeEndDeg で 0 になり、その間はなめらか（smoothstep）につなぐ。
 * @param panel パネルの変換（世界基準）
 * @param head 頭の姿勢（世界基準）
 * @param fadeEndDeg 消えきる角度（度。35〜90 に収める）
 * @return 見せる割合（0〜1）。頭とパネルが重なっている・値が壊れているときは 0
 */
double wristFacingAlpha(const vr::HmdMatrix34_t& panel, const vr::HmdMatrix34_t& head, double fadeEndDeg);

/**
 * 見せる割合を時間でなめらかに目標へ近づける（時定数 80ms。呼ぶ間隔が変わっても同じ速さになる）。
 * @param current 今の割合
 * @param target 目標の割合
 * @param elapsed 前回からの秒数
 * @return 新しい割合
 */
double smoothWristAlpha(double current, double target, double elapsed);

/**
 * 頭とコントローラーの両方がつながっていて、姿勢が正しく取れているか。
 * @param device コントローラーの番号
 * @param poses すべての機器の姿勢
 * @return 両方取れていれば true
 */
bool wristTrackingValid(uint32_t device, const vr::TrackedDevicePose_t (&poses)[vr::k_unMaxTrackedDeviceCount]);

/**
 * 手首のパネルを次に確かめるまでの秒数を決める。フェードの途中（今の割合が目標からずれている、または目標が 0 と 1 の
 * 間 = 角度がフェードの範囲に入っている）は kWristPollFadeSec、見えきっている・消えきっているときは kWristPollSteadySec。
 * @param alpha 今の見せる割合（0〜1）
 * @param target 目標の割合（0〜1）
 * @return 秒
 */
double wristPollInterval(double alpha, double target);

/**
 * 手首のパネルの見せる割合（フェード）の状態。OpenVR に実際に反映できた値（PanelPresentationState）とは別に持つ。
 */
class WristFadeState {
public:
    /**
     * 見せる割合を 1 回分進める。固定先・コントローラーが変わったとき、トラッキングや表示が戻ったときは 0 から始める。
     * 前回からの時間は kWristFadeMaxStepSec までに抑える（粗い間隔のあとでも、いきなり大きく変わらないように）。
     * @param wrist 手首に固定しているか（頭ならフェードせずにすぐ 1）
     * @param active 見せてよいか（表示がオンで、トラッキングが取れている）。false ならすぐ 0
     * @param device 固定しているコントローラーの番号
     * @param target 目標の割合（0〜1）
     * @param now 今の時刻（秒、単調増加）
     * @return 新しい割合
     */
    double update(bool wrist, bool active, uint32_t device, double target, double now);

    /** @return 最後に計算した割合 */
    double alpha() const { return alpha_; }

private:
    double alpha_ = 0.0;       ///< 今の割合
    double lastTime_ = -1.0;   ///< 前回の時刻（まだなら負）
    uint32_t device_ = vr::k_unTrackedDeviceIndexInvalid;  ///< 前回のコントローラーの番号
    bool active_ = false;      ///< 前回見せてよかったか
};

/**
 * オーバーレイに実際に反映できた透明度と表示・非表示を覚えておき、変わったところだけ OpenVR に渡す係。
 * OpenVR が断った操作は覚えず、次の呼び出しでやり直す（頭のときも同じ）。
 */
class PanelPresentationState {
public:
    /**
     * 透明度と表示・非表示を反映する。
     * 透明度を断られたときは、古い透明度のまま出してしまわないよう、隠れているパネルを出すのは次に回す（隠すのは試す）。
     * @param alpha 反映したい透明度（0 なら隠す）
     * @param setAlpha 透明度を渡す関数（OpenVR が受け付けたら true）
     * @param setVisible 表示（true）・非表示（false）を渡す関数（OpenVR が受け付けたら true）
     * @return やり直しが要る（どれかを断られた）なら true
     */
    template <class SetAlpha, class SetVisible>
    bool apply(double alpha, SetAlpha setAlpha, SetVisible setVisible) {
        bool retry = false;
        if (std::abs(alpha - appliedAlpha_) > 0.001 || (alpha == 0.0 && appliedAlpha_ != 0.0)) {
            if (setAlpha(alpha)) {
                appliedAlpha_ = alpha;
            } else {
                retry = true;
            }
        }
        const bool show = alpha > 0.001;
        if (show != shown_ && (!show || !retry)) {
            if (setVisible(show)) {
                shown_ = show;
            } else {
                retry = true;
            }
        }
        return retry;
    }

private:
    double appliedAlpha_ = -1.0;  ///< 最後に受け付けられた透明度（まだなら負）
    bool shown_ = false;          ///< 最後に受け付けられた表示・非表示
};
