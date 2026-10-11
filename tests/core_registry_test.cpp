// core/ registry and value store tests (CTest runs them; no SteamVR, no platform code).
#include "metric_registry.h"
#include "metric_store.h"

#include <cmath>
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

bool hasProblem(const std::vector<std::string>& problems, const std::string& needle) {
    for (const std::string& p : problems) {
        if (p.find(needle) != std::string::npos) return true;
    }
    return false;
}

void testCompiledRegistry() {
    const std::vector<std::string> problems = validateRegistry(registry());
    for (const std::string& p : problems) std::printf("registry problem: %s\n", p.c_str());
    CHECK(problems.empty());
}

void testBadTable() {
    using W = WidgetType;
    using S = SourceId;
    using T = WireType;
    const std::vector<MetricDef> bad = {
        {1, "a.ok", "", W::Cpu, S::CpuLoad, 500, T::F32},
        {1, "a.dup_id", "", W::Cpu, S::CpuLoad, 500, T::F32},       // duplicate id
        {2, "a.no_owner", "", W::None, S::CpuLoad, 500, T::F32},     // no owner
        {3, "a.shared", "", W::Cpu, S::CpuLoad, 500, T::F32},
        {4, "a.shared", "", W::Power, S::CpuLoad, 500, T::F32},      // same name, two owners
        {5, "a.no_source", "", W::Cpu, S::None, 500, T::F32},        // no source
        {6, "a.no_interval", "", W::Cpu, S::CpuLoad, 0, T::F32},     // zero interval
    };
    const std::vector<std::string> problems = validateRegistry(bad);
    CHECK(hasProblem(problems, "used more than once"));
    CHECK(hasProblem(problems, "no owner"));
    CHECK(hasProblem(problems, "two owners"));
    CHECK(hasProblem(problems, "no source"));
    CHECK(hasProblem(problems, "no read interval"));
}

void testFindMetric() {
    const MetricDef* cpu = findMetric(metric::kCpuUsagePct);
    CHECK(cpu != nullptr);
    CHECK(cpu != nullptr && cpu->id == metric::kCpuUsagePct);
    CHECK(findMetric(65535) == nullptr);
}

void testStore() {
    MetricStore store;
    const MetricValue* v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::NotSampledYet);
    CHECK(store.get(65535) == nullptr);

    CHECK(store.setF32(metric::kCpuUsagePct, 42.5, 1.0));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && v->ok && v->reason == Reason::None && v->f32 == 42.5f);

    // NaN -> N/A with the given reason
    CHECK(store.setF32(metric::kCpuUsagePct, std::nan(""), 2.0, Reason::SensorMissing));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && !v->ok && v->reason == Reason::SensorMissing);

    // wrong-type setters fail and leave the value alone
    CHECK(store.setF32(metric::kCpuUsagePct, 10.0, 3.0));
    CHECK(!store.setI32(metric::kCpuUsagePct, 7, 4.0));
    CHECK(!store.setEnum(metric::kCpuUsagePct, 1, 4.0));
    CHECK(!store.setText(metric::kCpuUsagePct, "x", 4.0));
    v = store.get(metric::kCpuUsagePct);
    CHECK(v != nullptr && v->ok && v->f32 == 10.0f && v->sampledAt == 3.0);
    CHECK(!store.setF32(65535, 1.0, 1.0));

    // setText cuts to kShortStringMax: needs a ShortString metric, so use a small table
    const std::vector<MetricDef> defs = {
        {1, "t.text", "", WidgetType::General, SourceId::Ram, 500, WireType::ShortString},
    };
    MetricStore textStore(defs);
    const std::string longText(kShortStringMax + 20, 'x');
    CHECK(textStore.setText(1, longText, 1.0));
    const MetricValue* t = textStore.get(1);
    CHECK(t != nullptr && t->ok && t->text.size() == kShortStringMax);
}

}  // namespace

int main() {
    testCompiledRegistry();
    testBadTable();
    testFindMetric();
    testStore();
    if (failures) std::printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
