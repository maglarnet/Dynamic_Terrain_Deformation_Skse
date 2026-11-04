// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "BoxFilter.h"
#include "CoverageShelterCombine.h"
#include "SnowCoverage.h"

#include "Shelter.h"

#include "Clipmap.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShelterTransition.h"
#include "ShelterFadeCells.h"
#include "ShelterScan.h"
#include "ShelterProbeCache.h"
#include "RoadSnowMask.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include <atomic>
#include <unordered_set>

namespace Shelter
{
	namespace
	{
		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		std::vector<uint8_t> g_raw;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_cap;
		std::vector<uint8_t> g_capSmooth;
		std::vector<uint8_t> g_roads, g_roadExpanded;
		RoadSnowMask::Cache<SnowCoverage::kTexels> g_roadCache;
		std::vector<uint8_t> g_coverageRoadExpanded;
		std::vector<ShelterProbe::LandCell> g_roadLand;
		std::vector<double> g_roadRetryAt;
		struct RoadCell { int32_t x{}, y{}; };
		std::vector<RoadCell> g_roadCandidates;
		size_t g_roadCursor{};
		double g_roadScanAt{};
		RE::NiPoint3 g_roadPreviousPosition{};
		bool g_roadHavePosition{};
		uint32_t g_roadsLogged{};
		std::vector<float> g_roofDisplay, g_capDisplay;
		std::vector<uint8_t> g_roofVisible, g_capVisible;
		bool g_transition{ false };
		ShelterTransition::FadeCells g_fadeCells;

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };
		bool                      g_capDirty{ false };
		bool                      g_capUploadPending{ false };
		std::atomic<bool>          g_resetRequested{ false };

		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		uint32_t g_cursor{ 0 };
		uint32_t g_ageCursor{ 0 };
		std::vector<ShelterProbe::LandCell> g_landCache;
		ShelterProbe::Clock g_probeClock;
		bool g_probeSettled{ false };

		int32_t  g_centreCellX{ 0 };
		int32_t  g_centreCellY{ 0 };
		bool     g_dirty{ false };
		bool     g_haveCentre{ false };
		uint32_t g_revision{ 1 };

		bool g_allocated{ false };

		void Allocate()
		{
			if (g_allocated) {
				return;
			}

			const size_t total = static_cast<size_t>(kTexels) * kTexels;
			g_raw.assign(total, 0);
			g_fadeCells.Reset(static_cast<uint32_t>(total));
			g_smooth.assign(total, 0);
			g_cap.assign(total, 255);
			g_capSmooth.assign(total, 255);
			g_roads.assign(total, 0);
			g_roadExpanded.assign(total, 255);
			constexpr size_t roadTotal = SnowCoverage::kTexels * SnowCoverage::kTexels;
			g_coverageRoadExpanded.assign(roadTotal, 255);
			g_roadLand.resize(roadTotal);
			g_roadRetryAt.assign(roadTotal, 0.0);
			g_roofDisplay.assign(total, 0.0f);
			g_capDisplay.assign(total, 255.0f);
			g_roofVisible.assign(total, 0);
			g_capVisible.assign(total, 255);
			g_rowSums.assign(total, 0);
			g_filledX.assign(total, -0x40000000);
			g_filledY.assign(total, -0x40000000);
			g_landCache.assign(total, {});
			g_allocated = true;
		}

		bool IsActorHit(const RE::hkpCollidable* a_collidable)
		{
			if (!a_collidable) {
				return false;
			}

			switch (a_collidable->GetCollisionLayer()) {
			case RE::COL_LAYER::kCharController:
			case RE::COL_LAYER::kBiped:
			case RE::COL_LAYER::kBipedNoCC:
			case RE::COL_LAYER::kDeadBip:
				return true;
			default:
				return false;
			}
		}

		float RayHeight(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ,
			float a_fromAbove, float a_toAbove, const RE::hkpCollidable** a_hitObject = nullptr)
		{
			const float scale = RE::bhkWorld::GetWorldScale();

			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_worldX * scale, a_worldY * scale,
				(a_landZ + a_fromAbove) * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_worldX * scale, a_worldY * scale,
				(a_landZ + a_toAbove) * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

			a_tes->Pick(pick);
			if (!pick.rayOutput.HasHit() || IsActorHit(pick.rayOutput.rootCollidable)) {
				return -1.0f;
			}
			if (a_hitObject) { *a_hitObject = pick.rayOutput.rootCollidable; }

			return a_fromAbove +
				(a_toAbove - a_fromAbove) * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
		}

