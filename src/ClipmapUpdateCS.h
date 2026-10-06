// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "Clipmap.h"

#include "Shelter.h"
#include "SnowCoverage.h"

#include <algorithm>
#include <format>
#include <string>

namespace Clipmap
{

	template <class Params>
	void FillStampBounds(Params& params, uint32_t count)
	{
		for (uint32_t i = 0; i < std::min(count, kMaxStamps); ++i) {
			const auto& stamp = params.stamps[i];
			const auto& motion = params.stampMotion[i];
			const bool snow = motion[2] > 0.5f;
			const float span = snow ? params.snowRim[0] : params.control[3];
			const float lean = snow ? params.snowRim[2] : params.rimShape[0];
			const float reach = std::max(stamp[2], params.stampShape[i][2]) *
				(1.0f + std::max(span, 0.0f) * (1.0f + std::clamp(lean, 0.0f, 1.0f)));
			for (uint32_t axis = 0; axis < 2; ++axis) {
				params.stampBounds[i][axis] = stamp[axis] + (std::min(0.0f, -motion[axis]) - reach);
				params.stampBounds[i][axis + 2] = stamp[axis] + (std::max(0.0f, -motion[axis]) + reach);
			}
		}
	}

	constexpr char kUpdateShader[] = R"(
RWTexture2D<float> Field : register(u0);
Texture2D<float> FieldBefore : register(t5);

RWTexture2D<float2> DecayRate : register(u1);

RWTexture2D<uint> Activity : register(u2);

SamplerState      FieldSampler : register(s0);

Texture2D<float> CoarseField : register(t1);
Texture2D<float2> CoarseDecay : register(t2);

Texture2D<float> SnowCoverageMap : register(t3);
Texture2D<float> SnowMeshCapMap : register(t4);

cbuffer Params : register(b0)
{

	float4 Window;

	float4 Control;

	float4 Weather;

	float4 Stamps[MAX_STAMPS];

	float4 StampParams[MAX_STAMPS];

	float4 StampShape[MAX_STAMPS];

	float4 StampMotion[MAX_STAMPS];

	float4 Coarse;

	float4 RimShape;

	float4 SnowRim;

	float4 Raise;

	float4 RaiseWindow;
	float4 StampBounds[MAX_STAMPS];
};

)"

		R"(

uint HashCell(int2 cell)
{
	uint2 q = uint2(cell) * uint2(1597334673u, 3812015801u);
	uint  n = (q.x ^ q.y) * 1597334673u;
	n ^= n >> 15;
	return n * 2246822519u;
}

