#define _USE_MATH_DEFINES  // M_PI (the icon)
#include "app_window.h"

#include "atomic_file.h"
#include "draw.h"
#include "editor_model.h"
#include "grid_renderer.h"
#include "layout.h"
#include "layout_edit.h"
#include "link_client.h"
#include "log.h"
#include "panel_view.h"
#include "pc_pages.h"  // R3: the Layout | Profiles | Logs tabs
#include "pc_editor.h"
#include "platform.h"
#include "resource.h"
#include "session_logs.h"  // T11
#include "settings.h"
#include "settings_dialog.h"
#include "single_instance.h"
#include "streaming/service.h"
#include "theme.h"
#include "udp_socket.h"
#include "updater.h"  // Release G
#include "widgets.h"
#include "win_text.h"

#include <cairo.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace fph {
namespace pc {

namespace {

constexpr UINT WM_APP_SOCKET = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 100;
constexpr UINT kTrayTickMs = 500;  // hidden: nothing to draw; the link's deadlines are all ≥ 1 s and packets wake us
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuShow = 100,
    kMenuEdit,
    kMenuOnTop,
    kMenuSettings,
    kMenuHide,
    kMenuExit,
    kMenuRemove,
    kMenuSumSameName,
    kMenuAdapterDongle,
    kMenuAdapterHome,
    kMenuRows5,  // + 0..4: 5, 10, 15, 25, 50 rows
    kMenuSave = kMenuRows5 + 5,  // T11: the last 10 minutes + the session's summary
    kMenuOpenLogs,               // T11: the data folder (sessions, saves) in Explorer
    kMenuCompact,                // R2: the hasCompactView() types (Frame, Power, Battery, Temp; R3: Streaming)
    kMenuHideSystem,             // R2: process lists
    kMenuUpdateInstall,          // Release G: download, verify, swap in, restart
    kMenuUpdatePage,             // Release G: the release page in the browser
    kMenuUpdateCheck,            // Release G: Check now (the Settings button; also WM_COMMAND for scripts)
};
constexpr uint32_t kRowChoices[] = {5, 10, 15, 25, 50};

/** --offline: sends nothing, finds nothing. */
class OfflineEnv : public LinkClientEnv {
public:
    void send(const Endpoint&, const std::vector<uint8_t>&) override {}
    std::vector<uint32_t> broadcastAddresses() override { return {}; }
};

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/** The app: one per process, owns the window, the tray icon, the socket and the link client. */
class App {
public:
    App(HINSTANCE instance, const AppOptions& options) : instance_(instance), options_(options), env_(socket_) {}
    ~App();
    int run();

private:
    HINSTANCE instance_;
    AppOptions options_;
    HWND hwnd_ = nullptr;
    HICON iconBig_ = nullptr;
    HICON iconSmall_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool exiting_ = false;

    std::string dataDir_;
    std::string settingsPath_;
    std::string cachePath_;
    Settings settings_;

    UdpSocket socket_;
    SocketEnv env_;
    OfflineEnv offlineEnv_;
    std::unique_ptr<LinkClient> client_;
    streaming::Service streaming_;
    double nextPcMetrics_ = 0;
    std::unique_ptr<PcSessionLogs> logs_;  ///< T11
    Rect saveButton_;                      ///< T11: the status bar's Save, window px
    // Release G: the self-updater
    std::unique_ptr<update::Updater> updater_;
    uint64_t updateRev_ = 0;
    Rect updateButton_;                    ///< the status bar's "Update available (x.y.z)", window px (w 0 = none)
    std::string updateAnnounced_;          ///< the version the tray balloon already told about
    // R3: the tabs and the Profiles / Logs pages
    PcPages pages_;
    std::string headsetMessage_;  ///< the last headset action's answer
    std::string pcMessage_;       ///< the last PC Save's result
    uint64_t controlRev_ = ~0ull;
    PageAction sentAction_ = PageAction::None;  ///< the button whose CONTROL_ACTION is in flight (NeedsConfirm re-arms it)

    FontSet fonts_;
    std::unique_ptr<PanelView> panel_;
    uint64_t renderedLayoutRev_ = ~0ull;
    bool panelDirty_ = true;
    uint64_t cameraRev_ = 0;  // T10: CameraFeed::revision last drawn

    cairo_surface_t* surface_ = nullptr;  ///< the window's picture, client size
    int surfaceW_ = 0;
    int surfaceH_ = 0;
    bool screenshotDone_ = false;
    double startedAt_ = 0;

    // View: panel px -> window px is (p * scale + origin)
    double viewScale_ = 1.0;  ///< unscaled panel px -> window px
    double originX_ = 0;
    double originY_ = 0;

    LinkPhase lastPhase_ = LinkPhase::Discovery;
    std::string notice_;
    double noticeUntil_ = 0;

    // Editor
    bool editing_ = false;
    PcEditor editor_;
    bool editInFlight_ = false;  ///< an edit was sent and its answer hasn't come yet (the editor's status line)

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool init();
    void loadState();
    void saveSettings();
    void saveLayoutCache();
    void restorePlacement();
    void rememberPlacement();
    void makeIcons();
    void addTray();
    void removeTray();
    void showTrayBalloon(const wchar_t* title, const wchar_t* text);
    void showWindow();
    void hideToTray();
    void exitApp();
    void setOnTop(bool on);
    void toggleEditing();
    void openSettings();
    void showMenu(POINT screen, bool fromTray);
    /** The window menu's actions; also reachable as WM_COMMAND (scripts, Ctrl+E). */
    void command(UINT id);
    void showCardMenu(POINT screen, int slot);
    void setNotice(const std::string& text, double seconds = 8.0);
    void flushSessionLog();  ///< T11: the session logs' lines into pc.log
    PagesView pagesView() const;           ///< R3
    void runPageAction(PageAction action);  ///< R3: a Profiles / Logs button
    // Release G
    bool updateNewer(const update::View& v) const;  ///< a newer release is known (Available, or one being installed)
    void afterUpdaterChange();
    void installUpdate(const std::string& readyPath);
    void showUpdateMenu(POINT screen);
    void setTrayTip(const std::wstring& tip);

    void onTick();
    void afterLinkEvents();
    void pushDiscoveryTargets();

