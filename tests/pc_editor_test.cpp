// PcEditor (the PC's Ctrl+E editor): a layout sync during a held drag cancels it and reports it, and the selection
// follows the exact widget instance across syncs (review fixes, ticket 07).
#include "pc_editor.h"

#include "editor_model.h"
#include "grid_renderer.h"
#include "i18n.h"
#include "layout.h"

#include "metric_registry.h"
#include "metric_store.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fph;
using namespace fph::pc;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

WidgetInstance make(WidgetType type, NetAdapter adapter = NetAdapter::Dongle) {
    WidgetInstance w;
    w.type = type;
    w.adapter = adapter;
    return w;
}

/** Column 0: Frame, CPU items. Column 1: Dongle, Home. */
Layout base(uint32_t version) {
    Layout l;
    l.version = version;
    l.columns.resize(2);
    l.columns[0].widgets = {make(WidgetType::Frame), make(WidgetType::CpuItems)};
    l.columns[1].widgets = {make(WidgetType::Network, NetAdapter::Dongle), make(WidgetType::Network, NetAdapter::Home)};
    return l;
}

/** A 1:1 view: panel px = device px, the palette far to the right. */
void attach(PcEditor& e, const Layout& l) {
    e.sync(l, layoutGrid(l));
    PcEditor::View v;
    v.viewport = {0, 0, 4000, 3000};
    v.layoutCard = {0, 0, 4000, 3000};
    v.paletteCard = {5000, 0, 400, 3000};
    v.scale = 1.0;
    v.ui = 1.0;
    e.setView(v);
}

/** @return the centre of the card at ref (device px = panel px here) */
void centre(const Layout& l, WidgetRef ref, double& x, double& y) {
    const GridGeometry g = layoutGrid(l);
    for (size_t i = 0; i < g.slots.size(); ++i) {
        if (refOfSlot(l, g, static_cast<int>(i)) == ref) {
            x = g.slots[i].x + g.slots[i].w / 2.0;
            y = g.slots[i].y + g.slots[i].h / 2.0;
            return;
        }
    }
    x = y = -1;
}

void testSelectionFollowsInstance() {
    PcEditor e;
    attach(e, base(1));
    e.select({1, 0});  // Dongle
    check(e.selected() == WidgetRef({1, 0}), "select Dongle");

    // Edited elsewhere: Home and Dongle swap. The selection must follow Dongle, not stay on (1,0) = Home
    Layout swapped = base(2);
    std::swap(swapped.columns[1].widgets[0], swapped.columns[1].widgets[1]);
    e.sync(swapped, layoutGrid(swapped));
    check(e.selected() == WidgetRef({1, 1}), "selection followed Dongle to (1,1)");

    // Dongle removed elsewhere: no selection (not Home, which now sits where it was)
    Layout gone = swapped;
    gone.version = 3;
    gone.columns[1].widgets.pop_back();
    e.sync(gone, layoutGrid(gone));
    check(!e.selected().valid(), "selection dropped when its widget is gone");

    // Two identical instances: ambiguous, dropped rather than guessed
    PcEditor e2;
    Layout twins = base(1);
    twins.columns[1].widgets = {make(WidgetType::Network, NetAdapter::Home), make(WidgetType::Network, NetAdapter::Home)};
    attach(e2, twins);
    e2.select({1, 0});
    Layout twinsMoved = twins;
    twinsMoved.version = 2;
    twinsMoved.columns[1].widgets.insert(twinsMoved.columns[1].widgets.begin(), make(WidgetType::General));
    e2.sync(twinsMoved, layoutGrid(twinsMoved));
    check(!e2.selected().valid(), "ambiguous selection dropped");
}

