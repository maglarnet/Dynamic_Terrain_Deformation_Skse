// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include "BoxFilter.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace RoadSnowMask
{
	template <uint32_t N>
	class Cache
	{
	public:
		static_assert(N > 0 && (N & (N - 1)) == 0);
		void Reset() { road.fill(0); }
		bool Add(int32_t cellX, int32_t cellY)
		{
			const auto i = Index(cellX, cellY);
			if (road[i] && x[i] == cellX && y[i] == cellY) { return false; }
			road[i] = 1; x[i] = cellX; y[i] = cellY;
			return true;
		}
		bool Contains(int32_t cellX, int32_t cellY) const
		{
			const auto i = Index(cellX, cellY);
			return road[i] && x[i] == cellX && y[i] == cellY;
		}
		const uint8_t* Data() const { return road.data(); }
		static uint32_t Index(int32_t cellX, int32_t cellY)
		{
			return (static_cast<uint32_t>(cellY) & (N - 1)) * N + (static_cast<uint32_t>(cellX) & (N - 1));
		}
	private:
		std::array<uint8_t, N * N> road{};
		std::array<int32_t, N * N> x{}, y{};
	};
	inline bool IsRoadModel(std::string_view path)
	{
		std::string normalized(path);
		for (auto& c : normalized) {
			if (c == '\\') { c = '/'; }
			else if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; }
		}
		const auto slash = normalized.find_last_of('/');
		const std::string_view file = std::string_view(normalized).substr(slash == std::string::npos ? 0 : slash + 1);
		if (!file.ends_with(".nif") || file.find("lod") != std::string_view::npos ||
			file.find("sign") != std::string_view::npos || file.find("marker") != std::string_view::npos) { return false; }
		return file.starts_with("road") || file.starts_with("nr_road") ||
			normalized.starts_with("landscape/roads/") || normalized.find("/landscape/roads/") != std::string::npos;
	}

	template <uint32_t N, class Valid>
	void Apply(const uint8_t* roads, uint8_t* cap, uint8_t* expanded,
		uint8_t* feathered, uint32_t* rowSums, int margin, Valid valid)
	{
		static_assert(N > 8 && (N & (N - 1)) == 0);
		constexpr uint32_t mask = N - 1;
		std::fill_n(expanded, N * N, uint8_t{255});
		margin = std::clamp(margin, 1, 3);
		for (uint32_t y = 0; y < N; ++y) {
			for (uint32_t x = 0; x < N; ++x) {
				const uint32_t i = y * N + x;
				if (!roads[i] || !valid(i)) { continue; }
				for (int dy = -margin; dy <= margin; ++dy) {
					for (int dx = -margin; dx <= margin; ++dx) {
						expanded[((y + dy) & mask) * N + ((x + dx) & mask)] = 0;
					}
				}
			}
		}
		BoxFilter::Apply<N>(expanded, feathered, rowSums, 1);
		for (uint32_t i = 0; i < N * N; ++i) {
			cap[i] = std::min(cap[i], std::min(expanded[i], feathered[i]));
		}
	}
}
