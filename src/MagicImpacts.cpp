// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "TerrainActivity.h"
#include "MagicImpacts.h"
#include "ImpactPatterns.h"
#include "Settings.h"
#include "Profiler.h"
#include "SurfaceProfiles.h"
#include "Weather.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <type_traits>

namespace MagicImpacts
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		enum class Kind { kProjectile, kShout, kExplosion, kArrow };
		enum class Element { kForce, kFire, kFrost, kShock };
		struct Event
		{
			Kind kind{};
			Element element{};
			RE::NiPoint3 position{}, direction{};
			RE::FormID source{}, space{};
			float radius{};
			bool voice{};
			Clock::time_point time{};
		};

		std::array<Event, 128> g_queue{};
		size_t g_head{}, g_count{};
		std::mutex g_lock;
		std::atomic<bool> g_enabled{ false };
		std::atomic<uint32_t> g_logged{};
		std::atomic<uint32_t> g_arrowContactsLogged{};
		std::atomic<uint32_t> g_arrowBuildsLogged{};
		// Keep deduplication after draining the queue: embedded arrows can report
		// more than one contact. Handles also distinguish recycled form IDs.
		std::array<RE::ObjectRefHandle, 256> g_arrowSeen{};
		size_t g_nextArrow{};
		struct BlastSource
		{
			RE::FormID base{}, space{};
			RE::NiPoint3 position{};
			Element element{};
			Clock::time_point time{};
		};
		std::array<BlastSource, 64> g_blastSources{};
		size_t g_nextBlast{};

		RE::FormID Space(RE::TESObjectREFR* ref)
		{
			auto* cell = ref ? ref->GetParentCell() : nullptr;
			if (!cell) { return 0; }
			auto* world = cell->GetRuntimeData().worldSpace;
			return world ? world->GetFormID() : cell->GetFormID();
		}

		bool Finite(const RE::NiPoint3& p)
		{
			return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
		}

		bool Classify(RE::MagicItem* spell, Element& element)
		{
			if (!spell) { return false; }
			bool physical = false;
			for (const auto* effect : spell->effects) {
				const auto* base = effect ? effect->baseEffect : nullptr;
				if (!base || !base->IsHostile()) { continue; }
				const auto& data = base->data;
				if (data.resistVariable == RE::ActorValue::kResistFire) {
					element = Element::kFire; return true;
				}
				if (data.resistVariable == RE::ActorValue::kResistFrost) {
					element = Element::kFrost; return true;
				}
				if (data.resistVariable == RE::ActorValue::kResistShock) {
					element = Element::kShock; return true;
				}
				physical |= data.archetype == RE::EffectArchetypes::ArchetypeID::kStagger ||
					data.archetype == RE::EffectArchetypes::ArchetypeID::kConcussion;
			}
			element = Element::kForce;
			return physical;
		}

		void Queue(Event event, RE::ObjectRefHandle arrow = {})
		{
			if (!TerrainActivity::Active() || !g_enabled.load(std::memory_order_relaxed) || !event.space ||
				!Finite(event.position) || !Finite(event.direction)) { return; }
			event.time = Clock::now();
			const std::scoped_lock lock(g_lock);
			if (!TerrainActivity::Active()) { return; }  // Recheck after waiting across an area transition.
			if (event.kind == Kind::kArrow) {
				if (!arrow || std::find(g_arrowSeen.begin(), g_arrowSeen.end(), arrow) != g_arrowSeen.end()) { return; }
			}

			for (size_t i = 0; i < g_count; ++i) {
				const auto& previous = g_queue[(g_head + i) % g_queue.size()];
				if (previous.source == event.source && previous.kind == event.kind &&
					event.time - previous.time < std::chrono::milliseconds(100) &&
					previous.position.GetSquaredDistance(event.position) < 256.0f) { return; }
			}
			if (g_count == g_queue.size()) { return; }
			if (event.kind == Kind::kArrow) { g_arrowSeen[g_nextArrow++ % g_arrowSeen.size()] = arrow; }
			g_queue[(g_head + g_count++) % g_queue.size()] = event;
		}

		bool GroundShout(RE::MagicItem* spell)
		{
			if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kVoicePower) { return false; }
			for (const auto* effect : spell->effects) {
				const auto* base = effect ? effect->baseEffect : nullptr;
				if (!base) { continue; }
				const auto* projectile = base->data.projectileBase;
				if ((projectile && (projectile->IsCone() || projectile->IsFlamethrower())) ||
					base->data.archetype == RE::EffectArchetypes::ArchetypeID::kStagger ||
					base->data.archetype == RE::EffectArchetypes::ArchetypeID::kConcussion) { return true; }
			}
			return false;
		}

		template <class T>
		struct ProjectileHit
		{
			static void thunk(RE::Projectile* projectile, RE::TESObjectREFR* target,
				const RE::NiPoint3& position, const RE::NiPoint3& velocity,
				RE::hkpCollidable* collidable, int32_t arg6, uint32_t arg7)
			{
				if constexpr (std::is_same_v<T, RE::ArrowProjectile>) {
					if (Settings::logMagicImpacts && g_arrowContactsLogged.fetch_add(1) < 48) {
						logger::info("Arrow contact source={:08X} active={} enabled={} layer={} target={:08X} pos=({:.2f},{:.2f},{:.2f})",
							projectile->GetFormID(), TerrainActivity::Active(), Settings::enableArrowImpacts,
							collidable ? static_cast<int>(collidable->GetCollisionLayer()) : -1,
							target ? target->GetFormID() : 0, position.x, position.y, position.z);
					}
				}
				if (TerrainActivity::Active() && g_enabled.load(std::memory_order_relaxed)) {
					if constexpr (std::is_same_v<T, RE::ArrowProjectile>) {
						// Collision identity is decisive; height alone would dent the
						// landscape underneath nearby rocks, buildings or actors.
						// Skyrim also reports landscape as kGround with no reference
						// target. Build validates contact height against that landscape.
						if (Settings::enableArrowImpacts && collidable &&
							(collidable->GetCollisionLayer() == RE::COL_LAYER::kTerrain ||
								(collidable->GetCollisionLayer() == RE::COL_LAYER::kGround && !target)) &&
							(!target || !target->As<RE::Actor>())) {
							Queue({ Kind::kArrow, Element::kForce, position, velocity,
								projectile->GetFormID(), Space(projectile) }, projectile->CreateRefHandle());
						}
					} else if (!projectile->As<RE::ArrowProjectile>()) {
						auto* spell = projectile->GetProjectileRuntimeData().spell;
						Element element{};
						if (Classify(spell, element)) {
							auto* blast = projectile->GetProjectileRuntimeData().explosion;
							if (blast) {

								const std::scoped_lock lock(g_lock);
								g_blastSources[g_nextBlast++ % g_blastSources.size()] =
									{ blast->GetFormID(), Space(projectile), position, element, Clock::now() };
							} else {

								Queue({ Kind::kProjectile, element, position, velocity,
									projectile->GetFormID(), Space(projectile), 0.0f,
									spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower });
							}
						}
					}
				}
				func(projectile, target, position, velocity, collidable, arg6, arg7);
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void Install()
			{
				REL::Relocation<std::uintptr_t> table{ T::VTABLE[0] };
				func = table.write_vfunc(REL::Module::IsVR() ? 0xBE : 0xBD, thunk);
			}
		};

		struct ExplosionInit
		{
			static void thunk(RE::Explosion* explosion)
			{
				func(explosion);
				if (!TerrainActivity::Active() || !g_enabled.load(std::memory_order_relaxed)) { return; }
				auto* object = explosion->GetBaseObject();
				auto* base = object ? object->As<RE::BGSExplosion>() : nullptr;
				if (!base) { return; }
				const auto& data = explosion->GetExplosionRuntimeData();
				Element element{};

				RE::MagicItem* magic = data.magicCaster ? data.magicCaster->currentSpell : nullptr;
				if (!magic) { magic = base->formEnchanting; }
				bool identified = Classify(magic, element);
				if (!identified) {
					const auto now = Clock::now();
					const auto space = Space(explosion);
					const std::scoped_lock lock(g_lock);
					for (size_t i = 0; i < g_blastSources.size(); ++i) {
						const auto& source = g_blastSources[(g_nextBlast + g_blastSources.size() - 1 - i) % g_blastSources.size()];
						if (source.base == base->GetFormID() && source.space == space &&
							now - source.time < std::chrono::milliseconds(500) &&
							source.position.GetSquaredDistance(explosion->GetPosition()) < 96.0f * 96.0f) {
							element = source.element; identified = true; break;
						}
					}
				}
				if (!identified && base->data.damage <= 0.0f && base->data.force <= 0.0f) { return; }
				Queue({ Kind::kExplosion, element, explosion->GetPosition(),
					{ -data.negativeVelocity.x, -data.negativeVelocity.y, -data.negativeVelocity.z },
					explosion->GetFormID(), Space(explosion), data.radius > 0 ? data.radius : base->data.radius });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		class CastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* event,
				RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (TerrainActivity::Active() && event && event->object && g_enabled.load(std::memory_order_relaxed)) {
					auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(event->spell);
					Element element{};
					if (GroundShout(spell) &&
						Classify(spell, element)) {
						auto* ref = event->object.get();
						const float yaw = ref->GetAngleZ(), pitch = ref->GetAngleX();
						auto origin = ref->GetPosition();

						origin.x += std::sin(yaw) * 40.0f;
						origin.y += std::cos(yaw) * 40.0f;
						origin.z += 60.0f;
						Queue({ Kind::kShout, element, origin,
							{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) },
							ref->GetFormID(), Space(ref) });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_castSink;

		bool ClearPath(RE::TES* tes, const RE::NiPoint3& from, const RE::NiPoint3& to)
		{
			const float scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0);
			pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
			tes->Pick(pick);
			return !pick.rayOutput.HasHit() || pick.rayOutput.hitFraction >= 0.98f;
		}

		void Build(const Event& event, const RE::NiPoint3& anchor, std::vector<Clipmap::Stamp>& out)
		{
			auto* tes = RE::TES::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool traceArrow = event.kind == Kind::kArrow && Settings::logMagicImpacts &&
				g_arrowBuildsLogged.fetch_add(1) < 48;
			if (traceArrow) {
				logger::info("Arrow gather source={:08X} space={:08X} playerSpace={:08X} distance={:.1f}",
					event.source, event.space, Space(player), std::sqrt(anchor.GetSquaredDistance(event.position)));
			}
			if (!tes || event.space != Space(player) ||
				anchor.GetSquaredDistance(event.position) > 2800.0f * 2800.0f) { return; }
			float landZ{};
			if (!tes->GetLandHeight(event.position, landZ)) {
				if (traceArrow) { logger::info("Arrow rejected: landscape height unavailable"); }
				return;
			}
			const float above = event.position.z - landZ;
			if (event.kind == Kind::kArrow) {
				if (traceArrow) { logger::info("Arrow terrain height={:.2f} contact delta={:.2f}", landZ, above); }
				if (!Settings::enableArrowImpacts || std::abs(above) > 8.0f ||
					out.size() >= Clipmap::kMaxStamps) { return; }
				const auto ground = Surfaces::GroundAt(event.position);
				const auto& response = ground.response;
				if (traceArrow) {
					logger::info("Arrow surface={} radiusScale={:.2f} depthScale={:.2f} radiusSetting={:.2f} depthSetting={:.2f}",
						Surfaces::Name(ground.type), response.radiusScale, response.depthScale,
						Settings::arrowImpactRadius, Settings::arrowImpactDepth);
				}
				if (ground.type == Surfaces::Type::kUnknown || ground.type == Surfaces::Type::kStone ||
					!std::isfinite(response.radiusScale) || !std::isfinite(response.depthScale) ||
					response.radiusScale <= 0 || response.depthScale <= 0 ||
					Settings::arrowImpactRadius <= 0 || Settings::arrowImpactDepth <= 0) { return; }
				Clipmap::Stamp stamp{};
				stamp.x = event.position.x; stamp.y = event.position.y;
				stamp.snow = ground.type == Surfaces::Type::kSnow;
				// A few fine-level texels wide, independent of model bounds, arrow
				// speed, snow blanket depth and the much larger magic settings.
				stamp.radius = std::clamp(Settings::arrowImpactRadius * response.radiusScale, 3.0f, 12.0f);
				stamp.depth = std::clamp(Settings::arrowImpactDepth * response.depthScale, 0.0f, 4.0f);
				stamp.shoulder = 0.25f;
				stamp.decay = std::clamp(Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(), 0.0f, 0.9999f);
				out.push_back(stamp);
				if (Settings::logMagicImpacts && g_logged++ < 32) {
					logger::info("Arrow landscape impact source={:08X} radius={:.1f} depth={:.1f}",
						event.source, stamp.radius, stamp.depth);
				}
				return;
			}
			float reach = Settings::magicImpactRadius;
			float depth = Settings::magicImpactDepth;
			ImpactPatterns::Pattern pattern{};
			if (event.kind == Kind::kExplosion) {
				if (!Settings::enableExplosionImpacts) { return; }
				reach = ImpactPatterns::BoundedRadius(event.radius, Settings::explosionImpactRadiusScale,
					Settings::explosionImpactMaxRadius);
				if (!(reach > 1) || above < -16 || above >= reach) { return; }
				reach = std::sqrt(std::max(0.0f, reach * reach - above * above));
				depth = Settings::explosionImpactDepth;
				pattern = ImpactPatterns::Explosion(reach);
			} else if (event.kind == Kind::kShout) {
				if (!Settings::enableShoutImpacts || event.direction.z > 0.45f || above < 0 || above > 150) { return; }
				reach = Settings::shoutImpactRange;
				depth = Settings::shoutImpactDepth;
				pattern = ImpactPatterns::Directional(reach, Settings::shoutImpactWidth,
					event.direction.x, event.direction.y, true);
			} else {
				if (!(event.voice ? Settings::enableShoutImpacts : Settings::enableMagicImpacts) ||
					above < -16 || above > 48) { return; }
				const float planar = std::hypot(event.direction.x, event.direction.y);
				if (planar > std::abs(event.direction.z) * 0.35f && planar > 0.001f) {
					pattern = ImpactPatterns::Directional(reach * 1.8f, reach,
						event.direction.x, event.direction.y, false);
				} else {
					pattern.strokes[pattern.count++] = { 0, 0, 0, 0, reach, 1 };
				}
			}
			const float elementScale = event.element == Element::kFrost ? 0.45f :
				event.element == Element::kFire ? 0.7f : 1.0f;
			const size_t before = out.size();
			for (size_t i = 0; i < pattern.count && out.size() < Clipmap::kMaxStamps; ++i) {
				const auto& stroke = pattern.strokes[i];
				RE::NiPoint3 point{ event.position.x + stroke.x, event.position.y + stroke.y, landZ };
				float groundZ{};
				if (!tes->GetLandHeight(point, groundZ)) { continue; }
				point.z = groundZ + 3.0f;
				if (event.kind == Kind::kShout) {
					const float distance = std::hypot(stroke.x, stroke.y);
					const float planar = std::max(std::hypot(event.direction.x, event.direction.y), 0.01f);
					const float beamZ = event.position.z + event.direction.z * distance / planar;
					if (beamZ - groundZ > 140 || beamZ - groundZ < -40) { continue; }
				}
				auto origin = event.position;
				origin.z += 8.0f;
				if (!ClearPath(tes, origin, point)) { continue; }
				const auto ground = Surfaces::GroundAt(point);
				const auto& response = ground.response;
				if (ground.type == Surfaces::Type::kUnknown || ground.type == Surfaces::Type::kStone ||
					response.depthScale <= 0) { continue; }
				Clipmap::Stamp stamp{};
				stamp.snow = ground.type == Surfaces::Type::kSnow;
				const float span = stamp.snow && Settings::snowStampRimSpan >= 0 ?
					Settings::snowStampRimSpan : Settings::stampRimSpan;
				const float lean = stamp.snow && Settings::snowStampRimLean >= 0 ?
					Settings::snowStampRimLean : Settings::stampRimLean;
				stamp.x = point.x; stamp.y = point.y;
				stamp.radius = ImpactPatterns::StampRadius(stroke.radius, response.radiusScale, span, lean);
				if (stamp.radius <= 0.5f) { continue; }
				stamp.motionX = stroke.motionX; stamp.motionY = stroke.motionY;
				stamp.depth = std::clamp(depth * elementScale * stroke.strength * response.depthScale, 0.0f, 64.0f);
				stamp.rim = stamp.depth * Settings::magicImpactRimScale * std::clamp(response.rimScale, 0.0f, 2.0f);

				if (stamp.snow && event.element == Element::kFire && Settings::heatMeltsSnow &&
					(event.kind != Kind::kExplosion || Settings::heatFromExplosions)) {
					stamp.depth = std::max(stamp.depth, Weather::SnowDepth() * Settings::magicImpactSnowMelt * stroke.strength);
				}
				stamp.depth = std::clamp(stamp.depth, 0.0f, 64.0f);
				stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.5f);
				stamp.decay = std::clamp(Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(), 0.0f, 0.9999f);
				out.push_back(stamp);
			}
			if (Settings::logMagicImpacts && g_logged < 32 && out.size() > before) {
				++g_logged;
				logger::info("Magic impact kind={} source={:08X} reach={:.1f} stamps={}",
					static_cast<int>(event.kind), event.source, reach, out.size() - before);
			}
		}
	}

	bool IsFireMagic(RE::MagicItem* spell)
	{
		Element element{};
		return Classify(spell, element) && element == Element::kFire;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_head = g_count = 0;
		g_blastSources = {};
		g_nextBlast = 0;
		g_logged = 0;
		g_arrowContactsLogged = 0;
		g_arrowBuildsLogged = 0;
		g_arrowSeen = {};
		g_nextArrow = 0;
		g_enabled.store(Settings::enableMagicImpacts || Settings::enableShoutImpacts ||
			Settings::enableExplosionImpacts || Settings::enableArrowImpacts, std::memory_order_relaxed);
	}

	void Append(const RE::NiPoint3& anchor, std::vector<Clipmap::Stamp>& out)
	{

		if (out.size() + 9 > Clipmap::kMaxStamps) { return; }
		Event event{};
		bool found = false;
		{
			const std::scoped_lock lock(g_lock);
			while (g_count) {
				event = g_queue[g_head];
				g_head = (g_head + 1) % g_queue.size(); --g_count;
				if (Clock::now() - event.time < std::chrono::seconds(1)) { found = true; break; }
			}
		}
		if (found) {
			const auto start = Profiler::Ticks();
			Build(event, anchor, out);
			Profiler::AddCpuTicks(Profiler::CpuScope::kMagicImpacts, Profiler::Ticks() - start);
		}
	}

	void Install()
	{
		Reset();
		ProjectileHit<RE::MissileProjectile>::Install();
		ProjectileHit<RE::ArrowProjectile>::Install();
		ProjectileHit<RE::BeamProjectile>::Install();
		ProjectileHit<RE::FlameProjectile>::Install();
		ProjectileHit<RE::ConeProjectile>::Install();
		ProjectileHit<RE::GrenadeProjectile>::Install();
		REL::Relocation<std::uintptr_t> table{ RE::Explosion::VTABLE[0] };
		ExplosionInit::func = table.write_vfunc(0xA2, ExplosionInit::thunk);
		if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
			source->AddEventSink<RE::TESSpellCastEvent>(&g_castSink);
		}
		logger::info("Installed bounded arrow landscape impacts, projectile/explosion impacts and directional shout events");
	}
}
