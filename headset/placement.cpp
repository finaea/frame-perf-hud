// 手首に固定するときの計算の実装。
#include "placement.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kRadians = M_PI / 180.0;
constexpr double kFadeBandDeg = 30.0;    ///< 薄くなり始めてから消えきるまでの角度
constexpr double kFadeTimeConstant = 0.08;  ///< フェードの時定数（秒）

}  // namespace

vr::HmdMatrix34_t wristTransform(const WristPose& pose) {
    // 基準（手首の上に寝かせた向き）からの回転。頭のパネルと同じ関数で、pitch は基準からの角度（+90）にする
    double r[3][3];
    panelRotation(pose.yaw, pose.pitch + 90.0, pose.roll, r);
    // 基準 = Rx(−90°) = [1 0 0; 0 0 1; 0 −1 0]。左から掛けると 2 行目は r の 3 行目、3 行目は r の 2 行目の符号反転
    vr::HmdMatrix34_t m {};
    for (int col = 0; col < 3; ++col) {
        m.m[0][col] = static_cast<float>(r[0][col]);
        m.m[1][col] = static_cast<float>(r[2][col]);
        m.m[2][col] = static_cast<float>(-r[1][col]);
    }
    m.m[0][3] = static_cast<float>(pose.x);
    m.m[1][3] = static_cast<float>(pose.y);
    m.m[2][3] = static_cast<float>(pose.z);
    return m;
}

vr::HmdMatrix34_t composeTransform(const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& local) {
    vr::HmdMatrix34_t out {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            for (int k = 0; k < 3; ++k) out.m[row][col] += parent.m[row][k] * local.m[k][col];
        }
        out.m[row][3] += parent.m[row][3];
    }
    return out;
}

double wristFacingAlpha(const vr::HmdMatrix34_t& panel, const vr::HmdMatrix34_t& head, double fadeEndDeg) {
    double toHead[3];
    double length = 0;
    for (int i = 0; i < 3; ++i) {
        toHead[i] = head.m[i][3] - panel.m[i][3];
        length += toHead[i] * toHead[i];
    }
    if (!std::isfinite(length) || length < 1e-8) return 0;
    length = std::sqrt(length);
    // 面の向き（パネルの +z 軸 = 3 列目）と、頭への向きの内積
    double facing = 0;
    for (int i = 0; i < 3; ++i) facing += panel.m[i][2] * toHead[i] / length;
    if (!std::isfinite(facing)) return 0;
    const double angle = std::acos(std::clamp(facing, -1.0, 1.0)) / kRadians;
    const double end = std::clamp(fadeEndDeg, 35.0, 90.0);
    const double t = std::clamp((end - angle) / kFadeBandDeg, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double smoothWristAlpha(double current, double target, double elapsed) {
    // 時定数 80ms で近づける（フェードの範囲を一気にまたいでも急に変わらない）
    return target + (current - target) * std::exp(-std::max(0.0, elapsed) / kFadeTimeConstant);
}

bool wristTrackingValid(uint32_t device, const vr::TrackedDevicePose_t (&poses)[vr::k_unMaxTrackedDeviceCount]) {
    if (device >= vr::k_unMaxTrackedDeviceCount) return false;
    const vr::TrackedDevicePose_t& head = poses[vr::k_unTrackedDeviceIndex_Hmd];
    return head.bDeviceIsConnected && head.bPoseIsValid && poses[device].bDeviceIsConnected &&
           poses[device].bPoseIsValid;
}

double wristPollInterval(double alpha, double target) {
    const bool changing = std::fabs(alpha - target) > 0.01;
    const bool inBand = target > 0.001 && target < 0.999;
    return changing || inBand ? kWristPollFadeSec : kWristPollSteadySec;
}

double WristFadeState::update(bool wrist, bool active, uint32_t device, double target, double now) {
    const bool start = !active_ || device != device_ || lastTime_ < 0;
    const double elapsed = start ? 0.0 : std::clamp(now - lastTime_, 0.0, kWristFadeMaxStepSec);
    lastTime_ = now;
    device_ = device;
    active_ = active;
    // 見せられないときはすぐ 0（トラッキングが外れたパネルを宙に残さない）。戻ったときも 0 からフェードする
    if (!active || start) alpha_ = 0.0;
    if (active) alpha_ = wrist ? smoothWristAlpha(alpha_, target, elapsed) : 1.0;
    return alpha_;
}
