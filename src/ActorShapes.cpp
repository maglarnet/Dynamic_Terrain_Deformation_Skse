// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ActorShapes.h"

#include <algorithm>
#include <cmath>

namespace ActorShapes
{
	// Forward declared: the recursive walk below consults it, and it is
	// defined further down next to the other projection arithmetic.
	bool ExtractExtent(const RE::hkpShape* a_shape, Extent& a_extent);

	// Which step of the extent walk declined.  Declared up here because
	// GetBound, which comes first, is one of the steps: every failure path
	// returns the same false, and the fix differs per step, so the name is
	// recorded rather than inferred.
	enum class ExtentStep
	{
		kNone,
		kNoObject,
		kNoRigid,
		kNoHkpRigid,
		kNoShape,
		kRecursed,
		kExtractRadius,
		kRadiusOk,
		kRadiusFailed,
	};

	const char* ExtentStepName(ExtentStep a_step)
	{
		switch (a_step) {
		case ExtentStep::kNoObject: return "no-object";
		case ExtentStep::kNoRigid: return "no-rigid";
		case ExtentStep::kNoHkpRigid: return "no-hkp-rigid";
		case ExtentStep::kNoShape: return "no-shape";
		case ExtentStep::kRecursed: return "recursed";
		case ExtentStep::kExtractRadius: return "extract-radius";
		case ExtentStep::kRadiusOk: return "radius-ok";
		case ExtentStep::kRadiusFailed: return "radius-failed";
		default: return "none";
		}
	}

	ExtentStep g_lastStep{ ExtentStep::kNone };

	// The last shape the extent walk looked at, and what it measured.  Kept
	// outside the per-call Extent because ExtractRadius throws its Extent
	// away, and the numbers it measured are the only evidence for why a
	// radius came back as zero.
	int    g_lastShapeType{ -1 };
	Extent g_lastExtent{};

	const char* LastExtentStep()
	{
		return ExtentStepName(g_lastStep);
	}

	int LastShapeType()
	{
		return g_lastShapeType;
	}

	Extent LastExtent()
	{
		return g_lastExtent;
	}

