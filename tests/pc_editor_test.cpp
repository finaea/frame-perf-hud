// PcEditor (the PC's Ctrl+E editor): a layout sync during a held drag cancels it and reports it, and the selection
// follows the exact widget instance across syncs (review fixes, ticket 07).
#include "pc_editor.h"

#include "editor_model.h"
#include "grid_renderer.h"
#include "layout.h"

#include <cstdio>

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

}  // namespace

int main() {
    testSelectionFollowsInstance();
    testOwnEditKeepsSelection();
    testSyncCancelsHeldDrag();
    if (failures == 0) std::printf("pc-editor: all passed\n");
    return failures == 0 ? 0 : 1;
}
