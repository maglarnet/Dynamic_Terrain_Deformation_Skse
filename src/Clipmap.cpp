// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ActorShapes.h"
#include "Clipmap.h"

#include "ClipmapUpdateCS.h"
#include "ClipmapMetadata.h"
#include "Globals.h"
#include "HeatSources.h"
#include "MagicImpacts.h"
#include "ObjectStamps.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>

namespace Clipmap
{
	namespace
	{

		struct WindowCB
		{
			float centreAndFade[4]{};

			float raise[4]{};

			float window1[4]{};
		};
		static_assert(sizeof(WindowCB) % 16 == 0);

		struct ParamsCB
		{
			float window[4]{};
			float control[4]{};
			float weather[4]{};
			float stamps[kMaxStamps][4]{};
			float stampParams[kMaxStamps][4]{};
			float stampShape[kMaxStamps][4]{};

			float stampMotion[kMaxStamps][4]{};

			float coarse[4]{};

			float rimShape[4]{};
			float snowRim[4]{};

			float raise[4]{};

			float raiseWindow[4]{};
			float stampBounds[kMaxStamps][4]{};
		};
		static_assert(sizeof(ParamsCB) % 16 == 0);
		static_assert(offsetof(ParamsCB, stampBounds) == (4 * kMaxStamps + 8) * 16);
		static_assert(sizeof(ParamsCB) == (5 * kMaxStamps + 8) * 16);

		ID3D11Texture2D*           g_texture[kMaxLevels]{};
		ID3D11Texture2D* g_nextField{};
		ID3D11ShaderResourceView* g_nextFieldSRV{};
		ID3D11UnorderedAccessView* g_nextFieldUAV{};
		ID3D11ShaderResourceView*  g_srv[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_uav[kMaxLevels]{};

		ID3D11Texture2D*           g_decayTexture[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_decayUAV[kMaxLevels]{};

		ID3D11ShaderResourceView*  g_decaySRV[kMaxLevels]{};

		ID3D11Texture2D*           g_activity[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_activityUAV[kMaxLevels]{};
		ID3D11ShaderResourceView*  g_activitySRV[kMaxLevels]{};
		ID3D11SamplerState*        g_sampler{ nullptr };
		ID3D11ComputeShader*       g_updateCS{ nullptr };
		ID3D11Buffer*              g_paramsCB{ nullptr };
		ID3D11Buffer*              g_windowCB{ nullptr };
		bool                       g_failed{ false };

		int32_t g_prevWindowX[kMaxLevels]{};
		int32_t g_prevWindowY[kMaxLevels]{};
		bool    g_prevWindowValid[kMaxLevels]{};

		float g_windowCentreX{ 0.0f };
		float g_windowCentreY{ 0.0f };
		float g_windowHalfExtent{ 0.0f };
		bool  g_windowValid{ false };

		struct ComputeStageGuard
		{
			explicit ComputeStageGuard(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_context->CSGetShader(&_shader, nullptr, nullptr);
				_context->CSGetConstantBuffers(0, 1, &_cb);
				_context->CSGetUnorderedAccessViews(0, 3, _uav);
				_context->CSGetShaderResources(0, 6, _srv);
				_context->CSGetSamplers(0, 1, &_sampler);
			}

			~ComputeStageGuard()
			{
				const UINT noOffset[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1),
					static_cast<UINT>(-1) };
				_context->CSSetShader(_shader, nullptr, 0);
				_context->CSSetConstantBuffers(0, 1, &_cb);
				_context->CSSetUnorderedAccessViews(0, 3, _uav, noOffset);
				_context->CSSetShaderResources(0, 6, _srv);
				_context->CSSetSamplers(0, 1, &_sampler);

				if (_shader) {
					_shader->Release();
				}
				if (_cb) {
					_cb->Release();
				}
				for (auto*& srv : _srv) {
					if (srv) {
						srv->Release();
					}
				}
				if (_sampler) {
					_sampler->Release();
				}
				for (auto*& uav : _uav) {
					if (uav) {
						uav->Release();
					}
				}
			}

			ComputeStageGuard(const ComputeStageGuard&) = delete;
			ComputeStageGuard& operator=(const ComputeStageGuard&) = delete;

		private:
			ID3D11DeviceContext*       _context;

			ID3D11ShaderResourceView*  _srv[6]{};
			ID3D11SamplerState*        _sampler{ nullptr };
			ID3D11ComputeShader*       _shader{ nullptr };
			ID3D11Buffer*              _cb{ nullptr };
			ID3D11UnorderedAccessView* _uav[3]{};
		};

		bool CreateLevel(ID3D11Device* a_device, uint32_t a_level)
		{
			if (a_level >= kMaxLevels) {
				return false;
			}
			if (g_srv[a_level] && g_uav[a_level] && g_decayUAV[a_level] && g_decaySRV[a_level] &&
				g_activityUAV[a_level] && g_activitySRV[a_level]) {
				return true;
			}

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTexels;
			desc.Height = kTexels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			if (!g_nextField) {
				if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_nextField)) ||
					FAILED(a_device->CreateShaderResourceView(g_nextField, nullptr, &g_nextFieldSRV)) ||
					FAILED(a_device->CreateUnorderedAccessView(g_nextField, nullptr, &g_nextFieldUAV))) {
					logger::error("Clipmap: spare height field allocation failed");
					return false;
				}
			}
			const std::vector<float> zeros(static_cast<size_t>(kTexels) * kTexels, 0.0f);
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = zeros.data();
			initial.SysMemPitch = kTexels * sizeof(float);

