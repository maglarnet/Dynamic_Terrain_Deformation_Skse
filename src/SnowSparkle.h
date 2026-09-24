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

	// Snow thrown from all the way round the lip of something that landed
	// hard, rather than from a single point under a foot.
	//
	// NoteContact carries a walking actor's two feet and keeps at most eight
	// points, which is the wrong shape for a blast: a crater wants the ring
	// sampled all the way round, in one frame, without competing with the
	// footprints for those eight slots.  a_radius is the lip's own radius,
	// a_outwardSpeed how fast the snow leaves it, a_points how many places
	// round the ring to throw from, and a_rate how many flakes the whole
	// blast should release - all of it spent on the frame this is called,
	// because a blast is an event and not a footfall.
	void NoteBurst(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_radius,
		float a_outwardSpeed, int a_points, float a_rate);

	void Update(float a_deltaSeconds);

	void Render();

	void Reset();
	void Release();
}
