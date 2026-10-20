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

// R2: compact / hide_system keys
void testCompactAndHideSystem() {
    // round trip, keys only emitted when on
    Layout l;
    l.columns.resize(1);
    WidgetInstance frame = inst(W::Frame);
    frame.compact = true;
    WidgetInstance cpuItems = inst(W::CpuItems);
    cpuItems.hideSystem = true;
    WidgetInstance ramItems = inst(W::RamItems);  // off: no key
    l.columns[0].widgets = {frame, cpuItems, ramItems, inst(W::Power)};
    const std::string text = layoutToJson(l, 0);
    CHECK(text.find("\"compact\":true") != std::string::npos);
    CHECK(text.find("\"hide_system\":true") != std::string::npos);
    size_t count = 0;
    for (size_t at = text.find("hide_system"); at != std::string::npos; at = text.find("hide_system", at + 1)) ++count;
    CHECK(count == 1);
    count = 0;
    for (size_t at = text.find("compact"); at != std::string::npos; at = text.find("compact", at + 1)) ++count;
    CHECK(count == 1);

    Layout back;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(parseLayoutText(text, back, warnings, error));
    CHECK(warnings.empty());
    CHECK(back.sameWidgets(l));
    CHECK(back.columns[0].widgets[0].compact && !back.columns[0].widgets[0].hideSystem);
    CHECK(back.columns[0].widgets[1].hideSystem && !back.columns[0].widgets[1].compact);
    CHECK(!back.columns[0].widgets[2].hideSystem && !back.columns[0].widgets[3].compact);

    // old layouts: false, and re-serialised without the new keys
    const Layout def = defaultLayout();
    const std::string defText = layoutToJson(def, 2);
    CHECK(defText.find("compact") == std::string::npos && defText.find("hide_system") == std::string::npos);
    Layout old;
    warnings.clear();
    CHECK(parseLayoutText(R"({"version":1,"columns":[[{"type":"frame"},{"type":"cpu_items","max_items":5}]]})", old,
                          warnings, error));
    CHECK(warnings.empty());
    CHECK(!old.columns[0].widgets[0].compact && !old.columns[0].widgets[1].hideSystem);
    const std::string oldText = layoutToJson(old, 0);
    CHECK(oldText.find("compact") == std::string::npos && oldText.find("hide_system") == std::string::npos);

    // wrong type: warns (unknown key) and is ignored
    Layout wrong;
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"cpu","compact":true},{"type":"general","compact":true},)"
                          R"({"type":"frame","hide_system":true}]]})",
                          wrong, warnings, error));
    CHECK(warnings.size() == 3);
    CHECK(wrong.columns[0].widgets.size() == 3);
    for (const WidgetInstance& w : wrong.columns[0].widgets) CHECK(!w.compact && !w.hideSystem);

    // Streaming has a Compact view now (R3): it round-trips
    Layout streaming;
    streaming.columns.resize(1);
    WidgetInstance st = inst(W::Streaming);
    st.compact = true;
    streaming.columns[0].widgets = {st};
    Layout streamingBack;
    warnings.clear();
    CHECK(parseLayoutText(layoutToJson(streaming, 0), streamingBack, warnings, error));
    CHECK(warnings.empty() && streamingBack.columns[0].widgets[0].compact);
    CHECK(streamingBack.sameWidgets(streaming));

    // non-bool value: warns, stays false
    Layout bad;
    warnings.clear();
    CHECK(parseLayoutText(R"({"columns":[[{"type":"temp","compact":"yes"},{"type":"ram_items","hide_system":1}]]})", bad,
                          warnings, error));
    CHECK(warnings.size() == 2);
    CHECK(!bad.columns[0].widgets[0].compact && !bad.columns[0].widgets[1].hideSystem);

    // operator== sees both fields
    WidgetInstance a = inst(W::Frame), b = inst(W::Frame);
    CHECK(a == b);
    b.compact = true;
    CHECK(a != b);
    WidgetInstance c = inst(W::CpuItems), d = inst(W::CpuItems);
    d.hideSystem = true;
    CHECK(c != d);
}

