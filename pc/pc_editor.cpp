#include "pc_editor.h"

#include "camera_frame.h"  // T10: camera names
#include "editor_model.h"
#include "i18n.h"
#include "theme.h"
#include "widgets.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fph {
namespace pc {

namespace {

// Sizes are the headset editor's (layout_editor.cpp), in DIPs × the window's DPI scale
constexpr double kPad = 16;
constexpr double kHeaderH = 48;
constexpr double kLaneInsetPanel = 3;  // lane edge inside its 512 px column (the cards sit 8 px in)
constexpr int kChipColumns = 2;
// R3: chips 28 + 4 (were 32 + 8), so the camera card's 4 settings rows fit a 600 px window; 3 columns would
// shrink "CPU usage items" to unreadable
constexpr double kChipH = 28;
constexpr double kChipGap = 4;
constexpr double kRowH = 34;
constexpr double kRowGap = 8;
constexpr double kFpsStepperW = 130;  // T10: the camera widgets' FPS − n + (ui units)
constexpr double kFpsStep = 40;
constexpr double kEditW = 60;  // HTTP camera: Edit next to the address
constexpr double kPairGap = 10;  // HTTP camera: between the FPS and Size steppers sharing a row
constexpr double kPairStep = 36;  // ... and their − / + buttons
constexpr double kDragStart = 6;  // px the pointer moves before a press counts as a drag (headset: 14 for a laser)

const UiText& text() {
    return uiText(Language::En);
}

bool isList(WidgetType t) {
    return t == WidgetType::CpuItems || t == WidgetType::RamItems;
}

// Rows − / + steps: the headset's (layout_editor.cpp rowsDown / rowsUp)
uint32_t rowsDown(uint32_t v) {
    if (v <= 5) return std::max(1u, v - 1);
    return v % 5 == 0 ? v - 5 : v - v % 5;
}

uint32_t rowsUp(uint32_t v) {
    if (v < 5) return v + 1;
    return std::min(kMaxListItems, (v / 5 + 1) * 5);
}

std::string chipLabel(WidgetType type) {
    if (type == WidgetType::Network) return text().paletteNetwork;
    if (type == WidgetType::CpuItems) return text().paletteCpuItems;  // R3: short chip labels
    if (type == WidgetType::RamItems) return text().paletteRamItems;
    if (type == WidgetType::HeadsetCamera) return text().paletteHeadsetCamera;
    if (type == WidgetType::EyeCamera) return text().paletteEyeCamera;
    if (type == WidgetType::HttpCamera) return text().paletteHttpCamera;
    WidgetInstance w;
    w.type = type;
    return widgetTitle(w);
}

}  // namespace

double PcEditor::headerHeight(double ui) {
    return kHeaderH * ui;
}

double PcEditor::paletteWidth(double ui) {
    return 340 * ui;
}

bool PcEditor::sync(const Layout& layout, const GridGeometry& geometry) {
    if (layout.version == layout_.version && layout.sameWidgets(layout_) && sameGridGeometry(geometry, geometry_)) {
        return false;
    }
    const bool versionMoved = layout.version != layout_.version;
    layout_ = layout;
    geometry_ = geometry;
    // A press made on another version would apply its indices to the wrong layout: drop it (like the headset), and
    // tell the window, which says so (never a silent loss)
    if (versionMoved && press_ != Press::None) {
        cancel();
        pressCancelled_ = true;
    }
    revalidateSelection();
    relayout();
    return true;
}

void PcEditor::revalidateSelection() {
    if (!selected_.valid()) return;
    const WidgetInstance* w = widgetAt(layout_, selected_);
    if (w != nullptr && *w == selectedInstance_) return;
    // Moved (or something else moved into its slot): follow the exact instance; drop it if it's gone or ambiguous
    WidgetRef found;
    int matches = 0;
    for (size_t c = 0; c < layout_.columns.size(); ++c) {
        for (size_t k = 0; k < layout_.columns[c].widgets.size(); ++k) {
            if (layout_.columns[c].widgets[k] == selectedInstance_) {
                found = {static_cast<int>(c), static_cast<int>(k)};
                ++matches;
            }
        }
    }
    selected_ = matches == 1 ? found : WidgetRef();
}

bool PcEditor::takePressCancelled() {
    const bool was = pressCancelled_;
    pressCancelled_ = false;
    return was;
}

void PcEditor::setView(const View& view) {
    view_ = view;
    relayout();
}

void PcEditor::select(WidgetRef ref) {
    const WidgetInstance* w = widgetAt(layout_, ref);
    selected_ = w != nullptr ? ref : WidgetRef();
    selectedInstance_ = w != nullptr ? *w : WidgetInstance();
    relayout();
}

void PcEditor::relayout() {
    const double u = view_.ui;
    lanes_.clear();
    const int columns = static_cast<int>(layout_.columns.size());
    const int n = columns + (layout_.columns.size() < kMaxColumns ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        lanes_.push_back({view_.originX + (i * kColumnWidth + kLaneInsetPanel) * view_.scale, view_.viewport.y,
                          (kColumnWidth - 2 * kLaneInsetPanel) * view_.scale, view_.viewport.h});
    }
    // Palette: one chip per type, WidgetType order (the headset's order and labels)
    chips_.clear();
    const Box& card = view_.paletteCard;
    const double chipX = card.x + kPad * u;
    const double chipW = (card.w - 2 * kPad * u - (kChipColumns - 1) * kChipGap * u) / kChipColumns;
    const double chipY = card.y + kHeaderH * u;
    for (int t = 1; t < static_cast<int>(kWidgetTypeCount); ++t) {
        const int i = t - 1;
        chips_.push_back({static_cast<WidgetType>(t),
                          {chipX + (i % kChipColumns) * (chipW + kChipGap * u),
                           chipY + (i / kChipColumns) * (kChipH + kChipGap) * u, chipW, kChipH * u}});
    }
    // The selected widget's settings: [title ...... Remove] / row A / row B
    settings_.clear();
    const WidgetInstance* w = widgetAt(layout_, selected_);
    if (w == nullptr) return;
    const double sx = card.x + kPad * u;
    const double sw = card.w - 2 * kPad * u;
    const double sy = chips_.back().second.y + chips_.back().second.h + 20 * u;
    const double removeW = 96 * u;
    settings_.push_back({Setting::Remove, {sx + sw - removeW, sy, removeW, kRowH * u}});
    const double rowA = sy + (kRowH + kRowGap) * u;
    const double rowB = rowA + (kRowH + kRowGap) * u;
    const double controlW = 170 * u;
    const double controlX = sx + sw - controlW;
    if (isList(w->type)) {
        const double step = 44 * u;
        settings_.push_back({Setting::RowsDown, {controlX, rowA, step, kRowH * u}});
        settings_.push_back({Setting::RowsUp, {controlX + controlW - step, rowA, step, kRowH * u}});
        settings_.push_back({Setting::SameNameOn, {controlX, rowB, controlW / 2, kRowH * u}});
        settings_.push_back({Setting::SameNameOff, {controlX + controlW / 2, rowB, controlW / 2, kRowH * u}});
        const double rowC = rowB + (kRowH + kRowGap) * u;  // R2
        settings_.push_back({Setting::HideSystemOn, {controlX, rowC, controlW / 2, kRowH * u}});
        settings_.push_back({Setting::HideSystemOff, {controlX + controlW / 2, rowC, controlW / 2, kRowH * u}});
    } else if (hasCompactView(w->type)) {  // R2
        settings_.push_back({Setting::CompactOn, {controlX, rowA, controlW / 2, kRowH * u}});
        settings_.push_back({Setting::CompactOff, {controlX + controlW / 2, rowA, controlW / 2, kRowH * u}});
    } else if (w->type == WidgetType::Network) {
        // R3: the pill reaches 70 px further left (like the camera row) so "Home Wi-Fi" + ✓ fits its half
        const double aw = (controlW + 70 * u) / 2;
        settings_.push_back({Setting::AdapterDongle, {controlX - 70 * u, rowA, aw, kRowH * u}});
        settings_.push_back({Setting::AdapterHome, {controlX - 70 * u + aw, rowA, aw, kRowH * u}});
    } else if (w->type == WidgetType::HeadsetCamera) {  // T10: one toggle per outward cam; FPS − n +; Full res
        // R3: the cam toggles are one-part pills, 2 × 2 so "video13" + ✓ fits (the palette has height to spare);
        // FPS and Full res move down one row
        const double step = (kRowH + kRowGap) * u;
        const double cw = (controlW + 70 * u) / 2;
        for (int c = 0; c < 4; ++c)
            settings_.push_back({static_cast<Setting>(static_cast<int>(Setting::Camera0) + c),
                                 {controlX - 70 * u + (c % 2) * cw, rowA + (c / 2) * step, cw - 4 * u, kRowH * u}});
        const double rowFps = rowB + step;
        settings_.push_back({Setting::FpsDown, {controlX - 70 * u, rowFps, kFpsStep * u, kRowH * u}});
        settings_.push_back({Setting::FpsUp, {controlX - 70 * u + (kFpsStepperW - kFpsStep) * u, rowFps, kFpsStep * u, kRowH * u}});
        // R3: Full res is an On | Off pill on its own row (it shared row B with FPS as a single toggle button)
        const double rowC = rowFps + step;
        settings_.push_back({Setting::FullSizeOn, {controlX, rowC, controlW / 2, kRowH * u}});
        settings_.push_back({Setting::FullSizeOff, {controlX + controlW / 2, rowC, controlW / 2, kRowH * u}});
    } else if (w->type == WidgetType::EyeCamera) {  // T10: FPS − n +
        settings_.push_back({Setting::FpsDown, {controlX - 70 * u, rowA, kFpsStep * u, kRowH * u}});
        settings_.push_back({Setting::FpsUp, {controlX - 70 * u + (kFpsStepperW - kFpsStep) * u, rowA, kFpsStep * u, kRowH * u}});
    } else if (w->type == WidgetType::HttpCamera) {
        // [address ...... Edit] / − n fps + − Size n% + / Full res On | Off / Rotation − n° + (FPS and Size share a
        // row, units in the value, so it still fits a 600 px window like the Headset camera's 4 rows)
        const double step = (kRowH + kRowGap) * u;
        settings_.push_back({Setting::AddressEdit, {sx + sw - kEditW * u, rowA, kEditW * u, kRowH * u}});
        const double pairW = (sw - kPairGap * u) / 2;
        const double ps = kPairStep * u;
        settings_.push_back({Setting::FpsDown, {sx, rowB, ps, kRowH * u}});
        settings_.push_back({Setting::FpsUp, {sx + pairW - ps, rowB, ps, kRowH * u}});
        settings_.push_back({Setting::SizeDown, {sx + pairW + kPairGap * u, rowB, ps, kRowH * u}});
        settings_.push_back({Setting::SizeUp, {sx + sw - ps, rowB, ps, kRowH * u}});
        const double rowC = rowB + step;
        settings_.push_back({Setting::FullSizeOn, {controlX, rowC, controlW / 2, kRowH * u}});
        settings_.push_back({Setting::FullSizeOff, {controlX + controlW / 2, rowC, controlW / 2, kRowH * u}});
        const double rowD = rowC + step;
        settings_.push_back({Setting::RotationDown, {controlX - 70 * u, rowD, kFpsStep * u, kRowH * u}});
        settings_.push_back({Setting::RotationUp, {controlX - 70 * u + (kFpsStepperW - kFpsStep) * u, rowD, kFpsStep * u, kRowH * u}});
    }
}

Box PcEditor::slotBox(const WidgetSlot& s) const {
    return {view_.originX + s.x * view_.scale, view_.originY + s.y * view_.scale, s.w * view_.scale,
            s.h * view_.scale};
}

Box PcEditor::cardBox(WidgetRef ref) const {
    for (size_t i = 0; i < geometry_.slots.size(); ++i) {
        if (refOfSlot(layout_, geometry_, static_cast<int>(i)) == ref) return slotBox(geometry_.slots[i]);
    }
    return {};
}

int PcEditor::cardAt(double x, double y) const {
    if (!view_.viewport.contains(x, y)) return -1;
    for (size_t i = 0; i < geometry_.slots.size(); ++i) {
        if (slotBox(geometry_.slots[i]).contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

int PcEditor::chipAt(double x, double y) const {
    for (size_t i = 0; i < chips_.size(); ++i) {
        if (chips_[i].second.contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

int PcEditor::settingAt(double x, double y) const {
    for (size_t i = 0; i < settings_.size(); ++i) {
        if (settings_[i].second.contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

void PcEditor::updateDrop(double x, double y) {
    dropColumn_ = -1;
    overPalette_ = press_ == Press::Card && view_.paletteCard.contains(x, y);
    if (overPalette_ || !view_.viewport.contains(x, y)) return;
    DragSource source;
    if (press_ == Press::Card) {
        source.kind = DragSource::Kind::Card;
        source.from = pressedCard_;
    } else {
        source.kind = DragSource::Kind::Palette;
        source.type = pressedChip_;
    }
    const double px = (x - view_.originX) / view_.scale;
    const double py = (y - view_.originY) / view_.scale;
    const DropTarget t = dropTargetAt(layout_, geometry_, px, py, false, source);
    if (t.kind != DropTarget::Kind::Grid) return;
    // Only show a marker where the drop would change something (the headset's rule)
    Layout trial = layout_;
    if (!applyDrop(trial, source, t)) return;
    dropColumn_ = t.column;
    dropSlot_ = t.index;
}

void PcEditor::queueCommit(const Layout& edited, WidgetRef selectAfter) {
    commitQueued_ = true;
    commitBase_ = pressVersion_;
    commitLayout_ = edited;
    // The window shows the edit right away (LinkClient::submitEdit); select where it landed. A reject restores the
    // old layout and sync() drops a selection that no longer fits.
    const WidgetInstance* w = widgetAt(edited, selectAfter);
    selected_ = w != nullptr ? selectAfter : WidgetRef();
    selectedInstance_ = w != nullptr ? *w : WidgetInstance();
}

bool PcEditor::takeCommit(uint32_t& baseVersion, Layout& layout) {
    if (!commitQueued_) return false;
    commitQueued_ = false;
    baseVersion = commitBase_;
    layout = commitLayout_;
    return true;
}

void PcEditor::commitResult(CommitOutcome outcome) {
    status_ = static_cast<int>(outcome);
}

void PcEditor::applySetting(Setting setting) {
    const WidgetInstance* w = widgetAt(layout_, selected_);
    if (w == nullptr) return;
    Layout edited = layout_;
    if (setting == Setting::Remove) {
        removeWidget(edited, selected_);
        queueCommit(edited, {});
        return;
    }
    if (setting == Setting::AddressEdit) {  // the dialog's address comes back as an edit on this version
        addressEdit_ = {selected_, layout_.version, w->url};
        return;
    }
    WidgetInstance& e =
        edited.columns[static_cast<size_t>(selected_.column)].widgets[static_cast<size_t>(selected_.index)];
    switch (setting) {
        case Setting::RowsDown: e.maxItems = rowsDown(e.maxItems); break;
        case Setting::RowsUp: e.maxItems = rowsUp(e.maxItems); break;
        case Setting::SameNameOn: e.sumSameName = true; break;
        case Setting::SameNameOff: e.sumSameName = false; break;
        case Setting::AdapterDongle: e.adapter = NetAdapter::Dongle; break;
        case Setting::AdapterHome: e.adapter = NetAdapter::Home; break;
        case Setting::Camera0:
        case Setting::Camera1:
        case Setting::Camera2:
        case Setting::Camera3: {  // T10: toggles one cam; the last one can't be turned off (an empty card)
            const auto bit = static_cast<uint8_t>(1u << (static_cast<int>(setting) - static_cast<int>(Setting::Camera0)));
            const auto mask = static_cast<uint8_t>(e.cameras ^ bit);
            if (mask & 0x0F) e.cameras = mask;
            break;
        }
        case Setting::FullSizeOn: e.cameraFullSize = true; break;  // R3
        case Setting::FullSizeOff: e.cameraFullSize = false; break;
        case Setting::FpsDown: e.cameraFps = cameraFpsStep(e.cameraFps, -1, e.type); break;
        case Setting::FpsUp: e.cameraFps = cameraFpsStep(e.cameraFps, +1, e.type); break;
        case Setting::CompactOn: e.compact = true; break;  // R2
        case Setting::CompactOff: e.compact = false; break;
        case Setting::HideSystemOn: e.hideSystem = true; break;
        case Setting::HideSystemOff: e.hideSystem = false; break;
        case Setting::RotationDown: e.rotation = static_cast<uint8_t>((e.rotation + 3) & 3); break;  // HTTP camera
        case Setting::RotationUp: e.rotation = static_cast<uint8_t>((e.rotation + 1) & 3); break;
        case Setting::SizeDown: e.cameraSize = cameraSizeStep(e.cameraSize, -1); break;
        case Setting::SizeUp: e.cameraSize = cameraSizeStep(e.cameraSize, +1); break;
        case Setting::Remove:
        case Setting::AddressEdit: break;
    }
    if (e != *w) queueCommit(edited, selected_);
}

bool PcEditor::takeAddressEdit(AddressEdit& out) {
    if (!addressEdit_.valid()) return false;
    out = addressEdit_;
    addressEdit_ = {};
    return true;
}

bool PcEditor::cancel() {
    const bool was = press_ != Press::None;
    press_ = Press::None;
    dragging_ = false;
    dropColumn_ = -1;
    overPalette_ = false;
    return was;
}

bool PcEditor::pointerDown(double x, double y) {
    if (press_ != Press::None) cancel();
    pointerX_ = pressX_ = x;
    pointerY_ = pressY_ = y;
    pressVersion_ = layout_.version;
    status_ = -1;
    const int s = settingAt(x, y);
    if (s >= 0) {
        applySetting(settings_[static_cast<size_t>(s)].first);  // like the headset's buttons: acts on press
        return true;
    }
    const int c = chipAt(x, y);
    if (c >= 0) {
        const WidgetType type = chips_[static_cast<size_t>(c)].first;
        if (!isOfferedInPalette(type)) return false;
        press_ = Press::Chip;
        pressedChip_ = type;
        return true;
    }
    const int slot = cardAt(x, y);
    if (slot >= 0) {
        press_ = Press::Card;
        pressedCard_ = refOfSlot(layout_, geometry_, slot);
        return true;
    }
    if (view_.viewport.contains(x, y)) press_ = Press::Lanes;
    return false;
}

bool PcEditor::pointerMove(double x, double y) {
    pointerX_ = x;
    pointerY_ = y;
    if (press_ != Press::Card && press_ != Press::Chip) return false;
    if (!dragging_) {
        if (std::hypot(x - pressX_, y - pressY_) < kDragStart * view_.ui) return false;
        if (press_ == Press::Chip && !canAdd(layout_, pressedChip_)) return false;
        dragging_ = true;
    }
    updateDrop(x, y);
    return true;
}

bool PcEditor::pointerUp(double x, double y) {
    if (press_ == Press::None) return false;
    pointerX_ = x;
    pointerY_ = y;
    const Press press = press_;
    const bool dragged = dragging_;
    if (dragged) updateDrop(x, y);
    const int column = dropColumn_;
    const int slot = dropSlot_;
    const bool toPalette = overPalette_;
    cancel();

    if (press == Press::Card) {
        if (!dragged) {
            // Click: select (again = deselect)
            if (selected_ == pressedCard_) select({});
            else select(pressedCard_);
            return true;
        }
        Layout edited = layout_;
        if (toPalette) {
            if (removeWidget(edited, pressedCard_)) queueCommit(edited, {});
            return true;
        }
        if (column < 0 || !moveWidget(edited, pressedCard_, column, slot)) return true;
        // Where it landed: its source column may have been removed (it was alone there) before the target
        const int columns = static_cast<int>(layout_.columns.size());
        const bool sourceGone =
            layout_.columns[static_cast<size_t>(pressedCard_.column)].widgets.size() == 1 && column != pressedCard_.column;
        WidgetRef landed {column, slot};
        if (column == pressedCard_.column && slot > pressedCard_.index) landed.index = slot - 1;
        if (column == columns) landed.index = 0;
        if (sourceGone && pressedCard_.column < column) landed.column -= 1;
        queueCommit(edited, landed);
        return true;
    }
    if (press == Press::Chip) {
        Layout edited = layout_;
        const WidgetInstance widget = newInstance(pressedChip_, layout_);
        if (dragged) {
            if (column >= 0 && insertWidget(edited, widget, column, slot)) {
                const int size = static_cast<int>(edited.columns[static_cast<size_t>(column)].widgets.size());
                queueCommit(edited, {column, std::min(slot, size - 1)});
            }
            return true;
        }
        // Click on a type that's there and can't repeat: select it
        if (!isRepeatable(pressedChip_) && countType(layout_, pressedChip_) > 0) {
            for (size_t c = 0; c < layout_.columns.size(); ++c) {
                for (size_t k = 0; k < layout_.columns[c].widgets.size(); ++k) {
                    if (layout_.columns[c].widgets[k].type == pressedChip_) {
                        select({static_cast<int>(c), static_cast<int>(k)});
                        return true;
                    }
                }
            }
            return true;
        }
        if (!canAdd(layout_, pressedChip_)) return true;
        // Click to add: below the selected widget if its column has room, else at the end of the shortest column
        int target = -1;
        int at = 0;
        if (selected_.valid() &&
            layout_.columns[static_cast<size_t>(selected_.column)].widgets.size() < kMaxWidgetsPerColumn) {
            target = selected_.column;
            at = selected_.index + 1;
        } else {
            target = shortestColumn(layout_, [](const WidgetInstance& w) { return widgetHeight(w); });
            at = target >= 0 && target < static_cast<int>(layout_.columns.size())
                     ? static_cast<int>(layout_.columns[static_cast<size_t>(target)].widgets.size())
                     : 0;
        }
        if (target >= 0 && insertWidget(edited, widget, target, at)) queueCommit(edited, {target, at});
        return true;
    }
    if (press == Press::Lanes && !dragged && selected_.valid()) select({});  // click on empty space: deselect
    return true;
}

// ---- Drawing (headset/layout_editor.cpp's look) ----

void PcEditor::drawUnderPanel(const Pen& pen) const {
    cairo_t* cr = pen.cr;
    const double u = view_.ui;
    const Box& card = view_.layoutCard;
    drawCard(pen, card.x, card.y, card.w, card.h, 16 * u, kCard, kCard, 0);
    const double titleW = pen.text(card.x + kPad * u, card.y + 31 * u, text().editorTitle, 19 * u, kText, true);
    // Status of the last edit, else the hint
    std::string line = text().editorHint;
    Color color = kTextMuted;
    bool bold = false;
    if (status_ == static_cast<int>(CommitOutcome::Applied)) {
        line = text().layoutSaved;
        color = kSuccess;
        bold = true;
    } else if (status_ == static_cast<int>(CommitOutcome::Stale)) {
        line = text().layoutStale;
        color = kWarn;
        bold = true;
    } else if (status_ == static_cast<int>(CommitOutcome::Failed)) {
        line = text().layoutSaveFailed;
        color = kDanger;
        bold = true;
    }
    const double size = fitSize(pen, line, 14 * u, 10 * u, card.w - 2 * kPad * u - titleW - 16 * u, bold);
    pen.text(card.x + card.w - kPad * u, card.y + 30 * u, line, size, color, bold, true);

    cairo_save(cr);
    cairo_rectangle(cr, view_.viewport.x, view_.viewport.y, view_.viewport.w, view_.viewport.h);
    cairo_clip(cr);
    for (size_t i = 0; i < lanes_.size(); ++i) {
        const Box& lane = lanes_[i];
        if (!isNewColumnLane(i)) {
            pen.color(kBg);
            pen.roundedRect(lane.x, lane.y, lane.w, lane.h, 12 * u);
            cairo_fill(cr);
        } else {
            // The "new column" lane: dashed outline
            const double dashes[] = {10 * u, 8 * u};
            cairo_set_dash(cr, dashes, 2, 0);
            const bool hot = dragging_ && dropColumn_ == static_cast<int>(i);
            strokeRounded(pen, lane.x + 1, lane.y + 1, lane.w - 2, lane.h - 2, 12 * u, hot ? kAccent : kBorder,
                          (hot ? 3 : 2) * u);
            cairo_set_dash(cr, nullptr, 0, 0);
            const double s = fitSize(pen, text().editorNewColumn, 15 * u, 10 * u, lane.w - 20 * u, false);
            textCentered(pen, lane.x + lane.w / 2, lane.y + 34 * u, text().editorNewColumn, s,
                         hot ? kAccent : kTextMuted, false);
        }
    }
    cairo_restore(cr);
}

void PcEditor::drawOverPanel(const Pen& pen) const {
    cairo_t* cr = pen.cr;
    const double u = view_.ui;
    cairo_save(cr);
    cairo_rectangle(cr, view_.viewport.x, view_.viewport.y, view_.viewport.w, view_.viewport.h);
    cairo_clip(cr);
    // Selected card: pink outline; the lifted card fades in place
    if (selected_.valid()) {
        const Box b = cardBox(selected_);
        if (b.w > 0) strokeRounded(pen, b.x - 2 * u, b.y - 2 * u, b.w + 4 * u, b.h + 4 * u, 12 * u, kAccent, 3 * u);
    }
    if (dragging_ && press_ == Press::Card) {
        const Box b = cardBox(pressedCard_);
        pen.color(kBg, 0.65);
        pen.roundedRect(b.x, b.y, b.w, b.h, 10 * u);
        cairo_fill(cr);
    }
    // Drop marker: a pink bar with a dot at each end, between the cards where it lands
    if (dragging_ && dropColumn_ >= 0 && dropColumn_ < static_cast<int>(lanes_.size())) {
        const Box& lane = lanes_[static_cast<size_t>(dropColumn_)];
        double y = lane.y + 6 * u;
        if (!isNewColumnLane(static_cast<size_t>(dropColumn_))) {
            DropTarget t;
            t.kind = DropTarget::Kind::Grid;
            t.column = dropColumn_;
            t.index = dropSlot_;
            y = view_.originY + dropMarkerY(geometry_, t) * view_.scale;
        }
        pen.color(kAccent);
        pen.roundedRect(lane.x + 4 * u, y - 3 * u, lane.w - 8 * u, 6 * u, 3 * u);
        cairo_fill(cr);
        drawDisc(cr, lane.x + 6 * u, y, 7 * u, kAccent);
        drawDisc(cr, lane.x + lane.w - 6 * u, y, 7 * u, kAccent);
    }
    cairo_restore(cr);

    // Palette card
    const Box& card = view_.paletteCard;
    drawCard(pen, card.x, card.y, card.w, card.h, 16 * u, kCard, overPalette_ ? kDanger : kCard, overPalette_ ? 3 * u : 0);
    pen.text(card.x + kPad * u, card.y + 31 * u, overPalette_ ? text().editorDropToRemove : text().paletteTitle, 19 * u,
             overPalette_ ? kDanger : kText, true);
    for (const auto& [type, r] : chips_) {
        const bool offered = isOfferedInPalette(type);
        const int count = countType(layout_, type);
        const bool present = count > 0 && !isRepeatable(type);
        const bool pressed = press_ == Press::Chip && pressedChip_ == type;
        pen.color(present ? (pressed ? kAccentPressed : kAccent) : (pressed ? kControlHover : kControl));
        pen.roundedRect(r.x, r.y, r.w, r.h, r.h / 2);
        cairo_fill(cr);
        if (offered && !present) strokeRounded(pen, r.x, r.y, r.w, r.h, r.h / 2, kBorder, 1.5 * u);
        std::string label = chipLabel(type);
        if (!offered) label += std::string(" (") + text().paletteSoon + ")";
        if (isRepeatable(type) && count > 0) label += " ×" + std::to_string(count);
        const double checkW = present ? 16 * u : 0;
        const double size = fitSize(pen, label, 13.5 * u, 9 * u, r.w - 16 * u - checkW, present);
        const double tw = pen.measure(label, size, present) + checkW;
        const double tx = r.x + (r.w - tw) / 2;
        if (present) drawCheck(cr, tx + 5 * u, r.y + r.h / 2, 11 * u, kOnAccent);
        pen.text(tx + checkW, centerBaseline(r.y, r.h, size), label, size,
                 present ? kOnAccent : (offered ? kText : kTextDisabled), present);
    }

    // The selected widget's settings
    const double sx = card.x + kPad * u;
    const double sy = chips_.empty() ? card.y + kHeaderH * u : chips_.back().second.y + chips_.back().second.h + 20 * u;
    const WidgetInstance* w = widgetAt(layout_, selected_);
    if (w == nullptr) {
        const double size = fitSize(pen, text().selectedNone, 14 * u, 9 * u, card.w - 2 * kPad * u, false);
        pen.text(sx, centerBaseline(sy, kRowH * u, size), text().selectedNone, size, kTextMuted);
    } else {
        const auto button = [&](Setting s, const std::string& label, bool on) {
            for (const auto& [id, r] : settings_) {
                if (id != s) continue;
                pen.color(on ? kAccent : kControl);
                pen.roundedRect(r.x, r.y, r.w, r.h, 10 * u);
                cairo_fill(cr);
                if (!on) strokeRounded(pen, r.x, r.y, r.w, r.h, 10 * u, s == Setting::Remove ? kDanger : kBorder, 1.5 * u);
                const double size = fitSize(pen, label, 14 * u, 9 * u, r.w - 12 * u, true);
                textCentered(pen, r.x + r.w / 2, centerBaseline(r.y, r.h, size), label, size, on ? kOnAccent : kText, true);
            }
        };
        // R3: two-way settings are the dashboard's segmented pill (the headset's look), the halves being the buttons
        const auto pill = [&](Setting a, Setting b, const std::string& labelA, const std::string& labelB, bool aSelected) {
            const Box* ra = nullptr;
            const Box* rb = nullptr;
            for (const auto& [id, r] : settings_) {
                if (id == a) ra = &r;
                if (id == b) rb = &r;
            }
            if (ra == nullptr || rb == nullptr) return;
            drawSegmentedPill(pen, ra->x, ra->y, rb->x + rb->w - ra->x, ra->h, {{labelA}, {labelB}}, aSelected ? 0 : 1,
                              true, 14 * u, u);
        };
        const std::string title = widgetTitle(*w);
        const double titleSize = fitSize(pen, title, 17 * u, 11 * u, card.w - 2 * kPad * u - 110 * u, true);
        pen.text(sx, centerBaseline(sy, kRowH * u, titleSize), title, titleSize, kText, true);
        button(Setting::Remove, text().removeWidget, false);
        const double rowA = sy + (kRowH + kRowGap) * u;
        const double rowB = rowA + (kRowH + kRowGap) * u;
        const double labelSize = 14 * u;
        const auto fpsStepper = [&](double row) {  // T10: − n + (like Rows)
            button(Setting::FpsDown, "−", false);
            button(Setting::FpsUp, "+", false);
            for (const auto& [id, r] : settings_) {
                if (id != Setting::FpsDown) continue;
                textCentered(pen, r.x + kFpsStepperW * u / 2, centerBaseline(row, kRowH * u, 17 * u),
                             std::to_string(w->cameraFps), 17 * u, kText, true);
            }
        };
        if (isList(w->type)) {
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().rowRows, labelSize, kTextMuted);
            button(Setting::RowsDown, "−", false);
            button(Setting::RowsUp, "+", false);
            for (const auto& [id, r] : settings_) {
                if (id != Setting::RowsDown) continue;
                textCentered(pen, r.x + 170 * u / 2, centerBaseline(rowA, kRowH * u, 17 * u), std::to_string(w->maxItems),
                             17 * u, kText, true);
            }
            const double sameSize = fitSize(pen, text().rowSameName, labelSize, 9 * u, card.w - 2 * kPad * u - 178 * u, false);
            pen.text(sx, centerBaseline(rowB, kRowH * u, sameSize), text().rowSameName, sameSize, kTextMuted);
            pill(Setting::SameNameOn, Setting::SameNameOff, text().on, text().off, w->sumSameName);
            const double rowC = rowB + (kRowH + kRowGap) * u;  // R2
            pen.text(sx, centerBaseline(rowC, kRowH * u, labelSize), text().rowHideSystem, labelSize, kTextMuted);
            pill(Setting::HideSystemOn, Setting::HideSystemOff, text().on, text().off, w->hideSystem);
        } else if (hasCompactView(w->type)) {  // R2
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().rowCompact, labelSize, kTextMuted);
            pill(Setting::CompactOn, Setting::CompactOff, text().on, text().off, w->compact);
        } else if (w->type == WidgetType::Network) {
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().rowAdapter, labelSize, kTextMuted);
            pill(Setting::AdapterDongle, Setting::AdapterHome, text().adapterDongle, text().adapterHome,
                 w->adapter == NetAdapter::Dongle);
        } else if (w->type == WidgetType::HeadsetCamera) {  // T10
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().rowCameras, labelSize, kTextMuted);
            for (uint8_t c = 0; c < 4; ++c) {  // R3: one-part pills (✓ + accent when the cam is shown)
                const Setting s = static_cast<Setting>(static_cast<int>(Setting::Camera0) + c);
                for (const auto& [id, r] : settings_) {
                    if (id == s)
                        drawSegmentedPill(pen, r.x, r.y, r.w, r.h, {{cam::cameraName(c)}},
                                          (w->cameras & (1u << c)) != 0 ? 0 : -1, true, 14 * u, u);
                }
            }
            const double rowFps = rowB + (kRowH + kRowGap) * u;  // R3: the cams take two rows
            pen.text(sx, centerBaseline(rowFps, kRowH * u, labelSize), text().rowFps, labelSize, kTextMuted);
            fpsStepper(rowFps);
            const double rowC = rowFps + (kRowH + kRowGap) * u;  // R3
            pen.text(sx, centerBaseline(rowC, kRowH * u, labelSize), text().rowFullSize, labelSize, kTextMuted);
            pill(Setting::FullSizeOn, Setting::FullSizeOff, text().on, text().off, w->cameraFullSize);
        } else if (w->type == WidgetType::EyeCamera) {  // T10
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().rowFps, labelSize, kTextMuted);
            fpsStepper(rowA);
        } else if (w->type == WidgetType::HttpCamera) {
            const double urlSize = 13 * u;
            const std::string url = ellipsize(pen, w->url, urlSize, card.w - 2 * kPad * u - kEditW * u - 8 * u, false);
            pen.text(sx, centerBaseline(rowA, kRowH * u, urlSize), url, urlSize, kText);
            button(Setting::AddressEdit, text().editAddress, false);
            // − 15 fps +   − Size 70% +: the value between each pair of buttons
            const auto pairValue = [&](Setting down, Setting up, const std::string& value) {
                const Box* a = nullptr;
                const Box* b = nullptr;
                for (const auto& [id, r] : settings_) {
                    if (id == down) a = &r;
                    if (id == up) b = &r;
                }
                if (a == nullptr || b == nullptr) return;
                button(down, "−", false);
                button(up, "+", false);
                const double size = fitSize(pen, value, 15 * u, 10 * u, b->x - (a->x + a->w) - 6 * u, true);
                textCentered(pen, (a->x + a->w + b->x) / 2, centerBaseline(rowB, kRowH * u, size), value, size, kText,
                             true);
            };
            pairValue(Setting::FpsDown, Setting::FpsUp, std::to_string(w->cameraFps) + " fps");
            char sizeText[32];
            std::snprintf(sizeText, sizeof(sizeText), text().sizeFormat, unsigned(cameraSizeNearest(w->cameraSize)));
            pairValue(Setting::SizeDown, Setting::SizeUp, sizeText);
            const double rowC = rowB + (kRowH + kRowGap) * u;
            pen.text(sx, centerBaseline(rowC, kRowH * u, labelSize), text().rowFullSize, labelSize, kTextMuted);
            pill(Setting::FullSizeOn, Setting::FullSizeOff, text().on, text().off, w->cameraFullSize);
            const double rowD = rowC + (kRowH + kRowGap) * u;
            pen.text(sx, centerBaseline(rowD, kRowH * u, labelSize), text().rowRotation, labelSize, kTextMuted);
            button(Setting::RotationDown, "−", false);
            button(Setting::RotationUp, "+", false);
            for (const auto& [id, r] : settings_) {
                if (id != Setting::RotationDown) continue;
                textCentered(pen, r.x + kFpsStepperW * u / 2, centerBaseline(rowD, kRowH * u, 17 * u),
                             std::to_string(w->rotation * 90) + "°", 17 * u, kText, true);
            }
        } else {
            pen.text(sx, centerBaseline(rowA, kRowH * u, labelSize), text().noSettings, labelSize, kTextMuted);
        }
    }

    // The ghost follows the pointer (drawn last, unclipped, so it can be dragged onto the palette)
    if (dragging_) {
        std::string title;
        double gw = 220 * u;
        const double gh = 44 * u;
        if (press_ == Press::Card) {
            gw = std::max(gw, std::min(cardBox(pressedCard_).w, 360 * u));
            if (const WidgetInstance* lifted = widgetAt(layout_, pressedCard_)) title = widgetTitle(*lifted);
        } else {
            title = chipLabel(pressedChip_);
        }
        const double gx = pointerX_ - gw / 2;
        const double gy = pointerY_ - 16 * u;
        pen.color(kControlHover, 0.92);
        pen.roundedRect(gx, gy, gw, gh, 10 * u);
        cairo_fill(cr);
        strokeRounded(pen, gx, gy, gw, gh, 10 * u, overPalette_ ? kDanger : kAccent, 3 * u);
        const double size = fitSize(pen, title, 16 * u, 10 * u, gw - 24 * u, true);
        pen.text(gx + 12 * u, centerBaseline(gy, gh, size), title, size, kText, true);
    }
}

}  // namespace pc
}  // namespace fph
