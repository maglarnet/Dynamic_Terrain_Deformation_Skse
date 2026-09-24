// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "Settings.h"

#include <algorithm>
#include <cstdint>

namespace Clipmap
{
	inline constexpr uint32_t kTexels = 2048;
	inline constexpr float    kWorldSize = 1536.0f;
	inline constexpr float    kCellSize = kWorldSize / static_cast<float>(kTexels);

	inline constexpr uint32_t kMaxLevels = 2;
	inline constexpr float    kLevelScale = 4.0f;

	constexpr float WorldSizeFor(uint32_t a_level)
	{
		float size = kWorldSize;
		for (uint32_t i = 0; i < a_level; ++i) {
			size *= kLevelScale;
		}
		return size;
	}

	constexpr float CellSizeFor(uint32_t a_level)
	{
		return WorldSizeFor(a_level) / static_cast<float>(kTexels);
	}

	inline uint32_t LevelCount()
	{
		return std::clamp(Settings::clipmapLevels, 1u, kMaxLevels);
	}

	inline constexpr uint32_t kActivityTexels = 32;
	inline constexpr uint32_t kActivityRatio = kTexels / kActivityTexels;
	static_assert(kTexels % kActivityTexels == 0);

	inline constexpr uint32_t kActivitySlot = 0;

	inline constexpr uint32_t kLevel1Slot = 2;

	inline constexpr uint32_t kMaxStamps = 64;

	static_assert((kTexels & (kTexels - 1)) == 0, "kTexels must be a power of two");

	static_assert(kTexels % 8 == 0, "kTexels must divide evenly by the 8x8 thread group");

	struct Stamp
	{

		enum class Kind
		{

			kPress,

			kMelt,

			kPrint,
		};

		float x{ 0.0f };
		float y{ 0.0f };
		float radius{ 40.0f };
		float depth{ 15.0f };

		Kind kind{ Kind::kPress };

		float shoulder{ 0.3f };

		float decay{ 0.92f };

		float rim{ 0.0f };

		float forwardX{ 0.0f };
		float forwardY{ 1.0f };
		float halfWidth{ 0.0f };

		float mirror{ 1.0f };

		float motionX{ 0.0f };
		float motionY{ 0.0f };

		// How much of this stamp's own radius its rim may wander by, as a
		// fraction, for a round stamp.  Zero - the default - leaves the
		// outline a circle, which is what every mark wants except a crater.
		// A blast sets it so the hole it leaves is torn rather than turned;
		// see RadialBulge in ClipmapUpdateCS.h for why the shape needs a
		// term of its own at all.
		float rimBulge{ 0.0f };

		// Loose snow thrown up at this stamp's lip, as a multiple of what a
		// footprint's own rim noise raises, and - in lipBand - how wide that
		// band is as a fraction of the stamp's radius.  Zero, the default,
		// leaves the lip smooth; only a blast sets it, because a crater is
		// the one shape whose edge has no lumps of its own to begin with.
		float rimNoise{ 0.0f };
		float lipBand{ 0.0f };

		bool snow{ false };
	};

	bool Initialize();

	void ResetDiagnostics();
	void Release();
	bool Ready();

	void Update(float a_deltaSeconds);

	inline constexpr uint32_t kParamsSlot = 13;

	void BindDomain(ID3D11DeviceContext* a_context);
	void UnbindDomain(ID3D11DeviceContext* a_context);

	bool GetWindow(float& a_centreX, float& a_centreY, float& a_halfExtent);
}
