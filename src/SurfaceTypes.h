// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

namespace RE
{
	class BSShaderMaterial;
	class NiPoint3;

	enum class MATERIAL_ID : std::uint32_t;
}

namespace Surfaces
{

	enum class Type : uint8_t
	{
		kUnknown = 0,
		kSnow,
		kGrass,
		kDirt,
		kMud,
		kSand,
		kAsh,
		kGravel,
		kStone,

		kCount
	};

	const char* Name(Type a_type);

	struct Response
	{

		float depthScale{ 1.0f };

		float radiusScale{ 1.0f };

		float shoulder{ 0.3f };

		float decayScale{ 1.0f };

		float rimScale{ 1.0f };

		float print{ 1.0f };

		float clearanceScale{ 1.0f };
	};

	const Response& ResponseFor(Type a_type);

	float MarkDepth(Type a_type, float a_ordinaryDepth, float a_sink, float a_worldX,
		float a_worldY);

	float RimHeight(float a_markDepth, float a_rimScale);

	bool MatchesKeywords(std::string_view a_name, std::string_view a_keywords);

	Type ClassifyTexturePath(std::string_view a_path, bool a_skipSnow = false);

	Type At(const RE::NiPoint3& a_position);

	Type FromMaterialID(RE::MATERIAL_ID a_id);

	inline constexpr size_t kLayers = 6;

	struct Classification
	{

		Type layers[kLayers]{};

		int8_t reveal{ -1 };

		bool revealUnderSnow{ false };

		int8_t snow{ -1 };

		uint8_t used{ 0 };
	};

	Classification Classify(RE::BSShaderMaterial* a_material);

	void Reset();
}
