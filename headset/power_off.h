// T11: why the headset last powered off. The PMIC's power-on driver logs its reason registers into the kernel log at
// every boot ("qcom-pon c400000.spmi:pmic@0:pon@1300: PON_POFF_REASON1 0x02", "PON: trigger: Hard reset"). Once
// per boot (when /proc/sys/kernel/random/boot_id changes) the agent reads them from this boot's kernel journal
// (read-only, no sudo: steamos is in wheel), decodes them with a table built from known events, and remembers the
// answer in power_off.json for the rest of the boot.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace poweroff {

/** The qcom-pon lines of one boot. */
struct PonReport {
    std::map<std::string, uint32_t> regs;  ///< "PON_POFF_REASON1" -> 0x02, ...
    std::string trigger;                   ///< "PON: trigger: <this>"
    std::string type;                      ///< "PON: type: <this>"

    bool any() const { return !regs.empty() || !trigger.empty(); }
};

/**
 * Takes one kernel message.
 * @return true if it was a qcom-pon line (then report has its value)
 */
bool parseLine(const std::string& message, PonReport& report);

/** @return "POFF 0x02 OFF 0x80 Hard reset": the registers that decide the reason, for unknown combinations */
std::string rawText(const PonReport& report);

/**
 * @return the decoded reason ("Normal restart"), or rawText() when the table doesn't know the combination; "" when
 *         there's nothing to decode. At most 31 bytes (it travels as a short string metric).
 */
std::string decode(const PonReport& report);

/** @return this boot's id without dashes (the journal's _BOOT_ID form), "" if unreadable */
std::string currentBootId();

/**
 * Reads this boot's kernel messages from the journal and parses the qcom-pon lines.
 * @return false with error if the journal can't be read
 */
bool readJournal(const std::string& bootId, PonReport& report, std::string& error);

/**
 * At agent start: the reason for this boot, read once per boot and cached in <stateDir>/power_off.json.
 * @param log one line for the journal ("" when the cached answer was used)
 * @return the text for general.last_power_off ("" if unknown)
 */
std::string lastPowerOff(const std::string& stateDir, std::string& log);

}  // namespace poweroff