	// An arrow's collidable is not one shape but several: a capsule for the
	// shaft and a pair of thin slabs for the fletching, gathered into a
	// collection.  Walking only the outermost shape therefore sees a
	// container, and a container cannot answer "how long is the long axis" -
	// it has no single axis.  The extent is the *widest* of its children's,
	// which is the shaft's capsule, and that is the number the shaft test
	// needs.  Taking the container's own projection instead is what made the
	// length come back as the whole bound and the ratio collapse.
	//
	// Asked of the shape's own container interface rather than by casting to
	// one of the concrete collection types.  An earlier version cast a rigid
	// body to a list shape, which are unrelated types - a rigid body is an
	// entity and a list shape is a shape - so its answer was whatever the
	// differing layouts happened to agree on.
	bool ExtractExtentRecursive(const RE::hkpShape* a_shape, Extent& a_extent, int a_depth)
	{
		if (!a_shape) {
			return false;
		}

		if (auto* container = a_shape->GetContainer(); container && a_depth < 4) {
			// Children in turn.  The widest long axis wins, so the shaft's
			// capsule is what the caller ends up describing even when it is
			// listed after the fletching slabs.
			bool any = false;
			Extent best{};
			for (auto key = container->GetFirstKey(); key != RE::HK_INVALID_SHAPE_KEY;
				key = container->GetNextKey(key)) {
				RE::hkpShapeBuffer buffer;
				const auto* child = container->GetChildShape(key, buffer);
				Extent childExtent{};
				if (ExtractExtentRecursive(child, childExtent, a_depth + 1) &&
					childExtent.length > best.length) {
					best = childExtent;
					any = true;
				}
			}
			if (any) {
				a_extent = best;
				a_extent.fromChildren = true;
				a_extent.childCount = static_cast<int>(container->GetNumChildShapes());
				return true;
			}
			// A container whose children all declined still has its own
			// projection to offer, so fall through rather than give up.  The
			// flag stays false, which is how the log tells the two routes
			// apart - and they are worth telling apart, because a container's
			// own projection is not documented to be the union of its
			// children's and is the one number here that cannot be checked
			// from outside.
			a_extent.childCount = static_cast<int>(container->GetNumChildShapes());
		}

		return ExtractExtent(a_shape, a_extent);
	}

	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius)
	{
		g_lastStep = ExtentStep::kNoObject;
		if (!a_object) {
			return false;
		}

		g_lastStep = ExtentStep::kNoRigid;
		auto* rigid = a_object->body.get() ? a_object->body.get()->AsBhkRigidBody() : nullptr;
		if (!rigid) {
			return false;
		}

		g_lastStep = ExtentStep::kNoHkpRigid;
		auto* hkpRigid = skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
		if (!hkpRigid) {
			return false;
		}

		RE::hkVector4 massCentre;
		rigid->GetCenterOfMassWorld(massCentre);

		float components[4];
		_mm_storeu_ps(components, massCentre.quad);

		a_centre = RE::NiPoint3(components[0], components[1], components[2]) *
		           RE::bhkWorld::GetWorldScaleInverse();

		g_lastStep = ExtentStep::kNoShape;
		const auto* shape = hkpRigid->collidable.GetShape();
		if (!shape) {
			return false;
		}

		g_lastStep = ExtentStep::kExtractRadius;
		const bool ok = ExtractRadius(shape, a_radius);

		// The last step is the interesting one: ExtractRadius has already
		// been reached, so the only ways on from here are a shape type it
		// does not recognise or a measurement of zero.
		g_lastStep = ok ? ExtentStep::kRadiusOk : ExtentStep::kRadiusFailed;
		return ok;
	}

	bool ExtractExtent(const RE::hkpShape* a_shape, Extent& a_extent)
	{
		// Only the measured numbers are cleared, not the whole struct: the
		// recursive caller has already stamped its diagnostics into
		// fromChildren and childCount, and assigning a fresh Extent here
		// would erase exactly the flags that say which route produced the
		// numbers.
		a_extent.length = 0.0f;
		a_extent.thickness = 0.0f;
		a_extent.radius = 0.0f;
		a_extent.vertical = 0.0f;
		if (!a_shape) {
			return false;
		}

		const auto rawProject = [a_shape](float a_x, float a_y, float a_z) {
			return a_shape->GetMaximumProjection(RE::hkVector4{ a_x, a_y, a_z, 0.0f });
		};

		// Kept un-scaled as well as scaled.  Whether the engine already
		// applies the world scale here is not stated anywhere in the header,
		// and guessing it wrong shrinks every number by a factor of seventy
		// without any symptom except a wrong answer - so both are recorded
		// and the log says which one the shape actually produced.
		a_extent.rawPX = rawProject(1.0f, 0.0f, 0.0f);
		a_extent.rawMX = rawProject(-1.0f, 0.0f, 0.0f);
		a_extent.rawPY = rawProject(0.0f, 1.0f, 0.0f);
		a_extent.rawMY = rawProject(0.0f, -1.0f, 0.0f);
		a_extent.rawPZ = rawProject(0.0f, 0.0f, 1.0f);
		a_extent.rawMZ = rawProject(0.0f, 0.0f, -1.0f);
		a_extent.measured = true;

		// Remembered for the caller.  Whatever this returns, the shape's own
		// type and its six projections are the only things that can say
		// whether a zero came from the engine or from the arithmetic.
		g_lastShapeType = static_cast<int>(a_shape->type);

		// `hkpConvexShape::radius` is deliberately **not** subtracted from
		// the readings below.  Doing so was written and built once, on the
		// reasoning that Havok defines `getMaximumProjection` as the core's
		// projection plus `m_radius`, and the session log refutes it: an
		// arrow read `+X=-X=0.03`, `+Y=-Y=0.41`, `+Z=-Z=0.00`, three
		// different numbers that cannot each carry the same radius, and the
		// half extents derived from them reproduce the logged length and
		// radius exactly.  See the note in `ActorShapes.h` for the two
		// worked objects.

		// Reused from the recorded raw values rather than asked again: the
		// engine's projection is a virtual call whose result cannot change
		// between two calls a few instructions apart, and asking twice only
		// gave two chances to disagree.
		//
		// Summed, not subtracted.  GetMaximumProjection is a support function:
		// it returns the largest projection of any vertex onto the direction,
		// and that is the same number for +d and -d whenever the box sits to
		// one side of its own origin - which is what the log showed, +X and
		// -X both 0.03 against +Y and -Y both 0.41.  Subtracting two equal
		// numbers gave zero on every axis, and a zero extent then fell
		// through to a zero radius.  The half width along an axis is the
		// average of the two support values, which is the standard way to
		// recover a width from a support function and works whether or not
		// the origin is centred.
		const float invScale = RE::bhkWorld::GetWorldScaleInverse();

		const float hx = 0.5f * (a_extent.rawPX + a_extent.rawMX) * invScale;
		const float hy = 0.5f * (a_extent.rawPY + a_extent.rawMY) * invScale;
		const float hz = 0.5f * (a_extent.rawPZ + a_extent.rawMZ) * invScale;

		// The long axis is whichever half extent is largest, and the short
		// axis whichever is smallest.  A long thin shape therefore keeps the
		// ratio that says it is long and thin, which is exactly what the
		// single radius used to destroy.
		//
		// A zero axis is skipped rather than counted.  An arrow's box came
		// back with one half extent of exactly zero, which is a degenerate
		// side rather than a genuine thinness - a box with no depth is a
		// plane - and letting it stand as the thickness made every ratio
		// divide by zero.  The thinnest axis that actually has a size is the
		// honest answer, and if none of them do the shape is not measurable
		// and says so.
		a_extent.length = std::max({ hx, hy, hz });

		float thinnest = -1.0f;
		for (const float half : { hx, hy, hz }) {
			if (half > 0.0f && (thinnest < 0.0f || half < thinnest)) {
				thinnest = half;
			}
		}
		a_extent.thickness = thinnest > 0.0f ? thinnest : 0.0f;

		const float hi = a_extent.length;
		const float lo = a_extent.thickness;

		// The vertical reach, taken the same way `hz` was: the projections
		// were asked along the world axes, so `+Z` and `-Z` measure exactly
		// how far the shape reaches up and down.  Averaged rather than
		// subtracted for the same reason as above - the support function
		// returns the same number for +d and -d whenever the shape sits to
		// one side of its own origin.
		//
		// This is deliberately the *raw* z half extent and not `length`,
		// `thickness` or `radius`.  Every one of those can be much larger
		// than the shape is tall: a box's `radius` is its space diagonal
		// (measured 12.62 against a true 4.16 on a fur helmet) and its
		// `length` is its longest horizontal side.  A drop test that
		// subtracts any of them from the centre pushes the object's lowest
		// point below the ground it is actually resting on, and the mark
		// then lands under the object instead of at its feet.
		a_extent.vertical = hz;

		switch (a_shape->type) {
		case RE::hkpShapeType::kSphere:

			a_extent.radius = hx;
			g_lastExtent = a_extent;
			return true;

		case RE::hkpShapeType::kCapsule:

			a_extent.radius = hi;
			g_lastExtent = a_extent;
			return true;

		case RE::hkpShapeType::kBox:

			a_extent.radius = std::sqrt(hx * hx + hy * hy + hz * hz);
			g_lastExtent = a_extent;
			return true;

		case RE::hkpShapeType::kCylinder: {

			const float radial = std::max(hx, hy);
			a_extent.radius = std::sqrt(radial * radial + hz * hz);
			g_lastExtent = a_extent;
			return true;
		}

		default:

			a_extent.radius = hi;
			g_lastExtent = a_extent;
			return true;
		}
	}

	// Kept as the single-number form for every existing caller: it is now
	// literally the radius of the extent form, so the two cannot drift.
	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius)
	{
		Extent extent{};
		if (!ExtractExtent(a_shape, extent)) {
			return false;
		}
		a_radius = extent.radius;
		return true;
	}

	bool GetExtent(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, Extent& a_extent)
	{
		a_extent = Extent{};
		if (!a_object) {
			g_lastStep = ExtentStep::kNoObject;
			return false;
		}

		auto* rigid = a_object->body.get() ? a_object->body.get()->AsBhkRigidBody() : nullptr;
		if (!rigid) {
			g_lastStep = ExtentStep::kNoRigid;
			return false;
		}

		auto* hkpRigid = skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
		if (!hkpRigid) {
			g_lastStep = ExtentStep::kNoHkpRigid;
			return false;
		}

		RE::hkVector4 massCentre;
		rigid->GetCenterOfMassWorld(massCentre);

		float components[4];
		_mm_storeu_ps(components, massCentre.quad);

		a_centre = RE::NiPoint3(components[0], components[1], components[2]) *
		           RE::bhkWorld::GetWorldScaleInverse();

		const auto* shape = hkpRigid->collidable.GetShape();
		if (!shape) {
			g_lastStep = ExtentStep::kNoShape;
			return false;
		}

		g_lastStep = ExtentStep::kRecursed;
		// Through the container, so an arrow's collection answers with its
		// shaft rather than with the collection itself.
		return ExtractExtentRecursive(shape, a_extent, 0);
	}

	bool GetLongAxis(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_axis)
	{
		if (!a_object) {
			return false;
		}

		auto* rigid = a_object->body.get() ? a_object->body.get()->AsBhkRigidBody() : nullptr;
		if (!rigid) {
			return false;
		}

		auto* hkpRigid = skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
		if (!hkpRigid) {
			return false;
		}

		const auto* shape = hkpRigid->collidable.GetShape();
		if (!shape) {
			return false;
		}

		// Re-measure the three local half axes, keeping which one was the
		// largest rather than only how large it was.  The identity of the
		// axis is what the caller needs: the extent form above already
		// reports its length, but a length with no direction cannot aim a
		// ray, and an arrow lying at an angle has its long axis pointing
		// somewhere other than straight down.
		//
		// Summed, not subtracted, and for the same reason as in the extent
		// walk above: the two support values along an axis are equal whenever
		// the shape sits to one side of its origin, so subtracting left all
		// three at zero and the comparison below then picked whichever axis
		// happened to win a tie - the last one, always.
		const auto project = [shape](float a_x, float a_y, float a_z) {
			return shape->GetMaximumProjection(RE::hkVector4{ a_x, a_y, a_z, 0.0f });
		};

		const float ex = 0.5f * (project(1.0f, 0.0f, 0.0f) + project(-1.0f, 0.0f, 0.0f));
		const float ey = 0.5f * (project(0.0f, 1.0f, 0.0f) + project(0.0f, -1.0f, 0.0f));
		const float ez = 0.5f * (project(0.0f, 0.0f, 1.0f) + project(0.0f, 0.0f, -1.0f));

		// Column 0, 1 and 2 of the body's rotation are the local X, Y and Z
		// axes expressed in world space, which is the mapping this needs.
		const auto& rotation = hkpRigid->motion.motionState.transform.rotation;

		const auto column = [&rotation](int a_index) {
			float v[4];
			switch (a_index) {
			case 0: _mm_storeu_ps(v, rotation.col0.quad); break;
			case 1: _mm_storeu_ps(v, rotation.col1.quad); break;
			default: _mm_storeu_ps(v, rotation.col2.quad); break;
			}
			return RE::NiPoint3(v[0], v[1], v[2]);
		};

		RE::NiPoint3 axis{ 0.0f, 0.0f, 1.0f };
		if (ez >= ex && ez >= ey) {
			axis = column(2);
		} else if (ey >= ex) {
			axis = column(1);
		} else {
			axis = column(0);
		}

		const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
		if (!(len > 0.0f) || !std::isfinite(len)) {
			return false;
		}

		a_axis = RE::NiPoint3(axis.x / len, axis.y / len, axis.z / len);
		return std::isfinite(a_axis.x) && std::isfinite(a_axis.y) && std::isfinite(a_axis.z);
	}
}
