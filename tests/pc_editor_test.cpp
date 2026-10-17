// PcEditor (the PC's Ctrl+E editor): a layout sync during a held drag cancels it and reports it, and the selection
// follows the exact widget instance across syncs (review fixes, ticket 07).
#include "pc_editor.h"

#include "editor_model.h"
#include "grid_renderer.h"
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
                                 type == WidgetType::Battery || type == WidgetType::Temp;
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
    for (const bool frame : {true, false}) {
        const Layout l = base(1);
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
        e.select(frame ? WidgetRef{0, 0} : WidgetRef{0, 1});
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
        cairo_surface_write_to_png(surface, (out + (frame ? "/pc_editor_frame.png" : "/pc_editor_cpu_items.png")).c_str());
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
}
#endif

}  // namespace

int main() {
    testCompactAndHideSystemButtons();
#ifdef FPH_FONT_DIR
    if (const char* dir = std::getenv("FPH_PNG_DIR"); dir != nullptr && dir[0] != '\0') writeScreens(dir);
#endif
    testSelectionFollowsInstance();
    testOwnEditKeepsSelection();
    testSyncCancelsHeldDrag();
    if (failures == 0) std::printf("pc-editor: all passed\n");
    return failures == 0 ? 0 : 1;
}
