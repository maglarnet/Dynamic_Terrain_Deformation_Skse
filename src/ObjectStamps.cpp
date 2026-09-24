// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "Profiler.h"

#include "ObjectStamps.h"
#include "ObjectStampFilter.h"

#include "ActorShapes.h"
#include "ContactSampler.h"
#include "Globals.h"
#include "HeatSources.h"
#include "LogBudget.h"
#include "MeshGeometry.h"
#include "Settings.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <chrono>
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

		// A projectile's contact, as reported by the engine through the hit
		// hook.  Keyed by the projectile reference and dropped after a second,
		// so a projectile that landed and was recycled cannot hand its landing
		// point to whatever reference reuses its id next.
		struct ContactRecord
		{
			RE::FormID     form{};
			RE::NiPoint3   position{};
			std::chrono::steady_clock::time_point time{};
			bool           live{ false };
		};

		std::array<ContactRecord, 64> g_contacts{};
		size_t                        g_nextContact{};

		// The engine's "first thing along this ray", wired to the same call
		// the engine's own projectiles use.  Used from the main thread only,
		// under the same lock that guards the candidate list.
		//
		// The output carries no hit point - only a normal and a fraction along
		// the ray - so the point is interpolated, which is exactly what the
		// shelter ray already does (Shelter.cpp:113).  Both the from/to and
		// the answer are in world units: the physics world's own scale is
		// applied *inside* Pick, and the fraction is unitless either way.
		// True when a ray that hit this collidable has hit something that is
		// not the world: a projectile, an actor, or a piece of gear in flight.
		// A shaft's own bound sits around its origin, so a ray aimed down from
		// the origin hits the arrow first every time.  That is exactly what
		// made the landing point sit slightly *above* the origin with a miss
		// of zero: the ray never reached the ground at all.
		bool IsNotGround(const RE::hkpCollidable* a_collidable)
		{
			if (!a_collidable) {
				return false;
			}

			switch (a_collidable->GetCollisionLayer()) {
			case RE::COL_LAYER::kProjectile:
			case RE::COL_LAYER::kProjectileZone:
			case RE::COL_LAYER::kConeProjectile:
			case RE::COL_LAYER::kSpell:
			case RE::COL_LAYER::kSpellExplosion:
			case RE::COL_LAYER::kBiped:
			case RE::COL_LAYER::kBipedNoCC:
			case RE::COL_LAYER::kCharController:
			case RE::COL_LAYER::kDeadBip:
			case RE::COL_LAYER::kWeapon:
			case RE::COL_LAYER::kClutter:
			case RE::COL_LAYER::kProps:
				return true;
			default:
				return false;
			}
		}

		bool PickRay(float a_x, float a_y, float a_z, float a_toX, float a_toY, float a_toZ,
			float& a_outX, float& a_outY, float& a_outZ)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return false;
			}

			const float scale = RE::bhkWorld::GetWorldScale();

			// Walking the ray forward past anything that is not the world.
			// A single pick would otherwise be spent on the arrow's own bound
			// and the caller would be told the shaft landed on itself.
			float fromX = a_x;
			float fromY = a_y;
			float fromZ = a_z;

			for (int attempt = 0; attempt < 4; ++attempt) {
				RE::bhkPickData pick;
				pick.rayInput.from =
					RE::hkVector4(fromX * scale, fromY * scale, fromZ * scale, 0.0f);
				pick.rayInput.to =
					RE::hkVector4(a_toX * scale, a_toY * scale, a_toZ * scale, 0.0f);
				// Static and terrain collision only.  The projectile's own
				// layer is not in this set, so the arrow cannot answer its
				// own question, and neither can an actor standing over it.
				pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kGround);

				tes->Pick(pick);
				if (!pick.rayOutput.HasHit()) {
					return false;
				}

				const float t = std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);

				// What the ray actually answered with.  A mark in the wrong
				// place is decided here, and the layer is the one number that
				// says whether the ground answered or the shaft's own bound
				// did.  Cheap, and it beats inferring from the result.
				if (Settings::logContactSamples) {
					const auto* hit = pick.rayOutput.rootCollidable;
					logger::info(
						"Probe attempt {}: layer={} t={:.4f} from=({:.1f},{:.1f},{:.1f}) "
						"to=({:.1f},{:.1f},{:.1f})",
						attempt, hit ? static_cast<int>(hit->GetCollisionLayer()) : -1, t, fromX,
						fromY, fromZ, a_toX, a_toY, a_toZ);
				}

				if (!IsNotGround(pick.rayOutput.rootCollidable)) {
					a_outX = a_x + (a_toX - a_x) * t;
					a_outY = a_y + (a_toY - a_y) * t;
					a_outZ = a_z + (a_toZ - a_z) * t;
					return true;
				}

				// Something in flight answered.  Resume from just past it so
				// the next attempt can find the ground behind it, and give up
				// rather than loop if the ray has no room left.
				const float stepX = fromX + (a_toX - fromX) * t;
				const float stepY = fromY + (a_toY - fromY) * t;
				const float stepZ = fromZ + (a_toZ - fromZ) * t;
				const float pushX = (a_toX - fromX) * 0.01f;
				const float pushY = (a_toY - fromY) * 0.01f;
				const float pushZ = (a_toZ - fromZ) * 0.01f;

				fromX = stepX + pushX;
				fromY = stepY + pushY;
				fromZ = stepZ + pushZ;

				// Past the destination: nothing left to search.
				if ((a_toX - fromX) * (a_toX - a_x) < 0.0f ||
					(a_toY - fromY) * (a_toY - a_y) < 0.0f ||
					(a_toZ - fromZ) * (a_toZ - a_z) < 0.0f) {
					return false;
				}
			}

			return false;
		}

		ContactSampler::Inputs PickInputs()
		{
			return ContactSampler::Inputs{ &PickRay };
		}

		struct float2
		{
			float x{ 0.0f }, y{ 0.0f };
		};

		struct Tracked
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };

			// Where this object was when it was last given a stamp, and
			// whether it has had one at all.
			//
			// A spent arrow keeps its projectile component and stays in the
			// candidate set for as long as it lies there, so it was re-stamped
			// every objectStampInterval - 0.20 s, five times a second -
			// indefinitely.  The log showed three arrows stamped 13, 13 and 14
			// times each at identical coordinates, 19 ms apart: not three
			// holes, one hole pressed into itself until the rims of the run
			// read as a trench.  The field was there to be "a hole an arrow
			// made" and it was the one thing the repeated stamping could not
			// be.
			//
			// A mark is laid once at a place.  It is laid again only when the
			// object has moved far enough that the new place is a new place -
			// a second arrow, or a projectile still in flight - so a shaft
			// that has come to rest stops marking instead of grinding.
			float    markX{ 0.0f }, markY{ 0.0f };
			bool     marked{ false };
		};

		// How far an object must move from where it was last marked before it
		// earns another.  A still arrow drifts by fractions of a unit as the
		// physics settles and must not count; an arrow still travelling moves
		// tens of units per interval and must.  8 units is well clear of the
		// settling and well under a flight step, and it is deliberately larger
		// than the 4-unit pin-prick radius so a re-mark cannot land on top of
		// the mark it is replacing.
		constexpr float kRemarkDistance = 8.0f;

		std::unordered_map<RE::FormID, Tracked> g_motion;
		std::unordered_set<RE::FormID> g_visualBloodReported;

		// Newest first, so a reference that somehow collected two records
		// resolves to the one it most recently earned.
		const ContactRecord* FindContact(RE::FormID a_form)
		{
			for (size_t i = 0; i < g_contacts.size(); ++i) {
				const auto& record = g_contacts[(g_nextContact + g_contacts.size() - 1 - i) %
					g_contacts.size()];
				if (record.live && record.form == a_form) {
					return &record;
				}
			}
			return nullptr;
		}

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

		// A spent arrow sticks in the ground and goes on registering as a
		// projectile.  Its collision bound, though, spans the whole shaft, so
		// the generic object path below presses a crater as long as the arrow
		// and rings it with a rim - which reads as heaps of snow thrown up all
		// around the player, not as a hole an arrow made.  A shaft gets one
		// pin-prick instead: fixed small radius, shallow, and no rim.
		//
		// Every projectile counts, not only ammo: a spent projectile's origin
		// is not its tip whatever it was fired from, which is the reason the
		// mark and the object disagreed in the first place.
		bool IsShaft(RE::TESObjectREFR* a_ref)
		{
			if (a_ref->As<RE::Projectile>()) {
				return true;
			}
			const auto* base = a_ref->GetBaseObject();
			return base && base->GetFormType() == RE::FormType::Ammo;
		}

		std::unordered_set<RE::FormID> g_reported;
		// The reach line has a budget of its own.
		//
		// `g_reported` writes one line per object for the whole run, and the
		// first scan of a dropped object usually happens while it is still in
		// the air - `drop` positive, no snow over it, so the reach is zero and
		// the line takes the ceiling branch instead.  The scan that matters is
		// the one after it has landed, and a budget shared with the first is
		// spent before that scan exists.  A dropped object reads a positive
		// drop while it is in the air and a negative one once it has landed,
		// so a budget shared between the two is spent on the reading that
		// says nothing and the interesting one never prints.
		std::unordered_set<RE::FormID> g_reachReported;
		// The last buried-object reading, and how many have been written.
		// See the line itself for why it is rate limited rather than given a
		// budget per object.
		int64_t g_buriedAt{ 0 };
		size_t  g_buriedLines{ 0 };
		// Lines already written this session; reset with the rest of the state.
		size_t g_contactLines{ 0 };
		// Lines reporting a shaft that was held back; same treatment.
		size_t g_heldLines{ 0 };

		// Milliseconds from a steady clock, for every rate limit in this file.
		//
		// It sits above its first caller instead of beside the other log
		// state, because `LogObject` is the earliest one: a helper only the
		// later half of a file can see is one the next earliest caller cannot
		// use, and the compiler says so in a way that reads like a typo.
		int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		void LogObject(RE::TESObjectREFR* a_ref, Surfaces::Type a_surface,
			const Clipmap::Stamp& a_stamp, float a_drop, float a_depthCeiling, float a_reach)
		{
			if (!Settings::logObjectStamps) {
				return;
			}

			const auto* base = a_ref->GetBaseObject();
			if (!base) {
				return;
			}

			// What a buried object is actually being answered with.
			//
			// A mark deepened to reach a buried object prints a line of its
			// own, and what that line cannot say is why it did *not* print.
			// An object read as being under snow, yet stamped with the
			// shallow depth, has two readings in front of it and they call
			// for opposite fixes: the reach came out zero, or the reach rule
			// was never reached for that object.  Nothing already written
			// tells them apart, because a budget spent on the first, airborne
			// scan leaves the interesting one silent either way.
			//
			// So this line is unconditional: every object whose lowest point
			// is under the snow surface prints the quantities the rule is
			// made of - the drop, the reach derived from it, and the radius,
			// depth and shoulder the mark ended up with - at one a second,
			// with a session cap so a busy field cannot fill the file.  A
			// radius near twice the object's own half width with a shoulder
			// of 0.60 in one of these lines is the buried widening working;
			// the same line reading `snowOver=0.0` says the rule never ran,
			// and names the number that has to change.
			if (a_drop < 0.0f && g_buriedLines < 60 &&
				LogBudget::Allow(g_buriedAt, NowMs(), 1000)) {
				++g_buriedLines;
				logger::info(
					"Object buried: {} drop={:.1f} snowOver={:.1f} radius={:.1f} "
					"depth={:.1f} shoulder={:.2f} shape={} ceiling={:.1f}",
					a_ref->GetName(), a_drop, a_reach, a_stamp.radius, a_stamp.depth,
					a_stamp.shoulder, a_stamp.halfWidth > 0.0f ? "mesh" : "hull",
					a_depthCeiling);
			}

			// The reach case is reported first and by its own wording and its
			// own budget, because it is the one reading that says whether the
			// mark got down to the object at all.  A mark that reaches is the
			// difference between an object lying in a hole and an object that
			// is not on screen: with the ceiling alone the depth is capped at
			// the object's own thickness, which under a 35-unit blanket leaves
			// a fur helmet thirty units under the snow and a dent exactly
			// where it is not.
			//
			// It carries the shoulder as well as the radius, because a radius
			// alone cannot say whether the mark covers the object: the flat
			// floor is `radius * shoulder`, so the two numbers together are
			// what says whether the snow was taken off the object or only off
			// the ground under its middle.
			if (a_reach > 0.0f) {
				if (g_reachReported.size() >= 24 ||
					!g_reachReported.insert(base->GetFormID()).second) {
					return;
				}
				logger::info(
					"Object stamp: {} on {:<7} radius={:.1f} depth={:.1f} drop={:.1f} "
					"shoulder={:.2f} shape={} mark reaches the object, {:.1f} of snow "
					"over its own underside",
					a_ref->GetName(), Surfaces::Name(a_surface), a_stamp.radius, a_stamp.depth,
					a_drop, a_stamp.shoulder, a_stamp.halfWidth > 0.0f ? "mesh" : "hull",
					a_reach);
				return;
			}

			if (g_reported.size() >= 24 ||
				!g_reported.insert(base->GetFormID()).second) {
				return;
			}

			// The ceiling is only worth naming when it was reached, because
			// a mark that was cut down and one that was never near the limit
			// read the same otherwise - and "the object is buried in its own
			// dent" is fixed by raising this, while "the mark is too shallow"
			// is fixed by lowering ObjectStampDepthScale.  Opposite edits.
			if (a_depthCeiling > 0.0f) {
				logger::info(
					"Object stamp: {} on {:<7} radius={:.1f} depth={:.1f} drop={:.1f} shape={} "
					"depth capped to {:.1f} by the object's own thickness",
					a_ref->GetName(), Surfaces::Name(a_surface), a_stamp.radius, a_stamp.depth,
					a_drop, a_stamp.halfWidth > 0.0f ? "mesh" : "hull", a_depthCeiling);
				return;
			}

			logger::info(
				"Object stamp: {} on {:<7} radius={:.1f} depth={:.1f} drop={:.1f} shape={}",
				a_ref->GetName(), Surfaces::Name(a_surface), a_stamp.radius, a_stamp.depth,
				a_drop, a_stamp.halfWidth > 0.0f ? "mesh" : "hull");
		}

		// An object that reached this far and was turned away by the contact
		// gate.  Printed because the gate returning 0 is otherwise identical in
		// the log to an object that was never scanned: a run where the limit
		// rejected every loose item reads exactly like a run where loose items
		// were not considered at all, and the two call for opposite fixes.
		void LogObjectRefused(RE::TESObjectREFR* a_ref, float a_drop, float a_limit,
			bool a_floating)
		{
			if (!Settings::logObjectStamps || g_reported.size() >= 24) {
				return;
			}

			const auto* base = a_ref->GetBaseObject();
			if (!base || !g_reported.insert(base->GetFormID()).second) {
				return;
			}

			logger::info(
				"Object stamp: {} REFUSED - {} snow surface by {:.1f}, limit {:.1f} "
				"(raise Object{}Limit to accept)",
				a_ref->GetName(), a_floating ? "above" : "below", std::fabs(a_drop), a_limit,
				a_floating ? "ContactTolerance" : "Sink");
		}

		// Why an object's mark is a circle when its mesh should have been read.
		//
		// The stamp line says `shape=mesh` or `shape=hull`, and the second is
		// the one that needs this: without it, a session where every dropped
		// object falls back to its hull looks exactly like a session where the
		// mesh route was never added, and the two call for opposite fixes.
		//
		// Rate limited rather than given a one-shot budget.  ObjectStamps.cpp
		// already has counters of the "first N lines only" kind, and the shaft
		// path spent all of its own inside five seconds once and then went
		// silent for the rest of the run - so the budget is deliberately not
		// repeated here.  A gap covers the whole session instead, and the
		// per-object FormID set keeps one repeated object from filling it.
		int64_t g_objectMeshLineAt{ 0 };

		// The gap between two object-mesh lines.  Clipmap.cpp's own shaft
		// lines use five seconds for the same job; this one names the mesh
		// route for loose objects, which changes when a player picks things
		// up rather than every stride, so it is spaced wider.
		constexpr int64_t kObjectMeshLineGapMs = 10000;

		// The same time-gap treatment for the lift lines.  A lift happens when
		// an object comes to rest, so it is rarer than a mesh reading and the
		// gap can be wider without hiding anything a run needs to show.
		int64_t           g_objectLiftLineAt{ 0 };
		constexpr int64_t kObjectLiftLineGapMs = 10000;

		// The physics behind a lift, spaced much tighter than the lift line.
		//
		// The lift line is wide because a lift is supposed to be a rare,
		// settling event.  The object was found cycling - raised, back near
		// the ground, raised again - which is a lift every scan, ten a second
		// at the shipped interval, and the ten-second gap above would have
		// shown one of those hundred and named the count as one.  These lines
		// are what the cycle is diagnosed from, so they are spaced a second
		// apart instead: still bounded on a long run, but dense enough that
		// the number of rows is a reading of how often the lift fired.
		int64_t           g_objectLiftPhysAt{ 0 };
		constexpr int64_t kObjectLiftPhysGapMs = 1000;

		// Its own budget, for the one reading taken after the restore.
		//
		// The physics line is written before the body is handed back, so its
		// `motion=` describes the state the write created and cannot answer
		// whether the restore took.  That is why the freezing was argued
		// from a reading that never contained it.  This line is taken after
		// the single restore call, so its `motion=` is the body as the
		// solver has it.
		int64_t           g_objectLiftRestoredAt{ 0 };
		constexpr int64_t kObjectLiftRestoredGapMs = 1000;

		// Its own budget, not shared with the physics line above.
		//
		// Two lines printed from the same call site with one budget would
		// have the first one spend it and the second stay silent - and the
		// silent one is the one that names the gap these two lines exist to
		// measure.  A shared budget that hides its own second half is the
		// failure this project has paid for before, so they are counted
		// apart.
		int64_t           g_objectLiftFrameAt{ 0 };
		constexpr int64_t kObjectLiftFrameGapMs = 1000;

		// And a third, for the same reason again.
		//
		// A shared budget that hides its own second half is the failure this
		// project has paid for before - this line is the one that says
		// whether the write landed, so it must not be the one that stays
		// silent because a neighbour printed first.
		int64_t           g_objectLiftWriteAt{ 0 };
		constexpr int64_t kObjectLiftWriteGapMs = 1000;

		// A dead constant, and it is kept rather than deleted so that the
		// next reader does not add it back.
		//
		// `kLiftWarpSpeed` decided how large a raise had to be before the
		// body's velocity was zeroed with it, on the theory that a warp is a
		// teleport and a solver answers a teleport with the relative motion
		// it implies.  There is no warp left to account for, and a clearance
		// that fires on a raise the player caused is a clearance of the
		// player's own push.
		//
		// A constant that only appears in a comment is a liability rather
		// than a help: an unused `constexpr` at namespace scope is not
		// diagnosed, so it would sit here looking like live tuning to build
		// on.  Removing it is what makes the removal of the clear visible in
		// the file.
		//
		// The height terms are the ones that decide whether an object is
		// raised, so they are worth seeing; but they are printed per scan, and
		// a run with an object on the ground scans it several times a second.
		// Same rule as the lift line: a budget, so the terms are still there
		// after the opening seconds.
		int64_t           g_objectTermsAt{ 0 };
		constexpr int64_t kObjectTermsGapMs = 2000;

		void LogObjectMeshRefused(RE::TESObjectREFR* a_ref, const char* a_why)
		{
			if (!Settings::logObjectStamps) {
				return;
			}
			if (!LogBudget::Allow(g_objectMeshLineAt, NowMs(), kObjectMeshLineGapMs)) {
				return;
			}

			logger::info(
				"Object mesh: {} - {}; the collision hull's circle is in use "
				"(see MeshGeometry.h for what refuses a reading)",
				a_ref->GetName(), a_why);
		}

		// An object raised onto the snow, and by how much.
		//
		// Rate limited by a time gap rather than by a one-shot count.  A
		// budget spent once would go silent after the first few objects and
		// then a run where nothing was lifted and a run where the lift was
		// removed would look the same - which is the failure the shaft lines
		// already cost this project once.
		//
		// The three numbers are what make it checkable: the lift applied, the
		// object's own height it was derived from, and the blanket's thickness
		// it was clamped against.  Without them "it moved" and "it moved the
		// right amount" are the same line.
		void LogObjectLifted(RE::TESObjectREFR* a_ref, float a_rise,
			float a_height, float a_lift)
		{
			if (!Settings::logObjectStamps) {
				return;
			}
			if (!LogBudget::Allow(g_objectLiftLineAt, NowMs(), kObjectLiftLineGapMs)) {
				return;
			}

			logger::info(
				"Object lifted: {} by {:.1f} - its own height is {:.1f} and the "
				"blanket is {:.1f}, so its top now sits at the snow",
				a_ref->GetName(), a_rise, a_height, a_lift);
		}

		// How long the body has been standing still for - the one reading here
		// that changes on its own.
		//
		// The line this replaces printed the speed and spin the body happened
		// to hold at the instant of the write.  Both were read at that instant
		// and both were therefore always the same: a body that has just been
		// written has been given no velocity yet.  Eight raises of a helmet
		// printed `speed=(0.00,0.00,0.00) spin=(0.00,0.00,0.00) motion=3
		// restored=1` every time - four readings that cannot disagree with
		// anything, printed next to the object being reported in game as
		// unmovable.
		//
		// `rest` is `deactivationNumInactiveFrames`, which the solver
		// increments every step it finds the body still: a body that really is
		// resting climbs towards the engine's own threshold, and a body being
		// pushed, or integrated and drifting, reads small.  It is a count of
		// steps and not a velocity, so unlike a velocity it keeps the history
		// of the last few hundred milliseconds rather than only the present
		// frame.  That is what makes it able to contradict the rest of this
		// line.
		//
		// `lift` is printed beside it because it is the other number that was
		// silently at its limit while nothing else changed: the surface the
		// object is being raised towards is `landZ + lift`, and `lift` reads
		// the blanket's whole thickness on every line of a run - meaning the
		// rule is not finding a snow surface that varies, it is finding one
		// that is always the full 35.
		void LogLiftPhysics(RE::bhkNiCollisionObject* a_object, float a_beforeZ,
			float a_rise, float a_afterZ, float a_lift)
		{
			if (!Settings::logObjectStamps || !a_object || !a_object->body.get()) {
				return;
			}
			if (!LogBudget::Allow(g_objectLiftPhysAt, NowMs(), kObjectLiftPhysGapMs)) {
				return;
			}

			auto* rigid = a_object->body.get()->AsBhkRigidBody();
			auto* hkpRigid = rigid ?
				skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) :
				nullptr;
			if (!hkpRigid) {
				return;
			}

			const auto& motion = hkpRigid->motion;

			float lin[4];
			float ang[4];
			_mm_storeu_ps(lin, motion.linearVelocity.quad);
			_mm_storeu_ps(ang, motion.angularVelocity.quad);

			logger::info(
				"Lift physics: centre.z {:.2f} -> {:.2f} (raised {:.2f}) "
				"| speed=({:.2f},{:.2f},{:.2f}) spin=({:.2f},{:.2f},{:.2f}) "
				"| lift={:.2f} motion={} rest={}",
				a_beforeZ, a_afterZ, a_rise,
				lin[0], lin[1], lin[2], ang[0], ang[1], ang[2],
				a_lift,
				static_cast<int>(motion.type.get()),
				static_cast<int>(motion.deactivationNumInactiveFrames[0]));
		}

		// The body's state after it has been handed back, which is the only
		// reading in the log that can say whether the restore took.
		//
		// `motion=` here is read after the single restore call, so 1 means
		// the body is dynamic again and 4 means it is still keyframed.  The
		// same field is printed by the physics line above, but that one is
		// read before the restore and is therefore always the state the
		// write created - a reading that cannot answer the question it looks
		// like it answers.
		//
		// Paired with the next line's `scan`, this separates the two ways
		// one symptom can be produced: `motion=4` is a restore that did not
		// take (nothing will ever push it), while `motion=1` with the next
		// scan lower is a restore that took and a body that is awake with
		// nothing under it (the raise undoes itself, ten times a second).
		void LogLiftRestored(RE::bhkNiCollisionObject* a_object, RE::TESObjectREFR* a_ref)
		{
			if (!Settings::logObjectStamps || !a_object || !a_object->body.get()) {
				return;
			}
			if (!LogBudget::Allow(g_objectLiftRestoredAt, NowMs(), kObjectLiftRestoredGapMs)) {
				return;
			}

			auto* rigid = a_object->body.get()->AsBhkRigidBody();
			auto* hkpRigid = rigid ?
				skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) :
				nullptr;
			if (!hkpRigid) {
				return;
			}

			const auto& motion = hkpRigid->motion;

			float lin[4];
			_mm_storeu_ps(lin, motion.linearVelocity.quad);

			RE::hkVector4 massCentre;
			rigid->GetCenterOfMassWorld(massCentre);
			float parts[4];
			_mm_storeu_ps(parts, massCentre.quad);

			// Where the scene graph is actually drawing.
			//
			// `a_ref->GetPosition()` is the reference's own record - the
			// field this block itself writes - so it cannot witness its own
			// write.  A node's `world.translate` is what the renderer uses,
			// and with the warp removed this is the one reading that can
			// answer what is left of the question: whether the engine copies
			// the body's position back over the node between scans, which
			// would erase the raise and put the object under the snow again
			// while every other number on this line still looked right.
			const auto* renderNode = a_ref->Get3D();
			const float rendered = renderNode ? renderNode->world.translate.z : 0.0f;

			logger::info(
				"Lift restored: node {:.2f} | centre {:.2f} motion={} rest={} "
				"| speed=({:.2f},{:.2f},{:.2f}) | render {:.2f}",
				a_ref->GetPosition().z,
				parts[2] * RE::bhkWorld::GetWorldScaleInverse(),
				static_cast<int>(motion.type.get()),
				static_cast<int>(motion.deactivationNumInactiveFrames[0]),
				lin[0], lin[1], lin[2],
				rendered);
		}

		// The two z values that are not the same number, printed side by side.
		//
		// `SetPosition` moves the reference's own node; the scan's `centre`
		// is the physics body's centre of mass.  They are read from different
		// layers of the engine and were being compared as if they were one
		// quantity - which is how a lift of 1.61 came to be reported next to
		// a centre that had moved 45.  Nothing in the log said which of the
		// two each number belonged to, so the gap could be read as the move
		// being amplified, or as the read-back being wrong, and those need
		// opposite fixes.
		//
		// So both are printed for the same instant: the node before and
		// after, the mass centre before and after, and the raise that was
		// asked for.  If the node moved by the raise and the centre did not,
		// the two layers disagree about where the object is.  If both moved
		// by more than the raise, something outside this block is moving the
		// object.
		void LogLiftFrames(RE::TESObjectREFR* a_ref, float a_nodeBefore, float a_nodeAfter,
			float a_centreBefore, float a_centreAfter, float a_rise)
		{
			if (!Settings::logObjectStamps) {
				return;
			}
			if (!LogBudget::Allow(g_objectLiftFrameAt, NowMs(), kObjectLiftFrameGapMs)) {
				return;
			}

			logger::info(
				"Lift frames: asked {:.2f} | node {:.2f} -> {:.2f} (moved {:.2f}) "
				"| centre {:.2f} -> {:.2f} (moved {:.2f})",
				a_rise,
				a_nodeBefore, a_nodeAfter, a_nodeAfter - a_nodeBefore,
				a_centreBefore, a_centreAfter, a_centreAfter - a_centreBefore);
		}

		// Where the write actually landed.
		//
		// The line this replaces printed a `from` read out of the scan, on the
		// theory that it and the value written a moment later belonged to the
		// same instant.  They do not: the scan runs at 0.1 s and this line is
		// written once a second, so `from` could be a tenth of a second - and
		// several units of drift - older than the write it was being
		// subtracted from.  That subtraction is what produced the "a raise of
		// 1.61 moved the centre 45" reading, and two rounds of code were
		// written to explain a number that was arithmetic on two clocks.
		//
		// So the three values printed are the ones that can be compared:
		//
		//   scan  -> what the last scan left as the object's centre
		//   wrote -> what this call put in (`centre + rise`), world units
		//   after -> the body's centre, read immediately after the write
		//
		// `wrote` and `after` are both read at the write, so they are the pair
		// that says whether the placement landed.  `scan` is printed beside
		// them only so a reader can see how far apart the two clocks are, and
		// it is named `scan` rather than `from` so that it cannot be mistaken
		// for the same instant a second time.
		void LogLiftWrite(RE::TESObjectREFR* a_ref, float a_scanZ, float a_wroteZ,
			float a_afterZ)
		{
			if (!Settings::logObjectStamps) {
				return;
			}
			if (!LogBudget::Allow(g_objectLiftWriteAt, NowMs(), kObjectLiftWriteGapMs)) {
				return;
			}

			logger::info(
				"Lift write: scan {:.2f} -> wrote {:.2f} | body after {:.2f} (landed {})",
				a_scanZ, a_wroteZ, a_afterZ,
				std::fabs(a_wroteZ - a_afterZ) <= 0.01f ? "yes" : "no");
		}

		// A shaft that was asked for a mark and denied one, because it already
		// has one where it still is.
		//
		// Bounded by its own count rather than by object type: the whole
		// question this line answers is how many times the same arrow came
		// back, so deduplicating by object would hide exactly the number it
		// exists to report.  Before the gate, three arrows produced 41 contact
		// lines; after it, the same three should produce a small handful of
		// these and no second marks.
		void LogShaftHeld(RE::TESObjectREFR* a_ref, float a_markX, float a_markY)
		{
			constexpr size_t kMaxHeldLines = 40;

			if (!Settings::logObjectStamps || g_heldLines >= kMaxHeldLines) {
				return;
			}
			++g_heldLines;

			const auto position = a_ref->GetPosition();
			logger::info(
				"Shaft held: {} already marked at ({:.1f}, {:.1f}), now at ({:.1f}, {:.1f}), "
				"moved {:.2f} units - no second mark",
				a_ref->GetName(), a_markX, a_markY, position.x, position.y,
				std::hypot(position.x - a_markX, position.y - a_markY));
		}

		// One line per shaft, carrying the fields a wrong answer shows up in:
		// which answer was used, where it landed relative to the object, how
		// far the object's origin was from the ground, and whether the mark
		// was kept at all.  A mark that is clearly on the snow and clearly
		// not where the arrow is has to be visible from the log alone.
		void LogContact(RE::TESObjectREFR* a_ref, const ContactSampler::Output& a_contact,
			const ContactSampler::Query& a_query, float a_miss, bool a_kept)
		{
			// Bounded by a plain count, not by arrow type.  Deduplicating on
			// the base form meant only the first arrow of each kind was ever
			// reported, so firing five arrows produced one line and looked
			// exactly like four arrows never being sampled at all.  A cap is
			// needed; a cap per type is a cap that hides the evidence.
			constexpr size_t kMaxContactLines = 40;

			if (!Settings::logContactSamples || g_contactLines >= kMaxContactLines) {
				return;
			}
			++g_contactLines;

			const float above = a_query.hasReference ?
				a_query.referenceZ - a_contact.z : 0.0f;

			logger::info(
				"Contact {}: {} origin=({:.1f},{:.1f},{:.1f}) landed=({:.1f},{:.1f},{:.1f}) "
				"lane={} above={:.1f} miss={:.1f} len={:.1f} kept={}",
				ContactSampler::SourceName(a_query.source), a_ref->GetName(),
				a_query.referenceX, a_query.referenceY, a_query.referenceZ,
				a_contact.x, a_contact.y, a_contact.z,
				ContactSampler::OriginName(a_contact.origin), above, a_miss,
				a_query.length, a_kept);
		}

		constexpr size_t kMaxShapesPerObject = 3;

		struct ShapeBound
		{
			RE::NiPoint3 centre;
			float        radius{ 0.0f };
			// The collidable this bound was measured from.  Kept so the mesh
			// behind it can be read later without a second walk of the scene
			// graph: the walk that produced these numbers is the walk that
			// reaches the node the mesh hangs off, and repeating it would
			// both cost another traversal and risk picking a different
			// collidable than the one these numbers describe.
			RE::bhkNiCollisionObject* collidable{ nullptr };
			// The long and short half axes, kept from the shape itself.  A
			// bound radius alone cannot say "long and thin", and an arrow is
			// the one object where that distinction is the whole point.
			float        length{ 0.0f };
			float        thickness{ 0.0f };
			// How far this shape reaches vertically from its own centre.
			// Kept apart from `radius` because the two are different
			// distances: `radius` has to cover the shape in every direction
			// and for a box is its space diagonal, while this is only the
			// upward and downward reach.  A drop test that subtracts the
			// diagonal from the centre puts the object's lowest point well
			// below the ground it is resting on, and its mark lands under
			// the object rather than at its feet.
			float        vertical{ 0.0f };
			// Which way the long axis points, in world space.  Without it the
			// only ray a shaft can be probed with is a vertical one, which
			// lands under the shaft's own origin by construction - the very
			// error these axes were added to expose.
			RE::NiPoint3 axis{ 0.0f, 0.0f, 1.0f };
			bool         hasAxis{ false };
			// Diagnostic mirror of the same two fields on the extent, so the
			// log can say which route measured the axes.
			bool         fromChildren{ false };
			int          childCount{ 0 };
			// The raw projections, un-scaled, straight from the engine.  A
			// zero length beside a non-zero radius cannot be explained from
			// the scaled numbers alone.
			bool         measured{ false };
			float        rawPX{ 0.0f }, rawMX{ 0.0f };
			float        rawPY{ 0.0f }, rawMY{ 0.0f };
			float        rawPZ{ 0.0f }, rawMZ{ 0.0f };
			// Whether GetExtent reported success at all.  A failed call
			// leaves length and thickness at their initial zeros, which is
			// indistinguishable in the log from a successful call that
			// measured a genuinely thin shape.
			bool         extentOk{ false };
			// Which step declined, when it did.
			const char*  extentStep{ "none" };
			// Set only for the zero-shape fallback, where the radius comes
			// from the scene graph's world bound rather than from any
			// collidable.  A reader that treats the two alike will take the
			// bound radius for a measured one.
			bool         fromWorldBound{ false };
		};

		size_t GatherShapes(RE::NiAVObject* a_root, ShapeBound (&a_out)[kMaxShapesPerObject])
		{
			std::vector<ShapeBound> found;
			// Counted rather than assumed.  "No shapes collected" and "one
			// shape collected with a zero length" produce the same log line
			// unless the visit is counted, and they are opposite problems:
			// the first means the walk never reached a collidable, the second
			// means it reached one and the measurement failed.
			size_t visited = 0;
			size_t boundFailed = 0;
			// Why a collidable was rejected, counted per cause.  "Rejected"
			// on its own does not say whether the node had no body, had a
			// body that is not a rigid body, or had one that made no sense -
			// and an arrow is exactly the case where the body may well be a
			// phantom rather than a rigid body, which is a different fix
			// from a shape whose measurement failed.
			size_t noBody = 0;
			size_t notRigid = 0;
			size_t shapeFailed = 0;
			RE::BSVisit::TraverseScenegraphCollision(
				a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
					++visited;
					ShapeBound bound{};
					bound.collidable = a_object;
					if (ActorShapes::GetBound(a_object, bound.centre, bound.radius) &&
						bound.radius > 0.1f) {
						// A second, cheap walk of the same node for the two
						// axes; a projection per axis is all it costs, and
						// only for nodes that already produced a bound.
						ActorShapes::Extent extent{};
						if (ActorShapes::GetExtent(a_object, bound.centre, extent)) {
							bound.length = extent.length;
							bound.thickness = extent.thickness;
							bound.vertical = extent.vertical;
							bound.fromChildren = extent.fromChildren;
							bound.childCount = extent.childCount;
							bound.measured = extent.measured;
							bound.rawPX = extent.rawPX;
							bound.rawMX = extent.rawMX;
							bound.rawPY = extent.rawPY;
							bound.rawMY = extent.rawMY;
							bound.rawPZ = extent.rawPZ;
							bound.rawMZ = extent.rawMZ;
							bound.extentOk = true;
						}
						// Recorded even on failure.  The step name is the whole
						// point of the last round's probe: "length is zero" was
						// consistent with four different causes and the log had
						// no way to pick one.
						bound.extentStep = ActorShapes::LastExtentStep();
						// And the direction those axes run in.  A length is
						// not enough to aim with.
						RE::NiPoint3 axis{};
						if (ActorShapes::GetLongAxis(a_object, axis)) {
							bound.axis = axis;
							bound.hasAxis = true;
						}
						found.push_back(bound);
					} else {
						// The branch that used to be invisible.  A collidable
						// that produced no usable bound simply vanished, and
						// the world-bound fallback below then filled in a
						// radius that looked like a successful measurement -
						// which is exactly how "radius is 151 but length is 0"
						// was read as a contradiction for a whole round.
						if (!a_object || !a_object->body.get()) {
							++noBody;
						} else if (!a_object->body.get()->AsBhkRigidBody()) {
							++notRigid;
						} else {
							++shapeFailed;
						}
						++boundFailed;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});

			if (Settings::logContactSamples) {
				logger::info(
					"Shape walk: visited={} accepted={} rejected={} (noBody={} notRigid={} shapeFailed={})",
					visited, found.size(), boundFailed, noBody, notRigid, shapeFailed);
				// What the extent walk last saw, whether or not it succeeded.
				// Reported after the walk rather than inside it so one line
				// covers the whole attempt: the shape type says which branch
				// of the extent switch ran, and the six raw projections say
				// whether the engine gave back anything at all.
				const auto last = ActorShapes::LastExtent();
				logger::info(
					"  last extent: step={} shapeType={} ok={} length={:.1f} thickness={:.1f} radius={:.1f} raw(+X={:.2f} -X={:.2f} +Y={:.2f} -Y={:.2f} +Z={:.2f} -Z={:.2f})",
					ActorShapes::LastExtentStep(), ActorShapes::LastShapeType(), last.measured,
					last.length, last.thickness, last.radius, last.rawPX, last.rawMX, last.rawPY,
					last.rawMY, last.rawPZ, last.rawMZ);
			}

			if (found.empty()) {
				const auto& bound = a_root->worldBound;
				if (bound.radius <= 0.1f) {
					return 0;
				}
				// The fallback, marked as such.  A ShapeBound built here has a
				// radius but no half axes, and every downstream reader that
				// cannot tell the two apart will read the bound radius as a
				// shaft half length - which is where 302.7 came from.
				//
				// Written field by field rather than as a braced list.  A
				// positional list is only correct while the field order is,
				// and this struct gained a `collidable` member between
				// `radius` and `length`; a list of two values then depends on
				// the compiler's own reading of which members it can skip.
				// Measured on this toolchain it happens to leave `collidable`
				// null and put the radius where it belongs, which is the
				// outcome wanted - there is no mesh here, because there was no
				// collidable - but "happens to" is not a guarantee to build
				// a fallback on.
				a_out[0] = ShapeBound{};
				a_out[0].centre = bound.center;
				a_out[0].radius = bound.radius;
				a_out[0].fromWorldBound = true;
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

			// The object's own mesh, when it can be read.
			//
			// Everything above measures the collision hull, and a hull is a
			// capsule or a box.  Two objects of the same size therefore get
			// the same hull and would leave the same mark whatever their
			// meshes look like - which is why a dropped helmet stamped a
			// circle.  The mesh has the shape: BSTriShape keeps a CPU-side
			// copy of its vertices, so the object's own geometry can be read
			// rather than inferred, exactly as the carried-weapon path
			// already does.
			//
			// The fit is cached per mesh and only ever computed once, because
			// a mesh's own shape does not change - only the transform over it
			// does.  A scene full of unmeasured objects therefore costs one
			// walk per distinct mesh per session.
			//
			// Refusing is not a failure.  The hull is what this code used
			// before the mesh could be read at all, so a refused reading
			// costs the shape and not the mark, and the log says which it
			// was.
			MeshGeometry::Result mesh{};
			bool                haveMesh = false;

			if (Settings::objectStampFromMesh && shapes[0].collidable) {
				const bool measured = MeshGeometry::Measure(
					MeshGeometry::SceneObject(shapes[0].collidable), mesh);

				if (measured && !mesh.skinned) {
					// The same sanity rule the weapon path applies, so a node
					// that held the whole character rather than the object
					// cannot put a body-sized mark through the snow.
					const float hull =
						shapes[0].length + shapes[0].thickness + shapes[0].radius;
					const float box = mesh.unionX + mesh.unionY + mesh.unionZ;

					haveMesh = MeshShape::MeshTrusted(box, hull);

					if (!haveMesh) {
						LogObjectMeshRefused(a_ref,
							"union box far larger than the collision hull");
					}
				} else {
					LogObjectMeshRefused(a_ref,
						mesh.skinned ? "skinned mesh" :
									   "no geometry, no CPU-side vertices, or no "
									   "layout agreed");
				}
			}

		const bool shaft = IsShaft(a_ref);

		// A shaft that has already been marked and has not moved does not
		// get a second mark.  See Tracked::marked for what the repeated
		// stamping looked like in the log; the short version is that the
		// component keeps answering the candidate query forever and the
		// interval kept re-emitting it, so one arrow became a trench.
		//
		// The check is on distance from the last mark, not on speed: an
		// arrow that has stopped is still "moving" by tiny amounts as the
		// physics settles, and a rule that asked whether it was moving would
		// keep re-marking it.  Asking how far it has come from where the
		// mark was laid is a question with a stable answer.
		if (shaft) {
			const auto mark = g_motion.find(a_ref->GetFormID());
			if (mark != g_motion.end() && mark->second.marked) {
				const float dx = a_ref->GetPosition().x - mark->second.markX;
				const float dy = a_ref->GetPosition().y - mark->second.markY;
				if (dx * dx + dy * dy < kRemarkDistance * kRemarkDistance) {
					// Logged, because a gate that silently returns 0 cannot be
					// told apart in the log from an arrow that was simply
					// never re-scanned - and then a run where nothing changed
					// would read as a working suppression.  This line is the
					// only evidence the mark was found and used, so it names
					// what was suppressed and how still it was.
					LogShaftHeld(a_ref, mark->second.markX, mark->second.markY);
					return 0;
				}
			}
		}

		// The ground a shaft is judged against is not the ground under its
			// bound - a shaft's bound bottom is a whole half length below
			// wherever it actually met something.  So a shaft is resolved to a
			// real contact point first (the engine's reported hit if we have
			// it, otherwise a short ray down the shaft) and everything below
			// uses that point.  Anything else keeps the bound it already had.
			// The object's lowest point, and the question of which half
			// extent measures it.
			//
			// This used to read `shapes[0].centre.z - shapes[0].radius`.
			// `radius` is a single bound that has to cover the shape in
			// every direction, so for a box it is the space diagonal
			// `sqrt(hx^2 + hy^2 + hz^2)` - a horizontal size pressed into
			// service as a vertical one.  Measured in game on a dropped fur
			// helmet: `radius` 12.62 against a shape whose own vertical half
			// extent was 4.16, so the "lowest point" was placed 8.47 units
			// below where the helmet really was.  `drop` came out 17.7
			// instead of about 9, and the mark was stamped under the helmet
			// rather than at its rim - which reads in game as the object
			// still being buried, however the mark itself is shaped.
			//
			// The vertical reach is the correct quantity and it is the one
			// the mesh reading also offers, so the two agree instead of
			// describing different objects.
			ContactSampler::Output contact{};
			float                 groundZ = shapes[0].centre.z -
				(shapes[0].vertical > 0.0f ? shapes[0].vertical : shapes[0].radius);
			const RE::NiPoint3    boundCentre{ shapes[0].centre.x, shapes[0].centre.y,
				   shapes[0].centre.z };
			RE::NiPoint3 contactPoint = boundCentre;

			if (shaft) {
				ContactSampler::Query query{};
				query.mustBeContact = true;
				query.hasShape = true;

				// The shaft's own axes, not its bound diameter.  Feeding the
				// diameter as the length and half of it as the thickness made
				// the ratio exactly 2:1 for every projectile, which is below
				// the 4:1 a shaft needs - so the shaft lane never ran, no
				// ray was ever spent, and the fix could not take effect.
				//
				// Both axes are taken from *one* shape - the longest - rather
				// than each being the largest across all shapes.  Mixing them
				// describes a shape that does not exist: an arrow's longest
				// part is its shaft and its thickest may be a fletching slab,
				// and inheriting the slab's thickness while keeping the
				// shaft's length shrinks the ratio until the object stops
				// reading as a shaft at all.
				float longest = -1.0f;
				for (size_t i = 0; i < shapeCount; ++i) {
					if (shapes[i].length > longest) {
						longest = shapes[i].length;
						query.length = shapes[i].length;
						query.thickness = shapes[i].thickness;
					}
				}

				// Fall back to the bound only if the shape walk gave nothing:
				// a long thin ratio is then assumed rather than measured, and
				// the caller's own shaft verdict decides.
				if (!(query.length > 0.0f)) {
					for (size_t i = 0; i < shapeCount; ++i) {
						query.length = std::max(query.length, shapes[i].radius * 2.0f);
					}
					query.thickness = query.length * 0.25f;
				} else if (!(query.thickness > 0.0f)) {
					query.thickness = query.length * 0.25f;
				}

				// The axis the shaft will be probed along.  The widest shape
				// wins, to match how length and thickness were taken: the
				// shaft's own capsule, not a fletching slab.
				//
				// Chosen by comparing the shapes against *each other*, never
				// against query.length: that field may already have been
				// rewritten by the bound fallback above into a bound diameter
				// several times any real half axis, and a comparison against
				// it would then never hold - leaving hasAxis false and the
				// axis ray unspent, which is the silent no-op this whole
				// change exists to remove.
				float bestAxisLength = -1.0f;
				for (size_t i = 0; i < shapeCount; ++i) {
					if (shapes[i].hasAxis && shapes[i].length > bestAxisLength) {
						bestAxisLength = shapes[i].length;
						query.hasAxis = true;
						query.axisX = shapes[i].axis.x;
						query.axisY = shapes[i].axis.y;
						query.axisZ = shapes[i].axis.z;
					}
				}

				// The axes the shaft branch will act on.  Printed because a
				// length far larger than the arrow is long was once the only
				// visible symptom of the bound walk picking up the wrong
				// collidable, and there was no way to tell that from the
				// landing point alone.  The axis is printed for the same
				// reason: a ray fired down a wrong axis looks identical to
				// one fired down the right axis when only the landing point
				// is recorded, but not when the direction is.
				if (Settings::logContactSamples) {
					logger::info(
						"Shaft axes: shape(s)={} length={:.1f} thickness={:.1f} axis=({:.3f},{:.3f},{:.3f}) hasAxis={}",
						shapeCount, query.length, query.thickness, query.axisX, query.axisY,
						query.axisZ, query.hasAxis);
					for (size_t i = 0; i < shapeCount; ++i) {
						logger::info(
							"  shape {}: radius={:.1f} length={:.1f} thickness={:.1f} axis=({:.3f},{:.3f},{:.3f})",
							i, shapes[i].radius, shapes[i].length, shapes[i].thickness,
							shapes[i].axis.x, shapes[i].axis.y, shapes[i].axis.z);
					}
					// Which route produced the two axes above.  A shape that
					// reports no children was measured directly; one that
					// reports children was a collection and the numbers came
					// from the widest child.  Printed because the two routes
					// share an output format and a wrong length looks the
					// same either way.
					for (size_t i = 0; i < shapeCount; ++i) {
						logger::info(
							"  shape {}: children={} fromChildren={} extentOk={} measured={} step={} worldBound={}",
							i, shapes[i].childCount, shapes[i].fromChildren, shapes[i].extentOk,
							shapes[i].measured, shapes[i].extentStep, shapes[i].fromWorldBound);
					}
					// The engine's own projections, un-scaled.  These are the
					// numbers everything above is derived from, so a zero
					// length is decided here: six zeros mean the shape's
					// projection really is empty, while six non-zeros with a
					// zero length mean the arithmetic after it is wrong.
					for (size_t i = 0; i < shapeCount; ++i) {
						logger::info(
							"  shape {}: raw +X={:.2f} -X={:.2f} +Y={:.2f} -Y={:.2f} +Z={:.2f} -Z={:.2f}",
							i, shapes[i].rawPX, shapes[i].rawMX, shapes[i].rawPY, shapes[i].rawMY,
							shapes[i].rawPZ, shapes[i].rawMZ);
					}
				}

				query.source = ContactSampler::Source::kProjectile;

				if (a_ref->As<RE::Projectile>()) {
					// Matched on the projectile base, the same way the object
					// scan matches it, so a visual-only blood spray is
					// excluded here too and not just there.
					const auto* base = a_ref->GetBaseObject();
					const auto* model = base ? base->As<RE::TESModel>() : nullptr;
					const char* path = model ? model->GetModel() : nullptr;
					if (path && ObjectStampFilter::IsVisualBloodProjectile(path)) {
						return 0;
					}
					if (const auto* record = FindContact(a_ref->GetFormID())) {
						query.hasContact = true;
						query.contactX = record->position.x;
						query.contactY = record->position.y;
						query.contactZ = record->position.z;
					}
				}

				if (!query.hasContact && !Settings::contactProbeShafts) {
					// Ray probing off, and the engine never told us where this
					// one met something.  There is no honest landing point to
					// stamp, and inventing one is what put the hole in the
					// wrong place, so nothing is stamped.  The engine's own
					// contact, when it exists, is still used - that is free.
					LogContact(a_ref, contact, query, 0.0f, false);
					return 0;
				}

				const auto origin = a_ref->GetPosition();
				query.hasReference = true;
				query.referenceX = origin.x;
				query.referenceY = origin.y;
				query.referenceZ = origin.z;

				const auto inputs = PickInputs();
				contact = ContactSampler::Resolve(query, inputs);

				const auto miss = query.length > 0.0f ? contact.horizontalMiss : 0.0f;
				if (!contact.reachedGround ||
					(Settings::contactProbeMaxMiss > 0.0f &&
						miss > Settings::contactProbeMaxMiss)) {
					LogContact(a_ref, contact, query, miss, false);
					return 0;
				}

				contactPoint.x = contact.x;
				contactPoint.y = contact.y;
				contactPoint.z = contact.z;
				groundZ = contact.z;
				LogContact(a_ref, contact, query, miss, true);
			} else {
				for (size_t i = 1; i < shapeCount; ++i) {
					const float reach = shapes[i].vertical > 0.0f ? shapes[i].vertical :
																	shapes[i].radius;
					groundZ = std::min(groundZ, shapes[i].centre.z - reach);
				}
			}

			const RE::NiPoint3 probe{ contactPoint.x, contactPoint.y, contactPoint.z };

			auto* tes = RE::TES::GetSingleton();
			float landZ = 0.0f;
			if (!tes || !tes->GetLandHeight(probe, landZ)) {
				return 0;
			}

			const float lift     = SnowSurface::LiftAt(probe.x, probe.y);
			const float surfaceZ = landZ + lift;
			const float drop     = groundZ - surfaceZ;

			// Every term of the height question, and the bound they were
			// taken from.
			//
			// A helmet read `drop = 2.6` after the vertical-reach fix, where
			// a helmet standing on the bare terrain under a 35-unit blanket
			// should read about -8.2 (its own height, below the snow).  Two
			// readings of that number are consistent with it: the bound the
			// height came from is not the helmet, or the terrain under this
			// point is not the terrain the helmet is resting on.  Neither can
			// be told apart from the outside, so the terms are printed - the
			// bound's own centre and reach, the land height, the blanket, and
			// the resulting drop.  Printing the answer alone is what made the
			// last round's log ambiguous.
			if (Settings::logObjectStamps &&
				LogBudget::Allow(g_objectTermsAt, NowMs(), kObjectTermsGapMs)) {
				logger::info(
					"Object height terms: land={:.2f} lift={:.2f} surface={:.2f} "
					"| bound0 centre.z={:.2f} vertical={:.2f} radius={:.2f} "
					"length={:.2f} thickness={:.2f} fromChildren={} children={} "
					"measured={} extentStep={} | groundZ={:.2f} drop={:.2f} "
					"| shapes={} centreXY=({:.1f},{:.1f})",
					landZ, lift, surfaceZ,
					shapes[0].centre.z, shapes[0].vertical, shapes[0].radius,
					shapes[0].length, shapes[0].thickness,
					shapes[0].fromChildren, shapes[0].childCount,
					shapes[0].measured, shapes[0].extentStep,
					groundZ, drop,
					shapeCount, shapes[0].centre.x, shapes[0].centre.y);
			}

			// Two different questions, so two different limits.
			//
			// Above the snow: how far an object may float over the surface and
			// still count as resting on it.  A fixed, small allowance, because
			// "in the air" is the thing being excluded.
			//
			// Below the snow: how far down an object may sit.  An object lying
			// on the ground under the blanket reads drop = -lift, and lift is
			// the snow's whole thickness there - 35 units by default.  A fixed
			// limit cannot tell "buried to the ground, as it really is" from
			// "clipped through the world", and when the limit is set tighter
			// than the blanket (an ini with ObjectSinkLimit 60 and a tolerance
			// of 10 is the case that exposed this) the honest resting case is
			// rejected outright: the object leaves no mark at all, which reads
			// in game as gear that fell through the snow and vanished.
			//
			// The blanket's own thickness is the honest bound.  It comes from
			// this point's own lift, so it tracks shelter, weather and distance
			// fade instead of guessing one number for every place and hour.
			float sinkLimit = std::max(Settings::objectSinkLimit, lift);
			if (Settings::objectSinkFollowsSnow) {
				sinkLimit = std::max(sinkLimit, lift + Settings::objectSinkSlack);
			}

			if (drop > Settings::objectContactTolerance || drop < -sinkLimit) {
				LogObjectRefused(a_ref, drop,
					drop > 0.0f ? Settings::objectContactTolerance : sinkLimit, drop > 0.0f);
				return 0;
			}

			const auto  ground = Surfaces::GroundAt(probe);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				return 0;
			}

			// Lift the object so that its top sits at the snow, rather than
			// leaving it resting on the terrain under the blanket.
			//
			// The raise is a displacement of the terrain *mesh*; the engine's
			// collision - and therefore GetLandHeight - knows nothing about
			// it.  So a dropped object falls until its collision meets the
			// bare terrain and stops there, while the snow surface stands
			// `lift` units above it.  Since `drop = groundZ - (landZ + lift)`,
			// an object resting on the ground under a full blanket reads a
			// drop of about minus the blanket's own thickness, which puts the
			// snow surface a blanket's depth over its head.  An object
			// shorter than the blanket is therefore buried whole, whatever
			// the mark does.
			//
			// Raising the object by `lift - its own height` puts its top at
			// the surface, so it is visible, kickable, and has a mark at its
			// own feet.  The height is the shape's own vertical reach, the
			// same quantity the drop test uses, so the two cannot disagree
			// about how tall the object is.
			//
			// A physics body that owns a shape is not a thing to move.
			//
			// A dropped object carries a `bhkRigidBody` whose transform is
			// authoritative.  Writing the referenced object's position does
			// not move it, and moving the node the body hangs off is
			// overwritten by the solver on its next step.  Bracketing the
			// write with a motion-type switch does move it, but a body
			// switched back to dynamic and then warped onto the node is
			// integrated against nothing and cycles, and that same warp
			// throws away any displacement a push had just produced - which
			// is what an object that cannot be kicked is.
			//
			// So the raise is a placement and nothing more, and it is stated
			// against a reading taken in the same instant as the write.  A
			// body at rest in snow reads its mass centre and its reference
			// position within a few units of each other, and the raise asked
			// for is usually under ten, so the two are easy to confuse - the
			// more so when the scan that reads the reference runs ten times a
			// second and the line that prints it once.  Two samples a tenth
			// of a second apart read as a throw that never happened.
			//
			// The write is skipped while the object already stands at the
			// height the rule asks for, so nothing re-runs on every scan.
			// What is left is the state a push needs: a body the solver
			// integrates, whose own motion the next scan can see.
			if (Settings::objectLiftToSnow && !shaft && lift > 0.0f &&
				surface == Surfaces::Type::kSnow && shapes[0].vertical > 0.0f) {
				const float objectHeight = 2.0f * shapes[0].vertical;
				const float lowestZ = shapes[0].centre.z - shapes[0].vertical;

				const float rise = MeshShape::LiftOntoSnow(
					lowestZ, objectHeight, surfaceZ, lift, Settings::objectLiftSlack);

				// The rule's answer, with nothing done to the body.
				//
				// `rise` says how far the object has to travel; nothing says
				// whether it is worth writing, so the two cases are named:
				// inside the settled band the object is already where the rule
				// wants it and is left alone, outside it the write happens.  A
				// body left alone is one the solver integrates, which is what
				// a push needs.
				//
				// If the player pushes it back into the snow, `rise` grows
				// past the band and the block below runs again, which is the
				// intended behaviour rather than a fight with the solver.
				// The action this scan takes is recorded rather than acted on
				// in place, so that the question "does the body have to be
				// handed back?" is asked once, at the block's exit, of
				// `LiftMustFreeBody` - see that function for why the answer
				// cannot be left to the branches.
				auto action = MeshShape::LiftAction::kNone;
				if (rise <= MeshShape::LiftSettledBand(
						objectHeight, Settings::objectLiftSlack)) {
					action = MeshShape::LiftAction::kLeftInPlace;
				} else if (rise > Settings::objectLiftSlack) {
					action = MeshShape::LiftAction::kWrote;
					// `beforeZ` is the body's mass centre, and it is the only
					// `before` this move may be stated against.
					//
					// A `before` taken from the scan and an `after` read at the
					// write are not the same instant: the scan runs ten times a
					// second and a body resting in snow drifts several units in
					// that time, so the difference between the two reads as a
					// throw.  The write is therefore stated against the mass
					// centre, which is read in the instant of the write, and
					// `Lift write` prints the three values on one line.
					const float beforeZ = shapes[0].centre.z;
					const auto  nodeBefore = a_ref->GetPosition();

					// No motion-type switch on this write, and no warp.
					//
					// The write is a plain placement of the referenced
					// object's position.  `a_warp = true` would put the body
					// where the node was put, after which the solver
					// integrates it against nothing and it falls - that is the
					// rise the object showed on every scan - and the same warp
					// discards the displacement a push had just produced.  Both
					// the rise and the unkickable object are that one argument,
					// so the body is left exactly as the solver had it.
					const auto pos = a_ref->GetPosition();
					const float wroteZ = beforeZ + rise;
					a_ref->SetPosition(RE::NiPoint3{ pos.x, pos.y, wroteZ });

					// Nothing is done to the body here.
					//
					// No motion type is switched, so there is no state to hand
					// back and no `a_force` question to answer.  The one
					// write-through is at the block's single exit below, where
					// `LiftMustFreeBody` decides whether it is needed, so a
					// branch added later cannot skip it by not repeating it.
					a_ref->AddChange(RE::TESObjectREFR::ChangeFlags::kHavokMoved);

					// The velocity is not cleared here.
					//
					// A clearance keyed on the size of the raise also fires on
					// a raise the player caused, which is a clearance of the
					// player's own push.  There is no warp to answer for, and
					// a body that has just been placed is not moving, so there
					// is nothing to clear: whatever velocity it carries is the
					// solver's to compute from the collision it lands on.

					// Read the centre back from the body that own the
					// transform, immediately, so "the move stuck" is a
					// measurement and not a hope about the next scan.
					float afterZ = beforeZ;
					if (shapes[0].collidable && shapes[0].collidable->body.get()) {
						if (auto* rigid =
								shapes[0].collidable->body.get()->AsBhkRigidBody()) {
							RE::hkVector4 massCentre;
							rigid->GetCenterOfMassWorld(massCentre);
							float parts[4];
							_mm_storeu_ps(parts, massCentre.quad);
							afterZ = parts[2] * RE::bhkWorld::GetWorldScaleInverse();
						}
					}
					const auto nodeAfter = a_ref->GetPosition();
					LogLiftWrite(a_ref, beforeZ, wroteZ, afterZ);
					LogLiftPhysics(shapes[0].collidable, beforeZ, rise, afterZ, lift);
					LogLiftFrames(a_ref, nodeBefore.z, nodeAfter.z, beforeZ, afterZ, rise);

					contactPoint.z += rise;
					groundZ += rise;
					LogObjectLifted(a_ref, rise, objectHeight, lift);
				}

				// The single place the move is written through.
				//
				// There is no motion type left to hand back - the write no
				// longer switches one - so what this exit does is the
				// placement itself: `a_warp = false`, which writes the node
				// through without warping the body to it.  `true` would put
				// the body where the node is and leave the solver to integrate
				// it against nothing, which is the cycle again; `false` leaves
				// the body where the solver last had it, resting on what it
				// was resting on, so gravity has nothing to close and the next
				// scan has nothing to raise.
				//
				// Asked of `LiftMustFreeBody` rather than repeated at each
				// exit, so a branch added later cannot skip the write-through
				// by simply not repeating the call.
				if (MeshShape::LiftMustFreeBody(action)) {
					if (a_ref->Get3D()) {
						a_ref->Update3DPosition(false);
						a_ref->AddChange(RE::TESObjectREFR::ChangeFlags::kHavokMoved);
					}
				}
			}

			// The state the body was left in, read after the restore above.
			//
			// The physics line is written before the restore and so cannot
			// say whether the restore took; this is the reading that can.
			// `shapes` is a fixed array, and a null collidable is one the
			// line declines to print rather than one it dereferences.
			LogLiftRestored(shapes[0].collidable, a_ref);

			const size_t take = shaft ? 1 : std::min(shapeCount, a_budget);
			for (size_t i = 0; i < take; ++i) {
				const auto& shape = shapes[i];

				Clipmap::Stamp stamp{};
				stamp.snow = surface == Surfaces::Type::kSnow;
				if (shaft) {
					// The mark goes where the shaft met something, not where
					// the shaft currently is - the whole point of resolving a
					// contact.  Taking the bound centre here is what put the
					// hole behind the player while the arrow was in front.
					stamp.x = contactPoint.x;
					stamp.y = contactPoint.y;
				} else {
					stamp.x = shape.centre.x;
					stamp.y = shape.centre.y;
				}

				stamp.motionX = a_motionX;
				stamp.motionY = a_motionY;

				stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);
				stamp.decay = std::clamp(
					Settings::stampDecayPerSecond * response.decayScale * Weather::DecayScale(),
					0.0f, 0.9999f);

				// The object's own half thickness in world units, set by the
				// mesh arm below when a reading is available.  Zero means
				// "not measured", and the depth ceiling is skipped rather than
				// applied to a hull the mesh did not confirm.  Declared out
				// here because the log line below needs it after the branch.
				float halfThinWorld = 0.0f;
				// Set to the ceiling only when the ceiling actually cut the
				// mark down, so the log can name it.
				float depthCeiling = 0.0f;

				// How much snow is stacked over the object's own underside.
				//
				// `drop` is the object's lowest point measured against the
				// snow surface, so its negative is the depth of snow the
				// object is under, and a mark that deep ends level with the
				// object's own bottom.  Bounded by the blanket, because a
				// mark cannot remove snow that is not there: an object that
				// has fallen through the world reads a drop of hundreds and
				// must not be answered with a shaft to match.
				//
				// It is worked out here rather than inside the mark's own
				// branch because the reporter below needs it too, and a
				// second copy of this arithmetic is a second answer waiting
				// to disagree with the first.
				//
				// Named for the snow and not `reach`, which this function
				// already uses for a shape's own vertical extent.
				const float snowOver =
					drop < 0.0f ? std::min(-drop, std::max(lift, 0.0f)) : 0.0f;

				if (shaft) {
					// Pin-prick: a fixed, small radius instead of the shaft's
					// own bound, and no rim at all, so nothing is thrown up
					// around it.  This is the "small hole, no snow" case.
					stamp.radius = Settings::arrowStampRadius *
						std::max(response.radiusScale, 0.0f);
					stamp.depth = std::clamp(
						Settings::stampDepth * Settings::arrowStampDepthScale *
							response.depthScale * Weather::DepthScale(),
						0.0f, 64.0f);
					stamp.rim = 0.0f;
				} else {
					stamp.radius =
						shape.radius * Settings::objectStampRadiusScale * response.radiusScale;

					// The depth an object sinks by is its own thickness, not
					// an estimate made from how big it is.
					//
					// `bulk` was radius over objectFullSizeRadius: a number
					// that says how large the object is and nothing at all
					// about how far it lies below its own centre line.  A
					// helmet and a plank of the same bound radius sank by the
					// same amount, and the amount was a property of the INI
					// rather than of the thing in the snow.  The mesh knows
					// the honest answer - it is how thick the object is where
					// it is lying.
					float bulk = std::clamp(
						shape.radius / std::max(Settings::objectFullSizeRadius, 1.0f), 0.0f, 1.0f);

					// The shape, when the object's own mesh could be read.
					//
					// Only the first bound has a mesh behind it - the
					// measurement follows one collidable, and an object with
					// several meshes reports the union box rather than one
					// box per mesh - so the later bounds keep the round mark
					// they have always had rather than being given a shape
					// that was not measured for them.
					if (haveMesh && i == 0 && mesh.local.halfLength > 0.0f) {
						// The bands are in the mesh's own units and the mark
						// is drawn in world units, so the mesh's own scale is
						// the ratio between the two half lengths - the
						// transform's scale without having to ask the
						// transform.
						const float meshToWorld =
							mesh.world.halfLength / mesh.local.halfLength;

						// The object's own long axis, which is the principal
						// direction of its vertices rather than the longest
						// side of a box - so a helmet's rim and a curved
						// blade both aim where the object actually points.
						stamp.forwardX = mesh.world.ax;
						stamp.forwardY = mesh.world.ay;

						// The mark is the object's own footprint in the snow,
						// so its two half axes come from the object and not
						// from the hull's single radius.
						//
						// The long axis is the mesh's own half length, but it
						// is clamped to the hull's radius times the same
						// ceiling the radius scale allows.  A mesh's half
						// length and a hull's radius are two different
						// measurements of the same object - one along its
						// principal axis and one around its widest cross
						// section - and for a staff the first is many times
						// the second.  Taking the mesh value unclamped would
						// stretch every long object's mark by that ratio in
						// one step, which is a shape change no one asked for;
						// the clamp keeps the improvement inside the size the
						// object was already stamping at.
						const float hullRadius =
							shape.radius * Settings::objectStampRadiusScale * response.radiusScale;

						stamp.radius = std::clamp(mesh.world.halfLength,
							std::max(0.25f * hullRadius, Settings::objectStampMinRadius),
							Settings::objectStampMaxRadius);

						// The short axis is the width across the part of the
						// object that is actually in the snow, not across its
						// widest point.  A helmet's crown is wider than its
						// rim, and the rim is what is touching.
						const float span = std::clamp(mesh.world.halfLength /
														  std::max(stamp.radius, 1e-3f),
							0.0f, 1.0f);
						const float radial = MeshShape::WidthOver(mesh.local, -span, span) *
							meshToWorld;

						stamp.halfWidth = std::max(radial, 0.05f);

						// The honest sink, as a fraction of the mark's own
						// extent: how thick the object is, over how long it
						// is drawn.
						//
						// This is a different quantity from the hull estimate
						// it replaces - that one was the object's size against
						// a nominal size, which says how big it is and nothing
						// about how far it lies below its own centre line.
						// Both are "some property of the object" by design,
						// because ObjectStampDepthScale multiplies it, so the
						// two are interchangeable as a scale and not as a
						// measurement.  The mesh value is the one that makes a
						// helmet and a plank of the same size sink differently.
						if (mesh.local.halfThin > 0.0f) {
							bulk = std::clamp(
								(mesh.local.halfThin * meshToWorld) /
									std::max(stamp.radius, 1.0f),
								0.0f, 1.0f);
						}

						// The same thickness again, kept in world units for
						// the depth ceiling below.  The line above divides it
						// by the mark's half length to make a fraction; this
						// keeps the measurement itself, because a ceiling on
						// how far the snow may sink has to be compared against
						// a distance and not against a ratio.
						halfThinWorld = mesh.local.halfThin * meshToWorld;
					}

					const float ordinary = Settings::stampDepth * Settings::objectStampDepthScale *
						bulk * response.depthScale * Weather::DepthScale();

					stamp.depth = Surfaces::MarkDepth(surface, ordinary, bulk, stamp.x, stamp.y);

					// Two rules, and the deeper of them decides.
					//
					// A mark deeper than the object that made it cannot be
					// seen, so it is capped at a multiple of the object's own
					// half thickness - but that cap is about an object standing
					// in its own dent, and an object under the blanket is not in
					// its dent at all.  `MarkCeiling` takes the deeper of the
					// two answers, and `MarkDepthFor` also lifts a mark that
					// came out shallower than the snow it has to get through:
					// the mark has to reach the object, or the object stays
					// invisible however right the rest of the line looks.
					//
					// The rules live in MeshShape.h so they can be asserted
					// directly; see MarkCeiling for the measurements behind them.
					const float beforeCut = stamp.depth;
					stamp.depth = MeshShape::MarkDepthFor(stamp.depth, halfThinWorld,
						Settings::objectStampMaxDepthPerThickness, snowOver);
					// Only reported when the ceiling is what cut the mark down,
					// so a run where every mark happened to be small enough does
					// not read as one where the ceiling was busy.  The reach
					// wording in `LogObject` takes precedence over this one.
					if (stamp.depth < beforeCut) {
						depthCeiling = MeshShape::MarkCeiling(halfThinWorld,
							Settings::objectStampMaxDepthPerThickness, snowOver);
					}

					// The hole has to be wide enough to uncover the object, not
					// only deep enough to reach it.
					//
					// The depth above is right - a mark that deep ends level
					// with the object's own underside - but the shader spends
					// the mark's disc on its falloff, and bare snow carries
					// shoulder = 0.0, so the sink falls away from the first
					// unit out.  A helmet marked radius = 10.1 into 23.4 units
					// of snow, at 9.8 half width, had 0.2 units of snow moved
					// at its own edge and showed through a circle about four
					// units across.
					//
					// Widening the mark does not move it, darken it or deepen
					// it: the floor is set under the object's whole footprint
					// and the object ends up standing in the hole it made.
					// See BuriedRadius for what the two constants are.
					if (snowOver > 0.0f) {
						const float face = std::max(mesh.world.halfLength, shape.radius);
						const float buried = MeshShape::BuriedRadius(face);
						if (buried > 0.0f) {
							stamp.radius = std::clamp(std::max(stamp.radius, buried),
								Settings::objectStampMinRadius,
								Settings::objectStampMaxRadius);
							stamp.shoulder = std::max(
								stamp.shoulder, MeshShape::BuriedShoulder());
						}
					}

					if (Settings::objectStampMaxDepthPerThickness > 0.0f &&
						halfThinWorld > 0.0f) {
						// The rim is scaled from the depth it belongs to, so
						// it is taken down with it - a rim built for a hole
						// three times this size would stand as a wall around
						// an object that is barely in the snow.
						stamp.rim = Surfaces::RimHeight(
							MeshShape::CapDepthByThickness(ordinary, halfThinWorld,
								Settings::objectStampMaxDepthPerThickness),
							response.rimScale);
					} else {
						stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);
					}
				}

				if (i == 0) {
					LogObject(a_ref, surface, stamp, drop, depthCeiling, snowOver);
				}
				a_out.push_back(stamp);

				// Record that this shaft has now been marked, and where.
				// The position taken is the object's own, not the contact
				// point: the gate in this function compares against
				// GetPosition(), so a mark stored as a contact point would
				// compare two different frames and never match.  The gate
				// asks "has this object come somewhere new", and that is a
				// question about where the object is.
				if (shaft) {
					auto& entry = g_motion[a_ref->GetFormID()];
					entry.markX = a_ref->GetPosition().x;
					entry.markY = a_ref->GetPosition().y;
					entry.marked = true;
				}
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

			// Only the three fields this function owns are written.  Assigning
			// a whole Tracked{} here would read the same and be wrong: this
			// runs every frame, before StampsFor is asked, and a fresh entry
			// carries marked = false - so the mark StampsFor laid on the
			// previous pass would be erased before the gate that reads it ran,
			// and the gate would never fire.
			auto& entry = g_motion[a_form];
			entry.x = a_position.x;
			entry.y = a_position.y;
			entry.frame = g_frame;
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

		// Entries are dropped once they stop being updated - but a marked
		// shaft that has come to rest is exactly that: its frame stops
		// advancing because TrackMotion only runs for live candidates, and
		// evicting it would forget the mark, so the next pass would lay a
		// second one on top of the first.  A spent arrow lies in the snow for
		// the rest of the session, so the whole point is that its entry
		// outlives its motion.
		//
		// Only unmarked entries expire.  A marked one is kept until the
		// object itself goes away and the reference handle stops resolving.
		for (auto it = g_motion.begin(); it != g_motion.end();) {
			it = (!it->second.marked && it->second.frame + 120 < g_frame) ?
				g_motion.erase(it) :
				std::next(it);
		}
	}

	void NoteContact(RE::Projectile* a_projectile, const RE::NiPoint3& a_position)
	{
		if (!a_projectile || !std::isfinite(a_position.x) || !std::isfinite(a_position.y) ||
			!std::isfinite(a_position.z)) {
			return;
		}

		const std::scoped_lock lock(g_lock);
		g_contacts[g_nextContact++ % g_contacts.size()] = {
			a_projectile->GetFormID(), a_position, std::chrono::steady_clock::now(), true
		};
	}

	void PruneContacts()
	{
		const auto now = std::chrono::steady_clock::now();
		const std::scoped_lock lock(g_lock);
		for (auto& record : g_contacts) {
			if (record.live && now - record.time > std::chrono::seconds(1)) {
				record.live = false;
			}
		}
	}

	size_t ContactCount()
	{
		const std::scoped_lock lock(g_lock);
		size_t live = 0;
		for (const auto& record : g_contacts) {
			live += record.live ? 1 : 0;
		}
		return live;
	}

	void Reset()
	{
		HeatSources::Reset();

		const std::scoped_lock lock(g_lock);
		g_candidates.clear();
		g_motion.clear();
		g_reported.clear();
		g_reachReported.clear();
		g_buriedAt = 0;
		g_buriedLines = 0;
		g_contactLines = 0;
		g_heldLines = 0;
		g_contacts = {};
		g_nextContact = 0;
		g_timer = 0.0f;
		g_haveSet = false;
	}
}
