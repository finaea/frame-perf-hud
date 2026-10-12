// The PC editor's logic (ticket 07): which card is under the mouse, where a drop lands, and the edited layout.
// Pure: works in unscaled panel px on core's GridGeometry, so tests run it without a window. The window draws the
// palette and the drop marker; every finished drop becomes one LAYOUT_SET (LinkClient::submitEdit).
#pragma once

#include "grid_renderer.h"
#include "layout.h"
#include "layout_edit.h"

#include <vector>

namespace fph {
namespace pc {

/** What is being dragged. */
struct DragSource {
    enum class Kind { None, Card, Palette } kind = Kind::None;
    WidgetRef from;                         ///< Card: where it was
    WidgetType type = WidgetType::None;     ///< Palette: what to add
};

/** Where a drop would land. */
struct DropTarget {
    enum class Kind { None, Grid, Remove } kind = Kind::None;
    int column = -1;  ///< Grid: target column (== columns: a new one on the right)
    int index = -1;   ///< Grid: slot in that column, as it looks before the move

    bool operator==(const DropTarget& o) const { return kind == o.kind && column == o.column && index == o.index; }
    bool operator!=(const DropTarget& o) const { return !(*this == o); }
};

/** @return the slot index (into geometry.slots) whose card contains the point, or -1 */
int slotAt(const GridGeometry& geometry, double px, double py);

/** @return where slot i sits in the layout (column + position from the top; zero-height widgets have no slot) */
WidgetRef refOfSlot(const Layout& layout, const GridGeometry& geometry, int slot);

/**
 * Where a drop at (px, py) would land.
 * @param layout the current layout
 * @param geometry its geometry
 * @param px, py the pointer in unscaled panel px (may be right of the panel: a new column)
 * @param overPalette the pointer is over the palette (a card dropped there is removed)
 * @param source what is dragged
 */
DropTarget dropTargetAt(const Layout& layout, const GridGeometry& geometry, double px, double py, bool overPalette,
                        const DragSource& source);

/**
 * Applies a drop.
 * @param layout edited in place (unchanged on failure)
 * @return true if the layout changed (send it)
 */
bool applyDrop(Layout& layout, const DragSource& source, const DropTarget& target);

/**
 * The y (unscaled panel px) of the drop marker for a Grid target: between two cards, or under the last.
 * @return the marker's y; x is column × kColumnWidth
 */
double dropMarkerY(const GridGeometry& geometry, const DropTarget& target);

/** @return the types the palette shows, in WidgetType order (only those isOfferedInPalette) */
std::vector<WidgetType> paletteTypes();

}  // namespace pc
}  // namespace fph
