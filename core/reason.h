// Why a metric has no value. One table for the headset, the PC app and the wire format.
#pragma once

#include <cstdint>

namespace fph {

/**
 * Reason code carried by every metric value whose status is N/A.
 * The numbers go over the link, so they are stable forever: append new codes, never renumber.
 */
enum class Reason : uint16_t {
    None = 0,                ///< the value is ok
    NotSampledYet = 1,       ///< the source hasn't been read yet (or was just resumed)
    SensorMissing = 2,       ///< the file / device wasn't found at startup
    ReadFailed = 3,          ///< the read failed or returned garbage
    NoSteamVr = 4,           ///< SteamVR isn't connected
    NoFrames = 5,            ///< connected, but no frames have arrived
    NotConnected = 6,        ///< the device (controller, link) isn't connected
    Paused = 7,              ///< nobody is looking, so the source isn't read
    InvalidReading = 8,      ///< the sensor reads, but the value is known to be meaningless
    PcAppNotRunning = 9,     ///< needs the Windows app
    CameraHelperMissing = 10,  ///< needs the camera helper
    SteamVrChanged = 11,     ///< a SteamVR update changed something this reader relies on
};

/**
 * Short English text for a reason, for the debug dump and N/A notes.
 * @param reason the code
 * @return static text ("unknown" for codes this build doesn't know)
 */
const char* reasonText(Reason reason);

}  // namespace fph
