// The widgets. Drawing helpers (levels, graphs, badges, icons) are upstream's panel.cpp ones, moved here.
#define _USE_MATH_DEFINES  // M_PI on MSVC
#include "widgets.h"

#include "theme.h"
#include "widget_text.h"
#include "stream_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace fph {

namespace {

using namespace metric;

// ---- Card geometry (relative to the card's top-left; the card sits 8 px inside its 512 px column) ----
constexpr double kCardW = kColumnWidth - 16;  // 496
constexpr double kInX = 6;                    // content left = upstream's 14 px panel padding
constexpr double kInR = kCardW - 6;           // content right
constexpr double kTop = 6;                    // upstream's "row top" sits 6 px below the card top
constexpr double kGraphX = 202;               // upstream: kPad + kLeftWidth + 8 = 210 from the panel edge
constexpr double kGraphW = 288;
constexpr double kGraphH = 84;
constexpr double kRowH = 20;                  // compact rows (power rails, battery, lists)
constexpr double kLineH = 22;                 // text rows (temps, general)

// Heights (card)
constexpr int kGraphCardH = 102;  // upstream row: 92 + 6 above + 4 below
constexpr int kPowerRails = 12;
constexpr int kBatteryRows = 10;

/** Value state. */
enum class Level { Normal, Warn, Crit };

Level levelOf(double value, double warn, double crit, bool lowerIsWorse = false) {
    if (std::isnan(value)) return Level::Normal;
    if (lowerIsWorse) {
        if (value <= crit) return Level::Crit;
        if (value <= warn) return Level::Warn;
        return Level::Normal;
    }
    if (value >= crit) return Level::Crit;
    if (value >= warn) return Level::Warn;
    return Level::Normal;
}

Color colorOf(Level level, Color normal = kText) {
    switch (level) {
        case Level::Warn: return kWarn;
        case Level::Crit: return kDanger;
        default: return normal;
    }
}

Level worse(Level a, Level b) {
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

/** @return the value with the given decimals, "--" for NaN */
std::string num(double value, int decimals) {
    if (std::isnan(value)) return "--";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

}  // namespace

RateText formatRate(double mbps) {
    if (std::isnan(mbps)) return {"--", " Mbps"};
    const long kbps = std::lround(std::max(0.0, mbps) * 1000.0 + 1e-6);  // + 1e-6: 0.9995 is stored as 0.99949999…
    if (kbps < 1000) return {std::to_string(kbps), " kbps"};  // 0.024 Mbps reads "24 kbps", not "0.0"
    return {num(mbps, mbps >= 100 ? 0 : 1), " Mbps"};
}

namespace {

/** @return the value rounded with thousands separators, "--" for NaN */
std::string grouped(double value) {
    if (std::isnan(value)) return "--";
    std::string digits = std::to_string(static_cast<long long>(std::lround(value)));
    const size_t start = digits[0] == '-' ? 1 : 0;
    for (int i = static_cast<int>(digits.size()) - 3; i > static_cast<int>(start); i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

// ---- Store access ----

const MetricValue* value(const WidgetContext& ctx, uint16_t id) {
    return ctx.store != nullptr ? ctx.store->get(id) : nullptr;
}

bool ok(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v != nullptr && v->ok;
}

/** @return the f32 value, NaN if N/A */
double f(const WidgetContext& ctx, uint16_t id) {
    return ctx.store != nullptr ? ctx.store->f32Or(id, std::nan("")) : std::nan("");
}

/** @return the i32 / enum value, fallback if N/A */
int64_t i(const WidgetContext& ctx, uint16_t id, int64_t fallback = 0) {
    return ctx.store != nullptr ? ctx.store->intOr(id, fallback) : fallback;
}

/** @return the short string, "" if N/A */
std::string text(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v != nullptr && v->ok ? v->text : std::string();
}

Reason reasonOf(const WidgetContext& ctx, uint16_t id) {
    const MetricValue* v = value(ctx, id);
    return v == nullptr ? Reason::SensorMissing : v->reason;
}

/** "N/A: <reason>" */
std::string naText(const WidgetContext& ctx, uint16_t id) {
    return std::string(widgetText().naPrefix) + reasonText(reasonOf(ctx, id));
}

/** Cuts text to fit maxWidth, ending in ".." (UTF-8 safe). */
std::string ellipsize(const Pen& pen, const std::string& s, double size, double maxWidth, bool bold = false) {
    if (pen.measure(s, size, bold) <= maxWidth) return s;
    std::string cut = s;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80) != 0) cut.pop_back();
        if (pen.measure(cut + "..", size, bold) <= maxWidth) return cut + "..";
    }
    return "..";
}

// ---- Graphs (upstream panel.cpp) ----

struct GraphArea {
    double x, y, w, h;
    double minV, maxV;
    double now;
    double seconds;

    double xOf(double t) const { return x + w - (now - t) / seconds * w; }
    double yOf(double v) const {
        const double ratio = (std::clamp(v, minV, maxV) - minV) / (maxV - minV);
        return y + h - ratio * h;
    }
};

GraphArea graphArea(double x, double y, double minV, double maxV, const WidgetContext& ctx) {
    return GraphArea {x + kGraphX, y + kTop + 4, kGraphW, kGraphH, minV, maxV, ctx.now,
                      static_cast<double>(std::max(1, ctx.graphSeconds))};
}

void drawGraphLabel(const Pen& pen, const GraphArea& area, const std::string& topLabel) {
    pen.text(area.x + 6, area.y + 14, topLabel, 12, kTextMuted);
}

void drawGuide(const Pen& pen, const GraphArea& area, double v, Color c) {
    if (std::isnan(v) || v <= area.minV || v >= area.maxV) return;
    const double y = std::round(area.yOf(v)) + 0.5;
    const double dashes[] = {4.0, 4.0};
    cairo_save(pen.cr);
    cairo_set_dash(pen.cr, dashes, 2, 0);
    cairo_set_line_width(pen.cr, 1.0);
    pen.color(c);
    cairo_move_to(pen.cr, area.x + 4, y);
    cairo_line_to(pen.cr, area.x + area.w - 4, y);
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

void drawSeries(const Pen& pen, const GraphArea& area, const History& history, Color c) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    cairo_set_line_width(pen.cr, 2.0);
    cairo_set_line_join(pen.cr, CAIRO_LINE_JOIN_ROUND);
    pen.color(c);
    bool drawing = false;
    for (const auto& p : history.points()) {
        if (std::isnan(p.v)) {
            drawing = false;
            continue;
        }
        const double px = area.xOf(p.t);
        const double py = area.yOf(p.v);
        if (drawing) {
            cairo_line_to(pen.cr, px, py);
        } else {
            cairo_move_to(pen.cr, px, py);
            drawing = true;
        }
    }
    cairo_stroke(pen.cr);
    cairo_restore(pen.cr);
}

void drawSeriesFill(const Pen& pen, const GraphArea& area, const History& history, Color c, double alpha) {
    cairo_save(pen.cr);
    pen.roundedRect(area.x, area.y, area.w, area.h, 6);
    cairo_clip(pen.cr);
    pen.color(c, alpha);
    const double baseY = area.y + area.h;
    bool open = false;
    double lastX = 0;
    for (const auto& p : history.points()) {
        const double px = area.xOf(p.t);
        if (std::isnan(p.v)) {
            if (open) {
                cairo_line_to(pen.cr, lastX, baseY);
                cairo_close_path(pen.cr);
                open = false;
            }
            continue;
        }
        if (!open) {
            cairo_move_to(pen.cr, px, baseY);
            open = true;
        }
        cairo_line_to(pen.cr, px, area.yOf(p.v));
        lastX = px;
    }
    if (open) {
        cairo_line_to(pen.cr, lastX, baseY);
        cairo_close_path(pen.cr);
    }
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
}

void drawBadge(const Pen& pen, double right, double top, const std::string& s, Color c, Color textColor = kOnAccent) {
    const double size = 14;
    const double width = pen.measure(s, size, true) + 16;
    pen.color(c, 0.95);
    pen.roundedRect(right - width, top, width, 22, 6);
    cairo_fill(pen.cr);
    pen.text(right - width + 8, top + 16, s, size, textColor, true);
}

/** Small up / down arrow (the bundled Noto Sans has no arrow glyphs). @return width */
double drawArrow(const Pen& pen, double x, double baseline, bool up, Color c) {
    const double h = 11;
    const double top = baseline - h;
    const double cx = x + 4.5;
    pen.color(c);
    cairo_save(pen.cr);
    cairo_set_line_width(pen.cr, 1.6);
    cairo_move_to(pen.cr, cx, top + (up ? 3 : 0));
    cairo_line_to(pen.cr, cx, baseline - (up ? 0 : 3));
    cairo_stroke(pen.cr);
    const double tipY = up ? top : baseline;
    const double backY = up ? top + 4.5 : baseline - 4.5;
    cairo_move_to(pen.cr, cx, tipY);
    cairo_line_to(pen.cr, cx - 4, backY);
    cairo_line_to(pen.cr, cx + 4, backY);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    cairo_restore(pen.cr);
    return 11;
}

/** Charging bolt. @return width */
double drawBolt(const Pen& pen, double x, double baseline, Color c) {
    const double h = 13;
    const double top = baseline - h + 1;
    pen.color(c);
    cairo_move_to(pen.cr, x + 6, top);
    cairo_line_to(pen.cr, x + 1, top + h * 0.58);
    cairo_line_to(pen.cr, x + 4.5, top + h * 0.58);
    cairo_line_to(pen.cr, x + 3, top + h);
    cairo_line_to(pen.cr, x + 8.5, top + h * 0.38);
    cairo_line_to(pen.cr, x + 5, top + h * 0.38);
    cairo_close_path(pen.cr);
    cairo_fill(pen.cr);
    return 10;
}

constexpr double kWifiFullBarsDbm = -60.0;

int wifiBars(double dbm, const Thresholds& th) {
    if (std::isnan(dbm)) return 0;
    if (dbm > kWifiFullBarsDbm) return 4;
    if (dbm > th.wifiWarnDbm) return 3;
    if (dbm > th.wifiCritDbm) return 2;
    return 1;
}

double drawWifiIcon(const Pen& pen, double x, double baseline, int bars, Color c, bool crossed) {
    const double width = 26;
    const double cx = x + width / 2;
    const double cy = baseline;
    const double radii[] = {6.0, 10.5, 15.0};
    cairo_save(pen.cr);
    cairo_set_line_cap(pen.cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(pen.cr, 2.4);
    if (bars >= 1) pen.color(c); else pen.color(kTextMuted, 0.35);
    cairo_arc(pen.cr, cx, cy - 1.5, 2.2, 0, 2 * M_PI);
    cairo_fill(pen.cr);
    for (int k = 0; k < 3; ++k) {
        if (bars >= k + 2) pen.color(c); else pen.color(kTextMuted, 0.35);
        cairo_new_sub_path(pen.cr);
        cairo_arc(pen.cr, cx, cy - 1.5, radii[k], -M_PI * 0.75, -M_PI * 0.25);
        cairo_stroke(pen.cr);
    }
    if (crossed) {
        pen.color(kTextMuted);
        cairo_set_line_width(pen.cr, 2.0);
        cairo_move_to(pen.cr, cx - 9, cy - 15);
        cairo_line_to(pen.cr, cx + 9, cy + 1);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
    return width;
}

/** "≈" drawn as two tildes (the bundled Noto Sans has no U+2248). @return width */
double drawApprox(const Pen& pen, double x, double baseline, double size, Color c) {
    const double w = size * 0.55;
    const double amp = size * 0.07;
    cairo_save(pen.cr);
    pen.color(c);
    cairo_set_line_width(pen.cr, std::max(1.2, size * 0.08));
    cairo_set_line_cap(pen.cr, CAIRO_LINE_CAP_ROUND);
    for (const double dy : {size * 0.40, size * 0.22}) {
        const double y = baseline - dy;
        cairo_move_to(pen.cr, x + 1, y + amp);
        cairo_curve_to(pen.cr, x + w * 0.35, y - 2 * amp, x + w * 0.65, y + 2 * amp, x + w - 1, y - amp);
        cairo_stroke(pen.cr);
    }
    cairo_restore(pen.cr);
    return w + 2;
}

/** Big number + unit ("8.2" + "ms"). @return width */
double drawBigValue(const Pen& pen, double x, double y, const std::string& v, const std::string& unit, Color c) {
    const double w = pen.text(x, y, v, 30, c, true);
    return w + 3 + pen.text(x + w + 3, y, unit, 16, c);
}

/** A label (muted, left) + value (right-aligned at right) row. */
void labelValue(const Pen& pen, double x, double right, double y, const std::string& label, const std::string& v,
                Color vc = kText, double size = 15) {
    pen.text(x, y, label, size, kTextMuted);
    pen.text(right, y, v, size, vc, false, true);
}

/** "1h 05m", "2d 3h" */
std::string durationText(double seconds) {
    if (std::isnan(seconds) || seconds < 0) return "--";
    const long s = static_cast<long>(seconds);
    char buffer[32];
    if (s >= 86400) {
        std::snprintf(buffer, sizeof(buffer), "%ldd %ldh", s / 86400, (s % 86400) / 3600);
    } else if (s >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%ldm %02lds", s / 60, s % 60);
    }
    return buffer;
}

/** "YYYY-MM-DD HH:MM:SS" → "YYYY-MM-DD  HH:MM" or "YYYY-MM-DD  h:MM PM" */
std::string clockText(const std::string& stamp, int format) {
    if (stamp.size() < 16) return stamp.empty() ? "--" : stamp;
    const std::string date = stamp.substr(0, 10);
    int hour = std::atoi(stamp.substr(11, 2).c_str());
    const std::string minute = stamp.substr(14, 2);
    if (format == 12) {
        const char* ampm = hour >= 12 ? "PM" : "AM";
        hour %= 12;
        if (hour == 0) hour = 12;
        return date + "  " + std::to_string(hour) + ":" + minute + " " + ampm;
    }
    return date + "  " + stamp.substr(11, 5);
}

/** "V × A = W" with N/A parts as "--" */
std::string vawText(double v, double a, double w) {
    return num(v, 2) + " V × " + num(a, 2) + " A = " + num(w, 1) + " W";
}

// ---- Static parts ----

void staticCard(const Pen& pen, double x, double y, int h) {
    drawCard(pen, x, y, kCardW, h, 12, kCard, kCard, 0);
}

void staticGraphBed(const Pen& pen, double x, double y) {
    pen.color(kBg);
    pen.roundedRect(x + kGraphX, y + kTop + 4, kGraphW, kGraphH, 6);
    cairo_fill(pen.cr);
}

void staticTitle(const Pen& pen, double x, double y, const std::string& title) {
    pen.text(x + kInX, y + kTop + 17, title, 16, kTextMuted);
}

const char* titleOf(const WidgetInstance& w) {
    const WidgetText& t = widgetText();
    switch (w.type) {
        case WidgetType::Frame: return t.frame;
        case WidgetType::Cpu: return t.cpu;
        case WidgetType::GpuRam: return t.gpuRam;
        case WidgetType::Power: return t.power;
        case WidgetType::Battery: return t.battery;
        case WidgetType::Temp: return t.temp;
        case WidgetType::CpuItems: return t.cpuItems;
        case WidgetType::RamItems: return t.ramItems;
        case WidgetType::Network: return w.adapter == NetAdapter::Home ? t.networkHome : t.networkDongle;
        case WidgetType::Streaming: return t.streaming;
        case WidgetType::General: return t.general;
        case WidgetType::HeadsetCamera: return t.headsetCamera;
        case WidgetType::EyeCamera: return t.eyeCamera;
        case WidgetType::None: break;
    }
    return "";
}

bool hasGraph(WidgetType type) {
    return type == WidgetType::Frame || type == WidgetType::GpuRam || type == WidgetType::Power ||
           type == WidgetType::Temp;
}

// ---- Widgets ----

void drawFrame(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double top = y + kTop;
    const double hz = f(ctx, kFrameDisplayHz);
    const double fps = f(ctx, kFrameAppFps);
    if (!ok(ctx, kFrameAppFps)) {
        drawBigValue(pen, cx, top + 52, "--", "fps", kTextMuted);
        pen.text(cx, top + 72, naText(ctx, kFrameAppFps), 15, kTextMuted);
    } else {
        const double targetMs = hz > 0 ? 1000.0 / hz : std::nan("");
        const Level fpsLevel = hz > 0 ? levelOf(fps, hz * th.fpsWarnRatio, hz * th.fpsCritRatio, true) : Level::Normal;
        pen.dot(cx + 4, top + 42, kAccent);
        const double bigWidth = drawBigValue(pen, cx + 14, top + 52, num(fps, 0), "fps", colorOf(fpsLevel));
        if (hz > 0) pen.text(cx + 14 + bigWidth + 6, top + 52, "/ " + num(hz, 0) + "Hz", 14, kTextMuted);
        const double gpuMs = f(ctx, kFrameGpuMs);
        const double cpuMs = f(ctx, kFrameCpuMs);
        const Level gpuLevel = levelOf(gpuMs, targetMs * th.frameWarnRatio, targetMs * th.frameCritRatio);
        const Level cpuLevel = levelOf(cpuMs, targetMs * th.frameWarnRatio, targetMs * th.frameCritRatio);
        double lx = cx + 14;
        lx += pen.text(lx, top + 72, "GPU " + num(gpuMs, 1), 15, colorOf(gpuLevel, kTextMuted));
        lx += pen.text(lx, top + 72, "  CPU " + num(cpuMs, 1), 15, colorOf(cpuLevel, kTextMuted));
        pen.text(lx, top + 72, " ms", 13, kTextMuted);
        const double reproj = f(ctx, kFrameReprojectedPct);
        const int64_t dropped = i(ctx, kFrameDropped);
        const Level reprojLevel = worse(levelOf(reproj, th.reprojWarnPct, th.reprojCritPct),
                                        dropped > 0 ? Level::Warn : Level::Normal);
        pen.text(cx + 14, top + 90, std::string(t.reproj) + num(reproj, 0) + "%  " + t.dropped + std::to_string(dropped),
                 15, colorOf(reprojLevel, kTextMuted));
    }
    // 0 fps to 25% above the refresh rate
    const double scaleHz = hz > 0 ? hz : 90.0;
    const double maxV = std::ceil(std::fmax(scaleHz, h.a.maxValue()) * 1.25 / 10.0) * 10.0;
    const GraphArea area = graphArea(x, y, 0.0, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "fps");
    drawGuide(pen, area, scaleHz, kTextMuted);
    drawSeries(pen, area, h.a, kAccent);
    const int64_t throttled = i(ctx, kFrameThrottled);
    if (ok(ctx, kFrameAppFps) && throttled > 0) {
        drawBadge(pen, area.x + area.w - 4, area.y + 4,
                  std::string(t.throttledTo) + std::to_string(throttled + 1) + t.throttledOf + num(scaleHz, 0) + "Hz",
                  kWarn);
    }
}

void drawCpu(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    // Header: total, busiest core
    const double maxCore = f(ctx, kCpuMaxCorePct);
    double hx = right;
    hx -= pen.text(hx, top + 17, num(maxCore, 0) + "%", 15,
                   colorOf(levelOf(maxCore, th.cpuWarnPct, th.cpuCritPct)), false, true);
    hx -= pen.text(hx, top + 17, std::string("  ") + t.maxCore, 14, kTextMuted, false, true);
    if (ok(ctx, kCpuUsagePct)) {
        pen.text(hx, top + 17, num(f(ctx, kCpuUsagePct), 0) + "%", 17, kText, true, true);
    } else {
        pen.text(hx, top + 17, naText(ctx, kCpuUsagePct), 14, kTextMuted, false, true);
    }
    // One bar per core (Halo-style), 0-100%
    const double barTop = top + 28;
    const double barH = 56;
    const double gap = 10;
    const double barW = (kInR - kInX - gap * (kCpuCoreSlots - 1)) / kCpuCoreSlots;
    for (uint16_t c = 0; c < kCpuCoreSlots; ++c) {
        const uint16_t id = kCpuCore0Pct + c;
        if (!ok(ctx, id) && reasonOf(ctx, id) == Reason::SensorMissing) continue;  // past the last core
        const double bx = cx + c * (barW + gap);
        pen.color(kBg);
        pen.roundedRect(bx, barTop, barW, barH, 4);
        cairo_fill(pen.cr);
        const double pct = f(ctx, id);
        if (!std::isnan(pct)) {
            const double fill = std::clamp(pct, 0.0, 100.0) / 100.0 * barH;
            pen.color(colorOf(levelOf(pct, th.cpuWarnPct, th.cpuCritPct), kAccent));
            pen.roundedRect(bx, barTop + barH - fill, barW, fill, std::min(4.0, fill / 2));
            cairo_fill(pen.cr);
        }
        textCentered(pen, bx + barW / 2, barTop + barH + 16, num(pct, 0), 13, kTextMuted, false);
    }
    // Clusters, two per row: current / max clock, "capped" under a Playtime cap
    const double half = (kInR - kInX) / 2;
    for (uint16_t k = 0; k < kCpuClusterSlots; ++k) {
        if (!ok(ctx, kCpuCluster0Mhz + k) && reasonOf(ctx, kCpuCluster0Mhz + k) == Reason::SensorMissing) continue;
        const double rx = cx + (k % 2) * half;
        const double ry = top + 124 + (k / 2) * kLineH;
        const double cur = f(ctx, kCpuCluster0Mhz + k);
        const double max = f(ctx, kCpuCluster0MaxMhz + k);
        const double hwMax = f(ctx, kCpuCluster0HwMaxMhz + k);
        double lx = rx + pen.text(rx, ry, "C" + std::to_string(k) + " ", 14, kTextMuted);
        lx += pen.text(lx, ry, num(cur / 1000.0, 2), 15, kText, true);
        lx += pen.text(lx, ry, "/" + num(max / 1000.0, 2) + "GHz", 14, kTextMuted);
        if (!std::isnan(max) && !std::isnan(hwMax) && max < hwMax - 1.0) {
            // Capped below the hardware max (Playtime): say from what
            pen.text(lx + 6, ry, std::string(t.capped) + num(hwMax / 1000.0, 2), 12, kWarn, true);
        }
    }
}

void drawGpuRam(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double top = y + kTop;
    const double busy = f(ctx, kGpuBusyPct);
    pen.dot(cx + 4, top + 42, kGpu);
    if (ok(ctx, kGpuBusyPct)) {
        const double w = drawBigValue(pen, cx + 14, top + 52, num(busy, 0), "%",
                                      colorOf(levelOf(busy, th.gpuWarnPct, th.gpuCritPct)));
        pen.text(cx + 14 + w + 6, top + 52, t.gpuBusyEstimate, 13, kTextMuted);
    } else {
        drawBigValue(pen, cx + 14, top + 52, "--", "%", kTextMuted);
        pen.text(cx + 14 + 60, top + 52, naText(ctx, kGpuBusyPct), 12, kTextMuted);
    }
    pen.text(cx + 14, top + 72,
             std::string(t.clock) + num(f(ctx, kGpuClockMhz), 0) + " / " + num(f(ctx, kGpuMaxClockMhz), 0) + " MHz", 15,
             kTextMuted);
    const double used = f(ctx, kRamUsedGib);
    const double total = f(ctx, kRamTotalGib);
    double lx = cx + 14 + pen.text(cx + 14, top + 90, t.ram, 15, kTextMuted);
    lx += pen.text(lx, top + 90, num(used, 1), 15, kText, true);
    pen.text(lx, top + 90, " / " + num(total, 1) + " GiB", 15, kTextMuted);
    const GraphArea area = graphArea(x, y, 0.0, 100.0, ctx);
    drawGraphLabel(pen, area, "100%");
    drawSeriesFill(pen, area, h.a, kGpu, kGpuFillAlpha);
    drawGuide(pen, area, th.gpuWarnPct, kWarn);
    drawSeries(pen, area, h.a, kGpu);
}

struct Rail {
    const char* group;  ///< label prefix (nullptr = indented sub-rail)
    const char* name;
    uint16_t id;        ///< watts id (volts at + kPowerRailVoltsOffset); 0 = the CPU sum row
};

void drawPower(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double top = y + kTop;
    const double vph = f(ctx, kPowerVphW);
    pen.dot(cx + 4, top + 42, kAccent);
    if (ok(ctx, kPowerVphW)) {
        drawBigValue(pen, cx + 14, top + 52, num(vph, 2), "W", colorOf(levelOf(vph, th.powerWarnW, th.powerCritW)));
    } else {
        drawBigValue(pen, cx + 14, top + 52, "--", "W", kTextMuted);
    }
    if (ok(ctx, kPowerVphW)) {
        pen.text(cx + 14, top + 72, "vph " + num(f(ctx, kPowerVphV), 2) + " V", 15, kTextMuted);
    } else {
        pen.text(cx + 14, top + 72, naText(ctx, kPowerVphW), 14, kTextMuted);
    }
    pen.text(cx + 14, top + 90, std::string(t.rails) + num(f(ctx, kPowerRailsW), 2) + " W", 15, kTextMuted);

    const double maxData = h.a.maxValue();
    const double maxV = std::isnan(maxData) ? 5.0 : std::max(2.0, std::ceil(maxData * 1.2));
    const GraphArea area = graphArea(x, y, 0.0, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "W");
    drawGuide(pen, area, th.powerWarnW, kWarn);
    drawGuide(pen, area, th.powerCritW, kDanger);
    drawSeries(pen, area, h.a, kAccent);

    // The tree: rails under vph, each with W + V and a share-of-vph bar
    static const Rail kRails[] = {
        {"CPU", "apc0-2", 0},           {nullptr, "apc0", kPowerApc0W}, {nullptr, "apc1", kPowerApc1W},
        {nullptr, "apc2", kPowerApc2W}, {"GPU", "gfx", kPowerGfxW},     {"NPU", "nsp1", kPowerNsp1W},
        {"NPU", "nsp2", kPowerNsp2W},   {"", "s1c", kPowerS1cW},        {"", "s3c", kPowerS3cW},
        {"", "s6c", kPowerS6cW},        {"", "bob", kPowerBobW},
    };
    const double barX = x + 200;
    const double barW = 120;
    const double wRight = x + 400;
    const double vRight = x + kInR;
    double ry = top + 100;
    pen.text(wRight, ry + 2, "W", 12, kTextMuted, false, true);
    pen.text(vRight, ry + 2, "V", 12, kTextMuted, false, true);
    const auto row = [&](const std::string& label, bool indent, double watts, const std::string& volts, Color c) {
        ry += kRowH;
        pen.text(cx + (indent ? 22 : 8), ry, label, 14, indent ? kTextMuted : kText);
        if (!std::isnan(watts) && !std::isnan(vph) && vph > 0) {
            pen.color(kAccent, kAccentTintAlpha * 1.5);
            pen.roundedRect(barX, ry - 10, std::clamp(watts / vph, 0.0, 1.0) * barW, 8, 3);
            cairo_fill(pen.cr);
        }
        pen.text(wRight, ry, num(watts, 2), 14, c, false, true);
        pen.text(vRight, ry, volts, 14, kTextMuted, false, true);
    };
    for (const Rail& r : kRails) {
        if (r.id == 0) {
            const double sum = f(ctx, kPowerApc0W) + f(ctx, kPowerApc1W) + f(ctx, kPowerApc2W);  // NaN if any is N/A
            row(std::string(r.group) + " " + r.name, false, sum, "", kText);
            continue;
        }
        const uint16_t vid = static_cast<uint16_t>(r.id + kPowerRailVoltsOffset);
        // gfx's voltage sense reads 2-7 mV at ~0.4 W: not a real reading, so "—" rather than a number
        std::string volts = num(f(ctx, vid), 2);
        if (!ok(ctx, vid) && reasonOf(ctx, vid) == Reason::InvalidReading) volts = "—";
        const std::string label = r.group == nullptr ? r.name
                                  : (r.group[0] != '\0' ? std::string(r.group) + " " + r.name : std::string(r.name));
        row(label, r.group == nullptr, f(ctx, r.id), volts, kText);
    }
    // vph − rails is unclamped data (the three chips sample at slightly different times): never show a negative
    const double unmeasured = f(ctx, kPowerUnmeasuredW);
    row(t.unmeasured, false, std::isnan(unmeasured) ? unmeasured : std::max(0.0, unmeasured), "", kTextMuted);
}

std::string statusText(const WidgetContext& ctx) {
    const WidgetText& t = widgetText();
    switch (static_cast<BatteryStatus>(i(ctx, kBatteryStatus))) {
        case BatteryStatus::Charging: return t.statusCharging;
        case BatteryStatus::Discharging: return t.statusDischarging;
        case BatteryStatus::Full: return t.statusFull;
        case BatteryStatus::NotCharging: return i(ctx, kBatteryChargerOnline) != 0 ? t.statusPlugged : t.statusIdle;
        case BatteryStatus::Unknown: break;
    }
    return "";
}

void drawBattery(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    const double pct = f(ctx, kBatteryPct);
    pen.dot(cx + 4, top + 42, kAccent);
    double bx = cx + 14;
    if (ok(ctx, kBatteryPct)) {
        bx += drawBigValue(pen, bx, top + 52, num(pct, 0), "%",
                           colorOf(levelOf(pct, th.batteryWarnPct, th.batteryCritPct, true)));
        bx += 8;
        if (static_cast<BatteryStatus>(i(ctx, kBatteryStatus)) == BatteryStatus::Charging) {
            bx += drawBolt(pen, bx, top + 50, kWarn) + 4;
        }
        pen.text(bx, top + 50, statusText(ctx), 15, kTextMuted);
    } else {
        drawBigValue(pen, bx, top + 52, "--", "%", kTextMuted);
        pen.text(bx + 70, top + 50, naText(ctx, kBatteryPct), 14, kTextMuted);
    }
    const double watts = f(ctx, kBatteryPowerW);
    std::string flow = watts < 0 ? t.flowOut : t.flowIn;
    if (!flow.empty()) flow[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(flow[0])));
    pen.dot(cx + 4, top + 67, kGpu);
    pen.text(cx + 14, top + 72, std::isnan(watts) ? "--" : num(std::fabs(watts), 2) + " W " + flow, 15, kText);
    pen.text(cx + 14, top + 88, t.batteryNote, 13, kTextMuted);

    double ry = top + 106;
    const auto row = [&](const std::string& label, const std::string& v, Color c = kText) {
        ry += kRowH;
        labelValue(pen, cx + 8, right, ry, label, v, c, 14);
    };
    row(t.charge, num(f(ctx, kBatteryChargeNowMah), 0) + " / " + num(f(ctx, kBatteryChargeFullMah), 0) +
                      " mAh (design " + num(f(ctx, kBatteryChargeDesignMah), 0) + ")");
    row(!std::isnan(watts) && watts < 0 ? t.flowOut : t.flowIn,
        vawText(f(ctx, kBatteryVoltageV), std::fabs(f(ctx, kBatteryCurrentMa)) / 1000.0, std::fabs(watts)));
    // Time rows only while they mean something (charging / discharging)
    if (ok(ctx, kBatteryTimeToFullS)) row(t.toFull, durationText(static_cast<double>(i(ctx, kBatteryTimeToFullS))));
    if (ok(ctx, kBatteryTimeToEmptyS)) row(t.toEmpty, durationText(static_cast<double>(i(ctx, kBatteryTimeToEmptyS))));
    const std::string health = text(ctx, kBatteryHealth);
    row(t.health, (health.empty() ? "--" : health) + " · " + t.cycles + " " +
                      (ok(ctx, kBatteryCycles) ? std::to_string(i(ctx, kBatteryCycles)) : std::string("--")));
    const std::string mode = text(ctx, kBatteryChargerMode);
    row(t.charger, mode.empty() ? "--" : mode);
    if (ok(ctx, kBatterySetCurrentA)) {
        row(t.setCurrent, num(f(ctx, kBatterySetCurrentA), 2) + " A (max " + num(f(ctx, kBatterySetCurrentMaxA), 2) + " A)");
    }
    const bool plugged = i(ctx, kBatteryPlugged) != 0;
    if (!plugged) {
        row(t.usb, t.unplugged, kTextMuted);
        return;
    }
    const std::string usb = text(ctx, kBatteryUsbType);
    row(t.usb, usb.empty() ? "--" : usb);
    const double offeredV = f(ctx, kBatteryPdOfferedV);
    if (ok(ctx, kBatteryPdOfferedV)) {
        row(t.pdOffered, vawText(offeredV, f(ctx, kBatteryPdOfferedA), f(ctx, kBatteryPdOfferedW)));
    }
    row(t.arriving, vawText(f(ctx, kBatteryInputV), f(ctx, kBatteryInputA), f(ctx, kBatteryInputW)));
    if (ok(ctx, kBatterySagV)) {
        // FramePerf's rule: sag at or above 10% of the offered voltage is worth a look (cable, plug, charger)
        const double sag = f(ctx, kBatterySagV);
        const bool high = !std::isnan(offeredV) && offeredV > 0 && sag >= 0.10 * offeredV;
        row(t.sag, num(sag, 2) + " V" + (high ? t.sagHigh : ""), high ? kWarn : kText);
    }
}

void drawTemp(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const double cx = x + kInX;
    const double top = y + kTop;
    const double cpu = f(ctx, kTempCpuC);
    const double gpu = f(ctx, kTempGpuC);
    pen.dot(cx + 4, top + 42, kAccent);
    drawBigValue(pen, cx + 14, top + 52, num(cpu, 0), "°C CPU", colorOf(levelOf(cpu, th.tempWarnC, th.tempCritC)));
    pen.dot(cx + 4, top + 67, kGpu);
    const double gw = pen.text(cx + 14, top + 72, "GPU " + num(gpu, 0) + "°C", 15,
                               colorOf(levelOf(gpu, th.tempWarnC, th.tempCritC)));
    pen.text(cx + 14 + gw + 10, top + 72, std::string(t.tDdr) + " " + num(f(ctx, kTempDdrC), 0) + "°C", 15, kTextMuted);
    pen.text(cx + 14, top + 90, std::string(t.fan) + grouped(f(ctx, kFanRpm)) + " rpm", 15, kTextMuted);

    const double dataMin = std::fmin(h.a.minValue(), h.b.minValue());
    const double dataMax = std::fmax(h.a.maxValue(), h.b.maxValue());
    const double minV = std::isnan(dataMin) ? 30.0 : std::min(30.0, std::floor(dataMin / 5.0) * 5.0 - 5.0);
    const double maxV = std::isnan(dataMax) ? th.tempCritC + 5.0 : std::max(th.tempCritC + 5.0, dataMax + 5.0);
    const GraphArea area = graphArea(x, y, minV, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "°C");
    drawGuide(pen, area, th.tempWarnC, kWarn);
    drawGuide(pen, area, th.tempCritC, kDanger);
    drawSeries(pen, area, h.b, kGpu);
    drawSeries(pen, area, h.a, kAccent);

    // Every other temperature, three per row
    const struct {
        const char* label;
        uint16_t id;
    } kSpots[] = {
        {t.tBattery, kTempBatteryC}, {t.tDisplay, kTempDisplayC}, {t.tExhaust, kTempExhaustC},
        {t.tHeatsink, kTempHeatsinkC}, {t.tNpu, kTempNpuC},     {t.tCamera, kTempCameraC},
        {t.tVideo, kTempVideoC},     {t.tWifi, kTempWifiC},       {t.tFlash, kTempFlashC},
        {t.tNrf, kTempNrfC},         {t.tConnector, kTempConnectorC},
    };
    const double colW = (kInR - kInX) / 3;
    for (size_t k = 0; k < sizeof(kSpots) / sizeof(kSpots[0]); ++k) {
        const double sx = cx + (k % 3) * colW;
        const double sy = top + 114 + (k / 3) * kLineH;
        const double v = f(ctx, kSpots[k].id);
        labelValue(pen, sx + 8, sx + colW - 10, sy, kSpots[k].label, num(v, 0) + "°C",
                   colorOf(levelOf(v, th.tempWarnC, th.tempCritC)), 14);
    }

    const bool cpuThrottle = i(ctx, kThrottleCpu) != 0;
    const bool gpuThrottle = i(ctx, kThrottleGpu) != 0;
    if (cpuThrottle || gpuThrottle) {
        // Upstream framed the whole panel in red; Temp owns the throttle flags, so its card gets the frame
        strokeRounded(pen, x, y, kCardW, widgetHeight(WidgetInstance {WidgetType::Temp}), 12, kDanger, 3.0);
        drawBadge(pen, area.x + area.w - 4, area.y + 4,
                  std::string(t.thermalLimit) + (cpuThrottle && gpuThrottle ? "CPU+GPU" : (cpuThrottle ? "CPU" : "GPU")),
                  kDanger);
    }
}

void drawProcessList(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const WidgetText& t = widgetText();
    const bool cpu = w.type == WidgetType::CpuItems;
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    pen.text(right, top + 17, cpu ? t.pctOfCore : t.rss, 13, kTextMuted, false, true);
    const uint16_t rowsId = cpu ? kCpuItemsRows : kRamItemsRows;
    if (ctx.processes == nullptr || !ok(ctx, rowsId)) {
        const bool first = reasonOf(ctx, rowsId) == Reason::NotSampledYet;
        pen.text(cx + 8, top + 26 + kRowH, first ? std::string(t.measuring) : naText(ctx, rowsId), 14, kTextMuted);
        return;
    }
    const std::vector<ProcessItem>& rows = cpu ? ctx.processes->byCpu : ctx.processes->byRam;
    // The in-game HUD isn't interactive: the top rows that fit, no scrolling
    const size_t n = std::min<size_t>(rows.size(), w.maxItems);
    for (size_t k = 0; k < n; ++k) {
        const ProcessItem& r = rows[k];
        const double ry = top + 26 + kRowH * static_cast<double>(k + 1);
        const std::string v = cpu ? num(r.cpuCorePct, 1) + "%" : num(r.rssMiB, 0) + " MiB";
        const double vw = pen.text(right, ry, v, 14, kText, false, true);
        std::string name = r.name;
        if (r.count > 1) name += " ×" + std::to_string(r.count);
        pen.text(cx + 8, ry, ellipsize(pen, name, 14, right - vw - 12 - (cx + 8)), 14, k == 0 ? kText : kTextMuted);
    }
}

const char* bandText(int64_t band) {
    switch (static_cast<WifiBand>(band)) {
        case WifiBand::Ghz2_4: return "2.4 GHz";
        case WifiBand::Ghz5: return "5 GHz";
        case WifiBand::Ghz6: return "6 GHz";
        case WifiBand::Ghz60: return "60 GHz";
        case WifiBand::Unknown: break;
    }
    return "";
}

void drawNetwork(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = widgetText();
    const uint16_t base = w.adapter == NetAdapter::Home ? kNetHomeBase : kNetDongleBase;
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    const std::string ifname = text(ctx, base + kNetAdapterIfName);
    pen.text(cx + pen.measure(w.adapter == NetAdapter::Home ? t.networkHome : t.networkDongle, 16) + 8, top + 17,
             ifname.empty() ? std::string(w.adapter == NetAdapter::Home ? "wlan0" : "wlanap") : ifname, 13, kTextMuted);
    if (!ok(ctx, base + kNetAdapterPresent) || i(ctx, base + kNetAdapterPresent) == 0) {
        drawWifiIcon(pen, right - 26, top + 18, 0, kTextMuted, true);
        const std::string why = ok(ctx, base + kNetAdapterPresent) ? std::string(t.naPrefix) + reasonText(Reason::SensorMissing)
                                                                    : naText(ctx, base + kNetAdapterPresent);
        pen.text(cx + 8, top + 44, why, 15, kTextMuted);
        return;
    }
    const bool connected = i(ctx, base + kNetAdapterConnected) != 0;
    const double dbm = f(ctx, base + kNetAdapterSignalDbm);
    const Level signalLevel = levelOf(dbm, th.wifiWarnDbm, th.wifiCritDbm, true);
    // Signal: icon + dBm at the right of the title row
    std::string signal = std::isnan(dbm) ? "--" : num(std::fabs(dbm), 0);
    if (!std::isnan(dbm) && dbm < 0) signal = "−" + signal;
    const double sw = connected ? pen.text(right, top + 17, signal + " dBm", 13, colorOf(signalLevel, kTextMuted), false, true)
                                : 0.0;
    drawWifiIcon(pen, right - sw - 32, top + 18, connected ? wifiBars(dbm, th) : 0,
                 connected ? colorOf(signalLevel) : kTextMuted, !connected);
    // Traffic
    double lx = cx + 8;
    const double ry = top + 44;
    if (!connected) {
        pen.text(lx, ry, t.notConnected, 16, kTextMuted);
    } else {
        // Each value carries its unit: one side can be kbps while the other is Mbps
        const RateText down = formatRate(f(ctx, base + kNetAdapterRxMbps));
        const RateText up = formatRate(f(ctx, base + kNetAdapterTxMbps));
        lx += drawArrow(pen, lx, ry, false, kTextMuted);
        lx += pen.text(lx, ry, down.value, 17, kText, true);
        lx += pen.text(lx, ry, down.unit, 14, kTextMuted);
        lx += 8;
        lx += drawArrow(pen, lx, ry, true, kTextMuted);
        lx += pen.text(lx, ry, up.value, 15, kText);
        pen.text(lx, ry, up.unit, 14, kTextMuted);
    }
    // Link rate, band, channel
    std::string radio = std::string(t.link) + num(f(ctx, base + kNetAdapterTxLinkMbps), 0) + "/" +
                        num(f(ctx, base + kNetAdapterRxLinkMbps), 0) + " Mbps";
    const std::string band = bandText(i(ctx, base + kNetAdapterBand));
    if (!band.empty()) radio += "  ·  " + band;
    if (ok(ctx, base + kNetAdapterChannel)) radio += "  ch " + std::to_string(i(ctx, base + kNetAdapterChannel));
    if (ok(ctx, base + kNetAdapterWidthMhz)) radio += "  " + std::to_string(i(ctx, base + kNetAdapterWidthMhz)) + " MHz";
    pen.text(cx + 8, top + 68, radio, 14, kTextMuted);
    const std::string ip = text(ctx, base + kNetAdapterIp);
    pen.text(cx + 8, top + 92, std::string(t.ip) + (ip.empty() ? "--" : ip), 14, kTextMuted);
}

/** "L 80%⚡" style. @return width */
double drawController(const Pen& pen, double x, double y, const char* label, uint16_t pctId, uint16_t chargingId,
                      const WidgetContext& ctx) {
    const double start = x;
    x += pen.text(x, y, label, 15, kTextMuted);
    if (!ok(ctx, pctId)) return x - start + pen.text(x, y, "--", 15, kTextMuted);
    const double pct = f(ctx, pctId);
    const Thresholds& th = ctx.thresholds;
    x += pen.text(x, y, num(pct, 0) + "%", 16, colorOf(levelOf(pct, th.controllerWarnPct, th.controllerCritPct, true)), true);
    if (i(ctx, chargingId) != 0) x += 2 + drawBolt(pen, x + 2, y, kWarn);
    return x - start;
}

void drawGeneral(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const WidgetText& t = widgetText();
    const double cx = x + kInX + 8;
    const double right = x + kInR;
    const double top = y + kTop;
    const double valueX = cx + 110;
    double ry = top + 20 + kLineH;
    pen.text(cx, ry, t.controllers, 15, kTextMuted);
    double lx = valueX;
    lx += drawController(pen, lx, ry, t.left, kCtrlLeftPct, kCtrlLeftCharging, ctx) + 16;
    drawController(pen, lx, ry, t.right, kCtrlRightPct, kCtrlRightCharging, ctx);
    // Clock "Off" on the dashboard (clock_format 0, upstream's "hidden") hides the Time row
    if (ctx.clockFormat != 0) {
        ry += kLineH;
        pen.text(cx, ry, t.time, 15, kTextMuted);
        pen.text(valueX, ry, clockText(text(ctx, kClock), ctx.clockFormat), 16, kText, true);
    }
    ry += kLineH;
    pen.text(cx, ry, t.uptime, 15, kTextMuted);
    pen.text(valueX, ry, ok(ctx, kUptimeS) ? durationText(static_cast<double>(i(ctx, kUptimeS))) : naText(ctx, kUptimeS),
             15, kText);
    ry += kLineH;
    pen.text(cx, ry, t.app, 15, kTextMuted);
    std::string app = text(ctx, kSceneAppName);
    if (app.empty()) app = ok(ctx, kScenePid) ? t.noApp : naText(ctx, kSceneAppName);
    pen.text(valueX, ry, ellipsize(pen, app, 15, right - valueX), 15, kText);
    ry += kLineH;
    pen.text(cx, ry, t.hudCost, 15, kTextMuted);
    // Own power is an estimate (own CPU share × CPU rails): always shown with "≈"
    double hx = valueX;
    hx += pen.text(hx, ry, num(f(ctx, kSelfCpuPct), 1) + "% CPU  ·  " + num(f(ctx, kSelfRssMib), 0) + " MiB  ·  ", 15,
                   kText);
    hx += drawApprox(pen, hx, ry, 15, kText);
    pen.text(hx, ry, num(f(ctx, kSelfPowerW), 2) + " W", 15, kText);
}

void drawStreaming(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const double left=x+kInX+8, valueX=x+216, right=x+kInR-8;
    double row=y+kTop+44;
    const auto line=[&](const char* label,const std::string& v) {
        pen.text(left,row,label,14,kTextMuted);
        pen.text(valueX,row,ellipsize(pen,v,14,right-valueX),14,kText);
        row+=20;
    };
    const auto number=[&](uint16_t id,const char* unit="",int precision=1) {
        return ok(ctx,id)?num(f(ctx,id),precision)+unit:
            reasonOf(ctx,id)==Reason::NoSteamVr?std::string("N/A: SteamVR not running"):naText(ctx,id);
    };
    const auto counter=[&](uint16_t id) {
        return ok(ctx,id)?grouped(i(ctx,id)):naText(ctx,id);
    };
    const char* modes[]={"Not streaming","Dongle","Home Wi-Fi","Multi-link","Idle"};
    const auto mode=i(ctx,kStreamLinkMode);
    line("Link",ok(ctx,kStreamLinkMode)&&mode>=0&&mode<5?modes[mode]:naText(ctx,kStreamLinkMode));
    line("Headset FEC retries",counter(kStreamFecRetries)+" / "+counter(kStreamFecRetryPackets)+" packets");
    const auto* pc=value(ctx,kPcStreamPing);
    if(!pc||(!pc->ok&&(pc->reason==Reason::PcAppNotRunning||pc->reason==Reason::NotSampledYet))) {
        pen.text(left,row,"PC app not running",14,kTextMuted);return;
    }
    line("FPS / ping",number(kPcStreamFps," Hz",0)+" / "+number(kPcStreamPing," ms"));
    line("Wire bitrate",number(kPcStreamWireMbps," Mibit/s"));
    line("Video / FEC",ok(ctx,kPcStreamVideoMbps)?number(kPcStreamVideoMbps)+" / "+number(kPcStreamFecMbps)+" Mibit/s":number(kPcStreamVideoMbps));
    line("Audio / retry / logic",ok(ctx,kPcStreamAudioMbps)?number(kPcStreamAudioMbps)+" / "+number(kPcStreamRetryMbps)+" / "+number(kPcStreamLogicMbps)+" Mibit/s":number(kPcStreamAudioMbps));
    line("Latency","avg / p95 (ms)");
    const char* stages[]={"Game","Encode","Send","Network","Decode + queue","Slack / display","Total"};
    for(unsigned j=0;j<7;++j) {
        const auto id=static_cast<uint16_t>(1040+2*j);
        line(stages[j],ok(ctx,id)?number(id)+" / "+number(static_cast<uint16_t>(id+1)):number(id));
    }
    line("Named encode / send",number(kPcStreamNamedEncode)+" / "+number(kPcStreamNamedSend)+" ms");
    for(unsigned j=0;j<4;++j) {
        const auto id=static_cast<uint16_t>(1060+2*j);const std::string label="Link "+std::to_string(j+1)+" max / share";
        line(label.c_str(),ok(ctx,id)?number(id," ms")+" / "+number(static_cast<uint16_t>(id+1),"%") : number(id));
    }
    line("Bandwidth estimate",number(kPcStreamEstimatedBandwidth," Mbit/s"));
    line("Retransmit req / sent",number(kPcStreamRetransmitRequests,"",0)+" / "+number(kPcStreamRetransmitSent,"",0));
    line("Skipped/s / late",number(kPcStreamSkipped,"",0)+" / "+number(kPcStreamLate,"",0));
    line("Encoder / stream resets",number(kPcStreamEncoderResets,"",0)+" / "+counter(kPcStreamStreamResets));
    line("Bad link / throttle",counter(kPcStreamBadLinks)+" / "+counter(kPcStreamThrottleEvents));
    line("Encoder reinitializations",counter(kPcStreamEncoderReinits));
    for(const auto id:{kPcStreamL1Note,kPcStreamL2Note,kPcStreamL3Note}) {
        const auto note=text(ctx,id);
        if(!note.empty()) {
            const std::string label=id==kPcStreamL1Note?"Named stats":id==kPcStreamL2Note?"Links":"Frames";
            line(label.c_str(),note);
        } else row+=20;
    }
    if(reasonOf(ctx,kPcStreamTotalAvg)==Reason::SteamVrChanged || reasonOf(ctx,kPcStreamNamedEncode)==Reason::SteamVrChanged || reasonOf(ctx,kPcStreamLink0Ping)==Reason::SteamVrChanged)
        pen.text(left,row,"SteamVR changed: streaming details unavailable (build "+text(ctx,kPcStreamBuild)+")",12,kTextMuted);
}

void drawPlaceholder(const Pen& pen, double x, double y, const WidgetInstance& w) {
    const Reason reason = w.type == WidgetType::Streaming ? Reason::PcAppNotRunning : Reason::CameraHelperMissing;
    pen.text(x + kInX + 8, y + kTop + 44, std::string(widgetText().naPrefix) + reasonText(reason), 15, kTextMuted);
}

}  // namespace

uint64_t widgetSignature(const WidgetInstance& w, const WidgetContext& ctx) {
    // Metric ids per owner widget, built once from the registry
    static const std::vector<std::vector<uint16_t>> kOwned = [] {
        std::vector<std::vector<uint16_t>> owned(kWidgetTypeCount);
        for (const MetricDef& d : registry()) owned[static_cast<size_t>(d.owner)].push_back(d.id);
        return owned;
    }();
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the inputs' bytes
    const auto mix = [&h](const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 1099511628211ull;
    };
    const size_t type = static_cast<size_t>(w.type);
    if (type < kOwned.size()) {
        for (const uint16_t id : kOwned[type]) {
            const MetricValue* v = value(ctx, id);
            if (v != nullptr) mix(&v->sampledAt, sizeof(v->sampledAt));
        }
    }
    if ((w.type == WidgetType::CpuItems || w.type == WidgetType::RamItems) && ctx.processes != nullptr) {
        mix(&ctx.processes->generation, sizeof(ctx.processes->generation));
    }
    mix(&ctx.thresholds, sizeof(ctx.thresholds));
    mix(&ctx.graphSeconds, sizeof(ctx.graphSeconds));
    mix(&ctx.clockFormat, sizeof(ctx.clockFormat));
    const bool hasStore = ctx.store != nullptr;
    const bool hasLists = ctx.processes != nullptr;
    mix(&hasStore, sizeof(hasStore));
    mix(&hasLists, sizeof(hasLists));
    return h;
}

int widgetHeight(const WidgetInstance& w) {
    switch (w.type) {
        case WidgetType::Frame:
        case WidgetType::GpuRam: return kGraphCardH;
        case WidgetType::Power: return kGraphCardH + static_cast<int>(kRowH) * (kPowerRails + 1) + 10;
        case WidgetType::Temp: return kGraphCardH + 4 * static_cast<int>(kLineH) + 8;
        case WidgetType::Cpu: return 166;
        case WidgetType::Battery: return static_cast<int>(kTop) + 106 + kBatteryRows * static_cast<int>(kRowH) + 10;
        case WidgetType::CpuItems:
        case WidgetType::RamItems: return static_cast<int>(kTop) + 26 + static_cast<int>(kRowH) * static_cast<int>(w.maxItems) + 10;
        case WidgetType::Network: return 108;
        case WidgetType::General: return static_cast<int>(kTop) + 20 + 5 * static_cast<int>(kLineH) + 12;
        case WidgetType::Streaming: return 628;
        case WidgetType::HeadsetCamera:
        case WidgetType::EyeCamera: return 64;
        case WidgetType::None: break;
    }
    return 0;
}

const char* widgetTitle(const WidgetInstance& w) {
    return titleOf(w);
}

void drawWidgetStatic(const Pen& pen, double x, double y, const WidgetInstance& w) {
    staticCard(pen, x, y, widgetHeight(w));
    if (hasGraph(w.type)) staticGraphBed(pen, x, y);
    staticTitle(pen, x, y, titleOf(w));
}

void pushWidgetHistory(const WidgetInstance& w, const WidgetContext& ctx, WidgetHistory& history) {
    const double keep = std::max(1, ctx.graphSeconds);
    switch (w.type) {
        case WidgetType::Frame: history.a.push(ctx.now, f(ctx, kFrameAppFps), keep); break;
        case WidgetType::GpuRam: history.a.push(ctx.now, f(ctx, kGpuBusyPct), keep); break;
        case WidgetType::Power: history.a.push(ctx.now, f(ctx, kPowerVphW), keep); break;
        case WidgetType::Temp:
            history.a.push(ctx.now, f(ctx, kTempCpuC), keep);
            history.b.push(ctx.now, f(ctx, kTempGpuC), keep);
            break;
        default: break;
    }
}

void drawWidgetDynamic(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
                       const WidgetHistory& history) {
    switch (w.type) {
        case WidgetType::Frame: drawFrame(pen, x, y, ctx, history); break;
        case WidgetType::Cpu: drawCpu(pen, x, y, ctx); break;
        case WidgetType::GpuRam: drawGpuRam(pen, x, y, ctx, history); break;
        case WidgetType::Power: drawPower(pen, x, y, ctx, history); break;
        case WidgetType::Battery: drawBattery(pen, x, y, ctx); break;
        case WidgetType::Temp: drawTemp(pen, x, y, ctx, history); break;
        case WidgetType::CpuItems:
        case WidgetType::RamItems: drawProcessList(pen, x, y, w, ctx); break;
        case WidgetType::Network: drawNetwork(pen, x, y, w, ctx); break;
        case WidgetType::General: drawGeneral(pen, x, y, ctx); break;
        case WidgetType::Streaming: drawStreaming(pen, x, y, ctx); break;
        case WidgetType::HeadsetCamera:
        case WidgetType::EyeCamera: drawPlaceholder(pen, x, y, w); break;
        case WidgetType::None: break;
    }
}

}  // namespace fph
