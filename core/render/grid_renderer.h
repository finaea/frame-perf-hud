// Draws a Layout into one panel image: the columns side by side, each widget at its natural height.
// - The static layer (background, cards, graph beds, titles) is rebuilt when layout or visible row states change.
// - The panel is the bounding box of the columns, capped at kMaxTextureSide per side; past that the whole
//   panel is scaled down to fit.
// - render() reports whether any pixel changed, so the caller uploads only then.
#pragma once

#include "draw.h"
#include "layout.h"
#include "widgets.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fph {

/** Largest texture side (px). */
constexpr int kMaxTextureSide = 2048;

/** Where one widget instance sits, in unscaled panel px. */
struct WidgetSlot {
    WidgetInstance widget;
    int column = 0;
    int x = 0;  ///< card left
    int y = 0;  ///< card top
    int w = 0;
    int h = 0;
};

/** The laid-out grid. */
struct GridGeometry {
    int width = 0;   ///< unscaled panel width (px): columns × kColumnWidth
    int height = 0;  ///< unscaled panel height (px): tallest column + padding
    double scale = 1.0;      ///< ≤ 1: applied so the texture fits kMaxTextureSide
    int textureWidth = 0;    ///< ceil(width × scale)
    int textureHeight = 0;
    int columns = 0;
    std::vector<WidgetSlot> slots;
};

/**
 * Lays a layout out. An empty layout gets a one-column, minimal-height panel.
 * @param layout the layout
 * @return the geometry
 */
GridGeometry layoutGrid(const Layout& layout, const WidgetContext* ctx = nullptr);
bool sameGridGeometry(const GridGeometry& a, const GridGeometry& b);

class GridRenderer {
public:
    /** @param fonts fonts (must outlive this object) */
    explicit GridRenderer(const FontSet& fonts);
    ~GridRenderer();
    GridRenderer(const GridRenderer&) = delete;
    GridRenderer& operator=(const GridRenderer&) = delete;

    /**
     * Sets the layout. Rebuilds the geometry and the static layer only if the widgets changed; graphs of
     * instances that keep their type and position survive.
     * @return true if the geometry (and so possibly the texture size) changed
     */
    bool setLayout(const Layout& layout);
    /** Reflows data-dependent card heights. True only if slots or texture dimensions changed. */
    bool updateGeometry(const WidgetContext& ctx);

    /** Forces a static-layer rebuild on the next render (fonts or theme changed). */
    void invalidateStatic() { staticDirty_ = true; }

    /** Redraws every widget on the next render even if its inputs didn't change (benchmarks). */
    void markAllDirty() { allDirty_ = true; }

    /**
     * Draws the widgets whose inputs changed since they were last drawn (widgetSignature); the others keep
     * their pixels. A graph only moves when its metric gets a new sample.
     * @param ctx values
     * @return true if any pixel may have changed (upload it): a widget was redrawn or the background was
     *         repainted (first render, layout / static change, markAllDirty); false if nothing was touched
     */
    bool render(const WidgetContext& ctx);

    /** @return the last render: cairo's premultiplied ARGB32 (B, G, R, A bytes), stride textureWidth × 4 */
    const uint8_t* pixels() const;

    /** @return widgets redrawn by the last render (tests, debug dump) */
    size_t lastRedrawn() const { return lastRedrawn_; }

    /** @return the last render as non-premultiplied RGBA, textureWidth × textureHeight × 4 bytes */
    const std::vector<uint8_t>& toRgba();

    /** Writes the last render to a PNG. @return true on success */
    bool writePng(const std::string& path) const;

    /** @return the current geometry */
    const GridGeometry& geometry() const { return geometry_; }

    /** @return how many times the static layer was built (tests) */
    uint64_t staticBuilds() const { return staticBuilds_; }

private:
    const FontSet& fonts_;
    Layout layout_;
    bool haveLayout_ = false;
    GridGeometry geometry_;
    std::vector<WidgetHistory> histories_;  ///< one per slot
    std::vector<uint64_t> signatures_;      ///< one per slot: inputs of its last draw
    std::vector<bool> drawn_;               ///< one per slot: drawn since the static layer was painted
    bool allDirty_ = true;
    size_t lastRedrawn_ = 0;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    cairo_surface_t* static_ = nullptr;
    bool staticDirty_ = true;
    uint64_t staticBuilds_ = 0;
    std::vector<uint8_t> rgba_;

    void ensureSurfaces();
    void buildStatic();
    void applyGeometry(const GridGeometry& next);
};

}  // namespace fph
