// T10: the camera widgets' settings in the layout (cameras / full_size / fps) and the per-camera headset rate.
#include "camera_frame.h"
#include "layout.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using fph::WidgetType;
using W = fph::WidgetType;

fph::WidgetInstance cam12(uint8_t cameras, bool full, uint8_t fps) {
    fph::WidgetInstance w;
    w.type = WidgetType::HeadsetCamera;
    w.cameras = cameras;
    w.cameraFullSize = full;
    w.cameraFps = fps;
    return w;
}

fph::WidgetInstance cam13(uint8_t fps) {
    fph::WidgetInstance w;
    w.type = WidgetType::EyeCamera;
    w.cameraFps = fps;
    return w;
}

fph::Layout layoutOf(std::vector<fph::WidgetInstance> widgets) {
    fph::Layout l;
    l.version = 7;
    l.columns.push_back({widgets});
    return l;
}

bool parse(const std::string& text, fph::Layout& out, std::vector<std::string>& warnings) {
    std::string error;
    return fph::parseLayoutText(text, out, warnings, error);
}

void testRoundTrip() {
    const fph::Layout l = layoutOf({cam12(0x0B, true, 10), cam13(30), cam12(0x01, false, 15)});
    for (int indent : {0, 2}) {
        fph::Layout back;
        std::vector<std::string> warnings;
        CHECK(parse(fph::layoutToJson(l, indent), back, warnings));
        CHECK(warnings.empty());
        CHECK(back.sameWidgets(l));
        CHECK(back.columns[0].widgets[0].cameras == 0x0B && back.columns[0].widgets[0].cameraFullSize);
        CHECK(back.columns[0].widgets[0].cameraFps == 10);
        CHECK(back.columns[0].widgets[1].cameraFps == 30);
    }
    // the keys are only written for the camera widgets
    fph::WidgetInstance frame;
    frame.type = WidgetType::Frame;
    const std::string json = fph::layoutToJson(layoutOf({frame, cam13(10)}), 0);
    CHECK(json.find("\"fps\":10") != std::string::npos);
    CHECK(json.find("\"cameras\"") == std::string::npos);  // only widget 12 has cameras / full_size
    CHECK(json.find("\"full_size\"") == std::string::npos);
}

void testDefaultsAndValidation() {
    fph::Layout l;
    std::vector<std::string> w;
    // missing keys: defaults (video0, 2x down, 15 fps)
    CHECK(parse(R"({"version":1,"columns":[[{"type":"headset_camera"},{"type":"eye_camera"}]]})", l, w));
    CHECK(w.empty());
    CHECK(l.columns[0].widgets[0].cameras == 0x01 && !l.columns[0].widgets[0].cameraFullSize);
    CHECK(l.columns[0].widgets[0].cameraFps == 15 && l.columns[0].widgets[1].cameraFps == 15);
    // fps snaps to 5 / 10 / 15 / 30 with a warning
    w.clear();
    CHECK(parse(R"({"version":1,"columns":[[{"type":"eye_camera","fps":24},{"type":"eye_camera","fps":1},{"type":"eye_camera","fps":60}]]})",
                l, w));
    CHECK(l.columns[0].widgets[0].cameraFps == 30);
    CHECK(l.columns[0].widgets[1].cameraFps == 5);
    CHECK(l.columns[0].widgets[2].cameraFps == 30);
    CHECK(w.size() == 3);
    // widget 12 offers 5 / 10 / 15 only: a stored 30 snaps to 15 with a warning
    w.clear();
    CHECK(parse(R"({"version":1,"columns":[[{"type":"headset_camera","fps":30}]]})", l, w));
    CHECK(l.columns[0].widgets[0].cameraFps == 15 && w.size() == 1);
    // bad types / masks are warned about and leave the default
    w.clear();
    CHECK(parse(R"({"version":1,"columns":[[{"type":"headset_camera","fps":"fast","cameras":0,"full_size":1}]]})", l, w));
    CHECK(l.columns[0].widgets[0].cameraFps == 15 && l.columns[0].widgets[0].cameras == 0x01);
    CHECK(!l.columns[0].widgets[0].cameraFullSize);
    CHECK(w.size() == 3);
    // fps on a non-camera widget is an unknown key
    w.clear();
    CHECK(parse(R"({"version":1,"columns":[[{"type":"frame","fps":10}]]})", l, w));
    CHECK(w.size() == 1);
}

void testFpsSteps() {
    const W eye = W::EyeCamera, out = W::HeadsetCamera;
    CHECK(fph::cameraFpsStep(15, +1, eye) == 30);
    CHECK(fph::cameraFpsStep(30, +1, eye) == 30);  // clamped
    CHECK(fph::cameraFpsStep(15, -1, eye) == 10);
    CHECK(fph::cameraFpsStep(10, -1, eye) == 5);
    CHECK(fph::cameraFpsStep(5, -1, eye) == 5);
    CHECK(fph::cameraFpsNearest(12.5, eye) == 10);  // a tie goes to the lower one
    CHECK(fph::cameraFpsNearest(13, eye) == 15);
    // widget 12 stops at 15 (the helper reports outward frames every 67 ms)
    CHECK(fph::cameraFpsMax(out) == 15 && fph::cameraFpsMax(eye) == 30);
    CHECK(fph::cameraFpsStep(15, +1, out) == 15);
    CHECK(fph::cameraFpsStep(10, +1, out) == 15);
    CHECK(fph::cameraFpsNearest(30, out) == 15);
    CHECK(fph::cameraFpsStep(30, -1, out) == 10);  // a stray 30 counts as 15
    // a widget's fps is part of its identity (an edit to it is a layout change)
    CHECK(cam12(1, false, 10) != cam12(1, false, 15));
}

void testMaxAcrossWidgets() {
    double fps[fph::cam::kCameraCount];
    // video0 in two widgets at 5 and 15, video3 only in the 5 one, eyes at 30, video9 / 13 not shown
    fph::cam::displayFpsInLayout(layoutOf({cam12(0x03, false, 5), cam12(0x01, false, 15), cam13(30)}), fps);
    CHECK(fps[0] == 15);
    CHECK(fps[1] == 5);
    CHECK(fps[2] == 0 && fps[3] == 0);
    CHECK(fps[4] == 30 && fps[5] == 30);
    fph::cam::displayFpsInLayout(layoutOf({cam12(0x03, false, 10), cam12(0x01, false, 15), cam13(5)}), fps);
    CHECK(fps[0] == 15);
    CHECK(fps[1] == 10);
    CHECK(fps[4] == 5 && fps[5] == 5);
    // two eye widgets: the faster wins
    fph::cam::displayFpsInLayout(layoutOf({cam13(5), cam13(15)}), fps);
    CHECK(fps[4] == 15 && fps[5] == 15 && fps[0] == 0);
    // no camera widgets: nothing
    fph::cam::displayFpsInLayout(fph::Layout(), fps);
    for (double f : fps) CHECK(f == 0);
}

}  // namespace

int main() {
    testRoundTrip();
    testDefaultsAndValidation();
    testFpsSteps();
    testMaxAcrossWidgets();
    if (failures) {
        std::printf("camera-layout-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("camera-layout-test: all passed\n");
    return 0;
}
