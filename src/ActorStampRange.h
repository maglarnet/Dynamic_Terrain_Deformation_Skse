// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <algorithm>
#include <cmath>

namespace ActorStampRange
{
	inline float Resolve(bool extended, float requested, float fineWorldSize, float outerWorldSize)
	{
		const float ordinary = fineWorldSize * 0.375f;
		if (!extended || !std::isfinite(requested)) { return ordinary; }
		return std::clamp(requested, ordinary, std::max(ordinary, outerWorldSize * 0.375f));
	}
}
