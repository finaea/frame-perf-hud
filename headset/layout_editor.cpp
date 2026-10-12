// T05: the dashboard tab's layout editor.
#include "layout_editor.h"

#include "camera_frame.h"  // T10: camera names
#include "draw.h"
#include "i18n.h"
#include "theme.h"
#include "widgets.h"  // widgetHeight, widgetTitle

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kCardPad = 20;       // inside a card, like settings_panel.cpp
constexpr double kHeaderH = 64;       // card title row
constexpr double kLaneGap = 12;
constexpr double kMaxLaneW = 300;
constexpr double kLaneInset = 8;      // cards inside a lane
constexpr double kBoxGap = 8;
constexpr double kBoxScale = 0.3;     // card height = HUD card height × this, within the limits below
constexpr double kBoxMinH = 52;       // still an easy laser target
constexpr double kBoxMaxH = 260;
constexpr double kAutoScrollEdge = 40;  // dragging this close to the lanes' top / bottom scrolls them
constexpr double kAutoScrollStep = 18;
constexpr double kScrollPxPerUnit = 120;  // VREvent_ScrollSmooth ydelta → px (verify on device)
constexpr int kChipColumns = 3;
constexpr double kChipH = 40;
constexpr double kChipGap = 8;
constexpr int kChipRows = 5;
constexpr double kRowH = 44;          // settings rows
constexpr double kRowGap = 8;
constexpr double kFpsStepperW = 200;  // T10: the camera widgets' FPS − n + (no room for a third settings row)
constexpr double kFpsStep = 60;
constexpr uint32_t kRowsMin = 1;

/** Card height in the lanes. */
double boxHeight(const fph::WidgetInstance& w) {
    return std::clamp(std::round(fph::widgetHeight(w) * kBoxScale), kBoxMinH, kBoxMaxH);
}

/** Rows −: 1 step below 5, else down to the previous multiple of 5. */
uint32_t rowsDown(uint32_t v) {
    if (v <= 5) return std::max(kRowsMin, v - 1);
    return v % 5 == 0 ? v - 5 : v - v % 5;
}

/** Rows +: 1 step below 5, else up to the next multiple of 5 (max kMaxListItems). */
uint32_t rowsUp(uint32_t v) {
    if (v < 5) return v + 1;
    return std::min(fph::kMaxListItems, (v / 5 + 1) * 5);
}

bool isList(fph::WidgetType t) {
    return t == fph::WidgetType::CpuItems || t == fph::WidgetType::RamItems;
}

/** Palette label of a type. */
std::string chipLabel(fph::WidgetType type, const UiText& text) {
    if (type == fph::WidgetType::Network) return text.paletteNetwork;
    fph::WidgetInstance w;
    w.type = type;
    return fph::widgetTitle(w);
}

/** Second line of a card in the lanes ("" = none). */
std::string cardDetail(const fph::WidgetInstance& w, const UiText& text) {
    if (!isList(w.type)) return "";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), text.rowsFormat, w.maxItems);
    return std::string(buffer) + (w.sumSameName ? text.summedSuffix : "");
}

}  // namespace

LayoutEditor::LayoutEditor(EditorRect layoutCard, EditorRect paletteCard)
    : layoutCard_(layoutCard), paletteCard_(paletteCard) {
    viewport_ = {layoutCard.x + kCardPad, layoutCard.y + kHeaderH, layoutCard.w - 2 * kCardPad,
                 layoutCard.h - kHeaderH - kCardPad};
    relayout();
}

bool LayoutEditor::sync(const fph::Layout& layout) {
    if (layout.version == layout_.version && layout.sameWidgets(layout_)) return false;
    const bool versionMoved = layout.version != layout_.version;
    layout_ = layout;
    // A press made on another version would apply its indices to the wrong layout: drop it
    if (versionMoved && press_ != Press::None && press_ != Press::Setting) cancelPress();
    const fph::WidgetInstance* w = fph::widgetAt(layout_, selected_);
    if (w == nullptr || w->type != selectedType_) selected_ = {};
    relayout();
    return true;
}

