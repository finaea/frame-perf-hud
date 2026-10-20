// The PC's layout editor (Ctrl+E), made to look and work like the headset's dashboard editor
// (headset/layout_editor.*): a "Layout" card whose columns are dark lanes holding the real widget cards, a dashed
// "New column" lane, and a palette card with the headset's chips (pink + check = already there, ×N for Network,
// "(soon)" disabled) and the selected widget's settings. Same strings (headset/i18n), same colours, same
// core/layout_edit logic. Device pixels throughout; the window places the cards with setView() every frame.
// Every change is handed out as a whole edited layout + the version it was made on (takeCommit), like the headset.
#pragma once

#include "draw.h"
#include "grid_renderer.h"
#include "layout.h"
#include "layout_edit.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fph {
namespace pc {

/** A rectangle in window (device) px. */
struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};

class PcEditor {
public:
    enum class CommitOutcome { Applied, Stale, Failed };
    enum class Setting {
        Remove, RowsDown, RowsUp, SameNameOn, SameNameOff, AdapterDongle, AdapterHome,
        Camera0, Camera1, Camera2, Camera3, FullSizeOn,      // T10 Headset camera (R3: Full res On | Off)
        FpsDown, FpsUp,                                      // T10 both camera widgets
        CompactOn, CompactOff,                               // R2 Frame, Power, Battery, Temp
        HideSystemOn, HideSystemOff,                         // R2 process lists (third row)
        FullSizeOff,                                         // R3 Headset camera
        AddressEdit, RotationDown, RotationUp,               // HTTP camera (Full res: FullSizeOn / Off)
        SizeDown, SizeUp
    };

    /** HTTP camera: Edit was clicked next to the address. The window opens the address dialog. */
    struct AddressEdit {
        WidgetRef ref;         ///< the widget; the dialog's address applies only if it's still there,
        uint32_t version = 0;  ///< in this layout version
        std::string url;       ///< its address now
        bool valid() const { return ref.valid(); }
    };

    /** Where things are this frame. */
    struct View {
        Box layoutCard;    ///< the left card (title row + lanes)
        Box paletteCard;   ///< the right card
        Box viewport;      ///< the lanes' area inside layoutCard
        double originX = 0;  ///< panel px -> device px: origin + p × scale
        double originY = 0;
        double scale = 1.0;
        double ui = 1.0;     ///< DPI scale for text and controls
    };

    /** @return the layout card's height for the title row (device px), so the window can place the viewport */
    static double headerHeight(double ui);
    /** @return the palette card's width for this DPI scale */
    static double paletteWidth(double ui);

    /** Takes the current layout + its geometry (before every frame and event). @return true if it changed */
    bool sync(const Layout& layout, const GridGeometry& geometry);
    void setView(const View& view);

    /** @return the lanes in device px (one per column, plus the New column lane when there's room) */
    const std::vector<Box>& lanes() const { return lanes_; }
    /** @return true if lane i is the New column lane */
    bool isNewColumnLane(size_t i) const { return i >= layout_.columns.size(); }

    /** Drawn before the panel: the layout card, its title + hint / status, the lanes. */
    void drawUnderPanel(const Pen& pen) const;
    /** Drawn after the panel: selection, the lifted card, the drop marker, the palette, the ghost. */
    void drawOverPanel(const Pen& pen) const;

    /** @return true if a redraw is needed */
    bool pointerDown(double x, double y);
    bool pointerMove(double x, double y);
    bool pointerUp(double x, double y);
    /** Cancels a press or drag (Esc, capture lost). */
    bool cancel();
    bool busy() const { return press_ != Press::None; }
    /**
     * @return true once after sync() cancelled a press or drag because the layout version moved under it (an edit in
     *         VR): the window releases its mouse capture and says the move wasn't applied
     */
    bool takePressCancelled();
    bool dragging() const { return dragging_; }

    bool takeCommit(uint32_t& baseVersion, Layout& layout);
    void commitResult(CommitOutcome outcome);
    /** HTTP camera: @return false if Edit wasn't clicked since the last call */
    bool takeAddressEdit(AddressEdit& out);

    /** @return the selected widget (invalid = none) */
    WidgetRef selected() const { return selected_; }
    /** Selects a card (right-click opens its options too). */
    void select(WidgetRef ref);

    // ---- for tests / the window ----
    const std::vector<std::pair<WidgetType, Box>>& chips() const { return chips_; }
    const std::vector<std::pair<Setting, Box>>& settingButtons() const { return settings_; }
    /** @return the slot index under (x, y) in device px, or -1 */
    int cardAt(double x, double y) const;

private:
    enum class Press { None, Card, Chip, Lanes };

    Layout layout_;
    GridGeometry geometry_;
    View view_;
    std::vector<Box> lanes_;
    std::vector<std::pair<WidgetType, Box>> chips_;
    std::vector<std::pair<Setting, Box>> settings_;

    Press press_ = Press::None;
    WidgetRef pressedCard_;
    WidgetType pressedChip_ = WidgetType::None;
    uint32_t pressVersion_ = 0;
    double pressX_ = 0, pressY_ = 0;
    double pointerX_ = -1, pointerY_ = -1;
    bool dragging_ = false;
    int dropColumn_ = -1;
    int dropSlot_ = 0;
    bool overPalette_ = false;

    WidgetRef selected_;
    WidgetInstance selectedInstance_;  ///< what selected_ pointed at: re-found after a sync, so an edit made elsewhere
                                       ///< can't leave the selection on a different widget
    bool pressCancelled_ = false;

    bool commitQueued_ = false;
    uint32_t commitBase_ = 0;
    Layout commitLayout_;
    int status_ = -1;  ///< -1 none, else a CommitOutcome
    AddressEdit addressEdit_;  ///< HTTP camera: Edit clicked, not taken yet

    void relayout();
    void revalidateSelection();
    Box slotBox(const WidgetSlot& slot) const;
    Box cardBox(WidgetRef ref) const;
    void updateDrop(double x, double y);
    void queueCommit(const Layout& edited, WidgetRef selectAfter);
    void applySetting(Setting setting);
    int chipAt(double x, double y) const;
    int settingAt(double x, double y) const;
};

}  // namespace pc
}  // namespace fph
