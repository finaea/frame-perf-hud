// OpenVR との接続とオーバーレイの実装。
#include "vr_overlay.h"

#include "detached.h"  // T05
#include "openvr.h"
#include "placement.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {

constexpr const char* kOverlayKey = "finaea.frame-perf-hud";
constexpr const char* kOverlayName = "Frame Perf HUD";
constexpr const char* kDashboardKey = "finaea.frame-perf-hud.settings";
constexpr const char* kDashboardName = "Frame Perf HUD";
// 設定パネルの 1px あたりの大きさ（m）。1200px を 2.8m で出していたときと同じ（前の 1024px / 2.4m ともほぼ同じ）。
// 幅（m）は画像の幅（px）から決める
constexpr float kDashboardMetersPerPx = 2.8f / 1200.0f;
constexpr uint32_t kMaxTimings = 256;
// 終了時、オーバーレイを消してから VR_Shutdown まで待つ時間（90Hz で約 36 フレーム）
constexpr int kShutdownWaitMs = 400;
// --verbose の向きの確かめを、変換を渡してから何秒後にするか
constexpr double kFacingCheckDelaySec = 0.5;
// 性能パネルの透明度・表示・変換を断られたとき、やり直すまでの秒数
constexpr double kPlacementRetrySec = 0.1;
// 手首に固定しているコントローラーの番号（役割から引く）を聞き直す間隔（秒）
constexpr double kWristRoleCheckSec = 1.0;
// T05: attachedDevice_ while the HUD is placed with SetOverlayTransformAbsolute (no device)
constexpr uint32_t kDetachedDevice = 0xfffffffeu;

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * オーバーレイのエラー名を返す。
 * @param error エラー
 * @return 名前（例: VROverlayError_None）
 */
const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

/**
 * オーバーレイのエラーを、成功以外なら標準エラーに出す。
 * @param what 何をしたときか
 * @param error エラー
 * @return 成功なら true
 */
bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[VR] %s failed: %s\n", what, overlayErrorName(error));
    return false;
}

/**
 * PID からプロセス名（/proc/<pid>/comm）を引く（診断の表示用）。
 * @param pid プロセス ID
 * @return 名前。分からなければ "?"
 */
std::string processName(uint32_t pid) {
    if (pid == 0) return "-";
    const std::string path = "/proc/" + std::to_string(pid) + "/comm";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "?";
    char name[64] = {};
    const ssize_t n = ::read(fd, name, sizeof(name) - 1);
    ::close(fd);
    std::string text(name, n > 0 ? static_cast<size_t>(n) : 0);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

/**
 * 終了処理の 1 手順の結果を、成功でも失敗でもログに出す（あとで順番と結果を確かめるため）。
 * @param what 何をしたか
 * @param error 戻り値
 */
void logShutdownStep(const char* what, vr::EVROverlayError error) {
    std::fprintf(stderr, "[VR] shutdown %s -> %s\n", what, overlayErrorName(error));
}

}  // namespace

VrOverlay::VrOverlay() : timingBuffer_(sizeof(vr::Compositor_FrameTiming) * kMaxTimings) {}

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int panelWidth, int panelHeight, int settingsWidth, int settingsHeight,
                                            std::string& message) {
    if (connected_) return ConnectResult::Ok;

    // 1) Background 型で「SteamVR が動いているか」だけ確かめる（動いていなければ起動させない）
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();

    // 2) オーバーレイ型でつなぎ直す
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;

    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError overlayError = vr::VROverlay()->CreateOverlay(kOverlayKey, kOverlayName, &handle);
    if (overlayError != vr::VROverlayError_None) {
        message = std::string("CreateOverlay: ") + overlayErrorName(overlayError);
        shutdown();
        return ConnectResult::Error;
    }
    panelHandle_ = handle;
    // 置き場所と見え方の状態は、つなぎ直すたびに最初から（新しいオーバーレイには何も渡していない）
    attachedDevice_ = vr::k_unTrackedDeviceIndexInvalid;
    transformDirty_ = true;
    wristFade_ = WristFadeState {};
    presentation_ = PanelPresentationState {};
    nextPlacementAt_ = std::numeric_limits<double>::infinity();
    wristDevice_ = vr::k_unTrackedDeviceIndexInvalid;
    wristDeviceCheckedAt_ = -1.0;
    panelInteractive_ = false;  // T05: a new overlay's input method is None
    appliedDetached_ = DetachedPose {};
    // 新しいオーバーレイはテクスチャの全部を使う（SetOverlayTextureBounds の既定）
    panelTextureHeight_ = panelHeight;
    panelVisibleHeight_ = panelHeight;

    // 3) Vulkan と性能パネルのテクスチャ
    std::string vkMessage;
    panelTexture_ = std::make_unique<OverlayTexture>();
    retiredPanelTexture_.reset();
    panelTextureWidth_ = panelWidth;
    // T04: cairo's premultiplied ARGB32 goes up as it is (B8G8R8A8 + IsPremultiplied), which saves a
    // ~3 ms per-redraw conversion. FPH_RGBA_UPLOAD=1 brings back upstream's non-premultiplied RGBA path
    const char* rgbaEnv = std::getenv("FPH_RGBA_UPLOAD");
    panelBgra_ = rgbaEnv == nullptr || rgbaEnv[0] == '\0' || rgbaEnv[0] == '0';
    checkOverlay("SetOverlayFlag(IsPremultiplied)",
                 vr::VROverlay()->SetOverlayFlag(panelHandle_, vr::VROverlayFlags_IsPremultiplied, panelBgra_));
    std::fprintf(stderr, "[VR] panel upload: %s\n",
                 panelBgra_ ? "premultiplied BGRA (no conversion)" : "RGBA (converted, FPH_RGBA_UPLOAD)");
    if (!vulkan_.init(vkMessage) || !panelTexture_->create(vulkan_, panelWidth, panelHeight, vkMessage, panelBgra_)) {
        message = "Vulkan setup failed: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }

    // 4) ダッシュボードの設定パネル（失敗しても性能パネルは動かす）
    createDashboard(settingsWidth, settingsHeight);
    cameras_ = std::make_unique<CameraOverlays>(vulkan_);  // T10: the camera tiles share this Vulkan device
    placement_ = PanelPlacement {};
    placement_.revision = 1;

    vrserverPid_ = findVrserverPid();
    lastFrameIndex_ = 0;
    haveFrameIndex_ = false;
    hzCheckedAt_ = -1.0;
    lastPanelError_.clear();
    lastSettingsError_.clear();
    return ConnectResult::Ok;
}

bool VrOverlay::connectReadOnly(std::string& message) {
    if (connected_) return true;
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return false;
    }
    if (vr::VRCompositor() == nullptr || vr::VRSystem() == nullptr) {
        message = "VRCompositor / VRSystem not available in Background mode";
        vr::VR_Shutdown();
        return false;
    }
    connected_ = true;
    readOnly_ = true;
    hzCheckedAt_ = -1.0;
    return true;
}

