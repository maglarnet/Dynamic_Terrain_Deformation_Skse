// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <vector>
#include "CollisionTraversal.h"
#include "CollisionRadius.h"
#include "ActorCollisionCachePolicy.h"
#include "GatherSnapshotPolicy.h"

namespace {
struct Collision { unsigned id{}; };
template<class T> struct Pointer { T* value{}; T* get() const { return value; } };
struct Object {
	Object* parent{};
    Pointer<Collision> collisionObject;
    std::vector<Pointer<Object>> children;
    __declspec(noinline) Object* AsNode() { return children.empty() ? nullptr : this; }
    auto& GetChildren() { return children; }
};
__declspec(noinline) bool OldVisit(Object* object, std::function<bool(Collision*)> visitor) {
    if (!object) { return false; }
    if (auto* collision = object->collisionObject.get(); collision && visitor(collision)) { return true; }
    if (auto* node = object->AsNode()) {
        for (auto& child : node->GetChildren()) { if (OldVisit(child.get(), visitor)) { return true; } }
    }
    return false;
}
struct Tree {
    std::vector<Object> nodes;
    std::vector<Collision> collisions;
    explicit Tree(unsigned count) : nodes(count), collisions(count) {
        for (unsigned i = 0; i < count; ++i) {
            collisions[i].id = i;
            if (i % 5 == 0) { nodes[i].collisionObject.value = &collisions[i]; }
            for (unsigned child = i*3+1; child < i*3+4 && child < count; ++child) { nodes[i].children.push_back({&nodes[child]}); }
			if (i > 0) { nodes[i].parent = &nodes[(i - 1) / 3]; }
            if (i % 11 == 0) { nodes[i].children.push_back({nullptr}); }
        }
    }
};
void Require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
void ValidateWalk() {
    for (unsigned size : {1u,17u,128u,512u}) {
        Tree tree(size);
        for (unsigned limit = 0; limit <= size/5+2; ++limit) {
            std::vector<unsigned> old, current;
            auto a = [&](Collision* c) { old.push_back(c->id); return old.size() >= limit; };
            auto b = [&](Collision* c) { current.push_back(c->id); return current.size() >= limit; };
            Require(OldVisit(&tree.nodes[0],std::cref(a)) == CollisionTraversal::Visit<Collision>(&tree.nodes[0],b), "Stop propagation differs");
            Require(old == current, "Collision order or early-stop prefix differs");
        }
    }
    struct Visitor { Visitor() = default; Visitor(const Visitor&) = delete; bool operator()(Collision*) { return false; } } visitor;
    Require(!CollisionTraversal::Visit<Collision>(static_cast<Object*>(nullptr),visitor), "Null roots must continue");
    Tree tree(17); CollisionTraversal::Visit<Collision>(&tree.nodes[0],visitor);
}
float ReferenceRadius(CollisionRadius::Kind kind, float x, float y, float z) {
    if (kind == CollisionRadius::Kind::Sphere) { return x; }
    if (kind == CollisionRadius::Kind::Box) { return std::sqrt(x*x+y*y+z*z); }
    if (kind == CollisionRadius::Kind::Cylinder) { const float r=std::max(x,y); return std::sqrt(r*r+z*z); }
    return std::max({x,y,z});
}
void ValidateRadius() {
    std::mt19937 rng(192402);
    for (unsigned i=0; i<10000; ++i) {
        const float x=(rng()%10000)*.01f, y=(rng()%10000)*.01f, z=(rng()%10000)*.01f;
        for (auto kind : {CollisionRadius::Kind::Sphere,CollisionRadius::Kind::Capsule,CollisionRadius::Kind::Box,CollisionRadius::Kind::Cylinder,CollisionRadius::Kind::Other}) {
            unsigned calls=0;
            const auto project=[&](float dx,float dy,float dz) { ++calls; return dx*x+dy*y+dz*z; };
            const float result=CollisionRadius::Evaluate(kind,project);
            Require(result==ReferenceRadius(kind,x,y,z),"Radius differs from old formula");
            Require(calls==(kind==CollisionRadius::Kind::Sphere ? 2u : 6u),"Unexpected projection count");
        }
    }
}
void ValidateCacheAndHandoff() {
    using namespace ActorCollisionCachePolicy;
    Tree tree(512);
    std::vector<std::pair<Collision*, Object*>> cached;
    const auto collect = [&](Collision* c, Object* owner) { cached.emplace_back(c,owner); return false; };
    CollisionTraversal::VisitOwned<Collision>(&tree.nodes[0], collect);
    std::vector<unsigned> direct;
    const auto visit = [&](Collision* c) { direct.push_back(c->id); return false; };
    CollisionTraversal::Visit<Collision>(&tree.nodes[0], visit);
    Require(cached.size() == direct.size(), "Owned discovery differs");
    for (size_t i=0; i<cached.size(); ++i) {
        Require(cached[i].first->id == direct[i], "Cached collision order differs");
        Require(Attached(cached[i].second,&tree.nodes[0]), "Cached object not attached");
    }
    for (unsigned flags=0; flags<32; ++flags) {
        Require(Rebuild(flags&1,flags&2,flags&4,flags&8,flags&16,0,1) == (flags!=31),
            "Cache must rebuild on missing entry/handle/root/ragdoll/attachment changes");
    }
    for (unsigned fps : {30u,60u,144u,300u}) {
        double clock=0, next=AuditSeconds; unsigned audits=0;
        for (unsigned frame=0; frame<fps*10; ++frame) {
            clock=Advance(clock,1.0f/fps);
            if (Rebuild(true,true,true,true,true,clock,next)) { ++audits; next=clock+AuditSeconds; }
        }
        Require(audits>=9 && audits<=10,"Audit cadence depends on frame rate");
        Require(Advance(clock,0)==clock && Advance(clock,-1)==clock,"Pause/reverse time ages cache");
        Require(Advance(clock,100)-clock<0.101,"Stall catches up audits");
    }
    auto* removed=&tree.nodes[5]; auto* parent=removed->parent;
    removed->parent=nullptr;
    Require(!Attached(removed,&tree.nodes[0]),"Detached object still accepted");
    removed->parent=removed;
    Require(!Attached(removed,&tree.nodes[0]),"Parent cycle never terminates");
    removed->parent=parent;
    Require(Attached(removed,&tree.nodes[0]),"Reattached object rejected");
    auto* ancestor=&tree.nodes[1]; parent=ancestor->parent; ancestor->parent=nullptr;
    Require(!Attached(&tree.nodes[5],&tree.nodes[0]),"Detached ancestor missed");
    ancestor->parent=parent;
    const unsigned previous=cached[0].first->id;
    tree.collisions[0].id=9999;
    Require(cached[0].first->id==9999,"Cache freezes live collision data");
    tree.collisions[0].id=previous;
    using namespace GatherSnapshotPolicy;
    const Identity original{1,2,3,4};
    State state;
    Require(!state.Consume(original),"Empty snapshot accepted");
    state.Publish(original);
    Require(state.Consume(original) && !state.Consume(original),"Snapshot replayed");
    for (Identity changed : {Identity{2,2,3,4},Identity{1,5,3,4},Identity{1,2,5,4},Identity{1,2,3,5}}) {
        state.Publish(original);
        Require(!state.Consume(changed) && !state.Consume(original),"Stale identity accepted");
    }
    state.Publish(original); state.Reset();
    Require(!state.Consume(original),"Reset snapshot accepted");
    std::puts("PASS owned collision discovery/order, cache invalidation, detach/reattach/cycles, live data, 30/60/144/300 FPS audits, pause/stalls, one-use snapshot identities.");
}
int64_t Tick() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
volatile uint64_t checksumSink{};
template<bool New>
double WalkTime(Tree& tree, bool stop, int64_t frequency) {
    uint64_t sum=0; unsigned visited=0;
    const auto visitor=[&](Collision* c) { sum += c->id; return stop && ++visited>=16; };
    const auto begin=Tick();
    for (unsigned loop=0; loop<8192; ++loop) {
        visited=0;
        if constexpr(New) { CollisionTraversal::Visit<Collision>(&tree.nodes[0],visitor); }
        else { OldVisit(&tree.nodes[0],std::cref(visitor)); }
    }
    const auto elapsed=Tick()-begin;
    checksumSink=sum;
    return static_cast<double>(elapsed)*1e9/frequency/8192;
}
template<class Old,class New>
void Paired(const char* name,Old old,New current) {
    old();current(); std::vector<double>a,b;
    for(unsigned i=0;i<9;++i) {
        if(i%2) { b.push_back(current()); a.push_back(old()); }
        else { a.push_back(old()); b.push_back(current()); }
    }
    std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
    std::printf("%s: old %.2f, new %.2f ns/operation (%+.1f%%); ranges %.2f..%.2f / %.2f..%.2f\n",name,a[4],b[4],(b[4]/a[4]-1)*100,a.front(),a.back(),b.front(),b.back());
}
}
int main() {
    try {
        ValidateWalk();ValidateRadius();ValidateCacheAndHandoff();
        std::puts("PASS preorder, null children/root, all stop prefixes, noncopyable callback; 50,000 exact radius checks.");
        std::puts("Synthetic CPU fixtures. Baseline uses std::cref, not a heap-copied large lambda. No gameplay speedup claimed.");
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);Tree tree(512);
        Paired("512-node full walk",[&]{return WalkTime<false>(tree,false,frequency.QuadPart);},[&]{return WalkTime<true>(tree,false,frequency.QuadPart);});
        Paired("stop after 16 collisions",[&]{return WalkTime<false>(tree,true,frequency.QuadPart);},[&]{return WalkTime<true>(tree,true,frequency.QuadPart);});
        std::puts("Sphere projections: 6 -> 2; other shapes: 6 -> 6. Havok projection timing is not modeled.");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
