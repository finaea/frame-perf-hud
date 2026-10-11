// End-to-end test of the system sensor readers: a fake /sys + /proc tree is built under a temp dir, Sensors
// discovers it, the scheduler runs the sources, and the metric store is checked.
// POSIX only (mkdtemp, nftw, pread via Sensors). The tree is built in code, not checked in: iio device names
// contain ':' and can't exist on NTFS.
#include "metric_registry.h"
#include "metric_store.h"
#include "scheduler.h"
#include "sensors.h"
#include "system_metrics.h"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using fph::MetricStore;
using fph::MetricValue;
using fph::Reason;
using fph::Scheduler;
using fph::SourceId;
using fph::WidgetType;
namespace metric = fph::metric;

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

// Metric checks: print the metric id and what was found, so a failure says which one
void fail(int line, uint16_t id, const char* what) {
    std::printf("FAIL line %d: metric %u: %s\n", line, static_cast<unsigned>(id), what);
    ++failures;
}

void checkF32(const MetricStore& st, uint16_t id, double expected, double tol, int line) {
    const MetricValue* v = st.get(id);
    if (v == nullptr || !v->ok) {
        fail(line, id, v == nullptr ? "unknown id" : "expected a value, got N/A");
    } else if (std::fabs(static_cast<double>(v->f32) - expected) > tol) {
        std::printf("FAIL line %d: metric %u: expected %g, got %g\n", line, static_cast<unsigned>(id), expected,
                    static_cast<double>(v->f32));
        ++failures;
    }
}

void checkNa(const MetricStore& st, uint16_t id, Reason reason, int line) {
    const MetricValue* v = st.get(id);
    if (v == nullptr) {
        fail(line, id, "unknown id");
    } else if (v->ok) {
        fail(line, id, "expected N/A, got a value");
    } else if (v->reason != reason) {
        std::printf("FAIL line %d: metric %u: expected reason %s, got %s\n", line, static_cast<unsigned>(id),
                    fph::reasonText(reason), fph::reasonText(v->reason));
        ++failures;
    }
}

void checkI32(const MetricStore& st, uint16_t id, int32_t expected, int line) {
    const MetricValue* v = st.get(id);
    if (v == nullptr || !v->ok) {
        fail(line, id, "expected a value, got N/A");
    } else if (v->i32 != expected) {
        std::printf("FAIL line %d: metric %u: expected %d, got %d\n", line, static_cast<unsigned>(id), expected,
                    static_cast<int>(v->i32));
        ++failures;
    }
}

void checkEnum(const MetricStore& st, uint16_t id, uint32_t expected, int line) {
    const MetricValue* v = st.get(id);
    if (v == nullptr || !v->ok) {
        fail(line, id, "expected a value, got N/A");
    } else if (v->enumValue != expected) {
        std::printf("FAIL line %d: metric %u: expected %u, got %u\n", line, static_cast<unsigned>(id), expected,
                    v->enumValue);
        ++failures;
    }
}

void checkText(const MetricStore& st, uint16_t id, const std::string& expected, int line) {
    const MetricValue* v = st.get(id);
    if (v == nullptr || !v->ok) {
        fail(line, id, "expected a value, got N/A");
    } else if (v->text != expected) {
        std::printf("FAIL line %d: metric %u: expected \"%s\", got \"%s\"\n", line, static_cast<unsigned>(id),
                    expected.c_str(), v->text.c_str());
        ++failures;
    }
}

#define F32_IS(st, id, expected) checkF32(st, id, expected, 1e-3, __LINE__)
#define F32_NEAR(st, id, expected, tol) checkF32(st, id, expected, tol, __LINE__)
#define NA_IS(st, id, reason) checkNa(st, id, reason, __LINE__)
#define I32_IS(st, id, expected) checkI32(st, id, expected, __LINE__)
#define ENUM_IS(st, id, expected) checkEnum(st, id, expected, __LINE__)
#define TEXT_IS(st, id, expected) checkText(st, id, expected, __LINE__)

// ---------------------------------------------------------------------------------------------
// Fake filesystem

bool mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string part = path.substr(0, i);
            if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

/** Writes root/rel (parents created, truncated in place so an open fd keeps seeing it) + a newline. */
void put(const std::string& root, const std::string& rel, const std::string& value) {
    const std::string path = root + "/" + rel;
    mkdirs(path.substr(0, path.rfind('/')));
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    out << value << "\n";
    if (!out) {
        std::printf("FAIL cannot write %s\n", path.c_str());
        ++failures;
    }
}

