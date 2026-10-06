// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "ShaderCompiler.h"

#include "Clipmap.h"
#include "BlanketTessellation.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "Tessellation.h"
#include "TessellationResources.h"
#include "TerrainBlendDiagnostics.h"
#include "TerrainDepthBias.h"
#include "TerrainCulling.h"
#include "TerrainProcessingGuard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace Tessellation
{
	namespace
	{

		std::string EmitStruct(const Reflection::Signature& a_signature)
		{
			std::string out = "struct CP\n{\n";

			for (size_t i = 0; i < a_signature.elements.size(); ++i) {
				const auto& e = a_signature.elements[i];

				const std::string semantic =
					e.semanticName == "SV_POSITION" ? "SV_POSITION" : e.Semantic();

				out += std::format("\t{} f{} : {};\n", e.hlslType, i, semantic);
			}

			out += "};\n\n";
			return out;
		}

		std::string EmitPrologue(bool a_displace, Mode a_mode)
		{
			std::string out =
				"cbuffer PerFrame : register(b12)\n"
				"{\n"
				"\trow_major float4x4 CameraViewProj        : packoffset(c8);\n"
				"\trow_major float4x4 CameraViewProjInverse : packoffset(c32);\n"

				"\tfloat4             CameraPosAdjust       : packoffset(c40);\n"
				"};\n\n";

			out += std::format(
				"static const float kMaxTess       = {:.1f}f;\n"
				"static const float kTargetSpacing = {:.4f}f;\n"
				"static const float kRaiseSpacing  = {:.4f}f;\n"
				"static const float kLift          = {:.6f}f;\n"
				"static const float kWaveAmp       = {:.6f}f;\n"
				"static const float kWaveLen       = {:.4f}f;\n"
				"static const float kGradEps       = {:.4f}f;\n"

				"static const float kGradFlat      = 1.0e-4f;\n\n",
				Settings::tessellationMaxFactor, Settings::tessellationTargetSpacing,
				Settings::tessellationRaiseSpacing,
				a_mode == Mode::kStaticProbe ? Settings::staticProbeOffset :
					Settings::debugWorldZOffset,
				Settings::debugWaveAmplitude,
				Settings::debugWaveLength, Settings::normalGradientEpsilon);

			const bool blanket = (a_mode == Mode::kLandscape || a_mode == Mode::kBloodDecal) && Settings::useClipmap &&
				Settings::enableSnowRaise && Settings::snowRaiseHeight > 0.0f;
			out += std::format("static const float kBlanketSpacing = {:.4f}f;\n",
				blanket ? Settings::tessellationBlanketSpacing : 0.0f);
			out += BlanketTessellation::source;

			if (Settings::enableSurfaceMaterial) {
				out += std::format(
					"cbuffer SurfaceMaterial : register(b{})\n"
					"{{\n"
					"\t\n"
					"\t\n"
					"\t\n"
					"\t\n"
					"\tfloat4 Material;\n"
					"}};\n\n",
					kMaterialSlot);
			}

			out +=
				"float3 ReconstructWorld(float4 clipPosition)\n"
				"{\n";
			if (a_mode == Mode::kLandscape && Settings::terrainBlendingCompatibility) {

				out = "cbuffer TerrainDepthCorrection : register(b9) { float4 TerrainClipBias; };\n" + out;
				out += "\tclipPosition.z -= TerrainClipBias.x;\n";
			}
			out +=
				"\tfloat4 world = mul(CameraViewProjInverse, clipPosition);\n"
				"\treturn world.xyz / world.w;\n"
				"}\n\n";

			if (a_displace) {

				const bool field = a_mode == Mode::kLandscape || a_mode == Mode::kBloodDecal;

				const char* const lift =
					"\tfloat  d  = kLift;\n";

				if (Settings::useClipmap && field) {

					const bool coarse = Clipmap::LevelCount() > 1;

					out += std::format(
						"Texture2D<float> ClipmapHeight : register(t0);\n"
						"SamplerState     ClipmapSampler : register(s0);\n\n"
						"cbuffer ClipmapWindow : register(b{})\n"
						"{{\n"
						"\t\n"
						"\tfloat4 Window;\n"
						"\t\n"
						"\tfloat4 Raise;\n"
						"{}"
						"}};\n\n"
						"static const float kClipmapWorldSize = {:.4f}f;\n\n",
						Clipmap::kParamsSlot,
						coarse ? "\t\n"
								 "\tfloat4 Window1;\n" :
								 "",
						Clipmap::kWorldSize);

					if (coarse) {
						out += std::format(
							"\n"
							"\n"
							"\n"
							"\n"
							"Texture2D<float> ClipmapHeight1 : register(t{});\n"
							"static const float kClipmapWorldSize1 = {:.4f}f;\n\n",
							Clipmap::kLevel1Slot, Clipmap::WorldSizeFor(1));
					}
				}

				const bool raising =
					field &&
					Settings::enableSnowRaise && Settings::useClipmap &&
					Settings::snowRaiseHeight > 0.0f;

				if (raising) {
					out += std::format(
						"Texture2D<float> SnowCoverageMap : register(t{0});\n"
						"SamplerState     SnowCoverageSampler : register(s{1});\n\n"
						"static const float kCoverageWorldSize = {2:.4f}f;\n"
						"static const float kCoverageTexels    = {6:.1f}f;\n"
						"static const float kRaiseHeight       = {3:.4f}f;\n"
						"static const float kRaiseFadeEnd      = {4:.4f}f;\n"
						"static const float kRaiseFadeStart    = {5:.4f}f;\n\n",
						SnowCoverage::kSlot, SnowCoverage::kSamplerSlot,
						SnowCoverage::kWorldSize, Settings::snowRaiseHeight,
						Settings::snowRaiseDistance,
						std::max(Settings::snowRaiseDistance - Settings::snowRaiseFadeBand,
							0.0f),
						static_cast<float>(SnowCoverage::kTexels));

					if (Settings::shelterMeshCap) {
						out += std::format(
							"Texture2D<float> SnowMeshCapMap : register(t{0});\n"
							"SamplerState     SnowMeshCapSampler : register(s{1});\n\n"
							"static const float kMeshCapWorldSize = {2:.4f}f;\n"
							"static const float kMeshCapTexels    = {3:.1f}f;\n"
							"static const float kMeshCapFadeStart = {4:.4f}f;\n"
							"static const float kMeshCapFadeEnd   = {5:.4f}f;\n\n",
							Shelter::kSlot, Shelter::kSamplerSlot, Shelter::kWorldSize,
							static_cast<float>(Shelter::kTexels),

							Shelter::kWorldSize * 0.39f,
							Shelter::kWorldSize * 0.47f);
					}
				}

				out += "float SnowRaise(float3 worldPosition)\n{\n";

				if (!raising) {

					out += "\treturn 0.0f;\n}\n\n";
				} else {
					out +=
						"\tconst float2 xy = worldPosition.xy + CameraPosAdjust.xy;\n\n"

						"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
						"\tconst float  reach      = max(fromCentre.x, fromCentre.y);\n"
						"\tconst float  fade       = 1.0f - saturate(\n"
						"\t\t(reach - kRaiseFadeStart) /\n"
						"\t\tmax(kRaiseFadeEnd - kRaiseFadeStart, 1e-3f));\n"
						"\tif (fade <= 0.0f) {\n"
						"\t\treturn 0.0f;\n"
						"\t}\n\n"

						"\tfloat2 uv = xy / kCoverageWorldSize;\n"
						"\tconst float2 t = uv * kCoverageTexels + 0.5f;\n"
						"\tconst float2 i = floor(t);\n"
						"\tfloat2 f = t - i;\n"
						"\tf = f * f * (3.0f - 2.0f * f);\n"
						"\tuv = (i + f - 0.5f) / kCoverageTexels;\n\n"
						"\tconst float cover = SnowCoverageMap.SampleLevel(\n"
						"\t\tSnowCoverageSampler, uv, 0.0f);\n\n"

						"\tconst float snowy = smoothstep(0.5f, 1.0f, cover);\n"
						"\tfloat lift = snowy * kRaiseHeight * Raise.x;\n\n";

					if (Settings::shelterMeshCap) {
						out +=

							"\tfloat2 cuv = xy / kMeshCapWorldSize;\n"
							"\tconst float2 ct = cuv * kMeshCapTexels + 0.5f;\n"
							"\tconst float2 ci = floor(ct);\n"
							"\tfloat2 cf = ct - ci;\n"
							"\tcf = cf * cf * (3.0f - 2.0f * cf);\n"
							"\tcuv = (ci + cf - 0.5f) / kMeshCapTexels;\n\n"
							"\tfloat cap = SnowMeshCapMap.SampleLevel(\n"
							"\t\tSnowMeshCapSampler, cuv, 0.0f);\n\n"

							"\tcap = lerp(cap, 1.0f, saturate(\n"
							"\t\t(reach - kMeshCapFadeStart) /\n"
							"\t\tmax(kMeshCapFadeEnd - kMeshCapFadeStart, 1e-3f)));\n\n"

							"\tlift = min(lift, cap * kRaiseHeight);\n\n";
					}

					out +=
						"\treturn lift * fade;\n"
						"}\n\n";
				}

				out +=
					"\n"
					"\n"
					"\n"
					"float Displacement(float3 worldPosition)\n"
					"{\n"
					"\tfloat2 xy = worldPosition.xy + CameraPosAdjust.xy;\n";
				out += lift;

				if (Settings::useClipmap && field) {

					out +=
						"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
						"\tconst float  edgeDist   = max(fromCentre.x, fromCentre.y);\n"
						"\tconst float  inWindow   = 1.0f - saturate(\n"
						"\t\t(edgeDist - Window.z) / max(Window.w - Window.z, 1e-3f));\n\n";

					if (Clipmap::LevelCount() > 1) {

						out +=
							"\tconst float2 fromCentre1 = abs(xy - Window1.xy);\n"
							"\tconst float  edgeDist1   = max(fromCentre1.x, fromCentre1.y);\n"
							"\tconst float  inWindow1   = 1.0f - saturate(\n"
							"\t\t(edgeDist1 - Window1.z) / max(Window1.w - Window1.z, 1e-3f));\n\n"
							"\tif (inWindow1 > 0.0f) {\n"
							"\t\tfloat fine = 0.0f;\n"
							"\t\tfloat coarse = 0.0f;\n"
							"\t\t[branch] if (inWindow > 0.0f) {\n"
							"\t\t\tfine = ClipmapHeight.SampleLevel(ClipmapSampler,\n"
							"\t\t                                             xy / kClipmapWorldSize, 0.0f);\n"
							"\t\t}\n"
							"\t\t[branch] if (inWindow < 1.0f) {\n"
							"\t\t\tcoarse = ClipmapHeight1.SampleLevel(ClipmapSampler,\n"
							"\t\t                                                xy / kClipmapWorldSize1, 0.0f);\n"
							"\t\t}\n"
							"\t\td += lerp(coarse, fine, inWindow) * inWindow1;\n"
							"\t}\n";
					} else {
						out +=
							"\tif (inWindow > 0.0f) {\n"
							"\t\td += ClipmapHeight.SampleLevel(ClipmapSampler,\n"
							"\t\t                               xy / kClipmapWorldSize, 0.0f) * inWindow;\n"
							"\t}\n";
					}
				} else if (field) {
					out +=
						"\tif (kWaveAmp != 0.0f) {\n"
						"\t\tconst float k = 6.2831853f / kWaveLen;\n"
						"\t\td += kWaveAmp * sin(xy.x * k) * sin(xy.y * k);\n"
						"\t}\n";
				}

				out +=
					"\treturn d;\n"
					"}\n\n";
			}

			if (a_displace) {

				out +=
					"float SurfaceOffset(float3 worldPosition)\n"
					"{\n"
					"\treturn Displacement(worldPosition) + SnowRaise(worldPosition);\n"
					"}\n\n";
			}

			return out;
		}

		std::string EmitPatchConstants(bool a_tessellate)
		{
			std::string out =
				"struct PatchConstants\n"
				"{\n"
				"\tfloat edges[3] : SV_TessFactor;\n"
				"\tfloat inside   : SV_InsideTessFactor;\n"
				"};\n\n";

			if (!a_tessellate) {

				out +=
					"PatchConstants PatchConstantFn(InputPatch<CP, 3> patch)\n"
					"{\n"
					"\tPatchConstants o;\n"
					"\to.edges[0] = 1.0f;\n"
					"\to.edges[1] = 1.0f;\n"
					"\to.edges[2] = 1.0f;\n"
					"\to.inside   = 1.0f;\n"
					"\treturn o;\n"
					"}\n\n";
				return out;
			}

			const bool bounded = Settings::useClipmap && Settings::enableTessellationBounds;

			if (bounded) {
				out += std::format(
					"Texture2D<uint> FieldActivity : register(t{});\n\n"
					"static const float kActivityCell  = {:.6f}f;\n"
					"static const int   kActivityMask  = {};\n"
					"static const float kBoundDepth    = {:.4f}f;\n\n"

					"float ActivityAt(float2 xy)\n"
					"{{\n"
					"\tconst int2 cell = int2(floor(xy / kActivityCell)) & kActivityMask;\n"
					"\treturn asfloat(FieldActivity.Load(int3(cell, 0)));\n"
					"}}\n\n"
					"float EdgeBound(float2 a, float2 b)\n"
					"{{\n"
					"\tfloat m = ActivityAt(a);\n"
					"\tm = max(m, ActivityAt(lerp(a, b, 0.25f)));\n"
					"\tm = max(m, ActivityAt(lerp(a, b, 0.50f)));\n"
					"\tm = max(m, ActivityAt(lerp(a, b, 0.75f)));\n"
					"\tm = max(m, ActivityAt(b));\n"
					"\treturn m;\n"
					"}}\n\n",
					Clipmap::kActivitySlot,
					Clipmap::CellSizeFor(0) * static_cast<float>(Clipmap::kActivityRatio),
					Clipmap::kActivityTexels - 1,
					std::max(Settings::tessellationBoundDepth, 1e-4f));
			}

			if (Settings::useClipmap) {
				out +=
					"float EdgeFactor(float3 a, float3 b)\n"
					"{\n"
					"\tconst float length2 = length(b - a);\n"
					"\tconst float near    =\n"
					"\t\tclamp(length2 / kTargetSpacing, 1.0f, kMaxTess);\n"
					"\tconst float far     = kRaiseSpacing > 0.0f ?\n"
					"\t\tclamp(length2 / kRaiseSpacing, 1.0f, kMaxTess) : 1.0f;\n\n"
					"\tconst float2 mid  = (a.xy + b.xy) * 0.5f + CameraPosAdjust.xy;\n"
					"\tconst float2 from = abs(mid - Window.xy);\n"
					"\tconst float  reach = max(from.x, from.y);\n\n"
					"\tconst float away = saturate(\n"
					"\t\t(reach - Window.z) / max(Window.w - Window.z, 1e-3f));\n\n";

				if (bounded) {
					out +=
						"\tconst float bound  = EdgeBound(\n"
						"\t\ta.xy + CameraPosAdjust.xy, b.xy + CameraPosAdjust.xy);\n"
						"\tconst float active = saturate(bound / kBoundDepth);\n"
						"\tconst float gated = BlanketDetail(length2, far, near,\n"
						"\t\tkBlanketSpacing, kMaxTess, active);\n\n";
				}

				const char* const nearTerm = bounded ? "gated" : "near";

				if (Clipmap::LevelCount() > 1) {
					out += std::format(
						"\tconst float mid1    =\n"
						"\t\tclamp(length2 / (kTargetSpacing * {0:.1f}f), 1.0f, kMaxTess);\n\n"
						"\tconst float2 from1  = abs(mid - Window1.xy);\n"
						"\tconst float  reach1 = max(from1.x, from1.y);\n"
						"\tconst float  away1  = saturate(\n"
						"\t\t(reach1 - Window1.z) / max(Window1.w - Window1.z, 1e-3f));\n\n"
						"\treturn lerp(lerp({1}, mid1, away), far, away1);\n"
						"}}\n\n",
						Clipmap::kLevelScale, nearTerm);
				} else {
					out += std::format(
						"\treturn lerp({}, far, away);\n"
						"}}\n\n",
						nearTerm);
				}
			} else {
				out +=
					"float EdgeFactor(float3 a, float3 b)\n"
					"{\n"
					"\treturn clamp(length(b - a) / kTargetSpacing, 1.0f, kMaxTess);\n"
					"}\n\n"
				;
			}

			out +=
				"PatchConstants PatchConstantFn(InputPatch<CP, 3> patch)\n"
				"{\n"
				"\tPatchConstants o;\n"
				"\tconst float3 p0 = ReconstructWorld(patch[0].f0);\n"
				"\tconst float3 p1 = ReconstructWorld(patch[1].f0);\n"
				"\tconst float3 p2 = ReconstructWorld(patch[2].f0);\n\n"

				"\to.edges[0] = EdgeFactor(p1, p2);\n"
				"\to.edges[1] = EdgeFactor(p2, p0);\n"
				"\to.edges[2] = EdgeFactor(p0, p1);\n"
				"\to.inside   = max(o.edges[0], max(o.edges[1], o.edges[2]));\n"
				"\treturn o;\n"
				"}\n\n";

			return out;
		}

		std::string EmitHull(const std::string& a_winding)
		{
			return std::format(
				"[domain(\"tri\")]\n"
				"[partitioning(\"integer\")]\n"
				"[outputtopology(\"triangle_{}\")]\n"
				"[outputcontrolpoints(3)]\n"
				"[patchconstantfunc(\"PatchConstantFn\")]\n"
				"CP main(InputPatch<CP, 3> patch, uint id : SV_OutputControlPointID)\n"
				"{{\n"
				"\treturn patch[id];\n"
				"}}\n",
				a_winding);
		}

		int IndexOf(const Reflection::Signature& a_signature, std::string_view a_name,
			uint32_t a_index)
		{
			for (size_t i = 0; i < a_signature.elements.size(); ++i) {
				const auto& e = a_signature.elements[i];
				if (e.semanticName == a_name && e.semanticIndex == a_index) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		std::string EmitNormalRecompute(const Reflection::Signature& a_signature)
		{
			if (!Settings::recomputeNormals) {
				return {};
			}

			const int t0 = IndexOf(a_signature, "TEXCOORD", 1);
			const int t1 = IndexOf(a_signature, "TEXCOORD", 2);
			const int t2 = IndexOf(a_signature, "TEXCOORD", 3);

			if (t0 < 0 || t1 < 0 || t2 < 0) {
				return {};
			}

			return std::format(
				"\n\t\n"
				"\tconst float2 hNeighbour = float2(\n"
				"\t\tSurfaceOffset(basePos + float3(kGradEps, 0.0f, 0.0f)),\n"
				"\t\tSurfaceOffset(basePos + float3(0.0f, kGradEps, 0.0f)));\n"
				"\tconst float2 fieldGrad = (hNeighbour - rise) / kGradEps;\n\n"

				"\tif (dot(fieldGrad, fieldGrad) > kGradFlat * kGradFlat) {{\n"
				"\t\tconst float3 T0 = float3(o.f{0}.x, o.f{1}.x, o.f{2}.x);\n"
				"\t\tconst float3 B0 = float3(o.f{0}.y, o.f{1}.y, o.f{2}.y);\n"
				"\t\tconst float3 N0 = normalize(float3(o.f{0}.z, o.f{1}.z, o.f{2}.z));\n\n"

				"\t\tconst float nz = (abs(N0.z) < 1e-4f) ? (N0.z >= 0.0f ? 1e-4f : -1e-4f) : N0.z;\n"
				"\t\tconst float2 grad = (-N0.xy / nz) + fieldGrad;\n"
				"\t\tconst float3 N = normalize(float3(-grad, 1.0f));\n\n"

				"\t\tconst float  handed = (dot(cross(N0, T0), B0) < 0.0f) ? -1.0f : 1.0f;\n"
				"\t\tfloat3 T = T0 - N * dot(N, T0);\n"
				"\t\tT = (dot(T, T) > 1e-8f) ? normalize(T) : normalize(cross(float3(0.0f, 1.0f, 0.0f), N));\n"
				"\t\tconst float3 B = cross(N, T) * handed;\n\n"
				"\t\to.f{0} = float3(T.x, B.x, N.x);\n"
				"\t\to.f{1} = float3(T.y, B.y, N.y);\n"
				"\t\to.f{2} = float3(T.z, B.z, N.z);\n"
				"\t}}\n",
				t0, t1, t2);
		}

		std::string EmitBlendWeights(const Reflection::Signature& a_signature)
		{
			if (!Settings::enableSurfaceMaterial) {
				return {};
			}

			const int w1 = IndexOf(a_signature, "TEXCOORD", 6);
			const int w2 = IndexOf(a_signature, "TEXCOORD", 7);

			if (w1 < 0 || w2 < 0) {
				return {};
			}

			return std::format(
				"\n\t\n"
				"\tconst int revealLayer = (int)Material.x;\n"
				"\tconst int snowLayer   = (int)Material.z;\n"
				"\tif (revealLayer >= 0) {{\n"
				"\t\tconst float4 snowSel1 = float4(\n"
				"\t\t\tsnowLayer == 0 ? 1.0f : 0.0f, snowLayer == 1 ? 1.0f : 0.0f,\n"
				"\t\t\tsnowLayer == 2 ? 1.0f : 0.0f, snowLayer == 3 ? 1.0f : 0.0f);\n"
				"\t\tconst float2 snowSel2 = float2(\n"
				"\t\t\tsnowLayer == 4 ? 1.0f : 0.0f, snowLayer == 5 ? 1.0f : 0.0f);\n"

				"\t\tconst float total = dot(o.f{0}, 1.0f) + o.f{1}.x + o.f{1}.y;\n"
				"\t\tconst float snowW = dot(o.f{0}, snowSel1) + dot(o.f{1}.xy, snowSel2);\n"
				"\t\tconst float snowFrac =\n"
				"\t\t\tsnowLayer >= 0 ? saturate(snowW / max(total, 1e-4f)) : 1.0f;\n\n"
				"\t\tconst float blendAmount =\n"
				"\t\t\tsaturate(-shift / Material.y) * Material.w * snowFrac;\n"
				"\t\tconst float4 target1 = float4(\n"
				"\t\t\trevealLayer == 0 ? 1.0f : 0.0f, revealLayer == 1 ? 1.0f : 0.0f,\n"
				"\t\t\trevealLayer == 2 ? 1.0f : 0.0f, revealLayer == 3 ? 1.0f : 0.0f);\n"
				"\t\tconst float2 target2 = float2(\n"
				"\t\t\trevealLayer == 4 ? 1.0f : 0.0f, revealLayer == 5 ? 1.0f : 0.0f);\n"
				"\t\to.f{0} = lerp(o.f{0}, target1 * total, blendAmount);\n"

				"\t\to.f{1}.xy = lerp(o.f{1}.xy, target2 * total, blendAmount);\n"
				"\t}}\n",
				w1, w2);
		}

		std::string EmitFieldDebug(const Reflection::Signature& a_signature)
		{
			const int mode = Settings::debugFieldColour;
			if (mode <= 0) {
				return {};
			}

			const float scale = std::max(Settings::stampDepth, 1.0f);

			std::string out = std::format(
				"\n\t\n"
				"\tconst float kDebugScale = {:.4f}f;\n"
				"\tconst float kDebugFlat  = {:.4f}f;\n"
				"\tconst float debugDepth  = saturate(abs(shift) / kDebugScale);\n",
				scale, scale * 0.02f);

			if (mode == 3) {
				const int w1 = IndexOf(a_signature, "TEXCOORD", 6);
				const int w2 = IndexOf(a_signature, "TEXCOORD", 7);
				if (w1 < 0 || w2 < 0) {
					return {};
				}

				out += std::format(
					"\tif (shift < -kDebugFlat) {{\n"
					"\t\to.f{0} = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
					"\t\to.f{1}.xyz = float3(0.0f, 0.0f, 0.0f);\n"
					"\t}} else if (shift > kDebugFlat) {{\n"
					"\t\to.f{0} = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
					"\t\to.f{1}.xyz = float3(0.0f, 1.0f, 0.0f);\n"
					"\t}}\n",
					w1, w2);
				return out;
			}

			const int c0 = IndexOf(a_signature, "COLOR", 0);
			if (c0 < 0) {
				return {};
			}

			out += mode == 1 ?
					   "\tconst float debugAmount = 0.35f + 0.65f * debugDepth;\n" :
					   "\tconst float debugAmount = 1.0f;\n";

			out += std::format(
				"\tif (shift < -kDebugFlat) {{\n"
				"\t\to.f{0}.rgb *= float3(1.0f, 1.0f - debugAmount, 1.0f - debugAmount);\n"
				"\t}} else if (shift > kDebugFlat) {{\n"
				"\t\to.f{0}.rgb *= float3(1.0f - debugAmount, 1.0f, 1.0f - debugAmount);\n"
				"\t}}\n",
				c0);

			return out;
		}

		std::string EmitDomain(const Reflection::Signature& a_signature, bool a_wantDisplace,
			Mode a_mode)
		{
			const bool wantDisplace = a_wantDisplace;

			const int clipIdx = IndexOf(a_signature, "SV_POSITION", 0);
			const int worldIdx = IndexOf(a_signature, "POSITION", 1);
			const int prevWorldIdx = IndexOf(a_signature, "POSITION", 2);

			const bool displace = wantDisplace && clipIdx >= 0;

			std::string out;
			out +=
				"[domain(\"tri\")]\n"
				"CP main(PatchConstants pc, float3 bary : SV_DomainLocation,\n"
				"        const OutputPatch<CP, 3> patch)\n"
				"{\n"
				"\tCP o;\n";

			for (size_t i = 0; i < a_signature.elements.size(); ++i) {
				const auto& e = a_signature.elements[i];

				const bool isFloat =
					static_cast<D3D_REGISTER_COMPONENT_TYPE>(e.componentType) ==
					D3D_REGISTER_COMPONENT_FLOAT32;

				if (isFloat) {
					out += std::format(
						"\to.f{0} = patch[0].f{0} * bary.x + patch[1].f{0} * bary.y + patch[2].f{0} * bary.z;\n",
						i);
				} else {
					out += std::format("\to.f{0} = patch[0].f{0};\n", i);
				}
			}

			if (displace) {

				out += std::format(
					"\n\tconst float3 basePos = ReconstructWorld(o.f{0});\n"
					"\tconst float  shift   = Displacement(basePos);\n"
					"\tconst float  rise    = shift + SnowRaise(basePos);\n\n"

					"\to.f{0} += mul(CameraViewProj, float4(0.0f, 0.0f, rise, 0.0f));\n",
					clipIdx);

				if (worldIdx >= 0) {
					out += std::format("\to.f{0}.z += rise;\n", worldIdx);
				}

				if (prevWorldIdx >= 0) {
					out += std::format("\to.f{0}.z += rise;\n", prevWorldIdx);
				}

				if (a_mode == Mode::kLandscape) {
					out += EmitNormalRecompute(a_signature);
					out += EmitBlendWeights(a_signature);

					out += EmitFieldDebug(a_signature);
				}
			}


			out += "\treturn o;\n}\n";
			return out;
		}

		struct Pair
		{
			ID3D11HullShader*   hs{ nullptr };
			ID3D11DomainShader* ds{ nullptr };
			bool                failed{ false };
			std::shared_ptr<ShaderCompiler::Job> pending;
		};

		std::mutex                            g_cacheLock;
		std::unordered_map<uint64_t, Pair>    g_cache;
		std::unordered_map<uint64_t, Pair>    g_probeCache;
		std::unordered_map<uint64_t, Pair>    g_bloodCache;

		std::unordered_map<uint64_t, Pair>& CacheFor(Mode a_mode)
		{
			switch (a_mode) {
			case Mode::kStaticProbe:
				return g_probeCache;
			case Mode::kBloodDecal:
				return g_bloodCache;
			default:
				return g_cache;
			}
		}

		bool FinishPair(Pair& pair)
		{
			const auto& job = pair.pending;
			if (!job || !job->ready.load(std::memory_order_acquire)) {
				return false;
			}
			if (!job->hull || !job->domain || !job->error.empty()) {
				logger::error("Terrain shader compilation failed: {}", job->error);
				pair.failed = true;
			} else {
				const auto hs = globals::d3d::device->CreateHullShader(job->hull->GetBufferPointer(),
					job->hull->GetBufferSize(), nullptr, &pair.hs);
				const auto ds = globals::d3d::device->CreateDomainShader(job->domain->GetBufferPointer(),
					job->domain->GetBufferSize(), nullptr, &pair.ds);
				pair.failed = FAILED(hs) || FAILED(ds);
				if (!pair.failed) {
					if (!TerrainProcessingGuard::Mark(pair.hs) || !TerrainProcessingGuard::Mark(pair.ds)) {
						logger::error("Terrain processing guard: could not mark shader ownership");
					}
				}
				if (pair.failed) {
					if (pair.hs) { pair.hs->Release(); pair.hs = nullptr; }
					if (pair.ds) { pair.ds->Release(); pair.ds = nullptr; }
					logger::error("Terrain shader creation failed (hs={:08X}, ds={:08X})",
						static_cast<uint32_t>(hs), static_cast<uint32_t>(ds));
				}
			}
			logger::info("Terrain shader pair: compiler {:.2f} ms, {}", job->milliseconds,
				pair.failed ? "failed (vanilla fallback)" : "ready");
			pair.pending.reset();
			return !pair.failed;
		}

		const Pair* GetOrCreate(uint64_t a_vertexDesc, const Reflection::Signature& a_signature,
			Mode a_mode)
		{
			const std::scoped_lock lock(g_cacheLock);

			auto& cache = CacheFor(a_mode);

			const uint64_t key = Reflection::Key(a_signature);

			if (const auto it = cache.find(key); it != cache.end()) {
				return it->second.failed || it->second.pending ? nullptr : &it->second;
			}

			Profiler::Tally(Profiler::Count::kShaderBuilds);

			Pair pair{};
			const auto fail = [&]() -> const Pair* {
				pair.failed = true;
				cache[key] = pair;
				return nullptr;
			};

			if (!globals::Ready() || !a_signature.valid || a_signature.elements.empty()) {
				return fail();
			}

			const bool displace = WantsDisplacement(a_mode);
			const bool tessellate = WantsSubdivision(a_mode);

			const std::string common = EmitStruct(a_signature) +
			                           EmitPrologue(displace, a_mode) +
			                           EmitPatchConstants(tessellate);

			const std::string hsSource = common + EmitHull(Settings::tessellationWinding);
			const std::string dsSource = common + EmitDomain(a_signature, displace, a_mode);

			if (Settings::logGeneratedShaders) {
				logger::info("Generated HS for 0x{:016X}:\n{}", a_vertexDesc, hsSource);
				logger::info("Generated DS for 0x{:016X}:\n{}", a_vertexDesc, dsSource);
			}

			pair.pending = std::make_shared<ShaderCompiler::Job>();
			pair.pending->hullSource = hsSource;
			pair.pending->domainSource = dsSource;
			if (Settings::asyncShaderCompilation) {
				ShaderCompiler::GetWorker().Submit(pair.pending);
			} else {
				ShaderCompiler::Compile(*pair.pending);
				FinishPair(pair);
			}
			cache[key] = std::move(pair);
			const auto& stored = cache.at(key);
			return stored.failed || stored.pending ? nullptr : &stored;
		}

		struct MaterialCB
		{
			float material[4]{ -1.0f, 1.0f, 0.0f, 0.0f };
		};
		static_assert(sizeof(MaterialCB) % 16 == 0);

		ID3D11Buffer* g_materialCB{ nullptr };

		bool EnsureBuffer(ID3D11Buffer*& a_buffer, size_t a_size, const char* a_what)
		{
			if (a_buffer) {
				return true;
			}
			if (!globals::d3d::device) {
				return false;
			}

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_size);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			const HRESULT hr = globals::d3d::device->CreateBuffer(&desc, nullptr, &a_buffer);
			if (FAILED(hr)) {
				logger::error("Failed to create the {} buffer (0x{:08X})", a_what,
					static_cast<uint32_t>(hr));
				a_buffer = nullptr;
				return false;
			}
			return true;
		}

		void BindMaterial(ID3D11DeviceContext* a_context, const DrawMaterial& a_material)
		{
			if (!Settings::enableSurfaceMaterial || !EnsureBuffer(g_materialCB, sizeof(MaterialCB), "per-draw material")) {
				return;
			}

			const bool forced = Settings::debugBlendLayer >= 0;
			const int  layer = forced ? Settings::debugBlendLayer : a_material.revealLayer;
			const int  snow = forced ? -1 : a_material.snowLayer;
			const float scale = forced ? 1.0f : a_material.strengthScale;

			MaterialCB material{};
			material.material[0] = static_cast<float>(layer);
			material.material[1] = Settings::blendFullDepth;
			material.material[2] = static_cast<float>(snow);
			material.material[3] =
				std::clamp(Settings::blendStrength * scale, 0.0f, 1.0f);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(g_materialCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &material, sizeof(material));
				a_context->Unmap(g_materialCB, 0);
			}

			a_context->DSSetConstantBuffers(kMaterialSlot, 1, &g_materialCB);
		}

		void UnbindMaterial(ID3D11DeviceContext* a_context)
		{
			ID3D11Buffer* nullCB = nullptr;
			a_context->DSSetConstantBuffers(kMaterialSlot, 1, &nullCB);
		}

		struct SavedState
		{
			ID3D11HullShader*         hs{ nullptr };
			ID3D11DomainShader*       ds{ nullptr };
			ID3D11GeometryShader*     gs{ nullptr };
			D3D11_PRIMITIVE_TOPOLOGY  topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
		};

		SavedState g_saved{};
		SavedResources g_bloodResources{};
		ID3D11Buffer* g_biasBuffer{}, *g_savedBiasHS{}, *g_savedBiasDS{};
		bool g_biasActive{};
		TerrainCulling::Bracket g_culling;
		float g_biasValue{};
		std::unordered_map<ID3D11VertexShader*, float> g_biasShaders;
		bool       g_active{ false };

		Mode g_activeMode{ Mode::kLandscape };

		void ReleaseSaved()
		{
			if (g_saved.hs) {
				g_saved.hs->Release();
				g_saved.hs = nullptr;
			}
			if (g_saved.ds) {
				g_saved.ds->Release();
				g_saved.ds = nullptr;
			}
			if (g_saved.gs) {
				g_saved.gs->Release();
				g_saved.gs = nullptr;
			}
		}
	}

	namespace
	{

		struct BracketCpuTimer
		{
			int64_t start{ Profiler::Ticks() };

			~BracketCpuTimer()
			{
				Profiler::AddCpuTicks(Profiler::CpuScope::kDrawBracket, Profiler::Ticks() - start);
			}
		};
	}

	bool Active()
	{
		return g_active;
	}

	bool BeginDraw(uint64_t a_vertexDesc, const Reflection::Signature& a_signature,
		Mode a_mode, const DrawMaterial& a_material)
	{
		const BracketCpuTimer cpuTimer;

		if (!Settings::enableTessellation || !globals::Ready()) {
			return false;
		}

		if (g_active) {
			EndDraw();
		}

		const Pair* pair = GetOrCreate(a_vertexDesc, a_signature, a_mode);
		if (!pair) {
			return false;
		}

		auto* context = globals::d3d::context;

		context->HSGetShader(&g_saved.hs, nullptr, nullptr);
		context->DSGetShader(&g_saved.ds, nullptr, nullptr);
		context->GSGetShader(&g_saved.gs, nullptr, nullptr);
		context->IAGetPrimitiveTopology(&g_saved.topology);
		if (a_mode == Mode::kBloodDecal) { g_bloodResources.Capture(context); }

		context->GSSetShader(nullptr, nullptr, 0);

		ID3D11Buffer* frameCB = nullptr;
		context->VSGetConstantBuffers(12, 1, &frameCB);
		if (!frameCB) {
			context->PSGetConstantBuffers(12, 1, &frameCB);
		}
		if (frameCB) {
			context->HSSetConstantBuffers(12, 1, &frameCB);
			context->DSSetConstantBuffers(12, 1, &frameCB);
			frameCB->Release();
		}

		if (a_mode == Mode::kLandscape || a_mode == Mode::kBloodDecal) {
			if (Settings::useClipmap) {
				Clipmap::BindDomain(context);
				SnowCoverage::BindDomain(context);
				Shelter::BindDomain(context);
			}
			BindMaterial(context, a_material);
		}

		g_activeMode = a_mode;

		context->HSSetShader(pair->hs, nullptr, 0);
		context->DSSetShader(pair->ds, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		Profiler::GpuBegin(
			a_mode != Mode::kLandscape        ? Profiler::Scope::kOtherRouted :
			a_signature.elements.size() == 1  ? Profiler::Scope::kLandscapeDepth :
												Profiler::Scope::kLandscape);

		g_active = true;
		if (a_mode == Mode::kLandscape && Settings::terrainBlendingCompatibility) {
			if (!g_biasBuffer) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = 16; desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				const float zero[4]{};
				D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = zero;
				if (FAILED(globals::d3d::device->CreateBuffer(&desc, &initial, &g_biasBuffer))) {
					logger::error("Terrain blending: correction buffer creation failed");
					EndDraw(); return false;
				}
			}
			context->HSGetConstantBuffers(9, 1, &g_savedBiasHS);
			context->DSGetConstantBuffers(9, 1, &g_savedBiasDS);
			context->HSSetConstantBuffers(9, 1, &g_biasBuffer);
			context->DSSetConstantBuffers(9, 1, &g_biasBuffer);
			g_biasActive = true;
			ID3D11VertexShader* vs{}; context->VSGetShader(&vs, nullptr, nullptr);
			VertexShaderBound(context, vs);
			if (vs) { vs->Release(); }
		}
		if (a_mode == Mode::kLandscape && Settings::terrainBlendingCompatibility && Settings::terrainBlendingCullBackfaces) {
			g_culling.Begin(context);
			ID3D11RasterizerState* state{}; context->RSGetState(&state);
			context->RSSetState(state);
			if (state) { state->Release(); }
		}
		if (a_mode == Mode::kLandscape) { TerrainBlendDiagnostics::Begin(context, g_biasValue); }
		return true;
	}

	void EndDraw()
	{
		if (!g_active) {
			return;
		}

		const BracketCpuTimer cpuTimer;

		Profiler::GpuEnd();

		auto* context = globals::d3d::context;

		if (g_activeMode == Mode::kLandscape) { TerrainBlendDiagnostics::End(context, g_biasValue); }
		g_culling.End(context);
		if (g_biasActive) {
			context->HSSetConstantBuffers(9, 1, &g_savedBiasHS);
			context->DSSetConstantBuffers(9, 1, &g_savedBiasDS);
			if (g_savedBiasHS) { g_savedBiasHS->Release(); g_savedBiasHS = nullptr; }
			if (g_savedBiasDS) { g_savedBiasDS->Release(); g_savedBiasDS = nullptr; }
			g_biasActive = false;
		}
		if (g_activeMode == Mode::kBloodDecal) {
			g_bloodResources.Restore(context);
		} else if (g_activeMode == Mode::kLandscape) {
			if (Settings::useClipmap) {
				Clipmap::UnbindDomain(context);
				SnowCoverage::UnbindDomain(context);
				Shelter::UnbindDomain(context);
			}
			UnbindMaterial(context);
		}

		context->HSSetShader(g_saved.hs, nullptr, 0);
		context->DSSetShader(g_saved.ds, nullptr, 0);
		context->GSSetShader(g_saved.gs, nullptr, 0);
		context->IASetPrimitiveTopology(g_saved.topology);

		ReleaseSaved();
		g_saved.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		g_active = false;
	}

	void VertexShaderBound(ID3D11DeviceContext* context, ID3D11VertexShader* shader)
	{
		if (context != globals::d3d::context || !g_biasActive || !g_active) { return; }
		float bias = 0.0f;
		if (shader) {
			if (const auto it = g_biasShaders.find(shader); it != g_biasShaders.end()) {
				bias = it->second;
			} else if (const auto bytes = ShaderRegistry::For(shader)) {
				ID3DBlob* assembly{};
				if (SUCCEEDED(D3DDisassemble(bytes.data, bytes.size, 0, nullptr, &assembly)) && assembly) {
					bias = TerrainDepthBias::Recognize(std::string_view(
						static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize()));
					assembly->Release();
				}

				g_biasShaders.emplace(shader, bias);
				if (bias != 0) { logger::info("Terrain blending correction: validated CS offset shader recognized; clip bias={}", bias); }
			}
		}
		if (bias != g_biasValue) {
			const float data[4]{ bias, 0, 0, 0 };
			context->UpdateSubresource(g_biasBuffer, 0, nullptr, data, 0, 0);
			g_biasValue = bias;
		}
	}

	ID3D11RasterizerState* RasterizerFor(ID3D11DeviceContext* context, ID3D11RasterizerState* state)
	{
		if (context != globals::d3d::context || !g_active || !g_culling.Active()) { return state; }
		auto* selected = g_culling.Select(globals::d3d::device, state);
		if (selected != state) {
			static bool reported{};
			if (!reported) { reported = true; logger::info("Terrain culling test: no-cull terrain request changed to back-face culling"); }
		}
		return selected;
	}

	bool WantsDisplacement(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::kStaticProbe:
			return Settings::staticProbeOffset != 0.0f;
		case Mode::kLandscape:
		case Mode::kBloodDecal:
			return Settings::debugWorldZOffset != 0.0f ||
				   Settings::debugWaveAmplitude != 0.0f ||
				   (Settings::useClipmap && Clipmap::Ready());
		default:

			return false;
		}
	}

	bool WantsSubdivision(Mode a_mode)
	{

		return (a_mode == Mode::kLandscape || a_mode == Mode::kBloodDecal) && WantsDisplacement(a_mode);
	}

	void PrepareFrame()
	{

		if (!globals::Ready()) { return; }
		const int64_t started = Profiler::Ticks();
		const std::scoped_lock lock(g_cacheLock);
		for (auto* cache : { &g_cache, &g_probeCache, &g_bloodCache }) {

			const bool waiting = std::any_of(cache->begin(), cache->end(), [](const auto& entry) {
				return entry.second.pending &&
					!entry.second.pending->ready.load(std::memory_order_acquire);
			});
			if (waiting) { continue; }
			for (auto& [key, pair] : *cache) {
				if (pair.pending) { FinishPair(pair); }
			}
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kShaderPrepare, Profiler::Ticks() - started);
	}

	void Reset()
	{
		if (g_active) { EndDraw(); }
		g_culling.Reset();
		if (g_biasBuffer) { g_biasBuffer->Release(); g_biasBuffer = nullptr; }
		g_biasValue = 0;
		g_biasShaders.clear();
		TerrainBlendDiagnostics::Reset();
		const std::scoped_lock lock(g_cacheLock);
		ShaderCompiler::GetWorker().CancelQueued();
		for (auto* cache : { &g_cache, &g_probeCache, &g_bloodCache }) {
			for (auto& [desc, pair] : *cache) {
				if (pair.hs) {
					pair.hs->Release();
				}
				if (pair.ds) {
					pair.ds->Release();
				}
			}
			cache->clear();
		}

		if (g_materialCB) {
			g_materialCB->Release();
			g_materialCB = nullptr;
		}
	}
}
