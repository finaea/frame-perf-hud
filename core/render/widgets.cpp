// The widgets. Drawing helpers (levels, graphs, badges, icons) are the fixed panel's ones (panel.cpp), moved here.
#include "widgets.h"

#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace fph {

namespace {

using namespace metric;

// ---- Card geometry (relative to the card's top-left; the card sits 8 px inside its 512 px column) ----
constexpr double kCardW = kColumnWidth - 16;  // 496
constexpr double kInX = 6;                    // content left = the fixed panel's 14 px padding
constexpr double kInR = kCardW - 6;           // content right
constexpr double kTop = 6;                    // the fixed panel's "row top" sits 6 px below the card top
constexpr double kGraphX = 202;               // the fixed panel: kPad + kLeftWidth + 8 = 210 from the panel edge
constexpr double kGraphW = 288;
constexpr double kGraphH = 84;
constexpr double kRowH = 20;                  // compact rows (battery)
constexpr double kLineH = 22;                 // text rows (temps, general)

// Heights (card)
constexpr int kGraphCardH = 102;  // the fixed panel's row: 92 + 6 above + 4 below
constexpr int kBatteryCompactH = static_cast<int>(kTop) + 82;  // % + the W line
constexpr int kBatteryFullH = static_cast<int>(kTop + 106 + kRowH) + 10;
constexpr int kTempSpots = 4;
constexpr int kTempFullH = kGraphCardH + (kTempSpots + 2) / 3 * static_cast<int>(kLineH) + 8;
constexpr int kCpuH = static_cast<int>(kTop) + 58;
constexpr int kNetworkShortH = 64;   // no link: the state line only
constexpr int kNetworkFullH = static_cast<int>(kTop) + 82;

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

const WidgetText& texts(const WidgetContext& ctx) {
    return widgetText(ctx.language);
}

/** "N/A: <reason>" */
std::string naText(const WidgetContext& ctx, uint16_t id) {
    return std::string(texts(ctx).naPrefix) + reasonText(reasonOf(ctx, id));
}

// ---- Graphs (the fixed panel's) ----

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

/** Small up / down arrow. @return width */
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

const char* titleOf(const WidgetInstance& w, const WidgetText& t) {
    switch (w.type) {
        case WidgetType::Frame: return t.frame;
        case WidgetType::Cpu: return t.cpu;
        case WidgetType::GpuRam: return t.gpuRam;
        case WidgetType::Power: return t.power;
        case WidgetType::Battery: return t.battery;
        case WidgetType::Temp: return t.temp;
        case WidgetType::Network: return t.network;
        case WidgetType::General: return t.general;
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
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double hz = f(ctx, kFrameDisplayHz);
    const double fps = f(ctx, kFrameAppFps);
    const double targetMs = hz > 0 ? 1000.0 / hz : std::nan("");
    const auto msLevel = [&](double ms) {
        return levelOf(ms, targetMs * th.frameWarnRatio, targetMs * th.frameCritRatio);
    };
    if (!ok(ctx, kFrameAppFps)) {
        drawBigValue(pen, cx, top + 52, "--", "fps", kTextMuted);
        const Reason why = reasonOf(ctx, kFrameAppFps);
        const std::string note = why == Reason::NoSteamVr ? std::string(t.noSteamVr)
                                 : why == Reason::NoFrames ? std::string(t.waiting)
                                                           : naText(ctx, kFrameAppFps);
        pen.text(cx, top + 72, note, 15, kTextMuted);
    } else {
        const Level fpsLevel = hz > 0 ? levelOf(fps, hz * th.fpsWarnRatio, hz * th.fpsCritRatio, true) : Level::Normal;
        pen.dot(cx + 4, top + 42, kAccent);
        const double bigWidth = drawBigValue(pen, cx + 14, top + 52, num(fps, 0), "fps", colorOf(fpsLevel));
        if (hz > 0) pen.text(cx + 14 + bigWidth + 6, top + 52, "/ " + num(hz, 0) + "Hz", 14, kTextMuted);
        const double gpuMs = f(ctx, kFrameGpuMs);
        const double cpuMs = f(ctx, kFrameCpuMs);
        double lx = cx + 14;
        lx += pen.text(lx, top + 72, "GPU " + num(gpuMs, 1), 15, colorOf(msLevel(gpuMs), kTextMuted));
        lx += pen.text(lx, top + 72, "  CPU " + num(cpuMs, 1), 15, colorOf(msLevel(cpuMs), kTextMuted));
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
        drawBadge(pen, area.x + area.w - 4, area.y + 4, throttleBadgeText(t, scaleHz, static_cast<int>(throttled)),
                  kWarn);
    }
}

void drawCpu(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
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
    // Clocks: the fastest cluster now, and the big core
    double lx = cx + 8;
    const double ly = top + 46;
    lx += pen.text(lx, ly, num(f(ctx, kCpuFastestClockMhz) / 1000.0, 2), 17, kText, true);
    lx += pen.text(lx, ly, " GHz", 15, kTextMuted);
    lx += 16;
    lx += pen.text(lx, ly, t.bigCore, 15, kTextMuted);
    lx += pen.text(lx, ly, num(f(ctx, kCpuBigCoreClockMhz) / 1000.0, 2), 15, kText);
    pen.text(lx, ly, " GHz", 15, kTextMuted);
}

void drawGpuRam(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double busy = f(ctx, kGpuBusyPct);
    pen.dot(cx + 4, top + 42, kGpu);
    if (ok(ctx, kGpuBusyPct)) {
        const double w = drawBigValue(pen, cx + 14, top + 52, num(busy, 0), "%",
                                      colorOf(levelOf(busy, th.gpuWarnPct, th.gpuCritPct)));
        pen.text(cx + 14 + w + 6, top + 52, t.gpuLoad, 13, kTextMuted);
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

void drawPower(const Pen& pen, double x, double y, const WidgetContext& ctx, const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double vph = f(ctx, kPowerVphW);
    pen.dot(cx + 4, top + 42, kAccent);
    if (ok(ctx, kPowerVphW)) {
        drawBigValue(pen, cx + 14, top + 52, num(vph, 2), "W", colorOf(levelOf(vph, th.powerWarnW, th.powerCritW)));
    } else {
        drawBigValue(pen, cx + 14, top + 52, "--", "W", kTextMuted);
        pen.text(cx + 14, top + 90, naText(ctx, kPowerVphW), 14, kTextMuted);
    }
    pen.text(cx + 14, top + 72, std::string(t.rails) + num(f(ctx, kPowerRailsW), 2) + " W", 15, kTextMuted);

    const double maxData = h.a.maxValue();
    const double maxV = std::isnan(maxData) ? 5.0 : std::max(2.0, std::ceil(maxData * 1.2));
    const GraphArea area = graphArea(x, y, 0.0, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + "W");
    drawGuide(pen, area, th.powerWarnW, kWarn);
    drawGuide(pen, area, th.powerCritW, kDanger);
    drawSeries(pen, area, h.a, kAccent);
}

std::string statusText(const WidgetContext& ctx) {
    const WidgetText& t = texts(ctx);
    switch (static_cast<BatteryStatus>(i(ctx, kBatteryStatus))) {
        case BatteryStatus::Charging: return t.statusCharging;
        case BatteryStatus::Discharging: return t.statusDischarging;
        case BatteryStatus::Full: return t.statusFull;
        case BatteryStatus::NotCharging: return i(ctx, kBatteryChargerOnline) != 0 ? t.statusPlugged : t.statusIdle;
        case BatteryStatus::Unknown: break;
    }
    return "";
}

void drawBattery(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
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
    if (w.compact) return;  // % and the W into / from the battery
    pen.text(cx + 14, top + 88, t.batteryNote, 13, kTextMuted);
    labelValue(pen, cx + 8, right, top + 106 + kRowH, !std::isnan(watts) && watts < 0 ? t.flowOut : t.flowIn,
               vawText(f(ctx, kBatteryVoltageV), std::fabs(f(ctx, kBatteryCurrentMa)) / 1000.0, std::fabs(watts)),
               kText, 14);
}

void drawTemp(const Pen& pen, double x, double y, const WidgetInstance& w, const WidgetContext& ctx,
              const WidgetHistory& h) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double top = y + kTop;
    const double cpu = f(ctx, kTempCpuC);
    const double gpu = f(ctx, kTempGpuC);
    pen.dot(cx + 4, top + 42, kAccent);
    drawBigValue(pen, cx + 14, top + 52, num(cpu, 0), t.cpuTempUnit, colorOf(levelOf(cpu, th.tempWarnC, th.tempCritC)));
    pen.dot(cx + 4, top + 67, kGpu);
    const double gw = pen.text(cx + 14, top + 72, "GPU " + num(gpu, 0) + t.celsius, 15,
                               colorOf(levelOf(gpu, th.tempWarnC, th.tempCritC)));
    if (!w.compact)  // Compact is CPU / GPU / fan only
        pen.text(cx + 14 + gw + 10, top + 72, std::string(t.tDdr) + " " + num(f(ctx, kTempDdrC), 0) + t.celsius, 15,
                 kTextMuted);
    pen.text(cx + 14, top + 90, std::string(t.fan) + grouped(f(ctx, kFanRpm)) + " rpm", 15, kTextMuted);

    const bool cpuThrottle = i(ctx, kThrottleCpu) != 0;
    const bool gpuThrottle = i(ctx, kThrottleGpu) != 0;
    const std::string throttleText =
        std::string(t.thermalLimit) + (cpuThrottle && gpuThrottle ? t.cpuAndGpu : (cpuThrottle ? "CPU" : "GPU"));
    if (w.compact) {  // CPU / GPU °C and the fan only; the badge sits where the graph would be
        if (cpuThrottle || gpuThrottle) {
            strokeRounded(pen, x, y, kCardW, widgetHeight(w), 12, kDanger, 3.0);
            drawBadge(pen, x + kInR, top + 4, throttleText, kDanger);
        }
        return;
    }

    const double dataMin = std::fmin(h.a.minValue(), h.b.minValue());
    const double dataMax = std::fmax(h.a.maxValue(), h.b.maxValue());
    const double minV = std::isnan(dataMin) ? 30.0 : std::min(30.0, std::floor(dataMin / 5.0) * 5.0 - 5.0);
    const double maxV = std::isnan(dataMax) ? th.tempCritC + 5.0 : std::max(th.tempCritC + 5.0, dataMax + 5.0);
    const GraphArea area = graphArea(x, y, minV, maxV, ctx);
    drawGraphLabel(pen, area, num(maxV, 0) + t.celsius);
    drawGuide(pen, area, th.tempWarnC, kWarn);
    drawGuide(pen, area, th.tempCritC, kDanger);
    drawSeries(pen, area, h.b, kGpu);
    drawSeries(pen, area, h.a, kAccent);

    // The other temperatures, three per row
    const struct {
        const char* label;
        uint16_t id;
    } kSpots[kTempSpots] = {
        {t.tBattery, kTempBatteryC}, {t.tDisplay, kTempDisplayC}, {t.tExhaust, kTempExhaustC},
        {t.tHeatsink, kTempHeatsinkC},
    };
    const double colW = (kInR - kInX) / 3;
    for (int k = 0; k < kTempSpots; ++k) {
        const double sx = cx + (k % 3) * colW;
        const double sy = top + 114 + (k / 3) * kLineH;
        const double v = f(ctx, kSpots[k].id);
        labelValue(pen, sx + 8, sx + colW - 10, sy, kSpots[k].label, num(v, 0) + t.celsius,
                   colorOf(levelOf(v, th.tempWarnC, th.tempCritC)), 14);
    }

    if (cpuThrottle || gpuThrottle) {
        // The fixed panel framed the whole panel in red; Temp owns the throttle flags, so its card gets the frame
        strokeRounded(pen, x, y, kCardW, widgetHeight(w), 12, kDanger, 3.0);
        drawBadge(pen, area.x + area.w - 4, area.y + 4, throttleText, kDanger);
    }
}

bool networkExpanded(const WidgetContext& ctx) {
    return i(ctx, kNetInterfaceUp) != 0 && i(ctx, kNetConnected) != 0;
}

void drawNetwork(const Pen& pen, double x, double y, const WidgetContext& ctx) {
    const Thresholds& th = ctx.thresholds;
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX;
    const double right = x + kInR;
    const double top = y + kTop;
    const bool up = ok(ctx, kNetInterfaceUp) && i(ctx, kNetInterfaceUp) != 0;
    const bool connected = up && i(ctx, kNetConnected) != 0;
    // Which link, after the title: the dongle if it has a peer, otherwise home Wi-Fi
    const bool dongle = connected && i(ctx, kNetHomeWifi) == 0;
    pen.text(cx + pen.measure(t.network, 16) + 8, top + 17, dongle ? t.linkDongle : t.linkHome, 13, kTextMuted);
    if (!up) {
        drawWifiIcon(pen, right - 26, top + 18, 0, kTextMuted, true);
        const std::string why = ok(ctx, kNetInterfaceUp) ? std::string(t.naPrefix) + reasonText(Reason::SensorMissing)
                                                         : naText(ctx, kNetInterfaceUp);
        pen.text(cx + 8, top + 44, why, 15, kTextMuted);
        return;
    }
    const double dbm = f(ctx, kNetSignalDbm);
    const Level signalLevel = levelOf(dbm, th.wifiWarnDbm, th.wifiCritDbm, true);
    // Signal: icon + dBm at the right of the title row
    std::string signal = std::isnan(dbm) ? "--" : num(std::fabs(dbm), 0);
    if (!std::isnan(dbm) && dbm < 0) signal = "−" + signal;
    const double sw = connected ? pen.text(right, top + 17, signal + " dBm", 13, colorOf(signalLevel, kTextMuted), false, true)
                                : 0.0;
    drawWifiIcon(pen, right - sw - 32, top + 18, connected ? wifiBars(dbm, th) : 0,
                 connected ? colorOf(signalLevel) : kTextMuted, !connected);
    double lx = cx + 8;
    const double ry = top + 44;
    if (!connected) {
        pen.text(lx, ry, t.notConnected, 16, kTextMuted);
        return;
    }
    // Traffic (peer → Frame first: with Steam Link the video comes this way). Each value carries its unit
    const RateText down = formatRate(f(ctx, kNetRxMbps));
    const RateText upRate = formatRate(f(ctx, kNetTxMbps));
    lx += drawArrow(pen, lx, ry, false, kTextMuted);
    lx += pen.text(lx, ry, down.value, 17, kText, true);
    lx += pen.text(lx, ry, down.unit, 14, kTextMuted);
    lx += 8;
    lx += drawArrow(pen, lx, ry, true, kTextMuted);
    lx += pen.text(lx, ry, upRate.value, 15, kText);
    pen.text(lx, ry, upRate.unit, 14, kTextMuted);
    pen.text(cx + 8, top + 68,
             std::string(t.link) + num(f(ctx, kNetTxLinkMbps), 0) + "/" + num(f(ctx, kNetRxLinkMbps), 0) + " Mbps", 14,
             kTextMuted);
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
    const WidgetText& t = texts(ctx);
    const double cx = x + kInX + 8;
    const double top = y + kTop;
    const double valueX = cx + 110;
    double ry = top + 20 + kLineH;
    pen.text(cx, ry, t.controllers, 15, kTextMuted);
    double lx = valueX;
    lx += drawController(pen, lx, ry, t.left, kCtrlLeftPct, kCtrlLeftCharging, ctx) + 16;
    drawController(pen, lx, ry, t.right, kCtrlRightPct, kCtrlRightCharging, ctx);
    // Clock "Off" on the dashboard (clock_format 0) hides the Time row
    if (ctx.clockFormat != 0) {
        ry += kLineH;
        pen.text(cx, ry, t.time, 15, kTextMuted);
        pen.text(valueX, ry, clockText(text(ctx, kClock), ctx.clockFormat), 16, kText, true);
    }
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
    mix(&ctx.thresholds, sizeof(ctx.thresholds));
    mix(&ctx.graphSeconds, sizeof(ctx.graphSeconds));
    mix(&ctx.clockFormat, sizeof(ctx.clockFormat));
    mix(&ctx.language, sizeof(ctx.language));
    const bool hasStore = ctx.store != nullptr;
    mix(&hasStore, sizeof(hasStore));
    return h;
}

int widgetHeight(const WidgetInstance& w, const WidgetContext* ctx) {
    switch (w.type) {
        case WidgetType::Frame: return kGraphCardH;
        case WidgetType::GpuRam: return kGraphCardH;
        case WidgetType::Power: return kGraphCardH;
        // Compact cards keep only their top row (Battery: % and the W line)
        case WidgetType::Temp: return w.compact ? kGraphCardH : kTempFullH;
        case WidgetType::Cpu: return kCpuH;
        case WidgetType::Battery: return w.compact ? kBatteryCompactH : kBatteryFullH;
        case WidgetType::Network: return ctx && !networkExpanded(*ctx) ? kNetworkShortH : kNetworkFullH;
        case WidgetType::General: {
            const int rows = ctx && ctx->clockFormat == 0 ? 1 : 2;
            return static_cast<int>(kTop) + 20 + rows * static_cast<int>(kLineH) + 12;
        }
        case WidgetType::None: break;
    }
    return 0;
}

const char* widgetTitle(const WidgetInstance& w, WidgetLanguage language) {
    return titleOf(w, widgetText(language));
}

void drawWidgetStatic(const Pen& pen, double x, double y, const WidgetInstance& w, WidgetLanguage language,
                      int heightOverride) {
    staticCard(pen, x, y, heightOverride > 0 ? heightOverride : widgetHeight(w));
    if (hasGraph(w.type) && !(w.type == WidgetType::Temp && w.compact)) staticGraphBed(pen, x, y);
    staticTitle(pen, x, y, titleOf(w, widgetText(language)));
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
        case WidgetType::Battery: drawBattery(pen, x, y, w, ctx); break;
        case WidgetType::Temp: drawTemp(pen, x, y, w, ctx, history); break;
        case WidgetType::Network: drawNetwork(pen, x, y, ctx); break;
        case WidgetType::General: drawGeneral(pen, x, y, ctx); break;
        case WidgetType::None: break;
    }
}

}  // namespace fph
