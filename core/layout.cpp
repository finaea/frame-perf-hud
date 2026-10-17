#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fph {

namespace {

struct KeyName {
    WidgetType type;
    const char* key;
};

// Stable: these go into saved layouts and over the link
const KeyName kKeys[] = {
    {WidgetType::Frame, "frame"},
    {WidgetType::Cpu, "cpu"},
    {WidgetType::GpuRam, "gpu_ram"},
    {WidgetType::Power, "power"},
    {WidgetType::Battery, "battery"},
    {WidgetType::Temp, "temp"},
    {WidgetType::CpuItems, "cpu_items"},
    {WidgetType::RamItems, "ram_items"},
    {WidgetType::Network, "network"},
    {WidgetType::Streaming, "streaming"},
    {WidgetType::General, "general"},
    {WidgetType::HeadsetCamera, "headset_camera"},
    {WidgetType::EyeCamera, "eye_camera"},
};

bool isList(WidgetType type) {
    return type == WidgetType::CpuItems || type == WidgetType::RamItems;
}

WidgetInstance instance(WidgetType type, NetAdapter adapter = NetAdapter::Dongle) {
    WidgetInstance w;
    w.type = type;
    w.adapter = adapter;
    return w;
}

}  // namespace

bool hasCompactView(WidgetType type) {
    return type == WidgetType::Frame || type == WidgetType::Power || type == WidgetType::Battery ||
           type == WidgetType::Temp;
}

uint8_t cameraFpsMax(WidgetType type) {
    return type == WidgetType::HeadsetCamera ? 15 : 30;
}

uint8_t cameraFpsNearest(double fps, WidgetType type) {
    uint8_t best = kCameraFpsSteps[0];
    for (uint8_t s : kCameraFpsSteps)
        if (s <= cameraFpsMax(type) && std::fabs(fps - s) < std::fabs(fps - best)) best = s;
    return best;
}

uint8_t cameraFpsStep(uint8_t fps, int direction, WidgetType type) {
    const uint8_t now = cameraFpsNearest(fps, type);
    for (size_t i = 0; i < kCameraFpsSteps.size(); ++i) {
        if (kCameraFpsSteps[i] != now) continue;
        if (direction < 0 && i > 0) return kCameraFpsSteps[i - 1];
        if (direction > 0 && i + 1 < kCameraFpsSteps.size() && kCameraFpsSteps[i + 1] <= cameraFpsMax(type))
            return kCameraFpsSteps[i + 1];
        return now;
    }
    return now;
}

Layout defaultLayout() {
    using W = WidgetType;
    Layout layout;
    layout.version = 1;
    layout.columns.resize(3);
    layout.columns[0].widgets = {instance(W::Frame), instance(W::Cpu), instance(W::GpuRam), instance(W::General)};
    layout.columns[1].widgets = {instance(W::Power), instance(W::Battery), instance(W::Temp)};
    layout.columns[2].widgets = {instance(W::Network, NetAdapter::Dongle), instance(W::Network, NetAdapter::Home),
                                 instance(W::CpuItems), instance(W::RamItems)};
    return layout;
}

std::vector<WidgetType> enabledWidgets(const Layout& layout) {
    std::vector<WidgetType> out;
    for (const LayoutColumn& column : layout.columns) {
        for (const WidgetInstance& w : column.widgets) out.push_back(w.type);
    }
    return out;
}

const char* widgetKey(WidgetType type) {
    for (const KeyName& k : kKeys) {
        if (k.type == type) return k.key;
    }
    return nullptr;
}

bool parseWidgetKey(const std::string& key, WidgetType& out) {
    for (const KeyName& k : kKeys) {
        if (key == k.key) {
            out = k.type;
            return true;
        }
    }
    return false;
}

