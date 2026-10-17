#include "pc_pages.h"

#include "theme.h"

#include <cairo.h>

#include <algorithm>
#include <cstdio>

namespace fph {
namespace pc {

namespace {

PageAction rowAction(size_t i) {
    return static_cast<PageAction>(static_cast<int>(PageAction::Row0) + static_cast<int>(i));
}

bool isRow(PageAction a) {
    return a >= PageAction::Row0 && a <= PageAction::Row7;
}

/** Greedy word wrap. */
std::vector<std::string> wrap(const Pen& pen, const std::string& text, double size, double maxW) {
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t space = text.find(' ', pos);
        if (space == std::string::npos) space = text.size();
        const std::string word = text.substr(pos, space - pos);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && pen.measure(candidate, size) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        pos = space + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

/** Wrapped text from baseline y. @return the next baseline */
double paragraph(const Pen& pen, double x, double y, double w, const std::string& text, double size, Color c,
                 bool bold = false) {
    for (const std::string& line : wrap(pen, text, size, w)) {
        pen.text(x, y, line, fitSize(pen, line, size, size * 0.7, w, bold), c, bold);
        y += size * 1.4;
    }
    return y;
}

}  // namespace

PageBox drawStatusSaveButton(const Pen& pen, double width, double barH, double s) {
    static const char* const kLabel = "Save Logs";
    const double bw = pen.measure(kLabel, 12.5 * s, true) + 24 * s;
    const double bh = barH - 8 * s;
    const PageBox b {width - bw - 6 * s, 4 * s, bw, bh};
    pen.color(kControl);
    pen.roundedRect(b.x, b.y, bw, bh, bh / 2);
    cairo_fill(pen.cr);
    strokeRounded(pen, b.x, b.y, bw, bh, bh / 2, kBorder, 1.5 * s);
    textCentered(pen, b.x + bw / 2, centerBaseline(b.y, bh, 12.5 * s), kLabel, 12.5 * s, kText, true);
    return b;
}

void PcPages::setTab(PcTab tab) {
    tab_ = tab;
    armed_ = PageAction::None;
}

void PcPages::drawTabs(const Pen& pen, double x, double y, double s) {
    const double w = 330 * s, h = 36 * s;
    const double top = y + (tabBarHeight(s) - h) / 2;
    std::vector<PillPart> parts(3);
    parts[0].label = "Layout";
    parts[1].label = "Profiles";
    parts[2].label = "Logs";
    const int selected = tab_ == PcTab::Layout ? 0 : tab_ == PcTab::Profiles ? 1 : 2;
    drawSegmentedPill(pen, x, top, w, h, parts, selected, true, 15 * s, s);
    tabBoxes_.clear();
    const PageAction actions[3] = {PageAction::TabLayout, PageAction::TabProfiles, PageAction::TabLogs};
    for (int i = 0; i < 3; ++i) tabBoxes_.push_back({actions[i], {x + i * w / 3, top, w / 3, h}});
}

void PcPages::button(const Pen& pen, PageAction action, const std::string& label, const PageBox& b, double s,
                     bool enabled, bool primary) {
    boxes_.push_back({action, b});
    if (!enabled) disabled_.push_back(action);
    const bool armed = enabled && armed_ == action;
    const double r = 10 * s;
    if (armed) {
        pen.color(kDanger);
    } else if (enabled && primary) {
        pen.color(kAccent);
    } else {
        pen.color(kControl);
    }
    pen.roundedRect(b.x, b.y, b.w, b.h, r);
    cairo_fill(pen.cr);
    if (enabled && !armed && !primary) strokeRounded(pen, b.x, b.y, b.w, b.h, r, kBorder, 1.5 * s);
    const Color fg = !enabled ? kTextDisabled : (armed || primary) ? kOnAccent : kText;
    const double size = fitSize(pen, label, 14 * s, 10 * s, b.w - 16 * s, true);
    textCentered(pen, b.x + b.w / 2, centerBaseline(b.y, b.h, size), label, size, fg, true);
}

void PcPages::drawPage(const Pen& pen, const PageBox& area, double s, const PagesView& view) {
    boxes_.clear();
    disabled_.clear();
    if (tab_ == PcTab::Profiles) drawProfiles(pen, area, s, view);
    if (tab_ == PcTab::Logs) drawLogs(pen, area, s, view);
}

void PcPages::drawProfiles(const Pen& pen, const PageBox& area, double s, const PagesView& view) {
    const double gap = 12 * s, pad = 16 * s;
    const double listW = std::min(area.w * 0.42, 380 * s);
    const double x2 = area.x + listW + gap, w2 = area.w - listW - gap;
    drawCard(pen, area.x, area.y, listW, area.h, 14 * s, kCard, kCard, 0);
    drawCard(pen, x2, area.y, w2, area.h, 14 * s, kCard, kCard, 0);
    pen.text(area.x + pad, area.y + 30 * s, "Profiles", 17 * s, kText, true);
    const link::ControlState* h = view.headset;
    if (h == nullptr) {
        rowIds_.clear();
        selected_.clear();
        paragraph(pen, area.x + pad, area.y + 64 * s, listW - 2 * pad,
                  view.subscribed ? "This headset's HUD is older than this app: update it to manage its profiles here"
                                  : "Profiles live on the headset. Connect to it to see them",
                  14 * s, kTextMuted);
        return;
    }
    char count[32];
    std::snprintf(count, sizeof(count), "%zu / %u", h->profiles.size(), static_cast<unsigned>(h->maxProfiles));
    pen.text(area.x + listW - pad, area.y + 30 * s, count, 13 * s, kTextMuted, false, true);

    // The list (selection stays on an id that still exists, else the active one, else the first)
    rowIds_.clear();
    for (const auto& p : h->profiles) rowIds_.push_back(p.id);
    const auto has = [&](const std::string& id) { return std::find(rowIds_.begin(), rowIds_.end(), id) != rowIds_.end(); };
    if (!has(selected_)) selected_ = has(h->activeId) ? h->activeId : rowIds_.empty() ? std::string() : rowIds_.front();
    const bool unsaved = (h->flags & link::kControlUnsaved) != 0;
    const double rowH = 32 * s, rowGap = 4 * s;
    const double rx = area.x + pad, rw = listW - 2 * pad;
    if (h->profiles.empty()) {
        paragraph(pen, rx, area.y + 64 * s, rw, "No profiles yet. New profile saves the headset's layout", 14 * s,
                  kTextMuted);
    }
    for (size_t i = 0; i < h->profiles.size() && i < 8; ++i) {
        const auto& p = h->profiles[i];
        const PageBox b {rx, area.y + 44 * s + static_cast<double>(i) * (rowH + rowGap), rw, rowH};
        boxes_.push_back({rowAction(i), b});
        const bool selected = p.id == selected_;
        const bool active = p.id == h->activeId;
        pen.color(selected ? kControlHover : kControl);
        pen.roundedRect(b.x, b.y, b.w, b.h, 8 * s);
        cairo_fill(pen.cr);
        if (selected) strokeRounded(pen, b.x, b.y, b.w, b.h, 8 * s, kAccent, 2 * s);
        double right = b.x + b.w - 10 * s;
        if (active) {
            if (unsaved) right -= pen.text(right, centerBaseline(b.y, b.h, 12 * s), "unsaved changes", 12 * s, kWarn, true, true) + 10 * s;
            right -= pen.text(right, centerBaseline(b.y, b.h, 12 * s), "Active", 12 * s, kAccentText, true, true) + 12 * s;
        }
        const double size = fitSize(pen, p.name, 14 * s, 10 * s, right - (b.x + 12 * s), active);
        pen.text(b.x + 12 * s, centerBaseline(b.y, b.h, size), p.name, size, kText, active);
    }

    // The selected profile's buttons
    std::string title = "-";
    for (const auto& p : h->profiles) {
        if (p.id == selected_) title = p.name;
    }
    const double bx = x2 + pad, bw = w2 - 2 * pad;
    pen.text(bx, area.y + 30 * s, title, fitSize(pen, title, 17 * s, 11 * s, bw, true), kText, true);
    const bool live = view.subscribed && !view.busy;
    const bool haveSelected = !selected_.empty();
    const bool haveActive = has(h->activeId);
    const double cw = (bw - 2 * gap) / 3, ch = 36 * s;
    const double r0 = area.y + 46 * s, r1 = r0 + ch + 10 * s;
    const auto label = [&](PageAction a, const char* normal) -> std::string {
        if (armed_ != a) return normal;
        return a == PageAction::ProfileDelete ? "Tap again to delete" : "Tap again";
    };
    button(pen, PageAction::ProfileUse, label(PageAction::ProfileUse, "Use"), {bx, r0, cw, ch}, s, live && haveSelected, true);
    button(pen, PageAction::ProfileRename, "Rename", {bx + cw + gap, r0, cw, ch}, s, live && haveSelected, false);
    button(pen, PageAction::ProfileDelete, label(PageAction::ProfileDelete, "Delete"), {bx + 2 * (cw + gap), r0, cw, ch}, s,
           live && haveSelected, false);
    button(pen, PageAction::ProfileNew, "New profile", {bx, r1, cw, ch}, s, live && h->profiles.size() < h->maxProfiles,
           false);
    button(pen, PageAction::ProfileSave, "Save", {bx + cw + gap, r1, cw, ch}, s, live && haveActive, false);
    button(pen, PageAction::ProfileReset, label(PageAction::ProfileReset, "Reset to default"), {bx + 2 * (cw + gap), r1, cw, ch},
           s, live, false);

    double y = r1 + ch + 30 * s;
    std::string activeName;
    for (const auto& p : h->profiles) {
        if (p.id == h->activeId) activeName = p.name;
    }
    if (haveActive) {
        y = paragraph(pen, bx, y, bw, "Active: " + activeName + (unsaved ? " · unsaved changes" : ""), 14 * s,
                      unsaved ? kWarn : kTextMuted, true);
    } else if (unsaved) {
        y = paragraph(pen, bx, y, bw, "The headset's layout isn't saved in a profile", 14 * s, kWarn, true);
    }
    if (armed_ == PageAction::ProfileUse || armed_ == PageAction::ProfileReset) {
        y = paragraph(pen, bx, y + 4 * s, bw, "Tap again: the headset's layout changes are lost", 13 * s, kDanger, true);
    }
    if (view.busy) y = paragraph(pen, bx, y + 4 * s, bw, "Waiting for the headset…", 13 * s, kTextMuted);
    if (!view.headsetMessage.empty()) y = paragraph(pen, bx, y + 4 * s, bw, view.headsetMessage, 13 * s, kText);
    if (!view.subscribed) paragraph(pen, bx, y + 4 * s, bw, "Not connected to the headset", 13 * s, kTextMuted);
    paragraph(pen, bx, area.y + area.h - 18 * s, bw,
              "Profiles live on the headset: the layout only (columns, widgets, their settings)", 12 * s, kTextMuted);
}

void PcPages::drawLogs(const Pen& pen, const PageBox& area, double s, const PagesView& view) {
    const double gap = 12 * s, pad = 16 * s;
    const double w1 = (area.w - gap) / 2;
    const double x2 = area.x + w1 + gap, w2 = area.w - w1 - gap;
    drawCard(pen, area.x, area.y, w1, area.h, 14 * s, kCard, kCard, 0);
    drawCard(pen, x2, area.y, w2, area.h, 14 * s, kCard, kCard, 0);

    // This PC: Save, the folder, the last session's card
    const double ax = area.x + pad, aw = w1 - 2 * pad;
    pen.text(ax, area.y + 30 * s, "This PC", 17 * s, kText, true);
    const double bw = (aw - gap) / 2, bh = 36 * s;
    button(pen, PageAction::SaveLocal, "Save Logs", {ax, area.y + 46 * s, bw, bh}, s, true, true);
    button(pen, PageAction::OpenFolder, "Open logs folder", {ax + bw + gap, area.y + 46 * s, bw, bh}, s, true, false);
    double y = paragraph(pen, ax, area.y + 46 * s + bh + 22 * s, aw,
                         "Save Logs writes the last 10 minutes of metrics and the headset session's summary to this "
                         "PC's logs folder, right now",
                         12 * s, kTextMuted) + 8 * s;
    if (!view.pcMessage.empty()) y = paragraph(pen, ax, y, aw, view.pcMessage, 13 * s, kText) + 6 * s;
    pen.text(ax, y + 6 * s, "Last session", 15 * s, kText, true);
    y += 30 * s;
    if (view.lastCard.empty()) {
        y = paragraph(pen, ax, y, aw, "No session saved since this app started", 13 * s, kTextMuted);
    } else {
        for (size_t i = 0; i < view.lastCard.size(); ++i) {
            y = paragraph(pen, ax, y, aw, view.lastCard[i], (i == 0 ? 14 : 13) * s, kText, i == 0) + 2 * s;
        }
    }
    paragraph(pen, ax, area.y + area.h - 18 * s, aw, "Files: " + view.dataDir, 12 * s, kTextMuted);

    // The headset: Record, the crash recorder, the last power-off
    const double hx = x2 + pad, hw = w2 - 2 * pad;
    pen.text(hx, area.y + 30 * s, "Headset", 17 * s, kText, true);
    const link::ControlState* h = view.headset;
    const bool live = h != nullptr && view.subscribed && !view.busy;
    const double labelW = 130 * s, pillW = std::min(200 * s, hw - labelW), pillH = 34 * s;
    const auto pill = [&](double top, const char* label, PageAction on, PageAction off, bool isOn) {
        pen.text(hx, centerBaseline(top, pillH, 14 * s), label, 14 * s, kTextMuted);
        std::vector<PillPart> parts(2);
        parts[0].label = "On";
        parts[1].label = "Off";
        drawSegmentedPill(pen, hx + labelW, top, pillW, pillH, parts, h == nullptr ? -1 : isOn ? 0 : 1, live, 14 * s, s);
        boxes_.push_back({on, {hx + labelW, top, pillW / 2, pillH}});
        boxes_.push_back({off, {hx + labelW + pillW / 2, top, pillW / 2, pillH}});
        if (!live) {
            disabled_.push_back(on);
            disabled_.push_back(off);
        }
    };
    const bool recording = h != nullptr && (h->flags & link::kControlRecording) != 0;
    const bool crash = h != nullptr && (h->flags & link::kControlCrashRecorder) != 0;
    // Each switch has its hint underneath, shown connected or not so the switches never move
    const double recordTop = area.y + 46 * s;
    pill(recordTop, "Record", PageAction::RecordOn, PageAction::RecordOff, recording);
    y = paragraph(pen, hx, recordTop + pillH + 20 * s, hw,
                  "Writes the metrics to a CSV file on the headset once a second until you turn it off, even with the "
                  "HUD hidden",
                  12 * s, kTextMuted);
    const double crashTop = y - 2 * s;
    pill(crashTop, "Crash recorder", PageAction::CrashOn, PageAction::CrashOff, crash);
    y = paragraph(pen, hx, crashTop + pillH + 20 * s, hw,
                  "Keeps the last 10 minutes of battery, power and temperatures on the headset's disk, so a crash or "
                  "power cut leaves a log",
                  12 * s, kTextMuted) + 10 * s;
    if (h == nullptr) {
        y = paragraph(pen, hx, y, hw,
                      view.subscribed ? "This headset's HUD is older than this app: update it to control Record and "
                                        "the crash recorder here"
                                      : "Not connected to the headset",
                      13 * s, kTextMuted);
    } else {
        if (!h->recordStatus.empty()) y = paragraph(pen, hx, y, hw, h->recordStatus, 13 * s, recording ? kDanger : kText, recording);
        if (!h->crashStatus.empty()) y = paragraph(pen, hx, y, hw, h->crashStatus, 13 * s, kWarn, true);
        y += 8 * s;
        pen.text(hx, y, "Last power-off", 13 * s, kTextMuted);
        y = paragraph(pen, hx, y + 20 * s, hw, h->powerOff.empty() ? "unknown" : h->powerOff, 14 * s, kText, true);
        if (view.busy) y = paragraph(pen, hx, y + 6 * s, hw, "Waiting for the headset…", 13 * s, kTextMuted);
        if (!view.headsetMessage.empty()) paragraph(pen, hx, y + 6 * s, hw, view.headsetMessage, 13 * s, kText);
    }
}

PageAction PcPages::press(double x, double y, double now, const PagesView& view) {
    lastConfirmed_ = false;
    for (const auto& t : tabBoxes_) {
        if (!t.second.contains(x, y)) continue;
        setTab(t.first == PageAction::TabLayout ? PcTab::Layout : t.first == PageAction::TabProfiles ? PcTab::Profiles : PcTab::Logs);
        return t.first;
    }
    if (tab_ == PcTab::Layout) return PageAction::None;
    PageAction hit = PageAction::None;
    for (const auto& b : boxes_) {
        if (b.second.contains(x, y)) hit = b.first;
    }
    if (hit != armed_) armed_ = PageAction::None;  // any other press cancels a pending second tap
    if (hit == PageAction::None || disabled(hit)) return PageAction::None;
    if (isRow(hit)) {
        const size_t i = static_cast<size_t>(hit) - static_cast<size_t>(PageAction::Row0);
        if (i < rowIds_.size()) selected_ = rowIds_[i];
        return PageAction::None;
    }
    // The second tap of an armed button (armed here, or by the headset's NeedsConfirm) confirms
    if (armed_ == hit && now <= armedUntil_) {
        armed_ = PageAction::None;
        lastConfirmed_ = true;
        confirmVersion_ = armedVersion_;
        return hit;
    }
    const bool unsaved = view.headset != nullptr && (view.headset->flags & link::kControlUnsaved) != 0;
    const bool twoTaps = hit == PageAction::ProfileDelete ||
                         ((hit == PageAction::ProfileUse || hit == PageAction::ProfileReset) && unsaved);
    if (twoTaps) {
        armed_ = hit;
        armedUntil_ = now + kArmSec;
        armedVersion_ = view.headset != nullptr ? view.headset->layoutVersion : 0;
        return PageAction::None;
    }
    confirmVersion_ = view.headset != nullptr ? view.headset->layoutVersion : 0;
    return hit;
}

void PcPages::armFromHeadset(PageAction action, uint32_t version, double now) {
    armed_ = action;
    armedUntil_ = now + kArmSec;
    armedVersion_ = version;
}

bool PcPages::tick(double now) {
    if (armed_ == PageAction::None || now <= armedUntil_) return false;
    armed_ = PageAction::None;
    return true;
}

bool PcPages::boxOf(PageAction action, PageBox& out) const {
    for (const auto& list : {&tabBoxes_, &boxes_}) {
        for (const auto& b : *list) {
            if (b.first == action) {
                out = b.second;
                return true;
            }
        }
    }
    return false;
}

bool PcPages::disabled(PageAction action) const {
    return std::find(disabled_.begin(), disabled_.end(), action) != disabled_.end();
}

}  // namespace pc
}  // namespace fph
