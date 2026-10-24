// Layout edits.
#include "layout_edit.h"

#include <algorithm>

namespace frame_perf_overlay {

bool isRepeatable(WidgetType /*type*/) {
    return false;  // every widget shows one thing, so a second copy would only repeat it
}

bool isOfferedInPalette(WidgetType type) {
    return widgetKey(type) != nullptr;  // not None, not a reserved number
}

int countType(const Layout& layout, WidgetType type) {
    int n = 0;
    for (const LayoutColumn& column : layout.columns) {
        for (const WidgetInstance& w : column.widgets) n += w.type == type ? 1 : 0;
    }
    return n;
}

const WidgetInstance* widgetAt(const Layout& layout, WidgetRef ref) {
    if (ref.column < 0 || ref.column >= static_cast<int>(layout.columns.size())) return nullptr;
    const std::vector<WidgetInstance>& widgets = layout.columns[static_cast<size_t>(ref.column)].widgets;
    if (ref.index < 0 || ref.index >= static_cast<int>(widgets.size())) return nullptr;
    return &widgets[static_cast<size_t>(ref.index)];
}

WidgetInstance newInstance(WidgetType type, const Layout& /*layout*/) {
    WidgetInstance w;
    w.type = type;
    return w;
}

namespace {

bool hasRoomSomewhere(const Layout& layout) {
    if (layout.columns.size() < kMaxColumns) return true;
    for (const LayoutColumn& column : layout.columns) {
        if (column.widgets.size() < kMaxWidgetsPerColumn) return true;
    }
    return false;
}

}  // namespace

bool canAdd(const Layout& layout, WidgetType type) {
    if (!isOfferedInPalette(type)) return false;
    if (!isRepeatable(type) && countType(layout, type) > 0) return false;
    return hasRoomSomewhere(layout);
}

bool insertWidget(Layout& layout, const WidgetInstance& widget, int toColumn, int toIndex) {
    if (widget.type == WidgetType::None) return false;
    if (!isRepeatable(widget.type) && countType(layout, widget.type) > 0) return false;
    const int columns = static_cast<int>(layout.columns.size());
    if (toColumn < 0 || toColumn > columns) return false;
    if (toColumn < columns && layout.columns[static_cast<size_t>(toColumn)].widgets.size() >= kMaxWidgetsPerColumn) {
        return false;  // full: unchanged
    }
    if (toColumn == columns) {
        if (layout.columns.size() >= kMaxColumns) return false;
        layout.columns.emplace_back();
    }
    std::vector<WidgetInstance>& widgets = layout.columns[static_cast<size_t>(toColumn)].widgets;
    const int at = std::clamp(toIndex, 0, static_cast<int>(widgets.size()));
    widgets.insert(widgets.begin() + at, widget);
    pruneEmptyColumns(layout);
    return true;
}

bool moveWidget(Layout& layout, WidgetRef from, int toColumn, int toIndex) {
    const WidgetInstance* found = widgetAt(layout, from);
    if (found == nullptr) return false;
    const int columns = static_cast<int>(layout.columns.size());
    if (toColumn < 0 || toColumn > columns) return false;
    const WidgetInstance widget = *found;
    if (toColumn == from.column) {
        std::vector<WidgetInstance>& widgets = layout.columns[static_cast<size_t>(toColumn)].widgets;
        int at = std::clamp(toIndex, 0, static_cast<int>(widgets.size()));
        if (at == from.index || at == from.index + 1) return false;  // its own slot: no change
        if (at > from.index) --at;  // the slots below shift up once it's taken out
        widgets.erase(widgets.begin() + from.index);
        widgets.insert(widgets.begin() + at, widget);
        return true;
    }
    if (toColumn == columns) {
        // A new column for a widget that's alone in its column would just move the same column: no change
        if (layout.columns[static_cast<size_t>(from.column)].widgets.size() == 1 && from.column == columns - 1) {
            return false;
        }
        if (layout.columns.size() >= kMaxColumns) return false;
    } else if (layout.columns[static_cast<size_t>(toColumn)].widgets.size() >= kMaxWidgetsPerColumn) {
        return false;
    }
    Layout next = layout;
    std::vector<WidgetInstance>& source = next.columns[static_cast<size_t>(from.column)].widgets;
    source.erase(source.begin() + from.index);
    if (toColumn == columns) next.columns.emplace_back();
    std::vector<WidgetInstance>& target = next.columns[static_cast<size_t>(toColumn)].widgets;
    target.insert(target.begin() + std::clamp(toIndex, 0, static_cast<int>(target.size())), widget);
    pruneEmptyColumns(next);
    layout = std::move(next);
    return true;
}

bool removeWidget(Layout& layout, WidgetRef ref) {
    if (widgetAt(layout, ref) == nullptr) return false;
    std::vector<WidgetInstance>& widgets = layout.columns[static_cast<size_t>(ref.column)].widgets;
    widgets.erase(widgets.begin() + ref.index);
    pruneEmptyColumns(layout);
    return true;
}

bool pruneEmptyColumns(Layout& layout) {
    const size_t before = layout.columns.size();
    layout.columns.erase(std::remove_if(layout.columns.begin(), layout.columns.end(),
                                        [](const LayoutColumn& c) { return c.widgets.empty(); }),
                         layout.columns.end());
    return layout.columns.size() != before;
}

int shortestColumn(const Layout& layout, const std::function<int(const WidgetInstance&)>& height) {
    int best = -1;
    long bestHeight = 0;
    for (size_t c = 0; c < layout.columns.size(); ++c) {
        const LayoutColumn& column = layout.columns[c];
        if (column.widgets.size() >= kMaxWidgetsPerColumn) continue;
        long total = 0;
        for (const WidgetInstance& w : column.widgets) total += height(w);
        if (best < 0 || total < bestHeight) {
            best = static_cast<int>(c);
            bestHeight = total;
        }
    }
    if (best < 0 && layout.columns.size() < kMaxColumns) best = static_cast<int>(layout.columns.size());
    return best;
}

int dropSlot(const std::vector<double>& centers, double y) {
    int slot = 0;
    for (const double c : centers) {
        if (y > c) ++slot;
    }
    return slot;
}

}  // namespace frame_perf_overlay