void VrOverlay::createDashboard(int width, int height) {
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError error = overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay -> %s\n", overlayErrorName(error));
    if (error != vr::VROverlayError_None) return;
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;
    settingsHeight_ = height;

    checkOverlay("SetOverlayWidthInMeters(settings)", overlay->SetOverlayWidthInMeters(main, kDashboardMetersPerPx * static_cast<float>(width)));
    checkOverlay("SetOverlayInputMethod(settings)", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    // マウス座標を画像の px にそろえる
    const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
    checkOverlay("SetOverlayMouseScale(settings)", overlay->SetOverlayMouseScale(main, &scale));
    // ダッシュボードの下のアイコンにホバーしたとき「閉じる」を出す。SteamVR の dashboard は
    // このフラグがあるオーバーレイに閉じるボタンを出し、押されると VREvent_OverlayClosed を送ってくる。
    const vr::EVROverlayError closeError =
        overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true);
    // 立ったかどうかを vrserver から読み返して確かめる（ログに残す）
    bool closeEnabled = false;
    const vr::EVROverlayError readError =
        overlay->GetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, &closeEnabled);
    std::fprintf(stderr, "[VR] SetOverlayFlag(EnableControlBarClose) -> %s (read back: %s, %s)\n",
                 overlayErrorName(closeError), overlayErrorName(readError), closeEnabled ? "true" : "false");
    // T05: thumbstick / trackpad scrolling over the tab (VREvent_ScrollSmooth; needs the Mouse input method)
    checkOverlay("SetOverlayFlag(SendVRSmoothScrollEvents)",
                 overlay->SetOverlayFlag(main, vr::VROverlayFlags_SendVRSmoothScrollEvents, true));

    std::string message;
    if (!settingsTexture_.create(vulkan_, width, height, message)) {
        std::fprintf(stderr, "[Vulkan] can't create settings panel texture: %s\n", message.c_str());
    }
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    if (readOnly_) {
        // オーバーレイも Vulkan も作っていないので、閉じるだけ
        vr::VR_Shutdown();
        connected_ = false;
        readOnly_ = false;
        return;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] starting shutdown\n");

    // 1) 性能パネルを隠す
    if (panelHandle_ != 0) logShutdownStep("HideOverlay(panel)", overlay->HideOverlay(panelHandle_));
    // 2) テクスチャを外す（コンポジタがこちらの画像を参照しないようにする）
    if (panelHandle_ != 0) logShutdownStep("ClearOverlayTexture(panel)", overlay->ClearOverlayTexture(panelHandle_));
    if (dashboardHandle_ != 0) {
        logShutdownStep("ClearOverlayTexture(settings)", overlay->ClearOverlayTexture(dashboardHandle_));
        logShutdownStep("ClearOverlayTexture(thumbnail)", overlay->ClearOverlayTexture(thumbnailHandle_));
    }
    // 3) オーバーレイを消す（サムネイルは設定パネルと一緒に消える）
    if (panelHandle_ != 0) logShutdownStep("DestroyOverlay(panel)", overlay->DestroyOverlay(panelHandle_));
    if (cameras_) cameras_->destroyOverlays();  // T10
    if (dashboardHandle_ != 0) logShutdownStep("DestroyOverlay(settings)", overlay->DestroyOverlay(dashboardHandle_));
    panelHandle_ = 0;
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;

    // 4) コンポジタが数フレーム回って、外したテクスチャを手放すのを待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    std::fprintf(stderr, "[VR] shutdown waited %dms\n", kShutdownWaitMs);

    // 5) OpenVR を閉じる（Vulkan の画像を壊すのはこの後、という OpenVR の決まり）
    vr::VR_Shutdown();
    connected_ = false;
    std::fprintf(stderr, "[VR] shutdown VR_Shutdown done\n");

    // 6) Vulkan の画像とデバイスを壊す
    if (cameras_) cameras_->destroyTextures();  // T10
    cameras_.reset();
    thumbnailTexture_.destroy();
    settingsTexture_.destroy();
    if (retiredPanelTexture_) retiredPanelTexture_->destroy();
    if (panelTexture_) panelTexture_->destroy();
    retiredPanelTexture_.reset();
    panelTexture_.reset();
    vulkan_.destroy();
    std::fprintf(stderr, "[VR] shutdown Vulkan cleaned up\n");
}

