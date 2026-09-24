// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cmath>

// Where a carried object meets the ground, and how wide its mark is drawn.
//
// This lives in its own header, with no engine types in it, for one reason:
// it is the arithmetic the whole carried-weapon fix turns on, and arithmetic
// that cannot be called from the offline test is arithmetic that cannot be
// tested.  Clipmap.cpp includes this and calls it with the numbers
// ActorShapes already measures; StampSurfaceTest includes it and asserts on
// the result.  The two therefore cannot drift.
//
// What it answers, and why the answer is not the bounding box:
//
// A stamp placed at the bound centre marks one point rigid to the body.  The
// recorded run showed what that costs - the bow's bound centre held at side
// +17 to +31 while its lowest point swung through twelve units of height, so
// walking and sprinting marked the same place and the trail read as an object
// laid beside the character rather than carried by it.
//
// The object's own axis is the missing half.  ActorShapes::GetLongAxis reads
// it out of the live hkpRigidBody rotation, so it follows the animation, and
// ActorShapes::GetExtent gives the half length it reaches along that axis.
// Stepping half a length either way and keeping the lower end is therefore
// the part of the object that is nearest the snow - and it moves when the
// arm swings, because half of it is the transform.
//
// For a capsule that step is exact rather than approximate: the support
// function along a capsule's own axis is exactly its half length, so the
// derived point is the shape's lowest point and no projection is needed.  For
// a box it is the bound corner, which is the same order of error the extent
// walk already carries.
namespace ContactPoint
{
	struct Result
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };

		// The other end.  Kept because the ground gate needs to know whether
		// *any* part of the object is down: an object whose lower end is
		// buried and whose upper end is in the air is touching the ground,
		// and its upper end is what must not be measured against it.
		float farX{ 0.0f };
		float farY{ 0.0f };
		float farZ{ 0.0f };

		// Which end was chosen, as the sign along the axis.  Reported so the
		// test can assert the choice rather than only the arithmetic.
		float lowSign{ 0.0f };

		// False when the axis was not usable, in which case the centre is
		// returned unchanged - the old behaviour rather than a worse one.
		bool live{ false };
	};

	inline Result LowestEnd(float a_cx, float a_cy, float a_cz, float a_halfLength,
		float a_ax, float a_ay, float a_az, bool a_haveAxis)
	{
		Result out{};
		out.farX = a_cx;
		out.farY = a_cy;
		out.farZ = a_cz;
		out.x = a_cx;
		out.y = a_cy;
		out.z = a_cz;

		if (!a_haveAxis) {
			return out;
		}

		const float ax = a_ax;
		const float ay = a_ay;
		const float az = a_az;
		const float len = std::sqrt(ax * ax + ay * ay + az * az);
		if (!(len > 0.0f) || !std::isfinite(len)) {
			return out;
		}

		const float half = (a_halfLength > 0.0f && std::isfinite(a_halfLength)) ? a_halfLength : 0.0f;

		const float px = ax / len;
		const float py = ay / len;
		const float pz = az / len;

		const float oneX = a_cx + px * half;
		const float oneY = a_cy + py * half;
		const float oneZ = a_cz + pz * half;

		const float twoX = a_cx - px * half;
		const float twoY = a_cy - py * half;
		const float twoZ = a_cz - pz * half;

		if (!std::isfinite(oneX) || !std::isfinite(oneY) || !std::isfinite(oneZ) ||
			!std::isfinite(twoX) || !std::isfinite(twoY) || !std::isfinite(twoZ)) {
			return out;
		}

		// Ties go to the positive end by construction, which keeps the choice
		// deterministic: a weapon held exactly horizontal must not flip its
		// mark from frame to frame on floating point noise.
		const bool firstIsLow = oneZ <= twoZ;

		out.x = firstIsLow ? oneX : twoX;
		out.y = firstIsLow ? oneY : twoY;
		out.z = firstIsLow ? oneZ : twoZ;

		out.farX = firstIsLow ? twoX : oneX;
		out.farY = firstIsLow ? twoY : oneY;
		out.farZ = firstIsLow ? twoZ : oneZ;

		out.lowSign = firstIsLow ? 1.0f : -1.0f;
		out.live = true;
		return out;
	}

	// Where the object's own axis crosses the ground.
	//
	// This is the answer that does not need a distance bound, and the previous
	// attempt did.  Keeping the axis's lower *end* is only right for an object
	// standing on its end: a rod carried at an angle has that end hanging in
	// the air, and what is actually in the snow is the point where the rod
	// crosses the surface.  Using the end therefore moved the mark, but not
	// necessarily onto the ground - and with a measured half length of 67.89
	// units it moved the mark further out than the character is wide.
	//
	// Solving for the crossing fixes both problems at once and introduces no
	// constant:
	//
	//     p(t) = c + t * a
	//     p.z(t) = landZ            =>  t = (landZ - c.z) / a.z
	//
	// The offset is then whatever the ground height says it is, so it cannot
	// run away: the derived point is on the ground by construction.  And the
	// same equation answers "is it touching at all" - a crossing further along
	// the axis than the object is long means the object cannot reach the
	// ground, which is not touching, and that is a geometric conclusion rather
	// than a threshold somebody picked.
	//
	// t is reported in units of the half length, so |t| <= 1 is exactly
	// "the crossing is on the object" and needs no scale to interpret.
	struct AxisHit
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float t{ 0.0f };        // crossing in units of the half length
		bool  onAxis{ false };  // |t| <= 1, so the crossing is on the object
		bool  live{ false };    // an axis was usable and a crossing was found
	};

	inline AxisHit AxisGroundHit(float a_cx, float a_cy, float a_cz, float a_halfLength,
		float a_ax, float a_ay, float a_az, float a_landZ, bool a_haveAxis)
	{
		AxisHit out{};
		out.x = a_cx;
		out.y = a_cy;
		out.z = a_cz;

		if (!a_haveAxis) {
			return out;
		}

		const float len = std::sqrt(a_ax * a_ax + a_ay * a_ay + a_az * a_az);
		if (!(len > 0.0f) || !std::isfinite(len)) {
			return out;
		}

		if (!(a_halfLength > 0.0f) || !std::isfinite(a_halfLength) ||
			!std::isfinite(a_landZ)) {
			return out;
		}

		const float px = a_ax / len;
		const float py = a_ay / len;
		const float pz = a_az / len;

		// A level axis never crosses the ground: every point on it is at the
		// same height, so the question becomes whether that height is the
		// ground's, and the centre answers it as well as any other point.  The
		// threshold is a slope in units of height per unit of axis, so it also
		// bounds how far the search may reach: at this slope a crossing within
		// one half length exists only for a centre already within a thousandth
		// of a unit of the ground, and reporting the centre is then correct.
		constexpr float kMinSlope = 1.0e-3f;
		if (std::abs(pz) < kMinSlope) {
			out.onAxis = true;
			out.live = true;
			return out;
		}

		const float t = (a_landZ - a_cz) / pz;
		if (!std::isfinite(t)) {
			return out;
		}

		out.t = t / a_halfLength;
		out.x = a_cx + px * t;
		out.y = a_cy + py * t;
		out.z = a_cz + pz * t;

		// The crossing is on the object only if it lies within the half length
		// the extent walk measured.  When it does not, the object is above the
		// ground along its whole length and there is nothing to mark.
		out.onAxis = std::abs(out.t) <= 1.0f;
		out.live = true;
		return out;
	}

	// The stretch of the object that is inside the snow.
	//
	// The crossing above answers *where* an object touches.  This answers the
	// other half of the question - *how much of it* touches - and that is what
	// makes a bow, a rod and a shield boss leave marks of their own shape
	// instead of the same bar.
	//
	// It replaces the last hand-picked number in the carried-weapon path.  The
	// drawn length was ShaftContactLineLength, one value for every weapon and
	// every pose, and the log showed what that costs: a mark that is the same
	// length whether the weapon has driven its tip twenty units under the snow
	// or has barely broken the surface.  The buried stretch is what the ground
	// itself says, so it is measured rather than chosen.
	//
	// Measured by walking the axis and asking the land under each sample, not
	// by solving one equation.  One crossing is exact on level ground and only
	// there; a slope puts the surface at a different height at every step
	// along the object, and the buried stretch is then bounded by two
	// crossings that no single land height can produce.  Sampling gets both,
	// and gets them for a curved surface as well, which is what a deforming
	// snow field is.
	//
	// Only a sampler is asked - never the terrain directly - which is why this
	// is a template rather than a call into TES.  The offline test hands it a
	// plane and asserts on the answer, so the arithmetic cannot drift from the
	// arithmetic the game runs.
	struct Span
	{
		float t0{ 0.0f };   // first buried parameter, in units of the half length
		float t1{ 0.0f };   // last buried parameter, likewise

		float x0{ 0.0f };   // world position of the first buried end
		float y0{ 0.0f };
		float z0{ 0.0f };

		float x1{ 0.0f };   // world position of the last buried end
		float y1{ 0.0f };
		float z1{ 0.0f };

		float midX{ 0.0f };  // middle of the buried stretch - where the mark goes
		float midY{ 0.0f };
		float midZ{ 0.0f };

		float nearX{ 0.0f };  // closest approach, when nothing is buried
		float nearY{ 0.0f };
		float nearZ{ 0.0f };

		float length{ 0.0f };  // world length of the buried stretch
		float depth{ 0.0f };   // deepest sample, in world units under the land
		float gap{ 0.0f };     // closest approach to the land when not buried

		// |z - land| at the ends that were refined, worst of the two, or -1
		// when both ends ran off the object (a fully buried object has no
		// refined end to check, and reporting zero there would be a claim the
		// measurement did not make).
		float resid{ -1.0f };

		int  sampled{ 0 };  // land samples attempted
		int  known{ 0 };    // of those, ones that returned a height
		bool live{ false }; // known > 0 and part of the axis is under the land
	};

	template <class LandFn>
	inline Span AxisSpan(float a_cx, float a_cy, float a_cz,
		float a_ax, float a_ay, float a_az, float a_halfLength,
		LandFn&& a_land, int a_steps, float a_lowOffset = 0.0f)
	{
		Span out{};
		out.nearX = a_cx;
		out.nearY = a_cy;
		out.nearZ = a_cz;
		out.midX = a_cx;
		out.midY = a_cy;
		out.midZ = a_cz;

		const float len = std::sqrt(a_ax * a_ax + a_ay * a_ay + a_az * a_az);
		if (!(len > 0.0f) || !std::isfinite(len)) {
			return out;
		}

		if (!(a_halfLength > 0.0f) || !std::isfinite(a_halfLength) ||
			!std::isfinite(a_cx) || !std::isfinite(a_cy) || !std::isfinite(a_cz)) {
			return out;
		}

		const float px = a_ax / len;
		const float py = a_ay / len;
		const float pz = a_az / len;
		const float half = a_halfLength;

		const int steps = a_steps < 2 ? 2 : (a_steps > 256 ? 256 : a_steps);
		const float span = 2.0f / static_cast<float>(steps);

		// t is in units of the half length, so |t| <= 1 is the object and the
		// span of the marks is independent of how long the object is.
		const auto at = [&](float a_t, float& a_x, float& a_y, float& a_z) {
			a_x = a_cx + px * a_t * half;
			a_y = a_cy + py * a_t * half;
			a_z = a_cz + pz * a_t * half;
		};

		// Signed height above the land: negative means buried.  False means
		// the land could not be read here, which is not the same as zero and
		// must not be counted as ground level.
		//
		// The height is taken from the object's **lowest surface**, not from
		// the centre line the walk is handed, and `a_lowOffset` is how far
		// that surface hangs below the line.  Getting this wrong is what made
		// a carried weapon's mark far shorter than its visible contact.
		//
		// The gate and the walk have to use the same surface.  The gate asks
		// whether the object's own lowest point is under the snow - the log
		// prints that as `lowest corner above by 2.12` - while this walk used
		// to ask whether the centre line was.  On a bow at a slant the two
		// differ by the whole drop: the log has `drop 9.14` against a mark
		// `span 6.6`, so the gate was letting the weapon through while the
		// walk measured only the 6.6 units of *axis* that were under the
		// snow, and the shell that was visibly dragging through far more of
		// it left no mark.  Iron law M: the quantity being measured and the
		// quantity being judged have to share a reference surface.
		//
		// A caller that passes zero - every caller except the carried weapon -
		// gets exactly the centre-line walk it had before.
		const auto height = [&](float a_t, float& a_f) {
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			float land = 0.0f;
			at(a_t, x, y, z);
			if (!a_land(x, y, land) || !std::isfinite(land) || !std::isfinite(z)) {
				return false;
			}
			a_f = (z - a_lowOffset) - land;
			return true;
		};

		const auto param = [&](int a_i) {
			return -1.0f + span * static_cast<float>(a_i);
		};

		int   firstBuried = -1;
		int   lastBuried = -1;
		float deepest = 0.0f;
		float bestGap = 0.0f;
		bool  haveGap = false;
		float nearT = 0.0f;

		for (int i = 0; i <= steps; ++i) {
			const float t = param(i);
			float       f = 0.0f;
			++out.sampled;

			if (!height(t, f)) {
				continue;
			}
			++out.known;

			if (f < 0.0f) {
				if (firstBuried < 0) {
					firstBuried = i;
				}
				lastBuried = i;
				if (-f > deepest) {
					deepest = -f;
				}
			} else if (!haveGap || f < bestGap) {
				bestGap = f;
				haveGap = true;
				nearT = t;
			}
		}

		if (out.known == 0) {
			return out;
		}

		out.depth = deepest;
		out.gap = haveGap ? bestGap : 0.0f;

		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			at(nearT, x, y, z);
			out.nearX = x;
			out.nearY = y;
			out.nearZ = z;
		}

		// There used to be an early return here for "nothing was buried",
		// conditioned - in the version just before this one - on the offset
		// being zero, because a non-zero offset meant a second search was
		// still to come further down.  That search is gone and `height`
		// subtracts the offset itself, so the condition on the offset became
		// meaningless and, worse, left a path where an object that never
		// touched the snow reported the offset as its depth.
		//
		// The return is deleted rather than corrected, because "nothing was
		// buried" is answered in exactly one place now: `anyBuried` below
		// suppresses the stretch, and it has to do so whether or not an
		// offset was passed.  Two mechanisms for one condition is how the
		// offset-conditioned guard outlived the change that made it wrong.

		bool refinedLow = false;
		bool refinedHigh = false;

		// The two ends of the buried stretch are bracketed to within one step
		// and then bisected.  `outside` is the parameter known to be above the
		// land and `inside` the one known to be under it, so the answer walks
		// in from the buried side and the ends stay on the buried side of the
		// surface - which keeps t0 <= t1 and stops a level contact reporting a
		// negative length.
		const auto refine = [&](float a_outside, float a_inside, float& a_out) {
			float outside = a_outside;
			float inside = a_inside;
			for (int k = 0; k < 24; ++k) {
				const float mid = 0.5f * (outside + inside);
				float       f = 0.0f;
				if (!height(mid, f)) {
					break;
				}
				if (f < 0.0f) {
					inside = mid;
				} else {
					outside = mid;
				}
			}
			a_out = inside;
		};

		// The ends of the buried stretch, in units of the half length.
		//
		// These start at the object's own ends rather than at zero.  A
		// crossing is only searched for where the walk found a sample on each
		// side of the surface, so an end that is buried all the way out - a
		// rod driven in past its lower end, which is the ordinary case - has
		// no crossing to find and the answer is simply that end of the object.
		// Leaving these at zero would silently shrink such a stretch to the
		// middle of the object, and the mark with it.
		float t0 = -1.0f;
		float t1 = 1.0f;

		// Whether anything was actually found.  Kept separately from the two
		// numbers above, which are now always meaningful: with both of them
		// starting at the object's ends, "no stretch was found" and "the
		// stretch is the whole object" would otherwise look identical - and
		// the second of those must not be reported for an object that is
		// simply nowhere near the ground.  (Iron law J: a sentinel that would
		// pass the test it exists to answer is not a sentinel.)
		//
		// This is now the only thing that turns "found nothing" into "no
		// contact": the two ends above start at the object's own ends, so
		// without it an object that never came near the ground would report
		// its whole length as a stretch.  Both bisection calls further down
		// are already conditioned on a buried sample existing, so a run with
		// none of them reaches the suppression below untouched.
		const bool anyBuried = firstBuried >= 0;

		if (firstBuried > 0) {
			refine(param(firstBuried - 1), param(firstBuried), t0);
			refinedLow = true;
		}
		if (lastBuried < steps && lastBuried >= 0) {
			refine(param(lastBuried + 1), param(lastBuried), t1);
			refinedHigh = true;
		}

		// There used to be a second search here, run only when the centre line
		// found nothing and re-run with `a_lowOffset` subtracted from every
		// sample.  It is gone because `height` now subtracts that offset
		// itself, which is the only way the walk and the gate can be asking
		// about the same surface - the second search would subtract it twice
		// and find nothing.
		//
		// Its lesson is kept, because it cost three attempts.  On level ground
		// a real contact and a ground that is simply lower than the walk was
		// told are indistinguishable from the walk's own samples: every sample
		// moves by the same amount, so both produce the same count, the same
		// two crossings and the same length.  Three guards were written to
		// separate them and all three threw away the contact they existed to
		// find.  Do not try again here - a caller worried about its land lookup
		// already has a gate to tune.

		if (t1 < t0) {
			t1 = t0;
		}

		// With no buried sample anywhere, the two ends above are still the
		// object's own ends - they have to be, for the case where the lower
		// end is buried - so the length has to be suppressed here rather than
		// by the numbers themselves.  Without this an object that never came
		// near the ground would report its whole length as a contact.
		if (!anyBuried) {
			t0 = 0.0f;
			t1 = 0.0f;
		}

		out.t0 = t0;
		out.t1 = t1;
		at(t0, out.x0, out.y0, out.z0);
		at(t1, out.x1, out.y1, out.z1);
		at(0.5f * (t0 + t1), out.midX, out.midY, out.midZ);

		out.length = (t1 - t0) * half;

		// `out.depth` was set from `deepest` before the early return that no
		// longer exists, and it needs no correction: `height` already measures
		// from the object's lowest surface, so `deepest` is how far that
		// surface went under, which is the number the caller's gate compares
		// and the number the log prints.
		//
		// There used to be a correction here for the route that found a
		// stretch by lowering the line - `out.depth = deepest + a_lowOffset`,
		// to turn "how far the lowered line went under" back into "how far the
		// object went under".  The offset is applied inside `height` now, so
		// that addition would double it.  It also gave a weapon that never
		// touched the snow a depth of exactly the offset, since `deepest` is
		// zero in that case - a number that reads as a buried weapon on a line
		// whose own `lowest` column says it is clear.


		// The residual is a check on a bisected crossing: how far off the
		// surface the refined end actually sits.  It is measured with the same
		// `height` the walk and the gate use, with nothing subtracted - a
		// residual taken against a different surface than the one the walk
		// crossed would report the drop itself as a miss and send a reader
		// looking for a fault that is not there.
		float worst = 0.0f;
		bool  haveResid = false;
		if (refinedLow) {
			float f = 0.0f;
			if (height(out.t0, f)) {
				worst = std::abs(f);
				haveResid = true;
			}
		}
		if (refinedHigh) {
			float f = 0.0f;
			if (height(out.t1, f)) {
				if (!haveResid || std::abs(f) > worst) {
					worst = std::abs(f);
				}
				haveResid = true;
			}
		}

		// There used to be a block here that reported a residual for the ends
		// of a stretch found by lowering the line, because those ends are
		// points the walk put under the surface rather than bisected
		// crossings.  Every stretch is found the same way now - there is one
		// walk and one surface - so the field keeps its narrower meaning: a
		// check on a bisected crossing, and the "not checked" sentinel when
		// there was no crossing to bisect.

		if (haveResid) {
			out.resid = worst;
		}

		// A contact with no length is a touch, not a trail, and is reported as
		// no contact so the caller does not draw a zero-length mark.
		out.live = out.length > 0.0f;
		return out;
	}

	// There used to be a SurfaceGap here, which turned the centre line's
	// clearance into the hull's by subtracting a half thickness.  It is gone
	// because the walk no longer reads the centre line: AxisSpan takes its
	// heights from the object's own lowest surface, so its `gap` already is
	// the hull's clearance and a converter between the two surfaces has
	// nothing left to convert.
	//
	// Its discipline is worth keeping even though the function is not.  A
	// gate comparing the line's gap to its own would call a stout weapon
	// floating while its hull was already down - the same class of mistake as
	// the width constant, in the other direction, throwing away a measurement
	// that had just been made.  And a gap that cannot be measured is not a
	// contact: `AxisSpan` leaves `gap` at zero when no sample could be read,
	// which is why every caller gates on `known > 0` first.

	// How far above the snow a carried object may be and still count as
	// touching it.
	//
	// This is the ceiling the carried-weapon gate compares against, and it is
	// a function of the object's own thickness rather than a constant, for a
	// reason that the recorded run makes plain.
	//
	// The number it replaces described the wrong surface.  The gate asked
	// `GetLandHeight` - the bare terrain - while the snow is a layer stacked
	// on top of it: `SnowRaiseHeight` is 35 units on this machine, and
	// `SnowSurface::LiftAt` is what the rest of the plugin already adds to
	// the land to get the surface an object actually rests on
	// (`ObjectStamps.cpp`: `landZ + SnowSurface::LiftAt(...)`).  So a weapon
	// pressed into the snow but not yet through to the terrain read as
	// "carried clear", and the log shows the shape of it: 57 refusals whose
	// lowest corner sat 4.21 to 65.93 above the land, against 14 marks placed
	// with a lowest corner of -11.91 to +0.40.  There is an empty band
	// between +0.40 and +4.21, and the threshold sat inside it - which is why
	// the trail came and went in blocks instead of tapering.
	//
	// Raising the reference to the snow removes that band.  What it must not
	// do is let a weapon that is simply *held* mark, because a raised blanket
	// can be tens of units thick and everything below it would then qualify.
	// The bound is therefore the object's own radius: a rod 4.9 units thick
	// whose lowest point is within 4.9 of the snow has its side in the snow,
	// and one whose lowest point is above that does not, however deep the
	// blanket is.  It scales with the object rather than with the weather,
	// which is the property that makes it safe to apply to every weapon.
	//
	// `a_thickness` is a half thickness - the same quantity `AxisSpan`'s
	// `lowOffset` is built from and `LineWidth` clamps - so the caller passes
	// `shapeExtent.thickness` or the mesh's own radial half width directly.
	// A non-finite or negative reading falls to `a_min`, which keeps a bad
	// hull measurement from widening the gate rather than closing it.
	inline float ContactReach(float a_thickness, float a_min, float a_max)
	{
		if (!std::isfinite(a_thickness) || !std::isfinite(a_min) || !std::isfinite(a_max)) {
			return (std::isfinite(a_min) && a_min > 0.0f) ? a_min : 0.0f;
		}

		if (!(a_thickness > a_min)) {
			return a_min > 0.0f ? a_min : 0.0f;
		}
		return a_thickness < a_max ? a_thickness : a_max;
	}

	// The surface a carried object is measured against: the bare land plus
	// the snow standing on it.
	//
	// This is the walk's own input, and it is a named function rather than a
	// `land + SnowSurface::LiftAt(...)` written inline at the call site,
	// because `Clipmap.cpp` is not linked into the offline test: an expression
	// living there cannot be asserted on at all.  Flipping it back to the bare
	// land leaves every test green and every byte pin matching, and half a fix
	// that no rule can see is the failure this whole file exists to avoid, so
	// the arithmetic lives here and the caller only names it.
	//
	// `a_lift` is whatever the caller measured standing on that land; a
	// non-finite one is treated as no blanket, which is the bare-land
	// behaviour and therefore a real fallback rather than a convenient one.
	inline float SurfaceAt(float a_land, float a_lift)
	{
		const float lift = (std::isfinite(a_lift) && a_lift > 0.0f) ? a_lift : 0.0f;
		return a_land + lift;
	}

	// The signed height of a point above the *snow*, given the bare land
	// height under it and how far the snow blanket stands above that land.
	//
	// This exists to stop the same mistake being made twice.  Two arms of the
	// carried-weapon path ask the same question - is this part of the object
	// down? - and each has to name the surface it means.  Writing
	// `z - land` in one and `z - (land + lift)` in the other is exactly how
	// the gate and the axis walk drifted apart before, so the subtraction
	// lives here and both callers take it from one place.
	//
	// It is built on SurfaceAt, so the definition of "the surface" is in one
	// place rather than two that have to be kept equal by hand.
	//
	// `a_lift` is expected to be zero or positive - it is the thickness of a
	// blanket, not a signed offset - and a non-finite one is treated as no
	// blanket rather than as an error, because falling back to the bare land
	// is the behaviour this replaces rather than a new failure mode.
	inline float HeightAboveSnow(float a_z, float a_land, float a_lift)
	{
		return a_z - SurfaceAt(a_land, a_lift);
	}


	// The drawn half width of a weapon's mark, from the weapon's own measured
	// thickness.
	//
	// This was a single constant for every weapon, which is why a bow, a rod
	// and a shield boss all left the same width of trail: the extent walk had
	// already measured them apart and the number it produced was thrown away.
	//
	// The clamp is a guard rather than a tuning range.  A collision hull is
	// sometimes deliberately oversized for gameplay, and a bad reading must not
	// widen the trail; the floor is the constant this replaces, so an object
	// thinner than that keeps the old look instead of vanishing.  A
	// non-finite reading falls to the floor for the same reason.
	inline float LineWidth(float a_thickness, float a_scale, float a_min, float a_max)
	{
		if (!std::isfinite(a_thickness) || !std::isfinite(a_scale) ||
			!std::isfinite(a_min) || !std::isfinite(a_max)) {
			return a_min > 0.0f ? a_min : 0.0f;
		}

		const float scaled = a_thickness * a_scale;
		if (!(scaled > a_min)) {
			return a_min;
		}
		return scaled < a_max ? scaled : a_max;
	}

	// The ceiling DrawLength should be given, which depends on whether the
	// length came from a measurement or from the object's own hull.
	//
	// This is a separate function rather than two lines inside the caller
	// because the distinction is the change that matters and the caller is not
	// covered by the offline tests: a copy of the rule that is asserted in the
	// test and applied differently in the real path is the exact failure this
	// codebase has already been bitten by once.  Here the rule itself is
	// assertable.
	//
	// A measured stretch may draw up to the object's own length - the constant
	// was one value for every weapon and every pose, so it must not cap a long
	// object at a short one's stub.  The hull route has no measurement behind
	// it, so the constant stays what it always was there: the plank guard.
	inline float DrawCeiling(bool a_measured, float a_contactCeiling,
		float a_objectLength)
	{
		const bool haveConstant =
			std::isfinite(a_contactCeiling) && a_contactCeiling > 0.0f;

		if (a_measured) {
			if (!(std::isfinite(a_objectLength) && a_objectLength > 0.0f)) {
				return haveConstant ? a_contactCeiling : 0.0f;
			}
			return haveConstant ?
				std::max(a_contactCeiling, a_objectLength) :
				a_objectLength;
		}

		return haveConstant ? a_contactCeiling : a_objectLength;
	}

	// Convert the buried stretch the walk reports into the half length the
	// mark's own field wants.
	//
	// The two are different units and the difference is a factor of two, so
	// the conversion is named rather than written inline.
	//
	// Span::length is a length *along* the object: the walk lays parameters on
	// the axis as p(t) = centre + t * halfLength * axis with t from -1 to +1,
	// so the axis segment spans 2 * halfLength and the stretch it reports is
	// (t1 - t0) * halfLength - a full length, whose maximum is the object's
	// whole self.  Its own comment says "world length of the buried stretch".
	//
	// stamp.radius is a *half* length.  ClipmapUpdateCS.h StampDistance reads
	// it as `halfLength = max(s.z, 1e-3)` and normalises the along-axis
	// distance by it, so an ellipse with radius r reaches r either side of its
	// centre: 2r of mark for r of field.
	//
	// Handing the first to the second therefore draws twice the stretch.  The
	// log shows the consequence once the scale below is applied on top: every
	// one of a session's twenty-four stamps read `half length 40.00`, because
	// twice a 46-to-72 unit stretch, widened by 2.2, is past ShaftLineMaxLength
	// every single frame - so the mark was one fixed 80-unit bar and no pose,
	// no weapon and no contact depth could change it.  That is the report this
	// answers: the furrow stopped being a measurement of anything.
	inline float HalfLengthFromStretch(float a_stretch)
	{
		if (!std::isfinite(a_stretch) || !(a_stretch > 0.0f)) {
			return 0.0f;
		}
		return 0.5f * a_stretch;
	}

	// How long to draw the mark for a weapon, from the stretch of it that was
	// measured under the surface.
	//
	// a_measured is a *half* length, so the caller converts a walk result with
	// HalfLengthFromStretch above; the hull route's own estimate is already
	// one, which is why the two can share this call.
	//
	// The measurement is a lower bound on the footprint rather than the
	// footprint itself: the walk counts only the samples that ended up below
	// the land height, so an object resting *on* the surface contributes
	// nothing, and samples are discrete, so the two either side of a crossing
	// are up to half a step apart either way.  The scale widens the stretch
	// to cover the snow the object disturbed without sinking into it.
	//
	// It is a scale rather than a fixed addition because the gap it covers is
	// proportional to the stretch: an object barely dipped in touches over a
	// short arc and one laid down touches over a long one, and the snow
	// disturbed either side scales with each.
	//
	// The ceilings are sanity limits and not tuning ranges.  ShaftLineMaxLength
	// stops an unusual reading laying a furrow across the clipmap, and
	// a_measuredCeiling is whatever the caller trusts most for the route it
	// took: for a measured stretch that is the larger of the constant and the
	// object's own length, because the constant was one value for every weapon
	// and pose and must no longer cap a long bow at a rod's stub; for the hull
	// route, where there is no measurement at all, it is the constant itself,
	// which is the plank guard the constant always was.  The caller resolves
	// that and passes one number, so this function has a single ceiling to
	// apply rather than a rule about which route is which.
	//
	// A result at or below a_min_length is not worth drawing as a line: it
	// would read as a dot with a direction.  Zero is returned for that, and
	// the caller keeps the disc.
	inline float DrawLength(float a_measured, float a_scale,
		float a_measuredCeiling, float a_maxLength, float a_minLength)
	{
		if (!std::isfinite(a_measured) || !(a_measured > 0.0f)) {
			return 0.0f;
		}

		float drawn = a_measured;
		if (std::isfinite(a_scale) && a_scale > 0.0f) {
			drawn = a_measured * a_scale;
		}

		if (std::isfinite(a_maxLength) && a_maxLength > 0.0f &&
			drawn > a_maxLength) {
			drawn = a_maxLength;
		}

		if (std::isfinite(a_measuredCeiling) && a_measuredCeiling > 0.0f &&
			drawn > a_measuredCeiling) {
			drawn = a_measuredCeiling;
		}

		if (!(drawn > a_minLength)) {
			return 0.0f;
		}
		return drawn;
	}

	// A footprint's own half width, from the three numbers the foot branch
	// multiplies together.
	//
	// This is its own function and not LineWidth(..., 0.0f, 0.0f) because
	// LineWidth *clamps*: its last line is `scaled < a_max ? scaled : a_max`,
	// so an a_max of zero does not mean "no ceiling", it means "return zero".
	// The first version of this call passed zero exactly as if it meant "no
	// ceiling" and got 0.0f back, which silently zeroed the footprint width
	// the alignment is built on - the aligned width fell back to the thickness
	// route's six-unit ceiling and every mark came out at the same 24 x 6
	// shape, whatever the weapon was doing.
	//
	// Nothing in the offline test could see it, because the tests called
	// AlignedWidthCeiling with the footprint width computed by hand while the
	// caller passed zero.  A rule is only assertable if the assertion can walk
	// the same arguments the caller does, so the caller's arithmetic lives
	// here, where it can.
	inline float FootprintHalfWidth(float a_footRadius, float a_footLength,
		float a_footAspect)
	{
		if (!std::isfinite(a_footRadius) || !std::isfinite(a_footLength) ||
			!std::isfinite(a_footAspect)) {
			return 0.0f;
		}

		const float halfLength = a_footRadius * a_footLength;
		const float halfWidth = halfLength * a_footAspect;
		return halfWidth > 0.0f ? halfWidth : 0.0f;
	}

	// A mark's half width, taken from its own half length exactly the way a
	// footprint's is.
	//
	// The rule this replaces kept a footprint's half width as a *floor*, so a
	// carried weapon's mark could never be narrower than a footprint.  That is
	// what stops long thin objects reading as a rake of parallel teeth, but it
	// also makes the aspect the wrong knob: a footprint's half width is 12.15
	// and a long weapon's mark is drawn at 24, so the aspect only decides
	// anything above 12.15 / 24 = 0.506 - every value below it is overridden,
	// and the one setting a user would reach for to make the mark narrower
	// does nothing at all.
	//
	// So the two jobs are split.  This function gives the width from the
	// length with the ordinary minimum and the resolved ceiling and no
	// footprint floor at all, which is what makes the aspect a real knob and
	// what lets a mark be tuned to something between a sliver and a full
	// footprint.  The footprint floor is applied by the caller, and only when
	// the caller decides the object is long enough to need it.
	//
	// `a_min` is the ordinary minimum width and is still honoured, because a
	// mark genuinely narrower than that reads as a cut with no snow on its
	// sides - the failure this codebase has already been bitten by.
	inline float LengthDerivedWidth(float a_halfLength, float a_aspect,
		float a_min, float a_max)
	{
		if (!std::isfinite(a_halfLength) || !(a_halfLength > 0.0f)) {
			return std::isfinite(a_min) && a_min > 0.0f ? a_min : 0.0f;
		}
		if (!std::isfinite(a_aspect) || !(a_aspect > 0.0f)) {
			return std::isfinite(a_min) && a_min > 0.0f ? a_min : 0.0f;
		}

		float width = a_halfLength * a_aspect;
		if (std::isfinite(a_min) && a_min > 0.0f && width < a_min) {
			width = a_min;
		}
		if (std::isfinite(a_max) && a_max > 0.0f && width > a_max) {
			width = a_max;
		}
		return width;
	}

	// The ceiling an aligned mark's half width should be given.
	//
	// The old ShaftLineMaxWidth is the plank guard for a thickness-derived
	// width.  It is the wrong number once the width comes from the footprint,
	// because it sits below the footprint's own half width - so the caller
	// raises the guard to clear the footprint and nothing more.  An object
	// that is genuinely wide is still bounded, and an object that is thin
	// gets the footprint's width rather than a sliver.
	inline float AlignedWidthCeiling(float a_ordinaryCeiling, float a_footWidth)
	{
		const bool haveOrdinary =
			std::isfinite(a_ordinaryCeiling) && a_ordinaryCeiling > 0.0f;
		const bool haveFoot = std::isfinite(a_footWidth) && a_footWidth > 0.0f;

		if (!haveOrdinary) {
			return haveFoot ? a_footWidth : 0.0f;
		}
		if (!haveFoot) {
			return a_ordinaryCeiling;
		}
		return a_footWidth > a_ordinaryCeiling ? a_footWidth : a_ordinaryCeiling;
	}

	// The longest a carried weapon's mark may be drawn, given the width it is
	// drawn at.
	//
	// This is the other half of "make it look like a footprint".  A footprint
	// is a 2:1 oval, so its half length is twice its half width; the weapon's
	// own half length is set by the measured stretch and is not bounded by the
	// width at all, which is how an 80-unit mark on a 4-unit-wide mark came
	// about.  A mark that long and that thin, re-stamped every frame while the
	// character walks, stacks its own side walls into the rake of parallel
	// teeth the screenshot shows: each frame's phase shifts the long thin
	// ellipse a little and cuts a new groove beside the last.
	//
	// Capping the length at a multiple of the width is what turns that rake
	// back into a row of foot-shaped ovals that overlap into one furrow -
	// which is exactly the look being asked for, and it is also what stops the
	// aspect ratio from squashing the raised band on the mark's sides.
	//
	// `a_maxAspect` is deliberately larger than the footprint's own 2.0 so a
	// long object still leaves a long mark: the cap removes the pathological
	// ratios, it does not force every weapon to be exactly a foot.
	inline float AspectCappedLength(float a_halfLength, float a_halfWidth,
		float a_maxAspect)
	{
		if (!std::isfinite(a_halfLength) || !(a_halfLength > 0.0f)) {
			return 0.0f;
		}
		if (!std::isfinite(a_halfWidth) || !(a_halfWidth > 0.0f) ||
			!std::isfinite(a_maxAspect) || !(a_maxAspect > 0.0f)) {
			return a_halfLength;
		}

		const float ceiling = a_halfWidth * a_maxAspect;
		return a_halfLength < ceiling ? a_halfLength : ceiling;
	}
}
