// T05: the dashboard tab's layout editor. Left card: the layout as columns of cards (one per widget instance, height
// roughly proportional to the HUD card); press on a card and drag it, a drop marker shows where it lands, release
// commits. Right card: the palette (tap to add, or drag a chip into a column) and the selected widget's settings.
// Every change is handed out as a whole edited layout + the version it was made on; the caller sends it through
// LayoutAuthority::trySetLayout (the same compare-and-set a PC's LAYOUT_SET uses) and reports the outcome back.
#pragma once

#include "layout.h"
#include "layout_edit.h"

#include <cstdint>
#include <string>
#include <vector>

struct Pen;
struct UiText;

/** A rectangle in dashboard px. */
struct EditorRect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};

class LayoutEditor {
public:
    /** What a commit came back as (LayoutAuthority's Outcome, without depending on it). */
    enum class CommitOutcome { Applied, Stale, Failed };

    /** A pointer has to move this far (px) after a press before it counts as a drag rather than a tap. */
    static constexpr double kDragStartPx = 14.0;

    /**
     * @param layoutCard the left card (layout lanes)
     * @param paletteCard the right card (palette + selected widget's settings)
     */
    LayoutEditor(EditorRect layoutCard, EditorRect paletteCard);

    /**
     * Takes the current layout (config.layout). Called before every draw and pointer event, so a layout changed by a
     * PC or a file reload shows at once. A press or drag in progress keeps the version it started on, unless the
     * version moved (then it's cancelled).
     * @return true if the layout differed (redraw the tab)
     */
    bool sync(const fph::Layout& layout);

    /** Draws both cards. */
    void render(const Pen& pen, const UiText& text) const;

    /** @return true if a redraw is needed */
    bool pointerMove(double x, double y);
    /** @return true if a redraw is needed. A tap on a settings button may queue a commit (takeCommit) */
    bool pointerDown(double x, double y);
    /** Ends a press: a drop, a tap. May queue a commit. @return true if a redraw is needed */
    bool pointerUp(double x, double y);
    /** The pointer left the tab (or the dashboard closed): a drag in progress is cancelled, nothing is committed. */
    bool pointerLeave();
    /**
     * Smooth scroll (VREvent_ScrollSmooth) over the layout lanes.
     * @param dy the event's ydelta (+ = towards the top of the content)
     * @return true if a redraw is needed
     */
    bool scroll(double dy);

    /**
     * Takes the queued commit, if any.
     * @param baseVersion the layout version the edit was made on
     * @param layout the edited layout
     * @return false if nothing is queued
     */
    bool takeCommit(uint32_t& baseVersion, fph::Layout& layout);

    /** Reports how the last commit went (the status line in the layout card). */
    void commitResult(CommitOutcome outcome);

    /** @return true while a press or a drag is held (the caller polls input faster) */
    bool busy() const { return press_ != Press::None; }

    // ---- For tests: the geometry the hit-testing uses ----

    /** One card in the lanes, in dashboard px (scroll applied). */
    struct CardBox {
        fph::WidgetRef ref;
        EditorRect rect;
    };
    /** @return the lanes (one per column, plus the "new column" lane when there's room for one) */
    const std::vector<EditorRect>& lanes() const { return lanes_; }
    /** @return every card */
    const std::vector<CardBox>& cards() const { return cards_; }
    /** @return the palette chips, in WidgetType order from Frame (1) */
    const std::vector<std::pair<fph::WidgetType, EditorRect>>& chips() const { return chips_; }
    /** @return where the drop marker is (column, slot), or column −1 when there's none */
    std::pair<int, int> dropTarget() const { return {dropColumn_, dropSlot_}; }
    /** @return the selected widget (invalid ref = none) */
    fph::WidgetRef selected() const { return selected_; }
    /** @return the scroll offset (px) */
    double scrollY() const { return scrollY_; }
    /** @return the settings buttons for the selected widget, by id (see Setting) */
    enum class Setting {
        Remove, RowsDown, RowsUp, SameNameOn, SameNameOff, AdapterDongle, AdapterHome,
        Camera0, Camera1, Camera2, Camera3, FullSizeToggle,  // T10 Headset camera
        FpsDown, FpsUp,                                      // T10 both camera widgets
        CompactOn, CompactOff,                               // R2 Frame, Power, Battery, Temp
        HideSystemOn, HideSystemOff                          // R2 process lists (third row)
    };
    const std::vector<std::pair<Setting, EditorRect>>& settingButtons() const { return settings_; }

private:
    enum class Press { None, Card, Chip, Setting, Lanes };

    EditorRect layoutCard_;
    EditorRect paletteCard_;
    EditorRect viewport_;  ///< the lanes' visible area inside layoutCard_

    fph::Layout layout_;  ///< the layout the boxes below were made from
    std::vector<EditorRect> lanes_;
    std::vector<CardBox> cards_;
    std::vector<std::pair<fph::WidgetType, EditorRect>> chips_;
    std::vector<std::pair<Setting, EditorRect>> settings_;
    double contentH_ = 0.0;  ///< tallest lane (px, unscrolled)
    double scrollY_ = 0.0;

    Press press_ = Press::None;
    fph::WidgetRef pressedCard_;
    fph::WidgetType pressedChip_ = fph::WidgetType::None;
    Setting pressedSetting_ = Setting::Remove;
    uint32_t pressVersion_ = 0;  ///< layout version when the press started (the commit's base)
    double pressX_ = 0, pressY_ = 0;
    double pointerX_ = -1, pointerY_ = -1;
    bool dragging_ = false;
    int dropColumn_ = -1;
    int dropSlot_ = 0;
    bool overPalette_ = false;  ///< dragging a card over the palette = remove on release

    fph::WidgetRef selected_;
    fph::WidgetType selectedType_ = fph::WidgetType::None;  ///< to notice the selection pointing elsewhere after a sync

    bool commitQueued_ = false;
    uint32_t commitBase_ = 0;
    fph::Layout commitLayout_;
    fph::WidgetRef selectAfterCommit_;
    int status_ = -1;  ///< -1 none, else a CommitOutcome

    void relayout();
    void clampScroll();
    void updateDrop(double x, double y);
    void queueCommit(const fph::Layout& edited, fph::WidgetRef selectAfter);
    void applySetting(Setting setting);
    void cancelPress();
    fph::WidgetRef cardAt(double x, double y) const;
    int chipAt(double x, double y) const;
    int settingAt(double x, double y) const;

    void drawLanes(const Pen& pen, const UiText& text) const;
    void drawPalette(const Pen& pen, const UiText& text) const;
};
