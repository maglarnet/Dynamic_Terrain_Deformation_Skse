// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

namespace SnowSparkle
{

	bool Enabled();
	void NoteHookInstalled(bool a_installed);

	void NoteContact(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_forwardX,
		float a_forwardY, float a_velX, float a_velY, float a_radius);

	void Update(float a_deltaSeconds);

	void Render();

	void Reset();
	void Forget();
	void Release();
}