void VrOverlay::applyConfig(const Config& config) {
    if (!connected_ || panelHandle_ == 0) return;
    // T04: width_m is one column's width, so the text keeps upstream's size whatever the layout
    configWidthM_ = config.widthM;
    checkOverlay("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(
                                                panelHandle_, static_cast<float>(config.widthM * panelColumns_)));
    transformDirty_ = true;
    notePlacement();  // T10: the width moved the camera tiles
    updatePlacement(config, nowSeconds());
    // 変換を渡した直後に交差を聞くと、SteamVR はまだ前の変換で答える（実機で確かめた）ので、少し待ってから聞く。
    // 確かめるのは頭に固定しているときだけ（頭の中心から見た向きを計算するため）
    facingCheckAt_ = 0;
    if (verbose_ && config.attachment == Attachment::Head) {
        facingCheckConfig_ = config;
        facingCheckAt_ = nowSeconds() + kFacingCheckDelaySec;
    }
}

void VrOverlay::setPanelVisibleHeight(int visibleHeight) {
    if (!connected_ || panelHandle_ == 0 || panelTextureHeight_ <= 0) return;
    visibleHeight = std::clamp(visibleHeight, 1, panelTextureHeight_);
    if (visibleHeight == panelVisibleHeight_) return;
    // テクスチャの上から visibleHeight px だけを使う（Vulkan のテクスチャは v = 0 が上の行）
    const vr::VRTextureBounds_t bounds {0.0f, 0.0f, 1.0f,
                                        static_cast<float>(visibleHeight) / static_cast<float>(panelTextureHeight_)};
    if (checkOverlay("SetOverlayTextureBounds", vr::VROverlay()->SetOverlayTextureBounds(panelHandle_, &bounds))) {
        panelVisibleHeight_ = visibleHeight;
        notePlacement();  // T10
    }
}

uint32_t VrOverlay::wristDeviceIndex(bool left, double now) {
    // 役割（左手・右手）の番号を聞くのは IPC なので、毎回は聞かない。手を替えたときと 1 秒ごと（コントローラーが
    // つなぎ直されて番号が変わっても 1 秒以内に追いつく）だけ聞き直す。隠すのは姿勢で決めるので、ここは待たない
    if (left != wristDeviceLeft_ || wristDeviceCheckedAt_ < 0 || now - wristDeviceCheckedAt_ >= kWristRoleCheckSec) {
        wristDevice_ = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
            left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        wristDeviceLeft_ = left;
        wristDeviceCheckedAt_ = now;
    }
    return wristDevice_;
}

