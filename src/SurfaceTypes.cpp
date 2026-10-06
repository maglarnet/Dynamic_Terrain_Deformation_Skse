// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Settings.h"
#include "SnowCoverage.h"
#include "SnowSurface.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <unordered_map>

namespace Surfaces
{
	namespace
	{

		constexpr Type kMatchOrder[] = {
			Type::kAsh,
			Type::kMud,
			Type::kSand,
			Type::kGravel,
			Type::kSnow,
			Type::kDirt,
			Type::kStone,
			Type::kGrass,
		};

		constexpr Type kSnowRevealPreference[] = {
			Type::kDirt,
			Type::kMud,
			Type::kGravel,
			Type::kAsh,
			Type::kSand,
			Type::kStone,
			Type::kGrass,
		};

		constexpr Type kRevealPreference[] = {
			Type::kDirt,
			Type::kMud,
			Type::kGravel,
			Type::kAsh,
			Type::kSand,
			Type::kStone,
		};

		const std::string& KeywordsFor(Type a_type)
		{
			static const std::string empty{};

			switch (a_type) {
			case Type::kSnow:   return Settings::surfaceSnow;
			case Type::kGrass:  return Settings::surfaceGrass;
			case Type::kDirt:   return Settings::surfaceDirt;
			case Type::kMud:    return Settings::surfaceMud;
			case Type::kSand:   return Settings::surfaceSand;
			case Type::kAsh:    return Settings::surfaceAsh;
			case Type::kGravel: return Settings::surfaceGravel;
			case Type::kStone:  return Settings::surfaceStone;
			default:            return empty;
			}
		}

		bool Matches(std::string_view a_name, std::string_view a_keywords)
		{
			size_t start = 0;
			while (start <= a_keywords.size()) {
				const auto comma = a_keywords.find(',', start);
				const auto end = comma == std::string_view::npos ? a_keywords.size() : comma;

				const auto keyword = a_keywords.substr(start, end - start);

				if (!keyword.empty()) {
					if (keyword.front() == '^') {
						const auto body = keyword.substr(1);
						if (!body.empty() && a_name.rfind(body, 0) == 0) {
							return true;
						}
					} else if (a_name.find(keyword) != std::string_view::npos) {
						return true;
					}
				}

				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
			return false;
		}

		Type ClassifyName(std::string_view a_path, bool a_skipSnow = false)
		{
			return ClassifyTexturePath(a_path, a_skipSnow);
		}

		std::string_view TexturePath(RE::NiSourceTexture* a_texture)
		{

			if (!a_texture) {
				return {};
			}
			const auto& name = a_texture->name;
			return name.empty() ? std::string_view{} : std::string_view(name.c_str());
		}

		std::mutex                                     g_cacheLock;
		std::unordered_map<uint64_t, Classification>   g_cache;

		constexpr size_t kMaxCacheEntries = 512;

		uint64_t CacheKey(RE::NiSourceTexture* const* a_textures, size_t a_count)
		{
			uint64_t key = 0xcbf29ce484222325ull;
			for (size_t i = 0; i < a_count; ++i) {
				key ^= reinterpret_cast<uint64_t>(a_textures[i]);
				key *= 0x100000001b3ull;
			}
			return key;
		}

		std::atomic<uint32_t> g_loggedPlain{ 0 };
		std::atomic<uint32_t> g_loggedSnow{ 0 };
		constexpr uint32_t    kMaxLoggedPlain = 16;
		constexpr uint32_t    kMaxLoggedSnow = 16;

		void LogClassification(const Classification& a_result,
			RE::NiSourceTexture* const*             a_textures)
		{
			if (!Settings::logSurfaceClassification) {
				return;
			}

			const bool hasSnow = a_result.snow >= 0;
			auto&      budget = hasSnow ? g_loggedSnow : g_loggedPlain;
			const auto cap = hasSnow ? kMaxLoggedSnow : kMaxLoggedPlain;

			if (budget.fetch_add(1) >= cap) {
				return;
			}

			std::string detail;
			for (size_t i = 0; i < kLayers; ++i) {
				const auto path = TexturePath(a_textures[i]);
				if (path.empty()) {
					continue;
				}

				const auto slash = path.find_last_of("\\/");
				const auto leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);

				detail += std::format("\n      [{}] {:<10} {}", i, Name(a_result.layers[i]), leaf);
			}

			logger::info("Landscape surfaces ({} layers, snow={}, reveal={}{}):{}",
				a_result.used,
				a_result.snow < 0 ? "no" : "yes",
				a_result.reveal < 0 ? "none" : Name(a_result.layers[a_result.reveal]),
				a_result.revealUnderSnow ? " under-snow" : "",
				detail);
		}
	}

