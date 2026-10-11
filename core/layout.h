// The widget grid layout: columns, each an ordered list of widget instances, plus a version number.
// Shared by the headset (config.json's "layout" key) and the PC app (LAYOUT_GET / LAYOUT_SET, ticket 06/07).
// A widget that isn't in the layout is off: its sources lose their reference (Scheduler::setEnabledWidgets).
#pragma once

#include "json.h"
#include "metric_registry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fph {

/** Which Wi-Fi adapter a Network widget shows (metric blocks 910 / 930). */
enum class NetAdapter : uint8_t {
    Dongle = 0,  ///< `wlanap`, the Steam Link dongle's AP
    Home = 1,    ///< `wlan0`, home Wi-Fi
};

/** Most process rows a list widget may show. */
constexpr uint32_t kMaxListItems = 50;
/** Most columns a layout may hold (8 × 512 px is already twice the 2048 cap). */
constexpr size_t kMaxColumns = 8;
/** Most widgets one column may hold. */
constexpr size_t kMaxWidgetsPerColumn = 16;

/** One widget in the grid, with its own settings. Settings a type doesn't use keep their defaults. */
struct WidgetInstance {
    WidgetType type = WidgetType::None;
    uint32_t maxItems = 15;           ///< CPU / RAM usage items: rows shown
    bool sumSameName = false;         ///< CPU / RAM usage items: one row per process name
    NetAdapter adapter = NetAdapter::Dongle;  ///< Network

    bool operator==(const WidgetInstance& o) const {
        return type == o.type && maxItems == o.maxItems && sumSameName == o.sumSameName && adapter == o.adapter;
    }
    bool operator!=(const WidgetInstance& o) const { return !(*this == o); }
};

/** One column, top to bottom. */
struct LayoutColumn {
    std::vector<WidgetInstance> widgets;

    bool operator==(const LayoutColumn& o) const { return widgets == o.widgets; }
    bool operator!=(const LayoutColumn& o) const { return !(*this == o); }
};

/** The whole grid. */
struct Layout {
    /** Bumped on every applied edit; the link's compare-and-set (LAYOUT_SET base_version) checks it. */
    uint32_t version = 1;
    std::vector<LayoutColumn> columns;

    /** Same widgets in the same places (the version isn't compared). */
    bool sameWidgets(const Layout& o) const { return columns == o.columns; }
};

/**
 * The first-run layout: every M1 widget (1-9 and 11, Network once per adapter) in three columns.
 * @return the layout, version 1
 */
Layout defaultLayout();

/**
 * @param layout a layout
 * @return one entry per widget instance (a type may repeat), for Scheduler::setEnabledWidgets
 */
std::vector<WidgetType> enabledWidgets(const Layout& layout);

/**
 * @param type a widget type
 * @return its key in the layout JSON ("frame", "cpu_items", ...), or nullptr for None / unknown
 */
const char* widgetKey(WidgetType type);

/**
 * @param key a layout JSON key
 * @param out the type
 * @return true if the key is known
 */
bool parseWidgetKey(const std::string& key, WidgetType& out);

/**
 * Writes the layout as a JSON object.
 * @param layout the layout
 * @param indent spaces before each line after the first (0 = compact, for the link)
 * @return `{ "version": N, "columns": [ [ {...}, ... ], ... ] }`
 */
std::string layoutToJson(const Layout& layout, int indent = 0);

/**
 * Reads a layout from the JSON object layoutToJson writes. Unknown widget types and keys are skipped with a
 * warning (a newer PC app may know more types); settings out of range are clamped with a warning.
 * @param value the object
 * @param out the layout (unchanged on failure)
 * @param warnings things noticed but tolerated
 * @param error why it failed
 * @return false if the value isn't a layout at all (not an object, "columns" missing or not an array)
 */
bool layoutFromJson(const JsonValue& value, Layout& out, std::vector<std::string>& warnings, std::string& error);

/**
 * parseJson + layoutFromJson.
 * @return false if the text isn't valid JSON or not a layout
 */
bool parseLayoutText(const std::string& text, Layout& out, std::vector<std::string>& warnings, std::string& error);

}  // namespace fph
