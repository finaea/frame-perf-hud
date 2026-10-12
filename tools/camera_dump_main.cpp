// T10: fph-camera-dump, the camera thread on its own (headset/camera_dump.h). The agent has the same as
// `frame-perf-hud --camera-dump`; this one exists so the camera path can be measured without the overlay.
//   fph-camera-dump [seconds] [camera mask, hex] [--pc] [--no-display] [--full] [--standby] [--snap DIR]
#include "camera_dump.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    CameraDumpOptions o;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pc") o.pc = true;
        else if (a == "--no-display") o.display = false;
        else if (a == "--full") o.fullSize = true;
        else if (a == "--standby") o.worn = false;
        else if (a == "--snap" && i + 1 < argc) o.snapshotDir = argv[++i];
        else if (positional == 0) o.seconds = std::atof(a.c_str()), ++positional;
        else if (positional == 1) o.cameras = static_cast<uint8_t>(std::strtoul(a.c_str(), nullptr, 16)), ++positional;
    }
    return runCameraDump(o, gStop);
}
