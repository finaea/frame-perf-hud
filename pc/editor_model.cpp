#include "editor_model.h"

#include "widgets.h"

#include <cmath>

namespace fph {
namespace pc {

int slotAt(const GridGeometry& geometry, double px, double py) {
    for (size_t i = 0; i < geometry.slots.size(); ++i) {
        const WidgetSlot& s = geometry.slots[i];
        if (px >= s.x && px < s.x + s.w && py >= s.y && py < s.y + s.h) return static_cast<int>(i);
    }
    return -1;
}

namespace {

/** @return the layout index of the k-th card drawn in a column (widgets of height 0 get no card); size at the end */
int layoutIndexOfCard(const Layout& layout, int column, int k) {
    if (column < 0 || column >= static_cast<int>(layout.columns.size())) return 0;
    const std::vector<WidgetInstance>& widgets = layout.columns[static_cast<size_t>(column)].widgets;
    int seen = 0;
    for (size_t i = 0; i < widgets.size(); ++i) {
        if (widgetHeight(widgets[i]) <= 0) continue;
        if (seen == k) return static_cast<int>(i);
        ++seen;
    }
    return static_cast<int>(widgets.size());
}

}  // namespace

WidgetRef refOfSlot(const Layout& layout, const GridGeometry& geometry, int slot) {
    WidgetRef ref;
    if (slot < 0 || slot >= static_cast<int>(geometry.slots.size())) return ref;
    const int column = geometry.slots[static_cast<size_t>(slot)].column;
    int k = 0;
    for (int i = 0; i < slot; ++i) {
        if (geometry.slots[static_cast<size_t>(i)].column == column) ++k;
    }
    ref.column = column;
    ref.index = layoutIndexOfCard(layout, column, k);
    return ref;
}

namespace {

std::vector<const WidgetSlot*> slotsOfColumn(const GridGeometry& geometry, int column) {
    std::vector<const WidgetSlot*> out;
    for (const WidgetSlot& s : geometry.slots) {
        if (s.column == column) out.push_back(&s);
    }
    return out;
}

}  // namespace

DropTarget dropTargetAt(const Layout& layout, const GridGeometry& geometry, double px, double py, bool overPalette,
                        const DragSource& source) {
    DropTarget t;
    if (source.kind == DragSource::Kind::None) return t;
    if (overPalette) {
        if (source.kind == DragSource::Kind::Card) t.kind = DropTarget::Kind::Remove;
        return t;
    }
    if (px < 0 || py < 0) return t;
    const int columns = static_cast<int>(layout.columns.size());
    int column = static_cast<int>(std::floor(px / kColumnWidth));
    if (column >= columns) {
        // Right of the panel: a new column, if there's room for one. Dragging a column's only card there would
        // just move the column, so treat that as its own last slot.
        if (columns >= static_cast<int>(kMaxColumns)) return t;
        column = columns;
    }
    t.kind = DropTarget::Kind::Grid;
    t.column = column;
    if (column == columns) {
        t.index = 0;
    } else {
        std::vector<double> centers;
        for (const WidgetSlot* s : slotsOfColumn(geometry, column)) centers.push_back(s->y + s->h / 2.0);
        t.index = layoutIndexOfCard(layout, column, dropSlot(centers, py));
        const bool full = layout.columns[static_cast<size_t>(column)].widgets.size() >= kMaxWidgetsPerColumn;
        const bool sameColumn = source.kind == DragSource::Kind::Card && source.from.column == column;
        if (full && !sameColumn) return DropTarget();
    }
    if (source.kind == DragSource::Kind::Palette) {
        Layout probe = layout;
        if (!insertWidget(probe, newInstance(source.type, layout), t.column, t.index)) return DropTarget();
    }
    return t;
}

bool applyDrop(Layout& layout, const DragSource& source, const DropTarget& target) {
    switch (target.kind) {
        case DropTarget::Kind::None: return false;
        case DropTarget::Kind::Remove:
            return source.kind == DragSource::Kind::Card && removeWidget(layout, source.from);
        case DropTarget::Kind::Grid:
            if (source.kind == DragSource::Kind::Card) return moveWidget(layout, source.from, target.column, target.index);
            if (source.kind == DragSource::Kind::Palette) {
                return insertWidget(layout, newInstance(source.type, layout), target.column, target.index);
            }
            return false;
    }
    return false;
}

double dropMarkerY(const GridGeometry& geometry, const DropTarget& target) {
    const std::vector<const WidgetSlot*> cards = slotsOfColumn(geometry, target.column);
    if (cards.empty()) return 12.0;
    if (target.index <= 0) return cards.front()->y - 6.0;
    if (target.index >= static_cast<int>(cards.size())) return cards.back()->y + cards.back()->h + 6.0;
    const WidgetSlot* above = cards[static_cast<size_t>(target.index - 1)];
    const WidgetSlot* below = cards[static_cast<size_t>(target.index)];
    return (above->y + above->h + below->y) / 2.0;
}

std::vector<WidgetType> paletteTypes() {
    std::vector<WidgetType> out;
    for (size_t i = 1; i < kWidgetTypeCount; ++i) {
        const WidgetType t = static_cast<WidgetType>(i);
        if (isOfferedInPalette(t)) out.push_back(t);
    }
    return out;
}

}  // namespace pc
}  // namespace fph
