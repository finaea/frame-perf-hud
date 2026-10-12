// T10: --camera-dump: runs the camera thread on its own (no overlay, no link) for a while, prints its state and
// counters (copies, rechecks, dark frames skipped, torn eye copies dropped, the thread's CPU), and writes the first
// live frame of each camera as a PGM. Read-only throughout, like the widgets.
#pragma once

#include <csignal>
#include <cstdint>
#include <string>

struct CameraDumpOptions {
    double seconds = 20.0;
    uint8_t cameras = 0x0F;  ///< bit = camera id (0-3 outward, 4-5 eyes)
    bool pc = false;         ///< also make JPEGs (as if a PC asked), to measure their cost
    bool display = true;     ///< make RGBA frames (as the headset tiles would)
    bool fullSize = false;
    bool worn = true;  ///< what the agent would learn from the HMD activity level (false = standby)
    std::string snapshotDir;  ///< empty = no snapshots
};

/** @return exit code */
int runCameraDump(const CameraDumpOptions& options, const volatile std::sig_atomic_t& stop);
