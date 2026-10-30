// What the dashboard tab's Logs page shows (ticket 11's SessionService provides it). Kept apart so the settings panel
// doesn't pull in the logger.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/** Session logs, Save / Record and the crash recorder, for the Logs page. */
struct LogsUiState {
    bool recording = false;
    std::string recordStatus;            ///< "Recording 3:12 to …" / last recording's file, "" = nothing yet (the PC)
    std::string recordLine;              ///< the Direct Record card: "Recording 3:12" / "Recorded 192 s" / an error
    std::string recordPath;              ///< under it: "saves/<file>_record.csv", "" = none
    std::string saveStatus;              ///< "Saved 13:05:09: <file> (600 s)" / why it failed, "" = nothing yet
    std::string saveLine;                ///< the Direct Save card: "Saved 13:05:09 (600 s)" / an error
    std::string savePath;                ///< under it: "saves/<stem>", "" = none
    bool crashRecorderOn = false;        ///< really running (the config may say on while it failed to start)
    std::string crashStatus;             ///< "Crash log kept (12:55)" / an error, "" = nothing to say
    std::string powerOff;                ///< the decoded reason, "" = unknown
    std::vector<std::string> lastCard;   ///< the last ended session, slog::shortCard (3 lines), empty = none yet
    uint64_t revision = 0;               ///< bumps whenever anything above changes (redraw)
};
