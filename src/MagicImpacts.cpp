// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "MagicImpacts.h"
#include "ImpactPatterns.h"
#include "ObjectStamps.h"
#include "Settings.h"
#include "SnowSparkle.h"
#include "Profiler.h"
#include "SurfaceProfiles.h"
#include "Weather.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace MagicImpacts
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		enum class Kind { kProjectile, kShout, kExplosion };
		enum class Element { kForce, kFire, kFrost, kShock };
		struct Event
		{
			Kind kind{};
			Element element{};
			RE::NiPoint3 position{}, direction{};
			RE::FormID source{}, space{};
			float radius{};
			// Straight-line metres the agent travelled before it landed: an
			// arrow's own odometer, or the flight of the fireball that
			// detonated.  Kept here rather than recomputed from the caster,
			// because a caster who walks while the projectile is in the air
			// would otherwise shorten a shot that did not change.  Negative
			// means "not known", which is also what an explosion inherits when
			// its source could not be found.
			float flight{ -1.0f };
			bool voice{};
			Clock::time_point time{};
		};

		std::array<Event, 128> g_queue{};
		size_t g_head{}, g_count{};
		std::mutex g_lock;
		std::atomic<bool> g_enabled{ false };
		std::atomic<uint32_t> g_logged{};
		uint32_t g_probeLogged{};
		struct BlastSource
		{
			RE::FormID base{}, space{};
			RE::NiPoint3 position{};
			Element element{};
			Clock::time_point time{};
			// How far the projectile that spawned this blast had flown when it
			// went off.  An Explosion carries no odometer of its own - its
			// runtime data has actorOwner and negativeVelocity but nothing that
			// counts distance - so the number has to be captured here, on the
			// projectile that actually did the flying, and looked up again from
			// the explosion a moment later.  Carried even when the element is
			// already known, so the lookup path is not the only one that needs
			// it.
			float flight{};
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

		void Queue(Event event)
		{
			if (!g_enabled.load(std::memory_order_relaxed) || !event.space ||
				!Finite(event.position) || !Finite(event.direction)) { return; }
			event.time = Clock::now();
			const std::scoped_lock lock(g_lock);

			for (size_t i = 0; i < g_count; ++i) {
				const auto& previous = g_queue[(g_head + i) % g_queue.size()];
				if (previous.source == event.source && previous.kind == event.kind &&
					event.time - previous.time < std::chrono::milliseconds(100) &&
					previous.position.GetSquaredDistance(event.position) < 256.0f) { return; }
			}
			if (g_count == g_queue.size()) { return; }
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
				if (g_enabled.load(std::memory_order_relaxed)) {
					// Probe for the distance work: an arrow and a fireball both
					// spend a different amount of flight on a near target than
					// on a far one, and the engine already counts that for us.
					// distanceMoved is the projectile's own odometer and range
					// is its design limit, so neither has to be inferred from
					// the caster's position - which would drift as the caster
					// walked.  Nothing downstream reads these yet; this line
					// exists only to prove the fields are live and sane before
					// any rule is built on them.
					if (Settings::logMagicImpacts && g_probeLogged < 32) {
						const std::scoped_lock probeLock(g_lock);
						if (g_probeLogged < 32) {
							++g_probeLogged;
							const auto& rdata = projectile->GetProjectileRuntimeData();
							const auto  shooterRef = rdata.shooter.get();
							const auto  origin = shooterRef ? shooterRef->GetPosition() : RE::NiPoint3{};
							logger::info("Projectile flight: moved={:.2f} range={:.2f} speedMult={:.2f} power={:.2f} "
										 "shooter={:08X} casterDist={:.2f} hit=({:.1f},{:.1f},{:.1f})",
								rdata.distanceMoved, rdata.range, rdata.speedMult, rdata.power,
								shooterRef ? shooterRef->GetFormID() : 0u,
								shooterRef ? origin.GetDistance(position) : -1.0f,
								position.x, position.y, position.z);
						}
					}
					// Hand the landing point to the object path before the
					// element classification, so a plain arrow - which is not
					// magic and would otherwise fall through - is stamped
					// where it actually met something rather than wherever its
					// origin happens to sit.
					ObjectStamps::NoteContact(projectile, position);
					auto* spell = projectile->GetProjectileRuntimeData().spell;
					Element element{};
					if (Classify(spell, element)) {
						auto* blast = projectile->GetProjectileRuntimeData().explosion;
						if (blast) {

							const std::scoped_lock lock(g_lock);
							g_blastSources[g_nextBlast++ % g_blastSources.size()] =
								{ blast->GetFormID(), Space(projectile), position, element,
									Clock::now(), projectile->GetProjectileRuntimeData().distanceMoved };
						} else {

							Queue({ Kind::kProjectile, element, position, velocity,
								projectile->GetFormID(), Space(projectile), 0.0f,
								projectile->GetProjectileRuntimeData().distanceMoved,
								spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower });
						}
					}
				}
				func(projectile, target, position, velocity, collidable, arg6, arg7);
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void Install()
			{
				REL::Relocation<std::uintptr_t> table{ T::VTABLE[0] };
				func = table.write_vfunc(0xBD, thunk);
			}
		};

		struct ExplosionInit
		{
			static void thunk(RE::Explosion* explosion)
			{
				// Probe: the very first thing on entry, before the original is
				// even called.  If a fireball is thrown and this never prints,
				// the vtable hook itself is not live and no amount of tuning
				// downstream will matter.
				const bool probe = Settings::logMagicImpacts && g_probeLogged < 32;
				if (probe) {
					++g_probeLogged;
					logger::info("Explosion hit: ref={:08X} id={:08X} enabled={}",
						explosion ? explosion->GetFormID() : 0u,
						explosion ? explosion->GetBaseObject()->GetFormID() : 0u,
						g_enabled.load(std::memory_order_relaxed));
				}
				func(explosion);
				if (!g_enabled.load(std::memory_order_relaxed)) { return; }
				auto* object = explosion->GetBaseObject();
				auto* base = object ? object->As<RE::BGSExplosion>() : nullptr;
				if (!base) {
					if (probe) { logger::info("Explosion drop: no BGSExplosion base"); }
					return;
				}
				const auto& data = explosion->GetExplosionRuntimeData();
				Element element{};
				float   flight = -1.0f;

				// Look the projectile up first, whatever the classification
				// turns out to be.  The blast record in this table is the only
				// place the flight distance exists at all, and a fireball that
				// named its own element would otherwise walk away without it -
				// which is exactly the case a distance rule cares about.
				{
					const auto now = Clock::now();
					const auto space = Space(explosion);
					const std::scoped_lock lock(g_lock);
					for (size_t i = 0; i < g_blastSources.size(); ++i) {
						const auto& source = g_blastSources[(g_nextBlast + g_blastSources.size() - 1 - i) % g_blastSources.size()];
						if (source.base == base->GetFormID() && source.space == space &&
							now - source.time < std::chrono::milliseconds(500) &&
							source.position.GetSquaredDistance(explosion->GetPosition()) < 96.0f * 96.0f) {
							flight = source.flight;
							break;
						}
					}
				}

				RE::MagicItem* magic = data.magicCaster ? data.magicCaster->currentSpell : nullptr;
				const bool fromCaster = magic != nullptr;
				if (!magic) { magic = base->formEnchanting; }
				bool identified = Classify(magic, element);
				if (probe) {
					logger::info("Explosion magic: caster={} magic={:08X} identified={} element={} rdata.radius={:.2f} base.radius={:.2f} damage={:.2f} force={:.2f}",
						fromCaster, magic ? magic->GetFormID() : 0u, identified, static_cast<int>(element),
						data.radius, base->data.radius, base->data.damage, base->data.force);
				}
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
					if (probe) {
						logger::info("Explosion lookup: base={:08X} space={:08X} identified={}",
							base->GetFormID(), space, identified);
					}
				}
				if (!identified && base->data.damage <= 0.0f && base->data.force <= 0.0f) {
					if (probe) { logger::info("Explosion drop: unidentified and damage/force both zero"); }
					return;
				}
				Queue({ Kind::kExplosion, element, explosion->GetPosition(),
					{ -data.negativeVelocity.x, -data.negativeVelocity.y, -data.negativeVelocity.z },
					explosion->GetFormID(), Space(explosion), data.radius > 0 ? data.radius : base->data.radius,
					flight });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		class CastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* event,
				RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (event && event->object && g_enabled.load(std::memory_order_relaxed)) {
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

		// Where an air burst's centre actually meets the ground, straight
		// down.  Returns the centre itself when there is nothing below it
		// within the blast's own reach, which is the case of an explosion
		// over a ledge: it then scorches nothing rather than reaching
		// arbitrarily far for a surface.
		//
		// The pick's output carries no hit point, so the point is interpolated
		// from the fraction, as the shelter ray does (Shelter.cpp:113).
		RE::NiPoint3 GroundUnder(const RE::NiPoint3& a_centre, float a_reach)
		{
			if (!Settings::contactProbeShafts || !(a_reach > 0.0f)) {
				return a_centre;
			}
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return a_centre;
			}
			const float scale = RE::bhkWorld::GetWorldScale();
			const float below = a_centre.z - a_reach;
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_centre.x * scale, a_centre.y * scale,
				a_centre.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_centre.x * scale, a_centre.y * scale,
				below * scale, 0.0f);
			// Ground layer only.  Without this the ray answers with the
			// bursting projectile's own collidable or with a bystander, and
			// the scorch lands at head height instead of on the snow.
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kGround);
			tes->Pick(pick);
			if (!pick.rayOutput.HasHit()) {
				return a_centre;
			}
			const float t = std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
			return { a_centre.x, a_centre.y, a_centre.z + (below - a_centre.z) * t };
		}

		void Build(const Event& event, const RE::NiPoint3& anchor, std::vector<Clipmap::Stamp>& out)
		{
			auto* tes = RE::TES::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!tes || event.space != Space(player) ||
				anchor.GetSquaredDistance(event.position) > 2800.0f * 2800.0f) { return; }

			// Probe for the distance work.  A negative flight means the value
			// never reached this event - an explosion whose spawning projectile
			// was not in the table, or a shout, which has no flight at all -
			// and a rule built on it would then silently treat every such case
			// as "point blank".  Printing it next to the stamps is what makes
			// that visible before anything reads the number.
			if (Settings::logMagicImpacts && g_probeLogged < 32) {
				++g_probeLogged;
				logger::info("Impact flight: kind={} element={} flight={:.2f} reach={:.2f} radius={:.2f} at ({:.1f},{:.1f},{:.1f})",
					static_cast<int>(event.kind), static_cast<int>(event.element), event.flight,
					event.kind == Kind::kExplosion ? Settings::explosionImpactRadiusScale * event.radius : 0.0f,
					event.radius, event.position.x, event.position.y, event.position.z);
			}

			// What "above the ground" means depends on the event:
			//
			//  * a projectile is already resolved to a point on the surface,
			//    so its own height above the land is the honest one;
			//  * an explosion is a sphere centred in the air - the ground it
			//    reaches is whatever is *below* it, and its own centre can be
			//    well above the land (a fireball detonating at head height is
			//    still supposed to scorch the snow under it).  Judging it by
			//    its centre height is what made a fireball produce nothing
			//    whenever it went off higher than the ground it was aimed at.
			//
			// So an explosion has its contact resolved straight down from its
			// centre and works from there; everything else keeps its own
			// height.
			float reach = Settings::magicImpactRadius;
			float depth = Settings::magicImpactDepth;
			ImpactPatterns::Pattern pattern{};
			auto  contact = event.position;
			float above = 0.0f;
			if (event.kind == Kind::kExplosion) {
				if (!Settings::enableExplosionImpacts) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Build drop: explosion impacts disabled"); }
					return;
				}
				reach = ImpactPatterns::BoundedRadius(event.radius, Settings::explosionImpactRadiusScale,
					Settings::explosionImpactMaxRadius);
				if (!(reach > 1)) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Build drop: event.radius={:.2f} bounded reach={:.2f} (needs > 1)", event.radius, reach); }
					return;
				}
				contact = GroundUnder(event.position, reach);
				depth = Settings::explosionImpactDepth;
				// The bank is thrown, so which bank it is comes from where it
				// landed.  GroundUnder's result is used rather than the raw
				// event position because a blast that detonates in the air
				// somewhere over a slope should still leave the same crater as
				// one that detonates on that slope's surface - the seed
				// describes the ground that was hit, not the flight that hit
				// it.  The three coordinates are quantised to a tenth of a unit
				// before hashing, so floating-point dust between two rebuilds
				// of the same crater cannot reseed it and make the bank crawl.
				const auto seedX = static_cast<int32_t>(std::lround(contact.x * 10.0f));
				const auto seedY = static_cast<int32_t>(std::lround(contact.y * 10.0f));
				const auto seedZ = static_cast<int32_t>(std::lround(contact.z * 10.0f));
				const uint32_t craterSeed = ImpactPatterns::Hash(
					static_cast<uint32_t>(seedX) * 0x9e3779b9u ^
					static_cast<uint32_t>(seedY) * 0x85ebca6bu ^
					static_cast<uint32_t>(seedZ) * 0xc2b2ae35u);
				pattern = ImpactPatterns::Explosion(reach, craterSeed);
			} else {
				float landZ{};
				if (!tes->GetLandHeight(event.position, landZ)) { return; }
				above = event.position.z - landZ;
				if (event.kind == Kind::kShout) {
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
			}
			const float elementScale = event.element == Element::kFrost ? 0.45f :
				event.element == Element::kFire ? 0.7f : 1.0f;
			const size_t before = out.size();
			for (size_t i = 0; i < pattern.count && out.size() < Clipmap::kMaxStamps; ++i) {
				const auto& stroke = pattern.strokes[i];
				RE::NiPoint3 point{ contact.x + stroke.x, contact.y + stroke.y, contact.z };
				float groundZ{};
				if (!tes->GetLandHeight(point, groundZ)) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Stroke drop: no land under ({:.1f},{:.1f})", point.x, point.y); }
					continue;
				}
				point.z = groundZ + 3.0f;
				if (event.kind == Kind::kShout) {
					// A shout radiates from the caster's mouth, not from the
					// ground it is standing on, so a stroke only lands where
					// the shout's own beam comes down near the ground.
					const float distance = std::hypot(stroke.x, stroke.y);
					const float planar = std::max(std::hypot(event.direction.x, event.direction.y), 0.01f);
					const float beamZ = event.position.z + event.direction.z * distance / planar;
					if (beamZ - groundZ > 140 || beamZ - groundZ < -40) { continue; }
				}
				auto origin = contact;
				origin.z += 8.0f;
				// A blast is a spherical shockwave, not a line of sight: it
				// travels over every metre of relief between the centre and
				// the lip, so a mark on the rim is reached even when the
				// straight ray to it clips a rise.  The LOS test is right for
				// a directional spell (a bolt really is blocked by a hill)
				// but wrong here, and applying it to the rim ring dropped
				// two or three marks per crater whenever the ground was not
				// flat - which is exactly when a crater is most visible, and
				// left it lopsided.  Directional and shout patterns keep the
				// test.
				if (event.kind != Kind::kExplosion && !ClearPath(tes, origin, point)) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Stroke drop: path blocked from ({:.1f},{:.1f}) to ({:.1f},{:.1f})", origin.x, origin.y, point.x, point.y); }
					continue;
				}
				const auto ground = Surfaces::GroundAt(point);
				const auto& response = ground.response;
				if (ground.type == Surfaces::Type::kUnknown || ground.type == Surfaces::Type::kStone ||
					response.depthScale <= 0) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Stroke drop: surface type={} depthScale={:.2f}", static_cast<int>(ground.type), response.depthScale); }
					continue;
				}
				Clipmap::Stamp stamp{};
				stamp.snow = ground.type == Surfaces::Type::kSnow;
				const float span = stamp.snow && Settings::snowStampRimSpan >= 0 ?
					Settings::snowStampRimSpan : Settings::stampRimSpan;
				const float lean = stamp.snow && Settings::snowStampRimLean >= 0 ?
					Settings::snowStampRimLean : Settings::stampRimLean;
				stamp.x = point.x; stamp.y = point.y;
				// A blast does not take the rim-span deduction.
				//
				// StampRadius divides by 1 + span * (1 + lean) because the
				// shader's other heap channel (stamp.rim, :359) spreads the
				// stamp outward past s.z by exactly that factor, so shrinking
				// s.z by it keeps the total footprint where the pattern asked.
				// That is the right arithmetic for a footprint - and it is
				// what made the fireball invisible.
				//
				// Blast strokes pin stamp.rim to 0 below, so rim.x is 0, so
				// the shader's spread never happens - but the division did.
				// Snow's own span is 1.5 and its lean 0.15, a support of
				// 2.725, so a blast stroke asking for radius 63.16 stamped at
				// 23.18: the crater came out 46 units across instead of 126,
				// and the rim heaps came out 6 units across instead of 22 and
				// 83-102 units away from the centre, which reads exactly as
				// "one small hole in an empty field".
				//
				// So a blast asks for the radius it wants and gets it, and
				// only the surface's own radius response scales it.  The
				// directional patterns keep the deduction, because they do
				// not pin their rim and really are expanded by it.
				stamp.radius = event.kind == Kind::kExplosion ?
					ImpactPatterns::PlainStampRadius(stroke.radius, response.radiusScale) :
					ImpactPatterns::StampRadius(stroke.radius, response.radiusScale, span, lean);
				if (stamp.radius <= 0.5f) {
					if (Settings::logMagicImpacts && g_probeLogged < 32) { ++g_probeLogged; logger::info("Stroke drop: stamp radius={:.3f} from stroke={:.2f} surfaceScale={:.2f} span={:.2f} lean={:.2f}", stamp.radius, stroke.radius, response.radiusScale, span, lean); }
					continue;
				}
				stamp.motionX = stroke.motionX; stamp.motionY = stroke.motionY;
				// Depth is signed, and the sign is the whole trick.
				//
				// The shader computes a stamp's contribution as
				// -s.w * falloff  (ClipmapUpdateCS.h:348), so a positive
				// stamp.depth digs and a negative one piles up.  The falloff
				// there is a smoothstep from the centre out to s.z, which
				// covers the whole disc.  That is the only channel with
				// whole-disc coverage: the other heap channel, stamp.rim
				// (:359), is gated on rim.x and multiplies its span by the
				// rim lean, so it collapses to a thin ring at the stamp's own
				// edge when the lean is 1 - the snow around a footprint, not
				// a mound at a chosen place.
				//
				// So a blast's ring is written as depth = -height: a disc of
				// raised snow with no pit anywhere under it.  strength 0 with
				// a negative rim is the shape the pattern asks for, and
				// strength 1 with rim 0 is the bowl.
				const float pit = stroke.strength;
				const float heap = stroke.rim > 0.0f ? stroke.rim : 0.0f;
				stamp.depth = depth * elementScale * response.depthScale * (pit - heap);
				// The per-stamp lip a pit raises.  Kept for the strokes that
				// do not ask for a heap of their own (stroke.rim < 0): the
				// directional spells and plain magic hits, which have always
				// had a lip proportional to their depth.  A stroke that does
				// state a rim gets its snow from the signed depth above
				// instead, so the two never double up.
				stamp.rim = stroke.rim >= 0.0f ? 0.0f :
					stamp.depth * Settings::magicImpactRimScale *
					std::clamp(response.rimScale, 0.0f, 2.0f);

				if (stamp.snow && event.element == Element::kFire && Settings::heatMeltsSnow &&
					(event.kind != Kind::kExplosion || Settings::heatFromExplosions)) {
					stamp.depth = std::max(stamp.depth, Weather::SnowDepth() * Settings::magicImpactSnowMelt * stroke.strength);
				}
				// Clamped against the shader's own limit, not against zero:
				// the lower bound is what a heap is made of, and clamping it
				// away would silently turn every ring back into a pit that
				// simply is not there.  The asymmetric bounds are deliberate
				// - a heap taller than the crater is deep reads as a snow
				// bank, which is the intent.
				stamp.depth = std::clamp(stamp.depth, -64.0f, 64.0f);
				// A crater needs a wall, and a wall is what shoulder buys.
				//
				// The shader's falloff is 1 - smoothstep(s.z * shoulder, s.z, d)
				// (ClipmapUpdateCS.h:324).  At shoulder 0 the falloff runs from
				// the centre all the way to the lip, so the whole disc is one
				// long slope: at the reach a fireball asks for - 63 units of
				// radius against 14.6 of depth - that is a 19 degree dish, which
				// is what "a shallow dent, not a hole" is.  With a shoulder the
				// first shoulder fraction of the radius stays at full depth, and
				// the drop is packed into the last (1 - shoulder) of it: the same
				// crater becomes a flat floor with a wall around it, 44 degrees
				// at 0.65.
				//
				// The clamp this replaces was 0.5, on the reasoning that a
				// footprint wants a soft edge - but a blast is a different
				// shape, and that clamp is what kept every crater a dish no
				// matter what the surface or the ini asked for.  The blast
				// now states its own shoulder, and the ceiling is raised for
				// both so an ini value is never silently rewritten.
				//
				// Snow supplies shoulder 0.00, so this changes nothing
				// outside explosions - the directional spells and plain hits
				// keep whatever their surface asked for.
				const float shoulder = event.kind == Kind::kExplosion ?
					std::clamp(Settings::explosionImpactShoulder, 0.0f, 0.9f) :
					std::clamp(response.shoulder, 0.0f, 0.9f);
				stamp.shoulder = shoulder;
				// Only the bowl gets a torn rim.  The eight bank capsules
				// are already irregular by construction - each has its own
				// centre, thrust and thickness - and bulging a capsule's
				// outline would warp the run/width split the shader builds
				// it from.  The bowl is the one stroke whose shape is a bare
				// length, so it is the one stroke that needs the term.
				stamp.rimBulge = event.kind == Kind::kExplosion && stroke.strength > 0.0f ?
					std::clamp(Settings::explosionImpactRimBulge, 0.0f, 0.5f) :
					0.0f;
				// Loose snow thrown clear of the lip.  The bowl is the only
				// stroke that gets it, and for the same reason it is the
				// only stroke that gets a torn rim: a bare round stamp's
				// edge has no shape of its own, so the crater it leaves is
				// smooth-cut, while every bank capsule is already an
				// irregular run of its own.
				//
				// Only where there is snow to throw.  A footprint's lip
				// noise is a snow effect throughout - the profile it is read
				// from is the snow profile - and raising crumbs on bare
				// ground would be soil behaving like powder.
				stamp.rimNoise = event.kind == Kind::kExplosion && stroke.strength > 0.0f && stamp.snow ?
					std::clamp(Settings::explosionImpactRimNoise, 0.0f, 8.0f) :
					0.0f;
				stamp.lipBand = event.kind == Kind::kExplosion && stroke.strength > 0.0f && stamp.snow ?
					std::clamp(Settings::explosionImpactRimNoiseBand, 0.0f, 0.5f) :
					0.0f;
				stamp.decay = std::clamp(Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(), 0.0f, 0.9999f);
				out.push_back(stamp);
			}

			// Snow thrown clear by the blast.
			//
			// The particle spray had only ever been fed from a footfall
			// (Clipmap.cpp, the actor stamp path), so a fireball landing in
			// snow dug a hole and then sat there: the geometry said "impact"
			// and nothing moved.  This is the missing half of the crater -
			// the part that is airborne - and it is fired once per blast
			// rather than once per stroke, because the ring is a single
			// event.
			//
			// The lip's radius is the bowl's own, taken from the stamp that
			// was actually built rather than from the pattern: the pattern's
			// radius has been through the surface response by now, so a
			// profile that scales it down would otherwise throw its snow
			// from a ring wider than the hole it is standing on.
			if (event.kind == Kind::kExplosion && out.size() > before) {
				const auto& bowl = out[before];
				if (bowl.snow && bowl.rimNoise > 0.0f) {
					// The throw scales with how much snow there is to move
					// and with how hard this particular spell hits.  The
					// speed is well above a walk's, so Update() drives the
					// spray at the top of its range and the flakes leave
					// fast enough to read as thrown rather than dropped.
					const float thrown = std::clamp(
						std::abs(bowl.depth) * 60.0f + bowl.radius * 1.2f, 60.0f, 900.0f);
					const float amount = std::clamp(
						Settings::snowSparkleRate * 1.4f, 0.0f, 4000.0f);

					RE::NiPoint3 lip = contact;
					lip.z += 3.0f;
					SnowSparkle::NoteBurst(Surfaces::Type::kSnow, lip, bowl.radius,
						thrown, 32, amount);
				}
			}
			if (Settings::logMagicImpacts && g_logged < 32 && out.size() > before) {
				++g_logged;
				logger::info("Magic impact kind={} source={:08X} reach={:.1f} stamps={}",
					static_cast<int>(event.kind), event.source, reach, out.size() - before);
				if (event.kind == Kind::kExplosion && !out.empty()) {
					// The values that decide the crater's shape, printed
					// because a crater is judged by eye and the eye cannot
					// tell a 0.5 shoulder from a 0.65 one by reading code.
					const auto& bowl = out.front();
					logger::info("Explosion crater: radius={:.1f} depth={:.2f} shoulder={:.2f} wallAngle={:.0f}deg rimBulge={:.2f} rimNoise={:.2f} lipBand={:.3f} snow={}",
						bowl.radius, bowl.depth, bowl.shoulder,
						bowl.shoulder < 0.999f ?
							std::atan(1.5f * std::abs(bowl.depth) /
								std::max(bowl.radius * (1.0f - bowl.shoulder), 1e-4f)) *
								57.2957795f :
							90.0f,
						bowl.rimBulge, bowl.rimNoise, bowl.lipBand,
						bowl.snow ? "yes" : "no");
				}
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
		g_enabled.store(Settings::enableMagicImpacts || Settings::enableShoutImpacts ||
			Settings::enableExplosionImpacts, std::memory_order_relaxed);
	}

	void Append(const RE::NiPoint3& anchor, std::vector<Clipmap::Stamp>& out)
	{
		// Contacts are only good for a moment, and this is the one place that
		// runs every frame on the main thread, so the sweep belongs here
		// rather than in a timer of its own.
		ObjectStamps::PruneContacts();

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
		ProjectileHit<RE::BeamProjectile>::Install();
		ProjectileHit<RE::FlameProjectile>::Install();
		ProjectileHit<RE::ConeProjectile>::Install();
		ProjectileHit<RE::GrenadeProjectile>::Install();
		REL::Relocation<std::uintptr_t> table{ RE::Explosion::VTABLE[0] };
		ExplosionInit::func = table.write_vfunc(0xA2, ExplosionInit::thunk);
		if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
			source->AddEventSink<RE::TESSpellCastEvent>(&g_castSink);
		}
		logger::info("Installed bounded projectile/explosion impacts and directional shout events");
		logger::info("Contact sampling: probe shafts={} max miss={:.1f} log samples={}",
			Settings::contactProbeShafts, Settings::contactProbeMaxMiss,
			Settings::logContactSamples);
	}
}
