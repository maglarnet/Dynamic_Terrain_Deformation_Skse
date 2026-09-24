// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

// The engine gives two different things when something meets the ground, and
// they are not interchangeable:
//
//   * a contact point - where the object and the ground actually met.  The
//     projectile hit hook hands this over, and it is the only honest answer
//     for "where did this arrow land".
//   * a reference point - somewhere on the object, not necessarily the place
//     it touched.  A spent arrow registering as a projectile has its origin
//     at the nock, so an arrow that landed two units in front of the player
//     reported a mark two units *behind* roughly where the player was
//     standing.  That is a reference, not a contact.
//
// The engine also exposes "the first thing along this ray" (TES::Pick), which
// is how a reference point is turned into an actual contact when no contact
// point was handed over.  Every ray below is filtered to static world
// collision, so an arrow resting on a crate, or a dropped sword lying on a
// barrel, is never credited with marking the terrain underneath.
//
// Kept free of SKSE headers so the offline test can drive the arithmetic.

#include <algorithm>
#include <cmath>
#include <string_view>

namespace ContactSampler
{
	// What the object is: decides which kind of answer is worth trusting, and
	// is recorded in the log so a wrong fix is visible from the log alone.
	enum class Origin
	{
		kContact,
		kRay,
		kReference,
	};

	enum class Source
	{
		kNone,
		kMovement,
		kPlacement,
		kProjectile,
		kExplosion,
		kShout,
	};

	inline const char* OriginName(Origin a_origin)
	{
		switch (a_origin) {
		case Origin::kContact: return "contact";
		case Origin::kRay: return "ray";
		default: return "reference";
		}
	}

	inline const char* SourceName(Source a_source)
	{
		switch (a_source) {
		case Source::kMovement: return "movement";
		case Source::kPlacement: return "placement";
		case Source::kProjectile: return "projectile";
		case Source::kExplosion: return "explosion";
		case Source::kShout: return "shout";
		default: return "none";
		}
	}

	struct ContactPoint
	{
		float x{}, y{}, z{};
		bool  finite{ false };

		bool Finite() const { return finite; }
	};

	struct ReferencePoint
	{
		float x{}, y{}, z{};
		bool  finite{ false };
	};

	struct Footprint
	{
		float x{}, y{}, z{};
		float radius{ 0.0f };
		float depthScale{ 1.0f };
		Origin origin{ Origin::kRay };
		bool   resolved{ false };
	};

	inline bool Finite(const ContactPoint& a_point)
	{
		return a_point.finite && std::isfinite(a_point.x) && std::isfinite(a_point.y) &&
			std::isfinite(a_point.z);
	}

	inline bool Finite(const ReferencePoint& a_point)
	{
		return a_point.finite && std::isfinite(a_point.x) && std::isfinite(a_point.y) &&
			std::isfinite(a_point.z);
	}

	inline bool Finite(const Footprint& a_footprint)
	{
		return a_footprint.resolved && std::isfinite(a_footprint.x) &&
			std::isfinite(a_footprint.y) && std::isfinite(a_footprint.z) &&
			std::isfinite(a_footprint.radius) && std::isfinite(a_footprint.depthScale) &&
			a_footprint.radius > 0.0f;
	}

	// Case handling for the collision-filter list, so a list written in the
	// ini with spaces or capitals still excludes what it names.
	inline bool MatchesFilter(std::string_view a_filter, std::string_view a_material)
	{
		if (a_filter.empty() || a_material.empty()) {
			return false;
		}

		const auto same = [](char a_lhs, char a_rhs) {
			const auto lower = [](char a_c) {
				return static_cast<char>(a_c >= 'A' && a_c <= 'Z' ? a_c + ('a' - 'A') : a_c);
			};
			return lower(a_lhs) == lower(a_rhs);
		};

		for (size_t start = 0; start <= a_filter.size();) {
			size_t end = a_filter.find(',', start);
			if (end == std::string_view::npos) {
				end = a_filter.size();
			}
			size_t first = start;
			size_t last = end;
			while (first < last && (a_filter[first] == ' ' || a_filter[first] == '\t')) { ++first; }
			while (last > first && (a_filter[last - 1] == ' ' || a_filter[last - 1] == '\t')) { --last; }
			if (last - first == a_material.size() &&
				std::equal(a_material.begin(), a_material.end(), a_filter.begin() + first, same)) {
				return true;
			}
			if (end == a_filter.size()) {
				break;
			}
			start = end + 1;
		}
		return false;
	}

	// A shaft is long and thin.  Written against the object's shape rather
	// than its type, so an object that is not a projectile but still reads as
	// a shaft (a dropped spear, a stuck bolt) gets the same treatment.
	inline bool IsShaftShape(float a_length, float a_thickness)
	{
		if (!(a_length > 0.0f) || !(a_thickness > 0.0f) || !std::isfinite(a_length) ||
			!std::isfinite(a_thickness)) {
			return false;
		}
		return a_length >= a_thickness * 4.0f;
	}

