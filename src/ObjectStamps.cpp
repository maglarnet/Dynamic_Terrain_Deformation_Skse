// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "Profiler.h"

#include "ObjectStamps.h"
#include "ObjectStampFilter.h"

#include "ActorShapes.h"
#include "Globals.h"
#include "HeatSources.h"
#include "Settings.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ObjectStamps
{
	namespace
	{
		std::mutex g_lock;

		struct Candidate
		{
			RE::ObjectRefHandle handle;

			bool heat{ false };
		};

		std::vector<Candidate> g_candidates;
		float                  g_timer{ 0.0f };
		bool                   g_haveSet{ false };
		uint64_t               g_frame{ 0 };

		constexpr size_t kMaxCandidates = 256;

		constexpr float kMaxMotion = 48.0f;

		struct float2
		{
			float x{ 0.0f }, y{ 0.0f };
		};

		struct Tracked
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };
		};

		std::unordered_map<RE::FormID, Tracked> g_motion;
		std::unordered_set<RE::FormID> g_visualBloodReported;

		bool LeavesAMark(RE::TESObjectREFR* a_ref)
		{

			if (auto* projectile = a_ref->As<RE::Projectile>()) {
				const auto* base = projectile->GetProjectileBase();
				const char* model = base ? base->GetModel() : nullptr;
				if (model && ObjectStampFilter::IsVisualBloodProjectile(model)) {
					if (g_visualBloodReported.size() < 16 && g_visualBloodReported.insert(base->GetFormID()).second) {
						logger::info("Object stamps B5: visual blood projectile excluded base={:08X} model={}",
							base->GetFormID(), model);
					}
					return false;
				}
				return true;
			}

			const auto* base = a_ref->GetBaseObject();
			if (!base) {
				return false;
			}

			switch (base->GetFormType()) {
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Ammo:
			case RE::FormType::Misc:
			case RE::FormType::Ingredient:
			case RE::FormType::AlchemyItem:
			case RE::FormType::Book:
			case RE::FormType::Scroll:
			case RE::FormType::SoulGem:
			case RE::FormType::KeyMaster:
			case RE::FormType::Apparatus:
			case RE::FormType::Light:
				return true;
			default:
				return false;
			}
		}

		std::unordered_set<RE::FormID> g_reported;

		void LogObject(RE::TESObjectREFR* a_ref, Surfaces::Type a_surface,
			const Clipmap::Stamp& a_stamp, float a_drop)
		{
			if (!Settings::logObjectStamps || g_reported.size() >= 24) {
				return;
			}

			const auto* base = a_ref->GetBaseObject();
			if (!base || !g_reported.insert(base->GetFormID()).second) {
				return;
			}

			logger::info(
				"Object stamp: {} on {:<7} radius={:.1f} depth={:.1f} drop={:.1f}",
				a_ref->GetName(), Surfaces::Name(a_surface), a_stamp.radius, a_stamp.depth,
				a_drop);
		}

		constexpr size_t kMaxShapesPerObject = 3;

		struct ShapeBound
		{
			RE::NiPoint3 centre;
			float        radius{ 0.0f };
		};

		size_t GatherShapes(RE::NiAVObject* a_root, ShapeBound (&a_out)[kMaxShapesPerObject])
		{
			std::vector<ShapeBound> found;
			RE::BSVisit::TraverseScenegraphCollision(
				a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
					ShapeBound bound{};
					if (ActorShapes::GetBound(a_object, bound.centre, bound.radius) &&
						bound.radius > 0.1f) {
						found.push_back(bound);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});

			if (found.empty()) {
				const auto& bound = a_root->worldBound;
				if (bound.radius <= 0.1f) {
					return 0;
				}
				a_out[0] = ShapeBound{ bound.center, bound.radius };
				return 1;
			}

			std::sort(found.begin(), found.end(),
				[](const ShapeBound& a_lhs, const ShapeBound& a_rhs) {
					return a_lhs.radius > a_rhs.radius;
				});

			const size_t take = std::min(found.size(), kMaxShapesPerObject);
			for (size_t i = 0; i < take; ++i) {
				a_out[i] = found[i];
			}
			return take;
		}

		size_t StampsFor(RE::TESObjectREFR* a_ref, float a_motionX, float a_motionY,
			size_t a_budget, std::vector<Clipmap::Stamp>& a_out)
		{
			if (a_budget == 0) {
				return 0;
			}

			auto* root = a_ref->Get3D();
			if (!root) {
				return 0;
			}

			ShapeBound   shapes[kMaxShapesPerObject]{};
			const size_t shapeCount = GatherShapes(root, shapes);
			if (shapeCount == 0) {
				return 0;
			}

			float groundZ = shapes[0].centre.z - shapes[0].radius;
			for (size_t i = 1; i < shapeCount; ++i) {
				groundZ = std::min(groundZ, shapes[i].centre.z - shapes[i].radius);
			}

			const RE::NiPoint3 probe{ shapes[0].centre.x, shapes[0].centre.y,
				shapes[0].centre.z };

			auto* tes = RE::TES::GetSingleton();
			float landZ = 0.0f;
			if (!tes || !tes->GetLandHeight(probe, landZ)) {
				return 0;
			}

			const float surfaceZ = landZ + SnowSurface::LiftAt(probe.x, probe.y);
			const float drop = groundZ - surfaceZ;
			if (drop > Settings::objectContactTolerance || drop < -Settings::objectSinkLimit) {
				return 0;
			}

			const auto  ground = Surfaces::GroundAt(probe);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				return 0;
			}

			const size_t take = std::min(shapeCount, a_budget);
			for (size_t i = 0; i < take; ++i) {
				const auto& shape = shapes[i];

				Clipmap::Stamp stamp{};
				stamp.snow = surface == Surfaces::Type::kSnow;
				stamp.x = shape.centre.x;
				stamp.y = shape.centre.y;
				stamp.radius =
					shape.radius * Settings::objectStampRadiusScale * response.radiusScale;

				stamp.motionX = a_motionX;
				stamp.motionY = a_motionY;

				const float bulk = std::clamp(
					shape.radius / std::max(Settings::objectFullSizeRadius, 1.0f), 0.0f, 1.0f);

				const float ordinary = Settings::stampDepth * Settings::objectStampDepthScale *
					bulk * response.depthScale * Weather::DepthScale();

				stamp.depth = Surfaces::MarkDepth(surface, ordinary, bulk, stamp.x, stamp.y);
				stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);
				stamp.decay = std::clamp(
					Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(),
					0.0f, 0.9999f);

				stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);

				if (i == 0) {
					LogObject(a_ref, surface, stamp, drop);
				}
				a_out.push_back(stamp);
			}
			return take;
		}

		void Refresh(const RE::NiPoint3& a_anchor, size_t a_budget)
		{
			g_candidates.clear();
			if (a_budget == 0) {
				return;
			}

			auto* tes = RE::TES::GetSingleton();
			auto* player = globals::game::player;
			if (!tes || !player) {
				return;
			}

			const float radius = Clipmap::kWorldSize * 0.375f;

			tes->ForEachReferenceInRange(player, radius,
				[&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
					if (g_candidates.size() >= kMaxCandidates) {
						return RE::BSContainer::ForEachResult::kStop;
					}

					if (!a_ref || !a_ref->Is3DLoaded() || a_ref->IsDisabled() ||
						a_ref->IsMarkedForDeletion()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}

					if (a_ref->As<RE::Actor>()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}

					Clipmap::Stamp probe{};
					if (HeatSources::StampFor(a_ref, probe)) {
						g_candidates.push_back({ a_ref->CreateRefHandle(), true });
					} else if (LeavesAMark(a_ref)) {
						g_candidates.push_back({ a_ref->CreateRefHandle(), false });
					}

					return RE::BSContainer::ForEachResult::kContinue;
				});
		}

		float2 TrackMotion(RE::FormID a_form, const RE::NiPoint3& a_position)
		{
			float2 motion{ 0.0f, 0.0f };

			const auto previous = g_motion.find(a_form);
			if (previous != g_motion.end()) {
				const float dx = a_position.x - previous->second.x;
				const float dy = a_position.y - previous->second.y;
				if (dx * dx + dy * dy <= kMaxMotion * kMaxMotion) {
					motion = { dx, dy };
				}
			}

			g_motion[a_form] = { a_position.x, a_position.y, g_frame };
			return motion;
		}

	}

	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor,
		std::vector<Clipmap::Stamp>& a_out)
	{
		if (!Settings::enableObjectStamps || a_out.size() >= Clipmap::kMaxStamps) {
			return;
		}

		const std::scoped_lock lock(g_lock);
		++g_frame;

		g_timer -= a_deltaSeconds;
		if (!g_haveSet || g_timer <= 0.0f) {
			g_timer = std::max(Settings::objectStampInterval, 0.0f);
			g_haveSet = true;
			const int64_t scanStarted = Profiler::Ticks();
			Refresh(a_anchor, Clipmap::kMaxStamps);
			Profiler::AddCpuTicks(Profiler::CpuScope::kObjectScan, Profiler::Ticks() - scanStarted);
		}

		const size_t room = Clipmap::kMaxStamps - a_out.size();
		if (room == 0) {
			return;
		}

		static std::vector<Clipmap::Stamp> built;
		built.clear();

		for (const auto& candidate : g_candidates) {
			if (built.size() >= room) {
				break;
			}

			auto  refPtr = candidate.handle.get();
			auto* ref = refPtr.get();
			if (!ref || !ref->Is3DLoaded() || ref->IsDisabled() || ref->IsMarkedForDeletion()) {
				continue;
			}

			if (candidate.heat) {

				Clipmap::Stamp stamp{};
				if (HeatSources::StampFor(ref, stamp)) {
					built.push_back(stamp);
				}
				continue;
			}

			const auto motion = TrackMotion(ref->GetFormID(), ref->GetPosition());
			StampsFor(ref, motion.x, motion.y, room - built.size(), built);
		}

		std::sort(built.begin(), built.end(),
			[&a_anchor](const Clipmap::Stamp& a_lhs, const Clipmap::Stamp& a_rhs) {
				const float lx = a_lhs.x - a_anchor.x;
				const float ly = a_lhs.y - a_anchor.y;
				const float rx = a_rhs.x - a_anchor.x;
				const float ry = a_rhs.y - a_anchor.y;
				return (lx * lx + ly * ly) < (rx * rx + ry * ry);
			});

		const size_t take = std::min(room, built.size());
		a_out.insert(a_out.end(), built.begin(), built.begin() + take);

		for (auto it = g_motion.begin(); it != g_motion.end();) {
			it = (it->second.frame + 120 < g_frame) ? g_motion.erase(it) : std::next(it);
		}
	}

	// Called while Append is skipped (indoors): the refs and positions from before the skip
	// would otherwise be stamped on the way back out, wherever the player is then. The timer
	// is kept, so the first refresh outside comes on its usual schedule.
	void Forget()
	{
		const std::scoped_lock lock(g_lock);
		g_candidates.clear();
		g_motion.clear();
	}

	void Reset()
	{
		HeatSources::Reset();

		const std::scoped_lock lock(g_lock);
		g_candidates.clear();
		g_motion.clear();
		g_reported.clear();
		g_timer = 0.0f;
		g_haveSet = false;
	}
}
