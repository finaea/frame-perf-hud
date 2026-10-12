#include "panel_view.h"

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace fph {
namespace pc {

namespace {

void setFontOptions(cairo_t* cr) {
    // Same as the headset's grid (grey AA, slight hinting), so both surfaces look alike
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

}  // namespace

PanelView::~PanelView() {
    dropSurfaces();
}

void PanelView::dropSurfaces() {
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    if (static_ != nullptr) cairo_surface_destroy(static_);
    surface_ = nullptr;
    static_ = nullptr;
}

void PanelView::setLayout(const Layout& layout) {
    if (haveLayout_ && layout.sameWidgets(layout_)) {
        layout_.version = layout.version;
        return;
    }
    const GridGeometry next = layoutGrid(layout);
    std::vector<WidgetHistory> histories(next.slots.size());
    for (size_t k = 0; k < next.slots.size() && k < geometry_.slots.size(); ++k) {
        if (next.slots[k].widget.type == geometry_.slots[k].widget.type) histories[k] = std::move(histories_[k]);
    }
    layout_ = layout;
    haveLayout_ = true;
    geometry_ = next;
    histories_ = std::move(histories);
    signatures_.assign(geometry_.slots.size(), 0);
    drawn_.assign(geometry_.slots.size(), false);
    sampled_.assign(geometry_.slots.size(), false);
    dropSurfaces();
    staticDirty_ = true;
}

cairo_surface_t* PanelView::render(const WidgetContext& ctx, double scale) {
    if (!haveLayout_) setLayout(defaultLayout());
    scale = std::max(0.05, scale);
    const int w = std::max(1, static_cast<int>(std::ceil(geometry_.width * scale)));
    const int h = std::max(1, static_cast<int>(std::ceil(geometry_.height * scale)));
    if (surface_ == nullptr || std::fabs(scale - scale_) > 1e-6 || cairo_image_surface_get_width(surface_) != w ||
        cairo_image_surface_get_height(surface_) != h) {
        dropSurfaces();
        surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        static_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        scale_ = scale;
        staticDirty_ = true;
    }
    cairo_t* cr = cairo_create(surface_);
    setFontOptions(cr);
    const Pen pen {cr, &fonts_};
    bool full = false;
    if (staticDirty_) {
        cairo_t* sc = cairo_create(static_);
        setFontOptions(sc);
        cairo_set_operator(sc, CAIRO_OPERATOR_CLEAR);
        cairo_paint(sc);
        cairo_set_operator(sc, CAIRO_OPERATOR_OVER);
        cairo_scale(sc, scale, scale);
        const Pen spen {sc, &fonts_};
        spen.color(kBg);
        spen.roundedRect(0, 0, geometry_.width, geometry_.height, 16);
        cairo_fill(sc);
        for (const WidgetSlot& slot : geometry_.slots) drawWidgetStatic(spen, slot.x, slot.y, slot.widget);
        cairo_destroy(sc);
        cairo_surface_flush(static_);
        staticDirty_ = false;
        full = true;
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, static_, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        std::fill(drawn_.begin(), drawn_.end(), false);
    }
    lastRedrawn_ = 0;
    for (size_t k = 0; k < geometry_.slots.size(); ++k) {
        const WidgetSlot& slot = geometry_.slots[k];
        const uint64_t signature = widgetSignature(slot.widget, ctx);
        const bool newInputs = !sampled_[k] || signature != signatures_[k];
        if (drawn_[k] && !newInputs) continue;
        // A rescale redraws everything, but only new inputs add a graph point
        if (newInputs) pushWidgetHistory(slot.widget, ctx, histories_[k]);
        signatures_[k] = signature;
        sampled_[k] = true;
        drawn_[k] = true;
        ++lastRedrawn_;
        cairo_save(cr);
        cairo_scale(cr, scale, scale);
        cairo_rectangle(cr, slot.x - 1, slot.y - 1, slot.w + 2, slot.h + 2);
        cairo_clip(cr);
        if (!full) {
            cairo_save(cr);
            cairo_identity_matrix(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_surface(cr, static_, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        drawWidgetDynamic(pen, slot.x, slot.y, slot.widget, ctx, histories_[k]);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface_);
    return surface_;
}

}  // namespace pc
}  // namespace fph
