// core/ layout tests: JSON round trip, bad input, and "widget off -> source refs drop" (no platform code).
#include "layout.h"
#include "scheduler.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace fph;
using W = WidgetType;

WidgetInstance inst(W type, uint32_t maxItems = 15, bool sum = false, NetAdapter adapter = NetAdapter::Dongle) {
    WidgetInstance w;
    w.type = type;
    w.maxItems = maxItems;
    w.sumSameName = sum;
    w.adapter = adapter;
    return w;
}

bool sameTypes(const LayoutColumn& column, const std::vector<W>& types) {
    if (column.widgets.size() != types.size()) return false;
    for (size_t i = 0; i < types.size(); ++i) {
        if (column.widgets[i].type != types[i]) return false;
    }
    return true;
}

void testDefaultLayout() {
    const Layout layout = defaultLayout();
    CHECK(layout.version == 1);
    CHECK(layout.columns.size() == 3);
    CHECK(sameTypes(layout.columns[0], {W::Frame, W::Cpu, W::GpuRam, W::General}));
    CHECK(sameTypes(layout.columns[1], {W::Power, W::Battery, W::Temp}));
    CHECK(sameTypes(layout.columns[2], {W::Network, W::Network, W::CpuItems, W::RamItems}));
    CHECK(layout.columns[2].widgets[0].adapter == NetAdapter::Dongle);
    CHECK(layout.columns[2].widgets[1].adapter == NetAdapter::Home);
}

void testRoundTrip() {
    // default, compact and indented
    const Layout def = defaultLayout();
    for (const int indent : {0, 2}) {
        Layout back;
        std::vector<std::string> warnings;
        std::string error;
        CHECK(parseLayoutText(layoutToJson(def, indent), back, warnings, error));
        CHECK(back.sameWidgets(def));
        CHECK(back.version == def.version);
        CHECK(warnings.empty());
    }

    // custom: lists with settings, both adapters, an empty column, version kept
    Layout custom;
    custom.version = 42;
    custom.columns.resize(3);
    custom.columns[0].widgets = {inst(W::CpuItems, 7, true), inst(W::RamItems, 50, false), inst(W::Frame)};
    custom.columns[1].widgets = {};
    custom.columns[2].widgets = {inst(W::Network, 15, false, NetAdapter::Home), inst(W::Network),
                                 inst(W::Streaming), inst(W::EyeCamera)};
    Layout back;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(parseLayoutText(layoutToJson(custom, 0), back, warnings, error));
    CHECK(back.version == 42);
    CHECK(back.columns.size() == 3);
    CHECK(back.sameWidgets(custom));
    CHECK(back.columns[0].widgets[0].maxItems == 7 && back.columns[0].widgets[0].sumSameName);
    CHECK(back.columns[0].widgets[1].maxItems == 50 && !back.columns[0].widgets[1].sumSameName);
    CHECK(back.columns[1].widgets.empty());
    CHECK(back.columns[2].widgets[0].adapter == NetAdapter::Home);
    CHECK(warnings.empty());
}

void testBadInput() {
    Layout out;
    std::vector<std::string> warnings;
    std::string error;

    // unknown widget type: skipped with a warning, the rest stays
    CHECK(parseLayoutText(R"({"version":3,"columns":[[{"type":"frame"},{"type":"bogus"},{"type":"cpu"}]]})", out,
                          warnings, error));
    CHECK(out.version == 3);
    CHECK(out.columns.size() == 1 && sameTypes(out.columns[0], {W::Frame, W::Cpu}));
    CHECK(!warnings.empty());

    // max_items clamped to 1..50 (with a warning each)
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"cpu_items","max_items":0},{"type":"ram_items","max_items":999},)"
                          R"({"type":"cpu_items","max_items":20}]]})",
                          out, warnings, error));
    CHECK(out.columns[0].widgets.size() == 3);
    CHECK(out.columns[0].widgets[0].maxItems == 1);
    CHECK(out.columns[0].widgets[1].maxItems == 50);
    CHECK(out.columns[0].widgets[2].maxItems == 20);
    CHECK(warnings.size() == 2);
    CHECK(out.version == 1);  // missing version = 1

    // columns that isn't an array: an error, `out` untouched
    const Layout before = out;
    warnings.clear();
    error.clear();
    CHECK(!parseLayoutText(R"({"version":9,"columns":5})", out, warnings, error));
    CHECK(!error.empty());
    CHECK(out.version == before.version && out.sameWidgets(before));
    (void)before;
    error.clear();
    CHECK(!parseLayoutText(R"({"version":9})", out, warnings, error));  // no columns at all
    CHECK(!error.empty());
    error.clear();
    CHECK(!parseLayoutText(R"([1,2,3])", out, warnings, error));  // not an object
    CHECK(!error.empty());
    error.clear();
    CHECK(!parseLayoutText("{ not json", out, warnings, error));
    CHECK(!error.empty());

    // one column that isn't an array: skipped with a warning, the others stay
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[{"type":"frame"},[{"type":"temp"}]]})", out, warnings, error));
    CHECK(out.columns.size() == 1 && sameTypes(out.columns[0], {W::Temp}));
    CHECK(!warnings.empty());
}

/** Reference counts the way the headset sets them: enabledWidgets(layout) + someone looking. */
struct Refs {
    Scheduler sched;
    explicit Refs(const Layout& layout) {
        DemandInputs d;
        d.editorOpen = true;
        sched.setDemand(d);
        sched.setEnabledWidgets(enabledWidgets(layout));
    }
};

void testWidgetOffDropsRefs() {
    Layout layout = defaultLayout();
    {
        Refs r(layout);
        CHECK(r.sched.refCount(SourceId::Processes) > 0);
        CHECK(r.sched.refCount(SourceId::Network) > 0);
    }

    // CPU items + RAM items removed: nothing owns the process scan any more
    Layout noLists = layout;
    for (LayoutColumn& column : noLists.columns) {
        std::vector<WidgetInstance> keep;
        for (const WidgetInstance& w : column.widgets) {
            if (w.type != W::CpuItems && w.type != W::RamItems) keep.push_back(w);
        }
        column.widgets = keep;
    }
    {
        Refs r(noLists);
        CHECK(r.sched.refCount(SourceId::Processes) == 0);
        CHECK(r.sched.refCount(SourceId::Network) > 0);
    }

    // One list left: still read
    Layout oneList = noLists;
    oneList.columns[2].widgets.push_back(inst(W::CpuItems));
    {
        Refs r(oneList);
        CHECK(r.sched.refCount(SourceId::Processes) > 0);
    }

    // Network removed
    Layout noNet = layout;
    std::vector<WidgetInstance> keep;
    for (const WidgetInstance& w : noNet.columns[2].widgets) {
        if (w.type != W::Network) keep.push_back(w);
    }
    noNet.columns[2].widgets = keep;
    {
        Refs r(noNet);
        CHECK(r.sched.refCount(SourceId::Network) == 0);
        CHECK(r.sched.refCount(SourceId::Processes) > 0);
    }

    // Empty layout: everything off
    Layout empty;
    {
        Refs r(empty);
        CHECK(r.sched.refCount(SourceId::Processes) == 0);
        CHECK(r.sched.refCount(SourceId::Network) == 0);
    }
}

}  // namespace

int main() {
    testDefaultLayout();
    testRoundTrip();
    testBadInput();
    testWidgetOffDropsRefs();
    if (failures == 0) std::printf("core-layout-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
