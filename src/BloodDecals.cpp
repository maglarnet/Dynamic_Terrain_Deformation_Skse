// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "BloodDecals.h"
#include "BloodDecalFilter.h"
#include "Settings.h"
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <array>

namespace BloodDecals
{
	namespace
	{

		std::unordered_map<RE::BSGeometry*, RE::NiPointer<RE::BSTempEffect>> targets;
		std::unordered_set<std::string> reported;
		std::unordered_set<std::string> reportedDrawTextures;
		bool reportedDraw{}, reportedSkip{}, reportedStart{};
		std::unordered_set<RE::BSTempEffect*> visited;
		// Decal nodes whose decals have been observed this frame, by Update() or by a draw.
		std::unordered_set<RE::BGSDecalNode*> observedNodes;
		std::array<size_t, 6> lastCounts{};
		std::chrono::steady_clock::time_point nextCensus{};
		const char* Diffuse(RE::BGSTextureSet* set)
		{
			return set ? set->textures[0].textureName.c_str() : "";
		}
		bool Blood(RE::BGSTextureSet* set)
		{
			const char* path = Diffuse(set);
			return path && BloodDecalFilter::Matches(path, Settings::bloodDecalTexturePrefixes);
		}
		bool Terrain(RE::BSGeometry* geometry)
		{
			if (!geometry || geometry->GetGeometryRuntimeData().skinInstance) { return false; }
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			auto* material = property ? property->GetBaseMaterial() : nullptr;
			return material && material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
		}
		void Report(std::string_view kind, RE::BGSTextureSet* set)
		{
			const char* path = Diffuse(set);
			const std::string key = std::string(kind) + ":" + (path ? path : "");
			if (reported.size() < 64 && reported.insert(key).second) {
				logger::info("Blood decals B4: {} texture={}", kind, path ? path : "");
			}
		}
		void Observe(RE::BSTempEffect* effect)
		{
			if (!effect || !visited.insert(effect).second) { return; }

			if (auto* decal = netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect)) {
				if (!Blood(decal->textureSet)) {
					Report("unmatched simple decal: native", decal->textureSet);
					return;
				}
				if (decal->textureSet2 && !Blood(decal->textureSet2)) {
					Report("unmatched secondary simple texture: native", decal->textureSet2);
					return;
				}

				if (!decal->effect3D || !Terrain(decal->avShape.get()) ||
					decal->effect3D->GetGeometryRuntimeData().skinInstance) {
					Report("simple blood on non-terrain or unresolved receiver: native", decal->textureSet);
					return;
				}
				targets.emplace(decal->effect3D.get(), RE::NiPointer<RE::BSTempEffect>(decal));
				Report("terrain simple blood geometry identified", decal->textureSet);
				return;
			}
			auto* decal = netimmerse_cast<RE::BSTempEffectGeometryDecal*>(effect);
			if (!decal) {
				const auto* rtti = effect->GetRTTI();
				const std::string key = "type:" + std::string(rtti ? rtti->GetName() : "unknown");
				if (reported.size() < 64 && reported.insert(key).second) {
					logger::info("Blood decals B4: other effect class={} type={} kept native", key, static_cast<int>(effect->GetType()));
				}
				return;
			}
			if (!Blood(decal->texSet)) {
				Report("unmatched geometry decal: native", decal->texSet);
				return;
			}
			if (decal->texSet2 && !Blood(decal->texSet2)) {
				Report("unmatched secondary geometry texture: native", decal->texSet2);
				return;
			}
			if (!decal->decal || !Terrain(decal->attachedGeometry.get()) ||
				decal->decal->GetGeometryRuntimeData().skinInstance) {
				Report("blood on non-terrain or unresolved receiver: native", decal->texSet);
				return;
			}
			targets.emplace(decal->decal.get(), RE::NiPointer<RE::BSTempEffect>(decal));
			Report("terrain blood geometry identified", decal->texSet);
		}
	}

	void Update()
	{
		targets.clear();
		visited.clear();
		observedNodes.clear();
		if (!Settings::enableBloodDecals || !Settings::enableTessellation || !Settings::useClipmap) { return; }
		if (!reportedStart) {
			reportedStart = true;
			logger::info("Blood decals B4 enabled: direct lists and attached nodes, prefixes={}", Settings::bloodDecalTexturePrefixes);
		}
		auto* manager = RE::BGSDecalManager::GetSingleton();
		if (!manager) {
			if (reported.insert("no-manager").second) { logger::info("Blood decals B4: decal manager unavailable"); }
			return;
		}
		for (const auto& effect : manager->decals) {
			Observe(effect.get());
		}
		for (const auto& decal : manager->simpleDecals) {
			Observe(decal.get());
		}
		size_t attached = 0;
		for (const auto& node : manager->decalNodes) {
			if (!node) { continue; }
			observedNodes.insert(node.get());
			const auto& effects = node->GetRuntimeData().decals;
			attached += effects.size();
			for (const auto& effect : effects) { Observe(effect.get()); }
		}
		const std::array<size_t, 6> counts{ manager->decals.size(), manager->simpleDecals.size(),
			manager->decalNodes.size(), attached, visited.size(), targets.size() };
		const auto now = std::chrono::steady_clock::now();
		if (now >= nextCensus && (counts != lastCounts || nextCensus.time_since_epoch().count() == 0)) {
			logger::info("Blood decals B4 census: direct={} simple={} nodes={} attached={} unique={} terrainTargets={}",
				counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
			lastCounts = counts;
			nextCensus = now + std::chrono::seconds(5);
		}
	}

	bool Contains(RE::BSGeometry* geometry)
	{
		if (!Settings::enableBloodDecals || !geometry) { return false; }
		if (targets.contains(geometry)) { return true; }
		// Observe() never makes skinned geometry a target, so a skinned decal draw can only
		// return false here. Skin decals on actors are most of the decal draws.
		if (geometry->GetGeometryRuntimeData().skinInstance) { return false; }
		auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property || !property->flags.any(Flag::kDecal, Flag::kDynamicDecal)) { return false; }

		auto* parent = geometry->parent;
		for (unsigned depth = 0; parent && depth < 8; ++depth, parent = parent->parent) {
			if (auto* node = netimmerse_cast<RE::BGSDecalNode*>(parent)) {
				// Once a frame per node: re-observing the whole node for every decal drawn under
				// it cost the square of its decal count. A decal attached after this frame's pass
				// is drawn natively until the next Update().
				if (!observedNodes.insert(node).second) { break; }
				for (const auto& effect : node->GetRuntimeData().decals) {
					visited.erase(effect.get());
					Observe(effect.get());
				}
				break;
			}
		}
		if (targets.contains(geometry)) { return true; }

		if (Settings::logDraws && reportedDrawTextures.size() < 32) {
			if (auto* texture = property->GetBaseTexture()) {
				const std::string path = texture->name.c_str();
				if (reportedDrawTextures.insert("draw:" + path).second) {
					logger::info("Blood decals B4: unregistered decal draw, texture={} prefixMatch={}; kept native",
						path, BloodDecalFilter::Matches(path, Settings::bloodDecalTexturePrefixes));
				}
			}
		}
		return false;
	}
	void NoteDraw(bool routed)
	{
		auto& once = routed ? reportedDraw : reportedSkip;
		if (!once) {
			once = true;
			logger::info("Blood decals B4: {}", routed ? "first terrain blood draw displaced" :
				"blood draw kept native: shader pending or unsupported pipeline");
		}
	}
	void Reset()
	{
		targets.clear(); reported.clear(); reportedDrawTextures.clear();
		visited.clear(); observedNodes.clear(); lastCounts = {}; nextCensus = {};
		reportedDraw = reportedSkip = reportedStart = false;
	}
}