void testOwnEditKeepsSelection() {
    PcEditor e;
    Layout l = base(1);
    attach(e, l);
    e.select({0, 1});  // CPU items
    // Rows + in the settings panel
    double bx = -1, by = -1;
    for (const auto& b : e.settingButtons()) {
        if (b.first == PcEditor::Setting::RowsUp) {
            bx = b.second.x + b.second.w / 2;
            by = b.second.y + b.second.h / 2;
        }
    }
    check(bx >= 0, "rows + button exists for a list widget");
    e.pointerDown(bx, by);
    e.pointerUp(bx, by);
    uint32_t baseVersion = 0;
    Layout edited;
    check(e.takeCommit(baseVersion, edited), "rows + queued a commit");
    check(baseVersion == 1, "commit base is the press's version");
    check(edited.columns[0].widgets[1].maxItems == 20, "rows 15 -> 20");
    // The window shows the edit (optimistic), then the ACK bumps the version: the selection stays on CPU items
    edited.version = 1;
    e.sync(edited, layoutGrid(edited));
    check(e.selected() == WidgetRef({0, 1}), "selection kept on the optimistic layout");
    edited.version = 2;
    e.sync(edited, layoutGrid(edited));
    check(e.selected() == WidgetRef({0, 1}), "selection kept after the ACK");
}

void testSyncCancelsHeldDrag() {
    PcEditor e;
    Layout l = base(1);
    attach(e, l);
    double x = 0, y = 0;
    centre(l, {0, 0}, x, y);
    e.pointerDown(x, y);
    e.pointerMove(x + 300, y + 40);
    check(e.busy() && e.dragging(), "dragging Frame");

    // A sync with the same version (a repaint) leaves the drag alone
    e.sync(l, layoutGrid(l));
    check(e.busy(), "same-version sync keeps the drag");
    check(!e.takePressCancelled(), "no cancel reported for a same-version sync");

    // An edit in VR moves the version under the drag: cancelled, and reported once
    Layout vr = base(2);
    e.sync(vr, layoutGrid(vr));
    check(!e.busy() && !e.dragging(), "version change cancelled the drag");
    check(e.takePressCancelled(), "cancel reported");
    check(!e.takePressCancelled(), "cancel reported only once");
    // The release that follows does nothing (no commit against the new layout)
    e.pointerUp(x + 300, y + 40);
    uint32_t baseVersion = 0;
    Layout edited;
    check(!e.takeCommit(baseVersion, edited), "no commit after a cancelled drag");
}

bool has(const PcEditor& e, PcEditor::Setting s) {
    for (const auto& b : e.settingButtons()) {
        if (b.first == s) return true;
    }
    return false;
}

/** Presses a setting button. @return true if a commit was queued (its layout in `edited`) */
bool press(PcEditor& e, PcEditor::Setting s, Layout& edited) {
    for (const auto& b : e.settingButtons()) {
        if (b.first != s) continue;
        const double x = b.second.x + b.second.w / 2, y = b.second.y + b.second.h / 2;
        e.pointerDown(x, y);
        e.pointerUp(x, y);
        uint32_t baseVersion = 0;
        return e.takeCommit(baseVersion, edited);
    }
    return false;
}

// R2: Compact buttons (Frame, Power, Battery, Temp) and Hide system buttons (lists)
void testCompactAndHideSystemButtons() {
    using S = PcEditor::Setting;
    Layout l;
    l.version = 1;
    l.columns.resize(1);
    l.columns[0].widgets = {make(WidgetType::Frame),     make(WidgetType::Power),   make(WidgetType::Battery),
                            make(WidgetType::Temp),      make(WidgetType::Streaming), make(WidgetType::Cpu),
                            make(WidgetType::Network),   make(WidgetType::CpuItems), make(WidgetType::RamItems)};
    PcEditor e;
    attach(e, l);
    const Box card = {5000, 0, 400, 3000};
    for (int i = 0; i < 9; ++i) {
        e.select({0, i});
        const WidgetType type = l.columns[0].widgets[static_cast<size_t>(i)].type;
        const bool wantCompact = type == WidgetType::Frame || type == WidgetType::Power ||
                                 type == WidgetType::Battery || type == WidgetType::Temp ||
                                 type == WidgetType::Streaming;  // R3
        const bool wantHide = type == WidgetType::CpuItems || type == WidgetType::RamItems;
        check(has(e, S::CompactOn) == wantCompact && has(e, S::CompactOff) == wantCompact,
              "Compact buttons only for Frame/Power/Battery/Temp");
        check(has(e, S::HideSystemOn) == wantHide && has(e, S::HideSystemOff) == wantHide,
              "Hide system buttons only for lists");
        double chipsBottom = 0;
        for (const auto& c : e.chips()) chipsBottom = std::max(chipsBottom, c.second.y + c.second.h);
        for (const auto& b : e.settingButtons()) {
            check(b.second.x >= card.x && b.second.x + b.second.w <= card.x + card.w && b.second.y >= chipsBottom &&
                      b.second.y + b.second.h <= card.y + card.h,
                  "setting button inside the palette card, below the chips");
        }
    }

    // Frame: Remove + CompactOn + CompactOff; pressing flips only `compact`
    e.select({0, 0});
    check(e.settingButtons().size() == 3, "Frame has Remove + CompactOn + CompactOff");
    Layout edited;
    check(press(e, S::CompactOn, edited), "Compact On queued a commit");
    Layout want = l;
    want.columns[0].widgets[0].compact = true;
    check(edited.sameWidgets(want), "Compact On changed only compact");

    // CPU items: Hide system On flips only hideSystem
    PcEditor e2;
    attach(e2, l);
    e2.select({0, 7});
    check(press(e2, S::HideSystemOn, edited), "Hide system On queued a commit");
    want = l;
    want.columns[0].widgets[7].hideSystem = true;
    check(edited.sameWidgets(want), "Hide system On changed only hideSystem");
}