void VrOverlay::updatePlacement(const Config& config, double now) {
    if (!connected_ || panelHandle_ == 0) {
        nextPlacementAt_ = std::numeric_limits<double>::infinity();
        return;
    }
    vr::IVROverlay* overlay = vr::VROverlay();
    // T05: Detached = a fixed standing-space pose. Without one (never placed), it stays hidden until it's placed
    if (config.attachment == Attachment::Detached) {
        bool shown = config.visible && config.detachedPose.valid;
        bool retry = false;
        if (shown && (transformDirty_ || attachedDevice_ != kDetachedDevice || appliedDetached_ != config.detachedPose)) {
            const vr::HmdMatrix34_t m = toHmdMatrix(config.detachedPose);
            if (checkOverlay("SetOverlayTransformAbsolute",
                             overlay->SetOverlayTransformAbsolute(panelHandle_, vr::TrackingUniverseStanding, &m))) {
                attachedDevice_ = kDetachedDevice;
                appliedDetached_ = config.detachedPose;
                appliedTransform_ = m;  // T10
                transformDirty_ = false;
            } else {
                shown = false;
                retry = true;
            }
        }
        const double alpha = wristFade_.update(false, shown, kDetachedDevice, 1.0, now);
        retry |= presentation_.apply(
            config.alpha * alpha,
            [&](double value) {
                return checkOverlay("SetOverlayAlpha", overlay->SetOverlayAlpha(panelHandle_, static_cast<float>(value)));
            },
            [&](bool show) {
                return checkOverlay(show ? "ShowOverlay" : "HideOverlay",
                                    show ? overlay->ShowOverlay(panelHandle_) : overlay->HideOverlay(panelHandle_));
            });
        nextPlacementAt_ = retry ? now + kPlacementRetrySec : std::numeric_limits<double>::infinity();
        notePlacement();  // T10
        return;
    }
    const bool wrist = config.attachment != Attachment::Head;
    vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndex_Hmd;
    bool tracked = true;
    double target = 1.0;
    vr::HmdMatrix34_t transform {};
    if (wrist) {
        // コントローラー基準の変換。見せる割合は、コントローラーと頭の今の姿勢から、面が目の方を向いている度合いで決める
        device = wristDeviceIndex(config.attachment == Attachment::LeftWrist, now);
        transform = wristTransform(selectedWrist(config));
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] {};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                        vr::k_unMaxTrackedDeviceCount);
        tracked = wristTrackingValid(device, poses);
        if (tracked && config.wristFade) {
            target = wristFacingAlpha(composeTransform(poses[device].mDeviceToAbsoluteTracking, transform),
                                      poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
                                      config.wristFadeEndDeg);
        }
    } else {
        // HMD 基準の変換 = 位置 × 回転（3×4 の左 3 列が回転、右端の列が位置）。
        // 回転はパネル自身の軸で yaw → pitch → roll の順（panelRotation のコメントを参照）。
        // オーバーレイは +z 側が表。回転なしなら表は頭の方（HMD の +z）を向き、パネルは顔の正面と平行になる
        double rotation[3][3];
        panelRotation(config, rotation);
        const double position[3] = {config.posX, config.posY, config.posZ};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) transform.m[row][col] = static_cast<float>(rotation[row][col]);
            transform.m[row][3] = static_cast<float>(position[row]);
        }
    }
    bool retry = false;
    if (tracked && config.visible && (transformDirty_ || attachedDevice_ != device)) {
        const vr::EVROverlayError error = overlay->SetOverlayTransformTrackedDeviceRelative(panelHandle_, device, &transform);
        if (checkOverlay("SetOverlayTransformTrackedDeviceRelative", error)) {
            attachedDevice_ = device;
            appliedTransform_ = transform;  // T10
            transformDirty_ = false;
        } else {
            // 置き場所を渡せなかったパネルは出さない（前の場所に残さない）。次の呼び出しでやり直す
            tracked = false;
            retry = true;
        }
    }
    // トラッキングが外れたら、フェードを待たずにすぐ隠す（止まったコントローラーの位置にパネルを浮かせたままにしない）
    const double shown = wristFade_.update(wrist, tracked && config.visible, device, target, now);
    retry |= presentation_.apply(
        config.alpha * shown,
        [&](double value) {
            return checkOverlay("SetOverlayAlpha", overlay->SetOverlayAlpha(panelHandle_, static_cast<float>(value)));
        },
        [&](bool show) {
            return checkOverlay(show ? "ShowOverlay" : "HideOverlay",
                                show ? overlay->ShowOverlay(panelHandle_) : overlay->HideOverlay(panelHandle_));
        });
    // 次に確かめる時刻: 断られた操作があれば少し後にやり直す。手首を出している間はフェードの途中なら細かく、
    // 見えきっている・消えきっているなら粗く（トラッキングが外れたのに気づくのもこの間隔）。頭で問題なければ呼ばない
    double interval = std::numeric_limits<double>::infinity();
    if (wrist && config.visible) interval = wristPollInterval(shown, tracked ? target : 0.0);
    if (retry) interval = std::min(interval, kPlacementRetrySec);
    nextPlacementAt_ = now + interval;
    notePlacement();  // T10
}