			if (FAILED(a_device->CreateTexture2D(&desc, &initial, &g_texture[a_level]))) {
				logger::error("Clipmap: CreateTexture2D failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_texture[a_level], nullptr, &g_srv[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_texture[a_level], nullptr, &g_uav[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView failed for level {}", a_level);
				return false;
			}

			D3D11_TEXTURE2D_DESC decayDesc = desc;
			decayDesc.Format = kMetadataFormat;

			decayDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

			const std::vector<MetadataChannel> decayZeros(static_cast<size_t>(kTexels) * kTexels * 2, 0);
			D3D11_SUBRESOURCE_DATA decayInitial{};
			decayInitial.pSysMem = decayZeros.data();
			decayInitial.SysMemPitch = kTexels * kMetadataPixelBytes;

			if (FAILED(a_device->CreateTexture2D(
					&decayDesc, &decayInitial, &g_decayTexture[a_level]))) {
				logger::error("Clipmap: CreateTexture2D (decay) failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_decayTexture[a_level], nullptr, &g_decayUAV[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView (decay) failed for level {}",
					a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_decayTexture[a_level], nullptr, &g_decaySRV[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView (decay) failed for level {}",
					a_level);
				return false;
			}

			D3D11_TEXTURE2D_DESC activityDesc{};
			activityDesc.Width = kActivityTexels;
			activityDesc.Height = kActivityTexels;
			activityDesc.MipLevels = 1;
			activityDesc.ArraySize = 1;
			activityDesc.Format = DXGI_FORMAT_R32_UINT;
			activityDesc.SampleDesc.Count = 1;
			activityDesc.Usage = D3D11_USAGE_DEFAULT;
			activityDesc.BindFlags =
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			if (FAILED(a_device->CreateTexture2D(&activityDesc, nullptr, &g_activity[a_level]))) {
				logger::error("Clipmap: CreateTexture2D (activity) failed for level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_activity[a_level], nullptr, &g_activityUAV[a_level]))) {
				logger::error("Clipmap: CreateUnorderedAccessView (activity) failed for "
							  "level {}", a_level);
				return false;
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_activity[a_level], nullptr, &g_activitySRV[a_level]))) {
				logger::error("Clipmap: CreateShaderResourceView (activity) failed for "
							  "level {}", a_level);
				return false;
			}

			logger::info("Clipmap level {}: {}x{} texels over {:.0f} world units "
						 "({:.2f} per cell, +/- {:.1f} m)",
				a_level, kTexels, kTexels, WorldSizeFor(a_level), CellSizeFor(a_level),
				WorldSizeFor(a_level) * 0.5f / 70.0f);
			return true;
		}

		bool CompileUpdateShader(ID3D11Device* a_device)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;

			const std::string source = UpdateShaderSource();

			const std::string maxStamps = std::to_string(kMaxStamps);
			const D3D_SHADER_MACRO defines[] = {
				{ "MAX_STAMPS", maxStamps.c_str() },
				{ nullptr, nullptr }
			};

