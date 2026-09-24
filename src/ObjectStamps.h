// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "Clipmap.h"

#include <vector>

namespace ObjectStamps
{

	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor,
		std::vector<Clipmap::Stamp>& a_out);

	// The engine's own answer to "where did this projectile meet something".
	// Told to us by MagicImpacts, which already has the hook.  Recorded so a
	// spent arrow is stamped where it landed rather than where its origin
	// happens to sit, and so the reading is checkable from the log.
	void NoteContact(RE::Projectile* a_projectile, const RE::NiPoint3& a_position);

	// Contact points older than a second are stale: a projectile that landed
	// and was cleaned up must not leave its point behind for the next
	// projectile that reuses the reference.
	void PruneContacts();

	size_t ContactCount();

	void Reset();
}