void VrOverlay::logFacingCheck(const Config& config) const {
    vr::TrackedDevicePose_t pose {};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
    if (!pose.bPoseIsValid) {
        std::fprintf(stderr, "[facing] no HMD pose, can't check\n");
        return;
    }
    const vr::HmdMatrix34_t& hmd = pose.mDeviceToAbsoluteTracking;
    double r[3][3];
    panelRotation(config, r);
    const double pos[3] = {config.posX, config.posY, config.posZ};
    const double distance = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
    std::fprintf(stderr,
                 "[facing] position (%.3f, %.3f, %.3f) yaw %.1f pitch %.1f roll %.1f | computed front normal (%.3f, %.3f, %.3f), "
                 "dot with direction to head %.4f | computed panel +x axis (%.3f, %.3f, %.3f)\n",
                 pos[0], pos[1], pos[2], config.yawDeg, config.pitchDeg, config.rollDeg, r[0][2], r[1][2], r[2][2],
                 distance > 0 ? -(r[0][2] * pos[0] + r[1][2] * pos[1] + r[2][2] * pos[2]) / distance : 0.0, r[0][0],
                 r[1][0], r[2][0]);

    // 狙う点（HMD 基準）: 中心、パネル自身の +x に 2cm、+y に 2cm
    const char* names[3] = {"center", "panel +x 2cm", "panel +y 2cm"};
    const double offsets[3][2] = {{0.0, 0.0}, {0.02, 0.0}, {0.0, 0.02}};
    for (int i = 0; i < 3; ++i) {
        double target[3];
        for (int k = 0; k < 3; ++k) target[k] = pos[k] + r[k][0] * offsets[i][0] + r[k][1] * offsets[i][1];
        const double length = std::sqrt(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
        if (length <= 0) continue;
        // HMD 基準 → 世界（Standing）: 向きは回転だけ、始点は HMD の位置
        vr::VROverlayIntersectionParams_t params {};
        for (int k = 0; k < 3; ++k) {
            params.vSource.v[k] = hmd.m[k][3];
            params.vDirection.v[k] = static_cast<float>(
                (hmd.m[k][0] * target[0] + hmd.m[k][1] * target[1] + hmd.m[k][2] * target[2]) / length);
        }
        params.eOrigin = vr::TrackingUniverseStanding;
        vr::VROverlayIntersectionResults_t hit {};
        if (!vr::VROverlay()->ComputeOverlayIntersection(panelHandle_, &params, &hit)) {
            std::fprintf(stderr, "[facing] %s: no hit\n", names[i]);
            continue;
        }
        // UV は u が右へ、v が上へ増える。どちらも幅で割った値（縦も幅で割る。実機で確かめた）。
        // 法線を HMD 基準に戻す（回転の転置を掛ける）
        double normal[3];
        for (int k = 0; k < 3; ++k) {
            normal[k] = hmd.m[0][k] * hit.vNormal.v[0] + hmd.m[1][k] * hit.vNormal.v[1] + hmd.m[2][k] * hit.vNormal.v[2];
        }
        const double dotComputed = normal[0] * r[0][2] + normal[1] * r[1][2] + normal[2] * r[2][2];
        std::fprintf(stderr,
                     "[facing] %s: UV (%.3f, %.3f) (expected %.3f, %.3f) distance %.3fm (expected %.3f) "
                     "SteamVR normal (HMD space) (%.3f, %.3f, %.3f), dot with computed front normal %.4f\n",
                     names[i], hit.vUVs.v[0], hit.vUVs.v[1], 0.5 + offsets[i][0] / config.widthM,
                     0.5 + offsets[i][1] / config.widthM, hit.fDistance, length, normal[0], normal[1], normal[2], dotComputed);
    }
}

VrEvents VrOverlay::pollEvents(bool includeSystem) {
    VrEvents result;
    if (!connected_) return result;
    vr::VREvent_t event {};
    if (includeSystem) {
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) result.quit = true;
            if (event.eventType == vr::VREvent_SceneApplicationChanged) result.sceneAppChanged = true;
            if (event.eventType == vr::VREvent_DashboardDeactivated) result.dashboardClosed = true;  // T05
        }
    }
    // T05: the HUD's own events. At the input rate while it takes mouse input (a Detached grab), else on the tick
    if (includeSystem || panelInteractive_) {
        while (vr::VROverlay()->PollNextOverlayEvent(panelHandle_, &event, sizeof(event))) {
            if (event.eventType == vr::VREvent_ImageFailed) std::fprintf(stderr, "[VR] image load failed\n");
            const bool down = event.eventType == vr::VREvent_MouseButtonDown;
            if ((down || event.eventType == vr::VREvent_MouseButtonUp) && event.data.mouse.button == vr::VRMouseButton_Left) {
                // The laser's controller. Some events may not name it: fall back to the dashboard's pointing device
                uint32_t device = event.trackedDeviceIndex;
                if (device == vr::k_unTrackedDeviceIndexInvalid || device == vr::k_unTrackedDeviceIndex_Hmd) {
                    device = vr::VROverlay()->GetPrimaryDashboardDevice();
                }
                std::fprintf(stderr, "[detached] laser %s on the HUD (event device %u, using %u)\n",
                             down ? "press" : "release", event.trackedDeviceIndex, device);
                result.grab.push_back({down, device});
            }
        }
    }
    if (dashboardHandle_ != 0) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // マウス座標は左下が原点なので、上が原点になるよう反転する
            const double x = event.data.mouse.x;
            const double y = settingsHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Down, x, y});
                    }
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left) {
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    }
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                case vr::VREvent_ScrollSmooth: {  // T05
                    PointerInput scroll {PointerInput::Type::Scroll, 0, 0};
                    scroll.scrollDy = event.data.scroll.ydelta;
                    result.pointer.push_back(scroll);
                    break;
                }
                // ダッシュボードのアイコンにホバーしたときの「閉じる」（VROverlayFlags_EnableControlBarClose）。
                // フラグを立てたこのオーバーレイにだけ VREvent_OverlayClosed が届く。
                // SteamVR 自体の終了（VRSystem 側の VREvent_Quit）とは別のイベント
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] dashboard \"Close\" pressed\n");
                    result.closeRequested = true;
                    break;
                default: break;
            }
        }
    }
    if (includeSystem && facingCheckAt_ > 0 && nowSeconds() >= facingCheckAt_) {
        facingCheckAt_ = 0;
        logFacingCheck(facingCheckConfig_);
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] got quit event from SteamVR\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    return result;
}

bool VrOverlay::steamVrAlive() const {
    if (vrserverPid_ <= 0) return true;  // 確かめられないときは生きている扱い
    return ::kill(vrserverPid_, 0) == 0 || errno == EPERM;
}

bool VrOverlay::dashboardOpen() const {
    return connected_ && !readOnly_ && vr::VROverlay()->IsDashboardVisible();
}

int VrOverlay::hmdActivityLevel() const {
    if (!connected_) return vr::k_EDeviceActivityLevel_Unknown;
    return vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
}

SettingsVisibility VrOverlay::settingsVisibility() const {
    SettingsVisibility result;
    if (!connected_ || readOnly_ || dashboardHandle_ == 0) return result;
    vr::IVROverlay* overlay = vr::VROverlay();
    result.dashboardVisible = overlay->IsDashboardVisible();
    // ダッシュボードが閉じていれば残りは聞かない（IPC を減らす）
    if (!result.dashboardVisible) return result;
    result.activeTab = overlay->IsActiveDashboardOverlay(dashboardHandle_);
    result.overlayVisible = overlay->IsOverlayVisible(dashboardHandle_);
    return result;
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[Vulkan] can't create thumbnail texture: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] can't submit thumbnail: %s\n", message.c_str());
        return false;
    }
    return true;
}