float ValueNoise(float2 p)
{
	const float2 base = floor(p);
	float2       f = p - base;
	f = f * f * (3.0f - 2.0f * f);

	const int2 c = int2(base);
	const float a = (float)HashCell(c)                  * (1.0f / 4294967296.0f);
	const float b = (float)HashCell(c + int2(1, 0))     * (1.0f / 4294967296.0f);
	const float d = (float)HashCell(c + int2(0, 1))     * (1.0f / 4294967296.0f);
	const float e = (float)HashCell(c + int2(1, 1))     * (1.0f / 4294967296.0f);

	return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

float FractalNoise(float2 worldXY, float wavelength)
{
	const float2x2 turn = float2x2(0.7986f, -0.6018f, 0.6018f, 0.7986f);

	float2 p = worldXY / max(wavelength, 1e-3f);
	float  n = ValueNoise(p);

	p = mul(turn, p) * 2.03f;
	n += 0.5f * ValueNoise(p);

	return (n * (1.0f / 1.5f)) * 2.0f - 1.0f;
}

float RimNoise(float2 worldXY)
{
	return FractalNoise(worldXY, 6.0f);
}

float ChurnNoise(float2 worldXY)
{

	float n = FractalNoise(worldXY, 12.0f);

	n *= abs(n);

	const float patch = 0.65f + 0.35f * FractalNoise(worldXY, 70.0f);

	return n * patch;
}

float SnowBlanket(float2 worldXY)
{
	const float2 fromCentre = abs(worldXY - RaiseWindow.xy);
	const float  reach      = max(fromCentre.x, fromCentre.y);
	const float  fade       = 1.0f - saturate(
		(reach - RaiseWindow.z) / max(RaiseWindow.w - RaiseWindow.z, 1e-3f));
	if (fade <= 0.0f) {
		return 0.0f;
	}

	float2 uv = worldXY / kCoverageWorldSize;
	const float2 t = uv * kCoverageTexels + 0.5f;
	const float2 i = floor(t);
	float2 f = t - i;
	f = f * f * (3.0f - 2.0f * f);
	uv = (i + f - 0.5f) / kCoverageTexels;

	const float cover = SnowCoverageMap.SampleLevel(FieldSampler, uv, 0.0f);
	const float snowy = smoothstep(0.5f, 1.0f, cover);

	float lift = snowy * Raise.x * Raise.y;

	if (Raise.w > 0.5f) {
		float2 cuv = worldXY / kMeshCapWorldSize;
		const float2 ct = cuv * kMeshCapTexels + 0.5f;
		const float2 ci = floor(ct);
		float2 cf = ct - ci;
		cf = cf * cf * (3.0f - 2.0f * cf);
		cuv = (ci + cf - 0.5f) / kMeshCapTexels;

		float cap = SnowMeshCapMap.SampleLevel(FieldSampler, cuv, 0.0f);
		cap = lerp(cap, 1.0f, saturate(
			(reach - kMeshCapFadeStart) /
			max(kMeshCapFadeEnd - kMeshCapFadeStart, 1e-3f)));

		lift = min(lift, cap * Raise.x);
	}

	return lift * fade;
}

float2 SweptDelta(float2 worldXY, float4 s, float2 motion)
{
	const float2 delta = worldXY - s.xy;
	const float  lenSq = dot(motion, motion);
	if (lenSq <= 1e-4f) {
		return delta;
	}

	const float t = saturate(dot(delta + motion, motion) / lenSq);
	return delta + motion * (1.0f - t);
}

float StampDistance(float2 worldXY, float4 s, float4 shape, float2 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion);
	if (shape.z <= 0.0f) {
		return length(delta);
	}

	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float halfLength = max(s.z, 1e-3f);
	const float halfWidth = max(shape.z, 1e-3f);

	const float2 n = float2(dot(delta, forward) / halfLength,
		dot(delta, right) / halfWidth);

	return length(n) * halfLength;
}

)"

		R"(
