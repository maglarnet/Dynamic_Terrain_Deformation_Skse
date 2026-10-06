// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ShelterProbe
{
	struct Refresh
	{
		bool due{};
		uint32_t cells{};
	};

	class Clock
	{
	public:
		Refresh Advance(float delta, uint32_t cellsAt60Hz, uint32_t total)
		{
			const double dt = std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.1f) : 0.0;
			now += dt;
			elapsed += dt;
			credit += dt * 60.0 * cellsAt60Hz;
			if (elapsed + 1e-7 < 0.25) { return {}; }
			elapsed = std::max(0.0, std::fmod(elapsed + 1e-7, 0.25));
			const auto count = static_cast<uint32_t>(std::min(std::floor(credit + 1e-5), static_cast<double>(total)));
			credit = std::max(0.0, credit - count);
			return { true, count };
		}
		double now{};
	private:
		double elapsed{}, credit{};
	};

	struct LandCell
	{
		int32_t x{}, y{};
		float height{};
		double retryAt{};
		bool known{}, valid{};

		bool Matches(int32_t cellX, int32_t cellY) const { return known && x == cellX && y == cellY; }
		bool Get(int32_t cellX, int32_t cellY, float& out) const
		{
			if (!Matches(cellX, cellY) || !valid) { return false; }
			out = height;
			return true;
		}
		bool CanRetry(int32_t cellX, int32_t cellY, double now) const
		{
			return !Matches(cellX, cellY) || valid || now >= retryAt;
		}
		void Store(int32_t cellX, int32_t cellY, float landZ)
		{
			*this = { cellX, cellY, landZ, 0.0, true, true };
		}
		void Miss(int32_t cellX, int32_t cellY, double now)
		{
			*this = { cellX, cellY, 0.0f, now + 0.5, true, false };
		}
	};
}
