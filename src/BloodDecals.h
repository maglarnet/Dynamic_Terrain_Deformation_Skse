// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace RE { class BSGeometry; }
namespace BloodDecals
{
	void Update();
	void Reset();
	bool MayContain(RE::BSGeometry* geometry);
	bool Contains(RE::BSGeometry* geometry);
	void NoteDraw(bool routed);
}
