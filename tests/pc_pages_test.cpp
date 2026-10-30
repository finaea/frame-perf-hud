// PcPages (the PC window's Layout | Profiles | Logs tabs): hit boxes, the two-tap rules, arming from the headset,
// disabled buttons and the Logs pills. Draws offscreen with the real fonts (FPH_FONT_DIR), then presses the boxes it
// drew, like the window does.
#include "pc_pages.h"

#include "draw.h"
#include "link_wire.h"

#include <cairo.h>

#include <cstdio>
#include <string>

using namespace fph;
using namespace fph::pc;

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kScale = 1.0;
const PageBox kArea {8, 100, 900, 520};

/** One offscreen surface + fonts; draw() lays out the tabs and the current page, press() taps a drawn box's centre. */
struct Rig {
    FontSet fonts;
    cairo_surface_t* surface;
    cairo_t* cr;
    PcPages pages;
    PagesView view;
    link::ControlState state;

    Rig() {
        fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 960, 640);
        cr = cairo_create(surface);
        state.revision = 1;
        state.layoutVersion = 42;
        state.maxProfiles = 8;
        state.profiles = {{"profile-1", "Gaming"}, {"profile-2", "Work"}, {"profile-3", "Minimal"}};
        state.activeId = "profile-1";
        state.recordStatus = "Recording 0:05";
        state.powerOff = "Normal shutdown";
        view.headset = &state;
        view.subscribed = true;
        view.dataDir = "C:\\data";
    }
    ~Rig() {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void draw() {
        Pen pen {cr, &fonts};
        pages.drawTabs(pen, 8, 30, kScale);
        pages.drawPage(pen, kArea, kScale, view);
    }
    /** Taps the centre of a drawn box. Fails the test if it isn't drawn. */
    PageAction tap(PageAction a, double now) {
        PageBox b;
        if (!pages.boxOf(a, b)) {
            std::printf("FAIL: box %d not drawn\n", static_cast<int>(a));
            ++failures;
            return PageAction::None;
        }
        return pages.press(b.x + b.w / 2, b.y + b.h / 2, now, view);
    }
    void go(PcTab tab) {
        PageAction a = tab == PcTab::Layout ? PageAction::TabLayout : tab == PcTab::Profiles ? PageAction::TabProfiles : PageAction::TabLogs;
        draw();
        CHECK(tap(a, 0.0) == a);
        CHECK(pages.tab() == tab);
        draw();
    }
};

void testTabs() {
    Rig r;
    CHECK(r.pages.tab() == PcTab::Layout);
    r.draw();
    PageBox lay, pro, logs;
    CHECK(r.pages.boxOf(PageAction::TabLayout, lay) && r.pages.boxOf(PageAction::TabProfiles, pro) &&
          r.pages.boxOf(PageAction::TabLogs, logs));
    CHECK(lay.w > 0 && lay.h > 0 && lay.x + lay.w <= pro.x + 1e-6 && pro.x + pro.w <= logs.x + 1e-6);
    // nothing is drawn for the Layout tab: no page boxes, presses on the page area do nothing
    PageBox none;
    CHECK(!r.pages.boxOf(PageAction::ProfileUse, none) && !r.pages.boxOf(PageAction::RecordOn, none));
    CHECK(r.pages.press(kArea.x + 50, kArea.y + 50, 0.0, r.view) == PageAction::None);

    CHECK(r.tap(PageAction::TabProfiles, 0.1) == PageAction::TabProfiles && r.pages.tab() == PcTab::Profiles);
    r.draw();
    PageBox use;
    CHECK(r.pages.boxOf(PageAction::ProfileUse, use) && !r.pages.boxOf(PageAction::RecordOn, none));
    CHECK(r.tap(PageAction::TabLogs, 0.2) == PageAction::TabLogs && r.pages.tab() == PcTab::Logs);
    r.draw();
    CHECK(r.pages.boxOf(PageAction::RecordOn, none) && !r.pages.boxOf(PageAction::ProfileUse, none));
    CHECK(r.tap(PageAction::TabLayout, 0.3) == PageAction::TabLayout && r.pages.tab() == PcTab::Layout);
    // a tap just outside the bar's boxes switches nothing
    CHECK(r.pages.press(lay.x - 5, lay.y + lay.h / 2, 0.4, r.view) == PageAction::None && r.pages.tab() == PcTab::Layout);
    // setTab disarms a pending tap
    r.go(PcTab::Profiles);
    CHECK(r.tap(PageAction::ProfileDelete, 1.0) == PageAction::None && r.pages.armed() == PageAction::ProfileDelete);
    r.pages.setTab(PcTab::Logs);
    CHECK(r.pages.armed() == PageAction::None);
}

