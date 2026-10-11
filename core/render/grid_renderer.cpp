#include "grid_renderer.h"

#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fph {

namespace {

constexpr int kPanelPadY = 8;  ///< above the first card and below the last
constexpr int kCardInsetX = 8;  ///< card inside its column (upstream's kCardInsetX)
constexpr int kCardGap = 12;   ///< between cards (their shadows reach ~5 px)
constexpr int kEmptyHeight = 48;

void setFontOptions(cairo_t* cr) {
    // VR pixels have no fixed subpixel order, so no subpixel AA (upstream's settings)
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

}  // namespace

GridGeometry layoutGrid(const Layout& layout) {
    GridGeometry g;
    g.columns = std::max<int>(1, static_cast<int>(layout.columns.size()));
    g.width = g.columns * kColumnWidth;
    int tallest = 0;
    for (size_t c = 0; c < layout.columns.size(); ++c) {
        int y = kPanelPadY;
        bool first = true;
        for (const WidgetInstance& w : layout.columns[c].widgets) {
            const int h = widgetHeight(w);
            if (h <= 0) continue;
            if (!first) y += kCardGap;
            first = false;
            WidgetSlot slot;
            slot.widget = w;
            slot.column = static_cast<int>(c);
            slot.x = static_cast<int>(c) * kColumnWidth + kCardInsetX;
            slot.y = y;
            slot.w = kColumnWidth - 2 * kCardInsetX;
            slot.h = h;
            g.slots.push_back(slot);
            y += h;
        }
        tallest = std::max(tallest, y - kPanelPadY);
    }
    g.height = tallest > 0 ? tallest + 2 * kPanelPadY : kEmptyHeight;
    g.scale = std::min({1.0, static_cast<double>(kMaxTextureSide) / g.width,
                        static_cast<double>(kMaxTextureSide) / g.height});
    g.textureWidth = std::min(kMaxTextureSide, static_cast<int>(std::ceil(g.width * g.scale - 1e-9)));
    g.textureHeight = std::min(kMaxTextureSide, static_cast<int>(std::ceil(g.height * g.scale - 1e-9)));
    return g;
}

GridRenderer::GridRenderer(const FontSet& fonts) : fonts_(fonts) {}

GridRenderer::~GridRenderer() {
    if (cr_ != nullptr) cairo_destroy(cr_);
    if (surface_ != nullptr) cairo_surface_destroy(surface_);
    if (static_ != nullptr) cairo_surface_destroy(static_);
}

bool GridRenderer::setLayout(const Layout& layout) {
    if (haveLayout_ && layout.sameWidgets(layout_)) {
        layout_.version = layout.version;
        return false;
    }
    const GridGeometry next = layoutGrid(layout);
    // Keep a graph where the same type sits in the same slot index (a settings tweak shouldn't wipe it)
    std::vector<WidgetHistory> histories(next.slots.size());
    for (size_t k = 0; k < next.slots.size() && k < geometry_.slots.size(); ++k) {
        if (next.slots[k].widget.type == geometry_.slots[k].widget.type) histories[k] = std::move(histories_[k]);
    }
    const bool sizeChanged = next.textureWidth != geometry_.textureWidth || next.textureHeight != geometry_.textureHeight;
    layout_ = layout;
    haveLayout_ = true;
    geometry_ = next;
    histories_ = std::move(histories);
    signatures_.assign(geometry_.slots.size(), 0);
    drawn_.assign(geometry_.slots.size(), false);
    staticDirty_ = true;
    if (sizeChanged) {
        if (cr_ != nullptr) cairo_destroy(cr_);
        if (surface_ != nullptr) cairo_surface_destroy(surface_);
        if (static_ != nullptr) cairo_surface_destroy(static_);
        cr_ = nullptr;
        surface_ = nullptr;
        static_ = nullptr;
    }
    return true;
}

void GridRenderer::ensureSurfaces() {
    if (!haveLayout_) setLayout(defaultLayout());
    if (surface_ != nullptr) return;
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, geometry_.textureWidth, geometry_.textureHeight);
    cr_ = cairo_create(surface_);
    setFontOptions(cr_);
    static_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, geometry_.textureWidth, geometry_.textureHeight);
    staticDirty_ = true;
}

void GridRenderer::buildStatic() {
    cairo_t* cr = cairo_create(static_);
    setFontOptions(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, geometry_.scale, geometry_.scale);
    const Pen pen {cr, &fonts_};
    // Opaque background (contrast is checked on an opaque one; the overlay's alpha fades the whole panel)
    pen.color(kBg);
    pen.roundedRect(0, 0, geometry_.width, geometry_.height, 16);
    cairo_fill(cr);
    for (const WidgetSlot& slot : geometry_.slots) drawWidgetStatic(pen, slot.x, slot.y, slot.widget);
    cairo_destroy(cr);
    cairo_surface_flush(static_);
    staticDirty_ = false;
    ++staticBuilds_;
}

bool GridRenderer::render(const WidgetContext& ctx) {
    ensureSurfaces();
    bool full = allDirty_;
    if (staticDirty_) {
        buildStatic();
        full = true;
    }
    allDirty_ = false;
    if (full) {
        // Background and every card from the static layer; then every widget below
        cairo_save(cr_);
        cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr_, static_, 0, 0);
        cairo_paint(cr_);
        cairo_restore(cr_);
        std::fill(drawn_.begin(), drawn_.end(), false);
    }
    lastRedrawn_ = 0;
    const Pen pen {cr_, &fonts_};
    for (size_t k = 0; k < geometry_.slots.size(); ++k) {
        const WidgetSlot& slot = geometry_.slots[k];
        const uint64_t signature = widgetSignature(slot.widget, ctx);
        if (drawn_[k] && signature == signatures_[k]) continue;  // same inputs → same pixels
        signatures_[k] = signature;
        drawn_[k] = true;
        ++lastRedrawn_;
        pushWidgetHistory(slot.widget, ctx, histories_[k]);
        cairo_save(cr_);
        cairo_scale(cr_, geometry_.scale, geometry_.scale);
        // Only this card: put its static pixels back, then draw its values
        cairo_rectangle(cr_, slot.x - 1, slot.y - 1, slot.w + 2, slot.h + 2);
        cairo_clip(cr_);
        if (!full) {
            cairo_save(cr_);
            cairo_identity_matrix(cr_);
            cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_surface(cr_, static_, 0, 0);
            cairo_paint(cr_);
            cairo_restore(cr_);
        }
        drawWidgetDynamic(pen, slot.x, slot.y, slot.widget, ctx, histories_[k]);
        cairo_restore(cr_);
    }
    cairo_surface_flush(surface_);
    // A full repaint changed pixels even when no widget was drawn (e.g. the last widget was removed)
    return full || lastRedrawn_ > 0;
}

const uint8_t* GridRenderer::pixels() const {
    return surface_ != nullptr ? cairo_image_surface_get_data(surface_) : nullptr;
}

const std::vector<uint8_t>& GridRenderer::toRgba() {
    ensureSurfaces();
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool GridRenderer::writePng(const std::string& path) const {
    return surface_ != nullptr && cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

}  // namespace fph
