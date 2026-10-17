// R3: offline pictures of the PC window's Profiles and Logs tabs (made-up headset state, never a live one), for
// review without starting the app (whose single-instance lock would only raise the running window).
// Usage: fph-pc-pages-preview OUT_DIR   (writes pc-tab-layout.png, pc-tab-profiles.png, pc-tab-logs.png)
#include "layout.h"
#include "panel_view.h"
#include "pc_pages.h"
#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::create_directories(std::filesystem::u8path(argv[1]));
    FontSet fonts;
    fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf", FPH_FONT_DIR "/NotoSans-Bold.ttf");
    using namespace fph;
    using namespace fph::pc;
    link::ControlState h;
    h.revision = 7;
    h.layoutVersion = 112;
    h.flags = link::kControlUnsaved | link::kControlRecording;
    h.maxProfiles = 8;
    h.profiles = {{"profile-1", "test"}, {"profile-2", "PC streaming"}, {"profile-3", "Minimal"}};
    h.activeId = "profile-1";
    h.status = "Renamed to test";
    h.recordStatus = "Recording 3:12 to saves/2026-10-17_170509_VRChat_record.csv";
    h.powerOff = "Normal shutdown";
    PagesView view;
    view.headset = &h;
    view.subscribed = true;
    view.headsetMessage = "Renamed to test";
    view.pcMessage = "Saved the last 10 min to saves\\2026-10-17_170812_VRChat_save";
    view.lastCard = {"VRChat · 1 h 05 min", "fps avg 71.8 · 12 dropped", "Power avg 9.4 W · peak 12.1 W",
                     "Battery 82% to 61% (-21 points)", "Peak CPU 71 °C · GPU 68 °C · battery 39 °C"};
    view.dataDir = "C:\\Users\\final\\AppData\\Local\\frame-perf-hud";
    const double s = 1.25;  // 120 DPI, Jack's window
    const int w = static_cast<int>(960 * s), hgt = static_cast<int>(640 * s);
    for (const PcTab tab : {PcTab::Layout, PcTab::Profiles, PcTab::Logs}) {
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, hgt);
        cairo_t* cr = cairo_create(surface);
        Pen pen {cr, &fonts};
        pen.color(kBg);
        cairo_paint(cr);
        PcPages pages;
        pages.setTab(tab);
        const double barH = 30 * s, pad = 8 * s;
        pages.drawTabs(pen, pad, barH, s);
        const double top = barH + PcPages::tabBarHeight(s) + pad * 0.5;
        if (tab == PcTab::Layout) {  // the existing panel (no values: N/A), as the window draws it
            MetricStore store;
            WidgetContext ctx;
            ctx.store = &store;
            PanelView panel(fonts);
            panel.setLayout(defaultLayout());
            panel.updateGeometry(ctx);
            const GridGeometry& g = panel.geometry();
            const double scale = std::min({(w - 2 * pad) / g.width, (hgt - top - pad) / g.height, 2.0 * s});
            cairo_set_source_surface(cr, panel.render(ctx, scale), pad + std::max(0.0, (w - 2 * pad - g.width * scale) / 2), top);
            cairo_paint(cr);
        } else {
            pages.drawPage(pen, {pad, top, w - 2 * pad, hgt - top - pad}, s, view);
        }
        cairo_surface_flush(surface);
        const std::string path = std::string(argv[1]) + (tab == PcTab::Layout     ? "/pc-tab-layout.png"
                                                         : tab == PcTab::Profiles ? "/pc-tab-profiles.png"
                                                                                  : "/pc-tab-logs.png");
        cairo_surface_write_to_png(surface, path.c_str());
        std::printf("wrote %s\n", path.c_str());
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
    // The status bar at 100% and 200%: the real Save button (drawStatusSaveButton), and lane G's "Update available"
    // pill drawn by its rule in app_window.cpp (label width + 24 px, 6 px left of Save) to check they don't overlap
    for (const double scale : {1.0, 2.0}) {
        const int bw = static_cast<int>(960 * scale), bh = static_cast<int>(30 * scale);
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
        cairo_t* cr = cairo_create(surface);
        Pen pen {cr, &fonts};
        pen.color(kBg);
        cairo_paint(cr);
        const PageBox save = drawStatusSaveButton(pen, bw, bh, scale);
        const std::string update = "Update available (0.2.0)";
        const double uw = pen.measure(update, 12.5 * scale, true) + 24 * scale;
        const double ux = save.x - uw - 6 * scale;
        pen.color(blendColor(kAccent, kBg, 0.25));
        pen.roundedRect(ux, save.y, uw, save.h, save.h / 2);
        cairo_fill(cr);
        strokeRounded(pen, ux, save.y, uw, save.h, save.h / 2, kAccent, 1.5 * scale);
        textCentered(pen, ux + uw / 2, centerBaseline(save.y, save.h, 12.5 * scale), update, 12.5 * scale, kAccentText,
                     true);
        drawDisc(cr, 14 * scale, bh / 2.0, 4.5 * scale, kSuccess);
        const std::string status = "Connected to 'Frame'  ·  right-click for options";
        const double size = fitSize(pen, status, 13.5 * scale, 9 * scale, ux - 32 * scale - 12 * scale, false);
        pen.text(26 * scale, centerBaseline(0, bh, size), status, size, kTextMuted);
        cairo_surface_flush(surface);
        const std::string path = std::string(argv[1]) + (scale == 1.0 ? "/pc-status-100.png" : "/pc-status-200.png");
        cairo_surface_write_to_png(surface, path.c_str());
        std::printf("wrote %s (Save at x %.0f-%.0f, update pill at x %.0f-%.0f)\n", path.c_str(), save.x,
                    save.x + save.w, ux, ux + uw);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
    return 0;
}