std::string layoutToJson(const Layout& layout, int indent) {
    const bool pretty = indent > 0;
    const std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
    const std::string nl = pretty ? "\n" : "";
    const std::string in1 = pretty ? pad + "  " : "";
    const std::string in2 = pretty ? pad + "    " : "";
    const std::string sp = pretty ? " " : "";
    std::string out = "{" + nl + in1 + "\"version\":" + sp + std::to_string(layout.version) + "," + nl + in1 +
                      "\"columns\":" + sp + "[";
    for (size_t c = 0; c < layout.columns.size(); ++c) {
        out += (c ? "," : "") + nl + in2 + "[";
        const std::vector<WidgetInstance>& widgets = layout.columns[c].widgets;
        for (size_t i = 0; i < widgets.size(); ++i) {
            const WidgetInstance& w = widgets[i];
            const char* key = widgetKey(w.type);
            if (key == nullptr) continue;
            out += std::string(i ? "," : "") + sp + "{" + sp + "\"type\":" + sp + "\"" + key + "\"";
            if (isList(w.type)) {
                out += "," + sp + "\"max_items\":" + sp + std::to_string(w.maxItems) + "," + sp +
                       "\"sum_same_name\":" + sp + (w.sumSameName ? "true" : "false");
            }
            if (w.type == WidgetType::Network) {
                out += "," + sp + "\"adapter\":" + sp + (w.adapter == NetAdapter::Home ? "\"home\"" : "\"dongle\"");
            }
            if (w.type == WidgetType::HeadsetCamera) {  // T10
                out += "," + sp + "\"cameras\":" + sp + std::to_string(w.cameras) + "," + sp +
                       "\"full_size\":" + sp + (w.cameraFullSize ? "true" : "false");
            }
            if (w.type == WidgetType::HeadsetCamera || w.type == WidgetType::EyeCamera) {  // T10
                out += "," + sp + "\"fps\":" + sp + std::to_string(w.cameraFps);
            }
            // R2: only when on, so a layout without them reads the same as before (and older apps don't warn)
            if (w.compact && hasCompactView(w.type)) out += "," + sp + "\"compact\":" + sp + "true";
            if (w.hideSystem && isList(w.type)) out += "," + sp + "\"hide_system\":" + sp + "true";
            out += sp + "}";
        }
        out += (widgets.empty() ? "" : sp) + "]";
    }
    out += nl + in1 + "]" + nl + pad + "}";
    return out;
}

