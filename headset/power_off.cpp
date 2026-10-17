#include "power_off.h"

#include "atomic_file.h"
#include "json.h"

#include <systemd/sd-journal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace poweroff {

namespace {

/** A known power-off: the registers and the trigger as logged, and what it was. */
struct Known {
    uint32_t poff;        ///< PON_POFF_REASON1
    uint32_t off;         ///< PON_OFF_REASON1
    const char* trigger;  ///< "PON: trigger: ..."
    const char* text;     ///< at most 31 bytes
};

// Built from real events (tech plan › session logs › last power-off reason). Add a row per captured event; an
// unknown combination shows its raw registers
constexpr Known kKnown[] = {
    // 2026-10-11 19:55, Jack restarted from the Steam menu
    {0x02, 0x80, "Hard reset", "Normal restart"},
    // 2026-10-15, Jack shut down normally; powered on with the power key on 2026-10-17 12:54
    {0x02, 0x80, "KPDPWR_N", "Normal shutdown"},
};

constexpr int kMaxEntries = 20000;  ///< kernel messages read at most (the qcom-pon lines come in the first second)

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool reg(const PonReport& r, const char* name, uint32_t& out) {
    const auto it = r.regs.find(name);
    if (it == r.regs.end()) return false;
    out = it->second;
    return true;
}

}  // namespace

