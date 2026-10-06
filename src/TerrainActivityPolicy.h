// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <cstdint>

namespace TerrainActivity
{
	template <class Location>
	bool IsCity(Location* location)
	{
		for (unsigned depth = 0; location && depth < 32; ++depth) {
			if (location->HasKeywordString("LocTypeCity")) { return true; }
			location = location->parentLoc;
		}
		return false;
	}

	template <class World>
	bool IsCityWorld(World* world)
	{
		return world && world->parentWorld && IsCity(world->location);
	}

	struct Policy
	{
		bool active{ false };
		uint32_t world{ 0 };

		bool Update(bool loaded, bool interior, bool city, uint32_t nextWorld)
		{
			const bool nextActive = loaded && !interior && !city && nextWorld != 0;
			const bool reset = nextActive != active || (nextActive && nextWorld != world);
			active = nextActive;
			world = nextWorld;
			return reset;
		}
	};
}