// R3: every two-way setting is one pill over two adjacent halves (the two Setting boxes)
void testPills() {
    using S = PcEditor::Setting;
    struct Pill {
        WidgetType type;
        S on, off;  ///< `on` is the half that sets the field, `off` the other
        bool (*isOn)(const WidgetInstance&);
    };
    const Pill pills[] = {
        {WidgetType::RamItems, S::SameNameOn, S::SameNameOff, [](const WidgetInstance& w) { return w.sumSameName; }},
        {WidgetType::CpuItems, S::HideSystemOn, S::HideSystemOff, [](const WidgetInstance& w) { return w.hideSystem; }},
        {WidgetType::Frame, S::CompactOn, S::CompactOff, [](const WidgetInstance& w) { return w.compact; }},
        {WidgetType::Streaming, S::CompactOn, S::CompactOff, [](const WidgetInstance& w) { return w.compact; }},
        {WidgetType::Network, S::AdapterHome, S::AdapterDongle,
         [](const WidgetInstance& w) { return w.adapter == NetAdapter::Home; }},
        {WidgetType::HeadsetCamera, S::FullSizeOn, S::FullSizeOff,
         [](const WidgetInstance& w) { return w.cameraFullSize; }},
    };
    const Box card = {5000, 0, 400, 3000};
    for (const Pill& pill : pills) {
        Layout l;
        l.version = 1;
        l.columns.resize(1);
        l.columns[0].widgets = {make(pill.type)};
        PcEditor e;
        attach(e, l);
        e.select({0, 0});
        Box a, b;
        for (const auto& s : e.settingButtons()) {
            if (s.first == pill.on) a = s.second;
            if (s.first == pill.off) b = s.second;
        }
        if (pill.type == WidgetType::Network) std::swap(a, b);  // Dongle is the left half
        check(a.w > 0 && b.w > 0, "pill halves exist");
        check(a.y == b.y && a.h == b.h && a.w == b.w, "pill halves share a row and a width");
        check(a.x + a.w == b.x, "pill halves are adjacent");
        double chipsBottom = 0;
        for (const auto& c : e.chips()) chipsBottom = std::max(chipsBottom, c.second.y + c.second.h);
        for (const Box& r : {a, b}) {
            check(r.x >= card.x && r.x + r.w <= card.x + card.w && r.y >= chipsBottom && r.y + r.h <= card.y + card.h,
                  "pill inside the palette card, below the chips");
        }

        const WidgetInstance before = l.columns[0].widgets[0];
        check(!pill.isOn(before), "pill starts in the Off state");
        Layout edited;
        check(!press(e, pill.off, edited), "the half that is already the state queues nothing");
        check(press(e, pill.on, edited), "the On half queues an edit");
        check(pill.isOn(edited.columns[0].widgets[0]), "On half set the field");
        WidgetInstance want = before;
        if (pill.type == WidgetType::RamItems) want.sumSameName = true;
        if (pill.type == WidgetType::CpuItems) want.hideSystem = true;
        if (pill.type == WidgetType::Frame || pill.type == WidgetType::Streaming) want.compact = true;
        if (pill.type == WidgetType::Network) want.adapter = NetAdapter::Home;
        if (pill.type == WidgetType::HeadsetCamera) want.cameraFullSize = true;
        check(edited.columns[0].widgets[0] == want, "On half changed only its field");

        // and back: with the field set, the Off half queues the way back
        PcEditor e2;
        Layout on = l;
        on.columns[0].widgets[0] = want;
        attach(e2, on);
        e2.select({0, 0});
        check(!press(e2, pill.on, edited), "On half is a no-op when already on");
        check(press(e2, pill.off, edited), "Off half queues the way back");
        check(edited.columns[0].widgets[0] == before, "Off half restored the field");
    }

    // Headset camera: Full size is its own row, below the FPS stepper
    Layout l;
    l.version = 1;
    l.columns.resize(1);
    l.columns[0].widgets = {make(WidgetType::HeadsetCamera)};
    PcEditor e;
    attach(e, l);
    e.select({0, 0});
    Box fps, full;
    for (const auto& s : e.settingButtons()) {
        if (s.first == S::FpsDown) fps = s.second;
        if (s.first == S::FullSizeOn) full = s.second;
    }
    check(fps.w > 0 && full.w > 0 && full.y >= fps.y + fps.h, "Full size sits below the FPS row");
}

