// What the dashboard tab's Logs page shows (ticket 11's SessionService provides it). Kept apart so the settings panel
// doesn't pull in the logger.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/** Session logs, Save / Record and the crash recorder, for the Logs page. */
struct LogsUiState {
    bool recording = false;
    std::string recordStatus;            ///< "Recording 3:12 to …" / last recording's file, "" = nothing yet
    std::string saveStatus;              ///< "Saved 13:05:09: <file>" / why it failed, "" = nothing yet
    bool crashRecorderOn = false;        ///< really running (the config may say on while it failed to start)
    std::string crashStatus;             ///< "Crash log saved (12:55)" / an error, "" = nothing to say
    std::string powerOff;                ///< the decoded reason, "" = unknown
    std::vector<std::string> lastCard;   ///< the last ended session's card, empty = none yet
    uint64_t revision = 0;               ///< bumps whenever anything above changes (redraw)
};