	Type ClassifyTexturePath(std::string_view a_path, bool a_skipSnow)
	{
		if (a_path.empty()) {
			return Type::kUnknown;
		}

		const auto slash = a_path.find_last_of("\\/");
		const auto leaf = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);

		std::string lowered(leaf);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		for (const Type candidate : kMatchOrder) {
			if (a_skipSnow && candidate == Type::kSnow) {
				continue;
			}
			if (Matches(lowered, KeywordsFor(candidate))) {
				return candidate;
			}
		}

		return Type::kUnknown;
	}

	Type FromMaterialID(RE::MATERIAL_ID a_id)
	{

		switch (a_id) {
		case RE::MATERIAL_ID::kSnow:
		case RE::MATERIAL_ID::kSnowStairs:
		case RE::MATERIAL_ID::kIce:
		case RE::MATERIAL_ID::kIceForm:
			return Type::kSnow;

		case RE::MATERIAL_ID::kGrass:
			return Type::kGrass;

		case RE::MATERIAL_ID::kDirt:
			return Type::kDirt;

		case RE::MATERIAL_ID::kMud:
			return Type::kMud;

		case RE::MATERIAL_ID::kSand:
			return Type::kSand;

		case RE::MATERIAL_ID::kAsh:
			return Type::kAsh;

		case RE::MATERIAL_ID::kGravel:
			return Type::kGravel;

		case RE::MATERIAL_ID::kStoneBroken:
			return Type::kGravel;

		case RE::MATERIAL_ID::kStone:
		case RE::MATERIAL_ID::kStoneHeavy:
		case RE::MATERIAL_ID::kStoneStairs:
		case RE::MATERIAL_ID::kStoneStairsBroken:
		case RE::MATERIAL_ID::kStoneAsStairs:
		case RE::MATERIAL_ID::kBoulderSmall:
		case RE::MATERIAL_ID::kBoulderMedium:
		case RE::MATERIAL_ID::kBoulderLarge:
			return Type::kStone;

		default:
			return Type::kUnknown;
		}
	}

	Type At(const RE::NiPoint3& a_position)
	{
		if (!Settings::enableSurfaceClassification) {
			return Type::kUnknown;
		}

		auto* tes = RE::TES::GetSingleton();
		if (!tes) {
			return Type::kUnknown;
		}

		return FromMaterialID(tes->GetLandMaterialType(a_position));
	}

	const Response& ResponseFor(Type a_type)
	{
		const auto index = static_cast<size_t>(a_type);
		return index < static_cast<size_t>(Type::kCount) ? Settings::surfaceResponse[index] :
		                                                   Settings::surfaceResponse[0];
	}

	float MarkDepth(Type a_type, float a_ordinaryDepth, float a_sink, float a_worldX,
		float a_worldY)
	{
		if (a_type != Type::kSnow) {
			return a_ordinaryDepth;
		}

		const float lower = std::clamp(Settings::snowTrenchCoverage, 0.5f, 0.95f);
		const float cover = SnowCoverage::At(a_worldX, a_worldY);
		const float t = std::clamp((cover - lower) / std::max(1.0f - lower, 1e-3f),
			0.0f, 1.0f);
		const float snowy = t * t * (3.0f - 2.0f * t);

		const float trench = SnowSurface::BlanketFor(snowy, a_worldX, a_worldY) *
			std::clamp(Settings::snowTrenchDepth, 0.0f, 4.0f) *
			std::clamp(a_sink, 0.0f, 1.0f);

		return std::max(a_ordinaryDepth, trench);
	}

	float RimHeight(float a_markDepth, float a_rimScale)
	{

		const float reference = std::lerp(a_markDepth,
			std::max(Settings::stampDepth, 0.0f) * Weather::DepthScale(),
			std::clamp(Settings::stampRimIndependence, 0.0f, 1.0f));

		return reference * Settings::stampRimHeight * a_rimScale;
	}

	bool MatchesKeywords(std::string_view a_name, std::string_view a_keywords)
	{
		return Matches(a_name, a_keywords);
	}


	const char* Name(Type a_type)
	{
		switch (a_type) {
		case Type::kSnow:   return "snow";
		case Type::kGrass:  return "grass";
		case Type::kDirt:   return "dirt";
		case Type::kMud:    return "mud";
		case Type::kSand:   return "sand";
		case Type::kAsh:    return "ash";
		case Type::kGravel: return "gravel";
		case Type::kStone:  return "stone";
		default:            return "unknown";
		}
	}

	Classification Classify(RE::BSShaderMaterial* a_material)
	{
		Classification result{};

		if (!Settings::enableSurfaceClassification || !a_material) {
			return result;
		}

		if (a_material->GetFeature() != RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend) {
			return result;
		}

		auto* land = static_cast<RE::BSLightingShaderMaterialLandscape*>(a_material);

		Type under[kLayers]{};

		RE::NiSourceTexture* textures[kLayers]{};
		textures[0] = land->diffuseTexture.get();
		for (size_t i = 1; i < kLayers; ++i) {
			textures[i] = land->landscapeDiffuseTexture[i - 1].get();
		}

		const uint64_t key = CacheKey(textures, kLayers);

		{
			const std::scoped_lock lock(g_cacheLock);
			if (const auto it = g_cache.find(key); it != g_cache.end()) {
				return it->second;
			}
		}

		for (size_t i = 0; i < kLayers; ++i) {
			if (!textures[i]) {
				result.layers[i] = Type::kUnknown;
				continue;
			}

			++result.used;

			if (land->textureIsSnow[i] != 0.0f) {
				result.layers[i] = Type::kSnow;
				if (result.snow < 0) {
					result.snow = static_cast<int8_t>(i);
				}

				under[i] = ClassifyName(TexturePath(textures[i]), true);
				continue;
			}

			result.layers[i] = ClassifyName(TexturePath(textures[i]));
			if (result.layers[i] == Type::kSnow && result.snow < 0) {
				result.snow = static_cast<int8_t>(i);
			}
		}

		if (result.snow >= 0) {
			for (const Type wanted : kRevealPreference) {
				const auto found =
					std::find(std::begin(result.layers), std::end(result.layers), wanted);
				if (found != std::end(result.layers)) {
					result.reveal =
						static_cast<int8_t>(std::distance(std::begin(result.layers), found));
					break;
				}
			}

			if (result.reveal < 0) {
				for (const Type wanted : kSnowRevealPreference) {
					auto* found = std::find(std::begin(under), std::end(under), wanted);
					if (found != std::end(under)) {
						result.reveal =
							static_cast<int8_t>(std::distance(std::begin(under), found));
						result.revealUnderSnow = true;
						break;
					}
				}
			}
		}

		LogClassification(result, textures);

		{
			const std::scoped_lock lock(g_cacheLock);
			if (g_cache.size() >= kMaxCacheEntries) {
				g_cache.clear();
			}
			g_cache[key] = result;
		}

		return result;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_cacheLock);
		g_cache.clear();
		g_loggedPlain.store(0);
		g_loggedSnow.store(0);
	}
}