// HTTP camera: the Size stepper shares the FPS row, steps through 40..100 and stops at both ends
void testHttpCameraSize() {
    using S = PcEditor::Setting;
    Layout l;
    l.version = 1;
    l.columns.resize(1);
    l.columns[0].widgets = {make(WidgetType::Frame), make(WidgetType::HttpCamera)};
    PcEditor e;
    attach(e, l);
    e.select({0, 1});
    Box fpsDown, fpsUp, sizeDown, sizeUp;
    for (const auto& b : e.settingButtons()) {
        if (b.first == S::FpsDown) fpsDown = b.second;
        if (b.first == S::FpsUp) fpsUp = b.second;
        if (b.first == S::SizeDown) sizeDown = b.second;
        if (b.first == S::SizeUp) sizeUp = b.second;
    }
    check(fpsDown.w > 0 && fpsUp.w > 0 && sizeDown.w > 0 && sizeUp.w > 0, "HTTP camera has FPS and Size steppers");
    check(sizeDown.y == fpsDown.y && sizeUp.y == fpsDown.y, "Size is on the FPS row");
    check(fpsUp.x + fpsUp.w <= sizeDown.x && sizeDown.x + sizeDown.w <= sizeUp.x, "FPS pair, then Size pair");

    // only the HTTP camera has it
    Layout h;
    h.version = 1;
    h.columns.resize(1);
    h.columns[0].widgets = {make(WidgetType::HeadsetCamera), make(WidgetType::EyeCamera)};
    PcEditor other;
    attach(other, h);
    other.select({0, 0});
    check(!has(other, S::SizeDown) && !has(other, S::SizeUp), "Headset camera has no Size");
    other.select({0, 1});
    check(!has(other, S::SizeDown) && !has(other, S::SizeUp), "Eye camera has no Size");

    struct Step {
        uint8_t from;
        S button;
        int to;  // -1 = nothing queued
    };
    const Step steps[] = {
        {100, S::SizeDown, 90}, {90, S::SizeDown, 80}, {80, S::SizeDown, 70}, {70, S::SizeDown, 60},
        {60, S::SizeDown, 50},  {50, S::SizeDown, 40}, {40, S::SizeDown, -1}, {40, S::SizeUp, 50},
        {50, S::SizeUp, 60},    {60, S::SizeUp, 70},   {70, S::SizeUp, 80},   {80, S::SizeUp, 90},
        {90, S::SizeUp, 100},   {100, S::SizeUp, -1},
    };
    for (const Step& step : steps) {
        Layout m = l;
        m.columns[0].widgets[1].cameraSize = step.from;
        m.columns[0].widgets[1].rotation = 3;
        m.columns[0].widgets[1].cameraFps = 30;
        PcEditor ed;
        attach(ed, m);
        ed.select({0, 1});
        Layout edited;
        const bool queued = press(ed, step.button, edited);
        if (step.to < 0) {
            check(!queued, "Size stops at its end");
        } else {
            check(queued, "Size press queues a commit");
            if (!queued) continue;
            const WidgetInstance& w = edited.columns[0].widgets[1];
            check(w.cameraSize == step.to, "Size steps to the next value");
            check(w.type == WidgetType::HttpCamera && w.rotation == 3 && w.cameraFps == 30 &&
                      w.url == kDefaultHttpCameraUrl,
                  "Size leaves the other settings alone");
            check(edited.columns[0].widgets[0] == m.columns[0].widgets[0], "Size leaves the other widget alone");
        }
    }
}

