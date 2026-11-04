// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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
		bool Add(int32_t cellX, int32_t cellY, uint8_t coverage = 255)
		{
			const auto i = Index(cellX, cellY);
			if (!coverage || (road[i] == coverage && x[i] == cellX && y[i] == cellY)) { return false; }
			road[i] = coverage; x[i] = cellX; y[i] = cellY;
			return true;
		}
		uint8_t Coverage(int32_t cellX, int32_t cellY) const
		{
			const auto i = Index(cellX, cellY);
			return x[i] == cellX && y[i] == cellY ? road[i] : uint8_t{0};
		}
		bool Contains(int32_t cellX, int32_t cellY) const { return Coverage(cellX, cellY) != 0; }
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
	void Keep(const uint8_t* roads, uint8_t* keep, float margin, int seam, Valid valid)
	{
		static_assert(N > 8 && (N & (N - 1)) == 0);
		constexpr uint32_t mask = N - 1;
		std::fill_n(keep, N * N, uint8_t{255});
		margin = std::clamp(margin, -3.0f, 3.0f);
		seam = std::clamp(seam, 0, 8);
		const float grow = std::max(margin, 0.0f), shrink = std::max(-margin, 0.0f);
		const int reach = static_cast<int>(std::ceil(grow)) + seam;
		constexpr int stride = 23;
		std::array<uint8_t, stride * stride> stencil{};
		for (int dy = -reach; dy <= reach; ++dy) {
			for (int dx = -reach; dx <= reach; ++dx) {
				const float x = std::max(static_cast<float>(std::abs(dx)) - grow, 0.0f);
				const float y = std::max(static_cast<float>(std::abs(dy)) - grow, 0.0f);
				const float t = std::clamp(std::hypot(x, y) / static_cast<float>(seam + 1), 0.0f, 1.0f);
				stencil[(dy + reach) * stride + dx + reach] = static_cast<uint8_t>(255.0f * t * t * (3.0f - 2.0f * t) + 0.5f);
			}
		}
		const int whole = static_cast<int>(shrink);
		const float part = shrink - static_cast<float>(whole);
		struct Neighbour { int dx, dy; bool inner; };
		std::array<Neighbour, 81> around{};
		uint32_t arounds = 0;
		if (shrink > 0.0f) {
			for (int dy = -(whole + 1); dy <= whole + 1; ++dy) {
				for (int dx = -(whole + 1); dx <= whole + 1; ++dx) {
					const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
					if (d <= static_cast<float>(whole + 1)) { around[arounds++] = { dx, dy, d <= static_cast<float>(whole) }; }
				}
			}
		}
		for (uint32_t y = 0; y < N; ++y) {
			for (uint32_t x = 0; x < N; ++x) {
				const uint32_t i = y * N + x;
				uint32_t share = roads[i];
				if (!share || !valid(i)) { continue; }
				if (arounds) {
					uint32_t inner = share, outer = share;
					for (uint32_t n = 0; n < arounds; ++n) {
						const uint32_t j = ((y + around[n].dy) & mask) * N + ((x + around[n].dx) & mask);
						const uint32_t held = valid(j) ? roads[j] : 0u;
						outer = std::min(outer, held);
						if (around[n].inner) { inner = std::min(inner, held); }
					}
					share = static_cast<uint32_t>(static_cast<float>(inner) +
						(static_cast<float>(outer) - static_cast<float>(inner)) * part + 0.5f);
					if (!share) { continue; }
				}
				for (int dy = -reach; dy <= reach; ++dy) {
					for (int dx = -reach; dx <= reach; ++dx) {
						const uint32_t open = 255u - stencil[(dy + reach) * stride + dx + reach];
						const auto value = static_cast<uint8_t>(255u - (share * open + 127u) / 255u);
						auto& out = keep[((y + dy) & mask) * N + ((x + dx) & mask)];
						out = std::min(out, value);
					}
				}
			}
		}
	}

	inline uint8_t CoverageFor(uint8_t keep)
	{
		static const auto table = [] {
			std::array<uint8_t, 256> out{};
			for (uint32_t k = 0; k < 256; ++k) {
				const float r = static_cast<float>(k) / 255.0f;
				const float g = 0.5f - std::sin(std::asin(1.0f - 2.0f * r) / 3.0f);
				out[k] = static_cast<uint8_t>(std::clamp(0.5f + 0.5f * g, 0.5f, 1.0f) * 255.0f + 0.5f);
			}
			out[255] = 255;
			return out;
		}();
		return table[keep];
	}

	template <uint32_t N, class Valid>
	void Apply(const uint8_t* roads, uint8_t* cap, uint8_t* keep, float margin, int seam, Valid valid)
	{
		Keep<N>(roads, keep, margin, seam, valid);
		for (uint32_t i = 0; i < N * N; ++i) {
			cap[i] = std::min(cap[i], keep[i]);
		}
	}

	template <uint32_t N, class Valid>
	void ApplyToCoverage(const uint8_t* roads, uint8_t* cover, uint8_t* keep, float margin, int seam, Valid valid)
	{
		Keep<N>(roads, keep, margin, seam, valid);
		for (uint32_t i = 0; i < N * N; ++i) {
			if (keep[i] != 255) { cover[i] = std::min(cover[i], CoverageFor(keep[i])); }
		}
	}
}