void testDeleteTwoTaps() {
    Rig r;
    r.go(PcTab::Profiles);
    CHECK(r.pages.selectedProfile() == "profile-1");  // the active one is selected first
    // first tap: nothing yet, armed; the second within 3 s: ProfileDelete, confirmed
    CHECK(r.tap(PageAction::ProfileDelete, 10.0) == PageAction::None);
    CHECK(r.pages.armed() == PageAction::ProfileDelete && !r.pages.lastConfirmed());
    r.draw();  // the armed label redraws
    CHECK(r.tap(PageAction::ProfileDelete, 11.5) == PageAction::ProfileDelete);
    CHECK(r.pages.lastConfirmed() && r.pages.armed() == PageAction::None);
    // delete is two taps even with nothing unsaved, and again after: the arm is consumed
    CHECK((r.state.flags & link::kControlUnsaved) == 0);
    CHECK(r.tap(PageAction::ProfileDelete, 12.0) == PageAction::None && !r.pages.lastConfirmed());
    // exactly at the 3 s edge still counts, just past it doesn't
    CHECK(r.tap(PageAction::ProfileDelete, 15.0) == PageAction::ProfileDelete && r.pages.lastConfirmed());
    CHECK(r.tap(PageAction::ProfileDelete, 20.0) == PageAction::None);
    CHECK(r.tap(PageAction::ProfileDelete, 23.01) == PageAction::None && !r.pages.lastConfirmed());  // expired, re-armed
    CHECK(r.pages.armed() == PageAction::ProfileDelete);
    CHECK(r.tap(PageAction::ProfileDelete, 23.5) == PageAction::ProfileDelete && r.pages.lastConfirmed());
}

void testUseAndReset() {
    for (PageAction a : {PageAction::ProfileUse, PageAction::ProfileReset}) {
        {
            // unsaved: two taps; the confirmation is bound to the version seen at the FIRST tap
            Rig r;
            r.state.flags = link::kControlUnsaved;
            r.go(PcTab::Profiles);
            CHECK(r.tap(a, 5.0) == PageAction::None && r.pages.armed() == a);
            r.state.layoutVersion = 43;  // an edit lands between the taps (state refreshes)
            r.draw();
            CHECK(r.tap(a, 6.0) == a && r.pages.lastConfirmed() && r.pages.confirmVersion() == 42);
        }
        {
            // nothing unsaved: one tap, not confirmed
            Rig r;
            r.go(PcTab::Profiles);
            CHECK(r.tap(a, 5.0) == a && !r.pages.lastConfirmed() && r.pages.armed() == PageAction::None);
            // the next press clears lastConfirmed even after a confirmed one
            r.state.flags = link::kControlUnsaved;
            r.draw();
            CHECK(r.tap(a, 6.0) == PageAction::None && !r.pages.lastConfirmed());
            CHECK(r.tap(a, 6.5) == a && r.pages.lastConfirmed());
            CHECK(r.tap(PageAction::ProfileSave, 7.0) == PageAction::ProfileSave && !r.pages.lastConfirmed());
        }
    }
    {
        // the unsaved flag read at the tap, not cached: a flag that clears between the taps still confirms
        Rig r;
        r.state.flags = link::kControlUnsaved;
        r.go(PcTab::Profiles);
        CHECK(r.tap(PageAction::ProfileUse, 1.0) == PageAction::None);
        r.state.flags = 0;
        r.draw();
        CHECK(r.tap(PageAction::ProfileUse, 1.5) == PageAction::ProfileUse && r.pages.lastConfirmed());
    }
}