// R3: the four cam toggles are one-part pills (PC: 2 x 2)
void testCameraPills() {
    using S = PcEditor::Setting;
    const S cams[4] = {S::Camera0, S::Camera1, S::Camera2, S::Camera3};
    Layout l;
    l.version = 1;
    l.columns.resize(1);
    l.columns[0].widgets = {make(WidgetType::HeadsetCamera)};  // cameras = 0x01
    PcEditor e;
    attach(e, l);
    e.select({0, 0});
    Box r[4], fps, full;
    for (const auto& s : e.settingButtons()) {
        for (int c = 0; c < 4; ++c) {
            if (s.first == cams[c]) r[c] = s.second;
        }
        if (s.first == S::FpsDown) fps = s.second;
        if (s.first == S::FullSizeOn) full = s.second;
    }
    double chipsBottom = 0;
    for (const auto& c : e.chips()) chipsBottom = std::max(chipsBottom, c.second.y + c.second.h);
    const Box card = {5000, 0, 400, 3000};
    for (int c = 0; c < 4; ++c) {
        check(r[c].w > 0, "cam chip exists");
        check(r[c].x >= card.x && r[c].x + r[c].w <= card.x + card.w && r[c].y >= chipsBottom &&
                  r[c].y + r[c].h <= card.y + card.h,
              "cam chip inside the palette card, below the chips");
        for (int d = c + 1; d < 4; ++d) {
            const bool apart = r[c].x + r[c].w <= r[d].x || r[d].x + r[d].w <= r[c].x || r[c].y + r[c].h <= r[d].y ||
                               r[d].y + r[d].h <= r[c].y;
            check(apart, "cam chips do not overlap");
        }
        check(r[c].y + r[c].h <= fps.y, "FPS row is below the cams");
    }
    check(r[0].y == r[1].y && r[2].y == r[3].y && r[2].y > r[0].y && r[1].x > r[0].x && r[3].x > r[2].x &&
              r[0].x == r[2].x,
          "cams are a 2 x 2 grid");
    check(full.y >= fps.y + fps.h, "Full size is below FPS");

    // a tap on a chip toggles exactly its bit; the only cam on cannot be turned off
    const uint8_t expect[4] = {0, 0x03, 0x05, 0x09};
    for (int c = 0; c < 4; ++c) {
        PcEditor ed;
        attach(ed, l);
        ed.select({0, 0});
        Layout edited;
        if (c == 0) {
            check(!press(ed, cams[0], edited), "the last cam cannot be turned off");
        } else {
            check(press(ed, cams[c], edited) && edited.columns[0].widgets[0].cameras == expect[c],
                  "cam chip toggles its bit");
        }
    }
    Layout two = l;
    two.columns[0].widgets[0].cameras = 0x05;
    for (const int c : {0, 2}) {
        PcEditor ed;
        attach(ed, two);
        ed.select({0, 0});
        Layout edited;
        check(press(ed, cams[c], edited) && edited.columns[0].widgets[0].cameras == (c == 0 ? 0x04 : 0x01),
              "turning one of two cams off");
    }
}

// R3: in a short client (1100 x 600 DIP) the palette is 340 x 510 below the tab bar. Every chip and every setting
// of every widget type must sit fully inside it, whatever the DPI scale.
// The app's numbers (pc/app_window.cpp): areaY = bar 30 + tabs 48 + pad 4 = 82, paletteH = h - areaY - 8, x = w - 8 - 340.
// (48 is PcPages::tabBarHeight at scale 1; hardcoded here so the test doesn't need the window.)
Box shortClientPalette(double s) {
    const double w = 1100 * s, h = 600 * s, areaY = 82 * s;
    return {w - 8 * s - 340 * s, areaY, 340 * s, h - areaY - 8 * s};
}

