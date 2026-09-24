// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ImpactPatterns
{
	struct Stroke
	{
		float x{}, y{}, motionX{}, motionY{}, radius{}, strength{};
		// Heap height, as a fraction of the pit the same stroke would dig at
		// strength 1.  A blast needs this to be a separate number from
		// strength, because its ring is the snow the bowl threw out and that
		// snow must not dig a hole of its own to exist.
		//
		// MagicImpacts combines the two into one signed depth - pit from
		// strength, heap from this - and the shader reads depth as a signed
		// quantity (-s.w * falloff, ClipmapUpdateCS.h:348), so a stroke that
		// states strength 0 and a positive rim raises a disc of snow with no
		// pit under it at all.
		//
		// The split exists because the shader's other heap path, stamp.rim
		// (:359), cannot place a mound: it is gated on the rim lean and its
		// span collapses to a thin ring at the stamp's own edge when the lean
		// is high, which is right for the snow around a footprint and wrong
		// for a bank thrown out by a blast.
		//
		// Negative means "not stated": the directional spells and plain magic
		// hits leave it alone and keep exactly the depth-derived lip they
		// have always had.
		float rim{ -1.0f };
	};
	struct Pattern
	{
		std::array<Stroke, 9> strokes{};
		size_t count{};
	};

	inline float BoundedRadius(float radius, float scale, float limit)
	{
		if (!std::isfinite(radius) || !std::isfinite(scale) || !std::isfinite(limit)) { return 0.0f; }
		return std::clamp(radius * scale, 0.0f, std::max(limit, 0.0f));
	}

	// A crater is thrown, not machined, so its bank cannot be a regular ring.
	//
	// Everything here is built from a caller-supplied seed rather than from a
	// global generator, for two reasons.  The pattern functions stay pure, so
	// the offline test drives them exactly as the game does and a failure
	// reproduces.  And the seed comes from the impact's world position, which
	// is the property a thrown bank actually needs: two blasts at the same
	// spot leave the same crater - the marks do not shimmer if something
	// re-runs the build - while two blasts anywhere else differ.  A counter or
	// a clock would give the opposite, and a bank that re-rolls under a
	// stationary projectile's repeated rebuild would crawl.
	//
	// The mixing is the standard 32-bit integer avalanche: multiply by an odd
	// constant to spread the low bits upward, xor-fold the high half back down,
	// multiply again.  It is not cryptographic and does not need to be - it
	// needs to turn neighbouring positions, which differ in one coordinate by
	// one unit, into unrelated-looking streams, and the xor-fold of the high
	// half is what does that.
	inline uint32_t Hash(uint32_t v)
	{
		v ^= v >> 16;
		v *= 0x7feb352du;
		v ^= v >> 15;
		v *= 0x846ca68bu;
		v ^= v >> 16;
		return v;
	}

	// One independent stream per (seed, lane).  Lane is mixed in first, so a
	// caller that wants four numbers for one item asks for four lanes and gets
	// four uncorrelated draws - asking four times sequentially would tie the
	// jitters together, because a linear counter only walks the avalanche.
	//
	// Lane goes through the avalanche before it is added, and that is not
	// decoration.  Adding a linearly scaled lane - seed + lane * golden - puts
	// a run of lanes that share a low-bit pattern into the hash as a run of
	// related inputs, and one avalanche pass does not fully decorrelate them.
	// With the ring asking for lanes 3, 7, 11, 15 (four draws apart, as the
	// per-heap loop does when it stores its four properties in a row) the
	// first four heights came out 0.0513, 0.0616, 0.0730, 0.0800 - a marching
	// sequence, not noise.  Hashing the lane first breaks the arithmetic
	// progression at the input, so the output has nothing to correlate.
	inline float Random01(uint32_t seed, uint32_t lane)
	{
		return static_cast<float>(Hash(seed ^ Hash(lane)) >> 8) * (1.0f / 16777216.0f);
	}

	// Symmetric in [-1, 1], which is what every jitter below wants: a
	// one-sided offset would move the bank's mean and the ring would grow a
	// bias the more it is wobbled.
	inline float RandomSigned(uint32_t seed, uint32_t lane)
	{
		return Random01(seed, lane) * 2.0f - 1.0f;
	}

	inline float StampRadius(float outerRadius, float surfaceScale, float span, float lean)
	{
		const float support = 1.0f + std::max(span, 0.0f) * (1.0f + std::clamp(lean, 0.0f, 1.0f));
		return outerRadius * std::clamp(surfaceScale, 0.0f, 1.0f) / support;
	}

	// The same conversion without the rim-span deduction, for stamps that pin
	// their own rim to zero.
	//
	// StampRadius exists to keep a footprint's *total* spread where the
	// pattern asked: the shader grows a stamp outward past s.z by
	// 1 + rim.x * (1 + saturate(rim.z)) when the rim channel fires
	// (ClipmapUpdateCS.h:350-359), so s.z is shrunk by the same factor up
	// front.  A blast pins rim.x to 0 - its ring is raised snow carried on
	// the signed depth instead (see the note in Explosion) - so nothing
	// grows and the shrink is a pure loss.  On snow the shrink is 2.725x,
	// which turned a crater meant to be 126 units across into one 46 units
	// across with its heaps stranded far outside it.
	inline float PlainStampRadius(float outerRadius, float surfaceScale)
	{
		return outerRadius * std::clamp(surfaceScale, 0.0f, 1.0f);
	}

	// A published copy of the Explosion ring's tuning, so the numbers can be
	// logged at startup.
	//
	// The constants themselves live inside Explosion, in the anonymous-looking
	// block further down, and that is where they belong: they are tuning for
	// one shape, not an interface.  But a header constant is baked into the
	// binary, so when a revision changes only numbers here there is otherwise
	// no way to tell from outside whether the new build is the one running.
	// That question has cost this project a full round-trip already.  This
	// struct is the answer to it: cheap to build, printed once at load, and
	// it cannot silently disagree with the tuning because the test compares
	// the two.
	struct RingConstants
	{
		int heaps{};
		float distance{};
		float size{};
		float fill{};
		float cap{};
		float wobble{};
		float height{};
	};

	inline RingConstants ExplosionRingConstants()
	{
		return RingConstants{
			.heaps = 8,
			.distance = 0.76f,
			.size = 0.16f,
			.fill = 0.45f,
			.cap = 0.5f,
			.wobble = 0.16f,
			.height = 0.55f,
		};
	}

	// seed picks which irregular bank this blast throws.  Callers pass
	// something derived from the impact's world position, so a crater is
	// reproducible in place and different everywhere else; see Hash above.
	// The default is the value a caller with no position to offer gets, and it
	// produces the same one bank every time, which is what the offline test
	// relies on to compare runs.
	inline Pattern Explosion(float reach, uint32_t seed = 0x5eed1eafu)
	{
		Pattern out{};
		if (!(reach > 0.0f) || !std::isfinite(reach)) { return out; }

		// A blast is one hole with snow thrown out of it, so the pattern is
		// exactly two things: the bowl, and the ring of snow the bowl's own
		// displacement piled up outside it.
		//
		// The ring must not dig.  Every earlier version of this file gave it
		// a depth, and a depth is a pit: the five-mark table read as "small
		// pit in the middle, small pits around it", and the 0.30-strength fan
		// that replaced it still carved eight shallow scoops.  A scoop is
		// still a hollow, so the lip was never purely raised - which is what
		// "the snow has to go somewhere" is supposed to look like.
		//
		// So the bowl and the ring carry strength and rim separately, and
		// MagicImpacts folds them into one signed depth (pit positive, heap
		// negative).  The ring is strength 0 with a rim, which the shader
		// treats as raised ground across its whole disc, so the only hollow
		// anywhere in the pattern is the bowl.
		//
		// The bowl is widened past the ring's inner edge so the two cannot
		// overlap.  Bowl 0.58 * reach, ring centred 0.85 * reach jittered by
		// +-0.10 and no wider than 0.075 * reach of its own, so the ring's
		// inner edge lands 2.2 units clear of the bowl lip at the reach this
		// pattern is tested at.  MagicImpacts no longer LOS-tests explosion
		// strokes, so the ring does not need to hug the centre to survive
		// terrain.  Exactly one bowl plus kRim marks must fit in
		// Pattern::strokes.
		out.strokes[out.count++] = { 0, 0, 0, 0, reach * 0.58f, 1.0f, 0.0f };

		// The ring is eight *tangential capsules*, not eight round marks.
		//
		// Eight round marks cannot be made to read as a thrown-up bank, and
		// the arithmetic says so.  For round marks of radius r centred on a
		// circle of radius R: neighbouring centres are 2R*sin(pi/8) apart, so
		// they only touch when r >= 0.3827R; the ring's inner edge is R - r and
		// its outer edge R + r.  Asking for an inner edge at the bowl lip
		// (R - r = 0.58) and an outer edge inside the blast's own extent
		// (R + r <= 1.05) leaves R <= 0.759 and then r = 0.29, an inner edge of
		// 0.469 - eleven units *inside* the bowl.  No assignment of eight
		// discs satisfies all three; the marks either leave bare ground between
		// the bowl and the ring, or sit scattered like a compass rose, or both.
		// That is exactly what shipped: 16-unit marks on a 126-unit crater,
		// 23% of the ring's circumference covered, 12 units of untouched snow
		// between the lip and the nearest mark.
		//
		// SweptDelta (ClipmapUpdateCS.h:182-192) turns any non-zero motion
		// into a capsule, and a *tangential* motion turns a round mark into a
		// segment lying along the ring.  Length is then set by the motion and
		// thickness by the radius, so the two constraints stop competing: the
		// capsules can be long enough to close the circle while staying thin
		// enough that their outer edge never breaches the extent.
		//
		// Each capsule covers its own eighth of the circumference, 2*pi*R/8.
		// With the round caps included, a capsule spans 2*motion + 2*r, so the
		// motion that just closes the ring is (pi*R/4 - r).  The marks then
		// meet end to end and the ring is one continuous bank.
		//
		// Placement is 0.76 * reach so the inner edge lands on the bowl lip,
		// and 0.16 * reach thick so the bank is 35 units across against the
		// bowl's 126 - a fifth of the crater, which is what a thrown-up bank
		// looks like beside the hole it came out of.
		constexpr int kRim = 8;
		constexpr float kRimDistance = 0.76f;
		constexpr float kRimSize = 0.16f;
		// How far each capsule's centre may wander off its nominal radius.
		//
		// This is the term that moves the *silhouette*, not just the bulk, so
		// it is the one that decides whether the bank has an outline or a
		// rim.  It was +-0.07 for one revision, chosen to keep the inner edge
		// from reaching back over the bowl lip, and a checkout against a real
		// crater showed what that cost: at +-0.07 the ring's outer edge came
		// out circular to within a couple of units in every direction, so the
		// hole in the screenshot had a rim drawn with a compass.  Snow does
		// not land in a circle.  +-0.16 moves centres by up to 17 units on a
		// 109-unit reach, which is a third of the bank's own width and enough
		// for the outline to stagger.
		constexpr float kRimWobble = 0.16f;
		// Heap height, as a fraction of the pit the bowl digs at strength 1.
		// The bowl displaces that much snow; these capsules are where it lands.
		constexpr float kRimHeight = 0.55f;
		// How much of its slot each capsule tries to fill, before the run
		// calculation below turns that into a length.
		//
		// At 1.0 the eight capsules meet end to end and the bank is one
		// continuous ridge - which is geometrically correct and visually
		// wrong, because a ridge has a smooth outer edge by construction.
		// A thrown bank is not continuous: it is lumps with snow between
		// them, and the silhouette is made of the lumps.
		//
		// 0.68 was the first attempt at that and it did not work, for a
		// reason worth recording.  Coverage - the sum of the eight capsules'
		// spans divided by the ring's circumference - came out at 92% at the
		// tuned reach, so the lumps still overlapped almost end to end and
		// the outline stayed a smooth circle.  0.68 sounds like "two thirds
		// full" and lands at "nine tenths full" because span is measured
		// between cap extremes, so each capsule reaches well past the run
		// that kRimFill scales; the caps, which are not scaled, make up the
		// difference.  0.45 puts coverage near 70%, which is where the gaps
		// are wide enough to separate the heaps in a screenshot.
		//
		// Coverage is the number to reason with, not fill.  Fill is what the
		// code takes; coverage is what the eye sees, and the two differ by
		// more than intuition allows for.
		constexpr float kRimFill = 0.45f;
		// How much of a capsule's own radius is subtracted from its length.
		//
		// 1.0 is the geometrically obvious value and it is wrong: it cancels
		// thickness out of the capsule's total span, so every heap comes out
		// the same width however its radius was drawn.  See the note at the
		// run calculation for the measurement.  0.5 keeps thickness in the
		// span while still shortening a fat capsule.
		constexpr float kRimCap = 0.5f;
		// How far each heap's own character may wander from the nominal:
		// angle off its share of the circle, radius off the ring, height off
		// kRimHeight, and thickness off kRimSize - each drawn independently.
		//
		// These three are what turn a ring into a bank, and they are separate
		// from kRimWobble above because they are separate complaints.  Wobble
		// moves where each heap sits on the circle; without the others every
		// heap is still the same size and the same height, so the bank is a
		// ring of identical beads - regular enough that the eye reads the
		// circle through it no matter how the centres are placed.  A thrown
		// bank is uneven in all four at once: some clods are tall, some are
		// low, some are broad and slumped, some are narrow, and the gaps
		// between them are not equal.
		//
		// Thickness now swings as wide as height does.  It used to be held at
		// +-0.12 on the argument that varying it made an earlier bank read as
		// saw teeth, and that argument was half right: the teeth came from
		// varying thickness *alone*, so the widths were the whole silhouette.
		// Here thickness varies alongside angle, radius and height, and it is
		// the combination the eye reads as clods rather than as teeth.  More
		// to the point, thickness is now a *narrow* term to begin with - 0.16
		// of reach - so +-0.12 of it is +-2 units against a 65-unit-wide
		// bank, 6%: it was never going to show however it was set.  +-0.38 of
		// a bank that has also been made slimmer is what actually varies the
		// lump size.
		constexpr float kRimAngleJitter = 0.22f;
		constexpr float kRimHeightJitter = 0.40f;
		constexpr float kRimSizeJitter = 0.38f;
		for (int i = 0; i < kRim; ++i) {
			// Every property of this heap is drawn from the impact's own seed
			// rather than from a formula of the index.  A formula of the index
			// is what shipped before: sin(i * 2.399) walks the radius by a
			// fixed amount, so every blast in the game left the identical
			// eight-lobed collar, and a player who throws two fireballs sees
			// the same crater twice.  Worse, the eye is very good at spotting a
			// repeated silhouette, so what should read as thrown snow read as a
			// decal.
			//
			// The lane number is mixed with the seed inside RandomSigned, so
			// the four draws below are independent.  Drawing four values in a
			// row from one stream would correlate them - a linear counter only
			// walks the avalanche - and correlated jitters here would mean the
			// tall heaps are also the far ones, which is a shape in itself.
			const uint32_t lane = static_cast<uint32_t>(i) * 4u;
			const float angle = static_cast<float>(i) * 0.78539816f +
				kRimAngleJitter * (0.78539816f) * RandomSigned(seed, lane + 0u);
			const float jitter = 1.0f + kRimWobble * RandomSigned(seed, lane + 1u);
			const float distance = reach * kRimDistance * jitter;
			const float x = std::cos(angle), y = std::sin(angle);
			// Motion lies along the tangent, which is the radius turned a
			// quarter turn: (-y, x).  A tangent, not (-x, y) - that one is a
			// reflection, perpendicular only where |x| = |y|, so four of the
			// eight capsules would have pointed straight at or away from the
			// centre and stabbed through the bowl or out past the extent.
			//
			// Thickness now varies as widely as height does - see
			// kRimSizeJitter's note.  The earlier objection was that varying
			// width alone turns the bank into saw teeth, and that holds; what
			// changed is that width is no longer alone, and that at 0.16 of
			// reach it was too small a term for any setting of its jitter to
			// matter.
			//
			// Length then has to follow thickness, or the capsules overlap
			// and pile up.  A capsule spans 2*run + 2*radius - its two round
			// caps included - and each owns an eighth of the circle, an arc
			// of pi*R/4, so filling that arc would mean run = pi*R/8 - radius.
			//
			// kRimFill takes a bite out of the arc so the capsules stop short
			// and leave snow between them.  It scales the arc, not run: a
			// capsule that is already short because it is fat stays
			// proportionally short, so the gaps stay even instead of closing
			// up wherever thickness was drawn high.
			//
			// The kRimCap factor on the radius is the part that is easy to get
			// wrong, and it was wrong first.  Subtracting the *whole* radius -
			// kRimCap = 1 - cancels thickness straight out of the length:
			// span = 2*(kRimFill*pi*R/8 - r) + 2r = kRimFill*pi*R/4, with no
			// r left in it.  Measured at the shipped jitters, all eight
			// capsules came out 30.6 degrees wide regardless of the +-38%
			// thickness draw, so the widest and narrowest heaps were the same
			// size on the ground - the exact uniformity this shape exists to
			// break, arrived at through arithmetic that looked reasonable.
			// Halving the subtraction leaves thickness in the span: the arc
			// term sets the slot and the radius term sets what the slot is
			// filled with, and span now runs 38 to 47 degrees across the
			// jitter's range.
			//
			// The caps are still not scaled by kRimFill - only the straight
			// run is - because radius is also the thickness the shader builds
			// from, and shrinking it to widen the gap would trade the outline
			// back for the beads.  The visible gap is (1 - kRimFill) of the
			// arc plus what kRimCap's complement adds back, around 14 degrees
			// at the reach this pattern is tuned for.
			const float radius = reach * kRimSize *
				(1.0f + kRimSizeJitter * RandomSigned(seed, lane + 2u));
			const float run = std::max(
				kRimFill * (3.14159265f * distance * 0.125f) - kRimCap * radius, 0.0f);
			const float height = kRimHeight *
				(1.0f + kRimHeightJitter * RandomSigned(seed, lane + 3u));
			out.strokes[out.count++] = { x * distance, y * distance,
				-y * run, x * run,
				radius, 0.0f, height };
		}
		return out;
	}

	inline Pattern Directional(float reach, float width, float forwardX, float forwardY, bool shout)
	{
		Pattern out{};
		if (!(reach > 0.0f) || !(width > 0.0f) || !std::isfinite(reach) || !std::isfinite(width)) { return out; }
		const float length = std::hypot(forwardX, forwardY);
		if (!(length > 0.001f) || !std::isfinite(length)) { return out; }
		const float fx = forwardX / length, fy = forwardY / length;
		const int lanes = shout ? 3 : 1;
		for (int lane = 0; lane < lanes; ++lane) {
			const float side = shout ? static_cast<float>(lane - 1) : 0.0f;
			for (int step = 0; step < 3; ++step) {
				const float t0 = step / 3.0f, t1 = (step + 1) / 3.0f;
				const float lateral0 = side * width * t0 * 0.52f;
				const float lateral1 = side * width * t1 * 0.52f;
				const float x0 = fx * reach * t0 - fy * lateral0;
				const float y0 = fy * reach * t0 + fx * lateral0;
				const float x1 = fx * reach * t1 - fy * lateral1;
				const float y1 = fy * reach * t1 + fx * lateral1;
				out.strokes[out.count++] = { x1, y1, x1 - x0, y1 - y0,
					width * (shout ? (0.14f + 0.13f * t1) : 0.5f),
					(1.0f - 0.5f * t1) * (side == 0 ? 1.0f : 0.65f) };
			}
		}
		return out;
	}
}
