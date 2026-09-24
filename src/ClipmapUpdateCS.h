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

	inline constexpr float kPrintCoreLo = 0.06f;
	inline constexpr float kPrintCoreHi = 0.96f;
	inline constexpr float kPrintBandRiseLo = 0.14f;
	inline constexpr float kPrintBandRiseHi = 0.34f;
	inline constexpr float kPrintBandFallLo = 0.52f;
	inline constexpr float kPrintBandFallHi = 0.84f;

	inline float PrintHeightFor(float a_mask, float a_depth, float a_rim)
	{
		const auto smoothstep = [](float a_lo, float a_hi, float a_x) {
			const float t = std::clamp((a_x - a_lo) / (a_hi - a_lo), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};

		const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, a_mask);
		const float band = std::clamp(smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, a_mask) -
										  smoothstep(kPrintBandFallLo, kPrintBandFallHi, a_mask),
			0.0f, 1.0f);

		return a_rim * band - a_depth * core;
	}

	constexpr char kUpdateShader[] = R"(
RWTexture2D<float> Field : register(u0);

RWTexture2D<float2> DecayRate : register(u1);

RWTexture2D<uint> Activity : register(u2);

Texture2D<float4> ShapeMask : register(t0);
SamplerState      ShapeSampler : register(s0);

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

	// Per-stamp loose snow at a stamp's lip.  x is the amplitude, y the
	// band's width as a fraction of the stamp's radius; zw are unused and
	// reserved, so the next lip-shaped term does not have to widen the
	// buffer again.
	//
	// This array exists because every existing per-stamp channel was
	// already spoken for when the crater's lip needed one.  StampMotion.w
	// carries rimBulge (see RadialBulge), StampShape.w is a print's mirror
	// (read in PrintProfile), all four Control lanes carry the frame's own
	// numbers, and StampParams.w is the footprint lip - which is exactly
	// the term wanted here, except that MagicImpacts pins it to zero for
	// every blast stroke and the shader gates the whole footprint branch on
	// it being non-zero.  Reusing it would have meant either reopening a
	// gate that exists to stop a crater spreading 2.7x past its pattern, or
	// special-casing explosions inside it.  A lane of their own costs 1 KB
	// for 64 stamps and keeps both behaviours independent.
	float4 StampNoise[MAX_STAMPS];

	float4 Coarse;

	float4 RimShape;

	float4 SnowRim;

	float4 Raise;

	float4 RaiseWindow;

	// How far a line mark's rim edge may be pushed in or out, and the
	// wavelength of the noise that does it.  See RimEdgeJitter.
	//
	// Appended rather than packed into a free lane, because there is no free
	// lane - every channel of every field above is read somewhere in this
	// file.  Appending is also the only insertion the byte-offset binding
	// tolerates: a field added anywhere else reinterprets every field after
	// it, which is noted on the C++ mirror in Clipmap.cpp for the same
	// reason.
	//
	// x is the jitter as a fraction of the mark's own width (so it scales
	// with the mark rather than with the world), y is the noise's wavelength
	// in world units, zw are reserved.
	float4 MarkRimJitter;
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

