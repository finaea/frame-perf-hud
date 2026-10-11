#include "reason.h"

namespace fph {

const char* reasonText(Reason reason) {
    switch (reason) {
        case Reason::None: return "ok";
        case Reason::NotSampledYet: return "not read yet";
        case Reason::SensorMissing: return "sensor not found";
        case Reason::ReadFailed: return "read failed";
        case Reason::NoSteamVr: return "SteamVR not connected";
        case Reason::NoFrames: return "no frames yet";
        case Reason::NotConnected: return "not connected";
        case Reason::Paused: return "paused (nobody looking)";
        case Reason::InvalidReading: return "reading not meaningful";
        case Reason::PcAppNotRunning: return "PC app not running";
        case Reason::CameraHelperMissing: return "camera helper not installed";
        case Reason::SteamVrChanged: return "SteamVR changed";
    }
    return "unknown";
}

}  // namespace fph
