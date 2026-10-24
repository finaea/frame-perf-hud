// Why a metric has no value.
#pragma once

#include <cstdint>

namespace frame_perf_overlay {

/**
 * Reason code carried by every metric value whose status is N/A.
 * Stable: append new codes, never renumber.
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
};

/**
 * Short English text for a reason, for the debug dump and N/A notes.
 * @param reason the code
 * @return static text ("unknown" for codes this build doesn't know)
 */
const char* reasonText(Reason reason);

}  // namespace frame_perf_overlay
