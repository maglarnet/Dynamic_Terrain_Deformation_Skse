// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
#include "CoverageWorkQueue.h"

namespace {
void Require(bool condition,const char* message) { if(!condition) {throw std::runtime_error(message);} }
template<uint32_t N>
std::pair<int32_t,int32_t> Coordinate(uint32_t index,int32_t centreX,int32_t centreY) {
    const int32_t bx=centreX-static_cast<int32_t>(N/2),by=centreY-static_cast<int32_t>(N/2);
    return {bx+static_cast<int32_t>(((index&(N-1))-static_cast<uint32_t>(bx))&(N-1)),
        by+static_cast<int32_t>(((index/N)-static_cast<uint32_t>(by))&(N-1))};
}
bool Inside(int32_t x,int32_t y,int32_t cx,int32_t cy,int32_t size) {
    return x>=cx-size/2 && x<cx+size/2 && y>=cy-size/2 && y<cy+size/2;
}
void TestFrontier() {
    constexpr uint32_t N=16;
    CoverageWorkQueue::Grid<N> queue;
    int32_t x=-70,y=19;
    queue.Move(x,y,[](uint32_t){});
    Require(queue.Count()==N*N,"Initial fill must schedule every slot");
    std::mt19937 random(192402);
    for(unsigned step=0;step<3000;++step) {
        while(queue.Count()) {queue.Pop();}
        const int32_t nx=x+static_cast<int32_t>(random()%49)-24,ny=y+static_cast<int32_t>(random()%49)-24;
        std::array<bool,N*N> entering{},actual{};
        queue.Move(nx,ny,[&](uint32_t i){entering[i]=true;});
        while(queue.Count()) {const auto i=queue.Pop();Require(!actual[i],"Duplicate queue index");actual[i]=true;}
        for(uint32_t i=0;i<N*N;++i) {
            const auto [wx,wy]=Coordinate<N>(i,nx,ny);
            const bool expected=!Inside(wx,wy,x,y,N);
            Require(actual[i]==expected && entering[i]==expected,"Frontier differs from full-grid coordinate oracle");
        }
        x=nx;y=ny;
        queue.Move(x,y,[](uint32_t){throw std::runtime_error("Stationary window requeued");});
        Require(queue.Count()==0,"Stationary work must be empty");
    }
    queue.Reset();queue.Move(x,y,[](uint32_t){});Require(queue.Count()==N*N,"Reset must refill same location");
}
void TestStreamingAndBacklog() {
    constexpr uint32_t N=16,total=N*N,budget=7;
    CoverageWorkQueue::Grid<N> queue;
    std::array<int32_t,total> filledX,filledY;filledX.fill(-999999);filledY.fill(-999999);
    std::array<double,total> retry{};
    std::array<uint8_t,total> coverage{};
    int32_t cx=-21,cy=-50;double time=0;
    const auto ground=[](int32_t x,int32_t y){return static_cast<uint8_t>((static_cast<uint32_t>(x*31+y)&1)?255:0);};
    unsigned retries=0,misses=0;
    for(unsigned frame=0;frame<450;++frame) {
        if(frame<100 && frame%3==0) {++cx;--cy;}
        queue.Move(cx,cy,[&](uint32_t i){retry[i]=0;});
        const auto limit=std::min(queue.Count(),budget);unsigned queries=0;
        for(unsigned scan=0;scan<limit;++scan) {
            const auto index=queue.Pop();const auto [x,y]=Coordinate<N>(index,cx,cy);
            if(filledX[index]==x && filledY[index]==y) {continue;}
            if(time<retry[index]) {queue.Push(index);++retries;continue;}
            ++queries;
            if(frame<180 && index%5==0) {retry[index]=time+.5;queue.Push(index);++misses;continue;}
            coverage[index]=ground(x,y);filledX[index]=x;filledY[index]=y;
        }
        Require(queries<=budget && queue.Count()<=total,"Budget/capacity exceeded");time+=1.0/60;
    }
    Require(misses>0 && retries>0,"Streaming/cooldown cases were not exercised");
    Require(queue.Count()==0,"Late-loaded land never caught up");
    for(uint32_t i=0;i<total;++i) {
        const auto [x,y]=Coordinate<N>(i,cx,cy);
        Require(filledX[i]==x && filledY[i]==y && coverage[i]==ground(x,y),"Moving backlog left stale or missing coverage");
    }
    retry.fill(999);queue.Move(cx+N,cy,[&](uint32_t i){retry[i]=0;});
    for(auto due:retry) {Require(due==0,"New coordinates retained a previous miss cooldown");}
}
int64_t Tick(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
volatile uint64_t resultSink{};
template<bool New>
double Measure(int64_t frequency) {
    constexpr uint32_t N=256,total=N*N;
    CoverageWorkQueue::Grid<N> queue;
    std::array<int32_t,total> filledX,filledY;
    for(uint32_t i=0;i<total;++i) {auto [x,y]=Coordinate<N>(i,0,0);filledX[i]=x;filledY[i]=y;}
    if constexpr(New) {queue.Move(0,0,[](uint32_t){});while(queue.Count()){queue.Pop();}}
    uint64_t queries=0;uint32_t cursor=0;
    const auto begin=Tick();
    for(int32_t step=1;step<=256;++step) {
        if constexpr(New) {
            queue.Move(step,0,[](uint32_t){});
            const auto work=queue.Count();
            for(uint32_t n=0;n<work;++n) {
                const auto i=queue.Pop();auto [x,y]=Coordinate<N>(i,step,0);
                if(filledX[i]!=x || filledY[i]!=y){filledX[i]=x;filledY[i]=y;++queries;}
            }
        } else {
            for(uint32_t scan=0;scan<total;++scan) {
                const auto i=cursor;cursor=(cursor+1)%total;auto [x,y]=Coordinate<N>(i,step,0);
                if(filledX[i]!=x || filledY[i]!=y){filledX[i]=x;filledY[i]=y;++queries;}
            }
        }
    }
    const auto elapsed=Tick()-begin;Require(queries==total,"Old/new walk workloads differ");resultSink=queries;
    return static_cast<double>(elapsed)*1e6/frequency/256;
}
}
int main(){
    try {
        TestFrontier();TestStreamingAndBacklog();
        std::puts("PASS: 3,000 signed/moving/diagonal/jumping frontier comparisons; duplicates, reset, budget, streaming retries and moving backlog.");
        LARGE_INTEGER f;QueryPerformanceFrequency(&f);std::vector<double>a,b;
        Measure<false>(f.QuadPart);Measure<true>(f.QuadPart);
        for(unsigned i=0;i<9;++i){if(i%2){b.push_back(Measure<true>(f.QuadPart));a.push_back(Measure<false>(f.QuadPart));}else{a.push_back(Measure<false>(f.QuadPart));b.push_back(Measure<true>(f.QuadPart));}}
        std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
        std::printf("Synthetic one-column movement scheduling: old %.2f, new %.2f us/step (%+.1f%%), same 256 queries/step.\n",a[4],b[4],(b[4]/a[4]-1)*100);
        std::puts("Excludes engine queries, smoothing and GPU uploads; not total Coverage CPU or FPS.");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
