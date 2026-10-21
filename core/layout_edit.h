// Editing a Layout (move / insert / remove widgets, the drop marker's position). Pure, no drawing: the
// dashboard editor uses it.
// Every edit keeps the layout's limits (kMaxColumns, kMaxWidgetsPerColumn), drops empty columns, and leaves
// Layout::version alone (LayoutAuthority::trySetLayout sets it).
#pragma once

#include "layout.h"

#include <functional>
#include <vector>

namespace fph {

/** Where one widget instance is: column, then position from the top. */
struct WidgetRef {
    int column = -1;
    int index = -1;

    bool valid() const { return column >= 0 && index >= 0; }
    bool operator==(const WidgetRef& o) const { return column == o.column && index == o.index; }
    bool operator!=(const WidgetRef& o) const { return !(*this == o); }
};

/** @return true for the types a layout may hold more than once (none for now) */
bool isRepeatable(WidgetType type);

/** @return true if the type can be added from the palette (every type but None and the reserved numbers) */
bool isOfferedInPalette(WidgetType type);

/** @return how many instances of the type the layout holds */
int countType(const Layout& layout, WidgetType type);

/** @return the widget at ref, or nullptr if ref is out of range */
const WidgetInstance* widgetAt(const Layout& layout, WidgetRef ref);

/** @return a new instance of the type with default settings */
WidgetInstance newInstance(WidgetType type, const Layout& layout);

/**
 * Whether the palette may add this type: offered, not already there (unless repeatable), and some column has room
 * (or a new column can be made).
 */
bool canAdd(const Layout& layout, WidgetType type);

/**
 * Inserts a widget. toColumn == columns.size() makes a new column on the right.
 * @param layout the layout (unchanged on failure)
 * @param widget the instance
 * @param toColumn target column
 * @param toIndex position in that column (clamped to its end)
 * @return false if the target is out of range or full, or a non-repeatable type is already there
 */
bool insertWidget(Layout& layout, const WidgetInstance& widget, int toColumn, int toIndex);

/**
 * Moves a widget. toIndex counts positions as the column looks BEFORE the move (the drop marker's slot), so
 * dropping a widget on its own slot or just below itself changes nothing. toColumn == columns.size() makes a new
 * column. Empty columns left behind are removed.
 * @return true if the layout changed
 */
bool moveWidget(Layout& layout, WidgetRef from, int toColumn, int toIndex);

/** Removes a widget (and its column if it becomes empty). @return false if ref is out of range */
bool removeWidget(Layout& layout, WidgetRef ref);

/** Removes empty columns. @return true if any was removed */
bool pruneEmptyColumns(Layout& layout);

/**
 * The column a palette tap appends to: the one with the least total height (ties: leftmost) that has room; a new
 * column if all are full and there's room for one.
 * @param layout the layout
 * @param height a widget's height (widgetHeight on the headset)
 * @return the column index (columns.size() = a new one), or -1 if nothing has room
 */
int shortestColumn(const Layout& layout, const std::function<int(const WidgetInstance&)>& height);

/**
 * The drop marker's slot in one column, from the pointer's y: the number of cards whose vertical centre is above y.
 * @param centers the cards' vertical centres, top to bottom
 * @param y the pointer
 * @return 0..centers.size()
 */
int dropSlot(const std::vector<double>& centers, double y);

}  // namespace fph
