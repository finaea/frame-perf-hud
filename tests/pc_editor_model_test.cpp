// pc/editor_model: hit testing, drop targets and applying drops over layoutGrid() geometry.
#include "editor_model.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;
using namespace fph::pc;

DragSource card(int column, int index) {
    DragSource s;
    s.kind = DragSource::Kind::Card;
    s.from.column = column;
    s.from.index = index;
    return s;
}

}  // namespace

int main() {
    const Layout layout = defaultLayout();
    const GridGeometry g = layoutGrid(layout);
    CHECK(g.columns == static_cast<int>(layout.columns.size()));
    CHECK(!g.slots.empty());

    // slotAt: the centre of every card finds it; outside the panel finds nothing
    for (size_t i = 0; i < g.slots.size(); ++i) {
        const WidgetSlot& s = g.slots[i];
        CHECK(slotAt(g, s.x + s.w / 2.0, s.y + s.h / 2.0) == static_cast<int>(i));
        const WidgetRef ref = refOfSlot(layout, g, static_cast<int>(i));
        CHECK(ref.valid() && ref.column == s.column);
        const WidgetInstance* w = widgetAt(layout, ref);
        CHECK(w != nullptr && w->type == s.widget.type);
    }
    CHECK(slotAt(g, -5, -5) == -1);
    CHECK(slotAt(g, g.width + 10.0, 10) == -1);
    CHECK(!refOfSlot(layout, g, -1).valid());
    CHECK(!refOfSlot(layout, g, 9999).valid());

    // Nothing dragged: no target
    CHECK(dropTargetAt(layout, g, 10, 10, false, DragSource()).kind == DropTarget::Kind::None);

    // A card over the palette is removed; a palette item over the palette is nothing
    const DropTarget rm = dropTargetAt(layout, g, 10, 10, true, card(0, 0));
    CHECK(rm.kind == DropTarget::Kind::Remove);
    Layout l = layout;
    const size_t widgetsBefore = enabledWidgets(l).size();
    CHECK(applyDrop(l, card(0, 0), rm));
    CHECK(enabledWidgets(l).size() == widgetsBefore - 1);
    DragSource pal;
    pal.kind = DragSource::Kind::Palette;
    pal.type = WidgetType::Frame;
    CHECK(dropTargetAt(layout, g, 10, 10, true, pal).kind == DropTarget::Kind::None);
    CHECK(!applyDrop(l, pal, DropTarget()));

    // Negative pointer: none
    CHECK(dropTargetAt(layout, g, -3, 10, false, card(0, 0)).kind == DropTarget::Kind::None);

    // Dropping a card above its own slot (or just below it) changes nothing
    {
        Layout m = layout;
        const DropTarget t = dropTargetAt(layout, g, 10, 1, false, card(0, 0));
        CHECK(t.kind == DropTarget::Kind::Grid && t.column == 0 && t.index == 0);
        CHECK(!applyDrop(m, card(0, 0), t));
        CHECK(m.columns == layout.columns);
    }

    // Right of the panel: a new column on the right
    {
        Layout m = layout;
        const DropTarget t = dropTargetAt(layout, g, g.width + 20.0, 30, false, card(1, 0));
        CHECK(t.kind == DropTarget::Kind::Grid);
        CHECK(t.column == static_cast<int>(layout.columns.size()));
        CHECK(applyDrop(m, card(1, 0), t));
        const WidgetInstance* moved = widgetAt(layout, WidgetRef {1, 0});
        CHECK(moved != nullptr && m.columns.back().widgets.size() == 1 && m.columns.back().widgets[0] == *moved);
        CHECK(enabledWidgets(m).size() == enabledWidgets(layout).size());
    }

    // Moving a card to the top of another column
    {
        Layout m = layout;
        const DropTarget t = dropTargetAt(layout, g, kColumnWidth + 10.0, 1, false, card(0, 0));
        CHECK(t.kind == DropTarget::Kind::Grid && t.column == 1 && t.index == 0);
        const WidgetInstance moved = layout.columns[0].widgets[0];
        CHECK(applyDrop(m, card(0, 0), t));
        CHECK(m.columns[1].widgets[0] == moved);
        CHECK(m.columns[1].widgets.size() == layout.columns[1].widgets.size() + 1);
        CHECK(m.columns[0].widgets.size() == layout.columns[0].widgets.size() - 1);
    }

    // Palette: something not in the layout is inserted at the drop slot
    {
        Layout m = layout;
        for (WidgetType type : paletteTypes()) {
            if (!canAdd(layout, type)) continue;
            DragSource src;
            src.kind = DragSource::Kind::Palette;
            src.type = type;
            const DropTarget t = dropTargetAt(layout, g, 10, 1, false, src);
            CHECK(t.kind == DropTarget::Kind::Grid && t.column == 0 && t.index == 0);
            CHECK(applyDrop(m, src, t));
            CHECK(m.columns[0].widgets[0].type == type);
            CHECK(enabledWidgets(m).size() == enabledWidgets(layout).size() + 1);
            break;
        }
    }
    // Palette: a type that's already there (and isn't repeatable) is refused at the target
    {
        WidgetType single = WidgetType::None;
        for (const LayoutColumn& c : layout.columns) {
            for (const WidgetInstance& w : c.widgets) {
                if (!isRepeatable(w.type)) single = w.type;
            }
        }
        if (single != WidgetType::None) {
            DragSource src;
            src.kind = DragSource::Kind::Palette;
            src.type = single;
            CHECK(dropTargetAt(layout, g, 10, 1, false, src).kind == DropTarget::Kind::None);
        }
    }

    // Marker y: above the first card, between cards, below the last
    {
        DropTarget t;
        t.kind = DropTarget::Kind::Grid;
        t.column = 0;
        t.index = 0;
        std::vector<const WidgetSlot*> col;
        for (const WidgetSlot& s : g.slots) {
            if (s.column == 0) col.push_back(&s);
        }
        CHECK(col.size() >= 2);
        CHECK(dropMarkerY(g, t) < col.front()->y);
        t.index = 1;
        const double mid = dropMarkerY(g, t);
        CHECK(mid >= col[0]->y + col[0]->h && mid <= col[1]->y);
        t.index = static_cast<int>(col.size());
        CHECK(dropMarkerY(g, t) > col.back()->y + col.back()->h);
    }

    // The palette lists only offered types
    for (WidgetType t : paletteTypes()) CHECK(isOfferedInPalette(t));
    CHECK(!paletteTypes().empty());

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("pc editor model: all checks passed\n");
    return 0;
}