double VrOverlay::displayHz(double now) {
    if (hzCheckedAt_ < 0 || now - hzCheckedAt_ > 5.0) {
        hzCheckedAt_ = now;
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                        vr::Prop_DisplayFrequency_Float, &error);
        displayHz_ = (error == vr::TrackedProp_Success && hz > 1.0f) ? hz : 0.0;
    }
    return displayHz_;
}

namespace {

/** 直近 1 秒ぶんのフレームタイミングから数えた値。 */
struct WindowStats {
    double fps = std::numeric_limits<double>::quiet_NaN();  ///< アプリの新しく表示されたフレーム（毎秒）
    double reprojectedPct = 0.0;  ///< 表示回数のうち、同じフレームの 2 回目以降（再投影で埋めたコマ）の %
    uint32_t dropped = 0;         ///< m_nNumDroppedFrames の合計
    int throttledFrames = 0;      ///< VRCompositor_ThrottleMask の段数（最新の確定したフレーム）
};

/**
 * 直近 1 秒ぶんのフレームタイミングを数える。1 件 = アプリの新しいフレーム 1 枚（m_nFrameIndex は重ならず、
 * 同じフレームを何回表示したかは m_nNumFramePresents に入る）なので:
 * - fps: 1 回以上表示されたフレームの数を、その時刻（m_flSystemTimeInSeconds、vsync にそろった時刻）の幅で割る
 * - 再投影: （表示回数の合計 − 新しいフレームの数）÷ 表示回数の合計。2 回ずつ表示なら 50%
 * いちばん新しい 1 件はまだ表示回数が決まっていないことがあるので数えない。
 * @param timings 古い順のフレームタイミング
 * @param count 件数
 * @return 数えた値（数えられなければ fps は NaN）
 */
WindowStats measureWindow(const vr::Compositor_FrameTiming* timings, uint32_t count) {
    WindowStats result;
    if (count < 3) return result;
    const uint32_t last = count - 2;  // 最新の 1 件は除く
    const double end = timings[last].m_flSystemTimeInSeconds;
    int shown = 0;
    uint32_t presents = 0;
    double first = end;
    for (int i = static_cast<int>(last); i >= 0; --i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (t.m_flSystemTimeInSeconds <= end - 1.0) break;
        result.dropped += t.m_nNumDroppedFrames;
        if (t.m_nNumFramePresents == 0) continue;  // 一度も表示されなかった（目に届いていない）
        ++shown;
        presents += t.m_nNumFramePresents;
        first = t.m_flSystemTimeInSeconds;
    }
    // 1 秒に 2 枚未満（読み込み中など）は、数えた枚数をそのまま毎秒の値にする
    result.fps = (shown < 2 || end <= first) ? shown : (shown - 1) / (end - first);
    result.reprojectedPct = presents > 0 ? 100.0 * (presents - shown) / presents : 0.0;
    result.throttledFrames = static_cast<int>(VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES(timings[last]));
    return result;
}

}  // namespace

FrameStats VrOverlay::readFrameStats() {
    FrameStats stats;
    if (!connected_) return stats;

    stats.displayHz = displayHz(nowSeconds());
    stats.targetMs = stats.displayHz > 0 ? 1000.0 / stats.displayHz : 0.0;

    // 直近のフレームを古い順にもらい、前回より新しいものだけ集計する
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    std::memset(timings, 0, timingBuffer_.size());
    timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
    const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);

    double gpuSum = 0.0;
    double cpuSum = 0.0;
    uint32_t newest = lastFrameIndex_;
    for (uint32_t i = 0; i < count; ++i) {
        const vr::Compositor_FrameTiming& t = timings[i];
        if (lastFrameIndex_ != 0 && t.m_nFrameIndex <= lastFrameIndex_) continue;
        const double gpu = t.m_flTotalRenderGpuMs;
        // アプリの CPU 時間（姿勢を受け取ってから 2 回目の Submit まで）+ コンポジタの CPU 時間
        const double appCpu = std::max(0.0, static_cast<double>(t.m_flNewFrameReadyMs - t.m_flNewPosesReadyMs));
        const double cpu = appCpu + t.m_flCompositorRenderCpuMs;
        gpuSum += gpu;
        cpuSum += cpu;
        stats.gpuMaxMs = std::max(stats.gpuMaxMs, gpu);
        stats.cpuMaxMs = std::max(stats.cpuMaxMs, cpu);
        ++stats.frames;
        newest = std::max(newest, t.m_nFrameIndex);
    }
    const bool firstRead = !haveFrameIndex_;
    lastFrameIndex_ = newest;
    haveFrameIndex_ = count > 0;
    stats.valid = count > 0;
    if (stats.frames > 0) {
        stats.gpuMs = gpuSum / stats.frames;
        stats.cpuMs = cpuSum / stats.frames;
        stats.haveTimes = true;
    }
    if (count > 0) {
        // fps・再投影・落ち・抑えている段数は、同じ直近 1 秒のフレームタイミングから出す
        // （以前の累積統計 GetCumulativeStats の差分はやめた。数える元をそろえるため）
        const WindowStats window = measureWindow(timings, count);
        // 前回から新しいフレームが 1 つも来ていなければ、アプリは止まっている（0 fps）
        stats.appFps = (!firstRead && stats.frames == 0) ? 0.0 : window.fps;
        stats.reprojectedPct = window.reprojectedPct;
        stats.dropped = window.dropped;
        stats.throttledFrames = window.throttledFrames;
    }
    return stats;
}