void LayoutEditor::relayout() {
    lanes_.clear();
    cards_.clear();
    const int columns = static_cast<int>(layout_.columns.size());
    const int n = std::max(1, columns + (layout_.columns.size() < fph::kMaxColumns ? 1 : 0));
    const double laneW = std::min(kMaxLaneW, (viewport_.w - (n - 1) * kLaneGap) / n);
    // Measure → clamp → place: a layout that got shorter (fewer rows, a card moved out) must not keep a scroll
    // offset that only the old height allowed, or its cards would sit clipped above the view
    contentH_ = 0.0;
    for (const fph::LayoutColumn& column : layout_.columns) {
        double y = kLaneInset;
        for (const fph::WidgetInstance& w : column.widgets) y += boxHeight(w) + kBoxGap;
        contentH_ = std::max(contentH_, y);
    }
    clampScroll();
    for (int i = 0; i < n; ++i) {
        const EditorRect lane {viewport_.x + i * (laneW + kLaneGap), viewport_.y, laneW, viewport_.h};
        lanes_.push_back(lane);
        if (i >= columns) continue;
        double y = kLaneInset;
        const std::vector<fph::WidgetInstance>& widgets = layout_.columns[static_cast<size_t>(i)].widgets;
        for (size_t k = 0; k < widgets.size(); ++k) {
            const double h = boxHeight(widgets[k]);
            cards_.push_back({{i, static_cast<int>(k)},
                              {lane.x + kLaneInset, viewport_.y + y - scrollY_, lane.w - 2 * kLaneInset, h}});
            y += h + kBoxGap;
        }
    }

    // Palette: one chip per type, WidgetType order
    chips_.clear();
    const double chipX = paletteCard_.x + kCardPad;
    const double chipW = (paletteCard_.w - 2 * kCardPad - (kChipColumns - 1) * kChipGap) / kChipColumns;
    const double chipY = paletteCard_.y + kHeaderH;
    for (int t = 1; t < static_cast<int>(fph::kWidgetTypeCount); ++t) {
        const int i = t - 1;
        chips_.push_back({static_cast<fph::WidgetType>(t),
                          {chipX + (i % kChipColumns) * (chipW + kChipGap), chipY + (i / kChipColumns) * (kChipH + kChipGap),
                           chipW, kChipH}});
    }

    // The selected widget's settings: [title ......... Remove] / row A / row B
    settings_.clear();
    const fph::WidgetInstance* w = fph::widgetAt(layout_, selected_);
    if (w == nullptr) return;
    const double sx = paletteCard_.x + kCardPad;
    const double sw = paletteCard_.w - 2 * kCardPad;
    const double sy = chipY + kChipRows * (kChipH + kChipGap) + 4;
    const double removeW = 130;
    settings_.push_back({Setting::Remove, {sx + sw - removeW, sy, removeW, kRowH}});
    const double rowA = sy + kRowH + kRowGap;
    const double rowB = rowA + kRowH + kRowGap;
    const double controlW = 270;
    const double controlX = sx + sw - controlW;
    if (isList(w->type)) {
        const double step = 70;
        settings_.push_back({Setting::RowsDown, {controlX, rowA, step, kRowH}});
        settings_.push_back({Setting::RowsUp, {controlX + controlW - step, rowA, step, kRowH}});
        settings_.push_back({Setting::SameNameOn, {controlX, rowB, controlW / 2, kRowH}});
        settings_.push_back({Setting::SameNameOff, {controlX + controlW / 2, rowB, controlW / 2, kRowH}});
    } else if (w->type == fph::WidgetType::Network) {
        settings_.push_back({Setting::AdapterDongle, {controlX, rowA, controlW / 2, kRowH}});
        settings_.push_back({Setting::AdapterHome, {controlX + controlW / 2, rowA, controlW / 2, kRowH}});
    } else if (w->type == fph::WidgetType::HeadsetCamera) {  // T10: one toggle per outward cam; FPS − n + and Full size
        const double cw = (controlW + 70) / 4;
        for (int c = 0; c < 4; ++c)
            settings_.push_back({static_cast<Setting>(static_cast<int>(Setting::Camera0) + c),
                                 {controlX - 70 + c * cw, rowA, cw - 4, kRowH}});
        settings_.push_back({Setting::FpsDown, {controlX - 70, rowB, kFpsStep, kRowH}});
        settings_.push_back({Setting::FpsUp, {controlX - 70 + kFpsStepperW - kFpsStep, rowB, kFpsStep, kRowH}});
        settings_.push_back({Setting::FullSizeToggle, {controlX + kFpsStepperW - 60, rowB, controlW + 60 - kFpsStepperW, kRowH}});
    } else if (w->type == fph::WidgetType::EyeCamera) {  // T10: FPS − n +
        settings_.push_back({Setting::FpsDown, {controlX - 70, rowA, kFpsStep, kRowH}});
        settings_.push_back({Setting::FpsUp, {controlX - 70 + kFpsStepperW - kFpsStep, rowA, kFpsStep, kRowH}});
    }
}

