// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include <cmath>

namespace MeshSnowSurface
{
	enum class Kind
	{
		kNone = 0,
		kGround = 1,
		kMesh = 2,
	};

	inline constexpr float kGroundBase = 1.0e6f;
	inline constexpr float kMeshBase = 3.0e6f;
	inline constexpr float kGroundFloor = 5.0e5f;
	inline constexpr float kMeshFloor = 2.0e6f;

	constexpr float Encode(Kind a_kind, float a_z)
	{
		return a_kind == Kind::kGround ? a_z + kGroundBase :
		       a_kind == Kind::kMesh   ? a_z + kMeshBase :
		                                 0.0f;
	}

	constexpr Kind KindOf(float a_code)
	{
		return a_code < kGroundFloor ? Kind::kNone : a_code < kMeshFloor ? Kind::kGround : Kind::kMesh;
	}

	constexpr float HeightOf(float a_code)
	{
		return KindOf(a_code) == Kind::kMesh ? a_code - kMeshBase : a_code - kGroundBase;
	}

	inline bool Differ(float a_code, float a_other, float a_band)
	{
		const auto a = KindOf(a_code);
		const auto b = KindOf(a_other);
		if (a != Kind::kMesh && b != Kind::kMesh) { return false; }
		if (a == Kind::kNone || b == Kind::kNone) { return true; }
		return std::abs(HeightOf(a_code) - HeightOf(a_other)) > a_band;
	}

	inline float Weight(float a_code, float a_z, float a_band, bool a_land)
	{
		const auto kind = KindOf(a_code);
		if (kind == Kind::kNone || (a_land && kind == Kind::kGround)) { return a_land ? 1.0f : 0.0f; }
		const float d = std::abs(HeightOf(a_code) - a_z);
		const float lo = a_band * 0.75f;
		if (d <= lo) { return 1.0f; }
		if (d >= a_band) { return 0.0f; }
		const float t = (d - lo) / (a_band - lo);
		return 1.0f - t * t * (3.0f - 2.0f * t);
	}

	inline constexpr const char* hlsl = R"(
static const float kSurfaceGroundBase = 1.0e6f;
static const float kSurfaceMeshBase   = 3.0e6f;
bool SurfaceIsNone(float code)   { return code < 5.0e5f; }
bool SurfaceIsMesh(float code)   { return code >= 2.0e6f; }
float SurfaceHeight(float code)  { return SurfaceIsMesh(code) ? code - kSurfaceMeshBase : code - kSurfaceGroundBase; }
float EncodeSurface(float kind, float z)
{
	return kind < 0.5f ? 0.0f : kind < 1.5f ? z + kSurfaceGroundBase : z + kSurfaceMeshBase;
}
bool SurfacesDiffer(float a, float b, float band)
{
	if (!SurfaceIsMesh(a) && !SurfaceIsMesh(b)) { return false; }
	if (SurfaceIsNone(a) || SurfaceIsNone(b)) { return true; }
	return abs(SurfaceHeight(a) - SurfaceHeight(b)) > band;
}
float SurfaceWeight(float code, float z, float band, bool land)
{
	if (SurfaceIsNone(code) || (land && !SurfaceIsMesh(code))) { return land ? 1.0f : 0.0f; }
	return 1.0f - smoothstep(band * 0.75f, band, abs(SurfaceHeight(code) - z));
}
float GatedField(Texture2D<float> heights, Texture2D<float> surfaces, SamplerState state,
	float2 uv, float texels, float z, float band, bool land)
{
	const float plain = heights.SampleLevel(state, uv, 0.0f);
	[branch] if (plain == 0.0f) { return 0.0f; }
	const float4 codes = surfaces.GatherRed(state, uv);
	[branch] if (land && !any(codes >= 2.0e6f)) { return plain; }
	const float4 h = heights.GatherRed(state, uv);
	const float4 w = float4(SurfaceWeight(codes.x, z, band, land), SurfaceWeight(codes.y, z, band, land),
		SurfaceWeight(codes.z, z, band, land), SurfaceWeight(codes.w, z, band, land));
	const float2 f = frac(uv * texels - 0.5f);
	const float4 v = h * w;
	return lerp(lerp(v.w, v.z, f.x), lerp(v.x, v.y, f.x), f.y);
}
)";
}