		bool Occluded(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ)
		{
			const float low = std::max(Settings::shelterClearance, 1.0f);
			return RayHeight(a_tes, a_worldX, a_worldY, a_landZ, low,
					   low + std::max(Settings::shelterHeight, 1.0f)) >= 0.0f;
		}

		uint8_t CapFor(RE::TES* a_tes, float a_worldX, float a_worldY, float a_landZ, bool& a_road)
		{
			const float raise = std::max(Settings::snowRaiseHeight, 1.0f);

			const float low = std::max(Settings::shelterFloorTolerance, 1.0f);
			const float high = raise + low;

			const RE::hkpCollidable* object = nullptr;
			const float hit = RayHeight(a_tes, a_worldX, a_worldY, a_landZ, high,
				Settings::excludeRoadSnow ? 0.0f : low, &object);
			a_road = false;
			if (Settings::excludeRoadSnow && object &&
				(object->GetCollisionLayer() == RE::COL_LAYER::kStatic ||
					object->GetCollisionLayer() == RE::COL_LAYER::kAnimStatic)) {
				auto* ref = RE::TESHavokUtilities::FindCollidableRef(*object);
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				auto* model = base ? base->As<RE::TESObjectSTAT>() : nullptr;
				const char* path = model ? model->GetModel() : nullptr;
				if (path && RoadSnowMask::IsRoadModel(path)) {
					a_road = true;
					if (g_roadsLogged < 16) {
						++g_roadsLogged;
						logger::info("Road snow exclusion: ref={:08X} model={} heightAboveLand={:.2f}",
							ref->GetFormID(), path, hit);
					}
					return 255;
				}
			}
			if (!object || hit < low) {
				return 255;
			}

			return static_cast<uint8_t>(
				std::clamp(hit / raise, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		uint8_t RoadCoverage(RE::TES* a_tes, int32_t a_cellX, int32_t a_cellY, float a_landZ, uint32_t& a_rays)
		{
			uint32_t hits = 1;
			for (const float ox : { 0.25f, 0.75f }) {
				for (const float oy : { 0.25f, 0.75f }) {
					const RE::NiPoint3 at{ (static_cast<float>(a_cellX) + ox) * kTexelSize,
						(static_cast<float>(a_cellY) + oy) * kTexelSize, a_landZ };
					float landZ = a_landZ;
					if (!a_tes->GetLandHeight(at, landZ)) { landZ = a_landZ; }
					bool road = false;
					CapFor(a_tes, at.x, at.y, landZ, road);
					++a_rays;
					hits += road ? 1u : 0u;
				}
			}
			return static_cast<uint8_t>((hits * 255u + 2u) / 5u);
		}

		void RememberRoad(int32_t x, int32_t y, uint8_t coverage)
		{
			if (g_roadCache.Add(x, y, coverage)) {
				++g_revision;
				g_capDirty = true;
			}
		}

		void DiscoverRoads(RE::TES* tes, RE::PlayerCharacter* player, const RE::NiPoint3& position, float delta)
		{
			if (!Settings::excludeRoadSnow || !Settings::shelterMeshCap || delta <= 0) { return; }
			auto ahead = position;
			if (g_roadHavePosition) {
				const float dx = position.x - g_roadPreviousPosition.x, dy = position.y - g_roadPreviousPosition.y;
				const float distance = std::hypot(dx, dy);
				if (distance > 0.01f && distance < 256.0f) {
					const float lead = std::min(distance / delta * 2.0f, 1536.0f);
					ahead.x += dx / distance * lead; ahead.y += dy / distance * lead;
				}
			}
			g_roadPreviousPosition = position; g_roadHavePosition = true;
			if (g_roadCursor >= g_roadCandidates.size() && g_probeClock.now >= g_roadScanAt) {
				g_roadCandidates.clear(); g_roadCursor = 0;
				g_roadScanAt = g_probeClock.now + 0.5;
				std::unordered_set<uint64_t> queued;
				constexpr size_t limit = 8192;
				tes->ForEachReferenceInRange(player, 6144.0f, [&](RE::TESObjectREFR* ref) {
					if (g_roadCandidates.size() >= limit) { return RE::BSContainer::ForEachResult::kStop; }
					if (!ref || ref->IsDisabled() || ref->IsMarkedForDeletion()) { return RE::BSContainer::ForEachResult::kContinue; }
					auto* base = ref->GetBaseObject();
					auto* model = base ? base->As<RE::TESObjectSTAT>() : nullptr;
					const char* path = model ? model->GetModel() : nullptr;
					auto* root = ref->Get3D();
					if (!path || !root || !RoadSnowMask::IsRoadModel(path)) { return RE::BSContainer::ForEachResult::kContinue; }
					const auto& bound = root->worldBound;
					if (!std::isfinite(bound.radius) || bound.radius <= 0 || bound.radius > 2048.0f ||
						!std::isfinite(bound.center.x) || !std::isfinite(bound.center.y)) { return RE::BSContainer::ForEachResult::kContinue; }
					const int32_t x0 = static_cast<int32_t>(std::floor((bound.center.x - bound.radius) / kTexelSize));
					const int32_t x1 = static_cast<int32_t>(std::floor((bound.center.x + bound.radius) / kTexelSize));
					const int32_t y0 = static_cast<int32_t>(std::floor((bound.center.y - bound.radius) / kTexelSize));
					const int32_t y1 = static_cast<int32_t>(std::floor((bound.center.y + bound.radius) / kTexelSize));
					for (int32_t y = y0; y <= y1 && g_roadCandidates.size() < limit; ++y) {
						for (int32_t x = x0; x <= x1 && g_roadCandidates.size() < limit; ++x) {
							if (g_roadCache.Contains(x, y)) { continue; }
							const auto i = RoadSnowMask::Cache<SnowCoverage::kTexels>::Index(x, y);
							if (g_roadLand[i].Matches(x, y) && g_roadRetryAt[i] > g_probeClock.now) { continue; }
							const uint64_t key = (uint64_t{static_cast<uint32_t>(x)} << 32) | static_cast<uint32_t>(y);
							if (queued.insert(key).second) { g_roadCandidates.push_back({x, y}); }
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				std::sort(g_roadCandidates.begin(), g_roadCandidates.end(), [&](const RoadCell& a, const RoadCell& b) {
					const auto distance = [&](const RoadCell& c) {
						const float x = (c.x + 0.5f) * kTexelSize - ahead.x, y = (c.y + 0.5f) * kTexelSize - ahead.y;
						return x * x + y * y;
					};
					return distance(a) < distance(b);
				});
			}
			uint32_t attempts = 0, rays = 0;
			const auto budget = static_cast<uint32_t>(Settings::roadSnowProbeBudget);
			while (g_roadCursor < g_roadCandidates.size() && attempts < budget && rays < budget) {
				const auto cell = g_roadCandidates[g_roadCursor++];
				if (g_roadCache.Contains(cell.x, cell.y)) { continue; }
				++attempts;
				RE::NiPoint3 probe{(cell.x + 0.5f) * kTexelSize, (cell.y + 0.5f) * kTexelSize, position.z};
				const auto i = RoadSnowMask::Cache<SnowCoverage::kTexels>::Index(cell.x, cell.y);
				auto& land = g_roadLand[i];
				float landZ{};
				if (!land.Get(cell.x, cell.y, landZ)) {
					Profiler::Tally(Profiler::Count::kShelterLandAttempts);
					if (!tes->GetLandHeight(probe, landZ)) {
						land.Miss(cell.x, cell.y, g_probeClock.now);
						g_roadRetryAt[i] = g_probeClock.now + 0.5;
						continue;
					}
					land.Store(cell.x, cell.y, landZ);
				}
				bool road = false;
				CapFor(tes, probe.x, probe.y, landZ, road); ++rays;
				g_roadRetryAt[i] = g_probeClock.now + 2.0;
				if (road) { RememberRoad(cell.x, cell.y, RoadCoverage(tes, cell.x, cell.y, landZ, rays)); }
			}
			Profiler::Tally(Profiler::Count::kShelterRays, rays);
		}

		void Smooth(const std::vector<uint8_t>& a_src, std::vector<uint8_t>& a_dst)
		{
			const int n = static_cast<int>(kTexels);
			const int r = std::clamp(Settings::shelterSeamTexels, 0, n / 4);
			if (r <= 0) {
				a_dst = a_src;
				return;
			}

			const auto started = Profiler::Ticks();
			BoxFilter::Apply<kTexels>(a_src.data(), a_dst.data(), g_rowSums.data(), r);
			Profiler::AddCpuTicks(Profiler::CpuScope::kShelterFilter, Profiler::Ticks() - started);
		}
	}

	bool Ready()
	{
		return g_texture && g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return false;
		}

		Allocate();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTexels;
		desc.Height = kTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = g_capSmooth.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			g_failed = true;
			logger::warn("Shelter: CreateTexture2D failed - the mesh cap is off");
			return false;
		}

		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			g_failed = true;
			logger::warn("Shelter: CreateShaderResourceView failed - the mesh cap is off");
			return false;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			g_failed = true;
			logger::warn("Shelter: CreateSamplerState failed - the mesh cap is off");
			return false;
		}

		logger::info("Shelter ready: {}x{} texels over {} world units ({} per texel); 0.35s temporal response",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	ID3D11ShaderResourceView* View()
	{
		return Ready() ? g_srv : nullptr;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !Ready()) {
			return;
		}
		a_context->DSSetShaderResources(kSlot, 1, &g_srv);
		a_context->DSSetSamplers(kSamplerSlot, 1, &g_sampler);
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context) {
			return;
		}
		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
	}

	void Shutdown()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_srv);
		drop(g_sampler);
		drop(g_texture);
		g_failed = false;

		g_roofDisplay.clear();
		g_capDisplay.clear();
		g_roofVisible.clear();
		g_capVisible.clear();
		g_transition = false;
		g_fadeCells.Reset(0);
		g_cap.clear();
		g_capSmooth.clear();
		g_roads.clear();
		g_roadExpanded.clear();
		g_coverageRoadExpanded.clear();
		g_roadLand.clear();
		g_roadRetryAt.clear();
		g_roadCandidates.clear();
		g_roadCache.Reset();
		g_roadCursor = 0; g_roadScanAt = 0; g_roadHavePosition = false;
		g_raw.clear();
		g_smooth.clear();
		g_rowSums.clear();
		g_filledX.clear();
		g_filledY.clear();
		g_landCache.clear();
		g_probeClock = {};
		g_probeSettled = false;
		g_allocated = false;
		g_haveCentre = false;
		g_dirty = false;
		g_capUploadPending = false;
		++g_revision;
	}

	void Reset()
	{
		if (!g_allocated) {
			return;
		}

		std::fill(g_filledX.begin(), g_filledX.end(), -0x40000000);
		std::fill(g_filledY.begin(), g_filledY.end(), -0x40000000);
		g_cursor = 0;
		g_ageCursor = 0;
		std::fill(g_landCache.begin(), g_landCache.end(), ShelterProbe::LandCell{});
		g_probeClock = {};
		g_probeSettled = false;

		std::fill(g_cap.begin(), g_cap.end(), static_cast<uint8_t>(255));
		std::fill(g_roads.begin(), g_roads.end(), uint8_t{0});
		g_roadCache.Reset();
		std::fill(g_roadLand.begin(), g_roadLand.end(), ShelterProbe::LandCell{});
		std::fill(g_roadRetryAt.begin(), g_roadRetryAt.end(), 0.0);
		g_roadCandidates.clear();
		g_roadCursor = 0; g_roadScanAt = 0; g_roadHavePosition = false;
		g_roadsLogged = 0;
		std::fill(g_raw.begin(), g_raw.end(), static_cast<uint8_t>(0));

		g_transition = true;
		g_fadeCells.ActivateAll();
		g_capDirty = true;
	}

	void ForgetWindow()
	{
		Reset();
		if (!g_allocated) { return; }
		std::fill(g_smooth.begin(), g_smooth.end(), 0);
		std::fill(g_roofDisplay.begin(), g_roofDisplay.end(), 0.0f);
		std::fill(g_roofVisible.begin(), g_roofVisible.end(), 0);
		std::fill(g_capSmooth.begin(), g_capSmooth.end(), 255);
		std::fill(g_capDisplay.begin(), g_capDisplay.end(), 255.0f);
		std::fill(g_capVisible.begin(), g_capVisible.end(), 255);
		g_fadeCells.Reset(static_cast<uint32_t>(g_raw.size()));
		g_transition = false;
		g_haveCentre = false;
		g_dirty = false;
		g_capUploadPending = true;
		++g_revision;
	}

	uint32_t Revision()
	{
		return g_revision;
	}

	float CapAt(float a_worldX, float a_worldY)
	{

		if (!Settings::enableShelter || !Settings::shelterMeshCap || !g_allocated ||
			!g_haveCentre || g_capVisible.empty()) {
			return 1.0f;
		}

		const float u = (a_worldX / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const float v = (a_worldY / kWorldSize) * static_cast<float>(kTexels) - 0.5f;

		const float u0f = std::floor(u);
		const float v0f = std::floor(v);

		const auto warp = [](float a_f) { return a_f * a_f * (3.0f - 2.0f * a_f); };
		const float fx = warp(u - u0f);
		const float fy = warp(v - v0f);

		const uint32_t x0 = static_cast<uint32_t>(static_cast<int32_t>(u0f)) & kMask;
		const uint32_t y0 = static_cast<uint32_t>(static_cast<int32_t>(v0f)) & kMask;
		const uint32_t x1 = (x0 + 1) & kMask;
		const uint32_t y1 = (y0 + 1) & kMask;

		const auto texel = [](uint32_t a_x, uint32_t a_y) {
			return static_cast<float>(
					   g_capSmooth[(static_cast<size_t>(a_y) * kTexels) + a_x]) /
			       255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		float       cap = top + (bottom - top) * fy;

		float centreX = 0.0f, centreY = 0.0f, halfExtent = 0.0f;
		if (Clipmap::GetWindow(centreX, centreY, halfExtent)) {
			const float reach = std::max(std::fabs(a_worldX - centreX),
				std::fabs(a_worldY - centreY));
			const float start = kWorldSize * 0.39f;
			const float end = kWorldSize * 0.47f;
			const float release =
				std::clamp((reach - start) / std::max(end - start, 1e-3f), 0.0f, 1.0f);
			cap = cap + (1.0f - cap) * release;
		}

		return std::clamp(cap, 0.0f, 1.0f);
	}

	float AtCell(int32_t a_cellX, int32_t a_cellY)
	{
		if (!Settings::enableShelter || !g_allocated || !g_haveCentre) {
			return 0.0f;
		}

		const int32_t half = static_cast<int32_t>(kTexels / 2);
		if (std::abs(a_cellX - g_centreCellX) >= half ||
			std::abs(a_cellY - g_centreCellY) >= half) {
			return 0.0f;
		}

		const uint32_t tx = static_cast<uint32_t>(a_cellX) & kMask;
		const uint32_t ty = static_cast<uint32_t>(a_cellY) & kMask;
		return static_cast<float>(g_smooth[(static_cast<size_t>(ty) * kTexels) + tx]) *
			(1.0f / 255.0f);
	}

	uint32_t CombineCoverage(const uint8_t* source, uint8_t* output, int32_t baseX, int32_t baseY)
	{
		static_assert(kTexelSize == SnowCoverage::kTexelSize);
		const auto* roof = Settings::enableShelter && g_allocated && g_haveCentre ? g_smooth.data() : nullptr;
		const auto scaled = CoverageShelter::Combine<SnowCoverage::kTexels, kTexels>(source, output,
			baseX, baseY, roof, g_centreCellX, g_centreCellY);
		if (Settings::enableShelter && Settings::shelterMeshCap && Settings::excludeRoadSnow && g_allocated && g_haveCentre) {
			constexpr uint32_t n = SnowCoverage::kTexels, mask = n - 1;
			RoadSnowMask::ApplyToCoverage<n>(g_roadCache.Data(), output, g_coverageRoadExpanded.data(),
				Settings::roadSnowMarginTexels, Settings::roadSnowSeamTexels,
				[&](uint32_t i) {
					const uint32_t tx = i & mask, ty = i / n;
					const int32_t x = baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & mask);
					const int32_t y = baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & mask);
					return g_roadCache.Contains(x, y);
				});
		}
		return scaled;
	}

	void InvalidateCPU()
	{
		g_resetRequested.store(true, std::memory_order_release);
	}

	void UpdateCPU()
	{
		if (g_resetRequested.exchange(false, std::memory_order_acq_rel)) { ForgetWindow(); }
		if (!Settings::enableShelter || !Settings::enableSnowRaise) {
			return;
		}

		auto* player = globals::game::player;
		auto* tes = RE::TES::GetSingleton();
		if (!player || !tes) {
			return;
		}

		Allocate();

		const int64_t started = Profiler::Ticks();
		Profiler::Tally(Profiler::Count::kShelterCPUUpdates);

		const RE::NiPoint3 position = player->GetPosition();

		const int32_t wasX = g_centreCellX;
		const int32_t wasY = g_centreCellY;

		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		const bool moved = !g_haveCentre || wasX != g_centreCellX || wasY != g_centreCellY;
		if (moved) {
			++g_revision;
			g_probeSettled = false;
			g_capDirty = true;
		}
		g_haveCentre = true;

		const int32_t baseX = g_centreCellX - static_cast<int32_t>(kTexels / 2);
		const int32_t baseY = g_centreCellY - static_cast<int32_t>(kTexels / 2);

		const uint32_t total = static_cast<uint32_t>(g_raw.size());

		const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
		const float delta = !paused && globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
		const auto refresh = g_probeClock.Advance(delta,
			static_cast<uint32_t>(std::max(Settings::shelterRefresh, 0)), total);
		DiscoverRoads(tes, player, position, !paused && delta > 0 && delta < 0.25f ? delta : 0.0f);
		for (uint32_t i = 0; i < refresh.cells; ++i) {
			g_filledX[g_ageCursor] = -0x40000000;
			g_ageCursor = (g_ageCursor + 1) % total;
		}

		const uint32_t budget = static_cast<uint32_t>(std::max(Settings::shelterBudget, 1));

		static uint32_t reportedRays = 0, roofChanges = 0, capChanges = 0;
		const uint32_t raysPerCell = Settings::shelterMeshCap && !g_failed ? 2u : 1u;
		ShelterScan::Counts scan{};
		uint32_t landQueries = 0, cachedHeights = 0, roadRays = 0;
		if (!g_probeSettled || refresh.due) {
			scan = ShelterScan::Run(g_cursor, total, budget, raysPerCell, [&](uint32_t index) {
			const uint32_t tx = index & kMask;
			const uint32_t ty = index / kTexels;

			const int32_t cellX =
				baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			const int32_t cellY =
				baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);

			if (g_filledX[index] == cellX && g_filledY[index] == cellY) {
				return ShelterScan::Result::kCached;
			}

			const RE::NiPoint3 probe{
				(static_cast<float>(cellX) + 0.5f) * kTexelSize,
				(static_cast<float>(cellY) + 0.5f) * kTexelSize,
				position.z
			};

			auto& land = g_landCache[index];
			if (!land.CanRetry(cellX, cellY, g_probeClock.now)) {
				return ShelterScan::Result::kCached;
			}
			float landZ = 0.0f;
			if (!land.Get(cellX, cellY, landZ)) {
				++landQueries;
				if (!tes->GetLandHeight(probe, landZ)) {
					land.Miss(cellX, cellY, g_probeClock.now);
					return ShelterScan::Result::kNoLand;
				}
				land.Store(cellX, cellY, landZ);
			} else {
				++cachedHeights;
			}

			const bool occluded = Occluded(tes, probe.x, probe.y, landZ);

			const uint8_t value = occluded ? 255 : 0;
			if (g_raw[index] != value) {
				g_raw[index] = value;
				++roofChanges;
				g_fadeCells.Activate(index);
				g_transition = true;
			}

			if (raysPerCell == 2) {
				bool road = false;
				const uint8_t cap = CapFor(tes, probe.x, probe.y, landZ, road);
				if (road && !g_roadCache.Contains(cellX, cellY)) {
					RememberRoad(cellX, cellY, RoadCoverage(tes, cellX, cellY, landZ, roadRays));
				}

				if (g_cap[index] != cap) {
					g_cap[index] = cap;
					++capChanges;
					g_fadeCells.Activate(index);
					g_transition = true;
				}
			}

			g_filledX[index] = cellX;
			g_filledY[index] = cellY;
			return ShelterScan::Result::kUpdated;
		});
			g_probeSettled = scan.scanned == total;
		}
		Profiler::Tally(Profiler::Count::kShelterLandAttempts, landQueries);
		Profiler::Tally(Profiler::Count::kShelterLandCached, cachedHeights);
		Profiler::Tally(Profiler::Count::kShelterLandMisses, scan.misses);
		Profiler::Tally(Profiler::Count::kShelterRays, scan.rays + roadRays);

		if (g_transition) {
			const float alpha = ShelterTransition::Alpha(delta);
			bool pending = false;
			const auto visited = g_fadeCells.Advance([&](uint32_t i) {
				bool cellPending = ShelterTransition::Advance(g_roofDisplay[i], g_raw[i], alpha);
				cellPending |= ShelterTransition::Advance(g_capDisplay[i], g_cap[i], alpha);
				pending |= cellPending;
				const auto roof = static_cast<uint8_t>(g_roofDisplay[i] + 0.5f);
				const auto cap = static_cast<uint8_t>(g_capDisplay[i] + 0.5f);
				g_dirty |= roof != g_roofVisible[i];
				g_capDirty |= cap != g_capVisible[i];
				g_roofVisible[i] = roof;
				g_capVisible[i] = cap;
				return cellPending;
			});
			Profiler::Tally(Profiler::Count::kShelterFadeVisited, visited);
			Profiler::Tally(Profiler::Count::kShelterFadeFull, total);
			g_transition = pending;
		}

		if (g_dirty) {
			Smooth(g_roofVisible, g_smooth);
			g_dirty = false;
			++g_revision;
		}

		if (g_capDirty && Settings::shelterMeshCap && !g_failed) {
			Smooth(g_capVisible, g_capSmooth);
			if (Settings::excludeRoadSnow) {
				for (uint32_t i = 0; i < total; ++i) {
					const uint32_t tx = i & kMask, ty = i / kTexels;
					const int32_t x = baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
					const int32_t y = baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);
					g_roads[i] = g_roadCache.Coverage(x, y);
				}
				RoadSnowMask::Apply<kTexels>(g_roads.data(), g_capSmooth.data(),
					g_roadExpanded.data(), Settings::roadSnowMarginTexels, Settings::roadSnowSeamTexels,
					[](uint32_t) { return true; });
			}
			g_capDirty = false;
			g_capUploadPending = true;
		}

		reportedRays += scan.rays;
		static auto reportAt = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (now - reportAt >= std::chrono::seconds(5)) {
			if (Settings::logSnowCoverage) {
				logger::info("Shelter transitions: {} rays, {} roof target changes, {} cap target changes; blending={}",
					reportedRays, roofChanges, capChanges, g_transition);
			}
			reportedRays = roofChanges = capChanges = 0;
			reportAt = now;
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kShelter, Profiler::Ticks() - started);
	}

	void Upload()
	{
		if (g_resetRequested.exchange(false, std::memory_order_acq_rel)) { ForgetWindow(); }
		if (!Settings::enableShelter || !Settings::enableSnowRaise || !Settings::shelterMeshCap) { return; }
		const auto started = Profiler::Ticks();
		if (Initialize() && g_capUploadPending) {
			auto* context = globals::d3d::context;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (context && SUCCEEDED(context->Map(g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_capSmooth.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_capUploadPending = false;
				Profiler::Tally(Profiler::Count::kShelterUploads);
			}
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kShelterUpload, Profiler::Ticks() - started);
	}
}