void LayoutEditor::clampScroll() {
    const double maxScroll = std::max(0.0, contentH_ - viewport_.h);
    scrollY_ = std::clamp(scrollY_, 0.0, maxScroll);
}

fph::WidgetRef LayoutEditor::cardAt(double x, double y) const {
    if (!viewport_.contains(x, y)) return {};
    for (const CardBox& c : cards_) {
        if (c.rect.contains(x, y)) return c.ref;
    }
    return {};
}

int LayoutEditor::chipAt(double x, double y) const {
    for (size_t i = 0; i < chips_.size(); ++i) {
        if (chips_[i].second.contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

int LayoutEditor::settingAt(double x, double y) const {
    for (size_t i = 0; i < settings_.size(); ++i) {
        if (settings_[i].second.contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

void LayoutEditor::cancelPress() {
    press_ = Press::None;
    dragging_ = false;
    dropColumn_ = -1;
    overPalette_ = false;
}

void LayoutEditor::updateDrop(double x, double y) {
    dropColumn_ = -1;
    overPalette_ = press_ == Press::Card && paletteCard_.contains(x, y);
    if (overPalette_) return;
    // Near the top / bottom edge of the lanes: scroll a step per pointer move
    if (x >= viewport_.x && x <= viewport_.x + viewport_.w) {
        const double before = scrollY_;
        if (y < viewport_.y + kAutoScrollEdge) scrollY_ -= kAutoScrollStep;
        if (y > viewport_.y + viewport_.h - kAutoScrollEdge) scrollY_ += kAutoScrollStep;
        clampScroll();
        if (scrollY_ != before) relayout();
    }
    if (y < viewport_.y || y > viewport_.y + viewport_.h) return;
    for (size_t i = 0; i < lanes_.size(); ++i) {
        const EditorRect& lane = lanes_[i];
        if (x < lane.x || x > lane.x + lane.w) continue;
        std::vector<double> centers;
        for (const CardBox& c : cards_) {
            if (c.ref.column == static_cast<int>(i)) centers.push_back(c.rect.y + c.rect.h / 2);
        }
        const int column = static_cast<int>(i);
        const int slot = fph::dropSlot(centers, y);
        // Only show a marker where the drop would change something
        fph::Layout trial = layout_;
        const bool changes = press_ == Press::Card
                                 ? fph::moveWidget(trial, pressedCard_, column, slot)
                                 : fph::insertWidget(trial, fph::newInstance(pressedChip_, layout_), column, slot);
        if (changes) {
            dropColumn_ = column;
            dropSlot_ = slot;
        }
        return;
    }
}

void LayoutEditor::queueCommit(const fph::Layout& edited, fph::WidgetRef selectAfter) {
    commitQueued_ = true;
    commitBase_ = pressVersion_;
    commitLayout_ = edited;
    selectAfterCommit_ = selectAfter;
}

bool LayoutEditor::takeCommit(uint32_t& baseVersion, fph::Layout& layout) {
    if (!commitQueued_) return false;
    commitQueued_ = false;
    baseVersion = commitBase_;
    layout = commitLayout_;
    return true;
}

void LayoutEditor::commitResult(CommitOutcome outcome) {
    status_ = static_cast<int>(outcome);
    if (outcome == CommitOutcome::Applied) {
        // The caller's config.layout is the edited one now; take it so the selection lands on the right card
        layout_ = commitLayout_;
        layout_.version = commitBase_ + 1;
        selected_ = selectAfterCommit_;
        const fph::WidgetInstance* w = fph::widgetAt(layout_, selected_);
        selectedType_ = w != nullptr ? w->type : fph::WidgetType::None;
        if (w == nullptr) selected_ = {};
        clampScroll();
        relayout();
    }
}

void LayoutEditor::applySetting(Setting setting) {
    const fph::WidgetInstance* w = fph::widgetAt(layout_, selected_);
    if (w == nullptr) return;
    fph::Layout edited = layout_;
    if (setting == Setting::Remove) {
        fph::removeWidget(edited, selected_);
        queueCommit(edited, {});
        return;
    }
    fph::WidgetInstance& e = edited.columns[static_cast<size_t>(selected_.column)].widgets[static_cast<size_t>(selected_.index)];
    switch (setting) {
        case Setting::RowsDown: e.maxItems = rowsDown(e.maxItems); break;
        case Setting::RowsUp: e.maxItems = rowsUp(e.maxItems); break;
        case Setting::SameNameOn: e.sumSameName = true; break;
        case Setting::SameNameOff: e.sumSameName = false; break;
        case Setting::AdapterDongle: e.adapter = fph::NetAdapter::Dongle; break;
        case Setting::AdapterHome: e.adapter = fph::NetAdapter::Home; break;
        case Setting::Camera0:
        case Setting::Camera1:
        case Setting::Camera2:
        case Setting::Camera3: {  // T10: toggles one cam; the last one can't be turned off (an empty card)
            const auto bit = static_cast<uint8_t>(1u << (static_cast<int>(setting) - static_cast<int>(Setting::Camera0)));
            const auto mask = static_cast<uint8_t>(e.cameras ^ bit);
            if (mask & 0x0F) e.cameras = mask;
            break;
        }
        case Setting::FullSizeToggle: e.cameraFullSize = !e.cameraFullSize; break;
        case Setting::FpsDown: e.cameraFps = fph::cameraFpsStep(e.cameraFps, -1, e.type); break;
        case Setting::FpsUp: e.cameraFps = fph::cameraFpsStep(e.cameraFps, +1, e.type); break;
        case Setting::Remove: break;
    }
    if (e != *w) queueCommit(edited, selected_);
}

bool LayoutEditor::pointerMove(double x, double y) {
    pointerX_ = x;
    pointerY_ = y;
    if (press_ != Press::Card && press_ != Press::Chip) return false;
    if (!dragging_) {
        if (std::hypot(x - pressX_, y - pressY_) < kDragStartPx) return false;
        if (press_ == Press::Chip && !fph::canAdd(layout_, pressedChip_)) return false;
        dragging_ = true;
    }
    updateDrop(x, y);
    return true;  // the ghost card follows the pointer
}

bool LayoutEditor::pointerDown(double x, double y) {
    if (press_ != Press::None) cancelPress();  // an Up we never got
    pointerX_ = pressX_ = x;
    pointerY_ = pressY_ = y;
    pressVersion_ = layout_.version;
    status_ = -1;
    const int s = settingAt(x, y);
    if (s >= 0) {
        press_ = Press::Setting;
        pressedSetting_ = settings_[static_cast<size_t>(s)].first;
        applySetting(pressedSetting_);  // like upstream's buttons: acts on press
        return true;
    }
    const int c = chipAt(x, y);
    if (c >= 0) {
        const fph::WidgetType type = chips_[static_cast<size_t>(c)].first;
        if (!fph::isOfferedInPalette(type)) return false;
        press_ = Press::Chip;
        pressedChip_ = type;
        return true;
    }
    const fph::WidgetRef card = cardAt(x, y);
    if (card.valid()) {
        press_ = Press::Card;
        pressedCard_ = card;
        return true;
    }
    if (viewport_.contains(x, y)) press_ = Press::Lanes;
    return false;
}

bool LayoutEditor::pointerUp(double x, double y) {
    if (press_ == Press::None) return false;
    pointerX_ = x;
    pointerY_ = y;
    const Press press = press_;
    const bool dragged = dragging_;
    if (dragged) updateDrop(x, y);
    const int column = dropColumn_;
    const int slot = dropSlot_;
    const bool toPalette = overPalette_;
    cancelPress();

    if (press == Press::Card) {
        if (!dragged) {
            // Tap: select (again = deselect)
            if (selected_ == pressedCard_) {
                selected_ = {};
            } else {
                selected_ = pressedCard_;
                selectedType_ = fph::widgetAt(layout_, selected_)->type;
            }
            relayout();
            return true;
        }
        fph::Layout edited = layout_;
        if (toPalette) {
            if (fph::removeWidget(edited, pressedCard_)) queueCommit(edited, {});
            return true;
        }
        if (column < 0 || !fph::moveWidget(edited, pressedCard_, column, slot)) return true;
        // Where it landed: its source column may have been removed (it was alone there) before the target
        const int columns = static_cast<int>(layout_.columns.size());
        const bool sourceGone =
            layout_.columns[static_cast<size_t>(pressedCard_.column)].widgets.size() == 1 && column != pressedCard_.column;
        fph::WidgetRef landed {column, slot};
        if (column == pressedCard_.column && slot > pressedCard_.index) landed.index = slot - 1;
        if (column == columns) landed.index = 0;
        if (sourceGone && pressedCard_.column < column) landed.column -= 1;
        queueCommit(edited, landed);
        return true;
    }
    if (press == Press::Chip) {
        fph::Layout edited = layout_;
        const fph::WidgetInstance widget = fph::newInstance(pressedChip_, layout_);
        if (dragged) {
            if (column >= 0 && fph::insertWidget(edited, widget, column, slot)) {
                queueCommit(edited, {column, std::min(slot, static_cast<int>(edited.columns[static_cast<size_t>(column)].widgets.size()) - 1)});
            }
            return true;
        }
        // Tap on a type that's there and can't repeat: select it
        if (!fph::isRepeatable(pressedChip_) && fph::countType(layout_, pressedChip_) > 0) {
            for (const CardBox& c : cards_) {
                if (fph::widgetAt(layout_, c.ref)->type == pressedChip_) {
                    selected_ = c.ref;
                    selectedType_ = pressedChip_;
                    break;
                }
            }
            relayout();
            return true;
        }
        if (!fph::canAdd(layout_, pressedChip_)) return true;
        // Tap to add: below the selected widget if its column has room, else at the end of the shortest column
        int target = -1;
        int at = 0;
        if (selected_.valid() &&
            layout_.columns[static_cast<size_t>(selected_.column)].widgets.size() < fph::kMaxWidgetsPerColumn) {
            target = selected_.column;
            at = selected_.index + 1;
        } else {
            target = fph::shortestColumn(layout_, [](const fph::WidgetInstance& w) { return fph::widgetHeight(w); });
            at = target >= 0 && target < static_cast<int>(layout_.columns.size())
                     ? static_cast<int>(layout_.columns[static_cast<size_t>(target)].widgets.size())
                     : 0;
        }
        if (target >= 0 && fph::insertWidget(edited, widget, target, at)) queueCommit(edited, {target, at});
        return true;
    }
    if (press == Press::Lanes && !dragged && selected_.valid()) {
        selected_ = {};  // tap on empty space: deselect
        relayout();
    }
    return true;
}

bool LayoutEditor::pointerLeave() {
    if (press_ == Press::None) return false;
    cancelPress();
    return true;
}

bool LayoutEditor::scroll(double dy) {
    if (!viewport_.contains(pointerX_, pointerY_) && press_ == Press::None) return false;
    const double before = scrollY_;
    scrollY_ -= dy * kScrollPxPerUnit;
    clampScroll();
    if (scrollY_ == before) return false;
    relayout();
    if (dragging_) updateDrop(pointerX_, pointerY_);
    return true;
}

// ---- Drawing ----

void LayoutEditor::render(const Pen& pen, const UiText& text) const {
    drawPalette(pen, text);
    drawLanes(pen, text);  // second: the dragged card's ghost is drawn over the palette
}

void LayoutEditor::drawLanes(const Pen& pen, const UiText& text) const {
    cairo_t* cr = pen.cr;
    const EditorRect& card = layoutCard_;
    drawCard(pen, card.x, card.y, card.w, card.h, 20, kCard, kCard, 0);
    const double titleW = pen.text(card.x + kCardPad, card.y + 42, text.editorTitle, 24, kText, true);
    // Status of the last edit, else the hint
    std::string line = text.editorHint;
    Color color = kTextMuted;
    bool bold = false;
    if (status_ == static_cast<int>(CommitOutcome::Applied)) {
        line = text.layoutSaved;
        color = kSuccess;
        bold = true;
    } else if (status_ == static_cast<int>(CommitOutcome::Stale)) {
        line = text.layoutStale;
        color = kWarn;
        bold = true;
    } else if (status_ == static_cast<int>(CommitOutcome::Failed)) {
        line = text.layoutSaveFailed;
        color = kDanger;
        bold = true;
    }
    const double lineSize = fitSize(pen, line, 18, 13, card.w - 2 * kCardPad - titleW - 16, bold);
    pen.text(card.x + card.w - kCardPad, card.y + 40, line, lineSize, color, bold, true);

    cairo_save(cr);
    cairo_rectangle(cr, viewport_.x, viewport_.y, viewport_.w, viewport_.h);
    cairo_clip(cr);
    const int columns = static_cast<int>(layout_.columns.size());
    for (size_t i = 0; i < lanes_.size(); ++i) {
        const EditorRect& lane = lanes_[i];
        if (static_cast<int>(i) < columns) {
            pen.color(kBg);
            pen.roundedRect(lane.x, lane.y, lane.w, lane.h, 14);
            cairo_fill(cr);
        } else {
            // The "new column" lane: dashed outline
            const double dashes[] = {10, 8};
            cairo_set_dash(cr, dashes, 2, 0);
            strokeRounded(pen, lane.x + 1, lane.y + 1, lane.w - 2, lane.h - 2, 14, kBorder, 2);
            cairo_set_dash(cr, nullptr, 0, 0);
            const double size = fitSize(pen, text.editorNewColumn, 18, 12, lane.w - 20, false);
            textCentered(pen, lane.x + lane.w / 2, lane.y + 40, text.editorNewColumn, size, kTextMuted, false);
        }
    }
    for (const CardBox& c : cards_) {
        const fph::WidgetInstance* w = fph::widgetAt(layout_, c.ref);
        if (w == nullptr) continue;
        const bool lifted = dragging_ && press_ == Press::Card && c.ref == pressedCard_;
        const bool selected = c.ref == selected_;
        pen.color(kControl, lifted ? 0.35 : 1.0);
        pen.roundedRect(c.rect.x, c.rect.y, c.rect.w, c.rect.h, 10);
        cairo_fill(cr);
        strokeRounded(pen, c.rect.x, c.rect.y, c.rect.w, c.rect.h, 10, selected ? kAccent : kBorder, selected ? 3 : 1.5);
        const std::string title = fph::widgetTitle(*w);
        const double size = fitSize(pen, title, 20, 13, c.rect.w - 24, true);
        pen.text(c.rect.x + 12, c.rect.y + 32, title, size, lifted ? kTextDisabled : kText, true);
        const std::string detail = cardDetail(*w, text);
        if (!detail.empty() && c.rect.h >= 70) {
            pen.text(c.rect.x + 12, c.rect.y + 56, detail, fitSize(pen, detail, 16, 12, c.rect.w - 24, false), kTextMuted);
        }
    }
    // Drop marker
    if (dragging_ && dropColumn_ >= 0 && dropColumn_ < static_cast<int>(lanes_.size())) {
        const EditorRect& lane = lanes_[static_cast<size_t>(dropColumn_)];
        double y = lane.y + kLaneInset / 2 - scrollY_;
        int k = 0;
        for (const CardBox& c : cards_) {
            if (c.ref.column != dropColumn_) continue;
            if (k == dropSlot_) {
                y = c.rect.y - kBoxGap / 2;
                break;
            }
            y = c.rect.y + c.rect.h + kBoxGap / 2;
            ++k;
        }
        pen.color(kAccent);
        pen.roundedRect(lane.x + 4, y - 3, lane.w - 8, 6, 3);
        cairo_fill(cr);
        drawDisc(cr, lane.x + 6, y, 7, kAccent);
        drawDisc(cr, lane.x + lane.w - 6, y, 7, kAccent);
    }
    // Scroll bar
    if (contentH_ > viewport_.h) {
        const double trackX = viewport_.x + viewport_.w - 6;
        const double thumbH = std::max(40.0, viewport_.h * viewport_.h / contentH_);
        const double thumbY = viewport_.y + (viewport_.h - thumbH) * scrollY_ / (contentH_ - viewport_.h);
        pen.color(kBorder, 0.8);
        pen.roundedRect(trackX, thumbY, 5, thumbH, 2.5);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    // The ghost follows the pointer (drawn last, unclipped, so it can be dragged onto the palette)
    if (dragging_) {
        std::string title;
        double w = 260, h = kBoxMinH;
        if (press_ == Press::Card) {
            for (const CardBox& c : cards_) {
                if (c.ref == pressedCard_) {
                    w = c.rect.w;
                    h = std::min(c.rect.h, 120.0);
                }
            }
            if (const fph::WidgetInstance* lifted = fph::widgetAt(layout_, pressedCard_)) title = fph::widgetTitle(*lifted);
        } else {
            title = chipLabel(pressedChip_, text);
        }
        const double gx = pointerX_ - w / 2;
        const double gy = pointerY_ - 20;
        pen.color(kControlHover, 0.92);
        pen.roundedRect(gx, gy, w, h, 10);
        cairo_fill(cr);
        strokeRounded(pen, gx, gy, w, h, 10, overPalette_ ? kDanger : kAccent, 3);
        pen.text(gx + 12, gy + 32, title, fitSize(pen, title, 20, 13, w - 24, true), kText, true);
    }
}

void LayoutEditor::drawPalette(const Pen& pen, const UiText& text) const {
    cairo_t* cr = pen.cr;
    const EditorRect& card = paletteCard_;
    drawCard(pen, card.x, card.y, card.w, card.h, 20, kCard, overPalette_ ? kDanger : kCard, overPalette_ ? 3 : 0);
    pen.text(card.x + kCardPad, card.y + 42, overPalette_ ? text.editorDropToRemove : text.paletteTitle, 24,
             overPalette_ ? kDanger : kText, true);

    for (const auto& [type, r] : chips_) {
        const bool offered = fph::isOfferedInPalette(type);
        const int count = fph::countType(layout_, type);
        const bool present = count > 0 && !fph::isRepeatable(type);
        const bool pressed = press_ == Press::Chip && pressedChip_ == type;
        if (present) {
            pen.color(pressed ? kAccentPressed : kAccent);
        } else {
            pen.color(pressed ? kControlHover : kControl);
        }
        pen.roundedRect(r.x, r.y, r.w, r.h, r.h / 2);
        cairo_fill(cr);
        if (offered && !present) strokeRounded(pen, r.x, r.y, r.w, r.h, r.h / 2, kBorder, 1.5);
        std::string label = chipLabel(type, text);
        if (!offered) label += std::string(" (") + text.paletteSoon + ")";
        if (fph::isRepeatable(type) && count > 0) label += " ×" + std::to_string(count);
        const double checkW = present ? 20 : 0;
        const double size = fitSize(pen, label, 17, 11, r.w - 20 - checkW, present);
        const double tw = pen.measure(label, size, present) + checkW;
        const double tx = r.x + (r.w - tw) / 2;
        if (present) drawCheck(cr, tx + 7, r.y + r.h / 2, 14, kOnAccent);
        pen.text(tx + checkW, centerBaseline(r.y, r.h, size), label, size,
                 present ? kOnAccent : (offered ? kText : kTextDisabled), present);
    }

    // The selected widget's settings
    const double sx = card.x + kCardPad;
    const double sy = card.y + kHeaderH + kChipRows * (kChipH + kChipGap) + 4;
    const fph::WidgetInstance* w = fph::widgetAt(layout_, selected_);
    if (w == nullptr) {
        const double size = fitSize(pen, text.selectedNone, 18, 12, card.w - 2 * kCardPad, false);
        pen.text(sx, centerBaseline(sy, kRowH, size), text.selectedNone, size, kTextMuted);
        return;
    }
    const auto button = [&](Setting s, const std::string& label, bool selected) {
        for (const auto& [id, r] : settings_) {
            if (id != s) continue;
            const bool pressed = press_ == Press::Setting && pressedSetting_ == s;
            pen.color(selected ? (pressed ? kAccentPressed : kAccent) : (pressed ? kControlHover : kControl));
            pen.roundedRect(r.x, r.y, r.w, r.h, 12);
            cairo_fill(cr);
            if (!selected) strokeRounded(pen, r.x, r.y, r.w, r.h, 12, s == Setting::Remove ? kDanger : kBorder, 1.5);
            const double size = fitSize(pen, label, 20, 12, r.w - 16, true);
            textCentered(pen, r.x + r.w / 2, centerBaseline(r.y, r.h, size), label, size, selected ? kOnAccent : kText, true);
        }
    };
    const std::string title = fph::widgetTitle(*w);
    pen.text(sx, centerBaseline(sy, kRowH, 22), title, fitSize(pen, title, 22, 14, card.w - 2 * kCardPad - 150, true), kText,
             true);
    button(Setting::Remove, text.removeWidget, false);
    const double rowA = sy + kRowH + kRowGap;
    const double rowB = rowA + kRowH + kRowGap;
    const auto fpsStepper = [&](double row) {  // T10: − n + (like Rows)
        button(Setting::FpsDown, "−", false);
        button(Setting::FpsUp, "+", false);
        for (const auto& [id, r] : settings_) {
            if (id != Setting::FpsDown) continue;
            textCentered(pen, r.x + kFpsStepperW / 2.0, centerBaseline(row, kRowH, 24), std::to_string(w->cameraFps), 24,
                         kText, true);
        }
    };
    if (isList(w->type)) {
        pen.text(sx, centerBaseline(rowA, kRowH, 20), text.rowRows, 20, kTextMuted);
        button(Setting::RowsDown, "−", false);
        button(Setting::RowsUp, "+", false);
        for (const auto& [id, r] : settings_) {
            if (id != Setting::RowsDown) continue;
            const double cx = r.x + 270 / 2.0;
            textCentered(pen, cx, centerBaseline(rowA, kRowH, 24), std::to_string(w->maxItems), 24, kText, true);
        }
        const double labelSize = fitSize(pen, text.rowSameName, 20, 12, card.w - 2 * kCardPad - 280, false);
        pen.text(sx, centerBaseline(rowB, kRowH, labelSize), text.rowSameName, labelSize, kTextMuted);
        button(Setting::SameNameOn, text.on, w->sumSameName);
        button(Setting::SameNameOff, text.off, !w->sumSameName);
    } else if (w->type == fph::WidgetType::Network) {
        pen.text(sx, centerBaseline(rowA, kRowH, 20), text.rowAdapter, 20, kTextMuted);
        button(Setting::AdapterDongle, text.adapterDongle, w->adapter == fph::NetAdapter::Dongle);
        button(Setting::AdapterHome, text.adapterHome, w->adapter == fph::NetAdapter::Home);
    } else if (w->type == fph::WidgetType::HeadsetCamera) {  // T10
        pen.text(sx, centerBaseline(rowA, kRowH, 20), text.rowCameras, 20, kTextMuted);
        for (uint8_t c = 0; c < 4; ++c)
            button(static_cast<Setting>(static_cast<int>(Setting::Camera0) + c), fph::cam::cameraName(c),
                   (w->cameras & (1u << c)) != 0);
        pen.text(sx, centerBaseline(rowB, kRowH, 20), text.rowFps, 20, kTextMuted);
        fpsStepper(rowB);
        button(Setting::FullSizeToggle, text.rowFullSize, w->cameraFullSize);
    } else if (w->type == fph::WidgetType::EyeCamera) {  // T10
        pen.text(sx, centerBaseline(rowA, kRowH, 20), text.rowFps, 20, kTextMuted);
        fpsStepper(rowA);
    } else {
        pen.text(sx, centerBaseline(rowA, kRowH, 18), text.noSettings, 18, kTextMuted);
    }
}