    double dpiScale() const;
    void ensureSurface(int w, int h);
    void paint(HDC hdc);
    void drawFrame();
    void drawStatus(const Pen& pen, double width, double barH);
    std::string statusText() const;
    Color statusColor() const;
    void panelPoint(double wx, double wy, double& px, double& py) const;
    bool editorUsable();
    void syncEditor();
    void pointerDown(double x, double y);
    void pointerUp(double x, double y);
    void takeEditorCommit();
    void cancelDrag();
    void submit(const Layout& edited, uint32_t baseVersion);
};

App::~App() {
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    if (iconBig_ != nullptr) DestroyIcon(iconBig_);
    if (iconSmall_ != nullptr) DestroyIcon(iconSmall_);
}

// ---- state on disk ----

void App::loadState() {
    dataDir_ = options_.dataDir.empty() ? defaultDataDir() : options_.dataDir;
    if (dataDir_.empty()) dataDir_ = ".";
    makeDirectories(dataDir_);
    openLog(dataDir_ + "\\pc.log");
    logLine("[app] frame-perf-hud for Windows %s, data in %s", FRAME_PERF_HUD_VERSION, dataDir_.c_str());
    settingsPath_ = dataDir_ + "\\settings.json";
    cachePath_ = dataDir_ + "\\layout-cache.json";
    std::vector<std::string> warnings;
    bool idCreated = false;
    const LoadResult r = loadSettings(settingsPath_, settings_, idCreated, warnings);
    for (const std::string& w : warnings) logLine("[app] %s", w.c_str());
    logLine("[app] settings %s, PC id %s%s", r == LoadResult::Loaded ? "loaded" : r == LoadResult::Missing ? "missing (first run)" : "broken",
            settings_.pcId.c_str(), idCreated ? " (new)" : "");
    if (idCreated || r != LoadResult::Loaded) saveSettings();
    logs_.reset(new PcSessionLogs(dataDir_));  // T11
    // Release G: the exe an update replaced (it was still running then), and the updater with its last answer
    const std::string exe = exePath();
    if (update::removeOldExe(exe)) logLine("[update] removed %s", update::oldExePath(exe).c_str());
    const update::UpdaterConfig uc = update::configFromEnvironment(FRAME_PERF_HUD_VERSION, dataDir_ + "\\update");
    if (uc.apiBase != "https://api.github.com" || uc.insecure) {
        logLine("[update] test override: %s%s", uc.apiBase.c_str(), uc.insecure ? " (insecure)" : "");
    }
    updater_.reset(new update::Updater(uc));
    updater_->restore(settings_.updateCheckedAt, settings_.updateLatest, settings_.updatePage);
}

void App::saveSettings() {
    std::string error;
    if (!pc::saveSettings(settingsPath_, settings_, error)) logLine("[app] can't save settings: %s", error.c_str());
}

void App::saveLayoutCache() {
    std::string error;
    if (!atomicWriteFile(cachePath_, layoutToJson(client_->layout(), 2), error)) {
        logLine("[app] can't save the layout cache: %s", error.c_str());
    }
}

void App::pushDiscoveryTargets() {
    std::vector<uint32_t> ips;
    for (const std::string& text : rememberedIps(settings_)) {
        uint32_t ip = 0;
        if (link::ipv4FromText(text, ip)) ips.push_back(ip);
    }
    uint32_t manual = 0;
    if (!settings_.manualIp.empty()) link::ipv4FromText(settings_.manualIp, manual);
    client_->setDiscoveryTargets(ips, manual);
}

// ---- window placement ----

void App::restorePlacement() {
    const WindowState& w = settings_.window;
    if (w.valid) {
        RECT r {w.x, w.y, w.x + w.width, w.y + w.height};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr) {
            WINDOWPLACEMENT wp {};
            wp.length = sizeof(wp);
            wp.showCmd = SW_HIDE;
            wp.rcNormalPosition = r;
            SetWindowPlacement(hwnd_, &wp);
            return;
        }
        logLine("[app] the saved window position is off every monitor: using the default");
    }
    // Default: the panel's natural size (DPI-scaled), at most 80% of the work area, centred
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    const RECT work = mi.rcWork;
    const double scale = dpiScale();
    const GridGeometry g = layoutGrid(client_->layout());
    int cw = static_cast<int>(g.textureWidth * scale);
    int ch = static_cast<int>(g.textureHeight * scale + 40 * scale);
    cw = std::min<int>(cw, static_cast<int>((work.right - work.left) * 0.8));
    ch = std::min<int>(ch, static_cast<int>((work.bottom - work.top) * 0.8));
    RECT r {0, 0, std::max(cw, 480), std::max(ch, 320)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(hwnd_));
    const int ww = r.right - r.left;
    const int wh = r.bottom - r.top;
    SetWindowPos(hwnd_, nullptr, work.left + ((work.right - work.left) - ww) / 2,
                 work.top + ((work.bottom - work.top) - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::rememberPlacement() {
    WINDOWPLACEMENT wp {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    WindowState w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (IsZoomed(hwnd_) != FALSE);
    if (w.width < 100 || w.height < 100) return;
    settings_.window = w;
}

// ---- icons + tray ----

void App::makeIcons() {
    // Drawn at run time (nothing to ship): an accent rounded square with three bars, frame-perf-overlay's colours
    const auto make = [](int size) -> HICON {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t* cr = cairo_create(s);
        const double r = size * 0.22;
        const double m = size * 0.04;
        const double w = size - 2 * m;
        cairo_new_sub_path(cr);
        cairo_arc(cr, m + w - r, m + r, r, -M_PI / 2, 0);
        cairo_arc(cr, m + w - r, m + w - r, r, 0, M_PI / 2);
        cairo_arc(cr, m + r, m + w - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, m + r, m + r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, kBg.r, kBg.g, kBg.b);
        const double barW = w * 0.16;
        const double base = m + w * 0.80;
        const double heights[3] = {0.30, 0.55, 0.42};
        for (int i = 0; i < 3; ++i) {
            const double x = m + w * 0.18 + i * (barW + w * 0.08);
            cairo_rectangle(cr, x, base - w * heights[i], barW, w * heights[i]);
        }
        cairo_fill(cr);
        cairo_destroy(cr);
        cairo_surface_flush(s);
        // Icons want straight (non-premultiplied) BGRA
        BITMAPV5HEADER bi {};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = size;
        bi.bV5Height = -size;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;
        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        const unsigned char* src = cairo_image_surface_get_data(s);
        const int stride = cairo_image_surface_get_stride(s);
        auto* dst = static_cast<unsigned char*>(bits);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const unsigned char* p = src + y * stride + x * 4;
                unsigned char* q = dst + (y * size + x) * 4;
                const unsigned a = p[3];
                for (int c = 0; c < 3; ++c) q[c] = a == 0 ? 0 : static_cast<unsigned char>(std::min(255u, p[c] * 255u / a));
                q[3] = static_cast<unsigned char>(a);
            }
        }
        cairo_surface_destroy(s);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO ii {};
        ii.fIcon = TRUE;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(color);
        DeleteObject(mask);
        return icon;
    };
    iconBig_ = make(GetSystemMetrics(SM_CXICON));
    iconSmall_ = make(GetSystemMetrics(SM_CXSMICON));
}

void App::addTray() {
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = iconSmall_;
    wcscpy_s(nid.szTip, L"frame-perf-hud");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (trayAdded_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        logLine("[app] couldn't add the tray icon (Explorer not running?)");
    }
}

void App::removeTray() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void App::showTrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::setTrayTip(const std::wstring& tip) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---- Release G: the self-updater ----

bool App::updateNewer(const update::View& v) const {
    return !v.latest.empty() && update::compareVersions(v.latest, v.current) > 0;
}

void App::afterUpdaterChange() {
    if (updater_->revision() == updateRev_) return;
    updateRev_ = updater_->revision();
    int64_t checkedAt = 0;
    std::string latest;
    std::string page;
    if (updater_->takeChecked(checkedAt, latest, page)) {  // remembered, so a restart within the day still knows
        settings_.updateCheckedAt = checkedAt;
        settings_.updateLatest = latest;
        settings_.updatePage = page;
        saveSettings();
    }
    const update::View v = updater_->view();
    if (v.state == update::State::Ready) {
        installUpdate(v.readyPath);
        return;
    }
    if (v.state == update::State::InstallFailed) setNotice(update::describe(v), 15);
    const bool newer = updateNewer(v);
    setTrayTip(newer ? widen("frame-perf-hud: update available (" + v.latest + ")") : L"frame-perf-hud");
    if (newer && v.state == update::State::Available && updateAnnounced_ != v.latest) {
        updateAnnounced_ = v.latest;
        logLine("[update] %s is available (this is %s)", v.latest.c_str(), v.current.c_str());
        if (!IsWindowVisible(hwnd_)) {
            showTrayBalloon(L"frame-perf-hud update available",
                            widen("Version " + v.latest + " is out. Right-click the tray icon to install it.").c_str());
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::installUpdate(const std::string& readyPath) {
    const std::string exe = exePath();
    const std::string dir = exe.substr(0, exe.find_last_of("\\/"));
    const update::View v = updater_->view();
    if (!update::folderWritable(dir)) {
        // e.g. Program Files: the browser download is the way, and the app keeps running as it is
        logLine("[update] %s isn't writable: opening the release page", dir.c_str());
        if (!v.page.empty()) ShellExecuteW(hwnd_, L"open", widen(v.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        updater_->installFailed("can't write to " + dir +
                                (v.page.empty() ? "" : ", so the release page was opened instead"));
        afterUpdaterChange();
        return;
    }
    std::string error;
    if (!update::swapInExe(exe, readyPath, error)) {
        logLine("[update] swap failed: %s", error.c_str());
        updater_->installFailed(error);
        afterUpdaterChange();
        return;
    }
    logLine("[update] %s is now %s (the old exe is %s)", exe.c_str(), v.latest.c_str(), update::oldExePath(exe).c_str());
    // The new exe waits for this process to exit (it holds the instance mutex until then)
    std::wstring cmd = L"\"" + widen(exe) + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId()) + L" --updated";
    if (!IsWindowVisible(hwnd_)) cmd += L" --tray";
    if (!options_.dataDir.empty()) cmd += L" --data-dir \"" + widen(options_.dataDir) + L"\"";
    if (options_.offline) cmd += L" --offline";
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    // A bad exe would make Windows put up a modal "Unsupported 16-Bit Application"-style box here and hold the
    // window until it's closed; fail at once instead and roll back
    DWORD previousMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode);
    const BOOL started =
        CreateProcessW(widen(exe).c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    const DWORD launchError = started ? ERROR_SUCCESS : GetLastError();
    SetThreadErrorMode(previousMode, nullptr);
    if (!started) {
        // Windows refused the new exe: put this one back, so the next start isn't stuck with the rejected file
        logLine("[update] can't start the new exe (error %lu): putting the old one back", launchError);
        std::string why = "Windows wouldn't start the new exe (error " + std::to_string(launchError) + ")";
        std::string undoError;
        const bool undone = update::undoSwap(exe, undoError);
        if (undone) {
            logLine("[update] %s is back to %s", exe.c_str(), v.current.c_str());
            why += ", so this version stays";
        } else {
            logLine("[update] %s", undoError.c_str());
            const std::string name = exe.substr(exe.find_last_of("\\/") + 1);
            why += " and the old exe couldn't be put back (" + undoError + "). Before the next start, move or delete " +
                   exe + " (the rejected file), then rename " + update::oldExePath(exe) + " to " + name;
        }
        updater_->installFailed(why, !undone);
        afterUpdaterChange();
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("[update] started the new exe (pid %lu); exiting", pi.dwProcessId);
    exitApp();  // BYE to the headset, settings saved, like Exit
}

void App::showUpdateMenu(POINT screen) {
    const update::View v = updater_->view();
    HMENU menu = CreatePopupMenu();
    const bool busy = v.state == update::State::Downloading || v.state == update::State::Ready;
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuUpdateInstall,
                widen(v.installable ? "Install " + v.latest + " and restart" : "Install " + v.latest).c_str());
    AppendMenuW(menu, MF_STRING | (v.page.empty() ? MF_GRAYED : 0), kMenuUpdatePage, L"Open release page");
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                    hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    command(cmd);
}

// ---- window actions ----

void App::showWindow() {
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, settings_.window.valid && settings_.window.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    } else if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    panelDirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::hideToTray() {
    cancelDrag();
    rememberPlacement();
    ShowWindow(hwnd_, SW_HIDE);
    SetTimer(hwnd_, kTickTimer, kTrayTickMs, nullptr);
    if (!settings_.trayHintShown && trayAdded_) {
        showTrayBalloon(L"frame-perf-hud is still running",
                        L"It stays in the tray. Right-click the icon and pick Exit to quit.");
        settings_.trayHintShown = true;
    }
    saveSettings();
}

void App::exitApp() {
    if (exiting_) return;
    exiting_ = true;
    logs_->shutdown();  // T11: the open sessions' copies first (Exit, and Windows shutting down via WM_ENDSESSION)
    flushSessionLog();
    streaming_.stop(); // the worker calls VR_Shutdown before returning
    logLine("[app] exit: %s", client_->debugText(nowSec()).c_str());
    cancelDrag();
    if (IsWindowVisible(hwnd_)) rememberPlacement();
    client_->sayGoodbye();
    saveSettings();
    if (client_->layoutIsLive() && !client_->editPending()) saveLayoutCache();
    KillTimer(hwnd_, kTickTimer);
    removeTray();
    socket_.close();
    DestroyWindow(hwnd_);
}

void App::setOnTop(bool on) {
    settings_.onTop = on;
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    saveSettings();
}

void App::toggleEditing() {
    editing_ = !editing_;
    if (editing_) pages_.setTab(PcTab::Layout);  // R3: the editor lives on the Layout tab
    cancelDrag();
    if (editing_) showWindow();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

PagesView App::pagesView() const {
    PagesView v;
    v.headset = client_->controlState();
    v.subscribed = client_->subscribed();
    v.busy = client_->controlInFlight();
    v.headsetMessage = headsetMessage_;
    v.pcMessage = pcMessage_;
    v.lastCard = logs_->lastCard();
    v.dataDir = dataDir_;
    return v;
}

void App::runPageAction(PageAction action) {
    using link::ControlKind;
    const auto send = [&](ControlKind kind, const std::string& id, const std::string& text) {
        sentAction_ = action;
        // the clock is read here, after Rename's dialog closed: its time in the dialog mustn't count toward the 5 s
        if (client_->sendControl(kind, id, text, pages_.lastConfirmed(), pages_.confirmVersion(), nowSec())) {
            headsetMessage_.clear();
        } else {
            headsetMessage_ = client_->controlInFlight() ? "Still waiting for the headset's last answer"
                                                         : "Not connected to the headset";
        }
    };
    const std::string& selected = pages_.selectedProfile();
    switch (action) {
        case PageAction::ProfileNew: send(ControlKind::ProfileNew, "", ""); break;
        case PageAction::ProfileUse: send(ControlKind::ProfileUse, selected, ""); break;
        case PageAction::ProfileSave: send(ControlKind::ProfileSave, "", ""); break;
        case PageAction::ProfileDelete: send(ControlKind::ProfileDelete, selected, ""); break;
        case PageAction::ProfileReset: send(ControlKind::ProfileReset, "", ""); break;
        case PageAction::ProfileRename: {
            std::string name;
            if (const link::ControlState* h = client_->controlState()) {
                for (const auto& p : h->profiles) {
                    if (p.id == selected) name = p.name;
                }
            }
            if (showRenameDialog(instance_, hwnd_, name)) send(ControlKind::ProfileRename, selected, name);
            break;
        }
        case PageAction::RecordOn: send(ControlKind::RecordOn, "", ""); break;
        case PageAction::RecordOff: send(ControlKind::RecordOff, "", ""); break;
        case PageAction::CrashOn: send(ControlKind::CrashRecorderOn, "", ""); break;
        case PageAction::CrashOff: send(ControlKind::CrashRecorderOff, "", ""); break;
        case PageAction::SaveLocal: command(kMenuSave); break;
        case PageAction::OpenFolder: command(kMenuOpenLogs); break;
        default: break;
    }
}

void App::flushSessionLog() {
    const std::string lines = logs_->takeLog();
    size_t start = 0;
    while (start < lines.size()) {
        const size_t end = lines.find('\n', start);
        logLine("%s", lines.substr(start, end == std::string::npos ? std::string::npos : end - start).c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void App::setNotice(const std::string& text, double seconds) {
    notice_ = text;
    noticeUntil_ = nowSec() + seconds;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::openSettings() {
    SettingsDialogData data;
    data.manualIp = settings_.manualIp;
    data.autostart = autostartEnabled();
    data.cameraFps = settings_.cameraFps;  // T10
    std::ostringstream info;
    info << "This PC: " << computerName() << "\r\nPC id: " << settings_.pcId << "\r\n";
    const LinkPhase phase = client_->phase();
    if (phase == LinkPhase::Discovery) {
        info << "Headset: looking for it\r\n";
    } else {
        info << "Headset: '" << client_->headset().name << "' at " << client_->headsetEndpoint().text() << " ("
             << linkPhaseName(phase) << ")\r\n";
    }
    info << "Remembered headsets:";
    if (settings_.headsets.empty()) info << " none";
    for (const RememberedHeadset& h : settings_.headsets) {
        info << "\r\n  " << (h.name.empty() ? "?" : h.name) << ":";
        for (const std::string& ip : h.ips) info << " " << ip;
    }
    info << "\r\nLog and settings: " << dataDir_;
    data.info = info.str();
    data.updateCheck = settings_.updateCheck;  // Release G
    data.checkNow = [this] { updater_->checkNow(); };
    data.updateStatus = [this] { return update::describe(updater_->view()); };
    if (!showSettingsDialog(instance_, hwnd_, data)) return;
    if (data.updateCheck != settings_.updateCheck) {
        logLine("[update] automatic checks %s", data.updateCheck ? "on" : "off");
        settings_.updateCheck = data.updateCheck;
    }
    if (data.manualIp != settings_.manualIp) {
        logLine("[app] manual IP: '%s'", data.manualIp.c_str());
        settings_.manualIp = data.manualIp;
    }
    if (data.cameraFps != settings_.cameraFps) {  // T10: the next SUBSCRIBE carries it
        logLine("[app] camera frame rate: %d fps", data.cameraFps);
        settings_.cameraFps = data.cameraFps;
    }
    if (data.forgetHeadsets) {
        logLine("[app] remembered headsets forgotten");
        settings_.headsets.clear();
    }
    if (data.autostart != autostartEnabled()) {
        if (!setAutostart(data.autostart)) {
            MessageBoxW(hwnd_, L"Windows didn't let frame-perf-hud change \"Start with Windows\".", L"frame-perf-hud",
                        MB_OK | MB_ICONWARNING);
        } else {
            logLine("[app] start with Windows: %s", data.autostart ? "on" : "off");
        }
    }
    saveSettings();
    pushDiscoveryTargets();
}

void App::showMenu(POINT screen, bool fromTray) {
    HMENU menu = CreatePopupMenu();
    if (const update::View u = updater_->view(); updateNewer(u)) {  // Release G: on top while there's one
        const bool busy = u.state == update::State::Downloading || u.state == update::State::Ready;
        AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuUpdateInstall,
                    widen(busy ? "Updating to " + u.latest + "…" : "Update available (" + u.latest + "): Install").c_str());
        AppendMenuW(menu, MF_STRING | (u.page.empty() ? MF_GRAYED : 0), kMenuUpdatePage, L"Open release page");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (fromTray) AppendMenuW(menu, MF_STRING, kMenuShow, L"Show");
    AppendMenuW(menu, MF_STRING | (editing_ ? MF_CHECKED : 0), kMenuEdit, L"Edit layout");
    AppendMenuW(menu, MF_STRING | (settings_.onTop ? MF_CHECKED : 0), kMenuOnTop, L"On top");
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSave, L"Save Logs");  // T11
    AppendMenuW(menu, MF_STRING, kMenuOpenLogs, L"Open logs folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!fromTray) AppendMenuW(menu, MF_STRING, kMenuHide, L"Hide to tray");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    if (fromTray) SetMenuDefaultItem(menu, kMenuShow, FALSE);
    SetForegroundWindow(hwnd_);  // so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                    hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    command(cmd);
}

void App::command(UINT id) {
    switch (id) {
        case kMenuShow: showWindow(); break;
        case kMenuEdit: toggleEditing(); break;
        case kMenuOnTop: setOnTop(!settings_.onTop); break;
        case kMenuSettings: showWindow(); openSettings(); break;
        case kMenuHide: hideToTray(); break;
        case kMenuExit: exitApp(); break;
        case kMenuSave:  // T11
            pcMessage_ = logs_->save(static_cast<int64_t>(std::time(nullptr)));  // R3: shown on the Logs tab too
            setNotice(pcMessage_);
            flushSessionLog();
            break;
        case kMenuUpdateInstall: {  // Release G
            const update::View u = updater_->view();
            const std::string exe = exePath();
            if (u.installable && !update::folderWritable(exe.substr(0, exe.find_last_of("\\/")))) {
                // e.g. Program Files: don't download what can't be swapped in; the browser download is the way
                installUpdate("");
            } else if (u.installable) {
                updater_->install();
            } else if (!u.page.empty()) {
                ShellExecuteW(hwnd_, L"open", widen(u.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            afterUpdaterChange();
            break;
        }
        case kMenuUpdatePage: {
            const update::View u = updater_->view();
            if (!u.page.empty()) ShellExecuteW(hwnd_, L"open", widen(u.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kMenuUpdateCheck:
            updater_->checkNow();
            afterUpdaterChange();
            break;
        case kMenuOpenLogs:
            ShellExecuteW(hwnd_, L"open", widen(dataDir_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default: break;
    }
}

void App::showCardMenu(POINT screen, int slot) {
    const Layout layout = client_->layout();
    const GridGeometry geometry = layoutGrid(layout);
    const WidgetRef ref = refOfSlot(layout, geometry, slot);
    const WidgetInstance* w = widgetAt(layout, ref);
    if (w == nullptr) return;
    HMENU menu = CreatePopupMenu();
    if (w->type == WidgetType::CpuItems || w->type == WidgetType::RamItems) {
        AppendMenuW(menu, MF_STRING | (w->sumSameName ? MF_CHECKED : 0), kMenuSumSameName, L"One row per process name");
        AppendMenuW(menu, MF_STRING | (w->hideSystem ? MF_CHECKED : 0), kMenuHideSystem, L"Hide system");
        HMENU rows = CreatePopupMenu();
        for (size_t i = 0; i < 5; ++i) {
            AppendMenuW(rows, MF_STRING | (w->maxItems == kRowChoices[i] ? MF_CHECKED : 0),
                        kMenuRows5 + static_cast<UINT>(i), std::to_wstring(kRowChoices[i]).c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rows), L"Rows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (w->type == WidgetType::Network) {
        AppendMenuW(menu, MF_STRING | (w->adapter == NetAdapter::Dongle ? MF_CHECKED : 0), kMenuAdapterDongle,
                    L"Steam Link dongle (wlanap)");
        AppendMenuW(menu, MF_STRING | (w->adapter == NetAdapter::Home ? MF_CHECKED : 0), kMenuAdapterHome,
                    L"Home Wi-Fi (wlan0)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (hasCompactView(w->type)) {
        AppendMenuW(menu, MF_STRING | (w->compact ? MF_CHECKED : 0), kMenuCompact, L"Compact");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kMenuRemove, L"Remove");
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                    hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) return;
    Layout edited = layout;
    WidgetInstance& target =
        edited.columns[static_cast<size_t>(ref.column)].widgets[static_cast<size_t>(ref.index)];
    if (cmd == kMenuRemove) {
        removeWidget(edited, ref);
    } else if (cmd == kMenuSumSameName) {
        target.sumSameName = !target.sumSameName;
    } else if (cmd == kMenuHideSystem) {
        target.hideSystem = !target.hideSystem;
    } else if (cmd == kMenuCompact) {
        target.compact = !target.compact;
    } else if (cmd == kMenuAdapterDongle || cmd == kMenuAdapterHome) {
        target.adapter = cmd == kMenuAdapterDongle ? NetAdapter::Dongle : NetAdapter::Home;
    } else if (cmd >= kMenuRows5 && cmd < kMenuRows5 + 5) {
        target.maxItems = kRowChoices[cmd - kMenuRows5];
    }
    if (!edited.sameWidgets(layout)) submit(edited, layout.version);
}

// ---- link ----

void App::onTick() {
    const double now = nowSec();
    // In case a FD_READ was coalesced; new packets (METRICS at 2 Hz, layouts) are what repaint the window
    if (socket_.receiveAll(*client_, now) > 0) panelDirty_ = true;
    client_->tick(now);
    // T10: camera pictures (they arrive as packets, which repaint) and stale ones dropping back to their state text
    client_->setCameraFps(settings_.cameraFps);
    client_->cameras().tick(now);
    if (client_->cameras().revision() != cameraRev_) {
        cameraRev_ = client_->cameras().revision();
        panelDirty_ = true;
    }
    bool streamWanted = false;
    for (const auto type : enabledWidgets(client_->layout())) streamWanted |= type == WidgetType::Streaming;
    streaming_.setDemand(streamWanted);
    if (streaming_.take(client_->store(), now)) panelDirty_ = true;
    if (streamWanted && now >= nextPcMetrics_) {
        nextPcMetrics_ = now + 1.0;
        client_->sendPcMetrics(pcStreamValues(client_->store()));
    }
    afterLinkEvents();
    // T11: the session logs follow the headset's session (LINK_STATE) and the PC's own scene app, once a second
    {
        const link::LinkState& st = client_->headsetState();
        SessionInfo headset;
        headset.id = st.sessionId;
        headset.appKey = st.appKey;
        headset.appName = st.appName;
        headset.startUnix = st.sessionStartUnix;
        logs_->tick(now, static_cast<int64_t>(std::time(nullptr)), headset, client_->lastMetricsAt(),
                    client_->store());
        flushSessionLog();
    }
    // R3: the Profiles / Logs tabs: the headset's answer, a second tap expiring, a new CONTROL_STATE
    {
        link::ControlResult result;
        link::ControlKind kind;
        if (client_->takeControlResult(result, kind)) {
            switch (result.outcome) {
                case link::ControlOutcome::NeedsConfirm:  // the headset's own rule said "ask first": arm for the second tap
                    pages_.armFromHeadset(sentAction_, result.layoutVersion, now);
                    headsetMessage_ = result.message;
                    break;
                case link::ControlOutcome::NotAllowed: headsetMessage_ = "The headset didn't accept this PC's button (no live connection)"; break;
                case link::ControlOutcome::UnknownSubscriber: headsetMessage_ = "The headset restarted: reconnecting"; break;
                case link::ControlOutcome::Unsupported: headsetMessage_ = "The headset's HUD doesn't know this button: update it"; break;
                default: headsetMessage_ = result.message; break;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (pages_.tick(now)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (client_->controlRevision() != controlRev_) {
            controlRev_ = client_->controlRevision();
            if (pages_.tab() != PcTab::Layout) InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    // Release G: automatic checks (at start, then once a day); --offline contacts nothing on its own
    if (!options_.offline) updater_->tick(settings_.updateCheck, static_cast<int64_t>(std::time(nullptr)));
    afterUpdaterChange();
    if (!notice_.empty() && now > noticeUntil_) {
        notice_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (panelDirty_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
    if (options_.exitAfterSec > 0 && now - startedAt_ > options_.exitAfterSec) exitApp();
}

void App::afterLinkEvents() {
    HeadsetSeen seen;
    if (client_->takeHeadsetSeen(seen)) {
        std::vector<std::string> ips;
        for (uint32_t ip : seen.ips) ips.push_back(link::ipv4ToText(ip));
        if (rememberHeadset(settings_, seen.idHex, seen.name, ips)) {
            saveSettings();
            pushDiscoveryTargets();
        }
    }
    const std::string notice = client_->takeNotice();
    if (!notice.empty()) setNotice(notice);
    if (editInFlight_ && !client_->editPending()) {
        editInFlight_ = false;
        const bool stale = notice.find("changed in the headset") != std::string::npos;
        editor_.commitResult(notice.empty() ? PcEditor::CommitOutcome::Applied
                                            : stale ? PcEditor::CommitOutcome::Stale : PcEditor::CommitOutcome::Failed);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (client_->layoutRevision() != renderedLayoutRev_) {
        renderedLayoutRev_ = client_->layoutRevision();
        panel_->setLayout(client_->layout());
        panelDirty_ = true;
        if (client_->layoutIsLive() && !client_->editPending()) saveLayoutCache();
        // The layout changed under a drag (an edit in VR): the drag still applies to the layout it started from,
        // and the headset rejects it if that's stale
    }
    if (client_->phase() != lastPhase_) {
        logLine("[app] link %s -> %s", linkPhaseName(lastPhase_), linkPhaseName(client_->phase()));
        lastPhase_ = client_->phase();
        pages_.cancelArm();  // R3: a second tap armed against the old connection's state doesn't carry over
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---- drawing ----

double App::dpiScale() const {
    return hwnd_ != nullptr ? GetDpiForWindow(hwnd_) / 96.0 : 1.0;
}

void App::ensureSurface(int w, int h) {
    if (surface_ != nullptr && w == surfaceW_ && h == surfaceH_) return;
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(w, 1), std::max(h, 1));
    surfaceW_ = w;
    surfaceH_ = h;
}

std::string App::statusText() const {
    if (!notice_.empty()) return notice_;
    const std::string name = client_->headset().name.empty() ? "the headset" : "'" + client_->headset().name + "'";
    switch (client_->phase()) {
        case LinkPhase::Discovery:
            if (client_->versionMismatch()) {
                return "The headset runs a different frame-perf-hud version. Update both to the same release.";
            }
            return "Looking for the headset… (manual IP in Settings)";
        case LinkPhase::Pending:
            return "Approve this PC (" + computerName() + ") in the headset's dashboard";
        case LinkPhase::Denied:
            return "This PC was denied in " + name + "'s dashboard. Restart the HUD there to ask again.";
        case LinkPhase::Subscribed:
            if (client_->editPending()) return "Saving the layout on the headset…";
            if (editing_) return "Connected to " + name + "  ·  editing the layout (Ctrl+E or Esc to stop)";
            if (!client_->layoutIsLive()) return "Connected to " + name + ", loading its layout…";
            return "Connected to " + name + "  ·  right-click for options";
    }
    return "";
}

Color App::statusColor() const {
    if (!notice_.empty()) return kWarn;
    switch (client_->phase()) {
        case LinkPhase::Subscribed: return client_->editPending() ? kAccentText : kTextMuted;
        case LinkPhase::Pending: return kAccentText;
        case LinkPhase::Denied: return kDanger;
        case LinkPhase::Discovery: return client_->versionMismatch() ? kDanger : kText;
    }
    return kText;
}

void App::drawStatus(const Pen& pen, double width, double barH) {
    const double s = dpiScale();
    const std::string text = statusText();
    const Color c = statusColor();
    const bool live = client_->subscribed() && notice_.empty();
    if (!live) {
        pen.color(blendColor(c, kBg, 0.12));
        cairo_rectangle(pen.cr, 0, 0, width, barH);
        cairo_fill(pen.cr);
    }
    // A dot says the same thing as the colour, so colour isn't the only signal
    const Color dot = client_->subscribed() ? kSuccess : client_->phase() == LinkPhase::Discovery ? kTextMuted : c;
    drawDisc(pen.cr, 14 * s, barH / 2, 4.5 * s, dot);
    // T11: Save on the right (the last 10 minutes + the session's summary), as wide as its label
    const PageBox save = drawStatusSaveButton(pen, width, barH, s);
    saveButton_ = {save.x, save.y, save.w, save.h};
    const double bh = save.h;
    // Release G: "Update available (x.y.z)" left of Save while a newer release is known; a click offers Install
    updateButton_ = {};
    double rightEdge = saveButton_.x;
    if (const update::View u = updater_->view(); updateNewer(u)) {
        const bool busy = u.state == update::State::Downloading || u.state == update::State::Ready;
        const std::string label = busy ? "Updating to " + u.latest + "…" : "Update available (" + u.latest + ")";
        const double uw = pen.measure(label, 12.5 * s, true) + 24 * s;
        updateButton_ = {saveButton_.x - uw - 6 * s, 4 * s, uw, bh};
        pen.color(blendColor(kAccent, kBg, 0.25));
        pen.roundedRect(updateButton_.x, updateButton_.y, uw, bh, bh / 2);
        cairo_fill(pen.cr);
        strokeRounded(pen, updateButton_.x, updateButton_.y, uw, bh, bh / 2, kAccent, 1.5 * s);
        textCentered(pen, updateButton_.x + uw / 2, centerBaseline(updateButton_.y, bh, 12.5 * s), label, 12.5 * s,
                     kAccentText, true);
        rightEdge = updateButton_.x;
    }
    const double size = fitSize(pen, text, 13.5 * s, 9 * s, rightEdge - 32 * s - 12 * s, !live);
    pen.text(26 * s, centerBaseline(0, barH, size), text, size, live ? kTextMuted : c, !live);
}

void App::panelPoint(double wx, double wy, double& px, double& py) const {
    px = (wx - originX_) / viewScale_;
    py = (wy - originY_) / viewScale_;
}

void App::drawFrame() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    ensureSurface(w, h);
    cairo_t* cr = cairo_create(surface_);
    Pen pen {cr, &fonts_};
    const double s = dpiScale();
    pen.color(kBg);
    cairo_paint(cr);

    const double barH = 30 * s;
    const double pad = 8 * s;
    const double tabsH = PcPages::tabBarHeight(s);  // R3: Layout | Profiles | Logs under the status bar
    const double areaX = pad;
    const double areaY = barH + tabsH + pad * 0.5;
    const double areaW = std::max(1.0, w - 2 * pad);
    const double areaH = std::max(1.0, h - areaY - pad);

    if (pages_.tab() != PcTab::Layout) {  // R3
        pages_.drawPage(pen, {areaX, areaY, areaW, areaH}, s, pagesView());
    } else if (client_->haveLayout()) {
        // Every paint: widgets whose inputs didn't change keep their pixels (widgetSignature), so this is cheap
        WidgetContext ctx;
        ctx.store = &client_->store();
        ctx.processes = &client_->processes();
        ctx.now = nowSec();
        for (uint8_t c = 0; c < cam::kCameraCount; ++c)  // T10: the camera cards' state text
            ctx.cameraStates[c] = static_cast<uint8_t>(client_->cameras().state(c, ctx.now));
        panel_->updateGeometry(ctx);
        const GridGeometry& g = panel_->geometry();
        const bool live = client_->subscribed() && client_->layoutIsLive();
        if (!editing_) {
            // Fit the panel into the area. The widgets are drawn as vectors at that scale (PanelView), so it stays
            // sharp at any DPI; past 2× the DPI scale it would just be huge.
            viewScale_ = std::min({areaW / g.width, areaH / g.height, 2.0 * std::max(1.0, s)});
            originX_ = std::floor(areaX + std::max(0.0, (areaW - g.width * viewScale_) / 2));
            originY_ = std::floor(areaY);
            cairo_surface_t* panel = panel_->render(ctx, viewScale_);
            cairo_set_source_surface(cr, panel, originX_, originY_);  // 1:1, whole device pixels
            cairo_paint(cr);
            if (live) {  // T10: the camera pictures go on top of their tiles
                cairo_save(cr);
                cairo_translate(cr, originX_, originY_);
                client_->cameras().paint(cr, g, viewScale_, ctx.now);
                cairo_restore(cr);
            }
            if (!live) {  // not live: the cached layout, greyed
                pen.color(kBg, 0.6);
                cairo_rectangle(cr, originX_, originY_, g.width * viewScale_, g.height * viewScale_);
                cairo_fill(cr);
            }
        } else {
            // The headset editor's arrangement: a "Layout" card with the columns as lanes, a palette card on the right
            const double paletteW = PcEditor::paletteWidth(s);
            PcEditor::View v;
            v.ui = s;
            v.paletteCard = {w - pad - paletteW, areaY, paletteW, areaH};
            v.layoutCard = {areaX, areaY, std::max(1.0, w - 3 * pad - paletteW), areaH};
            const double inset = 14 * s;
            v.viewport = {v.layoutCard.x + inset, v.layoutCard.y + PcEditor::headerHeight(s),
                          std::max(1.0, v.layoutCard.w - 2 * inset),
                          std::max(1.0, v.layoutCard.h - PcEditor::headerHeight(s) - inset)};
            const int lanes = g.columns + (client_->layout().columns.size() < kMaxColumns ? 1 : 0);
            const double contentW = static_cast<double>(lanes) * kColumnWidth;
            viewScale_ = std::min({v.viewport.w / contentW, v.viewport.h / g.height, 2.0 * std::max(1.0, s)});
            originX_ = std::floor(v.viewport.x);
            originY_ = std::floor(v.viewport.y);
            v.originX = originX_;
            v.originY = originY_;
            v.scale = viewScale_;
            syncEditor();
            editor_.setView(v);
            editor_.drawUnderPanel(pen);
            // The real widget cards, clipped into each lane so the lanes read as separate boxes
            cairo_surface_t* panel = panel_->render(ctx, viewScale_);
            const std::vector<Box>& laneBoxes = editor_.lanes();
            for (size_t i = 0; i < laneBoxes.size(); ++i) {
                if (editor_.isNewColumnLane(i)) continue;
                const Box& lane = laneBoxes[i];
                cairo_save(cr);
                pen.roundedRect(lane.x, lane.y, lane.w, std::min(lane.h, g.height * viewScale_), 12 * s);
                cairo_clip(cr);
                cairo_set_source_surface(cr, panel, originX_, originY_);
                cairo_paint(cr);
                if (!live) {
                    pen.color(kBg, 0.6);
                    cairo_paint(cr);
                }
                cairo_restore(cr);
            }
            editor_.drawOverPanel(pen);
        }
        panelDirty_ = false;
    }
    pages_.drawTabs(pen, pad, barH, s);  // R3
    drawStatus(pen, w, barH);
    cairo_destroy(cr);
    cairo_surface_flush(surface_);
}

void App::paint(HDC hdc) {
    drawFrame();
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = surfaceW_;
    bi.bmiHeader.biHeight = -surfaceH_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, surfaceW_, surfaceH_, 0, 0, 0, surfaceH_, cairo_image_surface_get_data(surface_), &bi,
                      DIB_RGB_COLORS);
    if (!options_.screenshotPath.empty() && !screenshotDone_ && nowSec() - startedAt_ > 1.5) {
        screenshotDone_ = cairo_surface_write_to_png(surface_, options_.screenshotPath.c_str()) == CAIRO_STATUS_SUCCESS;
        logLine("[app] screenshot %s: %s", options_.screenshotPath.c_str(), screenshotDone_ ? "written" : "failed");
    }
}

// ---- editor input ----

void App::syncEditor() {
    editor_.sync(client_->layout(), panel_->geometry());
    if (editor_.takePressCancelled()) {
        // An edit in VR moved the version under a held drag: it can't be applied to the new layout, so say so
        if (GetCapture() == hwnd_) ReleaseCapture();
        setNotice("Layout changed in the headset, reloaded. Your last move wasn't applied.");
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool App::editorUsable() {
    if (!client_->subscribed() || !client_->haveLayout()) {
        setNotice("Connect to the headset to edit the layout.", 4);
        return false;
    }
    if (client_->editPending()) {
        setNotice("Still saving the last change on the headset…", 3);
        return false;
    }
    syncEditor();
    return true;
}

void App::pointerDown(double x, double y) {
    if (!editorUsable()) return;
    editor_.pointerDown(x, y);
    if (editor_.busy()) SetCapture(hwnd_);
    takeEditorCommit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::pointerUp(double x, double y) {
    if (editor_.busy()) {
        syncEditor();  // may cancel the press (the version moved), which it reports
        if (editor_.busy()) editor_.pointerUp(x, y);
    }
    // Always let go of the mouse, whatever happened to the press. After the editor saw the release: ReleaseCapture
    // sends WM_CAPTURECHANGED, which cancels a press still in progress
    if (GetCapture() == hwnd_) ReleaseCapture();
    takeEditorCommit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::takeEditorCommit() {
    uint32_t base = 0;
    Layout edited;
    if (editor_.takeCommit(base, edited)) submit(edited, base);
}

void App::cancelDrag() {
    const bool was = editor_.cancel();
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (was) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::submit(const Layout& edited, uint32_t baseVersion) {
    if (!client_->submitEdit(edited, baseVersion, nowSec())) {
        setNotice("Couldn't send that change: the headset isn't connected.", 4);
    } else {
        editInFlight_ = true;
    }
    afterLinkEvents();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- window procedure ----

LRESULT CALLBACK App::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    }
    if (app != nullptr) return app->handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == showMessage() && showMessage() != 0) {
        showWindow();
        return 0;
    }
    if (msg == taskbarCreated_ && taskbarCreated_ != 0) {
        trayAdded_ = false;
        addTray();  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_APP_SOCKET:
            if (WSAGETSELECTEVENT(lp) == FD_READ) {
                if (socket_.receiveAll(*client_, nowSec()) > 0) panelDirty_ = true;
                afterLinkEvents();
            }
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    showWindow();
                    return 0;
                case WM_CONTEXTMENU: {
                    POINT pt {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
                    showMenu(pt, true);
                    return 0;
                }
            }
            return 0;
        case WM_TIMER:
            if (wp == kTickTimer) onTick();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd_, &ps);
            paint(hdc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (updateButton_.contains(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {  // Release G
                POINT pt {static_cast<LONG>(updateButton_.x), static_cast<LONG>(updateButton_.y + updateButton_.h)};
                ClientToScreen(hwnd_, &pt);
                showUpdateMenu(pt);
                return 0;
            }
            if (saveButton_.contains(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {  // T11
                command(kMenuSave);
                return 0;
            }
            {  // R3: the tabs, and the Profiles / Logs pages' buttons
                const PageAction a = pages_.press(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nowSec(), pagesView());
                if (a == PageAction::TabLayout || a == PageAction::TabProfiles || a == PageAction::TabLogs) {
                    if (a != PageAction::TabLayout && editing_) toggleEditing();
                    panelDirty_ = true;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (pages_.tab() != PcTab::Layout) {
                    runPageAction(a);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            if (editing_) pointerDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            if (editing_ && editor_.busy()) {
                syncEditor();
                if (editor_.busy() && editor_.pointerMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            pointerUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd_) cancelDrag();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && lp == 0) command(LOWORD(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == 'E' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) toggleEditing();
            if (wp == VK_ESCAPE) {
                if (editor_.busy()) cancelDrag();
                else if (editing_) toggleEditing();
            }
            return 0;
        case WM_CONTEXTMENU: {
            POINT pt {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {  // keyboard: the menu key
                RECT r;
                GetWindowRect(hwnd_, &r);
                pt = {r.left + 40, r.top + 40};
            }
            POINT client = pt;
            ScreenToClient(hwnd_, &client);
            if (editing_ && client_->subscribed() && !client_->editPending() && !editor_.busy()) {
                // Right-click a card: select it (its settings show in the palette card) and offer them as a menu too
                const int slot = editor_.cardAt(client.x, client.y);
                if (slot >= 0) {
                    editor_.select(refOfSlot(client_->layout(), panel_->geometry(), slot));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    showCardMenu(pt, slot);
                    return 0;
                }
            }
            showMenu(pt, false);
            return 0;
        }
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                logLine("[app] resumed from sleep: %s", client_->debugText(nowSec()).c_str());
            } else if (wp == PBT_APMSUSPEND) {
                logLine("[app] going to sleep");
            }
            return TRUE;
        case WM_CLOSE:
            hideToTray();  // X hides; Exit (tray / right-click menu) quits
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) exitApp();  // logoff / shutdown: still say BYE
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool App::init() {
    startedAt_ = nowSec();
    loadState();

    HRSRC regular = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_FONT_REGULAR), RT_RCDATA);
    HRSRC bold = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_FONT_BOLD), RT_RCDATA);
    const auto bytes = [&](HRSRC r) { return r ? static_cast<const unsigned char*>(LockResource(LoadResource(instance_, r))) : nullptr; };
    const auto size = [&](HRSRC r) { return r ? static_cast<size_t>(SizeofResource(instance_, r)) : size_t(0); };
    if (!fonts_.loadFromMemory(bytes(regular), size(regular), bytes(bold), size(bold))) {
        logLine("[app] the embedded fonts didn't load; using a system font");
    }
    panel_.reset(new PanelView(fonts_));

    link::Id128 id {};
    link::idFromHex(settings_.pcId, id);
    client_.reset(new LinkClient(options_.offline ? static_cast<LinkClientEnv&>(offlineEnv_) : env_, id,
                                 computerName()));
    pushDiscoveryTargets();

    // The cached layout (or the headset's first-run one) is drawn greyed until the headset's arrives
    Layout cached = defaultLayout();
    {
        std::ifstream in(widen(cachePath_).c_str(), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::vector<std::string> warnings;
        std::string error;
        if (in && parseLayoutText(text.str(), cached, warnings, error)) {
            logLine("[app] cached layout v%u", cached.version);
        } else {
            cached = defaultLayout();
            logLine("[app] no cached layout: showing the default one until the headset answers");
        }
    }
    client_->setCachedLayout(cached);

    makeIcons();
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance_;
    wc.hIcon = iconBig_;
    wc.hIconSm = iconSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    hwnd_ = CreateWindowExW(0, kMainWindowClass, L"frame-perf-hud", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            960, 720, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) return false;
    logLine("[app] DPI awareness %d (2 = per-monitor), window DPI %u",
            static_cast<int>(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext())),
            GetDpiForWindow(hwnd_));
    // Let the second launch's "show" message through even if this process runs elevated
    ChangeWindowMessageFilterEx(hwnd_, showMessage(), MSGFLT_ALLOW, nullptr);
    afterLinkEvents();
    restorePlacement();

    std::string error;
    if (options_.offline) {
        logLine("[app] --offline: no socket, nothing is sent");
    } else if (!socket_.open(error, hwnd_, WM_APP_SOCKET)) {
        logLine("[app] %s", error.c_str());
        MessageBoxW(hwnd_, widen("frame-perf-hud couldn't open its network socket: " + error).c_str(),
                    L"frame-perf-hud", MB_OK | MB_ICONERROR);
        return false;
    } else {
        logLine("[app] UDP socket on port %u", socket_.localPort());
    }
    addTray();
    if (settings_.onTop) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hwnd_, kTickTimer, options_.startInTray ? kTrayTickMs : kTickMs, nullptr);
    client_->tick(nowSec());
    if (!options_.startInTray) showWindow();
    if (options_.justUpdated) setNotice(std::string("Updated to ") + FRAME_PERF_HUD_VERSION, 15);  // Release G
    afterUpdaterChange();  // the remembered "update available", if any (and the tray tip)
    return true;
}

int App::run() {
    if (!init()) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logLine("[app] bye");
    return 0;
}

}  // namespace

int runApp(HINSTANCE instance, const AppOptions& options) {
    App app(instance, options);
    return app.run();
}

}  // namespace pc
}  // namespace fph