groupshared uint gBlockMax;
static const uint kStampWords = (MAX_STAMPS + 31) / 32;
groupshared uint gStampHits[kStampWords];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0) {
		gBlockMax = 0;
	}
	if (groupIndex < kStampWords) {
		gStampHits[groupIndex] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	const int n = (int)Window.w;

	const int mask = n - 1;
	const int2 centreCell = int2(Window.xy);
	const int2 base = centreCell - (n >> 1);
	const int2 cell = base + int2(
		((int)id.x - base.x) & mask,
		((int)id.y - base.y) & mask);

	const float2 worldXY = float2(cell) * Window.z;

	float rimNoise = 0.0f;
	float churn = 0.0f;

	float h = FieldBefore[id.xy];
	float2 metadata = DecayRate[id.xy];
	float rate = metadata.x;
	float snow = metadata.y >= 0.5f ? 1.0f : 0.0f;

	if (Control.x > 0.0f) {
		h *= pow(max(saturate(rate), 1e-6f), Control.x);
	}

	if (Weather.x > 0.0f && Control.x > 0.0f) {
		h *= pow(max(1.0f - Weather.x, 0.0f), Control.x);
	}

)"
		R"(

	float melt = 0.0f;

	float meltFloor = 0.0f;

	float meltRate = 0.0f;

	float target = 0.0f;
	float targetRate = 0.0f;
	float targetSnow = 0.0f;

	const int2 first = (int2(gid.xy * 8) - base) & mask;
	const bool2 wraps = first + 7 > mask;
	const float2 groupLo = float2(base + (wraps ? 0 : first) - 1) * Window.z;
	const float2 groupHi = float2(base + (wraps ? mask : first + 7) + 1) * Window.z;
	const int count = min((int)Control.y, MAX_STAMPS);
	for (int candidate = (int)groupIndex; candidate < count; candidate += 64) {
		const float4 bounds = StampBounds[candidate];
		if (!any(groupHi < bounds.xy) && !any(groupLo > bounds.zw)) {
			InterlockedOr(gStampHits[candidate >> 5], 1u << (candidate & 31));
		}
	}
	GroupMemoryBarrierWithGroupSync();

	uint anyHit = 0;
	[unroll] for (uint w = 0; w < kStampWords; ++w) {
		anyHit |= gStampHits[w];
	}

	[branch] if (anyHit != 0) {
		rimNoise = max(Weather.w, SnowRim.y) > 0.0f ? RimNoise(worldXY) : 0.0f;
		churn = max(RimShape.y, SnowRim.w) > 0.0f ? ChurnNoise(worldXY) : 0.0f;
	}

	uint word = 0;
	uint hits = gStampHits[0];
	[loop] for (;;) {
		[loop] while (hits == 0 && word + 1 < kStampWords) {
			hits = gStampHits[++word];
		}
		if (hits == 0) { break; }
		const int i = (int)(word * 32 + firstbitlow(hits));
		hits &= hits - 1;
		const float4 s = Stamps[i];
		const float4 p = StampParams[i];
		const float2 motion = StampMotion[i].xy;
		const float stampSnow = StampMotion[i].z;
		const float4 rim = stampSnow > 0.5f ? SnowRim :
			float4(Control.w, Weather.w, RimShape.x, RimShape.y);

		const float2 delta = worldXY - s.xy;

		const float reach = max(s.z, StampShape[i].z) *
			(1.0f + max(rim.x, 0.0f) * (1.0f + saturate(rim.z)));
		const float2 lo = min(0.0f.xx, -motion) - reach;
		const float2 hi = max(0.0f.xx, -motion) + reach;
		if (any(delta < lo) || any(delta > hi)) {
			continue;
		}

		const float  d = StampDistance(worldXY, s, StampShape[i], motion);

		const float f = 1.0f - smoothstep(s.z * p.x, s.z, d);

		if (p.z > 0.5f && p.z < 1.5f) {
			const float meltHere = s.w * f;
			if (meltHere > melt) {
				melt = meltHere;

				meltFloor = p.w * f;
				meltRate = p.y;
			}
			continue;
		}

		float profile = -s.w * f * max(1.0f + rim.w * churn, 0.0f);

		if (p.w > 0.0f && rim.x > 0.0f) {
			const float span = max(s.z * rim.x, 1e-4f);

			const float lean  = saturate(rim.z);
			const float reach = span * (d >= s.z ? 1.0f + lean : 1.0f - lean);
			const float u = (d - s.z) / max(reach, 1e-4f);

			const float bump = saturate(1.0f - u * u);

			profile += p.w * bump * bump * max(1.0f + rim.y * rimNoise, 0.0f);
		}

		if (abs(profile) > abs(target)) {
			target = profile;
			targetRate = p.y;
			targetSnow = stampSnow;
		}
	}

	if (target < 0.0f && target < h) {
		h = target;
		rate = targetRate;
		snow = targetSnow;
	} else if (target > 0.0f && target > h && h >= 0.0f) {
		h = target;
		rate = targetRate;
		snow = targetSnow;
	}

	if (melt > 0.0f && Control.x > 0.0f) {
		const float remains = pow(max(1.0f - melt, 0.0f), Control.x);
		h = meltFloor + (h - meltFloor) * remains;

		if (meltFloor < 0.0f) {
			rate = max(rate, meltRate);
			snow = 1.0f;
		}
	}

)"

		R"(

	const float repose = snow > 0.5f ? RimShape.z : Weather.z;
	if (Weather.y > 0.0f && repose > 0.0f) {
		const float maxStep = Window.z * Weather.y;

		const int2 im = int2(mask, mask);
		const float n0 = FieldBefore[uint2((int2(id.xy) + int2(-1, 0)) & im)];
		const float n1 = FieldBefore[uint2((int2(id.xy) + int2(1, 0)) & im)];
		const float n2 = FieldBefore[uint2((int2(id.xy) + int2(0, -1)) & im)];
		const float n3 = FieldBefore[uint2((int2(id.xy) + int2(0, 1)) & im)];

		float pull = 0.0f;
		pull += sign(n0 - h) * max(abs(n0 - h) - maxStep, 0.0f);
		pull += sign(n1 - h) * max(abs(n1 - h) - maxStep, 0.0f);
		pull += sign(n2 - h) * max(abs(n2 - h) - maxStep, 0.0f);
		pull += sign(n3 - h) * max(abs(n3 - h) - maxStep, 0.0f);

		h += pull * 0.25f * saturate(repose);
	}

	const int2 delta = abs(cell - centreCell);
	if (max(delta.x, delta.y) >= (int)Coarse.w) {
		if (Coarse.x > 0.5f) {

			const float2 uv = worldXY / max(Coarse.y, 1e-3f);

			const float o = (0.5f / Window.w) * Coarse.z;

			float sum = CoarseField.SampleLevel(FieldSampler, uv + float2(-o, -o), 0).r;
			sum += CoarseField.SampleLevel(FieldSampler, uv + float2(o, -o), 0).r;
			sum += CoarseField.SampleLevel(FieldSampler, uv + float2(-o, o), 0).r;
			sum += CoarseField.SampleLevel(FieldSampler, uv + float2(o, o), 0).r;
			h = sum * 0.25f;

			const float2 coarseMetadata = CoarseDecay.SampleLevel(FieldSampler, uv, 0);
			rate = coarseMetadata.x;
			snow = coarseMetadata.y >= 0.5f ? 1.0f : 0.0f;
		} else {
			h = 0.0f;
			rate = 0.0f;
			snow = 0.0f;
		}
	}

	if (Raise.x > 0.0f && snow > 0.5f && h < 0.0f) {
		h = max(h, -(SnowBlanket(worldXY) + Raise.z));
	}

	Field[id.xy] = h;
	DecayRate[id.xy] = float2(rate, snow);

	InterlockedMax(gBlockMax, asuint(abs(h)));
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0) {

		const int2 centre = int2(gid.xy / kActivityGroups);
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {

				InterlockedMax(
					Activity[uint2((centre + int2(dx, dy)) & kActivityWrap)], gBlockMax);
			}
		}
	}
}
)";

	inline std::string UpdateShaderSource()
	{
		return std::format(
				"static const uint2 kActivityGroups = uint2({}, {});\n"

				"static const int2 kActivityWrap = int2({}, {});\n"

				"static const float kCoverageWorldSize = {:.4f}f;\n"
				"static const float kCoverageTexels    = {:.1f}f;\n"
				"static const float kMeshCapWorldSize  = {:.4f}f;\n"
				"static const float kMeshCapTexels     = {:.1f}f;\n"

				"static const float kMeshCapFadeStart  = {:.4f}f;\n"
				"static const float kMeshCapFadeEnd    = {:.4f}f;\n",
				kActivityRatio / 8, kActivityRatio / 8,
				kActivityTexels - 1, kActivityTexels - 1,
				SnowCoverage::kWorldSize, static_cast<float>(SnowCoverage::kTexels),
				Shelter::kWorldSize, static_cast<float>(Shelter::kTexels),
				Shelter::kWorldSize * 0.39f, Shelter::kWorldSize * 0.47f) +
			kUpdateShader;
	}
}