bool layoutFromJson(const JsonValue& value, Layout& out, std::vector<std::string>& warnings, std::string& error) {
    if (!value.isObject()) {
        error = "layout must be { \"version\": N, \"columns\": [...] }";
        return false;
    }
    const JsonValue* columns = value.get("columns");
    if (columns == nullptr || columns->type != JsonValue::Type::Array) {
        error = "layout.columns must be an array of columns";
        return false;
    }
    Layout layout;
    layout.version = 1;
    if (const JsonValue* version = value.get("version")) {
        if (version->isNumber() && version->number >= 1 && version->number <= 4294967295.0) {
            layout.version = static_cast<uint32_t>(version->number);
        } else {
            warnings.push_back("layout.version must be a whole number >= 1, using 1");
        }
    }
    for (const auto& member : value.members) {
        if (member.first != "version" && member.first != "columns") {
            warnings.push_back("unknown key: layout." + member.first);
        }
    }
    for (size_t c = 0; c < columns->items.size(); ++c) {
        const JsonValue& column = columns->items[c];
        const std::string where = "layout.columns[" + std::to_string(c) + "]";
        if (layout.columns.size() >= kMaxColumns) {
            warnings.push_back(where + ": more than " + std::to_string(kMaxColumns) + " columns, the rest are skipped");
            break;
        }
        if (column.type != JsonValue::Type::Array) {
            warnings.push_back(where + " must be an array of widgets, skipped");
            continue;
        }
        LayoutColumn out1;
        for (size_t i = 0; i < column.items.size(); ++i) {
            const JsonValue& item = column.items[i];
            const std::string at = where + "[" + std::to_string(i) + "]";
            if (out1.widgets.size() >= kMaxWidgetsPerColumn) {
                warnings.push_back(at + ": more than " + std::to_string(kMaxWidgetsPerColumn) +
                                   " widgets in one column, the rest are skipped");
                break;
            }
            const JsonValue* type = item.isObject() ? item.get("type") : nullptr;
            WidgetInstance w;
            if (type == nullptr || !type->isString() || !parseWidgetKey(type->text, w.type)) {
                warnings.push_back(at + ": unknown widget" +
                                   (type != nullptr && type->isString() ? " \"" + type->text + "\"" : "") + ", skipped");
                continue;
            }
            for (const auto& member : item.members) {
                const std::string& k = member.first;
                const bool known = k == "type" ||
                                   ((k == "max_items" || k == "sum_same_name" || k == "hide_system") && isList(w.type)) ||
                                   (k == "compact" && hasCompactView(w.type)) ||
                                   (k == "adapter" && w.type == WidgetType::Network) ||
                                   ((k == "cameras" || k == "full_size") && w.type == WidgetType::HeadsetCamera) ||
                                   (k == "fps" &&
                                    (w.type == WidgetType::HeadsetCamera || w.type == WidgetType::EyeCamera));
                if (!known) warnings.push_back(at + ": unknown key " + k + " for " + type->text);
            }
            if (isList(w.type)) {
                if (const JsonValue* max = item.get("max_items")) {
                    if (max->isNumber()) {
                        const double clamped = std::clamp(std::floor(max->number), 1.0, double(kMaxListItems));
                        if (clamped != max->number) warnings.push_back(at + ".max_items clamped to " +
                                                                       std::to_string(int(clamped)));
                        w.maxItems = static_cast<uint32_t>(clamped);
                    } else {
                        warnings.push_back(at + ".max_items must be a number");
                    }
                }
                if (const JsonValue* sum = item.get("sum_same_name")) {
                    if (sum->isBool()) w.sumSameName = sum->boolean;
                    else warnings.push_back(at + ".sum_same_name must be true or false");
                }
                if (const JsonValue* hide = item.get("hide_system")) {  // R2
                    if (hide->isBool()) w.hideSystem = hide->boolean;
                    else warnings.push_back(at + ".hide_system must be true or false");
                }
            }
            if (hasCompactView(w.type)) {  // R2
                if (const JsonValue* compact = item.get("compact")) {
                    if (compact->isBool()) w.compact = compact->boolean;
                    else warnings.push_back(at + ".compact must be true or false");
                }
            }
            if (w.type == WidgetType::Network) {
                if (const JsonValue* adapter = item.get("adapter")) {
                    if (adapter->isString() && adapter->text == "home") w.adapter = NetAdapter::Home;
                    else if (adapter->isString() && adapter->text == "dongle") w.adapter = NetAdapter::Dongle;
                    else warnings.push_back(at + ".adapter must be \"dongle\" or \"home\"");
                }
            }
            if (w.type == WidgetType::HeadsetCamera) {  // T10
                if (const JsonValue* cams = item.get("cameras")) {
                    // a mask of the 4 outward cams; none at all would be an empty card, so that falls back to cam 0
                    const double mask = cams->isNumber() ? std::floor(cams->number) : -1.0;
                    if (mask >= 1.0 && mask <= 15.0) w.cameras = static_cast<uint8_t>(mask);
                    else warnings.push_back(at + ".cameras must be a mask 1-15 (bit n = outward cam n)");
                }
                if (const JsonValue* full = item.get("full_size")) {
                    if (full->isBool()) w.cameraFullSize = full->boolean;
                    else warnings.push_back(at + ".full_size must be true or false");
                }
            }
            if (w.type == WidgetType::HeadsetCamera || w.type == WidgetType::EyeCamera) {  // T10
                if (const JsonValue* fps = item.get("fps")) {
                    if (fps->isNumber()) {
                        const uint8_t snapped = cameraFpsNearest(fps->number, w.type);
                        if (snapped != fps->number)
                            warnings.push_back(at + ".fps must be 5, 10, 15" +
                                               (w.type == WidgetType::EyeCamera ? " or 30" : "") + ": using " +
                                               std::to_string(snapped));
                        w.cameraFps = snapped;
                    } else {
                        warnings.push_back(at + ".fps must be a number");
                    }
                }
            }
            out1.widgets.push_back(w);
        }
        layout.columns.push_back(std::move(out1));
    }
    out = std::move(layout);
    return true;
}

bool parseLayoutText(const std::string& text, Layout& out, std::vector<std::string>& warnings, std::string& error) {
    JsonValue root;
    if (!parseJson(text, root, error)) return false;
    return layoutFromJson(root, out, warnings, error);
}

}  // namespace fph
