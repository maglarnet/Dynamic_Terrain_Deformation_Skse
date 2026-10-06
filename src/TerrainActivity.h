// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <atomic>

namespace TerrainActivity
{
	inline std::atomic<bool> active{ false };
	inline bool Active() { return active.load(std::memory_order_relaxed); }
}