void testArmCancelAndExpire() {
    Rig r;
    r.state.flags = link::kControlUnsaved;
    r.go(PcTab::Profiles);
    // another press cancels an arm: Use armed, tap Save (one tap), then Use again is a first tap again
    CHECK(r.tap(PageAction::ProfileUse, 1.0) == PageAction::None && r.pages.armed() == PageAction::ProfileUse);
    CHECK(r.tap(PageAction::ProfileSave, 1.5) == PageAction::ProfileSave && r.pages.armed() == PageAction::None);
    CHECK(r.tap(PageAction::ProfileUse, 2.0) == PageAction::None && r.pages.armed() == PageAction::ProfileUse);
    // arming a different two-tap button moves the arm (Use armed, Delete tapped: Delete armed, Use not)
    CHECK(r.tap(PageAction::ProfileDelete, 2.2) == PageAction::None && r.pages.armed() == PageAction::ProfileDelete);
    CHECK(r.tap(PageAction::ProfileUse, 2.4) == PageAction::None && r.pages.armed() == PageAction::ProfileUse);
    // a tap on empty space cancels too
    CHECK(r.pages.press(2000, 2000, 2.5, r.view) == PageAction::None && r.pages.armed() == PageAction::None);
    // a row tap cancels
    CHECK(r.tap(PageAction::ProfileUse, 3.0) == PageAction::None && r.pages.armed() == PageAction::ProfileUse);
    CHECK(r.tap(PageAction::Row1, 3.1) == PageAction::None && r.pages.armed() == PageAction::None);
    CHECK(r.tap(PageAction::ProfileUse, 3.2) == PageAction::None);  // still a first tap (and not confirmed)
    CHECK(!r.pages.lastConfirmed());

    // tick: expires after 3 s (not before), reports the change once
    r.pages.setTab(PcTab::Profiles);
    CHECK(r.tap(PageAction::ProfileDelete, 10.0) == PageAction::None && r.pages.armed() == PageAction::ProfileDelete);
    CHECK(!r.pages.tick(10.5) && !r.pages.tick(13.0) && r.pages.armed() == PageAction::ProfileDelete);
    CHECK(r.pages.tick(13.01) && r.pages.armed() == PageAction::None);
    CHECK(!r.pages.tick(14.0));  // nothing left to expire
    // after expiry the tap is a first tap again
    CHECK(r.tap(PageAction::ProfileDelete, 14.0) == PageAction::None && !r.pages.lastConfirmed());
}

void testArmFromHeadset() {
    Rig r;
    r.go(PcTab::Profiles);  // nothing unsaved in this view: a plain Use would be one tap
    r.pages.armFromHeadset(PageAction::ProfileUse, 77, 100.0);
    CHECK(r.pages.armed() == PageAction::ProfileUse);
    CHECK(r.tap(PageAction::ProfileUse, 101.0) == PageAction::ProfileUse);
    CHECK(r.pages.lastConfirmed() && r.pages.confirmVersion() == 77);
    CHECK(r.pages.armed() == PageAction::None);
    // expires like any arm
    r.pages.armFromHeadset(PageAction::ProfileReset, 78, 200.0);
    CHECK(!r.pages.tick(203.0) && r.pages.tick(203.1) && r.pages.armed() == PageAction::None);
    // a tap on another button disarms it
    r.pages.armFromHeadset(PageAction::ProfileUse, 79, 300.0);
    CHECK(r.tap(PageAction::ProfileSave, 301.0) == PageAction::ProfileSave && r.pages.armed() == PageAction::None);
    CHECK(r.tap(PageAction::ProfileUse, 302.0) == PageAction::ProfileUse && !r.pages.lastConfirmed());
    // a headset-armed button for an unsaved view: the confirmation keeps the headset's version, not the view's
    r.state.flags = link::kControlUnsaved;
    r.draw();
    r.pages.armFromHeadset(PageAction::ProfileReset, 99, 400.0);
    CHECK(r.tap(PageAction::ProfileReset, 400.5) == PageAction::ProfileReset && r.pages.confirmVersion() == 99);
    // the label shows the arm
    r.pages.armFromHeadset(PageAction::ProfileDelete, 5, 500.0);
    r.draw();
    CHECK(r.pages.armed() == PageAction::ProfileDelete);
}

// Lane F review: a connection change drops a pending second tap (it was for the old connection's state)
void testCancelArm() {
    Rig r;
    r.go(PcTab::Profiles);
    CHECK(r.tap(PageAction::ProfileDelete, 10.0) == PageAction::None && r.pages.armed() == PageAction::ProfileDelete);
    r.pages.cancelArm();
    CHECK(r.pages.armed() == PageAction::None);
    CHECK(r.tap(PageAction::ProfileDelete, 10.5) == PageAction::None);  // a first tap again, not a confirmation
    CHECK(!r.pages.lastConfirmed());
}

