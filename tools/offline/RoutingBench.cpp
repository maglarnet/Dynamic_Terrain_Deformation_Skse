// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>
#include "BloodRoutingGate.h"
#include "BloodDecalFilter.h"
#include "UtilityRouting.h"

namespace {
struct Geometry { bool skinned{}, flags{}, isNear{}, lod{}, actor{}, registered{}, discovered{}; uint64_t descriptor{}; };
struct Draw { size_t geometry{}; uint32_t technique{}; bool utility{}, perspective{}; };
struct Fixture {
    std::vector<Geometry> geometry;
    std::vector<Draw> draws;
    std::unordered_set<const Geometry*> initial;
    bool probe{};
};
struct State {
    std::unordered_set<const Geometry*> targets;
    std::atomic<int64_t> timedTicks{};
    uint64_t lookups{}, candidates{}, descriptions{}, cameras{};
};
struct Info { bool isNear, lod; uint32_t technique; uint64_t descriptor; };
int64_t Tick() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
__declspec(noinline) Info Describe(const Geometry& g, const Draw& d) { return {g.isNear, g.lod, d.technique, g.descriptor}; }
__declspec(noinline) bool Camera(const Draw& d) {
    const std::array<float, 16> matrix{0,0,0,0, 0,0,0,0, 0,0,0,d.perspective ? 1.f : 0.f, 0,0,0,1};
    return UtilityRouting::IsPerspectiveProjection(matrix.data());
}
__declspec(noinline) bool Contains(State& state, const Geometry& g) {
    if (state.targets.contains(&g)) { return true; }
    if (g.skinned || !g.flags) { return false; }
    if (g.discovered && BloodDecalFilter::Matches("textures/decals/blood01.dds", "blood,decalsblood,bigspatter")) {
        state.targets.insert(&g);
    }
    return state.targets.contains(&g);
}
template<bool Profile, bool Count>
bool Lookup(State& state, const Geometry& g) {
    if constexpr (Count) { ++state.lookups; }
    int64_t begin{};
    if constexpr (Profile) { begin = Tick(); }
    const bool found = Contains(state, g);
    if constexpr (Profile) { state.timedTicks.fetch_add(Tick() - begin, std::memory_order_relaxed); }
    return found;
}
template<bool New, bool Profile, bool Count = false>
uint64_t Route(State& state, const Fixture& fixture, const Draw& draw) {
    const auto& g = fixture.geometry[draw.geometry];
    bool candidate = true;
    if constexpr (New) {
        candidate = BloodRoutingGate::Candidate(true, true, g.skinned, g.flags,
            [&] { return !state.targets.empty() && state.targets.contains(&g); });
        if constexpr (Count) { state.candidates += candidate; }
        if (!BloodRoutingGate::Relevant(fixture.probe, g.isNear, candidate)) { return 0; }
    }
    if (!draw.utility) {
        if constexpr (Count) { ++state.cameras; }
        if (!Camera(draw)) { return 0; }
    }
    if constexpr (Count) { ++state.descriptions; }
    const auto info = Describe(g, draw);
    if (draw.utility) {
        if (!UtilityRouting::IsCameraDepth(info.technique)) { return 0; }
        if constexpr (Count) { ++state.cameras; }
        if (!Camera(draw)) { return 0; }
    }
    if (candidate && Lookup<Profile, Count>(state, g)) { return 1; }
    if (fixture.probe && !info.isNear && !info.lod && !g.actor) { return 2; }
    return info.isNear ? 3 + (info.descriptor & 7) : 0;
}
Fixture Make(unsigned seed, unsigned bloodPercent, unsigned shadowPercent, bool probe) {
    Fixture f; f.probe = probe;
    std::mt19937 random(seed);
    f.geometry.resize(1024);
    for (size_t i = 0; i < f.geometry.size(); ++i) {
        auto& g = f.geometry[i];
        const auto kind = random() % 100;
        const bool blood = kind < bloodPercent;
        g.isNear = !blood && kind < bloodPercent + 8;
        g.lod = !blood && !g.isNear && kind < bloodPercent + 10;
        g.actor = !blood && !g.isNear && !g.lod && (random() % 4 == 0);
        g.skinned = g.actor;
        g.registered = blood && (i % 3 != 0);
        g.discovered = blood && !g.registered;
        g.flags = g.discovered || (g.registered && i % 5 != 0) || (!blood && !g.isNear && !g.skinned && i % 29 == 0);
        g.descriptor = random();
    }
    for (const auto& g : f.geometry) { if (g.registered) { f.initial.insert(&g); } }
    for (size_t i = 0; i < 4096; ++i) {
        Draw d; d.geometry = random() % f.geometry.size(); d.utility = i % 2 != 0;
        d.technique = random() % 100 < shadowPercent ? 1u << 9 : 1u << 13;
        d.perspective = random() % 100 >= 5;
        f.draws.push_back(d);
    }
    return f;
}
void Validate(const Fixture& f) {
    State old, current; old.targets = current.targets = f.initial;
    for (const auto& d : f.draws) {
        if (Route<false, false>(old, f, d) != Route<true, false>(current, f, d)) {
            throw std::runtime_error("Old/new routing decisions differ");
        }
    }
    if (old.targets != current.targets) { throw std::runtime_error("Attached-node discovery differs"); }
}
volatile uint64_t sink{};
template<bool New, bool Profile>
double Measure(const Fixture& f, unsigned frames, int64_t frequency) {
    State state; state.targets.reserve(f.geometry.size());
    uint64_t checksum = 0;
    const auto begin = Tick();
    for (unsigned frame = 0; frame < frames; ++frame) {
        state.targets = f.initial;
        for (const auto& d : f.draws) { checksum += Route<New, Profile>(state, f, d); }
    }
    const auto elapsed = Tick() - begin;
    sink = checksum;
    return static_cast<double>(elapsed) * 1e9 / frequency / (frames * f.draws.size());
}
double Median(std::vector<double> values) { std::sort(values.begin(), values.end()); return values[values.size()/2]; }
template<bool Profile>
void Benchmark(const char* name, const Fixture& f, unsigned samples, int64_t frequency, std::ofstream& csv) {
    Validate(f);
    const auto warmOld = Measure<false, Profile>(f, 128, frequency);
    const auto warmNew = Measure<true, Profile>(f, 128, frequency);
    const auto frames = static_cast<unsigned>(std::clamp(25e6 / (std::min(warmOld, warmNew) * f.draws.size()), 128.0, 16384.0));
    std::vector<double> old, current;
    for (unsigned i = 0; i < samples; ++i) {
        double a, b;
        if (i % 2) { b = Measure<true, Profile>(f, frames, frequency); a = Measure<false, Profile>(f, frames, frequency); }
        else { a = Measure<false, Profile>(f, frames, frequency); b = Measure<true, Profile>(f, frames, frequency); }
        old.push_back(a); current.push_back(b);
        if (csv) { csv << name << ',' << Profile << ',' << i << ',' << frames << ',' << a << ',' << b << '\n'; }
    }
    State a, b; a.targets = b.targets = f.initial;
    for (const auto& d : f.draws) { Route<false, false, true>(a, f, d); Route<true, false, true>(b, f, d); }
    std::printf("%-18s profiler=%s old %8.2f new %8.2f ns/draw | change %+6.1f%% | lookups %llu -> %llu, descriptions %llu -> %llu\n",
        name, Profile ? "on " : "off", Median(old), Median(current), (Median(current)/Median(old)-1)*100,
        static_cast<unsigned long long>(a.lookups), static_cast<unsigned long long>(b.lookups),
        static_cast<unsigned long long>(a.descriptions), static_cast<unsigned long long>(b.descriptions));
    std::printf("  %u paired samples, %u frames/sample; old range %.2f..%.2f, new range %.2f..%.2f ns/draw\n",
        samples,frames,*std::min_element(old.begin(),old.end()),*std::max_element(old.begin(),old.end()),
        *std::min_element(current.begin(),current.end()),*std::max_element(current.begin(),current.end()));
}
}
int main(int argc, char** argv) {
    try {
        unsigned samples = 9; std::ofstream csv;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--csv" && i+1 < argc) { csv.open(argv[++i]); if (!csv) { throw std::runtime_error("Cannot open CSV"); } }
            else if (arg == "--samples" && i+1 < argc) { samples = std::stoul(argv[++i]); if (samples < 3 || samples > 101 || samples%2 == 0) { throw std::runtime_error("Samples must be odd, 3..101"); } }
            else { throw std::runtime_error("Usage: RoutingBench [--samples 9] [--csv output.csv]"); }
        }
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        for (unsigned seed = 0; seed < 128; ++seed) { auto f = Make(seed, seed%30, seed%80, seed%2); Validate(f); }
        unsigned consulted = 0;
        if (!BloodRoutingGate::Candidate(true,true,false,false,[&]{++consulted;return true;}) || consulted != 1 ||
            !BloodRoutingGate::Candidate(true,true,false,true,[&]{++consulted;return false;}) || consulted != 1 ||
            BloodRoutingGate::Candidate(false,true,false,true,[]{return true;}) ||
            BloodRoutingGate::Candidate(true,false,false,true,[]{return true;}) ||
            BloodRoutingGate::Candidate(true,true,true,true,[]{return true;})) { throw std::runtime_error("Candidate gate regression"); }
        std::puts("PASS: 128 deterministic fixture decision/discovery comparisons and candidate edge cases.");
        std::puts("SYNTHETIC CPU MODEL: shared candidate/camera/technique predicates; adapted engine data. Not an FPS estimate.");
        std::puts("Entire frame replay timed, including candidate checks and snapshot copies; profiler on/off shown separately.");
        if (csv) { csv << "scenario,profiler,sample,frames,old_ns_per_draw,new_ns_per_draw\n"; }
        for (const auto& [name,blood,shadow,probe] : std::array<std::tuple<const char*,unsigned,unsigned,bool>,4>{
            std::tuple{"ordinary-heavy",1u,20u,false}, {"blood-heavy",25u,20u,false},
            {"shadow-heavy",1u,80u,false}, {"static-probe",1u,20u,true}}) {
            auto fixture = Make(192402,blood,shadow,probe);
            Benchmark<false>(name,fixture,samples,frequency.QuadPart,csv);
            Benchmark<true>(name,fixture,samples,frequency.QuadPart,csv);
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
