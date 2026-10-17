// T11: the power-off reason decoder (headset/power_off): pure parse + decode against captured kernel lines.
// Never reads the real journal (the cache test reads only its own temp file and /proc's boot id).
#include "power_off.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace poweroff;

const char* kP = "qcom-pon c400000.spmi:pmic@0:pon@1300: ";

std::string line(const std::string& rest) {
    return std::string(kP) + rest;
}

PonReport parseAll(const std::vector<std::string>& messages) {
    PonReport r;
    for (const std::string& m : messages) parseLine(m, r);
    return r;
}

/** 2026-10-11: Jack restarted from the Steam menu. */
PonReport normalRestart() {
    return parseAll({line("PON_POFF_REASON1 0x02"), line("PON_OFF_REASON1 0x00000080"), line("PON_ON_REASON 0x80"),
                     line("PON_WARM_RESET_REASON1 0x02"), line("PON: trigger: Hard reset")});
}

/** 2026-10-17 12:54 boot: Jack shut down normally on 10-15, powered on with the power key. */
PonReport normalShutdown() {
    return parseAll({line("PON_PON_REASON1 0x80"), line("PON_WARM_RESET_REASON1 0x02"), line("PON_ON_REASON 0x80"),
                     line("PON_POFF_REASON1 0x02"), line("PON_OFF_REASON1 0x00000080"),
                     line("PON_PBS_FAULT_REASON1 0x00000000"), line("PON: type: PON reset seq"),
                     line("PON: trigger: KPDPWR_N")});
}

void parsesNormalRestart() {
    PonReport r;
    CHECK(!r.any());
    CHECK(parseLine(line("PON_POFF_REASON1 0x02"), r));
    CHECK(r.any());
    CHECK(r.regs.at("PON_POFF_REASON1") == 0x02);
    CHECK(parseLine(line("PON_OFF_REASON1 0x00000080"), r));
    CHECK(r.regs.at("PON_OFF_REASON1") == 0x80);
    CHECK(parseLine(line("PON: trigger: Hard reset"), r));
    CHECK(r.trigger == "Hard reset");

    const PonReport full = normalRestart();
    CHECK(full.regs.size() == 4);
    CHECK(full.regs.at("PON_ON_REASON") == 0x80);
    CHECK(full.regs.at("PON_WARM_RESET_REASON1") == 0x02);
    CHECK(full.trigger == "Hard reset");
    CHECK(full.type.empty());
    CHECK(decode(full) == "Normal restart");
}

void parsesNormalShutdown() {
    const PonReport r = normalShutdown();
    CHECK(r.trigger == "KPDPWR_N");
    CHECK(r.type == "PON reset seq");
    CHECK(r.regs.at("PON_POFF_REASON1") == 0x02);
    CHECK(r.regs.at("PON_OFF_REASON1") == 0x80);
    CHECK(r.regs.at("PON_PBS_FAULT_REASON1") == 0);
    CHECK(r.regs.at("PON_ON_REASON") == 0x80);
    CHECK(r.regs.size() == 6);
    // the register's own name is kept whole ("PON_PON_REASON1" holds "PON_" twice)
    CHECK(r.regs.count("PON_PON_REASON1") == 1);
    CHECK(r.regs.count("PON_REASON1") == 0);
    // same off registers as the restart; the power-key trigger tells them apart
    CHECK(rawText(r) == "POFF 0x02 OFF 0x80 KPDPWR_N");
    CHECK(decode(r) == "Normal shutdown");
    CHECK(decode(r).size() <= 31);
}

