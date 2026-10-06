// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>

namespace CollisionRadius
{
	enum class Kind { Sphere, Capsule, Box, Cylinder, Other };
	template <class Projection>
	float Evaluate(Kind kind, Projection&& project)
	{
		const float hx = 0.5f * (project(1.0f, 0.0f, 0.0f) - project(-1.0f, 0.0f, 0.0f));
		if (kind == Kind::Sphere) { return hx; }
		const float hy = 0.5f * (project(0.0f, 1.0f, 0.0f) - project(0.0f, -1.0f, 0.0f));
		const float hz = 0.5f * (project(0.0f, 0.0f, 1.0f) - project(0.0f, 0.0f, -1.0f));
		if (kind == Kind::Box) { return std::sqrt(hx * hx + hy * hy + hz * hz); }
		if (kind == Kind::Cylinder) {
			const float radial = std::max(hx, hy);
			return std::sqrt(radial * radial + hz * hz);
		}
		return std::max({hx, hy, hz});
	}
}