void testRowSelect() {
    Rig r;
    r.go(PcTab::Profiles);
    CHECK(r.pages.selectedProfile() == "profile-1");
    CHECK(r.tap(PageAction::Row2, 1.0) == PageAction::None && r.pages.selectedProfile() == "profile-3");
    CHECK(r.tap(PageAction::Row1, 1.1) == PageAction::None && r.pages.selectedProfile() == "profile-2");
    r.draw();
    CHECK(r.pages.selectedProfile() == "profile-2");  // a redraw keeps the selection
    // rows past the list aren't boxes
    PageBox b;
    CHECK(!r.pages.boxOf(PageAction::Row3, b) && r.pages.boxOf(PageAction::Row0, b));
    // a vanished selection falls back to the active profile
    r.state.profiles.erase(r.state.profiles.begin() + 1);
    r.draw();
    CHECK(r.pages.selectedProfile() == "profile-1");
    // no active: first row
    r.state.activeId = "";
    r.pages.selectProfile("gone");
    r.draw();
    CHECK(r.pages.selectedProfile() == "profile-1");
    // selecting while rows are disabled-adjacent: busy doesn't block selecting
    r.view.busy = true;
    r.draw();
    CHECK(r.tap(PageAction::Row1, 2.0) == PageAction::None && r.pages.selectedProfile() == "profile-3");
}

