// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "SnowView.h"

#include "Clipmap.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "Weather.h"

#include <algorithm>

extern "C" __declspec(dllexport) bool NMN_DT_GetSnowView(NMN_DT_SnowView* a_view)
{
	if (!a_view || a_view->size < sizeof(NMN_DT_SnowView)) {
		return false;
	}
	const std::uint32_t size = a_view->size;
	*a_view = {};
	a_view->size = size;
	a_view->version = kSnowViewVersion;

	float      centreX = 0.0f;
	float      centreY = 0.0f;
	float      halfExtent = 0.0f;
	const bool window = Clipmap::GetWindow(centreX, centreY, halfExtent);

	ID3D11ShaderResourceView* coverage = SnowCoverage::Ready() ? SnowCoverage::View() : nullptr;
	a_view->raising = Settings::enableTessellation && Settings::useClipmap && Settings::enableSnowRaise && Settings::snowRaiseHeight > 0.0f && coverage &&
	                          window ?
	                      1u :
	                      0u;
	a_view->coverage = coverage;
	a_view->coverageWorldSize = SnowCoverage::kWorldSize;
	a_view->coverageTexels = SnowCoverage::kTexels;

	a_view->capOn = Settings::shelterMeshCap ? 1u : 0u;
	a_view->cap = Settings::shelterMeshCap ? Shelter::View() : nullptr;
	a_view->capWorldSize = Shelter::kWorldSize;
	a_view->capTexels = Shelter::kTexels;
	a_view->capFadeStart = Shelter::kWorldSize * 0.39f;
	a_view->capFadeEnd = Shelter::kWorldSize * 0.47f;

	a_view->raiseHeight = Settings::snowRaiseHeight;
	a_view->raiseScale = Weather::RaiseScale();
	a_view->windowX = centreX;
	a_view->windowY = centreY;
	a_view->fadeEnd = Settings::snowRaiseDistance;
	a_view->fadeStart = std::max(Settings::snowRaiseDistance - Settings::snowRaiseFadeBand, 0.0f);
	return true;
}
