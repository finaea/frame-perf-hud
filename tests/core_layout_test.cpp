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

WidgetInstance inst(W type, bool compact = false) {
    WidgetInstance w;
    w.type = type;
    w.compact = compact;
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
    CHECK(layout.columns.size() == 1);
    CHECK(sameTypes(layout.columns[0], {W::Frame, W::Power, W::Temp, W::Cpu, W::GpuRam, W::Network, W::Battery,
                                        W::General}));
    // only Battery is the short card
    for (const WidgetInstance& w : layout.columns[0].widgets) CHECK(w.compact == (w.type == W::Battery));
    CHECK(hasCompactView(W::Battery) && hasCompactView(W::Temp));
    CHECK(!hasCompactView(W::Frame) && !hasCompactView(W::Network) && !hasCompactView(W::General));
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

    // custom: several columns, an empty column, the version kept
    Layout custom;
    custom.version = 42;
    custom.columns.resize(3);
    custom.columns[0].widgets = {inst(W::Cpu), inst(W::GpuRam), inst(W::Frame)};
    custom.columns[1].widgets = {};
    custom.columns[2].widgets = {inst(W::Network), inst(W::Temp, true), inst(W::General)};
    Layout back;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(parseLayoutText(layoutToJson(custom, 0), back, warnings, error));
    CHECK(back.version == 42);
    CHECK(back.columns.size() == 3);
    CHECK(back.sameWidgets(custom));
    CHECK(back.columns[1].widgets.empty());
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

    // widgets that only exist in other versions are skipped the same way
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"cpu_items"},{"type":"streaming"},{"type":"temp"}]]})", out,
                          warnings, error));
    CHECK(out.columns[0].widgets.size() == 1 && out.columns[0].widgets[0].type == W::Temp);
    CHECK(warnings.size() == 2);
    CHECK(out.version == 1);  // missing version = 1

    // columns that isn't an array: an error, `out` untouched
    const Layout before = out;
    warnings.clear();
    error.clear();
    CHECK(!parseLayoutText(R"({"version":9,"columns":5})", out, warnings, error));
    CHECK(!error.empty());
    CHECK(out.version == before.version && out.sameWidgets(before));
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
        CHECK(r.sched.refCount(SourceId::Network) > 0);
        CHECK(r.sched.refCount(SourceId::CpuLoad) > 0);
        CHECK(r.sched.refCount(SourceId::Clock) > 0);
    }

    // Network removed: its source loses the reference, the others keep theirs
    Layout noNet = layout;
    std::vector<WidgetInstance> keep;
    for (const WidgetInstance& w : noNet.columns[0].widgets) {
        if (w.type != W::Network) keep.push_back(w);
    }
    noNet.columns[0].widgets = keep;
    {
        Refs r(noNet);
        CHECK(r.sched.refCount(SourceId::Network) == 0);
        CHECK(r.sched.refCount(SourceId::CpuLoad) > 0);
    }

    // Empty layout: everything off
    Layout empty;
    {
        Refs r(empty);
        CHECK(r.sched.refCount(SourceId::Network) == 0);
        CHECK(r.sched.refCount(SourceId::CpuLoad) == 0);
    }
}

// The compact key
void testCompact() {
    // round trip, the key only emitted when on
    Layout l;
    l.columns.resize(1);
    l.columns[0].widgets = {inst(W::Battery, true), inst(W::Temp), inst(W::Power)};
    const std::string text = layoutToJson(l, 0);
    size_t count = 0;
    for (size_t at = text.find("compact"); at != std::string::npos; at = text.find("compact", at + 1)) ++count;
    CHECK(count == 1);
    CHECK(text.find("\"compact\":true") != std::string::npos);

    Layout back;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(parseLayoutText(text, back, warnings, error));
    CHECK(warnings.empty());
    CHECK(back.sameWidgets(l));
    CHECK(back.columns[0].widgets[0].compact && !back.columns[0].widgets[1].compact);

    // a type without a compact card never writes the key, and warns when it reads one
    Layout noCompact;
    noCompact.columns.resize(1);
    noCompact.columns[0].widgets = {inst(W::Frame, true)};
    CHECK(layoutToJson(noCompact, 0).find("compact") == std::string::npos);
    Layout wrong;
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"cpu","compact":true},{"type":"general","compact":true}]]})",
                          wrong, warnings, error));
    CHECK(warnings.size() == 2);
    CHECK(wrong.columns[0].widgets.size() == 2);
    for (const WidgetInstance& w : wrong.columns[0].widgets) CHECK(!w.compact);

    // non-bool value: warns, stays false
    Layout bad;
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"temp","compact":"yes"}]]})", bad, warnings, error));
    CHECK(warnings.size() == 1);
    CHECK(!bad.columns[0].widgets[0].compact);

    // operator== sees the field
    WidgetInstance a = inst(W::Battery), b = inst(W::Battery);
    CHECK(a == b);
    b.compact = true;
    CHECK(a != b);
}

}  // namespace

int main() {
    testDefaultLayout();
    testRoundTrip();
    testBadInput();
    testWidgetOffDropsRefs();
    testCompact();
    if (failures == 0) std::printf("core-layout-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