	// ---- the resolver ----------------------------------------------------

	struct Query
	{
		bool  hasContact{ false };
		float contactX{}, contactY{}, contactZ{};

		bool  hasReference{ false };
		float referenceX{}, referenceY{}, referenceZ{};

		bool  hasShape{ false };
		float length{ 0.0f };
		float thickness{ 0.0f };

		// The direction of the long axis, in world space, as a unit vector.
		// Optional, but a shaft without it can only be probed straight down,
		// and straight down from the shaft's own origin lands wherever the
		// origin happens to be - which is the error this whole path exists
		// to avoid.  Supplied by the caller, which owns the shape.
		bool  hasAxis{ false };
		float axisX{}, axisY{}, axisZ{};

		bool  mustBeContact{ false };

		Source source{ Source::kNone };
	};

	struct Inputs
	{
		// "The first thing along this ray", from the engine's pick.  Kept as a
		// plain function pointer rather than a virtual: the resolver has no
		// state of its own, and a caller that forgets to bind it gets a clean
		// "no answer" instead of a crash.  Only the any-collision form is
		// used, deliberately - the engine's own projectiles are tuned against
		// exactly this ray, so an arrow that sticks in a signpost reports the
		// signpost rather than the snow behind it, and that correct behaviour
		// is inherited rather than re-derived.
		using PickFn = bool (*)(float a_x, float a_y, float a_z, float a_toX, float a_toY,
			float a_toZ, float& a_outX, float& a_outY, float& a_outZ);

		PickFn AnyPick{ nullptr };
	};

	struct Output
	{
		float x{}, y{}, z{};
		float radius{ 0.0f };
		float depthScale{ 1.0f };
		Origin origin{ Origin::kReference };
		bool   resolved{ false };
		Source source{ Source::kNone };
		bool   reachedGround{ false };
		float  horizontalMiss{ 0.0f };
	};