namespace {

/**
 * 役割（左手・右手）のコントローラーの電池を読む。
 * @param role 役割
 * @return 電池の状態
 */
ControllerBattery readControllerBattery(vr::ETrackedControllerRole role) {
    ControllerBattery battery;
    vr::IVRSystem* system = vr::VRSystem();
    const vr::TrackedDeviceIndex_t index = system->GetTrackedDeviceIndexForControllerRole(role);
    if (index == vr::k_unTrackedDeviceIndexInvalid || !system->IsTrackedDeviceConnected(index)) return battery;
    battery.present = true;
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    const float fraction = system->GetFloatTrackedDeviceProperty(index, vr::Prop_DeviceBatteryPercentage_Float, &error);
    if (error == vr::TrackedProp_Success) battery.pct = std::clamp(fraction * 100.0, 0.0, 100.0);  // 0〜1 で返る
    error = vr::TrackedProp_Success;
    const bool charging = system->GetBoolTrackedDeviceProperty(index, vr::Prop_DeviceIsCharging_Bool, &error);
    battery.charging = error == vr::TrackedProp_Success && charging;
    return battery;
}

}  // namespace

ControllerStatus VrOverlay::readControllers() {
    ControllerStatus status;
    if (!connected_) return status;
    status.left = readControllerBattery(vr::TrackedControllerRole_LeftHand);
    status.right = readControllerBattery(vr::TrackedControllerRole_RightHand);
    return status;
}

bool VrOverlay::readSceneApp(SceneApp& app) const {
    app = SceneApp();
    if (!connected_ || vr::VRApplications() == nullptr) return false;
    vr::IVRApplications* apps = vr::VRApplications();
    app.pid = apps->GetCurrentSceneProcessId();
    if (app.pid == 0) return true;
    char key[vr::k_unMaxApplicationKeyLength] = {};
    if (apps->GetApplicationKeyByProcessId(app.pid, key, sizeof(key)) == vr::VRApplicationError_None && key[0] != '\0') {
        app.key = key;
        char name[256] = {};
        vr::EVRApplicationError error = vr::VRApplicationError_None;
        apps->GetApplicationPropertyString(key, vr::VRApplicationProperty_Name_String, name, sizeof(name), &error);
        if (error == vr::VRApplicationError_None) app.name = name;
        return true;
    }
    // A scene process SteamVR has no app key for (e.g. a sideloaded app): name it by its process
    const std::string comm = processName(app.pid);
    if (!comm.empty() && comm != "?") {
        app.key = "process." + comm;
        app.name = comm;
    }
    return true;
}