void testDisabled() {
    const PageAction headsetButtons[] = {PageAction::ProfileUse,    PageAction::ProfileRename, PageAction::ProfileDelete,
                                         PageAction::ProfileNew,    PageAction::ProfileSave,   PageAction::ProfileReset};
    {
        // no CONTROL_STATE (older headset / not connected): every button is disabled and does nothing
        Rig r;
        r.view.headset = nullptr;
        r.go(PcTab::Profiles);
        PageBox b;
        for (PageAction a : headsetButtons) {
            if (r.pages.boxOf(a, b)) CHECK(r.pages.disabled(a) && r.tap(a, 1.0) == PageAction::None);
        }
        r.go(PcTab::Logs);
        for (PageAction a : {PageAction::RecordOn, PageAction::RecordOff, PageAction::CrashOn, PageAction::CrashOff}) {
            CHECK(r.pages.boxOf(a, b) && r.pages.disabled(a) && r.tap(a, 1.0) == PageAction::None);
        }
        // the PC's own buttons still work
        CHECK(!r.pages.disabled(PageAction::SaveLocal) && r.tap(PageAction::SaveLocal, 2.0) == PageAction::SaveLocal);
        CHECK(!r.pages.disabled(PageAction::OpenFolder) && r.tap(PageAction::OpenFolder, 2.0) == PageAction::OpenFolder);
    }
    {
        // not subscribed (state cached from before): disabled
        Rig r;
        r.view.subscribed = false;
        r.go(PcTab::Profiles);
        for (PageAction a : headsetButtons) CHECK(r.pages.disabled(a) && r.tap(a, 1.0) == PageAction::None);
        r.go(PcTab::Logs);
        CHECK(r.pages.disabled(PageAction::RecordOn) && r.tap(PageAction::CrashOff, 1.0) == PageAction::None);
        CHECK(r.tap(PageAction::SaveLocal, 1.0) == PageAction::SaveLocal);
    }
    {
        // busy: every headset button is disabled; the PC's isn't
        Rig r;
        r.view.busy = true;
        r.go(PcTab::Profiles);
        for (PageAction a : headsetButtons) CHECK(r.pages.disabled(a) && r.tap(a, 1.0) == PageAction::None);
        CHECK(r.pages.armed() == PageAction::None);  // a disabled Delete can't arm
        r.go(PcTab::Logs);
        CHECK(r.pages.disabled(PageAction::RecordOff) && r.tap(PageAction::RecordOff, 1.0) == PageAction::None);
        CHECK(r.tap(PageAction::OpenFolder, 1.0) == PageAction::OpenFolder);
        // busy ends: enabled again
        r.view.busy = false;
        r.draw();
        CHECK(!r.pages.disabled(PageAction::RecordOff) && r.tap(PageAction::RecordOff, 2.0) == PageAction::RecordOff);
    }
    {
        // New at 8 of 8 profiles
        Rig r;
        r.state.profiles.clear();
        for (int i = 1; i <= 8; ++i) r.state.profiles.push_back({"profile-" + std::to_string(i), "P" + std::to_string(i)});
        r.state.activeId = "profile-1";
        r.go(PcTab::Profiles);
        CHECK(r.pages.disabled(PageAction::ProfileNew) && r.tap(PageAction::ProfileNew, 1.0) == PageAction::None);
        CHECK(!r.pages.disabled(PageAction::ProfileUse) && !r.pages.disabled(PageAction::ProfileSave));
        PageBox b;
        CHECK(r.pages.boxOf(PageAction::Row7, b));
        CHECK(r.tap(PageAction::Row7, 1.1) == PageAction::None && r.pages.selectedProfile() == "profile-8");
        // 7 of 8: enabled
        r.state.profiles.pop_back();
        r.draw();
        CHECK(!r.pages.disabled(PageAction::ProfileNew) && r.tap(PageAction::ProfileNew, 2.0) == PageAction::ProfileNew);
        // a smaller maxProfiles from the headset is honoured
        r.state.maxProfiles = 7;
        r.draw();
        CHECK(r.pages.disabled(PageAction::ProfileNew));
    }
    {
        // Save with no active profile; Use / Rename / Delete still work on the selected row; Reset needs no profile
        Rig r;
        r.state.activeId = "";
        r.go(PcTab::Profiles);
        CHECK(r.pages.disabled(PageAction::ProfileSave) && r.tap(PageAction::ProfileSave, 1.0) == PageAction::None);
        CHECK(!r.pages.disabled(PageAction::ProfileUse) && !r.pages.disabled(PageAction::ProfileRename));
        CHECK(!r.pages.disabled(PageAction::ProfileReset) && !r.pages.disabled(PageAction::ProfileNew));
        CHECK(r.pages.selectedProfile() == "profile-1");  // falls back to the first row
        CHECK(r.tap(PageAction::ProfileRename, 2.0) == PageAction::ProfileRename);
        // active names a profile that isn't in the list: Save disabled too
        r.state.activeId = "profile-9";
        r.draw();
        CHECK(r.pages.disabled(PageAction::ProfileSave));
    }
    {
        // an empty list: nothing selected, Use / Rename / Delete disabled, New and Reset enabled
        Rig r;
        r.state.profiles.clear();
        r.state.activeId = "";
        r.go(PcTab::Profiles);
        CHECK(r.pages.selectedProfile().empty());
        CHECK(r.pages.disabled(PageAction::ProfileUse) && r.pages.disabled(PageAction::ProfileRename) &&
              r.pages.disabled(PageAction::ProfileDelete) && r.pages.disabled(PageAction::ProfileSave));
        CHECK(!r.pages.disabled(PageAction::ProfileNew) && !r.pages.disabled(PageAction::ProfileReset));
        CHECK(r.tap(PageAction::ProfileDelete, 1.0) == PageAction::None && r.pages.armed() == PageAction::None);
        CHECK(r.tap(PageAction::ProfileNew, 2.0) == PageAction::ProfileNew);
        PageBox b;
        CHECK(!r.pages.boxOf(PageAction::Row0, b));
    }
}