PcEditor::View shortClientView(double s) {
    PcEditor::View v;
    v.paletteCard = shortClientPalette(s);
    v.layoutCard = {8 * s, 82 * s, v.paletteCard.x - 16 * s, v.paletteCard.h};
    const double header = PcEditor::headerHeight(s);
    v.viewport = {v.layoutCard.x + 8 * s, v.layoutCard.y + header, v.layoutCard.w - 16 * s, v.layoutCard.h - header - 8 * s};
    v.originX = v.viewport.x;
    v.originY = v.viewport.y;
    v.ui = s;
    return v;
}

void testSettingsFitShortClient() {
    const WidgetType types[] = {WidgetType::Frame,    WidgetType::Cpu,         WidgetType::GpuRam,
                                WidgetType::Power,    WidgetType::Battery,     WidgetType::Temp,
                                WidgetType::CpuItems, WidgetType::RamItems,    WidgetType::Network,
                                WidgetType::Streaming, WidgetType::General,    WidgetType::HeadsetCamera,
                                WidgetType::EyeCamera};
    for (const double s : {1.0, 1.25, 1.5, 2.0}) {
        double worst = 1e9;
        for (const WidgetType type : types) {
            Layout l;
            l.version = 1;
            l.columns.resize(1);
            l.columns[0].widgets = {make(type)};
            PcEditor e;
            e.sync(l, layoutGrid(l));
            e.setView(shortClientView(s));
            e.select({0, 0});
            const Box card = shortClientPalette(s);
            const double bottom = card.y + card.h;
            bool inside = true;
            auto account = [&](const Box& r) {
                inside = inside && r.x >= card.x && r.x + r.w <= card.x + card.w && r.y >= card.y &&
                         r.y + r.h <= bottom - 4 * s;
                worst = std::min(worst, bottom - (r.y + r.h));
            };
            for (const auto& c : e.chips()) account(c.second);
            for (const auto& b : e.settingButtons()) account(b.second);
            check(inside, "chips and settings fit the palette of a 1100 x 600 client (4 * scale margin)");
        }
        std::printf("  short client, scale %.2f: worst bottom margin %.1f px (%.1f DIP)\n", s, worst, worst / s);
    }
}

