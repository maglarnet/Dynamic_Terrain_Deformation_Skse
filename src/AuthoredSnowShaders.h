// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <format>
#include <string>

#include "Clipmap.h"
#include "MeshSnowSurface.h"
#include "Settings.h"

namespace AuthoredSnowShaders {
    inline std::string Source() {
        const bool coarse = Clipmap::LevelCount() > 1;
        std::string out = std::format(R"(
cbuffer ClipmapWindow : register(b{0}) {{ float4 Window; float4 Raise; float4 Window1; }};
Texture2D<float> ClipmapHeight : register(t0);
Texture2D<float> ClipmapHeight1 : register(t{1});
Texture2D<float> ClipmapSurface : register(t{2});
Texture2D<float> ClipmapSurface1 : register(t{3});
SamplerState ClipmapSampler : register(s0);
Texture2D<uint> FieldActivity : register(t{4});
Texture2D<uint> FieldActivity1 : register(t{5});
static const float kFieldTexels = {6:.1f}f;
static const float kFieldWorld0 = {7:.4f}f;
static const float kFieldWorld1 = {8:.4f}f;
static const float kActivityCell0 = {9:.4f}f;
static const float kActivityCell1 = {10:.4f}f;
static const int kActivityMask = {11};
static const float kSurfaceBand = {12:.4f}f;
static const bool kCoarseLevel = {13};
static const float kMeshBoundDepth = {14:.4f}f;
static const float kMeshLevelScale = {15:.1f}f;
)",
            Clipmap::kParamsSlot, Clipmap::kLevel1Slot, Clipmap::kSurfaceSlot, Clipmap::kSurfaceSlot + 1,
            Clipmap::kActivitySlot, Clipmap::kActivity1Slot, float(Clipmap::kTexels), Clipmap::WorldSizeFor(0),
            Clipmap::WorldSizeFor(1), Clipmap::CellSizeFor(0) * float(Clipmap::kActivityRatio),
            Clipmap::CellSizeFor(1) * float(Clipmap::kActivityRatio), Clipmap::kActivityTexels - 1,
            Settings::meshSnowHeightBand, coarse ? "true" : "false",
            std::max(Settings::tessellationBoundDepth, 1e-4f), Clipmap::kLevelScale);
        out += MeshSnowSurface::hlsl;
        out += R"(
float InWindow(float2 xy, float4 window) {
 const float2 from = abs(xy - window.xy);
 return 1.0f - saturate((max(from.x, from.y) - window.z) / max(window.w - window.z, 1e-3f));
}
float Displacement(float3 relativePosition) {
 const float3 p = relativePosition + CameraPosAdjust.xyz;
 const float inWindow = InWindow(p.xy, Window);
 const float inWindow1 = kCoarseLevel ? InWindow(p.xy, Window1) : 0.0f;
 float fine = 0.0f, coarse = 0.0f;
 [branch] if (inWindow > 0.0f) {
  fine = GatedField(ClipmapHeight, ClipmapSurface, ClipmapSampler, p.xy / kFieldWorld0, kFieldTexels, p.z, kSurfaceBand, false);
 }
 [branch] if (kCoarseLevel && inWindow < 1.0f && inWindow1 > 0.0f) {
  coarse = GatedField(ClipmapHeight1, ClipmapSurface1, ClipmapSampler, p.xy / kFieldWorld1, kFieldTexels, p.z, kSurfaceBand, false);
 }
 return kCoarseLevel ? lerp(coarse, fine, inWindow) * inWindow1 : fine * inWindow;
}
float SnowRaise(float3 p) { return 0; }
float SurfaceOffset(float3 p) { return Displacement(p); }
float TileActivity(Texture2D<uint> map, float cell, float2 lo, float2 hi) {
 const int2 a = int2(floor(lo / cell)), b = int2(floor(hi / cell));
 if (any(b - a > 7)) return 1.0f;
 float activity = 0;
 [loop] for (int y = a.y; y <= b.y; ++y) {
  [loop] for (int x = a.x; x <= b.x; ++x) activity = max(activity, asfloat(map.Load(int3(int2(x, y) & kActivityMask, 0))));
 }
 return activity;
}
float MeshActivity(float2 lo, float2 hi) {
 const float2 mid = (lo + hi) * 0.5f;
 const float2 from0 = abs(mid - Window.xy);
 if (max(from0.x, from0.y) < Window.w) return TileActivity(FieldActivity, kActivityCell0, lo, hi);
 const float2 from1 = abs(mid - Window1.xy);
 if (kCoarseLevel && max(from1.x, from1.y) < Window1.w) return TileActivity(FieldActivity1, kActivityCell1, lo, hi);
 return 0;
}
float MeshDetail(float len, float2 lo, float2 hi) {
 const float2 mid = (lo + hi) * 0.5f;
 const float away = 1.0f - InWindow(mid, Window);
 const float within = kCoarseLevel ? InWindow(mid, Window1) : 1.0f - away;
 const float spacing = kTargetSpacing * lerp(1.0f, kMeshLevelScale, away);
 const float active = saturate(MeshActivity(lo, hi) / kMeshBoundDepth) * within;
 return lerp(1.0f, clamp(len / spacing, 1.0f, kMaxTess), active);
}
float MeshEdge(float3 a, float3 b) {
 const float2 aw = a.xy + CameraPosAdjust.xy, bw = b.xy + CameraPosAdjust.xy;
 return MeshDetail(length(b - a), min(aw, bw), max(aw, bw));
}
)";
        return out;
    }
}