void testLogsPills() {
    Rig r;
    r.go(PcTab::Logs);
    CHECK(r.tap(PageAction::RecordOn, 1.0) == PageAction::RecordOn && !r.pages.lastConfirmed());
    CHECK(r.tap(PageAction::RecordOff, 1.1) == PageAction::RecordOff);
    CHECK(r.tap(PageAction::CrashOn, 1.2) == PageAction::CrashOn);
    CHECK(r.tap(PageAction::CrashOff, 1.3) == PageAction::CrashOff);
    // the four pills are distinct, non-overlapping boxes
    const PageAction pills[] = {PageAction::RecordOn, PageAction::RecordOff, PageAction::CrashOn, PageAction::CrashOff};
    for (int i = 0; i < 4; ++i) {
        PageBox a;
        CHECK(r.pages.boxOf(pills[i], a) && a.w > 0 && a.h > 0);
        for (int j = i + 1; j < 4; ++j) {
            PageBox b;
            CHECK(r.pages.boxOf(pills[j], b));
            const bool apart = a.x + a.w <= b.x + 1e-6 || b.x + b.w <= a.x + 1e-6 || a.y + a.h <= b.y + 1e-6 || b.y + b.h <= a.y + 1e-6;
            CHECK(apart);
        }
    }
    // pressing a pill is never a two-tap, whatever the unsaved flag
    r.state.flags = link::kControlUnsaved | link::kControlRecording;
    r.draw();
    CHECK(r.tap(PageAction::RecordOff, 2.0) == PageAction::RecordOff && r.pages.armed() == PageAction::None);
    // the PC's own buttons are one tap too
    CHECK(r.tap(PageAction::SaveLocal, 3.0) == PageAction::SaveLocal && r.tap(PageAction::OpenFolder, 3.1) == PageAction::OpenFolder);
    // a page that was never drawn (fresh PcPages): presses do nothing
    PcPages fresh;
    fresh.setTab(PcTab::Logs);
    CHECK(fresh.press(100, 100, 1.0, r.view) == PageAction::None);
    // drawing the Profiles page after Logs drops the Logs boxes
    r.go(PcTab::Profiles);
    PageBox b;
    CHECK(!r.pages.boxOf(PageAction::RecordOn, b) && !r.pages.disabled(PageAction::RecordOn));
}

void testLongStrings() {
    // a worst-case state draws without trouble and keeps the boxes
    Rig r;
    r.state.status = std::string(127, 'x');
    r.state.recordStatus = std::string(127, 'r');
    r.state.crashStatus = std::string(63, 'c');
    r.state.profiles.clear();
    for (int i = 1; i <= 8; ++i) r.state.profiles.push_back({"profile-" + std::to_string(i), std::string(31, 'N')});
    r.view.headsetMessage = std::string(127, 'm');
    r.view.pcMessage = std::string(200, 'p');
    r.view.lastCard = {"one", "two", "three", "four", "five"};
    r.go(PcTab::Profiles);
    PageBox b;
    CHECK(r.pages.boxOf(PageAction::ProfileDelete, b));
    r.go(PcTab::Logs);
    CHECK(r.pages.boxOf(PageAction::CrashOff, b));
}

/** @return true if box b lies entirely inside area (the Logs page shrinks its own text to fit; the boxes must follow) */
bool insideArea(const PageBox& b, const PageBox& area) {
    return b.x >= area.x - 1e-6 && b.y >= area.y - 1e-6 && b.x + b.w <= area.x + area.w + 1e-6 &&
           b.y + b.h <= area.y + area.h + 1e-6;
}

// The Logs page shrinks its text/controls until the lowest line fits (drawLogsAt): check that really keeps every
// pill and button inside a cramped area, not just avoiding a crash -- a small window, and the preview's own size
void testLogsBoxesFitSmallArea() {
    Rig r;
    const struct {
        PageBox area;
        double scale;
    } cases[] = {
        {{0, 0, 960, 520}, 1.0},
        {{0, 0, 960, 640}, 1.25},
    };
    for (const auto& c : cases) {
        Pen pen {r.cr, &r.fonts};
        r.pages.setTab(PcTab::Logs);
        r.pages.drawPage(pen, c.area, c.scale, r.view);
        for (PageAction a : {PageAction::SaveLocal, PageAction::RecordOn, PageAction::RecordOff, PageAction::CrashOn,
                             PageAction::CrashOff}) {
            PageBox b;
            CHECK(r.pages.boxOf(a, b));
            CHECK(insideArea(b, c.area));
            if (!insideArea(b, c.area)) {
                std::printf("box %d at (%.1f,%.1f,%.1f,%.1f) outside area (%.1f,%.1f,%.1f,%.1f) scale %.2f\n",
                            static_cast<int>(a), b.x, b.y, b.w, b.h, c.area.x, c.area.y, c.area.w, c.area.h, c.scale);
            }
        }
    }
}

}  // namespace

int main() {
    testTabs();
    testDeleteTwoTaps();
    testUseAndReset();
    testArmCancelAndExpire();
    testArmFromHeadset();
    testCancelArm();  // lane F review
    testRowSelect();
    testDisabled();
    testLogsPills();
    testLongStrings();
    testLogsBoxesFitSmallArea();
    if (failures != 0) {
        std::printf("pc-pages-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pc-pages-test: all passed\n");
    return 0;
}