int removeEntry(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}

void removeTree(const std::string& root) {
    ::nftw(root.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string makeTempRoot() {
    const char* base = std::getenv("TMPDIR");
    std::string templ = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/fph-sysfs-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        std::printf("FAIL mkdtemp\n");
        std::exit(2);
    }
    return std::string(buf.data());
}

const char* kStatA =
    "cpu  367360 15068 256399 6245980 687 89570 79807 0 0 0\n"
    "cpu0 46199 1714 33845 761685 19 30759 5426 0 0 0\n"
    "cpu1 47552 2961 29886 791051 31 8510 894 0 0 0\n"
    "intr 123456 1 2 3\nctxt 999\n";
// Same, after 400 total jiffies: total +100 busy (25%), cpu0 +50 of 100 (50%), cpu1 idle (0%)
const char* kStatB =
    "cpu  367460 15068 256399 6246280 687 89570 79807 0 0 0\n"
    "cpu0 46249 1714 33845 761735 19 30759 5426 0 0 0\n"
    "cpu1 47552 2961 29886 791151 31 8510 894 0 0 0\n"
    "intr 123999 1 2 3\nctxt 1999\n";

const char* kStatusUnplugged =
    "JEITA: 0\nCV: 0\nCC: 0\nICL: 0\nBoost: 0\nBuck: 0\nUSB: 0\nInput OK: 0\nBuck OK: 0\nTemp OK: 0\n"
    "BATT OK: 0\nCHG Ctrl OK: 0\nUSB UV: 1\nUSB OV: 0\nUSB Suspend: 0\nIMP Trigger: 0\nBuck SS Done: 0\n"
    "Buck OC: 0\nVPH UV: 0\nDie Temp Bad: 0\nJEITA Cold: 0\nJEITA Hot: 0\nQPH VBAT1 OK: 0\nQPH VIN OK: 0\nQPH2: 0";
const char* kStatusCc =
    "JEITA: 0\nCV: 0\nCC: 1\nICL: 0\nBoost: 0\nBuck: 1\nUSB: 1\nInput OK: 1\nBuck OK: 1\nTemp OK: 1\n"
    "BATT OK: 1\nCHG Ctrl OK: 1\nUSB UV: 0\nUSB OV: 0\nUSB Suspend: 0\nIMP Trigger: 0\nBuck SS Done: 1\n"
    "Buck OC: 0\nVPH UV: 0\nDie Temp Bad: 0\nJEITA Cold: 0\nJEITA Hot: 0\nQPH VBAT1 OK: 1\nQPH VIN OK: 1\nQPH2: 1";

struct TreeOptions {
    bool plugged = false;
    bool withIio = true;
    bool withPowerSupply = true;
    bool chip12HasInLabels = true;
    bool throttledCpu = false;
};

void buildTree(const std::string& root, const TreeOptions& o) {
    // hwmon: three max34417 chips + the fan controller. power<N> pairs with in<N-1>, same label.
    struct Chip {
        const char* dir;
        const char* name;
        const char* labels[4];
        long uw[4];
        long mv[4];
    };
    const Chip chips[] = {
        {"hwmon0", "max34417_12", {"apc0", "apc2", "apc1", "nsp1"}, {519995, 806278, 231063, 201112}, {862, 826, 732, 812}},
        {"hwmon1", "max34417_1a", {"gfx", "nsp2", "bob", "unused"}, {439515, 85414, 491827, 46}, {665, 852, 3757, 4}},
        {"hwmon2", "max34417_10", {"vph", "s1c", "s3c", "s6c"}, {9188324, 505967, 292181, 898119}, {3804, 1249, 1010, 1894}},
    };
    for (const Chip& c : chips) {
        const std::string base = std::string("sys/class/hwmon/") + c.dir;
        put(root, base + "/name", c.name);
        const bool inLabels = o.chip12HasInLabels || std::string(c.name) != "max34417_12";
        for (int n = 1; n <= 4; ++n) {
            put(root, base + "/power" + std::to_string(n) + "_input", std::to_string(c.uw[n - 1]));
            put(root, base + "/power" + std::to_string(n) + "_label", c.labels[n - 1]);
            put(root, base + "/in" + std::to_string(n - 1) + "_input", std::to_string(c.mv[n - 1]));
            if (inLabels) put(root, base + "/in" + std::to_string(n - 1) + "_label", c.labels[n - 1]);
        }
    }
    put(root, "sys/class/hwmon/hwmon3/name", "slg4ax46073v");
    put(root, "sys/class/hwmon/hwmon3/fan1_input", "7314");

    // thermal zones (m°C)
    const struct {
        const char* type;
        long milli;
    } zones[] = {
        {"cpuss-0-thermal", 68900}, {"cpu-1-0-thermal", 66000}, {"gpuss-0-thermal", 52000},
        {"gpuss-1-thermal", 55500}, {"gpuss-2-thermal", 51000}, {"ddr-thermal", 47000},
        {"nsphvx0-thermal", 50500}, {"camera0-thermal", 41000}, {"video-thermal", 44000},
        {"max1720x_bat_7-36", 35700},
    };
    int zone = 0;
    for (const auto& z : zones) {
        const std::string base = "sys/class/thermal/thermal_zone" + std::to_string(zone++);
        put(root, base + "/type", z.type);
        put(root, base + "/temp", std::to_string(z.milli));
    }
    put(root, "sys/class/thermal/cooling_device0/type", "cpufreq-cpu0");
    put(root, "sys/class/thermal/cooling_device0/cur_state", o.throttledCpu ? "3" : "0");
    put(root, "sys/class/thermal/cooling_device4/type", "devfreq-3d00000.gpu");
    put(root, "sys/class/thermal/cooling_device4/cur_state", "0");

    // cpufreq (kHz)
    const struct {
        const char* policy;
        const char* cur;
        const char* max;
    } policies[] = {{"policy0", "2265600", "2265600"},
                    {"policy2", "3148800", "3148800"},
                    {"policy5", "2841600", "2956800"},
                    {"policy7", "2995200", "3052800"}};
    for (const auto& p : policies) {
        put(root, std::string("sys/devices/system/cpu/cpufreq/") + p.policy + "/scaling_cur_freq", p.cur);
        put(root, std::string("sys/devices/system/cpu/cpufreq/") + p.policy + "/scaling_max_freq", p.max);
    }
    put(root, "sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq", "2995200");
    put(root, "sys/class/devfreq/3d00000.gpu/cur_freq", "680000000");
    put(root, "sys/class/devfreq/3d00000.gpu/max_freq", "900000000");

    if (o.withPowerSupply) {
        const std::string bat = "sys/class/power_supply/max1720x_bat_7-36/";
        put(root, bat + "type", "Battery");
        put(root, bat + "capacity", "56");
        put(root, bat + "voltage_now", "7765936");
        put(root, bat + "current_now", o.plugged ? "1100000" : "-1503750");
        put(root, bat + "current_avg", o.plugged ? "1000000" : "-1333750");
        put(root, bat + "temp", "357");
        put(root, bat + "status", o.plugged ? "Charging" : "Discharging");
        put(root, bat + "charge_now", "1534000");
        put(root, bat + "charge_full", "2730000");
        put(root, bat + "charge_full_design", "2730000");
        put(root, bat + "health", "Good");
        put(root, bat + "cycle_count", "7");

        const std::string chg = "sys/class/power_supply/pm8550b-charger/";
        put(root, chg + "type", "USB");
        put(root, chg + "online", o.plugged ? "1" : "0");
        put(root, chg + "status_detailed", o.plugged ? kStatusCc : kStatusUnplugged);
        put(root, chg + "charging_enable", "1");
        put(root, chg + "charging_pause", "0");
        put(root, chg + "constant_charge_current", "1000000");
        put(root, chg + "constant_charge_current_max", "1200000");

        const std::string src = "sys/class/power_supply/tcpm-source-psy-c400000.spmi:pmic@7:typec@2b00/";
        put(root, src + "type", "USB");
        put(root, src + "online", o.plugged ? "1" : "0");
        put(root, src + "usb_type", o.plugged ? "C [PD] PD_PPS" : "[C] PD PD_PPS");
        put(root, src + "voltage_now", o.plugged ? "15000000" : "0");
        put(root, src + "current_max", o.plugged ? "3000000" : "0");
    }

    if (o.withIio) {
        const std::string iio = "sys/bus/iio/devices/iio:device0/";
        put(root, iio + "name", "c400000.spmi:pmic@0:vadc@9000");
        put(root, iio + "in_temp_display_left_therm_input", "64294");
        put(root, iio + "in_temp_pcba_at_fan_exhaust_input", "48500");
        put(root, iio + "in_temp_pcba_at_heat_sink_fins_input", "51000");
        put(root, iio + "in_temp_pcba_at_wifi_input", "62045");
        put(root, iio + "in_temp_pcba_at_flash_input", "45000");
        put(root, iio + "in_temp_pcba_at_nrf_input", "43000");
        put(root, iio + "in_temp_pcba_at_40pin_connector_input", "39000");
        put(root, iio + "in_voltage_pm8550b_usb_sns_v_div_16_input", o.plugged ? "14200000" : "3000");
        put(root, iio + "in_current_pm8550b_iin_fb_input", o.plugged ? "1650000" : "-14000");
    }

    put(root, "proc/stat", kStatA);
    put(root, "proc/meminfo",
        "MemTotal:       16777216 kB\nMemFree:         1000000 kB\nMemAvailable:    8388608 kB\nBuffers: 1 kB");
    put(root, "proc/uptime", "8914.95 62459.80");
    put(root, "proc/self/statm", "617 292 271 7 0 146 0");
}

/** Everything the scheduler needs, built once and never moved (the readers capture references). */
struct Rig {
    std::string root;
    MetricStore store;
    Scheduler sched;
    Sensors sensors;

    explicit Rig(const std::string& r) : root(r) {}

    void start(const std::vector<WidgetType>& widgets) {
        sensors.discover(root);
        registerSystemSources(sched, sensors, store);
        sched.setEnabledWidgets(widgets);
        fph::DemandInputs d;
        d.hudShown = true;
        d.hmdInUse = true;
        sched.setDemand(d);
    }
};

const std::vector<WidgetType> kAllSystemWidgets = {WidgetType::Cpu,     WidgetType::GpuRam,  WidgetType::Power,
                                                   WidgetType::Battery, WidgetType::Temp,    WidgetType::General};

// ---------------------------------------------------------------------------------------------
// Scenarios

void checkPower(const MetricStore& st) {
    F32_IS(st, metric::kPowerVphW, 9.188324);
    F32_IS(st, metric::kPowerVphV, 3.804);
    F32_IS(st, metric::kPowerApc0W, 0.519995);
    F32_IS(st, metric::kPowerApc0W + metric::kPowerRailVoltsOffset, 0.862);
    F32_IS(st, metric::kPowerApc1W, 0.231063);
    F32_IS(st, metric::kPowerApc2W, 0.806278);
    F32_IS(st, metric::kPowerNsp1W, 0.201112);
    F32_IS(st, metric::kPowerNsp2W, 0.085414);
    F32_IS(st, metric::kPowerBobW, 0.491827);
    F32_IS(st, metric::kPowerBobW + metric::kPowerRailVoltsOffset, 3.757);
    F32_IS(st, metric::kPowerS1cW, 0.505967);
    F32_IS(st, metric::kPowerS3cW, 0.292181);
    F32_IS(st, metric::kPowerS6cW, 0.898119);
    F32_IS(st, metric::kPowerS6cW + metric::kPowerRailVoltsOffset, 1.894);
    // gfx: watts are fine, its own voltage sense isn't meaningful
    F32_IS(st, metric::kPowerGfxW, 0.439515);
    NA_IS(st, metric::kPowerGfxW + metric::kPowerRailVoltsOffset, Reason::InvalidReading);
    // 10 rails, vph and the "unused" channel (46 µW) not in the sum, and never the 11-channel sum
    F32_NEAR(st, metric::kPowerRailsW, 4.471471, 1e-5);
    F32_NEAR(st, metric::kPowerUnmeasuredW, 9.188324 - 4.471471, 1e-5);
}

void testUnplugged() {
    const std::string root = makeTempRoot();
    buildTree(root, TreeOptions());
    {
        Rig rig(root);
        rig.start(kAllSystemWidgets);
        const MetricStore& st = rig.store;

        // discover(): the "unused" channel is skipped, the rest (vph + 10 rails) are found
        const std::string described = rig.sensors.describe();
        CHECK(described.find("unused") == std::string::npos);
        CHECK(described.find("Power: 11 ch") != std::string::npos);

        CHECK(rig.sched.runDue(10.0) > 0);

        // Power
        checkPower(st);

        // Battery, on battery
        F32_IS(st, metric::kBatteryPct, 56);
        F32_IS(st, metric::kBatteryVoltageV, 7.765936);
        F32_IS(st, metric::kBatteryCurrentMa, -1503.75);
        F32_IS(st, metric::kBatteryChargeNowMah, 1534);
        F32_IS(st, metric::kBatteryChargeFullMah, 2730);
        F32_IS(st, metric::kBatteryChargeDesignMah, 2730);
        F32_IS(st, metric::kTempBatteryC, 35.7);
        F32_IS(st, metric::kBatteryPowerW, 7.765936 * -1.50375);
        CHECK(st.f32Or(metric::kBatteryPowerW, 0.0) < 0);
        ENUM_IS(st, metric::kBatteryStatus, static_cast<uint32_t>(fph::BatteryStatus::Discharging));
        ENUM_IS(st, metric::kBatteryChargerOnline, 0);
        ENUM_IS(st, metric::kBatteryPlugged, 0);
        // From current_avg (-1.33375 A), not current_now
        I32_IS(st, metric::kBatteryTimeToEmptyS,
               static_cast<int32_t>(std::lround(1534 / 1000.0 / 1.33375 * 3600.0)));
        NA_IS(st, metric::kBatteryTimeToFullS, Reason::NotConnected);
        TEXT_IS(st, metric::kBatteryChargerMode, "Unplugged");
        TEXT_IS(st, metric::kBatteryHealth, "Good");
        I32_IS(st, metric::kBatteryCycles, 7);
        F32_IS(st, metric::kBatterySetCurrentA, 1.0);
        F32_IS(st, metric::kBatterySetCurrentMaxA, 1.2);
        NA_IS(st, metric::kBatteryUsbType, Reason::NotConnected);
        NA_IS(st, metric::kBatteryPdOfferedV, Reason::NotConnected);
        NA_IS(st, metric::kBatteryPdOfferedA, Reason::NotConnected);
        NA_IS(st, metric::kBatteryPdOfferedW, Reason::NotConnected);
        NA_IS(st, metric::kBatteryInputV, Reason::NotConnected);
        NA_IS(st, metric::kBatteryInputA, Reason::NotConnected);
        NA_IS(st, metric::kBatteryInputW, Reason::NotConnected);
        NA_IS(st, metric::kBatterySagV, Reason::NotConnected);

        // Temps
        F32_IS(st, metric::kTempCpuC, 68.9);
        F32_IS(st, metric::kTempGpuC, 55.5);
        F32_IS(st, metric::kTempDdrC, 47.0);
        F32_IS(st, metric::kTempNpuC, 50.5);
        F32_IS(st, metric::kTempCameraC, 41.0);
        F32_IS(st, metric::kTempVideoC, 44.0);
        F32_IS(st, metric::kTempDisplayC, 64.294);
        F32_IS(st, metric::kTempExhaustC, 48.5);
        F32_IS(st, metric::kTempHeatsinkC, 51.0);
        F32_IS(st, metric::kTempWifiC, 62.045);
        F32_IS(st, metric::kTempFlashC, 45.0);
        F32_IS(st, metric::kTempNrfC, 43.0);
        F32_IS(st, metric::kTempConnectorC, 39.0);
        ENUM_IS(st, metric::kThrottleCpu, 0);
        ENUM_IS(st, metric::kThrottleGpu, 0);
        F32_IS(st, metric::kFanRpm, 3657);

        // Clocks (kHz → MHz, Hz → MHz)
        F32_IS(st, metric::kCpuCluster0Mhz + 0, 2265.6);
        F32_IS(st, metric::kCpuCluster0Mhz + 1, 3148.8);
        F32_IS(st, metric::kCpuCluster0Mhz + 2, 2841.6);
        F32_IS(st, metric::kCpuCluster0Mhz + 3, 2995.2);
        F32_IS(st, metric::kCpuCluster0MaxMhz + 0, 2265.6);
        F32_IS(st, metric::kCpuCluster0MaxMhz + 1, 3148.8);
        F32_IS(st, metric::kCpuCluster0MaxMhz + 2, 2956.8);
        F32_IS(st, metric::kCpuCluster0MaxMhz + 3, 3052.8);
        F32_IS(st, metric::kCpuFastestClockMhz, 3148.8);
        F32_IS(st, metric::kCpuBigCoreClockMhz, 2995.2);
        F32_IS(st, metric::kGpuClockMhz, 680.0);
        F32_IS(st, metric::kGpuMaxClockMhz, 900.0);

        // RAM, uptime, clock, own cost
        F32_IS(st, metric::kRamTotalGib, 16.0);
        F32_IS(st, metric::kRamUsedGib, 8.0);
        I32_IS(st, metric::kUptimeS, 8914);
        const MetricValue* clock = st.get(metric::kClock);
        CHECK(clock != nullptr && clock->ok && clock->text.size() == 19 && clock->text[4] == '-' &&
              clock->text[10] == ' ' && clock->text[13] == ':');
        const double pageBytes = static_cast<double>(::sysconf(_SC_PAGESIZE));
        F32_NEAR(st, metric::kSelfRssMib, 292.0 * pageBytes / (1024.0 * 1024.0), 1e-3);
        NA_IS(st, metric::kSelfCpuPct, Reason::NotSampledYet);  // first read: no delta yet

        // CPU: the first read only sets the baseline
        NA_IS(st, metric::kCpuUsagePct, Reason::NotSampledYet);
        NA_IS(st, metric::kCpuCore0Pct, Reason::NotSampledYet);

        // Second /proc/stat snapshot: rewrite in place (same inode, the open fd re-reads it)
        put(root, "proc/stat", kStatB);
        CHECK(rig.sched.runDue(10.5) > 0);
        F32_IS(st, metric::kCpuUsagePct, 25.0);
        F32_IS(st, metric::kCpuMaxCorePct, 50.0);
        F32_IS(st, metric::kCpuCore0Pct + 0, 50.0);
        F32_IS(st, metric::kCpuCore0Pct + 1, 0.0);
        // The file has cpu0 and cpu1 only: the other slots are cores this machine doesn't have
        for (uint16_t i = 2; i < metric::kCpuCoreSlots; ++i) NA_IS(st, metric::kCpuCore0Pct + i, Reason::SensorMissing);

        // The 2 s sources ran at 10.0 and aren't due yet; the 2nd self-cost read is at 12.0
        CHECK(rig.sched.runDue(12.0) > 0);
        const MetricValue* selfCpu = st.get(metric::kSelfCpuPct);
        CHECK(selfCpu != nullptr && selfCpu->ok && selfCpu->f32 >= 0.0f);
        F32_NEAR(st, metric::kSelfRssMib, 292.0 * pageBytes / (1024.0 * 1024.0), 1e-3);
    }
    removeTree(root);
}

void testPlugged() {
    const std::string root = makeTempRoot();
    TreeOptions o;
    o.plugged = true;
    buildTree(root, o);
    {
        Rig rig(root);
        rig.start(kAllSystemWidgets);
        const MetricStore& st = rig.store;
        CHECK(rig.sched.runDue(10.0) > 0);

        ENUM_IS(st, metric::kBatteryStatus, static_cast<uint32_t>(fph::BatteryStatus::Charging));
        ENUM_IS(st, metric::kBatteryChargerOnline, 1);
        ENUM_IS(st, metric::kBatteryPlugged, 1);
        F32_IS(st, metric::kBatteryCurrentMa, 1100.0);
        F32_IS(st, metric::kBatteryPowerW, 7.765936 * 1.1);
        CHECK(st.f32Or(metric::kBatteryPowerW, 0.0) > 0);
        TEXT_IS(st, metric::kBatteryChargerMode, "CC");
        TEXT_IS(st, metric::kBatteryUsbType, "PD");  // the bracketed entry of "C [PD] PD_PPS"
        F32_IS(st, metric::kBatteryPdOfferedV, 15.0);
        F32_IS(st, metric::kBatteryPdOfferedA, 3.0);
        F32_IS(st, metric::kBatteryPdOfferedW, 45.0);
        F32_IS(st, metric::kBatteryInputV, 14.2);
        F32_IS(st, metric::kBatteryInputA, 1.65);
        F32_IS(st, metric::kBatteryInputW, 14.2 * 1.65);
        F32_IS(st, metric::kBatterySagV, 0.8);
        // From current_avg (+1.0 A)
        I32_IS(st, metric::kBatteryTimeToFullS,
               static_cast<int32_t>(std::lround((2730 - 1534) / 1000.0 / 1.0 * 3600.0)));
        NA_IS(st, metric::kBatteryTimeToEmptyS, Reason::InvalidReading);
        // Power doesn't depend on the plug
        checkPower(st);
    }
    removeTree(root);
}

void testThrottle() {
    const std::string root = makeTempRoot();
    TreeOptions o;
    o.throttledCpu = true;
    buildTree(root, o);
    {
        Rig rig(root);
        rig.start({WidgetType::Temp});
        CHECK(rig.sched.runDue(1.0) > 0);
        ENUM_IS(rig.store, metric::kThrottleCpu, 1);
        ENUM_IS(rig.store, metric::kThrottleGpu, 0);
    }
    removeTree(root);
}

void testMissingFiles() {
    // No iio device and no power_supply: board temps can't be read, the battery isn't there at all
    {
        const std::string root = makeTempRoot();
        TreeOptions o;
        o.withIio = false;
        o.withPowerSupply = false;
        buildTree(root, o);
        {
            Rig rig(root);
            rig.start(kAllSystemWidgets);
            const MetricStore& st = rig.store;
            CHECK(rig.sched.runDue(10.0) > 0);

            NA_IS(st, metric::kTempDisplayC, Reason::ReadFailed);
            NA_IS(st, metric::kTempExhaustC, Reason::ReadFailed);
            NA_IS(st, metric::kTempHeatsinkC, Reason::ReadFailed);
            NA_IS(st, metric::kTempWifiC, Reason::ReadFailed);
            NA_IS(st, metric::kTempFlashC, Reason::ReadFailed);
            NA_IS(st, metric::kTempNrfC, Reason::ReadFailed);
            NA_IS(st, metric::kTempConnectorC, Reason::ReadFailed);
            // The rest of the Temp widget is untouched
            F32_IS(st, metric::kTempCpuC, 68.9);
            F32_IS(st, metric::kFanRpm, 3657);

            NA_IS(st, metric::kBatteryPct, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryVoltageV, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryCurrentMa, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryPowerW, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryStatus, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryChargerOnline, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryPlugged, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryChargeNowMah, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryTimeToFullS, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryTimeToEmptyS, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryChargerMode, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryHealth, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryCycles, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryUsbType, Reason::SensorMissing);
            NA_IS(st, metric::kBatterySagV, Reason::SensorMissing);
            NA_IS(st, metric::kTempBatteryC, Reason::SensorMissing);
            checkPower(st);
        }
        removeTree(root);
    }

    // A chip without in*_label: its rails' volts are N/A, their watts still fine
    {
        const std::string root = makeTempRoot();
        TreeOptions o;
        o.chip12HasInLabels = false;
        buildTree(root, o);
        {
            Rig rig(root);
            rig.start({WidgetType::Power});
            const MetricStore& st = rig.store;
            CHECK(rig.sched.runDue(10.0) > 0);
            F32_IS(st, metric::kPowerApc0W, 0.519995);
            F32_IS(st, metric::kPowerApc1W, 0.231063);
            F32_IS(st, metric::kPowerApc2W, 0.806278);
            F32_IS(st, metric::kPowerNsp1W, 0.201112);
            NA_IS(st, metric::kPowerApc0W + metric::kPowerRailVoltsOffset, Reason::ReadFailed);
            NA_IS(st, metric::kPowerApc1W + metric::kPowerRailVoltsOffset, Reason::ReadFailed);
            NA_IS(st, metric::kPowerApc2W + metric::kPowerRailVoltsOffset, Reason::ReadFailed);
            NA_IS(st, metric::kPowerNsp1W + metric::kPowerRailVoltsOffset, Reason::ReadFailed);
            // The other chips keep their volts
            F32_IS(st, metric::kPowerBobW + metric::kPowerRailVoltsOffset, 3.757);
            F32_IS(st, metric::kPowerVphV, 3.804);
            // And the tree is unaffected
            F32_NEAR(st, metric::kPowerRailsW, 4.471471, 1e-5);
            F32_NEAR(st, metric::kPowerUnmeasuredW, 9.188324 - 4.471471, 1e-5);
        }
        removeTree(root);
    }

    // Nothing at all under the root: every reader copes, nothing crashes
    {
        const std::string root = makeTempRoot();
        {
            Rig rig(root);
            rig.start(kAllSystemWidgets);
            const MetricStore& st = rig.store;
            CHECK(rig.sched.runDue(10.0) > 0);
            CHECK(rig.sched.runDue(10.5) > 0);
            CHECK(rig.sched.runDue(12.0) > 0);
            NA_IS(st, metric::kPowerVphW, Reason::SensorMissing);
            NA_IS(st, metric::kPowerRailsW, Reason::SensorMissing);
            NA_IS(st, metric::kPowerApc0W, Reason::SensorMissing);
            NA_IS(st, metric::kCpuCluster0Mhz, Reason::SensorMissing);
            NA_IS(st, metric::kBatteryPct, Reason::SensorMissing);
            NA_IS(st, metric::kTempCpuC, Reason::ReadFailed);
            NA_IS(st, metric::kTempDisplayC, Reason::ReadFailed);
            NA_IS(st, metric::kFanRpm, Reason::ReadFailed);
            NA_IS(st, metric::kRamTotalGib, Reason::ReadFailed);
            NA_IS(st, metric::kUptimeS, Reason::ReadFailed);
            NA_IS(st, metric::kSelfRssMib, Reason::ReadFailed);
            const MetricValue* clock = st.get(metric::kClock);
            CHECK(clock != nullptr && clock->ok);  // the clock doesn't come from the tree
        }
        removeTree(root);
    }
}

void testIntervals() {
    Scheduler sched;  // compiled-in registry
    const struct {
        SourceId source;
        double sec;
    } plan[] = {
        {SourceId::CpuLoad, 0.5},    {SourceId::CpuClock, 0.5}, {SourceId::GpuClock, 1.0},
        {SourceId::GpuBusy, 1.0},    {SourceId::Ram, 2.0},      {SourceId::Temps, 2.0},
        {SourceId::BoardTemps, 2.0}, {SourceId::Throttle, 2.0}, {SourceId::Fan, 2.0},
        {SourceId::Power, 2.0},      {SourceId::Battery, 2.0},  {SourceId::Uptime, 1.0},
        {SourceId::SelfCost, 2.0},
    };
    for (const auto& p : plan) {
        if (std::fabs(sched.interval(p.source) - p.sec) > 1e-9) {
            std::printf("FAIL interval of %s: expected %g s, got %g s\n", fph::sourceName(p.source), p.sec,
                        sched.interval(p.source));
            ++failures;
        }
    }
}

void runSteps(Rig& rig, double from, double to, double step) {
    for (double t = from; t <= to + 1e-9; t += step) rig.sched.runDue(t);
}

void testDemand() {
    const std::string root = makeTempRoot();
    buildTree(root, TreeOptions());
    {
        Rig rig(root);
        rig.start({WidgetType::Power});
        Scheduler& sc = rig.sched;
        CHECK(sc.refCount(SourceId::Power) == 1);
        CHECK(sc.refCount(SourceId::Battery) == 0);

        // 10 s of fake time at 2 Hz polling: Power's 2 s interval gives 5-6 reads, nothing else is read
        runSteps(rig, 100.0, 110.0, 0.5);
        const uint64_t powerReads = sc.readCount(SourceId::Power);
        CHECK(powerReads >= 5 && powerReads <= 6);
        CHECK(sc.readCount(SourceId::Battery) == 0);
        CHECK(sc.readCount(SourceId::CpuLoad) == 0);
        CHECK(sc.readCount(SourceId::Temps) == 0);
        CHECK(sc.readCount(SourceId::Uptime) == 0);
        F32_IS(rig.store, metric::kPowerVphW, 9.188324);
        NA_IS(rig.store, metric::kBatteryPct, Reason::NotSampledYet);  // never read

        // Nobody looking: no more reads
        sc.setDemand(fph::DemandInputs());
        CHECK(sc.refCount(SourceId::Power) == 0);
        int ran = 0;
        for (double t = 111.0; t <= 120.0; t += 0.5) ran += sc.runDue(t);
        CHECK(ran == 0);
        CHECK(sc.readCount(SourceId::Power) == powerReads);

        // Back, with only Battery enabled: Battery reads every 2 s, Power stays quiet
        sc.setEnabledWidgets({WidgetType::Battery});
        fph::DemandInputs d;
        d.hudShown = true;
        d.hmdInUse = true;
        sc.setDemand(d);
        CHECK(sc.refCount(SourceId::Battery) >= 1);
        CHECK(sc.refCount(SourceId::Power) == 0);
        runSteps(rig, 121.0, 131.0, 0.5);
        const uint64_t batteryReads = sc.readCount(SourceId::Battery);
        CHECK(batteryReads >= 5 && batteryReads <= 6);
        CHECK(sc.readCount(SourceId::Power) == powerReads);
        F32_IS(rig.store, metric::kBatteryPct, 56);

        // Headset off the head (hmd not in use), HUD on: not looking either
        d.hmdInUse = false;
        sc.setDemand(d);
        CHECK(sc.refCount(SourceId::Battery) == 0);
        const uint64_t before = sc.readCount(SourceId::Battery);
        runSteps(rig, 132.0, 140.0, 0.5);
        CHECK(sc.readCount(SourceId::Battery) == before);
    }
    removeTree(root);
}

}  // namespace

int main() {
    testUnplugged();
    testPlugged();
    testThrottle();
    testMissingFiles();
    testIntervals();
    testDemand();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("system-sources: all checks passed\n");
    return 0;
}