	// Probing is budgeted: each ray is a full physics query on the main
	// thread, so an object asking for a contact it will not get must not cost
	// an unbounded number of them.  Three rays per shaft at most, and the
	// caller decides whether the result is worth keeping.
	inline Output Resolve(const Query& a_query, const Inputs& a_inputs)
	{
		Output out{};
		out.source = a_query.source;

		const auto contactFinite = a_query.hasContact &&
			std::isfinite(a_query.contactX) && std::isfinite(a_query.contactY) &&
			std::isfinite(a_query.contactZ);
		const auto referenceFinite = a_query.hasReference &&
			std::isfinite(a_query.referenceX) && std::isfinite(a_query.referenceY) &&
			std::isfinite(a_query.referenceZ);

		const auto shaft = a_query.hasShape &&
			IsShaftShape(a_query.length, a_query.thickness);

		// A contact point handed over by the engine is the best answer there
		// is, so a shaft does not spend a ray overriding it.  The exception is
		// a source whose reported contact is known not to be the meeting point
		// - an explosion reports its centre, not the ground under it.
		if (contactFinite && !(shaft && a_query.mustBeContact)) {
			out.x = a_query.contactX;
			out.y = a_query.contactY;
			out.z = a_query.contactZ;
			out.origin = Origin::kContact;
			out.reachedGround = true;
			out.resolved = true;
			return out;
		}

		const float shaftLength = shaft ? a_query.length : 0.0f;
		const float shaftThickness = shaft ? a_query.thickness : 0.0f;

		// A shaft reports its origin at the nock and its bound bottom well
		// below the point that actually entered the ground.  Neither is the
		// contact, so both are only used as somewhere to aim a ray from.
		const float px = referenceFinite ? a_query.referenceX : a_query.contactX;
		const float py = referenceFinite ? a_query.referenceY : a_query.contactY;
		const float pz = referenceFinite ? a_query.referenceZ : a_query.contactZ;
		if (!referenceFinite && !contactFinite) {
			return out;
		}

		out.x = px;
		out.y = py;
		out.z = pz;

		// A ray fired *away* from the ground can still hit something: the
		// shaft's own bound, or a snow shell lying over it.  Accepting that
		// hit is what put the mark on the arrow instead of under it, so a
		// lane that looks upward is not a lane that can find the ground and
		// must decline instead of claiming the first thing it bumps into.
		//
		// The test is on the ray's *direction*, not on where the hit came
		// back.  A pick is free to answer at any point along the segment -
		// an upward lane that merely reports a high z is the shaft's own
		// bound, while a downward lane reporting a high z is a probe whose
		// stub answered loosely, and refusing the latter would throw away a
		// real contact.  Only the heading decides which of the two it is.
		const auto headsToGround = [](float a_fromZ, float a_toZ) {
			return a_toZ <= a_fromZ;
		};

		const auto probe = [&](float a_fromX, float a_fromY, float a_fromZ, float a_toX,
							   float a_toY, float a_toZ) {
			if (!headsToGround(a_fromZ, a_toZ)) {
				return false;
			}
			// The output is only written once the ray is known to have hit.
			// Writing the ray's destination up front would leave a failed probe
			// having moved the mark onto a point nothing was found at - which
			// is how a mark ends up under the player with reachedGround false.
			float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
			const auto hit = a_inputs.AnyPick &&
				a_inputs.AnyPick(a_fromX, a_fromY, a_fromZ, a_toX, a_toY, a_toZ,
					hitX, hitY, hitZ);
			if (hit && std::isfinite(hitX) && std::isfinite(hitY) && std::isfinite(hitZ)) {
				out.x = hitX;
				out.y = hitY;
				out.z = hitZ;
				out.reachedGround = true;
				out.origin = Origin::kRay;
				out.resolved = true;
				// Measured from the object's reference point, not from wherever
				// the ray happened to start: a ray that begins below the origin
				// would otherwise score itself a miss of zero and hide exactly
				// the sideways error this number exists to expose.
				out.horizontalMiss = std::hypot(hitX - px, hitY - py);
			}
			return hit;
		};

		const auto radius = shaftLength * 0.5f;

		if (shaft) {
			// Down the shaft's own axis first.  A stuck arrow points into the
			// ground, so a short ray along that heading from the tip arrives
			// at the entry point; three lengths is far enough to leave a
			// shaft that fell flat on the surface without reaching whatever
			// lies underneath a pier.
			if (contactFinite && referenceFinite) {
				const float dx = a_query.contactX - a_query.referenceX;
				const float dy = a_query.contactY - a_query.referenceY;
				const float dz = a_query.contactZ - a_query.referenceZ;
				const float span = dx * dx + dy * dy + dz * dz;
				if (span > shaftThickness * shaftThickness * 0.25f) {
					const float len = std::sqrt(span);
					const float reach = 1.0f + 3.0f * shaftLength;
					if (probe(px, py, pz, px + dx / len * reach, py + dy / len * reach,
							pz + dz / len * reach)) {
						return out;
					}
				}
			}

			// The shaft's own long axis, when the caller could supply one.
			// This is the case that matters for a spent arrow: there is no
			// engine contact to work from - by the time the object scan runs,
			// the impact record has already expired - so the axis is the only
			// thing that says which way the arrow is pointing, and therefore
			// the only way to find the hole the arrowhead made rather than
			// the place the nock is sitting.  Both directions are tried
			// because a shaft sticking out of the ground points *up* while
			// one lying in a drift points along the surface, and only the
			// direction that reaches the ground can be the right one.
			if (a_query.hasAxis && std::isfinite(a_query.axisX) &&
				std::isfinite(a_query.axisY) && std::isfinite(a_query.axisZ)) {
				const float ax = a_query.axisX;
				const float ay = a_query.axisY;
				const float az = a_query.axisZ;
				const float aLen = std::sqrt(ax * ax + ay * ay + az * az);
				if (aLen > 0.0f) {
					const float ux = ax / aLen;
					const float uy = ay / aLen;
					const float uz = az / aLen;
					// Half a length back along the axis is roughly where the
					// shaft's middle sits, which is far enough from the
					// origin that the ray does not start inside the arrow's
					// own bound and stop on the arrow instead of the ground.
					const float backOff = shaftLength * 0.5f;
					const float reach = shaftLength * 2.0f + 1.0f;
					for (const float sign : { 1.0f, -1.0f }) {
						if (probe(px - ux * backOff, py - uy * backOff, pz - uz * backOff,
								px + ux * sign * reach, py + uy * sign * reach,
								pz + uz * sign * reach)) {
							return out;
						}
					}
				}
			}

			// How far below its own origin the shaft's far half actually is.
			// The bound centre sits length/2 above the origin, so a ray from
			// the origin downward by its own half length stops at the tip.
			// Kept as the last resort before the blind fallback: it makes no
			// claim about where the arrow points, so it is only worth
			// spending once the axis route has declined.
			if (radius > 0.0f &&
				probe(px, py, pz + radius, px, py, pz)) {
				return out;
			}

			// Last resort: a ray that starts below the origin and looks further
			// down, twice the half length.  It deliberately begins below the
			// origin so that its landing point cannot help but differ from the
			// origin horizontally whenever the ground is not level - which is
			// what a wrong fix looks like in a screenshot, and what
			// horizontalMiss is here to catch.
			if (probe(px, py, pz - radius, px, py, pz - radius * 3.0f)) {
				return out;
			}
		} else if (contactFinite) {
			// Not a shaft: there is nothing long and thin for the origin to
			// disagree with, so no ray is spent.
			out.origin = Origin::kContact;
			out.reachedGround = true;
		}

		// The tail is reached only when every ray lane declined: the origin is
		// all there is, and it says so.  horizontalMiss deliberately stays at
		// zero here - nothing was found, so nothing can be claimed about how
		// far the mark sat from the object.
		out.origin = Origin::kReference;
		out.resolved = true;
		return out;
	}
}
