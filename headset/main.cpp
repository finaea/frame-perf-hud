// frame-perf-hud: a performance HUD for the Steam Frame (frame timing, temperatures, power, ...).
// Read-only: no writes to sysfs, no sudo, no camera access.
// Forked from frame-perf-overlay by sasaken1102r (MIT).
#include "autostart.h"
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "metric_store.h"
#include "panel.h"
#include "paths.h"
#include "scheduler.h"
#include "sensor_bridge.h"
#include "settings_panel.h"
#include "theme.h"
#include "sensors.h"
#include "update_check.h"  // vendor/frame-updater/cpp
#include "vr_overlay.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// From project(VERSION) in CMakeLists.txt
#ifndef FRAME_PERF_HUD_VERSION
#define FRAME_PERF_HUD_VERSION "unknown"
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gToggleRequested = 0;  ///< SIGUSR1 (sent by a second launch): show / hide the HUD
volatile std::sig_atomic_t gDumpRequested = 0;    ///< SIGUSR2 (sent by --debug-dump): write the debug dump
int gWakeFd = -1;  ///< eventfd the signal handlers poke, so poll() in the main loop returns at once

/** Exit code when the user quits from the dashboard (systemd's RestartPreventExitStatus=). */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;              ///< dashboard thumbnail side (px)
constexpr double kSettingsPollSec = 0.05;        ///< input deadline while the pointer is active on the dashboard tab
constexpr double kSettingsIdlePollSec = 0.1;     ///< input deadline while the tab is visible but idle
constexpr double kSettingsActiveHoldSec = 3.0;   ///< the pointer counts as active this long after its last event
constexpr double kMetricsTickSec = 0.5;          ///< metrics deadline: events, config, demand, due sources (2 Hz)
constexpr double kMinPanelIntervalSec = 0.5;     ///< the panel never redraws faster than 2 Hz
constexpr double kAutostartRefreshSec = 3.0;     ///< re-read the autostart state this often while the tab is visible
constexpr double kPlacementCoalesceSec = 0.02;   ///< do a placement check this early if we're awake anyway

/** Command line. */
struct Options {
    enum class Mode {
        Overlay, Print, DumpPng, DumpSettingsPng, DumpFrameTimings, DebugDump, ContrastReport, Help, Version
    };
    Mode mode = Mode::Overlay;
    std::string configPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    int thumbnailSize = 256;  ///< side of the --thumbnail-png image (px)
    int printCount = 5;
    double dumpSeconds = 4.0;
    double timingSeconds = 5.0;
    bool timingSecondsGiven = false;
    bool secondsGiven = false;
    bool fakeThrottle = false;
    bool previewQuit = false;            ///< --dump-settings-png: draw the "press again to quit" state
    bool forceSettingsVisible = false;   ///< diagnostics: run as if the dashboard tab were visible
    std::string previewAutostart;        ///< --dump-settings-png: draw autostart in this state
    std::string previewUpdate;           ///< --dump-settings-png: draw the update check in this state
    bool fakeFrames = false;
    bool fakeWarnings = false;
    double fakeFps = 0.0;
    std::string fakeWifi;  ///< non-empty: fake the wireless link (dBm, "home:dBm" or "none")
    bool verbose = false;
};

void onSignal(int /*signal*/) {
    gStopRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

void onToggleSignal(int /*signal*/) {
    gToggleRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

void onDumpSignal(int /*signal*/) {
    gDumpRequested = 1;
    const uint64_t one = 1;
    if (gWakeFd >= 0) (void)!::write(gWakeFd, &one, sizeof(one));
}

/** SIGTERM / SIGINT stop cleanly, SIGUSR1 toggles the HUD, SIGUSR2 writes the debug dump. */
void installSignalHandlers() {
    gWakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const auto install = [](int signal, void (*handler)(int)) {
        struct sigaction action {};
        action.sa_handler = handler;
        sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr);
    };
    install(SIGUSR1, onToggleSignal);
    install(SIGUSR2, onDumpSignal);
    install(SIGTERM, onSignal);
    install(SIGINT, onSignal);
}

/** @return monotonic seconds */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** @return CPU time used by this thread, seconds (for --verbose) */
double cpuSeconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/**
 * Sleeps in one poll() until the timeout, a signal, or the camera eventfd fires.
 * @param seconds timeout; +infinity waits until something happens
 * @param cameraFd the camera thread's eventfd, or -1 (a stub until ticket 10)
 * @return true if the camera eventfd fired
 */
bool waitForWake(double seconds, int cameraFd = -1) {
    if (gStopRequested || gToggleRequested || gDumpRequested) return false;
    struct pollfd fds[2] = {{gWakeFd, POLLIN, 0}, {cameraFd, POLLIN, 0}};
    int timeoutMs = -1;
    if (std::isfinite(seconds)) timeoutMs = static_cast<int>(std::ceil(std::max(0.0, seconds) * 1000.0));
    const int n = ::poll(fds, 2, timeoutMs);
    uint64_t value = 0;
    if (n > 0 && (fds[0].revents & POLLIN)) (void)!::read(gWakeFd, &value, sizeof(value));
    if (n > 0 && cameraFd >= 0 && (fds[1].revents & POLLIN)) {
        (void)!::read(cameraFd, &value, sizeof(value));
        return true;
    }
    return false;
}

/** Waits until the time is up or a stop is requested (for --print and --dump-png). */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        waitForWake(left);
        gToggleRequested = 0;
        gDumpRequested = 0;
    }
}

/** @return local time as "YYYY-MM-DD HH:MM:SS" */
std::string localTimeText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

