// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
// Community Shaders render-hook references are credited in NOTICE.
// Provenance notice added 2026-09-28.

#include "PCH.h"

#include "BloodDecals.h"
#include "BloodRoutingGate.h"
#include "Clipmap.h"
#include "Globals.h"
#include "Hooks.h"
#include "ObjectStamps.h"
#include "MagicImpacts.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "ShaderReflection.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "TerrainActivity.h"
#include "TerrainActivityPolicy.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Tessellation.h"
#include "UtilityRouting.h"
#include "Weather.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <format>
#include <limits>
#include <string>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace Hooks
{
	namespace
	{

		constexpr auto kNearLand = RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
		constexpr auto kLodLand = RE::BSShaderMaterial::Feature::kLODLandNoise;

		std::mutex                   g_observedLock;
		std::unordered_set<uint64_t> g_observedLand;
		std::unordered_set<uint64_t> g_observedOther;

		constexpr size_t kMaxLand = 64;
		constexpr size_t kMaxOther = 96;

		struct DrawInfo
		{
			RE::BSShaderMaterial::Feature feature{ RE::BSShaderMaterial::Feature::kNone };
			uint64_t                      vertexDesc{ 0 };
			uint32_t                      byteCodeSize{ 0 };
			uint32_t                      passEnum{ 0 };
			uint32_t                      vertexTechnique{ 0 };
			const void*                   byteCode{ nullptr };
			const void*                   gameShader{ nullptr };
			RE::BSShaderMaterial*         material{ nullptr };
			bool                          isNearLand{ false };
			bool                          isLodLand{ false };
		};

		DrawInfo Describe(RE::BSRenderPass* a_pass)
		{
			DrawInfo info{};

			if (a_pass) {
				info.passEnum = a_pass->passEnum;

				if (a_pass->shaderProperty) {
					if (auto* material = a_pass->shaderProperty->GetBaseMaterial()) {
						info.material = material;
						info.feature = material->GetFeature();
						info.isNearLand = info.feature == kNearLand;
						info.isLodLand = info.feature == kLodLand;
					}
				}
			}

			if (auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton()) {
				if (auto* vs = REL::Module::IsVR() ? shadowState->GetVRRuntimeData().currentVertexShader : shadowState->GetRuntimeData().currentVertexShader) {
					info.vertexTechnique = vs->id;
					info.vertexDesc = vs->vertexDesc;
					info.byteCodeSize = vs->byteCodeSize;
					info.byteCode = vs->rawBytecode;
					info.gameShader = vs->shader;
				}
			}

			return info;
		}

		uint64_t BytecodeHash(const void* a_bytes, uint32_t a_size)
		{
			if (!a_bytes || a_size == 0) {
				return 0;
			}

			const auto* p = static_cast<const unsigned char*>(a_bytes);
			uint64_t    hash = 0xcbf29ce484222325ull;
			for (uint32_t i = 0; i < a_size; ++i) {
				hash ^= p[i];
				hash *= 0x100000001b3ull;
			}
			return hash;
		}

		void Observe(const char* a_stage, RE::BSRenderPass* a_pass)
		{

			if (!Settings::logDraws) {
				return;
			}

			const DrawInfo info = Describe(a_pass);

			const uint64_t key =
				std::hash<uint64_t>{}(info.vertexDesc) ^
				(std::hash<uint32_t>{}(static_cast<uint32_t>(info.feature)) << 1) ^
				(std::hash<std::string_view>{}(a_stage) << 2);

			const bool isLand = info.isNearLand || info.isLodLand;

			{
				const std::scoped_lock lock(g_observedLock);
				auto&                  seen = isLand ? g_observedLand : g_observedOther;
				const size_t           budget = isLand ? kMaxLand : kMaxOther;

				if (seen.size() >= budget || !seen.insert(key).second) {
					return;
				}
			}

			const char* kind = info.isNearLand ? "LANDSCAPE(near)" :
			                   info.isLodLand  ? "landscape(LOD)" :
			                                     "other";

			logger::info(
				"[{}] {:<15} feature={:<3} pass={:<3} vertexDesc=0x{:016X} bytecode={} bytes "
				"hash=0x{:016X}",
				a_stage,
				kind,
				static_cast<int32_t>(info.feature),
				info.passEnum,
				info.vertexDesc,
				info.byteCodeSize,
				BytecodeHash(info.byteCode, info.byteCodeSize));

			if ((isLand || Settings::enableStaticProbe) && info.byteCode) {
				const auto signature =
					Reflection::ReflectOutputSignature(info.byteCode, info.byteCodeSize);

				logger::info("    VS output signature ({} elements):{}",
					signature.elements.size(),
					Reflection::Describe(signature));
			}
		}

		std::mutex                                            g_signatureLock;
		std::unordered_map<uint64_t, Reflection::Signature>    g_signatures;

		std::unordered_map<const void*, const Reflection::Signature*> g_resolved;

		const Reflection::Signature* SignatureFor(const DrawInfo& a_info)
		{
			enum class Source
			{
				kStruct,
				kAgrees,
				kDiffers,
				kUnknown,
			};

			ID3D11VertexShader* bound = nullptr;
			if (auto* context = globals::d3d::context) {
				context->VSGetShader(&bound, nullptr, nullptr);
			}

			if (bound) {
				const std::scoped_lock lock(g_signatureLock);
				if (const auto it = g_resolved.find(bound); it != g_resolved.end()) {
					bound->Release();
					return it->second;
				}
			}

			const void* bytecode = a_info.byteCode;
			size_t      size = a_info.byteCodeSize;
			Source      source = Source::kStruct;

			if (bound) {
				if (const auto actual = ShaderRegistry::For(bound)) {
					const bool same =
						a_info.byteCode && actual.size == a_info.byteCodeSize &&
						std::memcmp(actual.data, a_info.byteCode, actual.size) == 0;
					if (same) {
						source = Source::kAgrees;
					} else {
						source = Source::kDiffers;
						bytecode = actual.data;
						size = actual.size;
					}
				} else {
					source = Source::kUnknown;
				}
			}

			{
				static bool reported[4]{};
				const auto  index = static_cast<size_t>(source);
				if (!reported[index]) {
					reported[index] = true;
					switch (source) {
					case Source::kAgrees:
						logger::info("Bound vertex shader matches the game's bytecode "
									 "({} shader(s) recorded).",
							ShaderRegistry::Count());
						break;
					case Source::kDiffers:
						logger::info("Bound vertex shader is NOT what the game's struct "
									 "describes: {} bytes bound, {} in the struct. "
									 "Reflecting the bound one.",
							size, a_info.byteCodeSize);
						break;
					case Source::kUnknown:
						logger::warn("Bound vertex shader was created before the registry "
									 "was installed, so it cannot be checked - falling "
									 "back to the game's bytecode. ({} recorded)",
							ShaderRegistry::Count());
						break;
					case Source::kStruct:
						logger::info("No bound vertex shader to check; using the game's "
									 "bytecode.");
						break;
					}
				}
			}

			const Reflection::Signature* resolved = nullptr;

			if (bytecode && size != 0) {
				const uint64_t key = reinterpret_cast<uint64_t>(bytecode) ^
				                     (static_cast<uint64_t>(size) << 48);

				const std::scoped_lock lock(g_signatureLock);

				const auto it = g_signatures.find(key);
				if (it != g_signatures.end()) {
					resolved = it->second.valid ? &it->second : nullptr;
				} else {
					auto       signature = Reflection::ReflectOutputSignature(bytecode, size);
					const bool valid = signature.valid;

					if (source == Source::kDiffers) {
						static bool dumped = false;
						if (!dumped) {
							dumped = true;
							const auto stale = Reflection::ReflectOutputSignature(
								a_info.byteCode, a_info.byteCodeSize);
							logger::info(
								"  bound shader reflects {} element(s), the struct's {}",
								signature.elements.size(), stale.elements.size());
							if (valid) {
								logger::info("  bound VS output signature:{}",
									Reflection::Describe(signature));
							}
						}
					}

					g_signatures[key] = std::move(signature);
					resolved = valid ? &g_signatures[key] : nullptr;
				}

				if (bound) {
					g_resolved[bound] = resolved;
				}
			}

			if (bound) {
				bound->Release();
			}

			return resolved;
		}

		RE::Actor* ActorForPass(RE::BSRenderPass* a_pass)
		{
			if (!a_pass || !a_pass->geometry) {
				return nullptr;
			}

			auto* ref = a_pass->geometry->GetUserData();
			return ref ? ref->As<RE::Actor>() : nullptr;
		}

		RE::TESObjectREFR* RefForPass(RE::BSRenderPass* a_pass)
		{
			if (!a_pass || !a_pass->geometry) {
				return nullptr;
			}
			return a_pass->geometry->GetUserData();
		}

		std::mutex                      g_probeLock;
		std::unordered_set<std::string> g_probedModels;
		constexpr size_t                kMaxProbedModels = 64;
		uint32_t                        g_largestProbed = 0;

		void LogProbedStatic(RE::BSRenderPass* a_pass, RE::TESObjectREFR* a_ref)
		{
			if (!Settings::logStaticProbe || !a_ref) {
				return;
			}

			const char* model = nullptr;
			if (auto* base = a_ref->GetBaseObject()) {
				if (const auto* asModel = base->As<RE::TESModel>()) {
					model = asModel->GetModel();
				}
			}
			if (!model || !*model) {
				return;
			}

			uint32_t triangles = 0;
			if (auto* tri = a_pass->geometry->AsTriShape()) {
				triangles = tri->GetTrishapeRuntimeData().triangleCount;
			}

			bool biggest = false;
			{
				std::scoped_lock lock{ g_probeLock };

				if (triangles > g_largestProbed) {
					g_largestProbed = triangles;
					biggest = true;
				}

				if (!biggest) {

					if (triangles < static_cast<uint32_t>(
									std::max(Settings::staticProbeMinTriangles, 0))) {
						return;
					}
					if (g_probedModels.size() >= kMaxProbedModels) {
						return;
					}
				}

				if (!g_probedModels.emplace(model).second) {
					return;
				}
			}

			logger::info("Probe:{} {} | {} triangles{}", biggest ? " LARGEST YET" : "",
				model, triangles,
				triangles > 16000 ? "  <- past the 16k note" : "");
		}

		std::mutex                   g_actorDrawLock;
		std::unordered_set<uint64_t> g_observedActorDraws;
		constexpr size_t             kMaxActorDraws = 48;

		void ObserveActorDraw(const char* a_stage, RE::BSRenderPass* a_pass,
			const DrawInfo& a_info)
		{
			if (!Settings::logActorDraws) {
				return;
			}

			auto* actor = ActorForPass(a_pass);
			if (!actor) {
				return;
			}

			const bool vertexColours =
				a_pass->shaderProperty &&
				a_pass->shaderProperty->flags.any(
					RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);

			const uint64_t key =
				std::hash<uint64_t>{}(a_info.vertexDesc) ^
				(std::hash<uint32_t>{}(static_cast<uint32_t>(a_info.feature)) << 1) ^
				(std::hash<std::string_view>{}(a_stage) << 2) ^
				(std::hash<bool>{}(vertexColours) << 3);

			{
				const std::scoped_lock lock(g_actorDrawLock);
				if (g_observedActorDraws.size() >= kMaxActorDraws ||
					!g_observedActorDraws.insert(key).second) {
					return;
				}
			}

			const auto* signature = SignatureFor(a_info);

			bool hasColour0 = false;
			bool hasColour1 = false;
			if (signature) {
				for (const auto& e : signature->elements) {
					if (e.semanticName == "COLOR" && e.semanticIndex == 0) {
						hasColour0 = true;
					}
					if (e.semanticName == "COLOR" && e.semanticIndex == 1) {
						hasColour1 = true;
					}
				}
			}

			logger::info(
				"[{}] ACTOR draw: {} (0x{:08X}){} feature={} vertexDesc=0x{:016X} "
				"COLOR0={} COLOR1={} vertexColourFlag={}",
				a_stage,
				actor->GetName(),
				actor->GetFormID(),
				actor->IsPlayerRef() ? " [player]" : "",
				static_cast<int32_t>(a_info.feature),
				a_info.vertexDesc,
				hasColour0 ? "yes" : "NO",
				hasColour1 ? "yes" : "no",
				vertexColours ? "SET" : "clear");

			if (signature) {
				logger::info("    VS output signature ({} elements):{}",
					signature->elements.size(), Reflection::Describe(*signature));
			} else {
				logger::info("    (no usable signature)");
			}
		}

		float ReachFromCentre(float a_windowHalfExtent)
		{
			if (!Settings::enableSnowRaise || !Settings::useClipmap ||
				Settings::snowRaiseHeight <= 0.0f) {
				return a_windowHalfExtent;
			}

			return std::max(a_windowHalfExtent, Settings::snowRaiseDistance);
		}

		bool ShouldRoute(RE::BSRenderPass* a_pass)
		{
			if (!Settings::cullDistantDraws) {
				return true;
			}

			float centreX = 0.0f;
			float centreY = 0.0f;
			float halfExtent = 0.0f;
			if (!Clipmap::GetWindow(centreX, centreY, halfExtent)) {
				return true;
			}

			const float reach = ReachFromCentre(halfExtent);

			if (!a_pass || !a_pass->geometry) {
				return true;
			}

			const auto& bound = a_pass->geometry->worldBound;
			if (!(bound.radius > 0.0f)) {

				return true;
			}

			const float margin = Settings::cullMargin;
			const float dx = std::max(0.0f, std::fabs(bound.center.x - centreX) - reach);
			const float dy = std::max(0.0f, std::fabs(bound.center.y - centreY) - reach);
			const float slack = bound.radius + margin;

			return (dx * dx + dy * dy) <= (slack * slack);
		}

		std::atomic<uint32_t> g_boundsLogged{ 0 };
		constexpr uint32_t    kMaxBoundsLogged = 12;

		bool DrawnFromPlayerCamera()
		{
			if (!Settings::routeOnlyPlayerCamera) {
				return true;
			}

			auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (!shadowState) {
				return true;
			}

			const auto& view = REL::Module::IsVR() ? shadowState->GetVRRuntimeData().cameraData.getEye() : shadowState->GetRuntimeData().cameraData.getEye();
			const float* vp = &view.viewProjMat.m[0][0];
			const bool   perspective = UtilityRouting::IsPerspectiveProjection(vp);

			static bool reported[2]{};
			if (!reported[perspective ? 1 : 0]) {
				reported[perspective ? 1 : 0] = true;
				logger::info("Camera gate: {} a draw, viewProj column 3 = "
							 "({:.6f}, {:.6f}, {:.6f}, {:.6f})",
					perspective ? "ROUTING" : "refusing",
					vp[3], vp[7], vp[11], vp[15]);
			}

			return perspective;
		}

		void LogBounds(RE::BSRenderPass* a_pass)
		{
			if (!Settings::logDraws) {
				return;
			}
			if (g_boundsLogged.load() >= kMaxBoundsLogged || !a_pass || !a_pass->geometry) {
				return;
			}
			if (g_boundsLogged.fetch_add(1) >= kMaxBoundsLogged) {
				return;
			}

			const auto& bound = a_pass->geometry->worldBound;

			float centreX = 0.0f;
			float centreY = 0.0f;
			float halfExtent = 0.0f;
			const bool haveWindow = Clipmap::GetWindow(centreX, centreY, halfExtent);

			logger::info(
				"  landscape bound: centre=({:.1f}, {:.1f}, {:.1f}) radius={:.1f} | "
				"window=({:.1f}, {:.1f}) half={:.1f}{}",
				bound.center.x, bound.center.y, bound.center.z, bound.radius,
				centreX, centreY, halfExtent,
				haveWindow ? "" : " (no window yet)");
		}

		bool NearLandscape(RE::BSRenderPass* pass)
		{
			auto* property = pass ? pass->shaderProperty : nullptr;
			auto* material = property ? property->GetBaseMaterial() : nullptr;
			return material && material->GetFeature() == kNearLand;
		}

		bool RouteBlood(RE::BSRenderPass* pass, const DrawInfo& info)
		{
			if (!pass) { return false; }
			const auto lookupStarted = Profiler::Ticks();
			const bool blood = BloodDecals::Contains(pass->geometry);
			Profiler::AddCpuTicks(Profiler::CpuScope::kBloodLookup, Profiler::Ticks() - lookupStarted);
			if (!blood) { return false; }
			bool routed = false;
			if (globals::Ready() && Settings::useClipmap && Clipmap::Ready() && ShouldRoute(pass)) {
				auto* context = globals::d3d::context;
				ID3D11VertexShader* bound{};
				ID3D11HullShader* hs{};
				ID3D11DomainShader* ds{};
				ID3D11GeometryShader* gs{};
				D3D11_PRIMITIVE_TOPOLOGY topology{};
				context->VSGetShader(&bound, nullptr, nullptr);
				context->HSGetShader(&hs, nullptr, nullptr);
				context->DSGetShader(&ds, nullptr, nullptr);
				context->GSGetShader(&gs, nullptr, nullptr);
				context->IAGetPrimitiveTopology(&topology);
				auto* shadow = RE::BSGraphics::RendererShadowState::GetSingleton();
				auto* gameVS = shadow ? (REL::Module::IsVR() ? shadow->GetVRRuntimeData().currentVertexShader : shadow->GetRuntimeData().currentVertexShader) : nullptr;
				const bool sameVS = gameVS &&
					bound == reinterpret_cast<ID3D11VertexShader*>(gameVS->shader);
				const bool noTess = !hs && !ds && !gs;
				const bool triList = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

				const bool ignoreIdentity = Settings::bloodDecalsIgnoreShaderIdentity;
				const bool supported = (sameVS || ignoreIdentity) && noTess && triList;

				if (!supported) {
					static bool reported = false;
					if (!reported) {
						reported = true;
						logger::info("Blood decals B4: pipeline gate refused - sameVS={} "
									 "noTess={} (hs={} ds={} gs={}) triList={} (topology={})",
							sameVS, noTess, hs != nullptr, ds != nullptr, gs != nullptr,
							triList, static_cast<int>(topology));
						logger::info("  reflected VS: id={} desc={:#018x} bytecode={} bytes"
									 " | set BloodDecalsIgnoreShaderIdentity = 1 to route "
									 "it anyway and see whether it draws correctly",
							gameVS ? gameVS->id : 0u, info.vertexDesc, info.byteCodeSize);
						if (!sameVS) {
							logger::info("  the bound vertex shader is not the game's own. "
										 "Another mod has replaced it, and our domain stage "
										 "is generated against the GAME's signature - so "
										 "routing it would mismatch. Community Shaders does "
										 "this.");
						}
					}
				}
				if (bound) { bound->Release(); }
				if (hs) { hs->Release(); }
				if (ds) { ds->Release(); }
				if (gs) { gs->Release(); }
				if (supported && !sameVS) {
					static bool forced = false;
					if (!forced) {
						forced = true;
						logger::warn("Blood decals B4: routing a draw whose vertex shader "
									 "is NOT the game's, because "
									 "BloodDecalsIgnoreShaderIdentity is on. If the decals "
									 "look wrong, this is why.");
					}
				}

				if (supported) {
					if (const auto* signature = SignatureFor(info)) {
						routed = Tessellation::BeginDraw(info.vertexDesc, *signature, Tessellation::Mode::kBloodDecal, {});
					}
				}
			}
			BloodDecals::NoteDraw(routed);
			return true;
		}

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
			{

				func(a_shader, a_pass, a_flags);

				if (!TerrainActivity::Active()) { return; }

				Observe("Lighting", a_pass);

				if (Settings::logActorDraws) {
					ObserveActorDraw("Lighting", a_pass, Describe(a_pass));
				}

				if (!Settings::enableTessellation) {
					return;
				}

				const bool bloodCandidate = a_pass && BloodDecals::MayContain(a_pass->geometry);
				if (!Settings::enableStaticProbe && !BloodRoutingGate::Relevant(false, NearLandscape(a_pass), bloodCandidate)) { return; }

				if (!DrawnFromPlayerCamera()) {
					return;
				}

				const DrawInfo info = Describe(a_pass);

				if (bloodCandidate && RouteBlood(a_pass, info)) { return; }

				if (Settings::enableStaticProbe && !info.isNearLand && !info.isLodLand) {
					auto* ref = RefForPass(a_pass);
					if (ref && !ref->As<RE::Actor>()) {
						Profiler::Tally(Profiler::Count::kStaticSeen);

						if (const auto* signature = SignatureFor(info)) {
							if (Tessellation::BeginDraw(info.vertexDesc, *signature,
										Tessellation::Mode::kStaticProbe, {})) {
								Profiler::Tally(Profiler::Count::kStaticRouted);
								LogProbedStatic(a_pass, ref);
							}
						}
						return;
					}
				}


				if (!info.isNearLand) {
					return;
				}

				LogBounds(a_pass);

				Profiler::Tally(Profiler::Count::kLandscapeSeen);

				if (!ShouldRoute(a_pass)) {
					Profiler::Tally(Profiler::Count::kLandscapeCulled);
					return;
				}

				const auto surfaces = Surfaces::Classify(info.material);

				Tessellation::DrawMaterial material{};
				material.revealLayer = surfaces.reveal;
				material.snowLayer = surfaces.snow;

				material.strengthScale =
					surfaces.revealUnderSnow ? Settings::snowRevealStrength : 1.0f;

				if (const auto* signature = SignatureFor(info)) {
					if (Tessellation::BeginDraw(info.vertexDesc, *signature,
							Tessellation::Mode::kLandscape, material)) {
						Profiler::Tally(Profiler::Count::kLandscapeRouted);
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
			{

				Tessellation::EndDraw();

				func(a_shader, a_pass, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
			{
				func(a_shader, a_pass, a_flags);

				if (!TerrainActivity::Active()) { return; }

				Observe("Utility", a_pass);

				if (!Settings::enableTessellation || !Settings::enableDepthPass) {
					return;
				}

				const bool bloodCandidate = a_pass && BloodDecals::MayContain(a_pass->geometry);
				const bool probeCandidate = Settings::enableStaticProbe && Settings::staticProbeOffset != 0.0f;
				if (!probeCandidate && !BloodRoutingGate::Relevant(false, NearLandscape(a_pass), bloodCandidate)) { return; }
				const DrawInfo info = Describe(a_pass);

				if (!UtilityRouting::IsCameraDepth(info.vertexTechnique)) {
					if (info.isNearLand) {
						Profiler::Tally(Profiler::Count::kDepthShadowSkipped);
					}
					return;
				}

				if (!DrawnFromPlayerCamera()) {
					if (info.isNearLand) {
						Profiler::Tally(Profiler::Count::kDepthShadowSkipped);
					}
					return;
				}
				if (bloodCandidate && RouteBlood(a_pass, info)) { return; }

				if (Settings::enableStaticProbe && Settings::staticProbeOffset != 0.0f &&
					!info.isNearLand && !info.isLodLand) {
					auto* ref = RefForPass(a_pass);
					if (ref && !ref->As<RE::Actor>()) {
						if (const auto* signature = SignatureFor(info)) {
							Tessellation::BeginDraw(info.vertexDesc, *signature,
									Tessellation::Mode::kStaticProbe, {});
						}
						return;
					}
				}


				if (!info.isNearLand) {
					return;
				}

				Profiler::Tally(Profiler::Count::kDepthSeen);

				if (!ShouldRoute(a_pass)) {
					Profiler::Tally(Profiler::Count::kDepthCulled);
					return;
				}

				const auto* signature = SignatureFor(info);
				if (!signature) {
					return;
				}

				if (signature->elements.size() != 1) {
					Profiler::Tally(Profiler::Count::kDepthShadowSkipped);
					return;
				}

				if (Tessellation::BeginDraw(
						info.vertexDesc, *signature, Tessellation::Mode::kLandscape, {})) {
					Profiler::Tally(Profiler::Count::kDepthRouted);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
			{
				Tessellation::EndDraw();

				func(a_shader, a_pass, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void ApplyReloadedSettings()
		{

			Tessellation::Reset();
			BloodDecals::Reset();

			Surfaces::Reset();

			Surfaces::LoadProfiles();

			SnowSparkle::Reset();

			ObjectStamps::Reset();
			MagicImpacts::Reset();

			Clipmap::ResetDiagnostics();

			Weather::Reset();

			SnowCoverage::Initialize();
			SnowCoverage::Reset();

			Shelter::Reset();

			Profiler::Reset();

		}

		bool RefreshTerrainActivity()
		{
			auto* player = globals::game::player;
			auto* cell = player ? player->GetParentCell() : nullptr;
			auto* world = cell && !cell->IsInteriorCell() ? player->GetWorldspace() : nullptr;
			static RE::FormID lastWorld = 0;
			static bool city = false;
			const auto worldID = world ? world->GetFormID() : 0;
			if (worldID != lastWorld) {
				city = TerrainActivity::IsCityWorld(world);
				lastWorld = worldID;
			}
			static TerrainActivity::Policy policy;
			const bool loaded = player && cell && player->Is3DLoaded();
			const bool interior = cell && cell->IsInteriorCell();
			const bool reset = policy.Update(loaded && Settings::enableTessellation, interior, city, worldID);
			if (reset) {
				logger::info("Terrain activity: {} | world={:08X} cell={:08X} cityWorld={} interior={} loaded3D={} tess={}",
					policy.active ? "active" : "idle", worldID, cell ? cell->GetFormID() : 0,
					city, interior, loaded, Settings::enableTessellation);
				TerrainActivity::active.store(false, std::memory_order_relaxed);
				Clipmap::ForgetWindow();
				ObjectStamps::Forget();
				MagicImpacts::Reset();
				BloodDecals::Reset();
				SnowSparkle::Forget();
				SnowCoverage::ForgetWindow();
				Shelter::ForgetWindow();
				Surfaces::Reset();
				Weather::Reset();
				Profiler::Reset();
			}
			TerrainActivity::active.store(policy.active, std::memory_order_relaxed);
			return policy.active;
		}

		std::mutex g_frameWorkLock;
		struct Main_Update
		{
			static void thunk(RE::Main* main, float delta)
			{
				func(main, delta);
				std::lock_guard guard(g_frameWorkLock);
				auto* player = globals::game::player;
				auto* cell = player ? player->GetParentCell() : nullptr;
				auto* world = player && cell && !cell->IsInteriorCell() ? player->GetWorldspace() : nullptr;
				static RE::FormID lastWorld = 0;
				static bool city = false;
				static bool wasEligible = false;
				const auto worldID = world ? world->GetFormID() : 0;
				if (worldID != lastWorld) { city = TerrainActivity::IsCityWorld(world); lastWorld = worldID; }
				if (!Settings::enableTessellation || !player ||
					!player->Is3DLoaded() || !cell || cell->IsInteriorCell() || !world || city) {
					if (wasEligible) { Clipmap::InvalidateGather(); }
					wasEligible = false;
					return;
				}
				wasEligible = true;
				Weather::Update();
				Shelter::UpdateCPU();
				const float raw = globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
				const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
				if (Settings::useClipmap) {
					Clipmap::GatherFrame(!paused && raw > 0.0f && raw < 0.25f ? raw : 0.0f);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderDepth
		{
			static void thunk(bool a1, bool a2)
			{
				std::unique_lock guard(g_frameWorkLock);

				const float raw = globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
				const float dt = (raw > 0.0f && raw < 0.25f) ? raw : 0.0f;

				const bool  paused = globals::game::ui && globals::game::ui->GameIsPaused();
				const float step = paused ? 0.0f : dt;

				if (Settings::PollForChanges(dt)) {
					ApplyReloadedSettings();
				}

				if (!RefreshTerrainActivity()) {
					guard.unlock();
					func(a1, a2);
					return;
				}

				Profiler::Frame(dt);
				Tessellation::PrepareFrame();
				Shelter::Upload();
				SnowCoverage::Update();
				if (Settings::useClipmap) { Clipmap::Update(step); }

				const auto sparkleStarted = Profiler::Ticks();
				SnowSparkle::Update(step);
				Profiler::AddCpuTicks(Profiler::CpuScope::kSparkleUpdate, Profiler::Ticks() - sparkleStarted);

				const auto bloodStarted = Profiler::Ticks();
				BloodDecals::Update();
				Profiler::AddCpuTicks(Profiler::CpuScope::kBloodUpdate, Profiler::Ticks() - bloodStarted);

				guard.unlock();
				func(a1, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_BlendedDecals
		{
			static void thunk(RE::BSShaderAccumulator* a_this, uint32_t a_renderFlags)
			{
				func(a_this, a_renderFlags);
				if (TerrainActivity::Active()) { SnowSparkle::Render(); }
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool g_installed{ false };
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		g_installed = true;

		REL::Relocation<std::uintptr_t> lighting{ RE::VTABLE_BSLightingShader[0] };
		BSLightingShader_SetupGeometry::func =
			lighting.write_vfunc(0x6, BSLightingShader_SetupGeometry::thunk);
		BSLightingShader_RestoreGeometry::func =
			lighting.write_vfunc(0x7, BSLightingShader_RestoreGeometry::thunk);

		REL::Relocation<std::uintptr_t> utility{ RE::VTABLE_BSUtilityShader[0] };
		BSUtilityShader_SetupGeometry::func =
			utility.write_vfunc(0x6, BSUtilityShader_SetupGeometry::thunk);
		BSUtilityShader_RestoreGeometry::func =
			utility.write_vfunc(0x7, BSUtilityShader_RestoreGeometry::thunk);

		auto& trampoline = SKSE::GetTrampoline();
		const auto renderDepthCall =
			REL::RelocationID(35560, 36559).address() + REL::Relocate(0x395, 0x395);

		const auto opcode = *reinterpret_cast<const std::uint8_t*>(renderDepthCall);
		if (opcode != 0xE8) {
			logger::critical(
				"Main_RenderDepth call site is not a call: expected E8 at {:X}, found "
				"{:02X}. This build's offset does not fit this Skyrim version, so the "
				"hook was NOT written and terrain deformation is off. Nothing has been "
				"modified - the game is safe to play. An updated build is needed.",
				renderDepthCall, opcode);

			Settings::enableTessellation = false;
			return;
		}

		Main_RenderDepth::func =
			trampoline.write_call<5>(renderDepthCall, Main_RenderDepth::thunk);

		const auto updateCall = REL::RelocationID(35551, 36544).address() + REL::Relocate(0x11F, 0x160);
		if (*reinterpret_cast<const std::uint8_t*>(updateCall) != 0xE8) {
			logger::critical("Main_Update call site {:X} is not E8; terrain disabled because CPU gathering cannot be installed safely", updateCall);
			Settings::enableTessellation = false;
			return;
		}
		Main_Update::func = trampoline.write_call<5>(updateCall, Main_Update::thunk);
		Clipmap::InstallActorCacheEvents();
		logger::info("Installed Main_Update CPU gathering at {:X}; render consumes prepared stamps", updateCall);

		logger::info("Installed BSLightingShader/BSUtilityShader geometry hooks and the "
		             "Main_RenderDepth field update (call site {:X}, opcode E8)",
			renderDepthCall);

		if (Settings::snowSparkle) {
			const auto blendedDecalsCall =
				REL::RelocationID(99938, 106583).address() + REL::Relocate(0x319, 0x308);
			const auto decalsOpcode =
				*reinterpret_cast<const std::uint8_t*>(blendedDecalsCall);
			if (decalsOpcode == 0xE8) {
				Main_RenderWorld_BlendedDecals::func = trampoline.write_call<5>(
					blendedDecalsCall, Main_RenderWorld_BlendedDecals::thunk);
				SnowSparkle::NoteHookInstalled(true);
				logger::info("Installed the post-opaque hook for kicked snow (call site {:X}, "
				             "opcode E8)",
					blendedDecalsCall);
			} else {
				logger::warn(
					"Post-opaque call site is not a call: expected E8 at {:X}, found {:02X}. "
					"This build's offset does not fit this Skyrim version, so the hook was NOT "
					"written and kicked snow is off. Nothing has been modified, and terrain "
					"deformation is unaffected - it runs from a different call site that "
					"installed normally.",
					blendedDecalsCall, decalsOpcode);
			}
		}
	}
}