bool parseLine(const std::string& message, PonReport& report) {
    if (message.find("qcom-pon") == std::string::npos) return false;
    const size_t trigger = message.find("PON: trigger:");
    if (trigger != std::string::npos) {
        report.trigger = trim(message.substr(trigger + 13));
        return true;
    }
    const size_t type = message.find("PON: type:");
    if (type != std::string::npos) {
        report.type = trim(message.substr(type + 10));
        return true;
    }
    // "...: PON_POFF_REASON1 0x02": the register is the word before the value (PON_PON_REASON1 has "PON_" twice)
    const size_t lower = message.rfind(" 0x"), upper = message.rfind(" 0X");
    const size_t space = lower == std::string::npos ? upper : upper == std::string::npos ? lower : std::max(lower, upper);
    if (space == std::string::npos || space == 0) return false;
    const size_t before = message.rfind(' ', space - 1);
    const size_t name = before == std::string::npos ? 0 : before + 1;
    if (message.compare(name, 4, "PON_") != 0) return false;
    const std::string value = trim(message.substr(space + 1));
    if (value.size() < 3 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X')) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(value.c_str() + 2, &end, 16);
    if (end == value.c_str() + 2) return false;
    report.regs[message.substr(name, space - name)] = static_cast<uint32_t>(v);
    return true;
}

std::string rawText(const PonReport& report) {
    std::string out;
    char part[48];
    uint32_t v = 0;
    if (reg(report, "PON_POFF_REASON1", v)) {
        std::snprintf(part, sizeof(part), "POFF 0x%02X", v);
        out += part;
    }
    if (reg(report, "PON_OFF_REASON1", v)) {
        std::snprintf(part, sizeof(part), "%sOFF 0x%X", out.empty() ? "" : " ", v);
        out += part;
    }
    if (!report.trigger.empty()) out += (out.empty() ? "" : " ") + report.trigger;
    return out;
}

std::string decode(const PonReport& report) {
    uint32_t poff = 0, off = 0;
    const bool havePoff = reg(report, "PON_POFF_REASON1", poff);
    const bool haveOff = reg(report, "PON_OFF_REASON1", off);
    if (havePoff && haveOff) {
        for (const Known& k : kKnown) {
            if (k.poff == poff && k.off == off && report.trigger == k.trigger) return k.text;
        }
    }
    const std::string raw = rawText(report);
    return raw.size() > 31 ? raw.substr(0, 31) : raw;
}

std::string currentBootId() {
    std::ifstream file("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(file, id);
    std::string out;
    for (const char c : id) {
        if (c != '-' && c != '\n') out += c;
    }
    return out.size() == 32 ? out : std::string();
}

bool readJournal(const std::string& bootId, PonReport& report, std::string& error) {
    sd_journal* j = nullptr;
    int rc = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM);
    if (rc < 0) {
        error = std::string("sd_journal_open: ") + std::strerror(-rc);
        return false;
    }
    const std::string bootMatch = "_BOOT_ID=" + bootId;
    sd_journal_add_match(j, bootMatch.c_str(), 0);
    sd_journal_add_match(j, "_TRANSPORT=kernel", 0);
    sd_journal_seek_head(j);
    int entries = 0;
    while (entries < kMaxEntries && sd_journal_next(j) > 0) {
        ++entries;
        const void* data = nullptr;
        size_t length = 0;
        if (sd_journal_get_data(j, "MESSAGE", &data, &length) < 0) continue;
        const char* text = static_cast<const char*>(data);
        if (length < 8 || std::strncmp(text, "MESSAGE=", 8) != 0) continue;
        parseLine(std::string(text + 8, length - 8), report);
        // they're logged together; past the trigger there's nothing more to find
        if (!report.trigger.empty() && report.regs.count("PON_POFF_REASON1")) break;
    }
    sd_journal_close(j);
    if (!report.any()) {
        error = "no qcom-pon lines in this boot's kernel journal (" + std::to_string(entries) + " messages read)";
        return false;
    }
    return true;
}

std::string lastPowerOff(const std::string& stateDir, std::string& log) {
    log.clear();
    const std::string path = stateDir + "/power_off.json";
    const std::string boot = currentBootId();
    {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        JsonValue root;
        std::string error;
        if (file && parseJson(text.str(), root, error)) {
            const JsonValue* id = root.get("boot_id");
            const JsonValue* regs = root.get("regs");
            const JsonValue* trigger = root.get("trigger");
            // Read once this boot already: decode the cached registers again, so a decode row added since applies
            // at once. A cache from before "regs" was written falls through and reads the journal one more time
            if (id != nullptr && id->isString() && id->text == boot && !boot.empty() && regs != nullptr &&
                regs->isObject()) {
                PonReport cached;
                for (const auto& r : regs->members) {
                    if (r.second.isNumber()) cached.regs[r.first] = static_cast<uint32_t>(r.second.number);
                }
                if (trigger != nullptr && trigger->isString()) cached.trigger = trigger->text;
                if (const JsonValue* type = root.get("type")) cached.type = type->isString() ? type->text : "";
                return decode(cached);
            }
        }
    }
    if (boot.empty()) {
        log = "can't read the boot id";
        return "";
    }
    PonReport report;
    std::string error;
    if (!readJournal(boot, report, error)) {
        log = "last power-off reason unknown: " + error;
        return "";  // not cached: the next start tries again (the kernel lines may not be in the journal yet)
    }
    const std::string text = decode(report);
    std::string regs;
    for (const auto& r : report.regs) {
        char part[64];
        std::snprintf(part, sizeof(part), "%s%s 0x%X", regs.empty() ? "" : ", ", r.first.c_str(), r.second);
        regs += part;
    }
    log = "last power-off: " + text + " (" + regs + "; trigger \"" + report.trigger + "\", type \"" + report.type +
          "\")";
    // "regs" + "trigger" are what a later start decodes; "text" is for people reading the file
    std::string regsJson;
    for (const auto& r : report.regs) {
        regsJson += (regsJson.empty() ? "" : ", ") + std::string("\"") + r.first + "\": " + std::to_string(r.second);
    }
    std::string json = "{\n  \"boot_id\": \"" + boot + "\",\n  \"text\": \"" + text + "\",\n  \"regs\": { " +
                       regsJson + " },\n  \"trigger\": \"" + report.trigger + "\",\n  \"type\": \"" + report.type +
                       "\"\n}\n";
    std::string saveError;
    if (!fph::atomicWriteFile(path, json, saveError)) log += "; can't remember it: " + saveError;
    return text;
}

}  // namespace poweroff