void VrOverlay::dumpFrameTimings(double seconds, const volatile int& stop) {
    if (!connected_) return;
    auto* timings = reinterpret_cast<vr::Compositor_FrameTiming*>(timingBuffer_.data());
    uint32_t last = 0;
    const double start = nowSeconds();
    std::printf("# t(s) index presents mispresented dropped reprojFlags systemTime(s) newFrameReady(ms) gpu(ms)\n");
    while (!stop && nowSeconds() - start < seconds) {
        std::memset(timings, 0, timingBuffer_.size());
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const uint32_t count = vr::VRCompositor()->GetFrameTimings(timings, kMaxTimings);
        const uint32_t focus = vr::VRCompositor()->GetCurrentSceneFocusProcess();
        const uint32_t renderer = vr::VRCompositor()->GetLastFrameRenderer();
        const WindowStats window = measureWindow(timings, count);
        std::printf("# t=%.2f returned=%u display %.1fHz app %.1ffps reprojected %.1f%% dropped %u throttle level %d "
                    "scene_process=%u(%s) last_renderer=%u(%s)\n",
                    nowSeconds() - start, count, displayHz(nowSeconds()), window.fps, window.reprojectedPct,
                    window.dropped, window.throttledFrames, focus, processName(focus).c_str(), renderer,
                    processName(renderer).c_str());
        for (uint32_t i = 0; i < count; ++i) {
            const vr::Compositor_FrameTiming& t = timings[i];
            if (last != 0 && t.m_nFrameIndex <= last) continue;
            if (last == 0 && i + 12 < count) continue;  // 初回は最後の 12 件だけ
            std::printf("%.2f %u %u %u %u 0x%x %.4f %.2f %.2f\n", nowSeconds() - start, t.m_nFrameIndex,
                        t.m_nNumFramePresents, t.m_nNumMisPresented, t.m_nNumDroppedFrames, t.m_nReprojectionFlags,
                        t.m_flSystemTimeInSeconds, t.m_flNewFrameReadyMs, t.m_flTotalRenderGpuMs);
        }
        if (count > 0) last = std::max(last, timings[count - 1].m_nFrameIndex);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool VrOverlay::submitPanel(const uint8_t* rgba, int width, int height, int columns) {
    if (!connected_ || panelHandle_ == 0 || !panelTexture_) return false;
    std::string message;
    // The previous texture has been off-screen for a whole submit interval (≥ 0.5 s): safe to free now
    if (retiredPanelTexture_) {
        retiredPanelTexture_->destroy();
        retiredPanelTexture_.reset();
    }
    if (width != panelTextureWidth_ || height != panelTextureHeight_) {
        auto next = std::make_unique<OverlayTexture>();
        if (!next->create(vulkan_, width, height, message, panelBgra_)) {
            std::fprintf(stderr, "[VR] can't create a %dx%d panel texture: %s\n", width, height, message.c_str());
            return false;
        }
        std::fprintf(stderr, "[VR] panel texture %dx%d -> %dx%d (%d column%s)\n", panelTextureWidth_,
                     panelTextureHeight_, width, height, columns, columns == 1 ? "" : "s");
        retiredPanelTexture_ = std::move(panelTexture_);
        panelTexture_ = std::move(next);
        panelTextureWidth_ = width;
        panelTextureHeight_ = height;
        // The whole new texture is visible (undo upstream's clock-row bounds)
        const vr::VRTextureBounds_t full {0.0f, 0.0f, 1.0f, 1.0f};
        checkOverlay("SetOverlayTextureBounds", vr::VROverlay()->SetOverlayTextureBounds(panelHandle_, &full));
        panelVisibleHeight_ = height;
    }
    if (columns != panelColumns_) {
        panelColumns_ = std::max(1, columns);
        checkOverlay("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(
                                                    panelHandle_, static_cast<float>(configWidthM_ * panelColumns_)));
    }
    notePlacement();  // T10: a new texture size / column count moves the camera tiles
    const bool ok = panelTexture_->update(panelHandle_, rgba, message);
    // 同じエラーを毎回出さない
    if (message != lastPanelError_) {
        if (!ok) std::fprintf(stderr, "[VR] can't submit panel: %s\n", message.c_str());
        lastPanelError_ = message;
    }
    return ok;
}

void VrOverlay::setPanelInteractive(bool interactive) {
    if (!connected_ || panelHandle_ == 0 || interactive == panelInteractive_) return;
    const vr::EVROverlayError error = vr::VROverlay()->SetOverlayInputMethod(
        panelHandle_, interactive ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
    std::fprintf(stderr, "[detached] HUD input method -> %s: %s\n", interactive ? "Mouse" : "None", overlayErrorName(error));
    // Refused: keep the old state, so the next call tries again
    if (error == vr::VROverlayError_None) panelInteractive_ = interactive;
}

bool VrOverlay::devicePose(uint32_t device, vr::HmdMatrix34_t& pose) const {
    if (!connected_ || device >= vr::k_unMaxTrackedDeviceCount) return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] {};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses, device + 1);
    if (!poses[device].bPoseIsValid) return false;
    pose = poses[device].mDeviceToAbsoluteTracking;
    return true;
}

void VrOverlay::setDetachedTransform(const DetachedPose& pose) {
    if (!connected_ || panelHandle_ == 0 || !pose.valid) return;
    const vr::HmdMatrix34_t m = toHmdMatrix(pose);
    if (checkOverlay("SetOverlayTransformAbsolute",
                     vr::VROverlay()->SetOverlayTransformAbsolute(panelHandle_, vr::TrackingUniverseStanding, &m))) {
        attachedDevice_ = kDetachedDevice;
        appliedDetached_ = pose;
        appliedTransform_ = m;  // T10
        notePlacement();
    }
}

void VrOverlay::notePlacement() {
    if (!cameras_) return;
    PanelPlacement p;
    p.valid = attachedDevice_ != vr::k_unTrackedDeviceIndexInvalid;
    p.shown = presentation_.shown();
    p.alpha = static_cast<float>(presentation_.appliedAlpha());
    p.absolute = attachedDevice_ == kDetachedDevice;
    p.device = p.absolute ? 0 : attachedDevice_;
    p.transform = appliedTransform_;
    p.widthM = static_cast<float>(configWidthM_ * panelColumns_);
    p.textureWidth = panelTextureWidth_;
    p.visibleHeight = panelVisibleHeight_;
    const bool same = p.valid == placement_.valid && p.shown == placement_.shown &&
                      std::fabs(p.alpha - placement_.alpha) < 0.002f && p.absolute == placement_.absolute &&
                      p.device == placement_.device && std::memcmp(&p.transform, &placement_.transform, sizeof(p.transform)) == 0 &&
                      p.widthM == placement_.widthM && p.textureWidth == placement_.textureWidth &&
                      p.visibleHeight == placement_.visibleHeight;
    if (same) return;
    const bool samePose = p.valid == placement_.valid && p.absolute == placement_.absolute && p.device == placement_.device &&
                          std::memcmp(&p.transform, &placement_.transform, sizeof(p.transform)) == 0 &&
                          p.widthM == placement_.widthM && p.visibleHeight == placement_.visibleHeight;
    p.revision = placement_.revision + 1;
    p.poseRevision = placement_.poseRevision + (samePose ? 0 : 1);  // a wrist fade only changes alpha
    placement_ = p;
    cameras_->follow(placement_);
}

bool VrOverlay::submitSettings(const uint8_t* rgba) {
    if (!connected_ || dashboardHandle_ == 0 || !settingsTexture_.ready()) return false;
    std::string message;
    const bool ok = settingsTexture_.update(dashboardHandle_, rgba, message);
    if (message != lastSettingsError_) {
        if (!ok) std::fprintf(stderr, "[VR] can't submit settings panel: %s\n", message.c_str());
        lastSettingsError_ = message;
    }
    return ok;
}
