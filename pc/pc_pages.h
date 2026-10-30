// R3: the PC window's tabs (Layout | Profiles | Logs) and the Profiles / Logs pages, drawn like the headset's dashboard
// pages. Platform-free (cairo + the shared draw helpers), so tests can hit-test it; app_window.cpp carries the actions
// out (CONTROL_ACTION to the headset, the PC's own Save / logs folder).
//
// Profiles are the headset's (they live on the Frame): the page lists CONTROL_STATE's profiles and every button is a
// request the headset decides on. The second-tap rules match the dashboard: Delete always, Use / Reset when the headset
// says its layout has unsaved changes. The headset checks again on its own state (controlActionConfirmed), and a
// NeedsConfirm answer arms the button here for the second tap (armFromHeadset).
#pragma once

#include "draw.h"
#include "link_wire.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fph {
namespace pc {

enum class PcTab { Layout, Profiles, Logs };

/** What a press on the tabs / pages asks for. */
enum class PageAction : uint8_t {
    None,
    TabLayout,
    TabProfiles,
    TabLogs,
    Row0,  ///< Row0 + i: select profile row i (handled inside)
    Row1,
    Row2,
    Row3,
    Row4,
    Row5,
    Row6,
    Row7,
    ProfileNew,
    ProfileUse,
    ProfileSave,
    ProfileRename,  ///< the app opens its text box, then sends ProfileRename
    ProfileDelete,
    ProfileReset,
    RecordOn,
    RecordOff,
    CrashOn,
    CrashOff,
    SaveLocal,   ///< the PC's own Save last 10 min
    OpenFolder,  ///< the PC's data folder in Explorer
};

/** A rectangle in window px. */
struct PageBox {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/** What the pages show. */
struct PagesView {
    const link::ControlState* headset = nullptr;  ///< nullptr: no CONTROL_STATE yet (not connected, or an older headset)
    bool subscribed = false;                      ///< the link is live (buttons that talk to the headset work)
    bool busy = false;                            ///< a headset action waits for its answer
    std::string headsetMessage;                   ///< the last headset action's answer
    std::string pcMessage;                        ///< the last PC action's result (Save)
    std::vector<std::string> lastCard;            ///< the PC's last saved session's card
    std::string dataDir;                          ///< shown under the PC card
};

/**
 * The status bar's "Direct Save" button, top right, as wide as its label (shared by the window and the
 * preview tool so both draw the same).
 * @param width the window's width (px)
 * @param barH the status bar's height
 * @return its box (hit area)
 */
PageBox drawStatusSaveButton(const Pen& pen, double width, double barH, double s);

class PcPages {
public:
    static constexpr double kArmSec = 3.0;  ///< a second tap counts within this long (as on the dashboard)

    PcTab tab() const { return tab_; }
    void setTab(PcTab tab);

    /** @return the tab bar's height (window px) at DPI scale s */
    static double tabBarHeight(double s) { return 48 * s; }

    /** Draws the tab bar (a segmented pill) at (x, y), places its hit boxes. */
    void drawTabs(const Pen& pen, double x, double y, double s);

    /** Draws the Profiles or Logs page into area (nothing for Layout: the window draws the panel there). */
    void drawPage(const Pen& pen, const PageBox& area, double s, const PagesView& view);

    /**
     * A press at (x, y) on the tabs or the page.
     * @return the action to carry out now: None while a two-tap button waits for its second tap, for row selection,
     *         and for disabled buttons. Tabs switch here and are returned too (the window may react)
     */
    PageAction press(double x, double y, double now, const PagesView& view);

    /** @return true if the last action press() returned was a confirmed second tap (send kControlConfirmed) */
    bool lastConfirmed() const { return lastConfirmed_; }
    /** @return the layout version that confirmation was for */
    uint32_t confirmVersion() const { return confirmVersion_; }

    /**
     * The headset answered NeedsConfirm (its layout has unsaved changes it would lose, or changed since the PC's tap):
     * arm that button so the next tap within kArmSec confirms, bound to the headset's current version.
     */
    void armFromHeadset(PageAction action, uint32_t version, double now);
    /** @return the button waiting for its second tap (None if none) */
    PageAction armed() const { return armed_; }
    /** Drops a pending second tap (the connection changed: it was for another state) */
    void cancelArm() { armed_ = PageAction::None; }
    /** Expires a pending second tap. @return true if something changed (redraw) */
    bool tick(double now);

    /** @return the profile selected on the Profiles page ("" = none) */
    const std::string& selectedProfile() const { return selected_; }
    void selectProfile(const std::string& id) { selected_ = id; }

    /** @return the box of a button as last drawn (tests); false if it isn't on the page */
    bool boxOf(PageAction action, PageBox& out) const;
    /** @return true if a button is drawn disabled (not pressable) */
    bool disabled(PageAction action) const;

private:
    PcTab tab_ = PcTab::Layout;
    std::vector<std::pair<PageAction, PageBox>> tabBoxes_;
    std::vector<std::pair<PageAction, PageBox>> boxes_;  ///< the page's, as last drawn
    std::vector<PageAction> disabled_;
    std::vector<std::string> rowIds_;
    std::string selected_;
    PageAction armed_ = PageAction::None;
    double armedUntil_ = 0;
    uint32_t armedVersion_ = 0;
    bool lastConfirmed_ = false;
    uint32_t confirmVersion_ = 0;

    void button(const Pen& pen, PageAction action, const std::string& label, const PageBox& box, double s, bool enabled,
                bool primary);
    void drawProfiles(const Pen& pen, const PageBox& area, double s, const PagesView& view);
    void drawLogs(const Pen& pen, const PageBox& area, double s, const PagesView& view);
    /** The Logs page at text scale s. @return the lowest baseline drawn (drawLogs shrinks s until it fits) */
    double drawLogsAt(const Pen& pen, const PageBox& area, double cardS, double s, const PagesView& view);
};

}  // namespace pc
}  // namespace fph