void ignoresOtherLines() {
    PonReport r;
    CHECK(!parseLine("usb 1-1: new high-speed USB device number 2 using xhci-hcd", r));
    CHECK(!parseLine("PON_POFF_REASON1 0x02", r));  // right shape, not the pon driver
    CHECK(!parseLine("", r));
    CHECK(!parseLine(line("registered"), r));            // pon driver, but no register
    CHECK(!parseLine(line("PON_POFF_REASON1"), r));      // no value
    CHECK(!parseLine(line("PON_POFF_REASON1 zzz"), r));  // not hex
    CHECK(!parseLine(line("PON_POFF_REASON1 0x"), r));
    CHECK(!r.any());
    CHECK(r.regs.empty() && r.trigger.empty() && r.type.empty());
    // a good line after the noise still lands
    CHECK(parseLine(line("PON_POFF_REASON1 0x02"), r));
    CHECK(r.regs.size() == 1);
}

void decodeNeedsTheWholeMatch() {
    // same registers, other trigger: the table row doesn't apply, raw text instead
    PonReport r = normalRestart();
    r.trigger = "Soft reset";
    CHECK(decode(r) == "POFF 0x02 OFF 0x80 Soft reset");
    // other OFF register
    r = normalRestart();
    r.regs["PON_OFF_REASON1"] = 0x40;
    CHECK(decode(r) == "POFF 0x02 OFF 0x40 Hard reset");
    // a missing register: no table lookup
    r = normalRestart();
    r.regs.erase("PON_OFF_REASON1");
    CHECK(decode(r) == "POFF 0x02 Hard reset");
    // nothing parsed: nothing to say
    CHECK(decode(PonReport()).empty());
    CHECK(rawText(PonReport()).empty());
    // only a trigger
    PonReport t;
    t.trigger = "KPDPWR_N";
    CHECK(decode(t) == "KPDPWR_N");
}

void resultFitsAShortString() {
    PonReport r = normalRestart();
    r.trigger = std::string(80, 'x');
    CHECK(decode(r).size() <= 31);
    CHECK(!decode(r).empty());
    r.regs["PON_POFF_REASON1"] = 0xFFFFFFFFu;
    r.regs["PON_OFF_REASON1"] = 0xFFFFFFFFu;
    CHECK(decode(r).size() <= 31);
}

// A boot read before a decode row existed: the cached registers are decoded again at start (no journal read), so
// the new row applies without waiting for the next boot
void cachedRegistersAreDecodedAgain() {
    const std::string boot = currentBootId();
    CHECK(!boot.empty());
    char dirTemplate[] = "/tmp/fph-poweroff-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    CHECK(dir != nullptr);
    if (dir == nullptr || boot.empty()) return;
    const std::string path = std::string(dir) + "/power_off.json";
    {
        std::ofstream f(path);
        f << "{\n  \"boot_id\": \"" << boot << "\",\n  \"text\": \"POFF 0x02 OFF 0x80 KPDPWR_N\",\n"
          << "  \"regs\": { \"PON_OFF_REASON1\": 128, \"PON_POFF_REASON1\": 2 },\n"
          << "  \"trigger\": \"KPDPWR_N\",\n  \"type\": \"PON reset seq\"\n}\n";
    }
    std::string log;
    CHECK(lastPowerOff(dir, log) == "Normal shutdown");
    CHECK(log.empty());  // the cache was used: no journal read, nothing logged
    std::remove(path.c_str());
    rmdir(dir);
}

void parsesHexCaseAndSpacing() {
    PonReport r;
    CHECK(parseLine(line("PON_POFF_REASON1 0X0A  "), r));
    CHECK(r.regs.count("PON_POFF_REASON1") == 1 && r.regs.at("PON_POFF_REASON1") == 0x0A);
    CHECK(parseLine(line("PON: trigger:   Hard reset  "), r));
    CHECK(r.trigger == "Hard reset");  // trimmed
}

}  // namespace

int main() {
    parsesNormalRestart();
    parsesNormalShutdown();
    cachedRegistersAreDecodedAgain();
    ignoresOtherLines();
    decodeNeedsTheWholeMatch();
    resultFitsAShortString();
    parsesHexCaseAndSpacing();
    if (failures == 0) std::printf("power_off_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
