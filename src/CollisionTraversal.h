// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace CollisionTraversal
{
	template <class Collision, class Object, class Visitor>
	bool VisitOwned(Object* object, Visitor& visitor)
	{
		if (!object) { return false; }
		auto* collision = static_cast<Collision*>(object->collisionObject.get());
		if (collision && visitor(collision, object)) { return true; }
		if (auto* node = object->AsNode()) {
			for (auto& child : node->GetChildren()) {
				if (VisitOwned<Collision>(child.get(), visitor)) { return true; }
			}
		}
		return false;
	}
	template <class Collision, class Object, class Visitor>
	bool Visit(Object* object, Visitor& visitor)
	{
		if (!object) { return false; }
		auto* collision = static_cast<Collision*>(object->collisionObject.get());
		if (collision && visitor(collision)) { return true; }
		if (auto* node = object->AsNode()) {
			for (auto& child : node->GetChildren()) {
				if (Visit<Collision>(child.get(), visitor)) { return true; }
			}
		}
		return false;
	}
}
