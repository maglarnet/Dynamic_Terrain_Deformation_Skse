// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

struct ID3D11ShaderResourceView;

inline constexpr std::uint32_t kSnowViewVersion = 1;

struct NMN_DT_SnowView
{
	std::uint32_t             size;
	std::uint32_t             version;
	std::uint32_t             raising;
	std::uint32_t             capOn;
	ID3D11ShaderResourceView* coverage;
	ID3D11ShaderResourceView* cap;
	float                     coverageWorldSize;
	std::uint32_t             coverageTexels;
	float                     capWorldSize;
	std::uint32_t             capTexels;
	float                     capFadeStart, capFadeEnd;
	float                     raiseHeight;
	float                     raiseScale;
	float                     windowX, windowY;
	float                     fadeStart, fadeEnd;
};
