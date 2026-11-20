// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "AuthoredSnowMath.h"
#include "SurfaceTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace RE
{
	class NiPoint3;
}

namespace Surfaces
{

	struct Ground
	{
		Type     type{ Type::kUnknown };
		Response response{};

		int profile{ -1 };
	};

	void LoadProfiles();

	void ReleaseProfiles();

	size_t      ProfileCount();
	const char* ProfileName(int a_index);

	std::filesystem::file_time_type ProfilesWriteTime();

	Ground GroundAt(const RE::NiPoint3& a_position);

	bool     HasMeshProfile();
	Response MeshResponse();

	const AuthoredSnowMath::ModelList& CitySnowMeshes();
}