Layout parseOk(const std::string& text, std::vector<std::string>& warnings) {
    Layout out;
    std::string error;
    warnings.clear();
    CHECK(parseLayoutText(text, out, warnings, error));
    return out;
}

WidgetInstance httpCam(const char* json, std::vector<std::string>& warnings) {
    const Layout layout = parseOk(std::string(R"({"columns":[[{"type":"http_camera")") + json + "}]]}", warnings);
    if (layout.columns.size() != 1 || layout.columns[0].widgets.size() != 1) {
        CHECK(false);
        return WidgetInstance();
    }
    return layout.columns[0].widgets[0];
}

void testHttpCameraJson() {
    // defaults
    std::vector<std::string> warnings;
    WidgetInstance d = httpCam("", warnings);
    CHECK(warnings.empty());
    CHECK(d.type == W::HttpCamera && d.url == kDefaultHttpCameraUrl && d.rotation == 0 && d.cameraFps == 15 &&
          !d.cameraFullSize);
    CHECK(std::string(kDefaultHttpCameraUrl) == "http://127.0.0.1:8082/preview");
    CHECK(d.cameraSize == 100);
    CHECK(cameraFpsMax(W::HttpCamera) == 30);

    // round trip, every setting off its default; rotation is degrees in the JSON, quarter turns in memory.
    // The quotes in the url go through the writer's escaping.
    Layout layout;
    layout.columns.resize(1);
    WidgetInstance w = inst(W::HttpCamera);
    w.url = "http://192.168.1.9:8080/cam?q=\"1\"";
    w.rotation = 3;
    w.cameraFps = 30;
    w.cameraFullSize = true;
    w.cameraSize = 60;
    layout.columns[0].widgets = {w};
    const std::string text = layoutToJson(layout, 0);
    CHECK(text.find("\"rotation\":270") != std::string::npos || text.find("\"rotation\": 270") != std::string::npos);
    for (const int indent : {0, 2}) {
        const Layout back = parseOk(layoutToJson(layout, indent), warnings);
        CHECK(warnings.empty());
        CHECK(back.sameWidgets(layout));
        const WidgetInstance& b = back.columns[0].widgets[0];
        CHECK(b.url == w.url && b.rotation == 3 && b.cameraFps == 30 && b.cameraFullSize && b.cameraSize == 60);
    }
    // a default widget still round trips
    Layout plain;
    plain.columns.resize(1);
    plain.columns[0].widgets = {inst(W::HttpCamera)};
    CHECK(parseOk(layoutToJson(plain, 0), warnings).sameWidgets(plain));
    CHECK(warnings.empty());

    // bad url: warns, keeps the default
    for (const char* bad : {R"(,"url":"ftp://x/y")", R"(,"url":"http://")", R"(,"url":5)", R"(,"url":"http://a b/")"}) {
        const WidgetInstance g = httpCam(bad, warnings);
        CHECK(warnings.size() == 1);
        CHECK(g.url == kDefaultHttpCameraUrl);
    }
    // a good one with the other fields
    d = httpCam(R"(,"url":"http://cam.local/stream","rotation":90,"fps":10,"full_size":true)", warnings);
    CHECK(warnings.empty());
    CHECK(d.url == "http://cam.local/stream" && d.rotation == 1 && d.cameraFps == 10 && d.cameraFullSize);

    // rotation snapping
    struct Case {
        const char* json;
        int turns;
        bool warns;
    };
    const Case cases[] = {
        {R"(,"rotation":0)", 0, false},   {R"(,"rotation":90)", 1, false}, {R"(,"rotation":180)", 2, false},
        {R"(,"rotation":270)", 3, false}, {R"(,"rotation":100)", 1, true}, {R"(,"rotation":-90)", 3, true},
        {R"(,"rotation":450)", 1, true},  {R"(,"rotation":360)", 0, true}, {R"(,"rotation":44)", 0, true},
        {R"(,"rotation":136)", 2, true},
    };
    for (const Case& c : cases) {
        const WidgetInstance g = httpCam(c.json, warnings);
        if (g.rotation != c.turns || (warnings.size() == 1) != c.warns) std::printf("rotation case %s\n", c.json);
        CHECK(g.rotation == c.turns);
        CHECK((warnings.size() == 1) == c.warns);
    }
    d = httpCam(R"(,"rotation":"left")", warnings);
    CHECK(warnings.size() == 1 && d.rotation == 0);
    d = httpCam(R"(,"full_size":"yes")", warnings);
    CHECK(warnings.size() == 1 && !d.cameraFullSize);
    d = httpCam(R"(,"fps":7)", warnings);  // the shared fps rule: nearest step, with a warning
    CHECK(warnings.size() == 1 && d.cameraFps == 5);

    // size: percent of the card's width, snapped to 40, 50, ... 100
    for (const int step : {40, 50, 60, 70, 80, 90, 100}) {
        d = httpCam((std::string(R"(,"size":)") + std::to_string(step)).c_str(), warnings);
        CHECK(warnings.empty() && d.cameraSize == step);
    }
    struct SizeCase {
        const char* json;
        int size;
    };
    const SizeCase sizes[] = {{R"(,"size":45)", 40},  {R"(,"size":55)", 50}, {R"(,"size":101)", 100}, {R"(,"size":10)", 40},
                              {R"(,"size":200)", 100}, {R"(,"size":0)", 40},  {R"(,"size":-5)", 40},   {R"(,"size":64)", 60}};
    for (const SizeCase& c : sizes) {
        d = httpCam(c.json, warnings);
        if (d.cameraSize != c.size || warnings.size() != 1) std::printf("size case %s: got %d\n", c.json, d.cameraSize);
        CHECK(d.cameraSize == c.size && warnings.size() == 1);
    }
    d = httpCam(R"(,"size":"big")", warnings);
    CHECK(warnings.size() == 1 && d.cameraSize == 100);
    d = httpCam(R"(,"size":null)", warnings);
    CHECK(warnings.size() == 1 && d.cameraSize == 100);
    // written for http_camera only, as a percent
    Layout sized;
    sized.columns.resize(1);
    WidgetInstance sw = inst(W::HttpCamera);
    sw.cameraSize = 40;
    sized.columns[0].widgets = {sw, inst(W::EyeCamera)};
    const std::string sizedJson = layoutToJson(sized, 0);
    CHECK(sizedJson.find("\"size\":40") != std::string::npos);
    CHECK(sizedJson.find("\"size\"") == sizedJson.rfind("\"size\""));  // only once
    // the steppers
    CHECK(cameraSizeNearest(100) == 100 && cameraSizeNearest(44) == 40 && cameraSizeNearest(45) == 40 &&
          cameraSizeNearest(46) == 50 && cameraSizeNearest(-1) == 40 && cameraSizeNearest(1000) == 100);
    CHECK(cameraSizeStep(100, +1) == 100 && cameraSizeStep(100, -1) == 90 && cameraSizeStep(40, -1) == 40 &&
          cameraSizeStep(40, +1) == 50 && cameraSizeStep(70, +1) == 80 && cameraSizeStep(70, -1) == 60);
    CHECK(cameraSizeStep(65, +1) == 70 || cameraSizeStep(65, +1) == 80);  // an off-step value counts as its nearest

    // the settings belong to http_camera only
    const Layout other = parseOk(R"({"columns":[[{"type":"frame","url":"http://x/","rotation":90}]]})", warnings);
    CHECK(other.columns[0].widgets[0].rotation == 0 && other.columns[0].widgets[0].url == kDefaultHttpCameraUrl);
}

void testHttpCameraEquality() {
    WidgetInstance a = inst(W::HttpCamera), b = inst(W::HttpCamera);
    CHECK(a == b);
    b.url = "http://other:1/";
    CHECK(a != b);
    b = a;
    b.rotation = 2;
    CHECK(a != b);
    b = a;
    b.cameraFps = 30;
    CHECK(a != b);
    b = a;
    b.cameraFullSize = true;
    CHECK(a != b);
    b = a;
    b.cameraSize = 70;
    CHECK(a != b);
    // layoutGrid's marker isn't part of the identity
    b = a;
    b.httpCameraExtra = true;
    CHECK(a == b);
    Layout x, y;
    x.columns.resize(1);
    y.columns.resize(1);
    x.columns[0].widgets = {a};
    y.columns[0].widgets = {b};
    CHECK(x.sameWidgets(y));
    CHECK(layoutToJson(x, 0) == layoutToJson(y, 0));  // and it isn't written
}

void testParseHttpUrl() {
    HttpUrl u;
    std::string why;
    CHECK(parseHttpUrl("http://127.0.0.1:8082/preview", u, &why));
    CHECK(u.host == "127.0.0.1" && u.port == 8082 && u.path == "/preview");
    CHECK(parseHttpUrl("http://cam.local", u));  // default port, path "/"
    CHECK(u.host == "cam.local" && u.port == 80 && u.path == "/");
    CHECK(parseHttpUrl("HTTP://Cam.Local:81/a/b?x=1&y=2", u));  // scheme is case-insensitive, query kept
    CHECK(u.host == "Cam.Local" && u.port == 81 && u.path == "/a/b?x=1&y=2");
    CHECK(parseHttpUrl("http://h?x=1", u) && u.path == "/?x=1");
    CHECK(parseHttpUrl("http://h:65535/", u) && u.port == 65535);
    CHECK(parseHttpUrl("http://h:1/", u) && u.port == 1);
    CHECK(parseHttpUrl("http://h/p#frag", u) && u.path == "/p");

    const char* bad[] = {
        "",                 "127.0.0.1:8082/preview", "//h/",         "https://h/",    "ftp://h/",
        "http:/h/",         "http://",                "http:///path", "http://:80/",   "http://h:0/",
        "http://h:65536/",  "http://h:abc/",          "http://h:/",   "http://h:-1/",  "http://h:123456/",
        "http://user@h/",   "http://user:pw@h:80/",   "http://h /x",  "http://h/a b",  " http://h/",
        "http://h/\t",      "http://h/\n",
    };
    for (const char* s : bad) {
        std::string reason;
        if (parseHttpUrl(s, u, &reason)) std::printf("accepted %s\n", s);
        CHECK(!parseHttpUrl(s, u, &reason));
        CHECK(!reason.empty());
        CHECK(!parseHttpUrl(s, u));  // why is optional
    }
    // 255 bytes is the limit
    std::string url = "http://h/";
    url.append(kMaxHttpCameraUrl - url.size(), 'a');
    CHECK(url.size() == 255 && parseHttpUrl(url, u));
    url += 'a';
    CHECK(!parseHttpUrl(url, u, &why));

    // a failed parse leaves `out` alone
    HttpUrl keep;
    keep.host = "keep";
    CHECK(!parseHttpUrl("nope", keep) && keep.host == "keep");

    CHECK(httpUrlHostPort("http://127.0.0.1:8082/preview") == "127.0.0.1:8082");
    CHECK(httpUrlHostPort("http://cam.local/x") == "cam.local:80");
    CHECK(httpUrlHostPort("not a url") == "not a url");
}

}  // namespace

int main() {
    testDefaultLayout();
    testRoundTrip();
    testBadInput();
    testWidgetOffDropsRefs();
    testCompactAndHideSystem();
    testHttpCameraJson();
    testHttpCameraEquality();
    testParseHttpUrl();
    if (failures == 0) std::printf("core-layout-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