			const HRESULT hr = ::D3DCompile(
				source.c_str(), source.size(), "ClipmapUpdateCS", defines, nullptr,
				"main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

			if (FAILED(hr)) {
				logger::error("Clipmap update CS failed to compile: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}

			const HRESULT createHr = a_device->CreateComputeShader(
				code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_updateCS);
			code->Release();

			if (FAILED(createHr)) {
				logger::error("Clipmap: CreateComputeShader failed");
				return false;
			}
			return true;
		}

		void CollectActors(std::vector<RE::ActorPtr>& a_out)
		{
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					auto actor = handle.get();
					if (actor && actor.get() && actor->Is3DLoaded()) {
						a_out.push_back(actor);
					}
				}
			}

			if (auto* player = globals::game::player) {
				if (auto handle = player->GetHandle().get()) {
					a_out.push_back(handle);
				}
			}
		}

		std::atomic<uint32_t> g_surfacesLogged{ 0 };

		std::atomic<uint32_t> g_sizesLogged{ 0 };
		constexpr uint32_t    kMaxSizesLogged = 4;

		void LogStampSize(const Stamp& a_stamp)
		{
			if (!Settings::logStampShape || a_stamp.kind != Stamp::Kind::kPrint) {
				return;
			}
			if (g_sizesLogged.load() >= kMaxSizesLogged ||
				g_sizesLogged.fetch_add(1) >= kMaxSizesLogged) {
				return;
			}

			const float length = a_stamp.radius * 2.0f;
			const float width = a_stamp.halfWidth * 2.0f;

			const float vertexSpacing = std::max(Settings::tessellationTargetSpacing, 0.01f);

			logger::info("Print size: {:.1f} x {:.1f} world units | {:.0f} x {:.0f} field "
						 "texels at {:.2f}/cell | about {:.1f} x {:.1f} GENERATED VERTICES "
						 "at spacing {:.1f}",
				length, width, length / kCellSize, width / kCellSize, kCellSize,
				length / vertexSpacing, width / vertexSpacing,
				Settings::tessellationTargetSpacing);

			if (length / vertexSpacing < 8.0f) {
				logger::warn("  that is too few vertices to carry a silhouette - the mark "
							 "will read as a smooth blob whatever the mask contains. The "
							 "field holds {:.0f}x more detail than the geometry samples. "
							 "Lower TessellationTargetSpacing (and raise "
							 "TessellationMaxFactor with it, or the cap just clamps it "
							 "back).",
					vertexSpacing / kCellSize);
			}
		}

		std::atomic<uint32_t> g_feetLogged{ 0 };
		constexpr uint32_t    kMaxFeetLogged = 40;

		void LogFootShape(float a_side, float a_radius, float a_z, float a_landZ, bool a_isFoot)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (g_feetLogged.load() >= kMaxFeetLogged ||
				g_feetLogged.fetch_add(1) >= kMaxFeetLogged) {
				return;
			}