#ifdef FPH_FONT_DIR
// R3: no palette chip label may shrink to fitSize's minimum (same formula as PcEditor::drawOverPanel, at scale 1)
void testChipLabelSizes() {
    FontSet fonts;
    fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* cr = cairo_create(surface);
    const Pen pen {cr, &fonts};
    const UiText& text = uiText(Language::En);
    const Layout l = base(1);
    PcEditor e;
    attach(e, l);
    for (const auto& chip : e.chips()) {
        const WidgetType type = chip.first;
        std::string label;
        if (type == WidgetType::Network) label = text.paletteNetwork;
        else if (type == WidgetType::CpuItems) label = text.paletteCpuItems;
        else if (type == WidgetType::RamItems) label = text.paletteRamItems;
        else if (type == WidgetType::HeadsetCamera) label = text.paletteHeadsetCamera;
        else if (type == WidgetType::EyeCamera) label = text.paletteEyeCamera;
        else {
            WidgetInstance w;
            w.type = type;
            label = widgetTitle(w);
        }
        if (!isRepeatable(type)) {
        check(fitSize(pen, label, 13.5, 9, chip.second.w - 16 - 16, true) > 9, "present chip label stays above the minimum size");
        }
        check(fitSize(pen, label, 13.5, 9, chip.second.w - 16, false) > 9, "chip label stays above the minimum size");
        if (isRepeatable(type)) {
            check(fitSize(pen, label + " \u00d72", 13.5, 9, chip.second.w - 16, false) > 9,
                  "repeatable chip label with a count stays above the minimum size");
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
#endif

#ifdef FPH_FONT_DIR
// Screenshots, only when FPH_PNG_DIR is set (the PC app can't be launched twice, so this is how they're seen).
void writeScreens(const char* dir) {
    FontSet fonts;
    fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
    const std::string out = dir;

    for (const bool compact : {false, true}) {
        Layout l;
        l.columns.resize(2);
        WidgetInstance frame = make(WidgetType::Frame), power = make(WidgetType::Power),
                       battery = make(WidgetType::Battery), temp = make(WidgetType::Temp);
        frame.compact = power.compact = battery.compact = temp.compact = compact;
        l.columns[0].widgets = {frame, power};
        l.columns[1].widgets = {temp, battery};
        GridRenderer renderer(fonts);
        renderer.setLayout(l);
        for (int i = 0; i < 12; ++i) {
            const double now = 100.0 + i;
            const double wob = 1.0 + 0.15 * std::sin(i * 0.9);
            MetricStore s;
            s.setF32(metric::kFrameAppFps, 72 * wob, now);
            s.setF32(metric::kFrameDisplayHz, 90, now);
            s.setF32(metric::kFrameCpuMs, 5 * wob, now);
            s.setF32(metric::kFrameGpuMs, 7 * wob, now);
            s.setF32(metric::kFrameCpuMaxMs, 9 * wob + (i == 7 ? 8 : 0), now);
            s.setF32(metric::kFrameGpuMaxMs, 11 * wob, now);
            s.setF32(metric::kPowerVphW, 9 * wob, now);
            s.setF32(metric::kTempCpuC, 52 * wob, now);
            s.setF32(metric::kTempGpuC, 55 * wob, now);
            s.setF32(metric::kBatteryPct, 64, now);
            WidgetContext ctx;
            ctx.store = &s;
            ctx.now = now;
            renderer.render(ctx);
        }
        renderer.writePng(out + (compact ? "/pc_compact.png" : "/pc_full.png"));
    }

    // the palette with a selection: Frame (Compact row), CPU usage items (Rows / One row per name / Hide system)
    // and the Headset camera card (cams 0 and 2 on, Full size on)
    for (const int variant : {0, 1, 2}) {
        Layout l = base(1);
        if (variant == 2) {
            l.columns[0].widgets.push_back(make(WidgetType::HeadsetCamera));
            l.columns[0].widgets.back().cameras = 0x05;
            l.columns[0].widgets.back().cameraFullSize = true;
        }
        PcEditor e;
        e.sync(l, layoutGrid(l));
        PcEditor::View v;
        v.viewport = {8, 48, 540, 700};
        v.layoutCard = {0, 0, 556, 760};
        v.paletteCard = {570, 0, PcEditor::paletteWidth(1.0), 760};
        v.originX = 20;
        v.originY = 56;
        v.ui = 1.0;
        e.setView(v);
        e.select(variant == 0 ? WidgetRef{0, 0} : variant == 1 ? WidgetRef{0, 1} : WidgetRef{0, 2});
        const int w = static_cast<int>(v.paletteCard.x + v.paletteCard.w) + 10;
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, 770);
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 0.07, 0.07, 0.08);
        cairo_paint(cr);
        const Pen pen {cr, &fonts};
        e.drawUnderPanel(pen);
        GridRenderer renderer(fonts);
        renderer.setLayout(l);
        MetricStore s;
        WidgetContext ctx;
        ctx.store = &s;
        ctx.now = 100;
        renderer.render(ctx);
        cairo_surface_t* panel = cairo_image_surface_create_for_data(
            const_cast<uint8_t*>(renderer.pixels()), CAIRO_FORMAT_ARGB32, renderer.geometry().textureWidth,
            renderer.geometry().textureHeight, renderer.geometry().textureWidth * 4);
        cairo_set_source_surface(cr, panel, v.originX, v.originY);
        cairo_paint(cr);
        cairo_surface_destroy(panel);
        e.drawOverPanel(pen);
        cairo_surface_write_to_png(surface, (out + (variant == 0   ? "/pc_editor_frame.png"
                                     : variant == 1 ? "/pc_editor_cpu_items.png"
                                                    : "/pc_editor_headset_camera.png")).c_str());
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }

    // the whole 1100 x 600 client with the real palette rect: Headset camera selected, cams 0 and 2 on, Full size on
    {
        Layout l = base(1);
        l.columns[0].widgets.push_back(make(WidgetType::HeadsetCamera));
        l.columns[0].widgets.back().cameras = 0x05;
        l.columns[0].widgets.back().cameraFullSize = true;
        PcEditor e;
        e.sync(l, layoutGrid(l));
        e.setView(shortClientView(1.0));
        e.select({0, 2});
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1100, 600);
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 0.07, 0.07, 0.08);
        cairo_paint(cr);
        const Pen pen {cr, &fonts};
        e.drawUnderPanel(pen);
        GridRenderer renderer(fonts);
        renderer.setLayout(l);
        MetricStore s;
        WidgetContext ctx;
        ctx.store = &s;
        ctx.now = 100;
        renderer.render(ctx);
        cairo_surface_t* panel = cairo_image_surface_create_for_data(
            const_cast<uint8_t*>(renderer.pixels()), CAIRO_FORMAT_ARGB32, renderer.geometry().textureWidth,
            renderer.geometry().textureHeight, renderer.geometry().textureWidth * 4);
        const PcEditor::View v = shortClientView(1.0);
        cairo_save(cr);
        cairo_rectangle(cr, v.viewport.x, v.viewport.y, v.viewport.w, v.viewport.h);
        cairo_clip(cr);
        cairo_set_source_surface(cr, panel, v.originX, v.originY);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(panel);
        e.drawOverPanel(pen);
        cairo_surface_write_to_png(surface, (out + "/pc_editor_headset_camera_600.png").c_str());
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }

    // HTTP camera selected in the 1100 x 600 client: Size 40 / 100 %, rotation 0 / 270
    for (const uint8_t size : {uint8_t(40), uint8_t(100)}) {
        for (const uint8_t turns : {uint8_t(0), uint8_t(3)}) {
            Layout l = base(1);
            l.columns[0].widgets.insert(l.columns[0].widgets.begin(), make(WidgetType::HttpCamera));
            l.columns[0].widgets.front().cameraSize = size;
            l.columns[0].widgets.front().rotation = turns;
            PcEditor e;
            e.sync(l, layoutGrid(l));
            const PcEditor::View v = shortClientView(1.0);
            e.setView(v);
            e.select({0, 0});
            cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1100, 600);
            cairo_t* cr = cairo_create(surface);
            cairo_set_source_rgb(cr, 0.07, 0.07, 0.08);
            cairo_paint(cr);
            const Pen pen {cr, &fonts};
            e.drawUnderPanel(pen);
            GridRenderer renderer(fonts);
            renderer.setLayout(l);
            MetricStore s;
            WidgetContext ctx;
            ctx.store = &s;
            ctx.now = 100;
            renderer.render(ctx);
            cairo_surface_t* panel = cairo_image_surface_create_for_data(
                const_cast<uint8_t*>(renderer.pixels()), CAIRO_FORMAT_ARGB32, renderer.geometry().textureWidth,
                renderer.geometry().textureHeight, renderer.geometry().textureWidth * 4);
            cairo_save(cr);
            cairo_rectangle(cr, v.viewport.x, v.viewport.y, v.viewport.w, v.viewport.h);
            cairo_clip(cr);
            cairo_set_source_surface(cr, panel, v.originX, v.originY);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(panel);
            e.drawOverPanel(pen);
            const std::string name =
                "/pc_editor_http_camera_" + std::to_string(size) + "_" + std::to_string(turns * 90) + ".png";
            cairo_surface_write_to_png(surface, (out + name).c_str());
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }
    }
}
#endif

}  // namespace

int main() {
    testCompactAndHideSystemButtons();
    testPills();
    testCameraPills();
    testHttpCameraSize();
    testSettingsFitShortClient();
#ifdef FPH_FONT_DIR
    testChipLabelSizes();
    if (const char* dir = std::getenv("FPH_PNG_DIR"); dir != nullptr && dir[0] != '\0') writeScreens(dir);
#endif
    testSelectionFollowsInstance();
    testOwnEditKeepsSelection();
    testSyncCancelsHeldDrag();
    if (failures == 0) std::printf("pc-editor: all passed\n");
    return failures == 0 ? 0 : 1;
}
