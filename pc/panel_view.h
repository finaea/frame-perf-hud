// The widget panel drawn at the window's real pixel scale. GridRenderer draws at panel size (the headset's texture)
// and stretching that on a 125% / 4K monitor looks soft, so the PC draws the same widgets (core's widget functions,
// same geometry) as vectors at `scale` device px per panel px. Like GridRenderer it keeps a static layer and only
// redraws a widget whose inputs changed (widgetSignature).
#pragma once

#include "draw.h"
#include "grid_renderer.h"
#include "layout.h"
#include "widgets.h"

#include <cairo.h>

#include <cstdint>
#include <vector>

namespace fph {
namespace pc {

class PanelView {
public:
    explicit PanelView(const FontSet& fonts) : fonts_(fonts) {}
    ~PanelView();
    PanelView(const PanelView&) = delete;
    PanelView& operator=(const PanelView&) = delete;

    /** Sets the layout; graphs of widgets that keep their type and slot survive. */
    void setLayout(const Layout& layout);

    /** @return the unscaled geometry (panel px), as layoutGrid gives it */
    const GridGeometry& geometry() const { return geometry_; }

    /**
     * Draws the panel at `scale` device px per panel px (redrawing only what changed).
     * @return the picture, ceil(width × scale) × ceil(height × scale), owned by this object
     */
    cairo_surface_t* render(const WidgetContext& ctx, double scale);

    /** @return widgets redrawn by the last render (tests) */
    size_t lastRedrawn() const { return lastRedrawn_; }

private:
    const FontSet& fonts_;
    Layout layout_;
    bool haveLayout_ = false;
    GridGeometry geometry_;
    std::vector<WidgetHistory> histories_;
    std::vector<uint64_t> signatures_;
    std::vector<bool> drawn_;    ///< drawn on the current static layer
    std::vector<bool> sampled_;  ///< has pushed a graph point for signatures_
    cairo_surface_t* surface_ = nullptr;
    cairo_surface_t* static_ = nullptr;
    double scale_ = 0.0;
    bool staticDirty_ = true;
    size_t lastRedrawn_ = 0;

    void dropSurfaces();
};

}  // namespace pc
}  // namespace fph