			logger::info("Foot test: side {:+7.2f} vs separation {:.2f} -> {:<6} | "
						 "shape radius {:5.2f} | bottom {:+7.2f} above land",
				a_side, Settings::stampFootSeparation, a_isFoot ? "PRINT" : "circle",
				a_radius, (a_z - a_radius) - a_landZ);
		}

		void LogStampSurface(const Surfaces::Ground& a_ground, const RE::NiPoint3& a_position)
		{
			if (!Settings::logStampSurfaces) {
				return;
			}

			const auto index = a_ground.profile >= 0 ?
								   static_cast<uint32_t>(a_ground.profile) +
									   static_cast<uint32_t>(Surfaces::Type::kCount) :
								   static_cast<uint32_t>(a_ground.type);
			const uint32_t bit = 1u << (index < 32u ? index : static_cast<uint32_t>(a_ground.type));
			if (g_surfacesLogged.fetch_or(bit) & bit) {
				return;
			}

			const auto* profile = Surfaces::ProfileName(a_ground.profile);
			const auto& response = a_ground.response;
			logger::info(
				"Stamp surface at ({:.0f}, {:.0f}): {:<8} depth x{:.2f} radius x{:.2f} "
				"shoulder {:.2f} decay x{:.2f}{}",
				a_position.x, a_position.y, Surfaces::Name(a_ground.type),
				response.depthScale, response.radiusScale, response.shoulder,
				response.decayScale,
				profile ? std::format("  <- profile [{}]", profile) :
						  std::string{ "  (no profile - keyword list or material id)" });
		}

		struct TrackedActor
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };
		};

		std::unordered_map<RE::FormID, TrackedActor> g_actorMotion;
		uint64_t                                     g_actorFrame{ 0 };

		constexpr float kMaxActorMotion = 48.0f;

		void TrackActorMotion(RE::FormID a_form, const RE::NiPoint3& a_position,
			float& a_motionX, float& a_motionY)
		{
			a_motionX = 0.0f;
			a_motionY = 0.0f;

			const auto previous = g_actorMotion.find(a_form);
			if (previous != g_actorMotion.end()) {
				const float dx = a_position.x - previous->second.x;
				const float dy = a_position.y - previous->second.y;
				if (dx * dx + dy * dy <= kMaxActorMotion * kMaxActorMotion) {
					a_motionX = dx;
					a_motionY = dy;
				}
			}

			g_actorMotion[a_form] = { a_position.x, a_position.y, g_actorFrame };
		}

		void AppendActorStamps(RE::Actor* a_actor, const RE::NiPoint3& a_eye,
			float a_maxDistanceSq, std::vector<Stamp>& a_out)
		{
			if (!a_actor || a_out.size() >= kMaxStamps) {
				return;
			}

			if (a_actor->IsOnMount()) {
				return;
			}

			auto* root = a_actor->Get3D(false);
			if (!root) {
				return;
			}

			const RE::NiPoint3 position = a_actor->GetPosition();
			if (a_eye.GetSquaredDistance(position) > a_maxDistanceSq) {
				return;
			}

			if (Clipmap::Stamp torch{}; HeatSources::ForActor(a_actor, torch)) {
				a_out.push_back(torch);
				if (a_out.size() >= kMaxStamps) {
					return;
				}
			}

			const auto  ground = Surfaces::GroundAt(position);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			LogStampSurface(ground, position);

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				return;
			}

			const float feetZ = position.z;
			const float reach = Settings::stampFootReach;

			const float  heading = a_actor->GetAngleZ();
			const float  headingSin = std::sin(heading);
			const float  headingCos = std::cos(heading);
			const RE::NiPoint3 forward{ headingSin, headingCos, 0.0f };
			const RE::NiPoint3 right{ headingCos, -headingSin, 0.0f };

			float motionX = 0.0f;
			float motionY = 0.0f;
			TrackActorMotion(a_actor->GetFormID(), position, motionX, motionY);

			if (SnowSparkle::Enabled() && !a_actor->IsDead()) {
				RE::NiPoint3 velocity{};
				a_actor->GetLinearVelocity(velocity);

				SnowSparkle::NoteContact(
					surface, position, forward.x, forward.y, velocity.x, velocity.y, 18.0f);
			}

			const bool booted =
				a_actor->GetRace() &&
				a_actor->GetRace()->HasKeywordString(Settings::stampShapeKeyword);

			const bool printed =
				Settings::enableStampShapes && StampShapes::Ready() &&
				response.print >= 0.5f && booted;

			const bool oriented = Settings::stampFootShape && booted;

			auto* const tes = RE::TES::GetSingleton();

			const float clearance =
				Settings::stampGroundClearance * std::max(response.clearanceScale, 0.0f);

			const auto visitCollision = [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
					if (a_out.size() >= kMaxStamps) {
						return RE::BSVisit::BSVisitControl::kStop;
					}

					RE::NiPoint3 centre;
					float        radius = 0.0f;
					if (!ActorShapes::GetBound(a_object, centre, radius) || radius <= 0.0f) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}

					if (centre.z - radius > feetZ + reach) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}

					if (clearance > 0.0f && tes) {
						float landZ = 0.0f;
						if (tes->GetLandHeight(centre, landZ) &&
							(centre.z - radius) - landZ > clearance) {
							return RE::BSVisit::BSVisitControl::kContinue;
						}
					}

					Stamp stamp{};
					stamp.snow = surface == Surfaces::Type::kSnow;
					stamp.x = centre.x;
					stamp.y = centre.y;
					stamp.radius = radius * Settings::stampRadiusScale * response.radiusScale;

					if (printed || oriented) {
						const RE::NiPoint3 offset{ centre.x - position.x,
							centre.y - position.y, 0.0f };
						const float side = offset.Dot(right);
						const bool  isFoot =
							std::abs(side) >= Settings::stampFootSeparation;

						if (Settings::logStampFeet) {
							float reportLand = 0.0f;
							if (tes) { tes->GetLandHeight(centre, reportLand); }
							LogFootShape(side, radius, centre.z, reportLand, isFoot);
						}

						if (isFoot) {

							if (printed) {
								stamp.kind = Stamp::Kind::kPrint;
								stamp.mirror = side < 0.0f ? -1.0f : 1.0f;
							}

							stamp.forwardX = forward.x;
							stamp.forwardY = forward.y;

							stamp.radius *= Settings::stampFootLength;
							stamp.halfWidth = stamp.radius * Settings::stampFootAspect;
						}
					}

					const float ordinary =
						Settings::stampDepth * response.depthScale * Weather::DepthScale();

					stamp.depth = Surfaces::MarkDepth(surface, ordinary, 1.0f, stamp.x, stamp.y);
					stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);

					stamp.decay = std::clamp(
						Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(),
						0.0f, 0.9999f);

					stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);

					if (stamp.kind != Stamp::Kind::kPrint) {
						stamp.motionX = motionX;
						stamp.motionY = motionY;
					}

					LogStampSize(stamp);
					a_out.push_back(stamp);

					return RE::BSVisit::BSVisitControl::kContinue;
				};
			RE::BSVisit::TraverseScenegraphCollision(root, std::cref(visitCollision));
		}

		std::vector<Stamp> GatherStamps(float a_deltaSeconds)
		{
			++g_actorFrame;

			for (auto it = g_actorMotion.begin(); it != g_actorMotion.end();) {
				it = (it->second.frame + 120 < g_actorFrame) ? g_actorMotion.erase(it) :
															   std::next(it);
			}

			std::vector<Stamp> stamps;
			stamps.reserve(kMaxStamps);

			RE::NiPoint3 anchor{};
			if (auto* player = globals::game::player) {
				anchor = player->GetPosition();
			}

			std::vector<RE::ActorPtr> actors;
			MagicImpacts::Append(anchor, stamps);
			CollectActors(actors);
			if (actors.empty()) {

				ObjectStamps::Append(a_deltaSeconds, anchor, stamps);
				return stamps;
			}

			const float maxDistance = kWorldSize * 0.375f;
			const float maxDistanceSq = maxDistance * maxDistance;

			std::sort(actors.begin(), actors.end(),
				[&anchor](const RE::ActorPtr& a_lhs, const RE::ActorPtr& a_rhs) {
					return anchor.GetSquaredDistance(a_lhs->GetPosition()) <
					       anchor.GetSquaredDistance(a_rhs->GetPosition());
				});

			for (const auto& actor : actors) {
				AppendActorStamps(actor.get(), anchor, maxDistanceSq, stamps);
				if (stamps.size() >= kMaxStamps) {
					break;
				}
			}

			ObjectStamps::Append(a_deltaSeconds, anchor, stamps);

			return stamps;
		}
	}

	void ResetDiagnostics()
	{
		g_sizesLogged.store(0);
		g_surfacesLogged.store(0);
		g_feetLogged.store(0);
	}

	bool Ready()
	{
		return g_nextFieldSRV && g_nextFieldUAV && g_srv[0] && g_uav[0] && g_sampler && g_updateCS && g_paramsCB && g_windowCB;
	}

	bool Initialize()
	{
		if (Ready()) {
			return true;
		}
		if (g_failed || !globals::Ready()) {
			return false;
		}

		auto* device = globals::d3d::device;

		const auto fail = [&](const char* a_what) {
			logger::error("Clipmap: {}", a_what);
			Release();
			g_failed = true;
			return false;
		};

		for (uint32_t level = 0; level < LevelCount(); ++level) {
			if (!CreateLevel(device, level)) {
				return fail("level allocation failed");
			}
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return fail("CreateSamplerState failed");
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(ParamsCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_paramsCB))) {
			return fail("CreateBuffer failed");
		}

		cbDesc.ByteWidth = sizeof(WindowCB);
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_windowCB))) {
			return fail("CreateBuffer (window) failed");
		}

		if (!CompileUpdateShader(device)) {
			return fail("update shader unavailable");
		}

		logger::info("Clipmap repose: immutable reads, shared 16 MiB height spare, no copy pass");
		logger::info("Clipmap metadata: R16G16_UNORM, {} MiB allocated",
			LevelCount() * kTexels * kTexels * kMetadataPixelBytes / (1024 * 1024));
		logger::info("Clipmap ready: {} level(s), marks survive to {:.0f} world units "
					 "({:.1f} m) from the player",
			LevelCount(), WorldSizeFor(LevelCount() - 1) * 0.5f,
			WorldSizeFor(LevelCount() - 1) * 0.5f / 70.0f);
		return true;
	}

	void Release()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};

		drop(g_nextFieldUAV);
		drop(g_nextFieldSRV);
		drop(g_nextField);
		drop(g_windowCB);
		drop(g_paramsCB);
		drop(g_updateCS);
		drop(g_sampler);

		for (uint32_t level = 0; level < kMaxLevels; ++level) {
			g_prevWindowValid[level] = false;
			drop(g_activitySRV[level]);
			drop(g_activityUAV[level]);
			drop(g_activity[level]);
			drop(g_decaySRV[level]);
			drop(g_decayUAV[level]);
			drop(g_decayTexture[level]);
			drop(g_uav[level]);
			drop(g_srv[level]);
			drop(g_texture[level]);
		}
	}

	void Update(float a_deltaSeconds)
	{
		if (!Ready() || !globals::game::player) {
			return;
		}

		const auto position = globals::game::player->GetPosition();

		const uint32_t levels = LevelCount();
		for (uint32_t level = 0; level < levels; ++level) {
			if (!g_srv[level] && !CreateLevel(globals::d3d::device, level)) {
				logger::error("Clipmap: level {} unavailable, running at {} level(s)",
					level, level);
				break;
			}
		}

		ParamsCB params{};

		params.window[3] = static_cast<float>(kTexels);

		const float dt = std::clamp(a_deltaSeconds, 0.0f, 0.25f);
		params.control[0] = dt;

		const int64_t gatherStart = Profiler::Ticks();
		const auto    stamps = GatherStamps(a_deltaSeconds);
		Profiler::AddCpuTicks(Profiler::CpuScope::kGatherStamps, Profiler::Ticks() - gatherStart);
		const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(stamps.size()), kMaxStamps);
		params.control[1] = static_cast<float>(count);

		static uint32_t reportedCount = 0xFFFFFFFFu;
		if (count != reportedCount) {
			reportedCount = count;
			if (stamps.size() > kMaxStamps) {
				logger::warn("Stamps: {} pressed, {} DROPPED - the frame wanted more than "
				             "the budget of {}, and the furthest were cut",
					count, stamps.size() - kMaxStamps, kMaxStamps);
			} else if (Settings::logObjectStamps) {

				logger::info("Stamps: {} pressed", count);
			}
		}

		params.control[2] = static_cast<float>(kTexels / 2 - 2);

		params.control[3] = std::max(Settings::stampRimSpan, 0.0f);

		params.weather[0] = Weather::FillPerSecond();

		params.weather[1] = Settings::stampSlopeLimit > 0.0f ?
								std::tan(std::clamp(Settings::stampSlopeLimit, 1.0f, 89.0f) *
									0.017453292f) :
								0.0f;
		params.weather[2] = std::clamp(Settings::stampReposeRate, 0.0f, 1.0f);
		params.weather[3] = std::clamp(Settings::stampRimNoise, 0.0f, 8.0f);
		params.rimShape[0] = std::clamp(Settings::stampRimLean, 0.0f, 1.0f);
		params.rimShape[1] = std::clamp(Settings::stampChurn, 0.0f, 8.0f);
		const auto snowValue = [](float overrideValue, float globalValue) {
			return overrideValue < 0.0f ? globalValue : overrideValue;
		};
		params.rimShape[2] = snowValue(Settings::snowStampReposeRate, params.weather[2]);
		params.snowRim[0] = snowValue(Settings::snowStampRimSpan, params.control[3]);
		params.snowRim[1] = snowValue(Settings::snowStampRimNoise, params.weather[3]);
		params.snowRim[2] = snowValue(Settings::snowStampRimLean, params.rimShape[0]);
		params.snowRim[3] = snowValue(Settings::snowStampChurn, params.rimShape[1]);

		for (uint32_t i = 0; i < count; ++i) {
			params.stamps[i][0] = stamps[i].x;
			params.stamps[i][1] = stamps[i].y;
			params.stamps[i][2] = stamps[i].radius;
			params.stamps[i][3] = stamps[i].depth;

			params.stampParams[i][0] = stamps[i].shoulder;
			params.stampParams[i][1] = stamps[i].decay;

			params.stampParams[i][2] = static_cast<float>(stamps[i].kind);
			params.stampParams[i][3] = stamps[i].rim;

			params.stampShape[i][0] = stamps[i].forwardX;
			params.stampShape[i][1] = stamps[i].forwardY;
			params.stampShape[i][2] = stamps[i].halfWidth;
			params.stampShape[i][3] = stamps[i].mirror;

			params.stampMotion[i][0] = stamps[i].motionX;
			params.stampMotion[i][1] = stamps[i].motionY;
			params.stampMotion[i][2] = stamps[i].snow ? 1.0f : 0.0f;
		}
		FillStampBounds(params, count);

		auto* context = globals::d3d::context;

		{

			const auto fadeFor = [&](uint32_t a_level, float a_out[4]) {
				const float cell = CellSizeFor(a_level);
				const float validHalf = params.control[2] * cell;

				a_out[0] = std::floor(position.x / cell) * cell;
				a_out[1] = std::floor(position.y / cell) * cell;
				a_out[2] = validHalf * 0.80f;
				a_out[3] = validHalf * 0.97f;
			};

			WindowCB window{};
			fadeFor(0, window.centreAndFade);
			if (levels > 1) {
				fadeFor(1, window.window1);
			}

			window.raise[0] = Weather::RaiseScale();

			{
				const bool floored = Settings::snowGroundFloor && Settings::enableSnowRaise &&
					Settings::useClipmap && Settings::snowRaiseHeight > 0.0f &&
					SnowCoverage::Ready();

				params.raise[0] = floored ? Settings::snowRaiseHeight : 0.0f;
				params.raise[1] = window.raise[0];
				params.raise[2] = std::max(Settings::snowGroundBite, 0.0f);

				params.raise[3] =
					(floored && Settings::shelterMeshCap && Shelter::View()) ? 1.0f : 0.0f;

				params.raiseWindow[0] = window.centreAndFade[0];
				params.raiseWindow[1] = window.centreAndFade[1];
				params.raiseWindow[3] = Settings::snowRaiseDistance;
				params.raiseWindow[2] = std::max(
					Settings::snowRaiseDistance - Settings::snowRaiseFadeBand, 0.0f);
			}

			g_windowCentreX = window.centreAndFade[0];
			g_windowCentreY = window.centreAndFade[1];

			g_windowHalfExtent = (levels > 1) ? window.window1[3] : window.centreAndFade[3];
			g_windowValid = true;

			D3D11_MAPPED_SUBRESOURCE windowMap{};
			if (SUCCEEDED(context->Map(g_windowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &windowMap))) {
				std::memcpy(windowMap.pData, &window, sizeof(window));
				context->Unmap(g_windowCB, 0);
			}
		}

		{
			const ComputeStageGuard guard(context);

			const UINT noOffset[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1),
				static_cast<UINT>(-1) };

			ID3D11ShaderResourceView* shape = StampShapes::View();
			context->CSSetShaderResources(0, 1, &shape);

			ID3D11ShaderResourceView* floorMaps[2] = {
				params.raise[0] > 0.0f ? SnowCoverage::View() : nullptr,
				params.raise[3] > 0.0f ? Shelter::View() : nullptr
			};
			if (!floorMaps[0]) {
				params.raise[0] = 0.0f;
			}
			if (!floorMaps[1]) {
				params.raise[3] = 0.0f;
			}
			context->CSSetShaderResources(3, 2, floorMaps);

			context->CSSetSamplers(0, 1, &g_sampler);
			context->CSSetShader(g_updateCS, nullptr, 0);

			Profiler::GpuBegin(Profiler::Scope::kClipmap);

			for (uint32_t i = 0; i < levels; ++i) {
				const uint32_t level = levels - 1 - i;
				if (!g_uav[level] || !g_decayUAV[level]) {
					continue;
				}

				const float cell = CellSizeFor(level);

				params.window[0] = std::floor(position.x / cell);
				params.window[1] = std::floor(position.y / cell);
				params.window[2] = cell;

				const bool  hasCoarser = (level + 1) < levels && g_srv[level + 1];
				params.coarse[0] = hasCoarser ? 1.0f : 0.0f;
				params.coarse[1] = WorldSizeFor(level + 1);
				params.coarse[2] = std::max(Settings::clipmapSeedSmoothing, 0.0f);

				const int32_t nowX = static_cast<int32_t>(params.window[0]);
				const int32_t nowY = static_cast<int32_t>(params.window[1]);

				int32_t moved = static_cast<int32_t>(kTexels);
				if (g_prevWindowValid[level]) {
					moved = std::max(std::abs(nowX - g_prevWindowX[level]),
						std::abs(nowY - g_prevWindowY[level]));
				}
				g_prevWindowX[level] = nowX;
				g_prevWindowY[level] = nowY;
				g_prevWindowValid[level] = true;

				params.coarse[3] = std::clamp(params.control[2] - static_cast<float>(moved) - 2.0f,
					0.0f, params.control[2]);

				ID3D11ShaderResourceView* seeds[2] = {
					hasCoarser ? g_srv[level + 1] : nullptr,
					hasCoarser ? g_decaySRV[level + 1] : nullptr
				};
				context->CSSetShaderResources(1, 2, seeds);

				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(g_paramsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					break;
				}
				std::memcpy(mapped.pData, &params, sizeof(params));
				context->Unmap(g_paramsCB, 0);

				const UINT zero[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(g_activityUAV[level], zero);

				context->CSSetShaderResources(5, 1, &g_srv[level]);
				ID3D11UnorderedAccessView* uavs[3] = { g_nextFieldUAV, g_decayUAV[level],
					g_activityUAV[level] };
				context->CSSetConstantBuffers(0, 1, &g_paramsCB);
				context->CSSetUnorderedAccessViews(0, 3, uavs, noOffset);
				context->Dispatch(kTexels / 8, kTexels / 8, 1);

				ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
				context->CSSetUnorderedAccessViews(0, 3, nullUAVs, noOffset);

				ID3D11ShaderResourceView* nullSeeds[2] = { nullptr, nullptr };
				context->CSSetShaderResources(1, 2, nullSeeds);
				context->CSSetShaderResources(5, 1, nullSeeds);
				std::swap(g_texture[level], g_nextField);
				std::swap(g_srv[level], g_nextFieldSRV);
				std::swap(g_uav[level], g_nextFieldUAV);
			}

			ID3D11ShaderResourceView* nullFloor[2] = { nullptr, nullptr };
			context->CSSetShaderResources(3, 2, nullFloor);

			Profiler::GpuEnd();
		}
	}

	// Called while Update is skipped (indoors): the next Update reseeds every level as a
	// window jump does, and actor motion starts fresh instead of from before the skip.
	void ForgetWindow()
	{
		for (auto& valid : g_prevWindowValid) {
			valid = false;
		}
		g_actorMotion.clear();
	}

	bool GetWindow(float& a_centreX, float& a_centreY, float& a_halfExtent)
	{
		if (!g_windowValid) {
			return false;
		}
		a_centreX = g_windowCentreX;
		a_centreY = g_windowCentreY;
		a_halfExtent = g_windowHalfExtent;
		return true;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!Ready()) {
			return;
		}
		a_context->DSSetShaderResources(0, 1, &g_srv[0]);
		a_context->DSSetSamplers(0, 1, &g_sampler);
		a_context->DSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);
		a_context->HSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);

		if (g_activitySRV[0]) {
			a_context->HSSetShaderResources(kActivitySlot, 1, &g_activitySRV[0]);
		}

		if (LevelCount() > 1 && g_srv[1]) {
			a_context->DSSetShaderResources(kLevel1Slot, 1, &g_srv[1]);
		}
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{

		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11SamplerState*       nullSampler = nullptr;
		ID3D11Buffer* nullCB = nullptr;
		a_context->DSSetShaderResources(0, 1, &nullSRV);
		a_context->DSSetShaderResources(kLevel1Slot, 1, &nullSRV);
		a_context->HSSetShaderResources(kActivitySlot, 1, &nullSRV);
		a_context->DSSetSamplers(0, 1, &nullSampler);
		a_context->DSSetConstantBuffers(kParamsSlot, 1, &nullCB);
		a_context->HSSetConstantBuffers(kParamsSlot, 1, &nullCB);
	}
}
