// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace ActorShapes
{

	// The engine's support readings do **not** carry the shape's base
	// radius, and this was measured rather than assumed.
	//
	// `hkpConvexShape::getMaximumProjection` is documented in Havok as the
	// core's projection plus `m_radius`, which made "every reading carries
	// the radius, so subtract it" a plausible correction - and it is wrong.
	// Two objects in one session's log settle it:
	//
	//   fur helmet  raw +X=-X=0.10  +Y=-Y=0.14  +Z=-Z=0.06
	//               -> hx 7.00, hy 9.80, hz 4.20  (world scale 70)
	//   steel arrow raw +X=-X=0.03  +Y=-Y=0.41  +Z=-Z=0.00
	//               -> hx 2.10, hy 28.70, hz 0.00
	//
	// A radius added to all six readings would be a single quantity present
	// in every one of them.  The arrow's `+X` reading is its `+Y` reading
	// divided by 13.7, and its `+Z` is zero - three different numbers, and
	// the two derived half extents reproduce the logged `length = 28.7` and
	// `radius = 28.8` exactly.  The helmet's reproduce `length = 9.74`,
	// `thickness = 4.11` and `radius = 12.62` exactly.  So the readings are
	// the core's own projections and nothing is added to them.
	//
	// The consequence is that the recovered half extent is already the true
	// one: `0.5 * (proj(+d) + proj(-d)) * invScale`.  A zero on an axis is a
	// degenerate core along that axis, not a radius standing in for one.
	//
	// Recorded here because the correction above is attractive, was written,
	// built and tested once, and would have shortened every derived height by
	// the shape's own radius - a helmet's 4.11 would have become 0.00, which
	// is the whole of what a height rule reads.

	// The three half extents of a shape along its own axes, in game units.
	// A single radius cannot describe an arrow: it is long and thin, and
	// collapsing it to one number is what made every projectile read as a
	// sphere and lose its long axis.  The largest of the three is the length
	// half; the smallest is the thickness half.
	struct Extent
	{
		float length{ 0.0f };     // half extent along the long axis
		float thickness{ 0.0f };  // half extent along the short axis
		float radius{ 0.0f };     // the single-number bound, as before

		// How far the shape reaches **vertically** from its own centre, in
		// game units.  This is the number a drop test wants and it is not
		// `radius`.
		//
		// `radius` is a single bound that has to cover the shape in every
		// direction, so for a box it is the *space diagonal*
		// `sqrt(hx^2 + hy^2 + hz^2)`.  Measured on a fur helmet: 12.62, of
		// which the vertical part was 4.16 - the diagonal is 8.47 units
		// longer than the shape is tall.  Subtracting the diagonal from the
		// centre put the object's "lowest point" 8.47 units below where it
		// really is, so the mark was stamped that far under the object and
		// the object read as buried in snow that had never been touched.
		//
		// The vertical reach is the half extent along whichever of the
		// shape's own axes ends up pointing up, which for a box held at an
		// angle is not any single raw projection - see the caller for how it
		// is derived.
		float vertical{ 0.0f };

		// Diagnostic only.  Says whether the numbers above came from walking
		// a container's children or from projecting the shape itself, and how
		// many children there were.  An arrow whose collidable turns out to
		// be a single capsule reports zero children; one wrapped in a
		// collection reports two or three.  Without this the log cannot tell
		// a correct reading from a container's undocumented self-projection.
		bool fromChildren{ false };
		int  childCount{ 0 };

		// Diagnostic only, and the one that matters most: the raw numbers
		// GetMaximumProjection handed back per axis, before any scaling or
		// halving.  A zero length with a non-zero radius is otherwise
		// ambiguous - it could mean the projection returned zero, or that the
		// caller never got as far as measuring - and those two want opposite
		// fixes.
		float rawPX{ 0.0f };
		float rawMX{ 0.0f };
		float rawPY{ 0.0f };
		float rawMY{ 0.0f };
		float rawPZ{ 0.0f };
		float rawMZ{ 0.0f };
		bool  measured{ false };
	};

	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius);

	// Which step of the last GetExtent call declined, as a short label.  All
	// the failure paths return the same false, and the fix differs per step,
	// so the name is reported rather than inferred.
	const char* LastExtentStep();

	// The last shape the extent walk measured, and what it measured.  The
	// raw hkpShapeType value is reported because the recognised types are a
	// short list and anything outside it takes a different branch.
	int    LastShapeType();
	Extent LastExtent();

	// The extent form: same walk, but it keeps the long and short axes
	// apart instead of discarding them.
	bool GetExtent(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, Extent& a_extent);

	// The unit direction, in world space, of the long axis that GetExtent
	// measured.  A length alone cannot aim a ray: an arrow stuck in at an
	// angle has its long axis pointing somewhere between horizontal and
	// vertical, and a ray fired straight down from its origin can only ever
	// land under the origin.
	bool GetLongAxis(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_axis);

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius);

	bool ExtractExtent(const RE::hkpShape* a_shape, Extent& a_extent);
}
