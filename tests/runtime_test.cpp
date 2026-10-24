// 機器基準の変換を手首の確認のたびではなく設定の反映時に作るようにした変更の回帰テスト。
#include "placement.h"
#include "vr_overlay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

// 作り置きにする前の頭固定の計算。float の全成分が完全に一致するか比べるために残す
vr::HmdMatrix34_t headReference(const Config& config) {
    double rotation[3][3];
    panelRotation(config, rotation);
    const double position[3] = {config.posX, config.posY, config.posZ};
    vr::HmdMatrix34_t transform {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) transform.m[row][col] = static_cast<float>(rotation[row][col]);
        transform.m[row][3] = static_cast<float>(position[row]);
    }
    return transform;
}

void equal(const vr::HmdMatrix34_t& actual, const vr::HmdMatrix34_t& expected) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            check(actual.m[row][col] == expected.m[row][col], "attachment transform changed");
        }
    }
}

void attachmentTransforms() {
    Config config;
    config.posX = -0.41;
    config.posY = 0.23;
    config.posZ = -1.37;
    config.leftWrist.x = -0.11;
    config.leftWrist.y = 0.22;
    config.leftWrist.z = 0.33;
    config.rightWrist.x = 0.44;
    config.rightWrist.y = -0.35;
    config.rightWrist.z = -0.26;
    for (int yaw = -180; yaw <= 180; yaw += 30) {
        for (int pitch = -90; pitch <= 90; pitch += 15) {
            for (int roll = -180; roll <= 180; roll += 30) {
                config.yawDeg = yaw;
                config.pitchDeg = pitch;
                config.rollDeg = roll;
                config.leftWrist.yaw = yaw;
                config.leftWrist.pitch = pitch - 90;
                config.leftWrist.roll = roll;
                config.rightWrist.yaw = -yaw;
                config.rightWrist.pitch = -pitch - 90;
                config.rightWrist.roll = -roll;
                config.attachment = Attachment::Head;
                equal(attachmentTransform(config), headReference(config));
                config.attachment = Attachment::LeftWrist;
                equal(attachmentTransform(config), wristTransform(config.leftWrist));
                config.attachment = Attachment::RightWrist;
                equal(attachmentTransform(config), wristTransform(config.rightWrist));
            }
        }
    }
}

void eventReuse() {
    VrEvents events;
    events.quit = events.closeRequested = true;
    events.pointer.push_back({PointerInput::Type::Move, 10, 20});
    events.pointer.push_back({PointerInput::Type::Down, 30, 40});
    const auto* storage = events.pointer.data();
    const size_t capacity = events.pointer.capacity();
    events.clear();
    check(!events.quit && !events.closeRequested && events.pointer.empty(), "old events were retained");
    check(events.pointer.capacity() == capacity, "event storage was discarded");
    events.pointer.push_back({PointerInput::Type::Up, 50, 60});
    check(events.pointer.data() == storage, "event storage was not reused");
    check(events.pointer[0].type == PointerInput::Type::Up && events.pointer[0].x == 50 && events.pointer[0].y == 60,
          "new event data was lost");
}

// GPU 負荷の目安（1 枚あたりの GPU 時間 × 毎秒の枚数）。値は作者の Frame で測ったもの
void gpuBusyFromFrames() {
    const auto near = [](double actual, double expected) { return std::fabs(actual - expected) < 1e-9; };
    check(near(estimateGpuBusyPct(16.5, 58.6), 96.69), "Half-Life: Alyx at 72Hz should be about 97%");
    check(near(estimateGpuBusyPct(3.0, 90.0), 27.0), "SteamVR Home at 90Hz should be 27%");
    check(near(estimateGpuBusyPct(0.0, 90.0), 0.0), "no GPU work should be 0%");
    check(near(estimateGpuBusyPct(16.0, 72.0), 100.0), "overlapping GPU work should stop at 100%");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check(std::isnan(estimateGpuBusyPct(nan, 90.0)) && std::isnan(estimateGpuBusyPct(3.0, nan)),
          "missing frame timing should be unavailable");
    check(std::isnan(FrameStats().gpuBusyPct), "GPU load should start unavailable");
}

// The panel counts as shown only once OpenVR took the "show": hidden, faded out and alpha 0 are all "not shown"
// (the sensors read only while it is)
void panelShown() {
    const auto accept = [](auto) { return true; };
    {
        PanelPresentationState state;
        check(!state.shown(), "a new panel should not count as shown");
        check(!state.apply(0.9, accept, accept) && state.shown(), "an accepted show should be shown");
        check(!state.apply(0.0, accept, accept) && !state.shown(), "alpha 0 should hide the panel");
        check(!state.apply(0.5, accept, accept) && state.shown(), "alpha back up should show it again");
    }
    {
        PanelPresentationState state;  // alpha 0 from the config
        check(!state.apply(0.0, accept, accept) && !state.shown(), "alpha 0 from the config should not be shown");
    }
    {
        PanelPresentationState state;  // OpenVR refuses the show: not shown, and the show is retried
        bool allow = false;
        const auto setVisible = [&allow](bool) { return allow; };
        check(state.apply(0.9, accept, setVisible) && !state.shown(), "a refused show should not count as shown");
        allow = true;
        check(!state.apply(0.9, accept, setVisible) && state.shown(), "the retried show should be shown");
    }
    {
        PanelPresentationState state;  // OpenVR refuses the alpha: the show waits for the next call
        bool allow = false;
        const auto setAlpha = [&allow](double) { return allow; };
        check(state.apply(0.9, setAlpha, accept) && !state.shown(), "a refused alpha should hold the show back");
        allow = true;
        check(!state.apply(0.9, setAlpha, accept) && state.shown(), "the retried alpha should show the panel");
    }
}

}  // namespace

int main() {
    attachmentTransforms();
    eventReuse();
    gpuBusyFromFrames();
    panelShown();
    std::puts("runtime transform/event/GPU load tests passed");
}