// How far a line mark's outline is pushed in or out at a given world position,
// as a fraction of the mark's own width.
//
// The rim's outer edge is an isocontour of the mark's distance field, so
// without this it is exactly the mark's shape - a smooth ellipse.  A weapon is
// re-stamped every frame while the character walks, each frame shifting the
// outline a little, so a smooth ellipse re-stamped at a walking pace lays down
// a ridge where each stamp's edge crosses the last one.  The ridges are evenly
// spaced because the stride is even, and the shape is a constant because the
// mark's shape is a constant, which together read as a plank's outline rather
// than as disturbed snow.  This is the term that roughs that contour up.
//
// Sampled at the world position and not at the mark, so two stamps that cover
// the same ground agree about where the edge is.  That matters more than it
// sounds: the marks overlap heavily, and a noise that moved with each stamp
// would have every one of them cut a different ragged edge into the same
// texels - the field's merge would then keep the deepest of a dozen unrelated
// edges, which reads as mush rather than as crumbs.  Seeding from the world
// also means the edge does not crawl when a frame is re-stamped over the same
// ground, so the roughness is stable while the character is still.
//
// The outer reach is pushed further than the inner so the band can only grow
// outward; a texture that pulled its own edge inward at random would leave
// gaps in the bank where two lobbies disagree.
float RimEdgeJitter(float2 worldXY, float a_amount, float a_wavelength)
{
	if (!(a_amount > 0.0f)) {
		return 0.0f;
	}

	const float w = max(a_wavelength, 1e-3f);
	const float n = FractalNoise(worldXY, w);

	// Skewed toward the outside: the noise's own zero is mapped a little below
	// it, so the band is ragged in both directions but spends more of its
	// travel outward.
	return a_amount * (0.5f + 0.5f * n) - 0.25f * a_amount;
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

// How far the rim of a crater bulges in or out at a given bearing.
//
// This exists because length(delta) is a circle, and a circle is what a
// smooth distance function can only ever produce.  Every knob the pattern
// had - reach, depth, shoulder - scaled that circle; none of them could
// change its shape, which is why a player kept reporting a perfectly round
// hole however the numbers were set.  The outline was not under-tuned, it
// was determined.
//
// The fix is to make the radius a function of the bearing rather than a
// constant.  n here is in [-1, 1] and the caller scales it into a fraction
// of the stamp's own radius, so the crater edge becomes a closed wavy
// curve instead of an arc.  Two features matter and both come from using
// smooth noise rather than a per-stamp constant: adjacent bearings get
// nearby values, so the edge bulges in lobes rather than jittering per
// texel; and the same world position always yields the same value, so a
// stamp held still does not crawl.
//
// The noise is sampled at the *bearing* - the direction from the stamp's
// centre - not at the world position.  Sampling the position directly
// would break the shape apart: two texels at the same distance but
// different bearings would get unrelated offsets, and the edge would fray
// into noise instead of waving.  Feeding an angle means the disturbance is
// periodic in the one variable that is itself periodic around the rim.
float RadialBulge(float2 offset, float2 centre)
{
	const float2 dir = offset;
	const float  len = length(dir);
	if (len < 1e-4f) {
		return 0.0f;
	}

	// The bearing as a point on the unit circle, scaled so the noise's own
	// lattice is a handful of lobes around the rim rather than one.  The
	// centre term decorrelates one crater from the next while staying fixed
	// for any single one - a blast's outline must not depend on when it was
	// drawn.
	const float2 bearing = (dir / len) * 2.4f + centre * 0.013f;

	return FractalNoise(bearing, 1.0f);
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

	const float cover = SnowCoverageMap.SampleLevel(ShapeSampler, uv, 0.0f);
	const float snowy = smoothstep(0.5f, 1.0f, cover);

	float lift = snowy * Raise.x * Raise.y;

	if (Raise.w > 0.5f) {
		float2 cuv = worldXY / kMeshCapWorldSize;
		const float2 ct = cuv * kMeshCapTexels + 0.5f;
		const float2 ci = floor(ct);
		float2 cf = ct - ci;
		cf = cf * cf * (3.0f - 2.0f * cf);
		cuv = (ci + cf - 0.5f) / kMeshCapTexels;

		float cap = SnowMeshCapMap.SampleLevel(ShapeSampler, cuv, 0.0f);
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

float PrintProfile(float2 worldXY, float4 s, float4 p, float4 shape, float2 motion)
{
	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float2 delta = SweptDelta(worldXY, s, motion);

	const float2 local = float2(dot(delta, right), dot(delta, forward));
	const float  halfWidth = max(shape.z, 1e-3f);
	const float  halfLength = max(s.z, 1e-3f);

	const float2 uv = float2(
		0.5f + (local.x * shape.w) / (halfWidth * 2.0f),
		0.5f - local.y / (halfLength * 2.0f));

	if (any(uv < 0.0f) || any(uv > 1.0f)) {
		return 0.0f;
	}

	const float m = ShapeMask.SampleLevel(ShapeSampler, uv, 0).r;

	const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, m);
	const float band = saturate(
		smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, m) -
		smoothstep(kPrintBandFallLo, kPrintBandFallHi, m));

	return p.w * band - s.w * core;
}

float StampDistance(float2 worldXY, float4 s, float4 shape, float4 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion.xy);
	if (shape.z <= 0.0f) {
		// A round stamp is a circle, and for most marks that is correct - a
		// footprint is round.  A crater is not: it is the hole a thrown mass
		// left, and a thrown mass tears an outline.  motion.w carries how
		// much of the radius the rim may vary by, in [0, 1]; zero keeps the
		// circle and every existing mark is unaffected.
		//
		// The bulge is applied to the distance, not to the profile, so the
		// whole crater moves with it - the lip, the wall angle and the floor
		// all follow the same wavy edge.  Scaling the profile instead would
		// leave the rim in place and only dent the depth, which reads as
		// lumps inside a round hole rather than as an irregular hole.
		const float bulge = motion.w;
		if (bulge > 0.0f) {
			const float radius = max(s.z, 1e-4f);
			const float scaled = radius * (1.0f + bulge * RadialBulge(delta, s.xy));
			return length(delta) * radius / max(scaled, 1e-4f);
		}
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

// How far a point lies beyond the mark's outline, in world units.
//
// StampDistance answers in the mark's own units, and for an ellipse those are
// not world units: it returns the normalised distance multiplied back by the
// *half length*, so one unit of it measures the half length along the long axis
// but only the half width along the short one.  A width in world units - and the
// band of raised snow is exactly that - must therefore not be read off that
// number.  A band of five units at the ends of a groove would be five units
// divided by the aspect ratio along its sides, which for a long thin weapon is a
// fraction of a texel: the snow banks up on the two ends of the mark and nowhere
// else, and the groove reads as a bare cut with piles at its ends.
//
// The first-order distance to the outline is (length(n) - 1) / |grad length(n)|.
// It is exact along both axes - it reduces to halfLength * (len - 1) on the long
// one and halfWidth * (len - 1) on the short one - and it is identically
// StampDistance - s.z for a circle, so every round mark keeps the rim, the lip
// band and the falloff it had.
float StampOutlineOvershoot(float2 worldXY, float4 s, float4 shape, float4 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion.xy);

	if (shape.z <= 0.0f) {
		// A disc, whose edge motion.w may have pushed out by a fraction of its
		// radius.  Once that is non-zero the current distance and the world
		// distance part company, so the edge is rebuilt here rather than
		// assumed to be s.z.
		const float radius = max(s.z, 1e-4f);
		const float scaled = radius * (1.0f + motion.w * RadialBulge(delta, s.xy));
		return length(delta) - scaled;
	}

	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float halfLength = max(s.z, 1e-3f);
	const float halfWidth = max(shape.z, 1e-3f);

	const float2 n = float2(dot(delta, forward) / halfLength,
		dot(delta, right) / halfWidth);
	const float  len = max(length(n), 1e-6f);

	const float2 grad = float2(n.x / (len * halfLength), n.y / (len * halfWidth));
	const float  invGrad = max(length(grad), 1e-6f);

	return (len - 1.0f) / invGrad;
}

)"

		R"(
