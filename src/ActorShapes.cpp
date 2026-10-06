// SPDX-License-Identifier: GPL-3.0-only
// Modifications Copyright (c) 2026 NearMidnightNow (NMN).
// Adapted from Community Shaders src/Utils/ActorUtils.cpp (GPLv3).
// Upstream contributors: Alan Tse, doodlum and Dawntic. See NOTICE.
// Renamed and refactored in the 2026-09-21 public snapshot.
// Provenance notice added 2026-09-28.

#include "PCH.h"

#include "ActorShapes.h"
#include "CollisionRadius.h"

#include <algorithm>
#include <cmath>

namespace ActorShapes
{
	bool GetBound(RE::bhkNiCollisionObject* a_object, RE::NiPoint3& a_centre, float& a_radius)
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

		if (skyrim_cast<RE::hkpListShape*>(hkpRigid)) {
			return false;
		}

		RE::hkVector4 massCentre;
		rigid->GetCenterOfMassWorld(massCentre);

		float components[4];
		_mm_storeu_ps(components, massCentre.quad);

		a_centre = RE::NiPoint3(components[0], components[1], components[2]) *
		           RE::bhkWorld::GetWorldScaleInverse();

		return ExtractRadius(hkpRigid->collidable.GetShape(), a_radius);
	}

	bool ExtractRadius(const RE::hkpShape* a_shape, float& a_radius)
	{
		if (!a_shape) {
			return false;
		}

		const auto project = [a_shape](float a_x, float a_y, float a_z) {
			return a_shape->GetMaximumProjection(RE::hkVector4{ a_x, a_y, a_z, 0.0f }) *
			       RE::bhkWorld::GetWorldScaleInverse();
		};

		CollisionRadius::Kind kind = CollisionRadius::Kind::Other;
		switch (a_shape->type) {
		case RE::hkpShapeType::kSphere: kind = CollisionRadius::Kind::Sphere; break;
		case RE::hkpShapeType::kCapsule: kind = CollisionRadius::Kind::Capsule; break;
		case RE::hkpShapeType::kBox: kind = CollisionRadius::Kind::Box; break;
		case RE::hkpShapeType::kCylinder: kind = CollisionRadius::Kind::Cylinder; break;
		default: break;
		}
		a_radius = CollisionRadius::Evaluate(kind, project);
		return true;
	}
}
