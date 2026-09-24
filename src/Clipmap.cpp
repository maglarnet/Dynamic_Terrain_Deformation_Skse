// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ActorShapes.h"
#include "Clipmap.h"
#include "ContactSampler.h"
#include "ContactPoint.h"

#include "ClipmapUpdateCS.h"
#include "Globals.h"
#include "HeatSources.h"
#include "LogBudget.h"
#include "MagicImpacts.h"
#include "MeshGeometry.h"
#include "ObjectStamps.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "SnowSurface.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
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

			// Mirrors StampNoise in ClipmapUpdateCS.h.  The field order here
			// must match the cbuffer there exactly: the shader binds by
			// byte offset, so a field inserted anywhere but the end silently
			// reinterprets every field after it.
			float stampNoise[kMaxStamps][4]{};

			float coarse[4]{};

			float rimShape[4]{};
			float snowRim[4]{};

			float raise[4]{};

			float raiseWindow[4]{};

			// Mirrors MarkRimJitter in ClipmapUpdateCS.h.  Appended rather
			// than squeezed into a spare lane, because there is no spare
			// lane: every channel of every existing field is read by the
			// shader, and a field inserted anywhere but the end would
			// silently reinterpret everything after it by byte offset.
			//
			// x is how far a line mark's rim edge may be pushed in or out,
			// as a fraction of the mark's own width; y is the wavelength of
			// that noise in world units.  z and w are reserved so the next
			// rim-shaped term does not have to widen the buffer again.
			float markRimJitter[4]{};
		};
		static_assert(sizeof(ParamsCB) % 16 == 0);

		ID3D11Texture2D*           g_texture[kMaxLevels]{};
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
				_context->CSGetShaderResources(0, 5, _srv);
				_context->CSGetSamplers(0, 1, &_sampler);
			}

			~ComputeStageGuard()
			{
				const UINT noOffset[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1),
					static_cast<UINT>(-1) };
				_context->CSSetShader(_shader, nullptr, 0);
				_context->CSSetConstantBuffers(0, 1, &_cb);
				_context->CSSetUnorderedAccessViews(0, 3, _uav, noOffset);
				_context->CSSetShaderResources(0, 5, _srv);
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

			ID3D11ShaderResourceView*  _srv[5]{};
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
			decayDesc.Format = DXGI_FORMAT_R8G8_UNORM;

			decayDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

			const std::vector<uint8_t> decayZeros(static_cast<size_t>(kTexels) * kTexels * 2, 0);
			D3D11_SUBRESOURCE_DATA decayInitial{};
			decayInitial.pSysMem = decayZeros.data();
			decayInitial.SysMemPitch = kTexels * 2 * sizeof(uint8_t);

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

		// ------------------------------------------------------------------
		// Where a carried object actually touches the ground.
		//
		// This is the whole of the "the furrow does not move with the gait"
		// fix, and it is deliberately short.  The measurement it needs is
		// already live: ActorShapes::GetExtent and ::GetLongAxis read the
		// hkpRigidBody's own transform through GetMaximumProjection, and the
		// animation drives that body every frame, so both numbers move with
		// the pose on their own.  The recorded run says so - over 1224 frames
		// the bow's bound bottom spanned 25.9 units of height and its long
		// axis turned up to 22.6 degrees between consecutive frames.
		//
		// What was wrong was not the numbers, it was that Clipmap.cpp threw
		// them away.  The stamp was placed at the bounding box's centre,
		// which is one point rigid to the body: the log recorded it holding
		// at side +17 to +31 while the weapon's lowest point swung through
		// twelve units of height.  A walk therefore marked the same place as
		// a sprint, which is the complaint exactly.
		//
		// So the contact point is rebuilt from the two live numbers instead:
		// step half a length along the measured axis, both ways, and keep the
		// end that is lower.  That end is the end in the snow, and it moves
		// when the arm swings because half of it is the transform.
		//
		// One consequence worth stating because it is what makes the gate
		// agree with this: half.Length * axis is a support-function step, and
		// for a capsule the support function along its own axis is exactly
		// half.Length - so the derived point IS the shape's lowest point, and
		// the height gate below and the position used for the stamp are the
		// same question answered once.
		struct Contact
		{
			RE::NiPoint3 point{};   // world position where the shape meets the ground
			RE::NiPoint3 axis{};    // unit long axis in world space, if known
			float        farthest{ 0.0f };  // highest of the two ends, for the clearance
			bool         valid{ false };
		};

		Contact LowestEnd(const RE::NiPoint3& a_centre, const ActorShapes::Extent& a_extent,
			const RE::NiPoint3& a_axis, bool a_axisOk)
		{
			// The arithmetic itself lives in ContactPoint.h, with no engine
			// types in it, so the offline test can call the same function
			// rather than a copy of it.  Everything here is the glue: an
			// engine point in, an engine point out.
			const auto r = ContactPoint::LowestEnd(a_centre.x, a_centre.y, a_centre.z,
				a_extent.length, a_axis.x, a_axis.y, a_axis.z, a_axisOk);

			Contact out{};
			out.point = RE::NiPoint3{ r.x, r.y, r.z };
			out.axis = a_axisOk ? a_axis : RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
			out.farthest = r.farZ;
			out.valid = true;
			return out;
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

		// Shape-level report for one collidable the actor walk accepted.
		//
		// The foot test above prints only a radius, and a radius cannot say
		// what a collidable *is*: a boot and a bow both produce one number.
		// Choosing between them needs the long axis and the thickness, which
		// is the whole reason ActorShapes keeps them apart.  The axis
		// direction is printed too, because a shape that lies along the
		// movement direction is what turns into a trench, and one that stands
		// upright cannot.
		// Shape-level report for one collidable the actor walk accepted.
		//
		// A session-wide cap was the wrong shape for this probe.  The first
		// run spent its whole budget inside the first four frames, so the
		// frames after the bow was drawn were never recorded - which is
		// exactly the window the question is about.  The cap is now applied
		// per frame, and each frame also prints a one-line tally of the
		// ratios it saw, so a frame with a bow in it is recognisable even
		// when the per-collidable lines are capped.
		std::atomic<uint32_t> g_collidablesLogged{ 0 };
		constexpr uint32_t    kMaxCollidablesLogged = 24;

		// The tally for the frame being walked.  GatherStamps clears it and
		// prints it once per frame, so it never accumulates across frames.
		std::vector<float> g_frameRatios;
		std::vector<int>   g_frameTypes;

		void LogCollidable(float a_side, float a_bottomAbove, const ActorShapes::Extent& a_extent,
			const RE::NiPoint3& a_axis, int a_shapeType, const char* a_step)
		{
			if (!Settings::logStampFeet) {
				return;
			}

			// How long is the shape relative to how thick, as a ratio.  This
			// is the same number IsShaftShape tests against 4.0, so the log
			// and the rule read identically.
			const float ratio =
				(a_extent.thickness > 0.0f) ? a_extent.length / a_extent.thickness : 0.0f;

			g_frameRatios.push_back(ratio);
			g_frameTypes.push_back(a_shapeType);

			if (g_collidablesLogged.load() >= kMaxCollidablesLogged ||
				g_collidablesLogged.fetch_add(1) >= kMaxCollidablesLogged) {
				return;
			}

			logger::info("Collidable: side {:+7.2f} bottom {:+7.2f} | type {} step {:<12} | "
						 "length {:6.2f} thickness {:6.2f} ratio {:6.2f} | axis ({:+.2f},{:+.2f},{:+.2f})",
				a_side, a_bottomAbove, a_shapeType, a_step, a_extent.length, a_extent.thickness,
				ratio, a_axis.x, a_axis.y, a_axis.z);
		}

		// One line per frame: how many shapes this frame's actor walk
		// accepted, and the ratio each one had.  A foot sits near 1-4; a
		// bow, being long and thin, would sit far above IsShaftShape's 4.0
		// if it ever reaches this walk at all.
		void LogCollidableTally()
		{
			if (!Settings::logStampFeet || g_frameRatios.empty()) {
				g_frameRatios.clear();
				g_frameTypes.clear();
				return;
			}

			std::string ratios;
			for (size_t i = 0; i < g_frameRatios.size(); ++i) {
				if (i > 0) {
					ratios += ' ';
				}
				ratios += std::format("{:.2f}", g_frameRatios[i]);
			}

			std::string types;
			for (size_t i = 0; i < g_frameTypes.size(); ++i) {
				if (i > 0) {
					types += ',';
				}
				types += std::to_string(g_frameTypes[i]);
			}

			const auto shafty = static_cast<uint32_t>(std::count_if(g_frameRatios.begin(),
				g_frameRatios.end(), [](float a_r) { return a_r >= 4.0f; }));

			logger::info("Shape tally: n={} shafty={} ratios [{}] types [{}]", g_frameRatios.size(),
				shafty, ratios, types);

			g_frameRatios.clear();
			g_frameTypes.clear();
		}

		// What a carried shaft is stamped as, in the units the shader reads.
		//
		// This exists because the shaft branch is skipped by the foot code
		// that would otherwise have logged it, and LogStampSize only fires for
		// kPrint stamps - so without this the numbers that decide how wide the
		// trail is would never appear in the log at all.
		//
		// These lines used to be spent against four one-shot budgets - 24
		// marks, 12 refusals, 32 mesh lines and 8 "not measured" lines - and a
		// single stretch of play can spend all four inside its first five
		// seconds, after which no shaft line appears at all.  That is
		// precisely the stretch a reader asks about when a carried weapon
		// stops leaving a furrow.  A rate limit covers the whole session
		// instead: two and a half lines a second each, still readable, and no
		// busy second can spend a budget that was meant to last.
		constexpr int64_t kShaftLineGapMs = 400;

		int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// One timestamp per line, because the four answer different questions
		// and a shared clock would let the busiest one starve the others -
		// which is the same reason the four budgets were kept apart.
		//
		// Plain integers rather than atomics: these are written from the same
		// single-threaded walk that already owns g_frameRatios.
		int64_t g_shaftStampLineAt = 0;
		int64_t g_shaftClearLineAt = 0;
		int64_t g_meshLineAt = 0;
		int64_t g_noMeshLineAt = 0;

		// What the rate-limited lines cannot say between them.
		//
		// Twelve refusals inside four tenths of a second say that refusals
		// happened; they do not say how the weapon was carried, and the two
		// readings of them - the gate is stricter than it should be, or the
		// weapon was never near the snow - call for opposite fixes.  The
		// window keeps the closest a refusal came to the limit, which is the
		// one number that separates the two.
		LogBudget::ShaftWindow g_shaftWindow;

		// How much larger than the collision hull a measured mesh box may be
		// before it is treated as a reading of something other than the
		// object.  The rule itself lives in MeshShape.h as MeshTrusted, so
		// the carried weapon and the dropped object in ObjectStamps.cpp
		// cannot drift apart.

		// How much play one summary line covers.
		constexpr int64_t kShaftTallySeconds = 5;

		// What a window of carried-shaft decisions adds up to.
		//
		// This is the line that answers "why is there no furrow".  The
		// per-decision lines are rate limited and a weapon is judged tens of
		// times a second, so the refusal that would explain a gap in the marks
		// is usually not the one that happened to be written.  The window
		// counts every decision, and it keeps the closest a refusal came to
		// the limit - the one number that says whether easing the limit would
		// have changed anything, as opposed to the weapon simply never having
		// been near the snow.
		//
		// On a wall-clock interval rather than per frame, because the question
		// is asked over seconds of play: one frame is far too short for "it
		// was carried clear the whole time" to be visible in it.
		void ReportShaftTally()
		{
			if (!Settings::logStampFeet) {
				g_shaftWindow.Reset();
				return;
			}

			static std::chrono::steady_clock::time_point nextCensus{};
			const auto now = std::chrono::steady_clock::now();

			// The first call only starts the clock.  Reporting the opening
			// window would describe the loading screen rather than the play,
			// and a run that starts with an empty summary trains the reader to
			// skip the line.
			if (nextCensus.time_since_epoch().count() == 0) {
				nextCensus = now + std::chrono::seconds(kShaftTallySeconds);
				return;
			}
			if (now < nextCensus) {
				return;
			}
			nextCensus = now + std::chrono::seconds(kShaftTallySeconds);

			const LogBudget::ShaftWindow window = g_shaftWindow;
			g_shaftWindow.Reset();

			if (window.Empty()) {
				return;
			}

			// Name the stage the weapon stopped at rather than leaving the
			// reader to subtract the counts: a window where nothing was seen,
			// one where a weapon was seen and never judged, and one where it
			// was judged and always refused are three different faults with
			// three different fixes.
			//
			// The limit both the verdict and the printed column are measured
			// against is the one the refusals actually failed, taken from the
			// window rather than rebuilt here.  It is no longer a constant -
			// it is derived from each weapon's own thickness (see
			// ContactPoint::ContactReach) - so a window holding two weapons of
			// different thicknesses holds two different limits, and the only
			// honest number to print is the widest of them.  The verdict uses
			// the tightest, because the question it answers is whether *any*
			// reach this window could have applied would have let the weapon
			// through: if the clearance exceeds even the strictest bound that
			// was in force, then no bound was the obstacle.
			//
			// With no refusal sampled both are zero, and GateIsBinding says
			// false for a non-positive limit without being told to.
			const float widestReach = window.WidestLimit();
			const float tightestReach = window.limitSamples > 0 ?
											window.TightestLimit() :
											widestReach;

			const char* verdict = "marks were placed where the weapon touched";
			if (window.marked == 0 && window.refused == 0) {
				verdict = window.seen > 0 ?
							  "the weapon was seen but never judged - no land to read, or the follow-pose route is off" :
							  "no carried weapon was seen at all - the shaft test rejected every object";
			} else if (window.marked == 0) {
				verdict = window.GateIsBinding(tightestReach) ?
							  "every mark was refused and the refusals sit near the limit - the gate is the thing to move" :
							  "every mark was refused and the refusals sit far above the limit - the weapon was carried clear, so no limit would help";
			}

			logger::info("Shaft tally: {}s | seen {} | judged {} | marked {} (line {}, disc {}) | "
						 "refused {} carried-clear | clearance over refusals {:.2f} best, {:.2f} worst "
						 "(limit {:.2f}) | {}",
				kShaftTallySeconds, window.seen, window.walked, window.marked, window.markedLine,
				window.marked - window.markedLine, window.refused, window.clearanceLeast,
				window.clearanceMost, widestReach, verdict);
		}

		void LogNoMesh(const char* a_why)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (!LogBudget::Allow(g_noMeshLineAt, NowMs(), kShaftLineGapMs)) {
				return;
			}

			logger::info("Shaft mesh: not measured ({}) - the collision hull's numbers "
						 "are in use",
				a_why);
		}

		// What the mesh walk found, in one line.
		//
		// This is the line that decides whether the mesh route is worth
		// having: it names the byte layout that was accepted and how far it
		// disagreed with the engine's own bounding sphere, which is the only
		// independent check on a vertex walk that reads raw bytes at a stride.
		// A wrong stride does not crash - it produces plausible floats made
		// out of a neighbouring attribute - so without this number a bad
		// reading would look exactly like a good one.
		//
		// The band range and the width it produced are on the same line as the
		// layout, so a mark that came out the wrong width can be traced to the
		// bands it was taken from rather than guessed at.
		//
		// `drop` is how far the node's lowest point sits below the dominant
		// mesh's centre line - the distance the axis walk subtracts from its
		// samples.  It is printed because it explains marks that would
		// otherwise look impossible: for a bow, whose limbs hang below its
		// middle, the centre line can miss the snow entirely while the limbs
		// are in it, and without this number a `place span` on such a shape
		// would look like the walk had invented a contact.
		void LogShaftMesh(const MeshGeometry::Result& a_mesh, const char* a_verdict,
			const MeshShape::Local& a_local, float a_t0, float a_t1, float a_toWorld,
			float a_width, bool a_haveGap, float a_gap, float a_hullSum)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (!LogBudget::Allow(g_meshLineAt, NowMs(), kShaftLineGapMs)) {
				return;
			}

			float lo = 0.0f;
			float hi = 0.0f;
			for (int i = 0; i < MeshShape::kSlabs; ++i) {
				if (i == 0 || a_local.slab[i] < lo) { lo = a_local.slab[i]; }
				if (i == 0 || a_local.slab[i] > hi) { hi = a_local.slab[i]; }
			}

			// The drop is a thickness, so it must not exceed the object's own
			// half width across its axis.  Printing that radius beside it is
			// what makes the number checkable from the line alone: a drop
			// larger than the radius is a drop contaminated by the object's
			// length, which is the fault this field now exists to catch.
			const float meshScale =
				a_mesh.local.halfLength > 0.0f ?
					(a_mesh.world.halfLength / a_mesh.local.halfLength) :
					1.0f;
			const float radial = a_mesh.local.halfWidth * meshScale;

			logger::info("Shaft mesh: {} | layout {} err {:.2f} | meshes {} verts {} "
						 "cached {} | bound r {:.2f} | union {:.1f} vs hull {:.1f} | "
						 "skin {} | lowest corner {} {:.2f} (drop {:.2f} of radial "
						 "{:.2f}, axis z {:+.2f}) | "
						 "bands {:.2f}..{:.2f} over t {:.2f}..{:.2f} x{:.3f} -> width {:.2f}",
				a_verdict, a_mesh.layout, a_mesh.error, a_mesh.geometries, a_mesh.vertices,
				a_mesh.cacheHits, a_mesh.boundRadius, a_mesh.unionX + a_mesh.unionY +
					a_mesh.unionZ,
				a_hullSum, a_mesh.skinned ? "yes" : "no",
				a_haveGap ? "above by" : "unread", a_gap, a_mesh.lowOffset, radial,
				a_mesh.world.az,
				lo, hi, a_t0, a_t1, a_toWorld, a_width);
		}

		void LogShaftCarried(float a_surfaceGap, float a_axisGap, float a_thickness,
			float a_halfLength, int a_steps, bool a_haveMesh, float a_meshGap)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (!LogBudget::Allow(g_shaftClearLineAt, NowMs(), kShaftLineGapMs)) {
				return;
			}

			// Two gaps, because they are answers to the same question from two
			// different objects.  The hull's is the centre line's less half a
			// thickness; the mesh's is the distance the object's own lowest
			// point stands above the snow.  Printing both is what says which
			// one the gate would have used, and therefore which one to trust
			// when a weapon stops marking.
			//
			// The limit printed is the derived reach rather than
			// shaftSpanMaxGap, for the same reason LogShaftStamp prints one:
			// what is in force is the thickness-scaled bound, and a reader
			// tuning the setting against this line has to see the number the
			// comparison actually used.
			logger::info("Shaft gate: carried clear of the snow - surface {:.2f} above it "
						 "against a limit of {:.2f}, so no mark (axis {:.2f}, half "
						 "thickness {:.2f}, half length {:.2f}, {} steps) | mesh {} "
						 "lowest corner {:.2f} above it",
				a_surfaceGap,
				ContactPoint::ContactReach(a_thickness, Settings::shaftSpanMaxGap,
					Settings::shaftStampMaxRadius),
				a_axisGap, a_thickness,
				a_halfLength, a_steps, a_haveMesh ? "yes" : "no", a_meshGap);
		}

		void LogShaftStamp(float a_radius, float a_halfWidth, float a_length, float a_thickness,
			float a_rim, float a_depthScale, const RE::NiPoint3& a_at,
			const RE::NiPoint3& a_centre, float a_lowest, float a_land,
			float a_poseAbove, const char* a_axisSource, const char* a_placement,
			float a_axisT, const ContactPoint::Span& a_span)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			if (!LogBudget::Allow(g_shaftStampLineAt, NowMs(), kShaftLineGapMs)) {
				return;
			}

			// The disc is what the old code drew: the cylinder case made the
			// radius the half length, so that is the number being replaced.
			const float wasRadius = std::hypot(a_length, a_thickness);

			// How far the mark was moved from the bound centre, and how far
			// the weapon's lowest point was from the ground.  These are the
			// two facts the carried-weapon fix turns on: a centre the mark
			// should not have been placed at, and a weapon that is often not
			// down at all.  Printing the offset rather than only the final
			// position is what lets a wrong placement be told from a right one
			// without knowing where the character was standing.
			const float moved = std::hypot(a_at.x - a_centre.x, a_at.y - a_centre.y);

			// Which of the two shapes was actually handed to the shader, told
			// apart the same way the shader tells them apart: a non-zero
			// halfWidth is the line (ClipmapUpdateCS.h StampDistance takes its
			// ellipse path on shape.z) and a zero one is the disc.  Printed
			// rather than inferred by the reader, because "radius 40" and
			// "radius 2" mean opposite things about how wide the mark is.
			//
			// The "end" field is a height *above the snow* and only the
			// crossing, near and centre routes produce one.  The span route
			// sets it to the buried stretch's depth, which is a distance
			// *under the snow* and is already printed as `deep` a few columns
			// later - so on that route the field used to appear twice, once
			// under a name that contradicted the other.  It reads as a
			// statement that the weapon's end is 52 units above the snow while
			// the same line says its lowest point is 7.81 under it, and a
			// reader has to know the placement column to disbelieve it.
			// Nothing is lost by omitting it there: `deep` carries the depth
			// and `lowest` carries the contact.
			const std::string endField =
				std::string_view{ a_placement } == "span" ?
					std::string{} :
					std::format("end {:.2f} above snow | ", a_poseAbove);

			// The limit actually applied, not the constant it was built from.
			//
			// The gate no longer compares against shaftGroundClearance
			// directly - it uses that value as the floor of a reach derived
			// from the weapon's own thickness, so that the bound scales with
			// the object rather than with the weather (see
			// ContactPoint::ContactReach).  Printing the raw setting beside a
			// measurement taken against the derived one would be the same
			// fault the `rim` field was fixed for, one level up: the line
			// would carry a number that is not the number in force, and a
			// reader tuning it would be tuning something the code does not use.
			const float reach = ContactPoint::ContactReach(a_thickness,
				Settings::shaftGroundClearance, Settings::shaftStampMaxRadius);

			// The axis parameter is only meaningful on the route that solved
			// for a crossing.  Everywhere else it used to print a constant
			// 0.00, which a reader takes as "the crossing is at the object's
			// own origin" instead of "no crossing was solved" - and the origin
			// is not where a carrying hand is, so the two readings send the
			// reader to opposite conclusions about where the mark went.  The
			// column is built as text so that nothing to report has its own
			// spelling, rather than borrowing a value that means something
			// else.  It is built here rather than by the caller for the same
			// reason `endField` is: the function knows which route ran, so the
			// caller cannot get the pairing wrong.
			const std::string tField = std::string_view{ a_placement } == "hit" ?
				std::format("t {:+.2f}", a_axisT) :
				std::string{ "t -" };

			// `rim` is printed because without it there is no way to tell a furrow
			// that was given the raised snow a footprint has from one that was
			// not.  It decides whether the heaped snow around the mark exists at
			// all, it was already being passed in, and it was being thrown away -
			// so the one field that would have answered "why is the groove bare"
			// was the one field the line did not carry.
			if (a_halfWidth > 0.0f) {
				logger::info("Shaft stamp: LINE half length {:.2f} half width {:.2f} "
						 "(hull length {:.2f}, hull thickness {:.2f}) | depth x{:.3f} "
						 "rim {:.2f} | "
						 "at ({:.1f}, {:.1f}) {:.1f} from bound centre | "
						 "lowest {:.2f} above snow (limit {:.2f}) | "
						 "{}axis {} | place {} {} | "
						 "span {:.1f} deep {:.2f} gap {:.2f} resid {:.2f}",
				a_radius, a_halfWidth, a_length, a_thickness, a_depthScale, a_rim,
				a_at.x, a_at.y, moved, a_lowest - a_land,
				reach, endField,
				a_axisSource, a_placement, tField,
				a_span.length, a_span.depth, a_span.gap, a_span.resid);
				return;
			}

			logger::info("Shaft stamp: circle r {:.2f} world units "
					 "(from thickness {:.2f}, shape length {:.2f}) | was a disc of "
					 "r {:.2f} | depth x{:.3f} rim {:.2f} | "
					 "at ({:.1f}, {:.1f}) {:.1f} from bound centre | "
					 "lowest {:.2f} above snow (limit {:.2f}) | "
					 "{}axis {} | place {} {} | "
					 "span {:.1f} deep {:.2f} gap {:.2f} resid {:.2f}",
			a_radius, a_thickness, a_length, wasRadius, a_depthScale, a_rim,
			a_at.x, a_at.y, moved, a_lowest - a_land,
			reach, endField,
			a_axisSource, a_placement, tField,
			a_span.length, a_span.depth, a_span.gap, a_span.resid);
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

			RE::BSVisit::TraverseScenegraphCollision(
				root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
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

					// A long thin collidable - a bow stowed on the back, a
					// quiver, a sheathed weapon - is not a foot, and stamping
					// it at foot size is what widened the player's trail.  The
					// cylinder case reports its radius as its half length
					// (ActorShapes.cpp:274-279), so a 57.7-unit bow was being
					// pressed into the snow as a 28.9-unit disc, wider than
					// the 18.69 a foot gets.
					//
					// It is still stamped, just small: the branch below sizes
					// it from its own thickness instead.  ActorShapes measured
					// the bow at length 57.72 and thickness 2.02 against the
					// feet's 3.37 at worst, so IsShaftShape's factor of four
					// separates them with room to spare - nothing in the log
					// landed between 3.37 and 28.58.
					ActorShapes::Extent shapeExtent{};
					const bool haveExtent = Settings::stampShaftShape &&
						ActorShapes::GetExtent(a_object, centre, shapeExtent);
					const bool isShaft = haveExtent &&
						ContactSampler::IsShaftShape(shapeExtent.length, shapeExtent.thickness);

					// Every object the shaft test accepted, counted here,
					// before anything can send it back.  This is the window's
					// denominator: a window where this is non-zero and the
					// judged count below is still zero holds a weapon that
					// never reached the walk, which is a different fault from
					// one that reached it and was refused.
					if (isShaft) {
						g_shaftWindow.Notice();
					}

					// The object's own mesh, when it can be read.
					//
					// Everything above this line measures the collision hull,
					// and a hull is a capsule or a box.  Two weapons of the
					// same length and thickness therefore get the same hull
					// and leave the same mark, whatever their meshes look like
					// - so a bow's curve never reaches the snow at all, and
					// "the shape of the furrow" is the half of the problem
					// that no amount of measuring the hull can answer.  The
					// mesh has it: BSTriShape keeps a CPU-side copy of its
					// vertices, so the object's own geometry can be read
					// rather than inferred.
					//
					// This is only reached for objects already classified as
					// carried weapons, and the fit is cached per mesh - a
					// mesh's own shape does not change, only the transform
					// over it - so it costs one vertex walk per weapon per
					// session and eight point transforms per frame after that.
					MeshGeometry::Result mesh{};
					bool                 haveMesh = false;

					if (isShaft && Settings::stampFromMesh) {
						const bool measured = MeshGeometry::Measure(
							MeshGeometry::SceneObject(a_object), mesh);

						// Two reasons to refuse, and both are about the node
						// rather than about the mesh.  A skinned mesh is
						// deformed on the GPU, so its node transform says
						// nothing about where its vertices ended up.  And a
						// box far larger than the hull it stands for means the
						// node held more than the object, and a mark derived
						// from it would be worse than the hull's.
						//
						// Refusing is not a failure.  The hull is what this
						// code used before the mesh could be read at all, so a
						// refused reading costs the shape and not the
						// placement, and the log says which it was.
						if (measured && !mesh.skinned) {
							const float hull = shapeExtent.length + shapeExtent.thickness;
							const float box = mesh.unionX + mesh.unionY + mesh.unionZ;

							haveMesh = MeshShape::MeshTrusted(box, hull);

							if (!haveMesh) {
								LogShaftMesh(mesh,
									"refused: union box far larger than the hull",
									MeshShape::Local{}, -1.0f, 1.0f, 1.0f, 0.0f, false,
									0.0f, hull);
							}
						} else {
							LogNoMesh(mesh.skinned ? "skinned mesh" :
							                       "no geometry, no CPU-side vertices, or no layout agreed");
						}
					}

					// The live axis, read before anything needs it.  It used
					// to be read much further down, inside the log block, and
					// the branch that positions the stamp could not see it.
					RE::NiPoint3 poseAxis{ 1.0f, 0.0f, 0.0f };
					const bool   poseAxisOk = Settings::shaftFollowPose && isShaft &&
						ActorShapes::GetLongAxis(a_object, poseAxis);

					// The three numbers the shaft path runs on - where the
					// object is, which way its long axis points, and how far
					// it reaches - taken from the mesh when one was read and
					// from the hull otherwise.  Everything below uses these,
					// so the two sources differ only in what fed them and not
					// in what is done with them.
					const RE::NiPoint3 shaftCentre = haveMesh ?
						RE::NiPoint3{ mesh.world.cx, mesh.world.cy, mesh.world.cz } :
						centre;

					const RE::NiPoint3 shaftAxis = haveMesh ?
						RE::NiPoint3{ mesh.world.ax, mesh.world.ay, mesh.world.az } :
						poseAxis;

					const float shaftHalf = haveMesh ? mesh.world.halfLength : shapeExtent.length;

					// A mesh axis is a measurement, so it needs no flag; the
					// hull's axis is a reading that can fail, and poseAxisOk
					// says whether it did.
					const bool shaftAxisOk = haveMesh || poseAxisOk;

					// The point on the object that is nearest the snow, on
					// the same live numbers the axis came from.  When the axis
					// is unknown this degrades to the bound centre, which is
					// what the old code used unconditionally.
					Contact pose = isShaft ?
						LowestEnd(shaftCentre, shapeExtent, shaftAxis, shaftAxisOk) :
						Contact{ centre, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, 0.0f, true };

					// A measured mesh knows where its own lowest point is, so
					// the fallback route stops constructing one.  The lowest
					// corner of the object's own box is the object's lowest
					// point to box accuracy, and it needs neither a half
					// length nor an axis to find - which matters, because
					// those are exactly the two numbers the constructed answer
					// gets wrong for a long weapon held at an angle.
					if (haveMesh) {
						pose.point = RE::NiPoint3{ mesh.lowX, mesh.lowY, mesh.lowZ };
					}

					// A carried weapon only marks the snow when it is actually
					// against it.
					//
					// The foot clearance gate above cannot do this job.  It
					// asks whether a planted foot is a hair off the ground,
					// and half a unit is the right answer to that; a weapon
					// rides the skeleton and swings with the gait, so its
					// lowest point travels further vertically than that in a
					// single stride.  Measured over 1224 frames, the bow's
					// bound bottom spanned -13.52 to +12.41 units and sat
					// above the ground on 919 of them - 75% of the time -
					// while being stamped 41 times a second.  Three quarters
					// of those marks were laid at a point in midair.
					//
					// The gate asks the question about the *same point the
					// mark will be placed at*.  Asking about the bound's
					// bottom while stamping somewhere else is how a weapon
					// could pass this gate and still leave a mark in the air:
					// the two are different points on the same object, and
					// the log showed the bound centre holding at side +22
					// while the weapon's lowest end swung through twelve
					// units of height.  ShaftGateAtContact ties them together.
					//
					// The gate is on the shape's own lowest point, which is
					// what being "down" means for a long object.  This is also
					// the gate that makes the mark follow the step rather than
					// sit still: the weapon's lowest point is the part in the
					// snow, and that part moves with the stride, so a walk and
					// a sprint now leave their marks in different places.
					//
					// It compares against the *snow*, and its limit is the
					// object's own radius.  Both halves of that were wrong
					// before, in the same direction.
					//
					// The surface: GetLandHeight returns the terrain, and the
					// snow is a blanket stacked on top of it - SnowRaiseHeight
					// is 35 units here, and ObjectStamps.cpp already reads the
					// surface an object rests on as `landZ +
					// SnowSurface::LiftAt(x, y)`.  Measuring against the land
					// therefore asks whether the weapon has reached the
					// terrain, not whether it has touched the snow.
					//
					// The limit: a fixed 2 units is a statement about the
					// weather, and the weather is what is being measured.  A
					// blanket tens of units thick needs a limit that scales
					// with the thing being tested instead, which is the
					// object: a rod r units thick has its side in the snow
					// once its lowest point is within r of it, and is still
					// carried once it is above that.
					//
					// shaftGroundClearance is kept as the floor of that reach
					// so the value in the INI still means something - it is
					// now "the least a weapon may be off the snow by" rather
					// than "the whole answer".
					const float shaftLowest = centre.z - radius;
					float       shaftLand = 0.0f;
					const bool  haveShaftLand = tes && tes->GetLandHeight(centre, shaftLand);

					if (haveShaftLand) {
						shaftLand += SnowSurface::LiftAt(centre.x, centre.y);
					}

					if (isShaft && Settings::shaftGroundClearance > 0.0f && haveShaftLand &&
						shaftLowest - shaftLand >
							ContactPoint::ContactReach(shapeExtent.thickness,
								Settings::shaftGroundClearance,
								Settings::shaftStampMaxRadius)) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}

					// How far the object's own lowest point is above the snow,
					// when its mesh was read.
					//
					// This is the number the "is it touching" question is
					// really about, and the hull cannot produce it.  The hull
					// route walks the object's centre line and then subtracts
					// half a thickness to get back to the surface - which is
					// the same answer only for a cylinder whose axis is level,
					// and is wrong by the whole radial extent for anything
					// curved.  The mesh's own box corner is the lowest point
					// of the thing itself, so it needs no correction and has
					// nothing to be wrong about.
					float meshGap = 0.0f;
					bool  haveMeshGap = false;

					if (haveMesh && tes) {
						float landAtLow = 0.0f;
						if (tes->GetLandHeight(
								RE::NiPoint3{ mesh.lowX, mesh.lowY, mesh.lowZ },
								landAtLow) &&
							std::isfinite(landAtLow)) {
							meshGap = ContactPoint::HeightAboveSnow(mesh.lowZ, landAtLow,
								SnowSurface::LiftAt(mesh.lowX, mesh.lowY));
							haveMeshGap = true;
						}
					}

					// How much of the weapon is under the snow, and where that
					// stretch is.
					//
					// The crossing that used to be computed here answered
					// "where does it touch" and did it exactly, but only on
					// level ground and only for one point.  Three things were
					// still being guessed on top of it: how long to draw the
					// mark (ShaftContactLineLength, one value for every weapon
					// and every pose), where along the mark to put it (the
					// single crossing, which is one end of the contact rather
					// than its middle), and whether a weapon carried just clear
					// of the snow should mark at all (the clearance constants).
					//
					// Walking the axis and asking the land under each step
					// answers all three from the ground itself.  The stretch
					// between the first and the last sample that is under the
					// surface is the contact: its length is the mark's length,
					// its middle is where the mark goes, and a weapon with no
					// such stretch along its whole length is not touching.  A
					// slope, a ridge or a curved snow surface is handled by the
					// same walk rather than by a second equation, because the
					// land height is read where the sample actually is.
					//
					// The two ends are bisected afterwards, so the offset does
					// not depend on how finely the axis was walked - the walk
					// only has to find the patch, not measure it.
					//
					// The crossing is kept below as the fallback rather than
					// deleted, and it is what ShaftSpanFromContact = 0 restores.
					ContactPoint::AxisHit hit{};
					bool                 hitOk = false;

					ContactPoint::Span span{};
					bool               spanOk = false;  // part of it is buried
					bool               nearOk = false;  // carried just clear of it
					float              surfaceGap = 0.0f;  // hull to snow, not axis to snow

					if (isShaft && Settings::shaftFollowPose && shaftAxisOk && haveShaftLand) {
						// Judged, whether or not it ends in a mark.  Placed at
						// the entry rather than at the mark so that a refusal
						// has a denominator to be read against.
						g_shaftWindow.Evaluate();

						if (Settings::shaftSpanFromContact && tes) {
							// The surface the walk measures against is the
							// snow, not the bare land under it.
							//
							// This is the one line the whole carried-weapon
							// gate turns on.  Everything downstream - the
							// buried stretch, the gap, whether the weapon is
							// touching at all - is derived from what this
							// returns, so naming the wrong surface here makes
							// every later answer wrong in the same direction.
							//
							// The plugin already knows the difference
							// elsewhere: ObjectStamps.cpp reads the surface an
							// object rests on as
							// `landZ + SnowSurface::LiftAt(x, y)`, and
							// SnowRaiseHeight is 35 units on this machine.
							// Asking GetLandHeight alone therefore measures
							// against a surface that is up to 35 units below
							// the one being walked on, which is what made a
							// weapon pressed into the snow read as carried
							// clear.
							const auto landAt = [tes](float a_x, float a_y, float& a_z) {
								float land = 0.0f;
								if (!tes->GetLandHeight(RE::NiPoint3{ a_x, a_y, 0.0f }, land)) {
									return false;
								}
								a_z = ContactPoint::SurfaceAt(land,
									SnowSurface::LiftAt(a_x, a_y));
								return true;
							};

							// The walk follows a centre line, so it can only
							// see crossings that the line itself makes.  A bow
							// carried on the back has its limbs hanging below
							// its middle, and the log shows the consequence:
							// `lowest -10.05 above land` - the bow is ten
							// units into the snow - while the same line says
							// `place near-mesh span 0.0`, no buried stretch
							// found at all.
							//
							// Handing the walk the distance down to the
							// object's own lowest point makes it ask the same
							// question the gate asks, so the two stop
							// disagreeing and the real crossings are found.
							// Zero for a mesh that is itself the lowest thing
							// in its node, which leaves that case exactly as
							// it was.
							const float lowOffset = !Settings::shaftLowOffset ?
								0.0f :
								(haveMesh ?
										std::max(mesh.lowOffset, shapeExtent.thickness) :
										shapeExtent.thickness);

							span = ContactPoint::AxisSpan(shaftCentre.x, shaftCentre.y,
								shaftCentre.z, shaftAxis.x, shaftAxis.y, shaftAxis.z,
								shaftHalf, landAt, Settings::shaftSpanSteps, lowOffset);

							if (span.known > 0) {
								// The walk reads the object's own lowest
								// surface, not its centre line: AxisSpan takes
								// its heights from `z - lowOffset`, which is
								// what the gate reads too.  So `span.gap` is
								// already the shell's distance above the snow
								// and needs no second correction.
								//
								// It used to be corrected, as
								// `SurfaceGap(span.gap, thickness)`, back when
								// the walk measured the centre line and only
								// the gate looked at the shell.  Leaving that
								// in now would subtract a thickness that has
								// already been subtracted - on a bow whose
								// `drop` is 9.14 against a 4.90 thickness it
								// would report the weapon four units deeper
								// than it is and let through one that is not
								// touching at all.  Iron law M again: one
								// reference surface, applied in one place.
								surfaceGap = span.gap;

								const float gateGap = haveMeshGap ? meshGap : surfaceGap;

								// The bound this refusal is measured against,
								// computed once so the comparison, the window
								// and the log all name the same number.
								const float spanReach =
									ContactPoint::ContactReach(shapeExtent.thickness,
										Settings::shaftSpanMaxGap,
										Settings::shaftStampMaxRadius);

								if (span.live) {
									spanOk = true;
								} else if (gateGap <= spanReach) {
									nearOk = true;
								} else {
									// Above the snow along its whole length
									// and further off than the gap allows: a
									// carried weapon, not a touching one.  This
									// is the "only when it is really touching"
									// rule, and it is now the ground's answer
									// rather than a clearance constant.
									// The clearance sampled here is the same
									// number the gate has just compared - the
									// mesh's own lowest point where there is
									// one, the hull's surface otherwise.
									// Sampling anything else would make the
									// window's "best" column disagree with the
									// limit printed beside it.
									//
									// The bound travels with it.  It is derived
									// from the weapon now, so a window that
									// stored the clearance without the bound
									// would be printing a margin against a
									// limit it never re-checked - see
									// LogBudget::ShaftWindow::Refuse.
									g_shaftWindow.Refuse(gateGap, spanReach);

									LogShaftCarried(surfaceGap, span.gap,
										shapeExtent.thickness, shapeExtent.length,
										Settings::shaftSpanSteps, haveMeshGap, meshGap);

									return RE::BSVisit::BSVisitControl::kContinue;
								}
							}
						}

						if (!spanOk && !nearOk) {
							hit = ContactPoint::AxisGroundHit(shaftCentre.x, shaftCentre.y,
								shaftCentre.z, shaftHalf, shaftAxis.x, shaftAxis.y,
								shaftAxis.z, shaftLand, true);

							if (hit.live && hit.onAxis) {
								float landThere = 0.0f;
								if (tes->GetLandHeight(RE::NiPoint3{ hit.x, hit.y, hit.z },
										landThere) &&
									std::isfinite(landThere) &&
									std::abs(landThere - shaftLand) > 0.05f) {
									const auto refined = ContactPoint::AxisGroundHit(
										shaftCentre.x, shaftCentre.y, shaftCentre.z,
										shaftHalf, shaftAxis.x, shaftAxis.y, shaftAxis.z,
										landThere, true);
									if (refined.live && refined.onAxis) {
										hit = refined;
									}
								}
								hitOk = true;
							} else if (hit.live && !hit.onAxis) {
								// The axis does cross the ground, but only beyond
								// the weapon's own length: it is carried clear of
								// the surface and nothing should be marked.  This is
								// the "only when it is actually touching" rule the
								// whole line of work is for, and it is now a
								// consequence of the geometry rather than a
								// threshold.
								return RE::BSVisit::BSVisitControl::kContinue;
							}
						}
					}

					// Which of the four routes placed the mark, and the one
					// measured number that says whether that route's answer is
					// on the snow.
					//
					// The sign convention differs by route: the buried stretch
					// and the gap are both reported as a distance *under* the
					// land (always positive), while the crossing and the end
					// fallback report a height *above* it, which is the number
					// their gate compares against.  The log prints the last of
					// those as "end ... above land" and deliberately omits it
					// on the span route, where the number would be the stretch's
					// depth under a name that says above - see the note in
					// LogShaftStamp.
					float       poseAbove = 0.0f;
					const char* placement = "centre";

					if (spanOk) {
						placement = "span";
						poseAbove = span.depth;
					} else if (nearOk) {
						placement = haveMeshGap ? "near-mesh" : "near";
						poseAbove = haveMeshGap ? meshGap : surfaceGap;
					} else if (hitOk) {
						// The mark is placed at this crossing - `px`/`py` are
						// taken from `hit` a few dozen lines down - so the
						// column has to name it.  It used to fall through to
						// "centre", which named the one route the mark was not
						// taking, and that is the route whose mark sits at the
						// bound centre.
						placement = "hit";
						// Measured, not assumed.  Writing zero here would make
						// the log's "end" field constant and the crossing's
						// own accuracy unobservable: the whole claim is that
						// the mark lands on the ground, and the only way to
						// check it is to sample the ground under the point that
						// was produced and report the difference.
						//
						// A non-zero value here means the terrain differs
						// between the bound centre - where the ground height
						// was sampled to solve the crossing - and the crossing
						// itself.  One refinement pass corrects the small
						// cases; the rest are a slope steep enough that the
						// crossing is not a reliable contact, and the mark is
						// dropped rather than drawn at a point that is not on
						// the ground.  The gate is the same number the fallback
						// uses, so the two paths agree about what "on the snow"
						// means.
						float landAtHit = 0.0f;
						if (tes->GetLandHeight(RE::NiPoint3{ hit.x, hit.y, hit.z }, landAtHit)) {
							poseAbove = ContactPoint::HeightAboveSnow(hit.z, landAtHit,
								SnowSurface::LiftAt(hit.x, hit.y));
							if (Settings::shaftGroundClearance > 0.0f &&
								poseAbove >
									ContactPoint::ContactReach(shapeExtent.thickness,
										Settings::shaftGroundClearance,
										Settings::shaftStampMaxRadius)) {
								return RE::BSVisit::BSVisitControl::kContinue;
							}
						}
					}

					if (!spanOk && !nearOk && !hitOk && isShaft &&
						Settings::shaftGateAtContact && shaftAxisOk &&
						haveShaftLand && Settings::shaftGroundClearance > 0.0f) {
						float endLand = 0.0f;
						tes->GetLandHeight(pose.point, endLand);
						poseAbove = ContactPoint::HeightAboveSnow(pose.point.z, endLand,
							SnowSurface::LiftAt(pose.point.x, pose.point.y));
						placement = "end";
						if (poseAbove >
							ContactPoint::ContactReach(shapeExtent.thickness,
								Settings::shaftGroundClearance,
								Settings::shaftStampMaxRadius)) {
							return RE::BSVisit::BSVisitControl::kContinue;
						}
					}
					if (isShaft) {
						// A carried weapon should mark the snow, but only
						// faintly.  Drawing it along its own axis made a bar
						// the length of the bow, which read as a plank laid
						// through the trail, so the stamp goes back to a
						// circle - just a much smaller one.
						//
						// The radius is the measured half thickness, not the
						// half length the cylinder case reports: the bow
						// measures 2.02 thick, so it marks a 2.02 unit circle
						// against the 28.9 it used to and the 18.69 a foot
						// gets.  Keeping it proportional to thickness lets a
						// stouter weapon read as stouter, and the caps stop
						// anything long from widening the trail again.
						//
						// This matches the pin-prick ObjectStamps.cpp already
						// gives a spent arrow (734-744), and the log says why
						// the size alone was not enough.  A bow is carried on
						// the skeleton, so it swings: over 0.585 seconds the
						// recorded bow swept 33.8 world units in side while its
						// length and thickness never moved, and the shaft was
						// stamped 41 times a second through that sweep.  A
						// small circle every frame, each one still raising a
						// rim of its own - a rim taken from RimHeight like any
						// foot's - laid a ridge along the whole arc, so an even
						// furrow and not a pinhole is what the size change on
						// its own left behind.  ShaftStampRim and the shader's
						// band width govern the ridge now, and both are set for
						// a footprint's ridge rather than a dyke.
						stamp.radius = std::clamp(shapeExtent.thickness,
							Settings::shaftStampMinRadius, Settings::shaftStampMaxRadius);

						// No half width, so the shader takes its circular
						// path (ClipmapUpdateCS.h StampDistance returns
						// length(delta) when shape.z is zero).
						stamp.halfWidth = 0.0f;

						// Logged further down, after the rim is settled - the
						// probe reports the rim that was actually used.
					}

					if (printed || oriented) {
						const RE::NiPoint3 offset{ centre.x - position.x,
							centre.y - position.y, 0.0f };
						const float side = offset.Dot(right);
						// A shaft is not a foot.  isFoot is a left/right test
						// and stampFootSeparation defaults to zero, so it is
						// true for every collidable in the skeleton - the
						// bow included.  Letting the foot branch run over a
						// shaft overwrote the axis with the character's
						// heading and set halfWidth from the bow's own half
						// length (75.0 * 0.50 = 37.5), which is wider than
						// the disc it was meant to replace.
						const bool isFoot =
							!isShaft && std::abs(side) >= Settings::stampFootSeparation;

						float reportLand = 0.0f;
						if (tes) {
							tes->GetLandHeight(centre, reportLand);
						}
						LogFootShape(side, radius, centre.z, reportLand, isFoot);

						ActorShapes::Extent extent{};
						RE::NiPoint3        axis{ 1.0f, 0.0f, 0.0f };
						const bool extentOk = ActorShapes::GetExtent(a_object, centre, extent);
						const bool axisOk = ActorShapes::GetLongAxis(a_object, axis);
						LogCollidable(side, (centre.z - radius) - reportLand, extent,
							axisOk ? axis : RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
							ActorShapes::LastShapeType(),
							extentOk ? ActorShapes::LastExtentStep() : "no-extent");

						if (isFoot) {

							if (printed) {
								stamp.kind = Stamp::Kind::kPrint;
								stamp.mirror = side < 0.0f ? -1.0f : 1.0f;
							}

							stamp.forwardX = forward.x;
							stamp.forwardY = forward.y;

							stamp.radius *= Settings::stampFootLength;
							stamp.halfWidth = stamp.radius * Settings::stampFootAspect;
						} else if (isShaft && Settings::stampShaftLine &&
								   (axisOk || haveMesh)) {

							// A carried weapon marks the snow where it touches,
							// not where its bounding box happens to be centred.
							//
							// This is the difference between a mark that
							// follows the step and one that does not.  The
							// bound centre is one point rigid to the body: it
							// sits 17 to 31 units out to the character's right
							// and it does not care what the legs are doing, so
							// a walk marked the same place as a sprint and the
							// trail read as an object laid beside the character
							// rather than carried by it.  The log showed
							// exactly that - the centre holding around side
							// +22 while the weapon's lowest point swung through
							// twelve units of height.
							//
							// The end that is lower is the end that is in the
							// snow.  Both ends are taken along the shape's own
							// axis, which ActorShapes::GetLongAxis read out of
							// the body's own rotation a few lines up, and the
							// half length the extent walk measured decides how
							// far along that axis they lie.
							if (Settings::shaftStampAtContact) {
								// Three routes, in order of how much of the
								// ground's own answer they carry.
								//
								// The buried stretch wins because its middle is
								// the middle of the contact rather than one end
								// of it.  The crossing is next, and it is exact
								// - on the ground by construction, with an
								// offset that is an output of the ground height
								// rather than a bound.  The axis's lower end is
								// the last resort, and its offset is the one
								// that has to be watched: the log measured the
								// bow's half length at 67.89 units, so an angled
								// weapon puts that end further out than the
								// character is wide.
								float px = 0.0f;
								float py = 0.0f;
								if (spanOk) {
									px = span.midX;
									py = span.midY;
								} else if (nearOk) {
									px = span.nearX;
									py = span.nearY;
								} else {
									px = hitOk ? hit.x : pose.point.x;
									py = hitOk ? hit.y : pose.point.y;
								}
								stamp.x = px;
								stamp.y = py;
							}

							// A carried weapon marks the snow along the weapon,
							// not as a dot beside it.
							//
							// The disc the isShaft branch sets above is the
							// safe answer and it is the one a player can see
							// is wrong: a rod held upright and a bow slung
							// across the back have nothing in common on the
							// ground, and both being drawn as the same 2-unit
							// circle put the rod's furrow wherever the bow's
							// happened to be.
							//
							// The axis is not guessed.  ActorShapes::
							// GetLongAxis reads it out of the body's own
							// rotation (ActorShapes.cpp:343 onwards) and picks
							// the column whose projection the shape extends
							// furthest along, so a rod angled in the hand draws
							// an angled line, and axisOk says whether it found
							// one at all.  It was already being measured a few
							// lines up for the collidable log and thrown away.
							//
							// The radius becomes the half length and halfWidth
							// the half thickness, which is exactly the ellipse
							// StampDistance already draws for a footprint: its
							// non-zero shape.z branch normalises the offset by
							// (s.z, shape.z) and returns the length in world
							// units, so the shape drawn is a capsule of half
							// length s.z and half width shape.z.  This reuses a
							// path that is already in use rather than adding a
							// fourth shape to the shader.
							//
							// The rim used to have to stay off, and the reason
							// is worth keeping: a line stamped every frame
							// along an arc banks up a ridge if each stamp
							// raises a rim of its own, which is the furrow
							// this whole line of work started from.  What made
							// it a furrow rather than a footprint's ridge was
							// the band's width - it came from the mark's half
							// length.  The band now comes from the half width
							// (ClipmapUpdateCS.h: `bandRef`), so ShaftStampRim
							// can be on without the ridge becoming a dyke.
							//
							// How long the line is matters for the same reason.
							// Drawing the whole weapon was the plank: a 68-unit
							// bow laid a 40-unit bar every frame, and because
							// the weapon swings, consecutive bars crossed each
							// other and left a fan of strips rather than a
							// trail.
							//
							// What is in the snow is the buried stretch, and
							// that is now measured rather than assumed.  It used
							// to be this line's only source of a number:
							// ShaftContactLineLength, one value for every weapon
							// and every pose, so a weapon that had driven its
							// tip twenty units under the surface and one that
							// had barely broken it drew the same bar.  The
							// stretch is taken from the ground, so a shallow
							// contact draws a short line and a deep one a long
							// one, which is the "different shapes, different
							// furrows" half of the problem.
							//
							// Half of it, because the field the shader draws
							// along is a half length: stamp.radius is the semi
							// axis and halfWidth the other, so the capsule it
							// draws is twice this.  The two caps below are kept
							// as ceilings rather than as the source - a weapon
							// whose whole length is buried would otherwise lay a
							// plank again, and ShaftContactLineLength is the one
							// that was tuned down from 40 to 12 for exactly
							// that.
							// The stretch is the mark, not half of it.
							//
							// The two quantities here are both named "length"
							// and they are not the same one, which is why this
							// conversion is easy to get backwards.  The walk
							// lays its parameters out as
							// p(t) = centre + t * half * axis with t from -1 to
							// +1, so the axis spans 2 * half and
							// (t1 - t0) * half is a length *along the object* -
							// the full stretch, whose maximum is the object's
							// whole length.  Span's own field comment says
							// "world length of the buried stretch".
							//
							// stamp.radius, on the other hand, is a semi axis:
							// ClipmapUpdateCS.h StampDistance reads it as
							// `halfLength = max(s.z, 1e-3)` and normalises the
							// along-axis distance by it, so an ellipse of radius
							// r reaches r either side of its centre.  Handing it
							// the full stretch therefore draws twice the mark,
							// and the scale below would multiply that by 2.2 on
							// top.
							//
							// Getting it wrong is not subtle in the log: with
							// the full stretch used as a semi axis, a 46-to-72
							// unit stretch is already past ShaftLineMaxLength
							// every frame, so the weapon lays one fixed 80-unit
							// bar and no pose, no weapon and no contact depth
							// can change it.  The walk's answer is computed and
							// then discarded by a ceiling.
							//
							// The conversion lives in ContactPoint.h as
							// HalfLengthFromStretch, where it can be asserted
							// rather than argued about in a comment.
							// The length decision also lives there so it can be
							// asserted.  The two routes differ in what they are
							// measuring, not in how the result is treated, so
							// they are folded into one call by handing it the
							// measured stretch where there is one and the
							// hull-derived estimate where there is not - with a
							// scale of one for the estimate, which has already
							// been through ShaftLineLengthScale by the time it
							// arrives.
							//
							// The hull route is kept as the fallback because
							// the walk can legitimately have nothing to say:
							// no CPU-side vertices, or a node whose transform
							// does not place the object.  It is the route that
							// has no measurement in it at all, so its length is
							// whatever the object's own half length scaled by
							// ShaftLineLengthScale comes to - and, crucially,
							// it keeps ShaftContactLineLength as a hard cap.
							//
							// The two routes must not share a ceiling.  The
							// whole point of the measured route is that the
							// constant stopped deciding, so there it is only
							// the floor of the ceiling and a long bow may draw
							// past it.  Here there is no measurement to trust,
							// so the constant is exactly what it always was:
							// the most this route is allowed to draw.  Without
							// that distinction a bow with no readable geometry
							// would lay 57.7 units - the full weapon - which is
							// the plank the constant existed to prevent.
							// Named `spanMeasured` rather than `measured`
							// because an outer `measured` already holds whether
							// the *mesh* was measured, and that is a different
							// question: a mesh can be measured while the span
							// walk still found nothing under the surface, which
							// is exactly the `place near-mesh span 0.0` case.
							// Shadowing it would compile and read the wrong
							// one at a glance.
							const bool spanMeasured = spanOk;
							const float hullDrawn = std::min(
								shaftHalf * Settings::shaftLineLengthScale,
								Settings::shaftContactLineLength > 0.0f ?
									Settings::shaftContactLineLength :
									shaftHalf);

							// The walk reports a length along the object; the
							// field the shader draws into holds a half length.
							// Converting here rather than after the scale, so
							// the scale multiplies the *stretch* - which is
							// what the key is documented to widen - and not a
							// half of it.
							const float drawnSource = spanMeasured ?
								ContactPoint::HalfLengthFromStretch(span.length) :
								hullDrawn;
							const float drawnScale =
								spanMeasured ? Settings::shaftSpanLengthScale : 1.0f;

							// A line much shorter than it is wide would read as
							// a dot with a direction, which is worse than the
							// dot.  Falling through keeps the disc.
							//
							// This used to compare against
							// ShaftStampMaxRadius, which is the ceiling the
							// *disc* radius is clamped to - a number chosen for
							// how fat a foot's mark may be, doing duty here as
							// how short a furrow may be.  Nothing connects the
							// two, and borrowing it meant a measured contact
							// shorter than four units drew a disc: the log has
							// `span 0.8 deep 0.79` - the bow's limb a third of
							// a unit under the snow - coming out as a 4-unit
							// circle, so a real contact left a dot.
							//
							// The honest comparison is against a width, because
							// "shorter than it is wide" is the shape that reads
							// as a dot.  The mark's own width is not settled yet
							// at this point - it is derived below from the mesh
							// bands - so the comparison uses the floor that
							// width is built from, which is the object's own
							// measured half thickness.  A weapon thinner than
							// ShaftLineMinLength still has to clear the absolute
							// minimum rather than becoming a speck with a
							// direction.
							const float minLineLength =
								std::max(Settings::shaftLineMinLength,
									shapeExtent.thickness);

							// The ceiling differs by route, and the rule for it
							// lives in ContactPoint.h so it can be asserted -
							// the caller is not covered by the offline tests,
							// and a rule that is asserted in one place and
							// applied differently in the real path is the
							// failure this codebase has already been bitten
							// by once.
							const float objectCeiling = 2.0f * shaftHalf;
							const float drawCeiling = ContactPoint::DrawCeiling(
								spanMeasured, Settings::shaftContactLineLength,
								objectCeiling);

							const float drawn = ContactPoint::DrawLength(
								drawnSource, drawnScale, drawCeiling,
								Settings::shaftLineMaxLength, minLineLength);

							if (drawn > 0.0f) {
								// The mesh's axis when there is one: it is the
								// principal direction of the object's own
								// vertices, so it follows a curve where the
								// hull's axis follows the capsule around it.
								// The hull's is kept as the fallback rather
								// than replaced because the two are read by
								// different guards - the hull's axis can be
								// refused while the mesh's stands.
								stamp.forwardX = haveMesh ? shaftAxis.x : axis.x;
								stamp.forwardY = haveMesh ? shaftAxis.y : axis.y;

								// The width comes from the object's own shape,
								// and from the measured mesh where there is
								// one, rather than from a constant.
								//
								// The constant was the last hand-picked number
								// in this path and it is the one that made
								// every weapon look the same: a 1.1 for a bow,
								// a rod and a shield boss alike.  The hull's
								// half thickness replaced it and is a real
								// measurement, but it is one number for the
								// whole object, so a bow's mark is the same
								// width where the limb is thick and where it
								// tapers to nothing.
								//
								// The mesh is banded along its own axis, so
								// the width of the mark can be the width of
								// the part of the object that is actually in
								// the snow.  This is what makes two weapons of
								// the same length and the same hull draw
								// different furrows - the answer to "the
								// shapes are all different and the furrows
								// should be too" that measuring a capsule
								// could never reach.
								//
								// The clamp is a guard rather than a tuning
								// range: a bad reading must not widen the
								// trail, and the floor is the constant this
								// replaces so an object thinner than that
								// keeps the old look instead of vanishing.
								float sourceWidth = shapeExtent.thickness;
								float meshT0 = -1.0f;
								float meshT1 = 1.0f;
								float meshToWorld = 1.0f;

								if (haveMesh && mesh.local.halfLength > 0.0f) {
									// The bands are in the mesh's own units and
									// the mark is drawn in world units, so the
									// mesh's own scale is the ratio between the
									// two half lengths - the transform's scale
									// without having to ask the transform.
									meshToWorld =
										mesh.world.halfLength / mesh.local.halfLength;

									// The stretch the mark covers, or the whole
									// object when the span walk had nothing to
									// say - which is the hull route's situation
									// rather than an empty contact.
									meshT0 = spanOk ? span.t0 : -1.0f;
									meshT1 = spanOk ? span.t1 : 1.0f;

									sourceWidth = MeshShape::WidthOver(mesh.local, meshT0,
													  meshT1) *
										meshToWorld;
								}

								// The width comes from the footprint's recipe
								// when alignment is on, and from the object's
								// own thickness when it is not.
								//
								// The thickness route is why a weapon and a
								// footprint never looked like the same gesture.
								// A footprint is an ellipse whose half width is
								// a fixed fraction of its half *length*
								// (Clipmap.cpp, foot branch), so it is about
								// 2:1 with a raised band 24 units wide.  A
								// weapon is a rod - a couple of units of
								// thickness against tens of length - so the
								// same derivation gave an up-to-20:1 sliver
								// whose band was four units, below the grid on
								// its sides.  Taking the width from the length
								// is not a tuning choice; it is what makes the
								// two marks the same shape.
								//
								// The object's own width is kept as a floor,
								// so a shield's face or a hammer's head still
								// leaves a mark as wide as it is.
								const float thicknessWidth = ContactPoint::LineWidth(
									sourceWidth, Settings::shaftLineWidthScale,
									Settings::shaftLineMinWidth, Settings::shaftLineMaxWidth);

								if (Settings::shaftMarkAlignToFoot) {
									// What a footprint's own half width comes
									// out at, from the same three numbers the
									// foot branch multiplies together.  The
									// foot branch starts from the actor's own
									// bound radius, which is not a constant the
									// settings hold, so the documented foot
									// radius stands in for it - the same
									// constant the depth derivation a few lines
									// below already uses for exactly this
									// purpose.  Using the weapon's own bound
									// here would make the mark track the weapon
									// and defeat the point.
									//
									// This goes through its own function rather
									// than LineWidth, which was the first version
									// and was wrong: LineWidth clamps to its
									// a_max, so the 0.0f that looked like "no
									// ceiling" came back as 0.0f and zeroed the
									// footprint width entirely.  Every aligned
									// mark then fell back to the thickness
									// route's six-unit ceiling and was drawn at
									// the same 24 x 6 shape whatever the weapon
									// did, which is exactly what a constant
									// shape across 96% of the log's marks looked
									// like on screen.
									const float footHalfWidth =
										ContactPoint::FootprintHalfWidth(
											Settings::shaftStampFootRadius,
											Settings::stampFootLength,
											Settings::stampFootAspect);

									// The width is the mark's own length times
									// the aspect, so the aspect is the knob and
									// a target width is reachable by arithmetic:
									// the longest mark is 24 half length, so
									// ShaftMarkFootAspect 0.35 lands on 8.4.
									//
									// The footprint's own width is used two
									// ways and neither is a floor on the aspect
									// or the knob would be dead.  It bounds the
									// result, so no mark of any length can come
									// out wider than a footprint - and that
									// bound is what makes the long mark stop
									// growing, which is what stops a long thin
									// mark from stacking into teeth.  It is also
									// a floor for a mark longer than a whole
									// footprint, where matching the lengths and
									// letting the width follow is the point of
									// the alignment; a mark shorter than that
									// keeps its own width, because a stub of a
									// weapon should leave a stub, not a foot.
									const float footCeiling =
										ContactPoint::AlignedWidthCeiling(
											Settings::shaftLineMaxWidth,
											footHalfWidth);

									float alignedWidth = ContactPoint::LengthDerivedWidth(
										drawn, Settings::shaftMarkFootAspect,
										Settings::shaftLineMinWidth, footCeiling);

									if (drawn > 2.0f * footHalfWidth &&
										alignedWidth < footHalfWidth) {
										alignedWidth = footHalfWidth;
									}

									// An object thicker than the aligned width
									// keeps its own width, so a plank or a
									// shield's edge is not slimmed to a foot.
									if (thicknessWidth > alignedWidth) {
										alignedWidth = thicknessWidth;
									}

									stamp.halfWidth = alignedWidth;
								} else {
									stamp.halfWidth = thicknessWidth;
								}

								// The length is capped against the width it
								// was just given, because a mark that is long
								// and thin is the one that stacks into teeth:
								// re-stamped every frame while the character
								// walks, each frame shifts the ellipse a
								// little and cuts a new groove beside the
								// last.  Capping the ratio turns that rake
								// back into a row of overlapping foot-shaped
								// ovals - one furrow - and is also what stops
								// the raised band from being squashed on the
								// mark's sides.
								const float cappedLength =
									Settings::shaftMarkAlignToFoot ?
									ContactPoint::AspectCappedLength(drawn,
										stamp.halfWidth,
										Settings::shaftMarkMaxAspect) :
									drawn;

								stamp.radius = cappedLength;

								// Logged after the width has been settled, so
								// the number in the line is the one the mark
								// was drawn at rather than the one it was
								// about to be.
								if (haveMesh) {
									LogShaftMesh(mesh, "accepted", mesh.local, meshT0, meshT1,
										meshToWorld, sourceWidth, haveMeshGap, meshGap,
										shapeExtent.length + shapeExtent.thickness);
								}
							}
						}
					}

					// A shaft used to mark the snow at a fraction of a foot's
					// depth as well as a fraction of its width, on the
					// argument that a 2.02 unit circle pressed as deep as an
					// 18.69 unit foot would read as a pinhole.  That argument
					// is right about a circle and wrong about a line, and it
					// also put the mark ten times shallower than the rim
					// standing next to it: StampRimIndependence makes a rim's
					// height independent of the mark's depth, so a shaft got a
					// foot's full rim beside a tenth of a foot's groove.  The
					// log has exactly that, `depth x0.103`, and a ridge taller
					// than the depression it borders is snow on flat ground
					// rather than a furrow.
					//
					// The width derivation is kept and a floor is laid under
					// it, because the derivation is not wrong about a *thin*
					// object - only about how little a long thin one is
					// pressed.  At the default floor the derivation is inert
					// and a weapon presses as deep as a foot, which with the
					// rim restored is what makes the mark read as displaced
					// snow.  Setting ShaftStampMinDepth to 0 restores the old
					// behaviour exactly.
					//
					// What is measured is the width the mark is drawn at, so
					// stamp.radius only stands in for it while the mark is a
					// circle.  The line case sets radius to the half length,
					// which is tens of units, and dividing that by an 18.69
					// unit foot would saturate the clamp and press a weapon
					// line into the snow as deep as a boot - the depth
					// following a number that stopped describing thickness.
					// halfWidth is the thickness a line is actually drawn at,
					// so the same question gets asked of the right number.
					const float markWidth = stamp.halfWidth > 0.0f ? stamp.halfWidth :
																	stamp.radius;
					const float widthDepth = markWidth /
						std::max(Settings::shaftStampFootRadius, 1.0f);
					const float depthScale = isShaft ?
						std::clamp(std::max(widthDepth,
									   Settings::shaftStampMinDepth),
							0.0f, 1.0f) :
						1.0f;

					const float ordinary = Settings::stampDepth * response.depthScale *
						Weather::DepthScale() * depthScale;

					stamp.depth = Surfaces::MarkDepth(surface, ordinary, 1.0f, stamp.x, stamp.y);
					stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);

					stamp.decay = std::clamp(
						Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(),
						0.0f, 0.9999f);

					stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);

					// A shaft gets no rim, so nothing is thrown up around it.
					// This has to come after the line above: RimHeight assigns
					// stamp.rim unconditionally for every stamp, so clearing it
					// in the isShaft branch earlier would simply be overwritten
					// and the change would have no effect at all.
					//
					// The rim is what made the furrow, and that is why this
					// branch exists at all.  A bow rides on the skeleton and
					// swings with the body - over 0.585 seconds of the
					// recorded run its side swept 33.8 world units while its
					// length and thickness stayed at 57.72 and 2.02 - and it
					// was stamped 41 times a second across that sweep.  Each
					// of those stamps raised its own rim, so a small circle
					// every frame still banked the snow up along the whole
					// arc, which reads as one even furrow.  That is why the
					// default for a shaft's own rim is 0.
					//
					// It does not have to be 0 any more.  The ridge was as
					// wide as the mark was long - the shader took the rim's
					// band from `s.z`, which on a line is the *half length*,
					// tens of units - so the only way to stop the ridge was to
					// stop the rim.  The band now follows the mark's half width
					// (ClipmapUpdateCS.h: `bandRef`), a few units, which is
					// the ridge a footprint gets and what the user asked for:
					// "should look like a footprint".  ShaftStampRim decides
					// how much of it there is; 0 restores the pin-pricks, with
					// no banked snow beside the mark at all.
					if (isShaft) {
						stamp.rim = Settings::stampShaftRim * std::max(response.rimScale, 0.0f);
						const RE::NiPoint3 markAt{ stamp.x, stamp.y,
							spanOk ? span.midZ :
							(nearOk ? span.nearZ : (hitOk ? hit.z : pose.point.z)) };

						// Counted before the line is written, and counted even
						// when that line is rate limited away: the window's
						// job is to describe the decisions, not the lines.  The
						// same test the log uses to choose between the line and
						// the disc form decides whether this was a line.
						g_shaftWindow.Mark(stamp.halfWidth > 0.0f);

						LogShaftStamp(stamp.radius, stamp.halfWidth, shapeExtent.length,
							shapeExtent.thickness, stamp.rim, depthScale,
							markAt,
							centre, shaftLowest, haveShaftLand ? shaftLand : shaftLowest,
							poseAbove, haveMesh ? "mesh" : (poseAxisOk ? "live" : "centre"),
							placement, hitOk ? hit.t : 0.0f,
							span);
					}

					if (stamp.kind != Stamp::Kind::kPrint) {
						stamp.motionX = motionX;
						stamp.motionY = motionY;
					}

					LogStampSize(stamp);
					a_out.push_back(stamp);

					return RE::BSVisit::BSVisitControl::kContinue;
				});
		}

		std::vector<Stamp> GatherStamps(float a_deltaSeconds)
		{
			++g_actorFrame;

			// Per-frame probe budget, not per-session.  The first run burned
			// its whole allowance in four frames and never saw the frames
			// that had the bow drawn.
			g_collidablesLogged.store(0);
			g_frameRatios.clear();
			g_frameTypes.clear();

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
				LogCollidableTally();
				ReportShaftTally();
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

			LogCollidableTally();
			ReportShaftTally();
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
		return g_srv[0] && g_uav[0] && g_sampler && g_updateCS && g_paramsCB && g_windowCB;
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
			params.stampMotion[i][3] = stamps[i].rimBulge;

			params.stampNoise[i][0] = stamps[i].rimNoise;
			params.stampNoise[i][1] = stamps[i].lipBand;
		}

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

			// How ragged a line mark's rim edge is.  Filled unconditionally
			// rather than only when the alignment is on, because the shader
			// gates the term on the mark being a line and not on this being
			// non-zero - a zero here simply means a smooth edge, which is
			// what an ini with the key set to zero asks for.
			params.markRimJitter[0] = std::max(Settings::shaftMarkRimJitter, 0.0f);
			params.markRimJitter[1] = std::max(Settings::shaftMarkRimJitterBand, 1.0f);

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

				ID3D11UnorderedAccessView* uavs[3] = { g_uav[level], g_decayUAV[level],
					g_activityUAV[level] };
				context->CSSetConstantBuffers(0, 1, &g_paramsCB);
				context->CSSetUnorderedAccessViews(0, 3, uavs, noOffset);
				context->Dispatch(kTexels / 8, kTexels / 8, 1);

				ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
				context->CSSetUnorderedAccessViews(0, 3, nullUAVs, noOffset);

				ID3D11ShaderResourceView* nullSeeds[2] = { nullptr, nullptr };
				context->CSSetShaderResources(1, 2, nullSeeds);
			}

			ID3D11ShaderResourceView* nullFloor[2] = { nullptr, nullptr };
			context->CSSetShaderResources(3, 2, nullFloor);

			Profiler::GpuEnd();
		}
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