groupshared uint gBlockMax;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0) {
		gBlockMax = 0;
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
	bool  haveNoise = false;

	float h = Field[id.xy];
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

	// Thrown snow at a crater's lip, accumulated across stamps and applied
	// after the pit merge - see where it is written below for why it cannot
	// ride the normal target path.
	//
	// It is carried in two parts rather than as one accumulable number.  The
	// value is a displacement and the merge is a min, so the obvious
	// "start at a very negative number and take the min" is wrong: the
	// sentinel would pass the very test that decides whether anything was
	// written.  A blast's rim snow is only raised on some texels, and the
	// rest have to keep the pit's own value - so "nothing was written" and
	// "something was written" have to be distinguishable, and a number that
	// is itself a valid candidate cannot do that.  A flag can.
	float lipSnow = 0.0f;
	bool  haveLip = false;

	const int count = (int)Control.y;
	for (int i = 0; i < count; ++i) {
		const float4 s = Stamps[i];
		const float4 p = StampParams[i];
		const float4 motion4 = StampMotion[i];
		const float2 motion = motion4.xy;
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

		if (!haveNoise) {
			haveNoise = true;
			rimNoise = max(Weather.w, SnowRim.y) > 0.0f ? RimNoise(worldXY) : 0.0f;
			churn = max(RimShape.y, SnowRim.w) > 0.0f ? ChurnNoise(worldXY) : 0.0f;
		}

		const float  d = StampDistance(worldXY, s, StampShape[i], motion4);

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

		if (p.z > 1.5f) {
			const float printed =
				PrintProfile(worldXY, s, p, StampShape[i], motion);
			if (abs(printed) > abs(target)) {
				target = printed;
				targetRate = p.y;
				targetSnow = stampSnow;
			}
			continue;
		}

		float profile = -s.w * f * max(1.0f + rim.w * churn, 0.0f);

		if (p.w > 0.0f && rim.x > 0.0f) {
			// How wide the raised snow is, and it is a fraction of the
			// mark's own *width* rather than of its reach.
			//
			// For a circle the two are the same number, because a disc's
			// radius is the only size it has, and every mark that had a rim
			// before this was a circle.  For a line they are not the same at
			// all: s.z is the line's half *length*, which for a carried
			// weapon is tens of units, so reading the band off it would raise
			// a bank of snow as wide as the weapon is long on either side of
			// its groove.  Carried weapons had their rim turned off rather
			// than scaled down, and this is why - the size field was read as
			// a different size, which is the same mistake this shader has
			// been bitten by before.
			//
			// StampShape.z is the half width, zero for a circle, so it is
			// exactly the reference that is missing for a line.
			//
			// The reference on its own was not enough.  The band is a width,
			// and the distance it was measured against was not: StampDistance
			// hands back the half length along a line's long axis but only the
			// half width along its short one, so a band fed to it directly came
			// out narrower by the aspect ratio along the groove's sides than
			// across its ends.  A bow's recorded marks ran 5.3 to 12.6 half
			// length against 2.6 to 2.9 half width, which puts one to four
			// texels of snow on the sides against six to seven across the ends,
			// on a 0.75 unit grid.  The bank was there, but thinner than the
			// grid on the two sides - which is the part of a groove a player
			// looks at - so a carried weapon read as a bare cut.  The overshoot
			// is the same edge measured in world units.
			const float bandRef = StampShape[i].z > 0.0f ? StampShape[i].z : s.z;

			// Two cells, because a band narrower than the grid it is written to
			// cannot be drawn: the texels either side of the outline miss it.
			// At the coarse level the cell is four times larger and so is this
			// floor, which is right - that level cannot resolve anything finer
			// either.
			const float band = max(bandRef * rim.x, 2.0f * Window.z);
			const float gap  = StampOutlineOvershoot(worldXY, s, StampShape[i], motion4);

			// A footprint's mark is already a rounded oval that the feet place
			// one at a time, so its rim reads as snow however smooth its edge
			// is and it is deliberately left alone - the shape it has is the
			// shape the user asked the weapon to match.  A line is the case
			// that needs the edge broken up, because it is re-stamped every
			// frame at a walking stride and its outline is as long as the
			// weapon.  Gating on `shape.z` is what separates the two, since
			// that is zero for a circle and non-zero only for a line.
			float jitter = 0.0f;
			float bandScale = 1.0f;
			if (StampShape[i].z > 0.0f && MarkRimJitter.x > 0.0f) {
				jitter = RimEdgeJitter(worldXY, MarkRimJitter.x,
							 MarkRimJitter.y) *
					band;

				// The band's own width is scrambled too, a little, so the
				// roughness survives where the edge jitter happens to be
				// small - a band of exactly even width with a ragged edge
				// still reads as a machined shape.
				bandScale = 1.0f + 0.5f * jitter / max(band, 1e-4f);
			}

			const float lean  = saturate(rim.z);
			const float reach = band * bandScale *
				(gap >= 0.0f ? 1.0f + lean : 1.0f - lean);
			const float u = (gap - jitter) / max(reach, 1e-4f);

			const float bump = saturate(1.0f - u * u);

			profile += p.w * bump * bump * max(1.0f + rim.y * rimNoise, 0.0f);
		}

		// Loose snow thrown clear of the lip, for a stamp that asks for it.
		//
		// A footprint gets this from the branch above: p.w is its lip height
		// and rimNoise is the 6-unit-wavelength field that roughens it into
		// crumbs.  A crater cannot use that branch - MagicImpacts pins its
		// rim to zero, because the shader would otherwise carry its edge out
		// to 2.7x the radius the pattern asked for - so a blast raises no
		// lip noise at all and its rim reads as smooth-cut.
		//
		// This band sits where a footprint's crumbs sit: on the outside of
		// the wall, at the point the depression is giving way to level
		// ground.  The wall's own falloff already spends the last
		// (1 - p.x) of the radius, so the band is measured from there, and
		// it reaches a little past the lip - into the ring of the stamp's
		// own disc, which is where the snow a blast throws actually lands.
		//
		// The noise is sampled directly rather than through the global
		// rimNoise above.  That variable is gated on the frame's rim-noise
		// settings being non-zero, which is a footprint concern: on a
		// surface profile that has no rim noise the crater's snow would
		// vanish with it, and a blast throws snow whether or not anything
		// walked there.  Sampling here keeps the two independent, and
		// FractalNoise is cheap next to the texture fetches already done.
		const float lipAmount = StampNoise[i].x;
		const float lipBand   = StampNoise[i].y;
		if (lipAmount > 0.0f && lipBand > 0.0f) {
			const float inner = s.z * saturate(p.x);
			const float outer = s.z * (1.0f + lipBand);
			const float u = saturate((d - inner) / max(outer - inner, 1e-4f));

			// A hump rather than a shelf: 0 at the shoulder, 1 at the lip,
			// back to 0 past it, so the band has no step at either end and
			// joins the untouched field smoothly.
			const float hump = saturate(1.0f - (2.0f * u - 1.0f) * (2.0f * u - 1.0f));

			// Always positive, because thrown snow can only pile up - a
			// negative lobe would dig a second trench around the crater,
			// which is the opposite of what is wanted.  FractalNoise is in
			// [-1, 1], so this maps it to [0, 1].
			const float crumb = 0.5f + 0.5f * RimNoise(worldXY + s.xy);

			// Scaled by how much rock this stamp moved, which is s.w - the
			// depth - and not by -s.w.  A stamp's contribution to the field
			// is -s.w * falloff, so a pit is a *positive* depth and s.w is
			// the magnitude of the carve.  The first version of this line
			// carried max(-s.w, 0.0f), which is zero for every digging stamp
			// there is - the lip term was identically zero and the crater
			// stayed bare.  The assertion in StampSurfaceTest caught it.
			const float thrown = max(s.w, 0.0f);

			// The lip term fed to the merge below.  It is the wall's own
			// displacement with the crumb field cut further into it, so a
			// texel where the crumbs are tall sits lower than the wall
			// beside it and a texel where they are short sits at the wall.
			// The merge keeps the minimum, so what survives is a wall whose
			// surface is carved wherever the noise is strong: crumbs cut out
			// of the crater's own wall.
			//
			// haveLip is what makes the term's absence expressible.  The
			// accumulation cannot start from a chosen "empty" number, because
			// whichever number that is would also be a valid height and would
			// win the min on every texel the band does not reach - which is
			// the whole field outside the lip.  The first version of this
			// started at -1e9 and flattened every crater in the test to
			// exactly that, pit and all.
			const float cut = thrown * f + lipAmount * hump * crumb * thrown;
			lipSnow = haveLip ? min(lipSnow, -cut) : -cut;
			haveLip = true;
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

	// Thrown snow cuts into whatever the pit merge decided, including into
	// the wall of the crater it was thrown from.  Merging it here rather
	// than folding it into target keeps it off the merge's h >= 0 condition
	// without weakening that condition for the footprints it exists for.
	//
	// It is a min against the pit, not an addition to it, and lipSnow is a
	// displacement rather than a rise - the same units as h.  A stamp
	// persists for as long as its decay lets it, so the shader runs over it
	// every frame: an addition would deepen another band on the last one and
	// the lip would be a trench within a second.  A min is idempotent - the
	// second frame finds a field that already carries the band and leaves it
	// - and the test that caught the additive version is "Repeated impact
	// must not deepen or accumulate rims".
	//
	// The comparison is against the field, not against zero.  h is a
	// displacement from the terrain, so zero is undisturbed ground and the
	// crater's wall is negative; a band that only ever cuts downward has to
	// be allowed to lose to a wall that is already deeper, and forcing a
	// comparison against zero would raise every such texel back to level.
	if (haveLip) {
		h = min(h, lipSnow);
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
		const float n0 = Field[uint2((int2(id.xy) + int2(-1, 0)) & im)];
		const float n1 = Field[uint2((int2(id.xy) + int2(1, 0)) & im)];
		const float n2 = Field[uint2((int2(id.xy) + int2(0, -1)) & im)];
		const float n3 = Field[uint2((int2(id.xy) + int2(0, 1)) & im)];

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

			float sum = CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, -o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, -o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, o), 0).r;
			sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, o), 0).r;
			h = sum * 0.25f;

			const float2 coarseMetadata = CoarseDecay.SampleLevel(ShapeSampler, uv, 0);
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
				"static const float kPrintCoreLo = {:.6f};\n"
				"static const float kPrintCoreHi = {:.6f};\n"
				"static const float kPrintBandRiseLo = {:.6f};\n"
				"static const float kPrintBandRiseHi = {:.6f};\n"
				"static const float kPrintBandFallLo = {:.6f};\n"
				"static const float kPrintBandFallHi = {:.6f};\n"

				"static const uint2 kActivityGroups = uint2({}, {});\n"

				"static const int2 kActivityWrap = int2({}, {});\n"

				"static const float kCoverageWorldSize = {:.4f}f;\n"
				"static const float kCoverageTexels    = {:.1f}f;\n"
				"static const float kMeshCapWorldSize  = {:.4f}f;\n"
				"static const float kMeshCapTexels     = {:.1f}f;\n"

				"static const float kMeshCapFadeStart  = {:.4f}f;\n"
				"static const float kMeshCapFadeEnd    = {:.4f}f;\n",
				kPrintCoreLo, kPrintCoreHi, kPrintBandRiseLo, kPrintBandRiseHi,
				kPrintBandFallLo, kPrintBandFallHi,
				kActivityRatio / 8, kActivityRatio / 8,
				kActivityTexels - 1, kActivityTexels - 1,
				SnowCoverage::kWorldSize, static_cast<float>(SnowCoverage::kTexels),
				Shelter::kWorldSize, static_cast<float>(Shelter::kTexels),
				Shelter::kWorldSize * 0.39f, Shelter::kWorldSize * 0.47f) +
			kUpdateShader;
	}
}