/** @return the value with the given decimals, "--" for NaN */
std::string fmt(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

void printUsage() {
    std::printf(
        "Usage: frame-perf-hud [options]\n"
        "  (none)               run as a SteamVR overlay (waits for SteamVR if it isn't running)\n"
        "  --print [--count N]  without OpenVR, print the readings every second, N times (default 5), and exit\n"
        "  --dump-png PATH      without OpenVR, render the panel to a PNG and exit\n"
        "      --seconds S      collect readings for S seconds first (default 4) so the graphs have data\n"
        "      --fake-frames    fill in fake frame timings and controllers (to check the look)\n"
        "      --fake-warnings  fake bad values (thermal limit, weak signal, low battery) to check warning colours\n"
        "      --fake-fps N     fake the app's fps around N, to see what a drop looks like\n"
        "      --fake-wifi D    fake the dongle link (D = signal in dBm, none = not connected, home:D = home Wi-Fi)\n"
        "      --fake-throttle  fake SteamVR throttling the app to half rate (36 fps at 72 Hz, 50%% reprojection)\n"
        "  --dump-settings-png PATH  without OpenVR, render the dashboard tab to a PNG and exit\n"
        "      --thumbnail-png PATH  also write the dashboard thumbnail (same image as the launcher icon)\n"
        "      --thumbnail-size N    side of that thumbnail (default 256)\n"
        "      --preview-quit   draw the \"press again to quit\" state\n"
        "      --preview-autostart S  draw autostart in this state (enabled / disabled / notinstalled / busy / failed)\n"
        "      --preview-update S     draw the update check in this state (uptodate / checking / available / manual /\n"
        "                             confirm / installing / installed / checkfailed / installfailed; default uptodate)\n"
        "  --config PATH        config file (default ~/.config/frame-perf-hud/config.json)\n"
        "  --verbose            while running as an overlay, log the values every few seconds\n"
        "  --force-settings-visible  diagnostics: run as if the dashboard tab were visible (to measure the cost)\n"
        "  --debug-dump         print the running instance's debug dump (sources, reference counts, read counters,\n"
        "                       every metric with its status) and exit\n"
        "  --contrast-report    print the WCAG contrast ratio of every text/background colour pair\n"
        "  --dump-frame-timings [S]  diagnostics: connect to SteamVR and print raw frame timings for S seconds\n"
        "                       (default 5). Read-only (Background app type), so it can run next to the overlay\n"
        "  --version            print the version and exit\n");
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if (arg == "--count" && hasNext) {
            options.printCount = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-settings-png" && hasNext) {
            options.mode = Options::Mode::DumpSettingsPng;
            options.pngPath = argv[++i];
        } else if (arg == "--dump-frame-timings") {
            options.mode = Options::Mode::DumpFrameTimings;
            if (hasNext && argv[i + 1][0] != '-') {
                options.timingSeconds = std::fmax(0.5, std::atof(argv[++i]));
                options.timingSecondsGiven = true;
            }
        } else if (arg == "--debug-dump") {
            options.mode = Options::Mode::DebugDump;
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--seconds" && hasNext) {
            options.dumpSeconds = std::fmax(0.0, std::atof(argv[++i]));
            options.secondsGiven = true;
        } else if (arg == "--fake-frames") {
            options.fakeFrames = true;
        } else if (arg == "--fake-fps" && hasNext) {
            options.fakeFps = std::fmax(1.0, std::atof(argv[++i]));
        } else if (arg == "--preview-autostart" && hasNext) {
            options.previewAutostart = argv[++i];
        } else if (arg == "--preview-update" && hasNext) {
            options.previewUpdate = argv[++i];
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--force-settings-visible") {
            options.forceSettingsVisible = true;
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--fake-throttle") {
            options.fakeThrottle = true;
        } else if (arg == "--fake-wifi" && hasNext) {
            options.fakeWifi = argv[++i];
        } else if (arg == "--fake-warnings") {
            options.fakeWarnings = true;
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    // Also accept --dump-frame-timings --seconds S
    if (options.mode == Options::Mode::DumpFrameTimings && options.secondsGiven && !options.timingSecondsGiven) {
        options.timingSeconds = std::fmax(0.5, options.dumpSeconds);
    }
    return true;
}

/** Loads the fonts named in the config, or the bundled Noto Sans when they're empty. */
void loadFonts(FontSet& fonts, const Config& config) {
    fonts.load(paths::resolveFontPath(config.fontPath, false), paths::resolveFontPath(config.boldFontPath, true));
}

/** Prints one sample for --print. */
void printSample(const SensorSample& s) {
    std::printf("  CPU      usage %s%% (max core %s%%)  fastest cluster %s MHz  big core %s MHz\n",
                fmt(s.cpuUsagePct, 1).c_str(), fmt(s.cpuMaxCorePct, 1).c_str(), fmt(s.cpuFreqMHz, 0).c_str(),
                fmt(s.cpuBigFreqMHz, 0).c_str());
    std::printf("  GPU      busy %s%% (estimate)  %s MHz (max %s MHz)\n", fmt(s.gpuBusyPct, 1).c_str(),
                fmt(s.gpuFreqMHz, 0).c_str(), fmt(s.gpuMaxFreqMHz, 0).c_str());
    std::printf("  Temp     CPU %s °C  GPU %s °C  DDR %s °C  battery %s °C\n", fmt(s.cpuTempC, 1).c_str(),
                fmt(s.gpuTempC, 1).c_str(), fmt(s.ddrTempC, 1).c_str(), fmt(s.batteryTempC, 1).c_str());
    std::printf("           display %s °C  exhaust %s °C  heatsink %s °C   thermal limit CPU %s / GPU %s\n",
                fmt(s.displayTempC, 1).c_str(), fmt(s.exhaustTempC, 1).c_str(), fmt(s.heatsinkTempC, 1).c_str(),
                s.throttleCpu ? "yes" : "no", s.throttleGpu ? "yes" : "no");
    if (!s.wifi.interfaceUp) {
        std::printf("  Wireless no interface (neither the dongle AP nor a home Wi-Fi station)\n");
    } else if (!s.wifi.connected) {
        std::printf("  Wireless not connected (no peer on the dongle, not on home Wi-Fi)\n");
    } else {
        // The dongle's peer is the PC; home Wi-Fi's is the access point
        std::printf("  %s rx %s Mbps  tx %s Mbps  link tx %s / rx %s Mbps  signal %s dBm\n",
                    s.wifi.homeWifi ? "Wi-Fi   " : "Dongle  ", fmt(s.wifi.rxMbps, 2).c_str(),
                    fmt(s.wifi.txMbps, 2).c_str(), fmt(s.wifi.txLinkMbps, 1).c_str(),
                    fmt(s.wifi.rxLinkMbps, 1).c_str(), fmt(s.wifi.signalDbm, 0).c_str());
    }
    std::printf("  Power    vph %s W  all channels %s W\n          ", fmt(s.powerMainW, 3).c_str(),
                fmt(s.powerSumW, 3).c_str());
    for (const auto& ch : s.powerChannels) std::printf(" %s=%s", ch.label.c_str(), fmt(ch.watts, 3).c_str());
    std::printf("\n  Fan      %s rpm\n", fmt(s.fanRpm, 0).c_str());
    std::printf("  Battery  %s%%  %s V  %s mA  %s (charger %s)\n", fmt(s.batteryPct, 0).c_str(),
                fmt(s.batteryVoltageV, 3).c_str(), fmt(s.batteryCurrentMa, 0).c_str(), s.batteryStatus.c_str(),
                s.chargerOnline ? "online" : "offline");
    std::printf("  Memory   %s / %s GiB\n", fmt(s.memUsedGiB, 2).c_str(), fmt(s.memTotalGiB, 2).c_str());
}

/** Demand for the offline modes (--print, --dump-png): someone is "looking" at the terminal / PNG. */
fph::DemandInputs offlineDemand() {
    fph::DemandInputs demand;
    demand.hudShown = true;
    demand.hmdInUse = true;
    return demand;
}

/** --print: print the readings a few times without OpenVR. */
int runPrint(const Options& options) {
    Sensors sensors;
    sensors.discover();
    std::printf("== Sensors found ==\n%s\n", sensors.describe().c_str());
    fph::MetricStore store;
    fph::Scheduler scheduler;
    SensorBridge bridge(sensors, nullptr, store);
    bridge.registerSources(scheduler);
    scheduler.setEnabledWidgets(SensorBridge::upstreamPanelWidgets());
    scheduler.setDemand(offlineDemand());
    scheduler.runDue(nowSeconds());  // CPU usage baseline
    // Reads every source at least once per print (some run every 2 or 5 s, like on the panel)
    for (int i = 1; i <= options.printCount && !gStopRequested; ++i) {
        sleepInterruptible(1.0);
        scheduler.runDue(nowSeconds());
        std::printf("== [%d/%d] %s ==\n", i, options.printCount, localTimeText().c_str());
        printSample(bridge.sample());
        std::fflush(stdout);
    }
    return 0;
}

/** Fake frame timings to check the look. fps > 0 holds the app's fps around that value. */
FrameStats fakeFrameStats(double t, double fps) {
    FrameStats f;
    f.valid = true;
    f.haveTimes = true;
    f.displayHz = 90.0;
    f.targetMs = 1000.0 / 90.0;
    // Around 90 unless given, with an occasional small dip
    f.appFps = fps > 0 ? fps + 1.5 * std::sin(t * 2.1) : 90.0 - (std::fmod(t, 7.0) < 0.6 ? 6.0 : 0.3 * std::fabs(std::sin(t)));
    f.gpuMs = 8.0 + 1.5 * std::sin(t * 1.3) + (std::fmod(t, 7.0) < 0.6 ? 4.0 : 0.0);
    f.gpuMaxMs = f.gpuMs + 1.0;
    f.cpuMs = 5.0 + 1.0 * std::sin(t * 0.7 + 1.0);
    f.cpuMaxMs = f.cpuMs + 0.8;
    f.frames = 45;
    f.reprojectedPct = f.gpuMs > f.targetMs ? 12.0 : 0.0;
    if (fps > 0) {
        // A dropping-fps picture: heavy GPU, lots of reprojection
        f.gpuMs = 14.5 + 1.0 * std::sin(t * 1.3);
        f.reprojectedPct = 100.0 * (1.0 - fps / 90.0);
    }
    f.dropped = 0;
    return f;
}

/** Fake controller batteries (left charging). low = the low-battery picture. */
ControllerStatus fakeControllers(bool low) {
    ControllerStatus status;
    status.left.present = true;
    status.left.pct = low ? 8.0 : 80.0;
    status.left.charging = true;
    status.right.present = !low;  // the low-battery picture shows the right hand as not connected
    status.right.pct = 75.0;
    return status;
}

/** Replaces some readings with bad values to check the warning colours. */
void applyFakeWarnings(SensorSample& sample) {
    sample.throttleCpu = true;
    sample.throttleGpu = true;
    sample.gpuBusyPct = 97.0;
    sample.cpuTempC = 86.0;
    sample.batteryPct = 12.0;
    sample.batteryStatus = "Discharging";
    sample.chargerOnline = false;
    sample.wifi.interfaceUp = true;
    sample.wifi.connected = true;
    sample.wifi.signalDbm = -81.0;
    sample.wifi.rxMbps = 42.5;
    sample.wifi.txMbps = 1.2;
    sample.wifi.txLinkMbps = 288.2;
    sample.wifi.rxLinkMbps = 216.1;
}

/** Fakes SteamVR throttling the app to half rate (values seen in Half-Life: Alyx). */
void applyFakeThrottle(FrameStats& frame, double t) {
    frame.displayHz = 72.0;
    frame.targetMs = 1000.0 / 72.0;
    frame.appFps = 36.0 - (std::fmod(t, 6.0) < 0.5 ? 2.0 : 0.0);
    frame.gpuMs = 22.0 + 1.0 * std::sin(t * 1.3);
    frame.cpuMs = 4.4 + 0.5 * std::sin(t * 0.7);
    frame.reprojectedPct = 50.0;
    frame.dropped = 0;
    frame.throttledFrames = 1;
}

/** Fakes the wireless link: "-65" (dongle), "home:-47" (home Wi-Fi) or "none" (not connected). */
void applyFakeWifi(SensorSample& sample, const std::string& value) {
    sample.wifi = WifiInfo();
    sample.wifi.interfaceUp = true;
    if (value == "none") return;
    sample.wifi.connected = true;
    const bool home = value.rfind("home:", 0) == 0;
    sample.wifi.homeWifi = home;
    sample.wifi.signalDbm = std::atof(value.c_str() + (home ? 5 : 0));
    sample.wifi.rxMbps = 187.4;
    sample.wifi.txMbps = 2.1;
    sample.wifi.txLinkMbps = 1152.8;
    sample.wifi.rxLinkMbps = 960.7;
}

/** Update checker settings (install.sh puts the script in ~/.local/share/frame-perf-hud/). */
frame_updater::UpdaterConfig makeUpdaterConfig() {
    frame_updater::UpdaterConfig cfg;
    cfg.script = paths::dataHome() + "/frame-perf-hud/frame-update.sh";
    cfg.app = "frame-perf-hud";
    cfg.repo = "finaea/frame-perf-hud";
    cfg.currentVersion = FRAME_PERF_HUD_VERSION;
    cfg.assetPattern = "frame-perf-hud-{version}.tar.gz";
    return cfg;
}

/** A fake update-check state for --preview-update (unknown names give uptodate). */
frame_updater::UpdateStatus fakeUpdateStatus(const std::string& name) {
    using frame_updater::UpdateState;
    frame_updater::UpdateStatus s;
    s.current = FRAME_PERF_HUD_VERSION;
    s.latest = FRAME_PERF_HUD_VERSION;
    s.checkedAt = std::time(nullptr);
    if (name == "available" || name == "confirm") {
        s.state = UpdateState::Available;
        s.latest = std::string(FRAME_PERF_HUD_VERSION) + "-preview";
        s.installable = true;
        s.url = "https://github.com/finaea/frame-perf-hud/releases/tag/v" + s.latest;
    } else if (name == "manual") {
        s.state = UpdateState::Available;
        s.latest = std::string(FRAME_PERF_HUD_VERSION) + "-preview";
        s.installable = false;
        s.reason = "no-checksums";
    } else if (name == "checking") {
        s.state = UpdateState::Unknown;  // just started, no answer yet
        s.checking = true;
    } else if (name == "installing") {
        s.state = UpdateState::Installing;
        s.version = s.latest;
        s.step = "download";
    } else if (name == "installed") {
        s.state = UpdateState::Installed;
        s.version = s.latest;
    } else if (name == "checkfailed") {
        s.state = UpdateState::CheckFailed;
        s.error = "network";
    } else if (name == "installfailed") {
        s.state = UpdateState::InstallFailed;
        s.version = s.latest;
        s.error = "checksum-mismatch";
    } else {
        s.state = UpdateState::UpToDate;
    }
    return s;
}

/** --dump-png: collect readings without OpenVR, render the panel and write a PNG. */
int runDumpPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    loadFonts(fonts, config);
    PanelRenderer renderer(fonts);
    PanelState state;
    fph::MetricStore store;
    fph::Scheduler scheduler;
    SensorBridge bridge(sensors, nullptr, store);
    bridge.registerSources(scheduler);
    scheduler.setEnabledWidgets(SensorBridge::upstreamPanelWidgets());
    scheduler.setDemand(offlineDemand());

    scheduler.runDue(nowSeconds());  // CPU usage baseline
    const double interval = std::max(kMinPanelIntervalSec, config.updateIntervalMs / 1000.0);
    const double start = nowSeconds();
    do {
        sleepInterruptible(interval);
        const double t = nowSeconds();
        scheduler.runDue(t);
        const bool fake = options.fakeFrames || options.fakeWarnings || options.fakeFps > 0 || options.fakeThrottle;
        FrameStats frame = fake ? fakeFrameStats(t - start, options.fakeFps) : FrameStats();
        if (options.fakeThrottle) applyFakeThrottle(frame, t - start);
        SensorSample sample = bridge.sample();
        if (options.fakeWarnings) applyFakeWarnings(sample);
        if (!options.fakeWifi.empty()) applyFakeWifi(sample, options.fakeWifi);
        state.update(t, sample, frame, fake, config.graphSeconds);
        if (fake) state.controllers = fakeControllers(options.fakeWarnings);
    } while (!gStopRequested && nowSeconds() - start < options.dumpSeconds);

    renderer.render(state, config);
    if (!renderer.writePng(options.pngPath)) {
        std::fprintf(stderr, "Couldn't write the PNG: %s\n", options.pngPath.c_str());
        return 1;
    }
    std::printf("Wrote %s (%dx%d)\n", options.pngPath.c_str(), renderer.width(), renderer.visibleHeight());
    return 0;
}

/** --dump-settings-png: render the dashboard tab (and thumbnail) without OpenVR. */
int runDumpSettingsPng(const Options& options) {
    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    FontSet fonts;
    loadFonts(fonts, config);
    SettingsPanel panel(fonts);
    if (options.previewQuit) panel.armQuitForPreview();
    // Autostart: the real state (read-only), unless a preview state is given
    Autostart autostart;
    AutostartStatus autostartView = autostart.status();
    const std::string& preview = options.previewAutostart;
    if (preview == "enabled") autostartView = {AutostartStatus::State::Enabled, false, false};
    if (preview == "disabled") autostartView = {AutostartStatus::State::Disabled, false, false};
    if (preview == "notinstalled") autostartView = {AutostartStatus::State::NotInstalled, false, false};
    if (preview == "busy") autostartView = {AutostartStatus::State::Enabled, true, false};
    if (preview == "failed") autostartView = {AutostartStatus::State::Enabled, false, true};
    const frame_updater::UpdateStatus updateView = fakeUpdateStatus(options.previewUpdate);
    if (options.previewUpdate == "confirm") panel.armUpdateConfirmForPreview();
    panel.render(config, autostartView, updateView);
    if (!panel.writePng(options.pngPath)) {
        std::fprintf(stderr, "Couldn't write the PNG: %s\n", options.pngPath.c_str());
        return 1;
    }
    std::printf("Wrote %s (%dx%d)\n", options.pngPath.c_str(), panel.width(), panel.height());
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("Wrote the thumbnail %s (%dx%d)\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * --dump-frame-timings: connect as a Background app (fails instead of starting SteamVR) and print raw
 * frame timings. Creates no overlay and no Vulkan device, so it can run next to the resident instance.
 */
int runDumpFrameTimings(const Options& options) {
    VrOverlay vr;
    std::string message;
    if (!vr.connectReadOnly(message)) {
        std::fprintf(stderr, "[VR] Can't connect: %s\n", message.c_str());
        return 1;
    }
    vr.dumpFrameTimings(options.timingSeconds, gStopRequested);
    vr.shutdown();
    return 0;
}

/** Applies a dashboard action to the config, to the HUD, and saves the config file. */
void handleSettingsAction(SettingsAction action, Config& config, ConfigWatcher& watcher, VrOverlay& vr,
                          double angleStepDeg = 1.0) {
    if (!applySettingsAction(action, config, angleStepDeg)) return;
    vr.applyConfig(config);
    std::string error;
    if (saveConfig(watcher.path(), config, error)) {
        watcher.noteSaved();
    } else {
        std::fprintf(stderr, "[config] Saving failed: %s\n", error.c_str());
    }
}

/** @return the single-instance lock file path */
std::string lockFilePath() {
    const std::string dir = paths::runtimeDir();
    if (dir == "/tmp") return "/tmp/frame-perf-hud-" + std::to_string(::getuid()) + ".lock";
    return dir + "/frame-perf-hud.lock";
}

/**
 * Takes the single-instance lock. On success writes our PID and keeps the file open (the OS releases the
 * lock when we exit). Otherwise returns the PID of the instance holding it.
 * @param lockFd the open lock file on success
 * @param holderPid the holder's PID on failure (0 if unreadable)
 * @return true if we hold the lock (or locking isn't possible, so just start)
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[startup] Can't open the lock file %s; starting without the single-instance check\n",
                     path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[startup] Couldn't write our PID to the lock file\n");
        }
        lockFd = fd;
        return true;
    }
    // The holder may have just taken the lock and not written its PID yet: retry the read briefly
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * Whether we were started as our own systemd unit. INVOCATION_ID is inherited by children of Steam
 * (also a unit) started with +, so check whether our cgroup is this app's unit instead.
 */
bool runningAsService() {
    std::ifstream file("/proc/self/cgroup");
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("/frame-perf-hud.service") != std::string::npos) return true;
    }
    return false;
}

/** Handles a pending SIGUSR1 like the dashboard's On / Off. @return true if it toggled */
bool handleToggleRequest(Config& config, ConfigWatcher& watcher, VrOverlay& vr) {
    if (!gToggleRequested) return false;
    gToggleRequested = 0;
    handleSettingsAction(config.visible ? SettingsAction::ShowOff : SettingsAction::ShowOn, config, watcher, vr);
    std::fprintf(stderr, "[startup] A second launch asked to toggle: the HUD is now %s\n",
                 config.visible ? "shown" : "hidden");
    return true;
}

/** @return a readable name for an EDeviceActivityLevel value */
const char* activityName(int level) {
    switch (level) {
        case -1: return "unknown";
        case 0: return "idle";
        case 1: return "user interaction";
        case 2: return "user interaction timeout";
        case 3: return "standby";
        case 4: return "idle timeout";
        default: return "?";
    }
}

/** @return true unless the HMD reports standby (off-head reads standby; unknown counts as in use) */
bool hmdInUse(int activityLevel) {
    return activityLevel != 3;  // vr::k_EDeviceActivityLevel_Standby
}

/**
 * Writes the debug dump (signals, sources with reference and read counters, every metric) to
 * paths::debugDumpPath(), via a temp file + rename so a reader never sees half of it.
 */
void writeDebugDump(const fph::Scheduler& scheduler, const fph::MetricStore& store, int activityLevel,
                    bool settingsVisible, double now) {
    std::string text = "frame-perf-hud " FRAME_PERF_HUD_VERSION " debug dump, pid " + std::to_string(::getpid()) +
                       ", " + localTimeText() + "\n";
    text += "hmd activity level: " + std::to_string(activityLevel) + " (" + activityName(activityLevel) +
            "), dashboard tab visible: " + (settingsVisible ? "yes" : "no") + "\n\n";
    text += scheduler.dump(now);
    text += "\n";
    text += store.dump(now);
    const std::string path = paths::debugDumpPath();
    const std::string tmp = path + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[debug] Can't write %s\n", tmp.c_str());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) std::fprintf(stderr, "[debug] Can't rename %s\n", tmp.c_str());
}

/** --debug-dump: ask the running instance for its dump (SIGUSR2) and print it. */
int runDebugDump() {
    int lockFd = -1;
    pid_t holderPid = 0;
    if (acquireInstanceLock(lockFd, holderPid)) {
        if (lockFd >= 0) ::close(lockFd);
        std::fprintf(stderr, "frame-perf-hud isn't running\n");
        return 1;
    }
    if (holderPid <= 0) {
        std::fprintf(stderr, "frame-perf-hud is running, but its PID couldn't be read from %s\n",
                     lockFilePath().c_str());
        return 1;
    }
    const std::string path = paths::debugDumpPath();
    std::remove(path.c_str());
    if (::kill(holderPid, SIGUSR2) != 0) {
        std::fprintf(stderr, "Couldn't signal PID %d: %s\n", static_cast<int>(holderPid), std::strerror(errno));
        return 1;
    }
    for (int i = 0; i < 30; ++i) {
        std::ifstream file(path);
        if (file) {
            std::printf("%s", std::string(std::istreambuf_iterator<char>(file), {}).c_str());
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "No dump from PID %d after 3 s (%s)\n", static_cast<int>(holderPid), path.c_str());
    return 1;
}

/**
 * Runs as the overlay. Waits for SteamVR, and exits quietly on SIGTERM.
 * If another instance runs, tells it to toggle the HUD and exits at once (no VR_Init).
 *
 * The loop sleeps in one poll() with the timeout at the earliest of these deadlines:
 * - metrics (2 Hz): SteamVR events, config reload, "someone looking" signals, due sources, HUD redraw,
 * - input (50 / 100 ms): dashboard pointer events, only while the dashboard tab is visible,
 * - placement: upstream's wrist fade (1/30 s while fading), only when attached to a hand,
 * - camera: an eventfd the camera thread will signal (ticket 10; never fires yet).
 * Signals wake it through the same poll().
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    bool locked = acquireInstanceLock(lockFd, holderPid);
    if (!locked && runningAsService()) {
        // The service never toggles (Restart=always would toggle over and over). Wait for the manual one to exit
        std::fprintf(stderr, "[startup] Already running outside the service (PID %d). Waiting for it to exit\n",
                     static_cast<int>(holderPid));
        while (!locked && !gStopRequested) {
            waitForWake(2.0);
            gToggleRequested = 0;
            gDumpRequested = 0;
            locked = acquireInstanceLock(lockFd, holderPid);
        }
        if (!locked) return 0;
        std::fprintf(stderr, "[startup] The other instance exited; starting\n");
    }
    if (!locked) {
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[startup] Already running (PID %d). Toggled its HUD; exiting\n",
                         static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[startup] Seems to be running already, but couldn't signal it (PID %d)\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    ConfigWatcher watcher(options.configPath);
    Config config = watcher.loadInitial();
    Sensors sensors;
    sensors.discover();
    FontSet fonts;
    loadFonts(fonts, config);
    PanelRenderer renderer(fonts);
    SettingsPanel settings(fonts);
    Autostart autostart;  // autostart (systemd user unit): state and switching
    frame_updater::UpdateChecker updater(makeUpdaterConfig());
    std::uint64_t lastUpdaterRevision = updater.revision();
    PanelState state;
    VrOverlay vr;
    vr.setVerbose(options.verbose);

    // Registry + scheduler: nothing is read until the demand says someone is looking
    fph::MetricStore store;
    fph::Scheduler scheduler;
    SensorBridge bridge(sensors, &vr, store);
    bridge.registerSources(scheduler);
    scheduler.setEnabledWidgets(SensorBridge::upstreamPanelWidgets());
    const int cameraFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);  // stub: the camera thread arrives in ticket 10

    // Wait for SteamVR
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result =
            vr.connect(renderer.width(), renderer.height(), settings.width(), settings.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR isn't running; waiting (retrying every 3 s)\n");
            } else {
                std::fprintf(stderr, "[VR] Connecting failed: %s (retrying in 3 s)\n", message.c_str());
            }
            lastMessage = message;
        }
        const double retryAt = nowSeconds() + 3.0;
        while (!gStopRequested && nowSeconds() < retryAt) {
            waitForWake(retryAt - nowSeconds());
            handleToggleRequest(config, watcher, vr);  // toggles still work while waiting
            if (gDumpRequested) {
                gDumpRequested = 0;
                writeDebugDump(scheduler, store, -1, false, nowSeconds());
            }
        }
    }
    if (gStopRequested) return 0;
    std::fprintf(stderr, "[VR] Connected to SteamVR\n");
    vr.applyConfig(config);
    if (vr.hasSettings()) {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
    }

    double nextPanel = nowSeconds();
    double nextMetricsTick = 0.0;
    double lastVerbose = 0.0;
    bool settingsDirty = true;
    bool settingsVisible = false;
    bool settingsWasVisible = false;
    double lastAutostartRefresh = 0.0;
    double lastPointerEvent = -1e9;  // last dashboard pointer event (poll faster while it's active)
    int activityLevel = -2;          // last HMD activity level we logged (-2 = none yet)
    bool userQuit = false;
    bool panelDrawnWithClock = true;  // the last panel had the clock row (the texture is created at that height)
    unsigned loopCount = 0;  // --verbose: loop iterations since the last log line
    while (!gStopRequested && !userQuit) {
        ++loopCount;
        const double loopStart = nowSeconds();
        // HUD placement and fade (wrist fade, hide when tracking is lost, retry refused calls).
        // Done early if we're awake for something else anyway, to wake less often
        if (loopStart >= vr.nextPlacementAt() - kPlacementCoalesceSec) vr.updatePlacement(config, loopStart);

        // ---- Metrics deadline (2 Hz) ----
        // A toggle (SIGUSR1) is handled on whatever wake it caused: waitForWake() returns at once while the flag
        // is set, so leaving it for the next tick would spin
        settingsDirty |= handleToggleRequest(config, watcher, vr);
        const bool metricsTick = loopStart >= nextMetricsTick;
        if (metricsTick) {
            nextMetricsTick = loopStart + kMetricsTickSec;
            if (watcher.reloadIfChanged(config)) {
                vr.applyConfig(config);
                loadFonts(fonts, config);
                settingsDirty = true;
            }
            // Update check: only goes to GitHub when the script's cache has expired
            updater.tick(config.updateCheck);
            if (updater.revision() != lastUpdaterRevision) {
                lastUpdaterRevision = updater.revision();
                settingsDirty = true;
            }
        }
        // ---- Input deadline: dashboard events only while the tab is visible; SteamVR events on the tick ----
        const VrEvents events = metricsTick || settingsVisible ? vr.pollEvents(metricsTick) : VrEvents {};
        // SteamVR itself quitting (VREvent_Quit) exits with 0. Close on the dashboard icon (VREvent_OverlayClosed)
        // is the user quitting, like Quit on the tab: exit code 3, so systemd doesn't restart us
        if (events.quit) break;
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] Closed from the dashboard; exiting\n");
            userQuit = true;
            break;
        }
        if (metricsTick && !vr.steamVrAlive()) {
            std::fprintf(stderr, "[VR] vrserver is gone; exiting\n");
            break;
        }

        // Pointer on the dashboard tab
        if (!events.pointer.empty()) lastPointerEvent = loopStart;
        for (const PointerInput& input : events.pointer) {
            switch (input.type) {
                case PointerInput::Type::Move: settingsDirty |= settings.pointerMove(input.x, input.y); break;
                case PointerInput::Type::Down: {
                    const SettingsAction action = settings.pointerDown(input.x, input.y, nowSeconds());
                    if (action == SettingsAction::Quit) {
                        std::fprintf(stderr, "[VR] Quit on the dashboard tab; exiting\n");
                        userQuit = true;
                    } else if (action == SettingsAction::AutostartOn || action == SettingsAction::AutostartOff) {
                        // Only starts systemctl --user enable / disable in a child (doesn't wait for it)
                        autostart.request(action == SettingsAction::AutostartOn);
                    } else if (action == SettingsAction::UpdateCheckNow) {
                        updater.checkNow();  // works even with update_check off
                    } else if (action == SettingsAction::UpdateConfirmYes || action == SettingsAction::UpdateRetry) {
                        updater.install();  // returns at once; progress comes from the state file
                    } else if (action == SettingsAction::UpdateDismiss) {
                        updater.dismiss();
                    } else {
                        handleSettingsAction(action, config, watcher, vr, settings.angleStepDeg());
                    }
                    settingsDirty = true;
                    break;
                }
                case PointerInput::Type::Up: settingsDirty |= settings.pointerUp(); break;
                case PointerInput::Type::Leave: settingsDirty |= settings.pointerLeave(); break;
            }
        }
        if (userQuit) break;
        if (metricsTick) {
            settingsDirty |= settings.tick(loopStart);  // "press again to quit" expiry
            settingsDirty |= autostart.poll();          // reap systemctl if it finished
            // Whether the tab is actually visible (the dashboard being open isn't enough)
            const SettingsVisibility visibility = vr.settingsVisibility();
            const bool nowVisible = visibility.visible() || options.forceSettingsVisible;
            if (nowVisible != settingsVisible) {
                std::fprintf(stderr, "[dashboard] Tab %s (dashboard %s, tab %s, overlay %s%s)\n",
                             nowVisible ? "visible: polling the pointer every 100 ms (50 ms while active)" : "hidden",
                             visibility.dashboardVisible ? "open" : "closed",
                             visibility.activeTab ? "selected" : "not selected",
                             visibility.overlayVisible ? "visible" : "hidden",
                             options.forceSettingsVisible ? ", --force-settings-visible" : "");
            }
            settingsVisible = nowVisible;

            // "Someone looking": recomputes the reference counts whenever a signal changes
            const int level = vr.hmdActivityLevel();
            if (level != activityLevel) {
                std::fprintf(stderr, "[demand] HMD activity level %d (%s)\n", level, activityName(level));
                activityLevel = level;
            }
            fph::DemandInputs demand;
            demand.hudShown = config.visible;
            demand.hmdInUse = hmdInUse(level);
            demand.editorOpen = settingsVisible;
            demand.pcLease = false;  // stub until ticket 06
            demand.record = false;   // stub until ticket 11
            if (someoneLooking(demand) != someoneLooking(scheduler.demand())) {
                std::fprintf(stderr, "[demand] %s\n", someoneLooking(demand)
                                                          ? "Someone is looking: reading sources"
                                                          : "Nobody is looking: all sources paused");
            }
            scheduler.setDemand(demand);
        }
        // Dashboard tab: draw only while visible and only when something changed
        if (metricsTick && settingsVisible) {
            // Re-read the autostart state when the tab appears and every few seconds (catches outside changes)
            const double t = nowSeconds();
            if (!settingsWasVisible || t - lastAutostartRefresh >= kAutostartRefreshSec) {
                lastAutostartRefresh = t;
                if (autostart.refresh()) {
                    const AutostartStatus::State autostartState = autostart.status().state;
                    std::fprintf(stderr, "[autostart] State changed: %s\n",
                                 autostartState == AutostartStatus::State::Enabled
                                     ? "enabled"
                                     : (autostartState == AutostartStatus::State::Disabled ? "disabled"
                                                                                           : "not installed"));
                    settingsDirty = true;
                }
            }
        }
        if (settingsVisible && (settingsDirty || !settingsWasVisible)) {
            settings.render(config, autostart.status(), updater.status());
            vr.submitSettings(settings.toRgba().data());
            settingsDirty = false;
        }
        settingsWasVisible = settingsVisible;

        // ---- Sources: only the due ones, and only while someone is looking ----
        const double c0 = cpuSeconds();
        scheduler.runDue(nowSeconds());
        const double c1 = cpuSeconds();

        // ---- HUD redraw (2 Hz at most; at once when the clock row is switched, since the height changes).
        // Only while it's shown and the headset is in use: nobody sees it otherwise ----
        double now = nowSeconds();
        const double panelInterval = std::max(kMinPanelIntervalSec, config.updateIntervalMs / 1000.0);
        if (config.visible && (config.clockFormat != 0) != panelDrawnWithClock) nextPanel = now;
        if (now >= nextPanel) {
            if (config.visible && scheduler.demand().hmdInUse) {
                const double c2 = cpuSeconds();
                const SensorSample sample = bridge.sample();
                const FrameStats frame = bridge.frame();
                state.controllers = bridge.controllers();
                state.update(now, sample, frame, true, config.graphSeconds);
                renderer.render(state, config);
                const double c3 = cpuSeconds();
                const std::vector<uint8_t>& rgba = renderer.toRgba();
                const double c4 = cpuSeconds();
                // When the visible height changes (clock row on / off): grow before sending, shrink after, so the
                // only thing visible during the switch is one transparent row, never cut-off content
                const int visibleHeight = renderer.visibleHeight();
                vr.setPanelVisibleHeight(std::max(visibleHeight, vr.panelVisibleHeight()));
                const bool sent = vr.submitPanel(rgba.data());
                vr.setPanelVisibleHeight(visibleHeight);
                panelDrawnWithClock = config.clockFormat != 0;
                const double c5 = cpuSeconds();
                if (options.verbose && now - lastVerbose >= 2.0) {
                    lastVerbose = now;
                    std::fprintf(stderr,
                                 "[values] loops %u | fps %s frames=%u GPU %s/%sms CPU %s/%sms target %sms(%sHz) "
                                 "reproj %s%% dropped %u | GPU busy %s%% CPU %s°C GPU %s°C vph %sW throttle %d/%d | "
                                 "wifi %s down %sMbps %sdBm | ctrl L%s R%s | dashboard %s tab %s | submit %s | "
                                 "CPU ms: sources %.2f build %.2f draw %.2f convert %.2f submit %.2f\n",
                                 loopCount, fmt(frame.appFps, 1).c_str(), frame.frames, fmt(frame.gpuMs, 2).c_str(),
                                 fmt(frame.gpuMaxMs, 2).c_str(), fmt(frame.cpuMs, 2).c_str(),
                                 fmt(frame.cpuMaxMs, 2).c_str(), fmt(frame.targetMs, 2).c_str(),
                                 fmt(frame.displayHz, 1).c_str(), fmt(frame.reprojectedPct, 1).c_str(), frame.dropped,
                                 fmt(sample.gpuBusyPct, 1).c_str(), fmt(sample.cpuTempC, 1).c_str(),
                                 fmt(sample.gpuTempC, 1).c_str(), fmt(sample.powerMainW, 2).c_str(),
                                 sample.throttleCpu ? 1 : 0, sample.throttleGpu ? 1 : 0,
                                 !sample.wifi.connected ? "none" : (sample.wifi.homeWifi ? "home" : "dongle"),
                                 fmt(sample.wifi.rxMbps, 1).c_str(), fmt(sample.wifi.signalDbm, 0).c_str(),
                                 state.controllers.left.present ? fmt(state.controllers.left.pct, 0).c_str() : "-",
                                 state.controllers.right.present ? fmt(state.controllers.right.pct, 0).c_str() : "-",
                                 vr.dashboardOpen() ? "open" : "closed", settingsVisible ? "visible" : "hidden",
                                 sent ? "OK" : "failed", (c1 - c0) * 1e3, (c2 - c1) * 1e3, (c3 - c2) * 1e3,
                                 (c4 - c3) * 1e3, (c5 - c4) * 1e3);
                    loopCount = 0;
                }
            }
            // Next redraw (don't try to catch up after a delay)
            nextPanel += panelInterval;
            if (nextPanel < now) nextPanel = now;
        }

        if (gDumpRequested) {
            gDumpRequested = 0;
            writeDebugDump(scheduler, store, activityLevel, settingsVisible, nowSeconds());
        }

        // ---- Sleep until the earliest deadline ----
        now = nowSeconds();
        double wakeAt = std::min(nextMetricsTick, scheduler.nextDue(now));
        if (config.visible) wakeAt = std::min(wakeAt, nextPanel);
        if (settingsVisible) {
            const double pollSec =
                now - lastPointerEvent < kSettingsActiveHoldSec ? kSettingsPollSec : kSettingsIdlePollSec;
            wakeAt = std::min(wakeAt, now + pollSec);
        }
        wakeAt = std::min(wakeAt, vr.nextPlacementAt());  // +infinity when no placement check is planned
        waitForWake(wakeAt - now, cameraFd);  // the camera's texture upload goes here in ticket 10
    }

    // Same cleanup for SIGTERM / SIGINT, SteamVR quitting, vrserver vanishing, and Quit
    vr.shutdown();
    std::fprintf(stderr, "[VR] Shut down\n");
    if (cameraFd >= 0) ::close(cameraFd);
    if (lockFd >= 0) ::close(lockFd);
    // A user quit gets the exit code systemd's RestartPreventExitStatus= keeps from restarting
    return userQuit ? kExitCodeUserQuit : 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffer stderr so each line reaches the journal at once
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    installSignalHandlers();

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("frame-perf-hud %s\n", FRAME_PERF_HUD_VERSION); return 0;
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::DumpSettingsPng: return runDumpSettingsPng(options);
        case Options::Mode::DumpFrameTimings: return runDumpFrameTimings(options);
        case Options::Mode::DebugDump: return runDebugDump();
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
