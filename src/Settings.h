// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

namespace Settings
{

	inline bool enableTessellation{ true };

	inline std::string tessellationWinding{ "cw" };

	inline bool logGeneratedShaders{ false };

	inline bool logTerrainBlendDraws{ false };

	inline bool terrainBlendingCompatibility{ true };
	inline bool terrainBlendingCullBackfaces{ true };

	inline bool enableDepthPass{ true };

	inline float tessellationMaxFactor{ 64.0f };

	inline float tessellationTargetSpacing{ 2.0f };

	inline float debugWorldZOffset{ 0.0f };

	inline float debugWaveAmplitude{ 0.0f };

	inline bool useClipmap{ true };

	inline uint32_t clipmapLevels{ 2 };

	inline float clipmapSeedSmoothing{ 1.0f };

	inline bool enableTessellationBounds{ true };

	inline float tessellationBoundDepth{ 0.25f };

	inline float stampRadiusScale{ 1.0f };

	inline float stampFootReach{ 48.0f };

	inline float stampGroundClearance{ 0.1f };

	inline float stampDepth{ 6.0f };

	inline float stampDecayPerSecond{ 0.99f };

	inline bool enableSnowRaise{ true };

	inline float snowRaiseHeight{ 35.0f };

	inline bool snowRaiseWeather{ false };

	inline float snowRaiseDistance{ 7936.0f };
	inline float snowRaiseFadeBand{ 4000.0f };

	inline int snowCoverageBudget{ 2048 };

	inline float snowTrenchDepth{ 0.5f };

	inline float snowTrenchCoverage{ 0.95f };

	inline bool snowGroundFloor{ true };

	inline float snowGroundBite{ 0.0f };

	inline int snowSeamTexels{ 1 };

	inline bool enableShelter{ true };

	inline int shelterBudget{ 48 };

	inline int shelterRefresh{ 16 };

	inline float shelterClearance{ 48.0f };

	inline float shelterHeight{ 1024.0f };

	inline int shelterSeamTexels{ 1 };

	inline bool shelterMeshCap{ true };

	inline float shelterFloorTolerance{ 8.0f };

	inline bool enableStaticProbe{ false };

	inline float staticProbeOffset{ 0.0f };

	inline bool logStaticProbe{ true };

	inline int staticProbeMinTriangles{ 2000 };

	inline bool enableMeshRaise{ false };

	inline float meshRaiseHeight{ 0.0f };

	inline float meshRaiseBand{ 0.5f };

	inline std::string meshSnowKeywords{ "snow,ice,frost,icicle" };

	inline bool meshRaiseAnyMato{ false };

	inline std::string meshSnowMatoIds{ "" };

	inline bool debugMeshRaiseFlat{ false };

	inline bool logMeshRaise{ false };

	inline bool logSnowCoverage{ false };

	inline bool enableWeather{ false };

	inline float weatherSoftenHours{ 1.0f };
	inline float weatherFirmHours{ 1.0f };

	inline float weatherSnowSoften{ 1.0f };
	inline float weatherRainSoften{ 8.0f };

	inline float weatherCloudyFirmScale{ 0.25f };

	inline float weatherSoftDepthScale{ 16.0f };
	inline float weatherFirmDepthScale{ 0.70f };

	inline float weatherSoftDecayScale{ 0.95f };
	inline float weatherFirmDecayScale{ 1.05f };

	inline float weatherFillPerSecond{ 0.06f };

	inline bool logWeather{ false };

	inline float tessellationRaiseSpacing{ 64.0f };

	inline float tessellationBlanketSpacing{ 8.0f };

	inline bool cullDistantDraws{ true };

	inline float cullMargin{ 256.0f };

	inline bool enableSurfaceMaterial{ true };

	inline bool enableSurfaceClassification{ true };

	inline bool logSurfaceClassification{ false };

	inline int debugBlendLayer{ -1 };

	inline float blendFullDepth{ 4.0f };

	inline float blendStrength{ 0.45f };

	inline float snowRevealStrength{ 0.4f };

	inline std::string surfaceSnow{ "snow01,snow02,snowrocks01,dirtsnowpath01,snowcobble01,frozenmarshice01" };
	inline std::string surfaceGrass{ "tundra01,tundra02,fieldgrass01,fieldgrass02,fielddirtgrass01,fallforestgrass01,fallforestleaves01,frozenmarshgrass01,grasssnow01,pineforest03,reachgrass01,reachmoss01,coastbeachgrass01,winterforestleaves" };
	inline std::string surfaceDirt{ "dirt01,dirt02,dirtpath01,fallforestdirt01,cavedirt,blackreachdirt,reachdirt01,minefloordirt01,volcanictundradirt,frozenmarshdirtslopes01,pineforest01,pineforest02,soulcairndirt,soulcairnpath01,glowingforestdirt01,winterforestdirt01,volcanictundramineralpool01" };
	inline std::string surfaceMud{ "rivermud01,coastoceanfloor01,frozenmarshlichen01" };
	inline std::string surfaceSand{ "coastbeach01,coastbeach02" };
	inline std::string surfaceAsh{ "volcanicash" };
	inline std::string surfaceGravel{ "fallforestrocks01,volcanictundragravel01,tundrarocks01,riverbottom01,reachmossyrocks01,volcanictundraminerals01" };
	inline std::string surfaceStone{ "rocks01,riverbededge01,volcanictundrarocks01,soulcairnrock01" };

	inline Surfaces::Response surfaceResponse[static_cast<size_t>(Surfaces::Type::kCount)]{

		  { 0.20f, 1.50f, 0.45f, 2.00f, 0.45f, 0.00f, 1.0f },

		  { 0.80f, 1.00f, 0.10f, 1.05f, 1.45f, 0.00f, 25.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.35f, 1.00f, 0.20f, 0.97f, 0.50f, 0.0f, 1.0f },
		  { 0.00f, 1.00f, 0.30f, 1.00f, 0.00f, 0.0f, 1.0f },
	};

	inline Surfaces::Paint surfacePaint[static_cast<size_t>(Surfaces::Type::kCount)]{
		  { { 0.00f, 0.00f, 0.00f }, 0.00f, 0.00f, 1.0f },
		  { { 0.92f, 0.94f, 0.98f }, 0.90f, 0.95f, 1.0f },
		  { { 0.00f, 0.00f, 0.00f }, 0.00f, 0.00f, 1.0f },
		  { { 0.34f, 0.26f, 0.17f }, 0.25f, 0.00f, 1.0f },
		  { { 0.16f, 0.12f, 0.08f }, 1.00f, 0.00f, 1.0f },
		  { { 0.70f, 0.62f, 0.44f }, 0.45f, 0.10f, 1.6f },
		  { { 0.24f, 0.23f, 0.23f }, 0.70f, 0.35f, 1.0f },
		  { { 0.00f, 0.00f, 0.00f }, 0.00f, 0.00f, 1.0f },
		  { { 0.00f, 0.00f, 0.00f }, 0.00f, 0.00f, 1.0f },
	};

	inline float paintPickupRate{ 0.80f };

	inline float paintBlendRate{ 0.35f };

	inline float paintFullSpeed{ 200.0f };

	inline float paintStandingScale{ 0.25f };

	inline float paintKeepPerSecond{ 0.97f };
	inline float paintKeepRunning{ 0.90f };

	inline float paintReach{ 20.0f };

	inline float paintReachFraction{ 0.22f };

	inline float paintReachPlateau{ 0.3f };

	inline float paintClingAngle{ 55.0f };
	inline float paintClingFeather{ 20.0f };

	inline float paintStrength{ 1.0f };

	inline float paintNoise{ 1.0f };

	inline bool logActorPaint{ false };

	inline bool logStampSurfaces{ false };

	inline bool seasonalTextureSwap{ true };

	inline bool enableStampShapes{ false };

	inline float stampReposeRate{ 0.01f };

	inline float stampRimNoise{ 0.0f };

	inline float stampRimLean{ 1.0f };

	inline float stampChurn{ 0.45f };

	inline float snowStampRimSpan{ 0.7f };
	inline float snowStampReposeRate{ 0.10f };
	inline float snowStampRimNoise{ 0.8f };
	inline float snowStampRimLean{ 1.0f };
	inline float snowStampChurn{ 0.80f };
	inline bool asyncShaderCompilation{ true };

	inline float stampSlopeLimit{ 50.0f };

	inline bool stampFootShape{ true };

	// Long thin collidables carried on the skeleton - a stowed bow, a quiver,
	// a sheathed weapon - are drawn as small circles rather than at foot size.
	// A cylinder's extent radius comes out as its half length
	// (ActorShapes.cpp:274-279), so sizing one as a foot made the player's
	// trail as wide as the bow is long.  The radius now comes from the
	// shape's own thickness, clamped to this range, so nothing long can widen
	// the trail whatever it measures.  Set StampShaftShape to 0 to go back to
	// the old foot-sized disc exactly.
	inline bool stampShaftShape{ true };

	inline float shaftStampMinRadius{ 0.75f };

	inline float shaftStampMaxRadius{ 4.0f };

	// A shaft's mark is cut at radius/footRadius of a foot's depth, so a small
	// disc does not punch a pinhole.  The default is the radius ActorShapes
	// reports for a boot (18.69), the figure the bow was being compared
	// against.  Only affects shapes the shaft rule accepts.
	inline float shaftStampFootRadius{ 18.69f };

	// Whether a carried weapon's mark runs along the weapon or stays a dot,
	// and how much of the weapon it draws.
	//
	// A disc was the safe answer and it is the wrong one: a rod held upright
	// and a bow slung across the back have nothing in common on the ground,
	// and drawing both as the same 2-unit circle made the rod's furrow land
	// in the same place as the bow's.  The axis the shape actually lies along
	// is available - ActorShapes::GetLongAxis reads the body's own rotation -
	// and using it is the difference between a mark that reads as the weapon
	// and one that reads as a dot the game happened to draw near it.
	//
	// The length is a fraction of the shape's measured half length, so a long
	// weapon draws a long line and a short one a short line.  1.0 is full
	// length; the default is a little under, because the extent walk measures
	// the collision hull rather than the mesh and reads slightly long.
	inline bool stampShaftLine{ true };

	inline float shaftLineLengthScale{ 0.85f };

	// Half width of that line, in world units before the surface's own radius
	// scale.  This replaces the thickness-derived radius, so it is the only
	// thing setting how heavy a carried weapon's trail looks.  1.1 is a little
	// wider than the 2.02-unit bow thickness the disc used to draw at, which
	// is wanted: a line of the same width reads thinner than a disc because
	// its ends taper away.
	inline float shaftLineHalfWidth{ 1.1f };

	// The shortest half length that is still worth drawing as a line.
	//
	// The line path used to fall through to the disc whenever the drawn half
	// length was not greater than ShaftStampMaxRadius, which is the *disc's*
	// clamp ceiling - a number chosen for how fat a foot's mark may be, read
	// here as how short a furrow may be.  The two are unrelated, and the log
	// shows the cost: a bow with a real crossing under it (`span 0.8 deep
	// 0.79`) was drawn as a 4-unit circle rather than a line, so a genuine
	// contact left a dot.
	//
	// The concern behind that gate is real and is kept: a line much shorter
	// than it is wide reads as a dot with a direction, which is worse than
	// the dot.  The test is therefore against the line's own width, which is
	// the actual comparison - a half length of 2 against a half width of 1.1
	// is still a shape with a clear long axis.  1.6 is a little under one and
	// a half times the default half width, so the default look is unchanged
	// and a thinner weapon scales with it rather than being cut off.
	inline float shaftLineMinLength{ 1.6f };

	// Cap on the drawn half length, so a weapon measured unusually long cannot
	// lay a furrow across the whole clipmap.  40 units is two thirds of the
	// 57.7-unit bow and still several times any reasonable rod.
	inline float shaftLineMaxLength{ 40.0f };

	// How much longer than the buried stretch the trail is drawn.
	//
	// The walk measures only the part of the object that is *strictly* under
	// the surface, and the log shows how short that is in practice: across a
	// full session of a carried bow the measured spans run 0.8 to 8.0 units
	// on a 67.9-unit weapon, median 4.6.  Drawn at one to one those are
	// stubs, which is the report this answers - the bow reads as touching and
	// the trail is barely there.
	//
	// The honest reason to draw longer than the measurement is that the
	// measurement is a lower bound on the footprint, not the footprint.  The
	// walk only counts samples that ended up below the land height, so a limb
	// resting *on* the surface contributes nothing, and the sampling is
	// discrete: the two samples either side of the crossing are up to half a
	// step apart.  A body pressed into snow disturbs a little of it either
	// side of where it actually sinks, which is the same reason a footprint
	// is longer than the sole that made it.
	//
	// This is a scale and not an offset because the error it corrects is
	// proportional to the stretch: a bow barely dipped in touches over a
	// short arc and a bow laid in the snow touches over a long one, and both
	// should grow by the same fraction.  Set it to 1.0 to draw exactly the
	// measured stretch, with no widening at all.
	inline float shaftSpanLengthScale{ 2.2f };

	// How the drawn line's width is taken from the weapon's own measured
	// thickness, rather than from ShaftLineHalfWidth.
	//
	// The extent walk reports a half thickness per object, and using it is what
	// makes a bow, a rod and a shield draw marks of their own weight instead of
	// one constant.  The scale is applied first so a single number can widen or
	// narrow every weapon at once, and the clamp afterwards guards against a
	// collision hull that is deliberately oversized for gameplay widening the
	// trail - it is a floor and a ceiling, not a tuning range.
	//
	// Setting the scale to 0 collapses the width to ShaftLineMinWidth and
	// reproduces the old constant-drawn look closely enough to compare against.
	inline float shaftLineWidthScale{ 1.0f };

	// Floor of the drawn half width.  Matches the old constant so the two can
	// be told apart at a glance in the log.
	inline float shaftLineMinWidth{ 1.1f };

	// Ceiling of the drawn half width.  6 units is about a third of a foot's
	// 18.69-unit radius, which is as wide as a carried object can read without
	// looking like a boot.
	inline float shaftLineMaxWidth{ 6.0f };

	// How far a carried weapon's lowest point may be above the ground and
	// still leave a mark.
	//
	// This is the gate the whole carried-weapon problem turns on, and it is
	// separate from StampGroundClearance because the two measure different
	// things.  A foot is planted by construction, so the only question
	// StampGroundClearance has to answer is whether it is a hair off the
	// ground; 0.5 units is right for that.  A weapon rides the skeleton and
	// swings with the gait, so its lowest point travels vertically by more
	// than ten units every stride.
	//
	// The log measured it: over 1224 frames the bow's bound bottom ranged
	// -13.52 to +12.41 and was above ground on 919 of them - 75% of the time.
	// With the foot threshold in force 0.5 units rejects almost none of that,
	// so the weapon was stamped 41 times a second at a point that was usually
	// in midair.  Its bound centre also sits 17 to 31 units out to the
	// character's right, which is why the mark appeared beside the trail
	// rather than in it.
	//
	// 2 units means the weapon must be genuinely down against the surface.
	// It is deliberately a small absolute number rather than a fraction of
	// the weapon, because what is being asked is about the surface, not the
	// weapon: anything lower than this is lying on the snow.
	inline float shaftGroundClearance{ 2.0f };

	// Whether a shaft's mark is placed at the point of the weapon that is
	// nearest the ground, rather than at its bound centre.
	//
	// This is what makes the mark follow the step.  A bow's bound centre is
	// a single point rigid to the body, so stamping there produced the same
	// furrow whatever the gait - the walking, running and sprinting poses all
	// marked one place, which is the "it is all the same, so it feels wrong"
	// the change answers.  The weapon's *lowest* point is the part that is
	// actually in the snow, and that part does move with the stride: it is
	// where the weapon is, not where its enclosing box happens to be
	// centred.
	inline bool shaftStampAtContact{ true };

	// Half length of the mark laid when a shaft touches at a point.
	//
	// A weapon that is genuinely resting on the snow touches along a line, so
	// the line is still drawn - but it is the *touching part* that is drawn,
	// not the whole weapon on the off chance.  This caps the drawn half
	// length to something that reads as the contact rather than as the
	// weapon's full length, which is what turned a swing into a plank.
	// 12 units is about a quarter of the bow and several times a rod's radius.
	//
	// Set this to 0 to draw the whole measured length instead, capped by
	// ShaftLineMaxLength as before.
	inline float shaftContactLineLength{ 12.0f };

	// Whether a carried weapon's mark is placed and shaped from where the
	// weapon is *now*, rather than from the bound box it was measured in.
	//
	// The numbers this needs were already being read every frame and already
	// move with the animation - ActorShapes measures them off the live
	// hkpRigidBody transform, and the recorded run confirmed it: the bow's
	// bound bottom spanned 25.9 units of height and its long axis turned up
	// to 22.6 degrees in a single frame.  What was missing is that Clipmap
	// used only the bound centre, which is one point rigid to the body, so
	// every pose of the gait marked the same place.
	//
	// With this on, the stamp is placed at the lower end of the measured long
	// axis and turned to lie along it.  A weapon that swings marks where it
	// swings to; a weapon held still marks in one spot.  Set it to 0 to
	// restore the bound-centre placement exactly as it was.
	inline bool shaftFollowPose{ true };

	// Whether the ground gate is taken at the same point the mark is placed.
	//
	// These must agree, or the gate filters one point and the stamp draws at
	// another.  While shaftStampAtContact placed the mark at the measured
	// end, the gate was still asking about the bound's bottom - two different
	// heights for the same object, which is how a weapon could pass the gate
	// and then stamp a point that was still in the air.  Both are now the
	// same derived point whenever this is on.
	inline bool shaftGateAtContact{ true };

	// Whether the mark's length comes from how much of the weapon is under the
	// snow, rather than from a length constant.
	//
	// This is the last hand-picked number in the carried-weapon path.  The
	// drawn length used to be ShaftContactLineLength, one value for every
	// weapon and every pose, so a weapon that had driven its tip twenty units
	// under the surface and one that had barely broken it both drew the same
	// bar - which is the "why is a rod and a bow the same" complaint.  With
	// this on, the axis is walked and the land sampled under each step; the
	// buried stretch between the two crossings is what decides the length, the
	// position is the middle of that stretch, and whether the weapon is
	// touching at all stops being a clearance constant.
	//
	// This is also the strictest gate in the file, and deliberately so: a bow
	// carried on the back or a sword held at guard is above the snow along its
	// whole length, and drawing nothing is the correct answer for it.  That is
	// what "only mark it when it is really touching" means.  ShaftSpanMaxGap
	// is the one knob that relaxes it.
	//
	// Set this to 0 to restore the single-crossing placement, which is kept as
	// the fallback rather than deleted for that reason.
	inline bool shaftSpanFromContact{ true };

	// How far above the snow a carried object may be and still leave a mark.
	//
	// Reading: when no part of the weapon is under the surface, the closest
	// its axis comes to the land is the gap, and this is the largest gap that
	// still marks.  Zero means strictly only on contact; a large value
	// restores the old "it is near enough" behaviour the clearance constants
	// used to give, with the mark placed at the point of closest approach
	// rather than at the bound centre.
	//
	// Two units is around three centimetres and is not a guess at how high a
	// weapon is carried - it is deliberately below the twelve units the
	// recorded bow's lowest point swung through, so that a walking bow stops
	// marking.  The log prints the measured gap, which is the number to set
	// this from.
	inline float shaftSpanMaxGap{ 2.0f };

	// How finely the axis is walked when looking for the buried stretch.
	//
	// The two crossings are bisected after this and the offset is therefore
	// independent of it, so this only sets how reliably a thin buried patch is
	// found in the first place.  Sixteen steps over a 68-unit bow is one
	// sample roughly every eight units, which finds a patch wider than that;
	// raise it for a weapon whose contact patch is short.
	inline int shaftSpanSteps{ 16 };

	// Whether the buried-stretch walk is told how far the object's own lowest
	// point hangs below the centre line it follows.
	//
	// The walk samples the object's centre line, and a line can only cross the
	// ground where the line itself goes.  A bow carried on the back has its
	// limbs hanging well below its middle, so the centre line can stay clear
	// of the snow along its whole length while the object is visibly in it -
	// the recorded case is a bow whose lowest point was ten units under, with
	// no mark at all to show for it.
	//
	// Handing the walk the distance down to the object's lowest point makes it
	// ask the same question the contact gate asks, and the real crossings are
	// found.  Zero restores the centre-line walk exactly, which is the old
	// behaviour.
	//
	// The distance is measured from the mesh where there is one and from the
	// hull otherwise, so this only decides whether it is used.
	inline bool shaftLowOffset{ true };

	// Whether a carried weapon's mark is derived from its own mesh rather than
	// from the collision hull that stands in for it.
	//
	// The hull is a capsule or a box, so a bow and a staff of the same length
	// and thickness produce the same hull and therefore the same mark.  The
	// mesh has the bow's curve in it, and the shape of the mark is what is
	// left to get right: the position was fixed by measuring the hull live
	// where it had only been logged, and the hull cannot do more than that.
	//
	// Reading a mesh means reading raw vertex bytes at a stride, and the
	// layout of those bytes is decided by agreement with the bounding sphere
	// the engine already computed - a layout that disagrees is refused and the
	// hull is used instead, so a wrong guess costs the old behaviour rather
	// than a wrong mark.  The log reports the layout that was accepted.
	//
	// The fit is cached per mesh and the mesh's own shape never changes, so
	// this costs one walk per weapon per session and eight point transforms
	// per frame after that.  It is only reached once the object has already
	// been classified as a carried weapon.
	inline bool stampFromMesh{ true };

	// How much rim a carried weapon raises around its mark.  1.0 is the rim a
	// foot raises.
	//
	// This was 0, and the reason written for it was real: a bow rides the
	// skeleton and swings with the body, so it is stamped many times a second
	// along an arc, and every one of those stamps raised a rim of its own
	// until the run of them banked the snow up into one continuous furrow.
	// Turning the rim off removed the banking.  What it also removed is the
	// whole reason the mark reads as snow being displaced rather than as a
	// line drawn on the snow - and that is the report this answers.  A
	// carried weapon's furrow had a groove at a tenth of a foot's depth and
	// no raised snow anywhere along it, which is a scratch.
	//
	// It can be raised now because the band the rim occupies is taken from
	// the mark's half width in ClipmapUpdateCS.h rather than from its reach.
	// The old band was the stamp's radius, and for a line that is the line's
	// half length, so the bank it raised was as wide as the weapon is long -
	// which is what "one continuous furrow" was describing.  With the band
	// proportional to the mark's own width, the ridge stands beside the
	// groove the way a footprint's does.
	//
	// The value is a fraction of the rim a foot would raise, so 0.1 gives a
	// tenth of it, and 0 turns it off and restores the old behaviour.
	// ObjectStamps.cpp clears the rim outright for a spent arrow (734-744).
	inline float stampShaftRim{ 1.0f };

	// The shallowest a carried weapon's mark may be pressed, as a fraction of
	// what a foot presses.  It is a floor on the depth that ShaftStampFootRadius
	// otherwise derives from the mark's width, and at 1.0 the derivation is
	// inert and a weapon presses as deep as a foot.
	//
	// The derivation exists on the argument that a two-unit circle pressed as
	// deep as an eighteen-unit foot would read as a pinhole.  That argument is
	// right about a circle and wrong about a line, and it also produced a
	// number too small to sit under the rim above: the log has a carried
	// weapon's mark at `depth x0.103`, a tenth of a foot's, beside a rim
	// raised at the foot's full height, because StampRimIndependence makes the
	// rim independent of the mark's own depth.  A ridge twice as tall as the
	// groove it borders reads as snow piled on flat ground, not as a furrow.
	//
	// The mark's width is not the measure of how hard it was pressed - it says
	// how wide the mark is, not how much weight stood on it, and a long groove
	// and a short one of the same width are not pressed equally.  A weapon
	// held against the ground also concentrates its weight on a far smaller
	// area than a boot does, so pressing as deep as a foot is the modest end
	// of what is defensible.
	//
	// Lower it to lighten the furrow without touching its shape; 0 restores
	// the old width-derived depth exactly.
	inline float shaftStampMinDepth{ 1.0f };

	inline float stampFootLength{ 1.3f };

	inline float stampFootAspect{ 0.50f };

	inline float stampFootSeparation{ 0.0f };

	// Align a carried weapon's mark to the footprint's own recipe.
	//
	// On (the default) the mark's half width is taken from its half length the
	// way a footprint's is, floored by the object's own thickness, and its
	// length is capped at a multiple of that width.  Off restores the
	// thickness-derived width and the uncapped length, which is the escape
	// hatch if the aligned look is not wanted.
	inline bool shaftMarkAlignToFoot{ true };

	// The aspect a footprint's own mark is built at - half width as a fraction
	// of half length.  Its default mirrors StampFootAspect so the two widths
	// come out the same by construction rather than by coincidence.
	//
	// This is the knob for "the weapon's groove is too wide".  The mark's
	// width is its own length times this, so the value is reachable by
	// arithmetic: the longest a carried weapon's mark is drawn at is 24 half
	// length, so a target half width of 8.5 is 8.5 / 24 = 0.35.  The default
	// of 0.50 lands on 12.0, which is a footprint's own width and is the
	// "fully aligned" look; the footprint's width is a hard ceiling, so
	// raising this above it changes nothing.
	inline float shaftMarkFootAspect{ 0.50f };

	// The longest a carried weapon's mark may be, as a multiple of its own
	// half width.  A footprint sits at 2.0; the default of 4.0 leaves a long
	// object a visibly longer mark while removing the pathological ratios
	// that stack into parallel teeth.  64 is effectively no cap.
	inline float shaftMarkMaxAspect{ 4.0f };

	// How ragged a carried weapon's rim is, as a fraction of the mark's own
	// half width.
	//
	// The rim's outer edge is an isocontour of the mark's distance field, so
	// without this it is exactly the mark's shape - a smooth ellipse.  A
	// weapon is re-stamped every frame at a walking stride, so a smooth
	// ellipse lays its edge down in evenly spaced ridges and the groove reads
	// as a plank's outline rather than as disturbed snow.  This pushes the
	// edge in and out by this fraction of the width, seeded from the world
	// position so overlapping stamps agree about where the edge is and the
	// roughness does not crawl while the character stands still.
	//
	// Only line marks take it.  A footprint's mark is a rounded oval laid
	// down one at a time and is deliberately left smooth, because that shape
	// is what the weapon was aligned to.  Zero turns it off and restores the
	// smooth edge exactly.
	inline float shaftMarkRimJitter{ 0.35f };

	// The wavelength of the rim-edge noise, in world units.  This is the size
	// of the crumbs: small values read as fine spatter, large ones as a
	// swollen, lobed bank.  Six units is the same field a footprint's own lip
	// noise uses (RimNoise in ClipmapUpdateCS.h), so the weapon's crumbs and
	// a footprint's crumbs come out the same size.
	inline float shaftMarkRimJitterBand{ 6.0f };

	inline bool logStampFeet{ false };

	inline std::string stampShapeKeyword{ "ActorTypeNPC" };

	inline float stampRimHeight{ 0.2f };

	inline float stampRimIndependence{ 1.0f };

	inline float stampRimSpan{ 1.0f };

	inline bool enableObjectStamps{ true };

	inline float objectStampRadiusScale{ 1.0f };

	// Whether a dropped object's mark is derived from its own mesh rather
	// than from its collision hull.
	//
	// A hull is a capsule or a box, so every object of a given size stamped
	// the same circle however it was shaped.  The mesh carries the shape -
	// the long axis, the width across it and the thickness that decides how
	// far it lies below its own centre line - so a helmet, a sword and a
	// wheelbarrow leave three different marks.  Reading it costs one vertex
	// walk per distinct mesh per session, because a mesh's own shape does not
	// change; everything after that is eight transformed corners.
	//
	// Off, or on a mesh that cannot be read, the hull is used exactly as
	// before, so turning this off is a true escape hatch and not a partial
	// one.  See MeshGeometry.h for why a reading can be refused.
	inline bool objectStampFromMesh{ true };

	// The range a mesh-derived mark's long half axis is allowed to fall in,
	// in world units.
	//
	// The floor is two texels - the clipmap's own resolution is 0.75 - so a
	// mark narrower than the grid it is drawn on cannot exist, which is the
	// same rule the rim band's width follows.  The ceiling is four footprint
	// half lengths, so no dropped object can leave a mark larger than the
	// scene's own largest ordinary one however its mesh measures.
	inline float objectStampMinRadius{ 1.5f };

	inline float objectStampMaxRadius{ 48.0f };

	// How deep, as a multiple of the object's own half thickness, a
	// mesh-derived mark is allowed to be.
	//
	// A mark deeper than the object that made it cannot be seen: the snow
	// surface sinks below the object's underside and the object is buried in
	// its own dent.  Measured on a fur helmet, whose collision hull is 4.1
	// units thick: the mesh arm was drawing depth 13.4 under a mark 10.1 wide,
	// so the depression was three times the helmet's thickness and wider than
	// the helmet itself.  What showed was a hole with nothing in it.
	//
	// The ceiling is the object's own measured thickness rather than a number
	// from the INI, so it holds for a ring and for a cart alike, and it moves
	// with ObjectStampDepthScale instead of capping against it - the scale
	// still decides how deep an object sinks relative to others, and this
	// only stops the deepest of them from disappearing.  Two is a mark that
	// reaches twice the object's half thickness: it still reads as something
	// heavy having pressed in, while the object stays proud of its own hole.
	// Set 0 to switch the ceiling off and get the raw scaled depth back.
	inline float objectStampMaxDepthPerThickness{ 2.0f };

	inline float objectStampDepthScale{ 1.0f };

	inline float objectFullSizeRadius{ 1.0f };

	inline float objectContactTolerance{ 48.0f };

	inline float objectSinkLimit{ 40.0f };

	// How far below the snow surface an object may sit before its mark is
	// refused.  The floor is always the blanket's own thickness at that point,
	// so an object lying on the ground under the snow is never mistaken for
	// one clipped through the world.  When this is on, a further ObjectSinkSlack
	// is allowed on top of the blanket.
	inline bool objectSinkFollowsSnow{ true };

	inline float objectSinkSlack{ 16.0f };

	// Lift a dropped object so that its own top sits at the snow surface.
	//
	// The raise displaces the terrain *mesh*; the engine's collision does not
	// know about it, so a dropped object falls through the blanket and stops
	// on the bare terrain under it.  Measured in game: a steel arrow read
	// `drop = -35.9`, which against a land height of about 0 puts its lowest
	// point at -0.9 - on the ground, with the snow surface 35 units over its
	// head.  Anything shorter than the blanket is then buried whole, which is
	// what an object "vanishing when it lands" has actually been.
	//
	// The lift is `snow surface - object's own height`, so the object's top
	// ends up level with the snow and its bottom is its own height below it.
	// The object is only ever moved up, and never by more than the blanket is
	// thick, so gear placed on a rock or standing on a floor is left alone.
	inline bool objectLiftToSnow{ true };

	// The least lift worth applying, in world units.  Below this the object is
	// already where it belongs and moving it would only jitter against the
	// physics engine a fraction of a unit at a time.
	inline float objectLiftSlack{ 0.5f };

	inline float objectStampInterval{ 0.20f };

	inline float arrowStampRadius{ 4.0f };

	inline float arrowStampDepthScale{ 0.6f };

	inline bool contactProbeShafts{ true };

	inline float contactProbeMaxMiss{ 96.0f };

	inline bool logContactSamples{ false };

	inline bool logObjectStamps{ false };

	inline bool enableHeatSources{ true };

	inline std::string heatKeywords{
		"fire,campfire,brazier,firepit,candle,torch,forge,smelt,hearth"
	};

	inline float heatRadius{ 2.5f };
	inline float heatRadiusScale{ 1.0f };
	inline float heatMaxRadius{ 256.0f };

	inline bool enableMagicImpacts{ true };
	inline bool enableShoutImpacts{ true };
	inline bool enableExplosionImpacts{ true };
	inline float magicImpactRadius{ 36.0f };
	inline float magicImpactDepth{ 32.0f };
	inline float shoutImpactRange{ 1200.0f };
	inline float shoutImpactWidth{ 500.0f };
	inline float shoutImpactDepth{ 24.0f };
	inline float explosionImpactRadiusScale{ 0.5f };
	inline float explosionImpactMaxRadius{ 256.0f };
	inline float explosionImpactDepth{ 18.0f };
	// How much of a crater's radius is flat floor, 0..0.9.
	//
	// The shader's falloff, 1 - smoothstep(radius * shoulder, radius, d),
	// spends the whole disc on the drop when shoulder is 0, and the blast
	// radius is large next to its depth, so a fireball came out a 19 degree
	// dish.  A real crater is a floor with a wall: at 0.65 the wall is 44
	// degrees and the floor takes the rest.  Surfaces supply their own
	// shoulder for footprints; this one is only read for explosions.
	inline float explosionImpactShoulder{ 0.65f };
	// How far a crater's rim may wander off a perfect circle, as a fraction
	// of its own radius, 0..0.5.
	//
	// This is not a tuning knob for a shape that already exists - it is the
	// only term that lets the shape exist at all.  A round stamp's outline
	// is length(worldXY - centre), and a length is a circle: reach, depth
	// and shoulder all scale that circle and none can deform it, so a
	// crater stayed round however they were set.  0.25 lets the rim swing a
	// quarter of the radius either way, which is roughly what a thrown
	// mass leaves; 0 keeps the circle.  Only explosions read this.
	inline float explosionImpactRimBulge{ 0.25f };
	// How tall the loose snow thrown up at a crater's lip may be, as a
	// multiple of what a footprint's own rim noise would raise.
	//
	// A footprint is ringed by a band of small crumbly lumps - that is what
	// RimNoise, at a 6 world unit wavelength, draws around every stamp.  A
	// crater had none of it, and read as smooth-cut however its rim was
	// bulged, which is not how snow behaves when something is thrown into
	// it.  The lumps belong on the same part of the crater a footprint's
	// lumps sit on: the transition from the depression out to level ground.
	//
	// This cannot ride the footprint's own channel.  That one is stamp.rim,
	// which the shader gates on being non-zero (ClipmapUpdateCS.h:410) and
	// which MagicImpacts deliberately pins to zero for every blast stroke
	// (MagicImpacts.cpp:564), because a blast's rim is the bowl's own edge
	// and letting the shader spread it would push the crater 2.7x past the
	// radius the pattern asked for.  So a crater needs its own term, and
	// this is it.  0 leaves the lip as it was.
	inline float explosionImpactRimNoise{ 1.2f };
	// How wide that band of thrown snow is, as a fraction of the crater's
	// radius, above the shoulder where the wall becomes lip.
	//
	// The wall's falloff already occupies the last (1 - shoulder) of the
	// radius - 22 world units on a fireball - so the band is measured from
	// there outwards, not from the centre.  0.14 puts the band on the
	// outside of the wall, where a footprint's lumps sit on the slope of
	// its depression, and keeps it clear of the snow bank the ring's heaps
	// build a further 2 units out.  0 disables the band entirely.
	inline float explosionImpactRimNoiseBand{ 0.14f };
	inline float magicImpactRimScale{ 0.1f };
	inline float magicImpactSnowMelt{ 0.3f };
	inline bool logMagicImpacts{ false };

	inline float heatMeltPerSecond{ 0.85f };

	inline float heatShoulder{ 0.20f };

	inline bool  heatFromTorches{ true };
	inline float heatTorchRadius{ 40.0f };

	inline float heatTorchScale{ 0.45f };

	inline bool heatMeltsSnow{ true };

	inline float heatMeltDepthScale{ 0.80f };

	inline bool heatFromExplosions{ true };

	inline bool logHeatSources{ false };

	inline bool logStampShape{ false };

	inline bool enableLiveReload{ true };

	inline float liveReloadInterval{ 0.5f };

	inline bool enableActorPaint{ false };
	inline bool enableBloodDecals{ true };

	inline bool bloodDecalsIgnoreShaderIdentity{ true };
	inline std::string bloodDecalTexturePrefixes{ "blood,decalsblood,bigspatter" };

	inline int debugPaintMask{ 0 };

	inline float debugActorTint{ 0.0f };

	inline float debugActorTintColour[3]{ 1.0f, 0.0f, 0.8f };

	// On by default, and deliberately so.  This switch used to be off by
	// default and to silence the whole log, which meant a user who saw a mark
	// in the wrong place had no way to find out why - the mod wrote nothing
	// after loading.  Turning it down now only drops the noisy per-draw
	// lines; the one-per-event diagnostics still come through.
	inline bool enableLogging{ true };

	inline bool logDraws{ false };

	inline bool logActorDraws{ false };

	inline bool routeOnlyPlayerCamera{ true };

	inline bool recomputeNormals{ true };

	inline float normalGradientEpsilon{ 0.75f };

	inline float debugWaveLength{ 256.0f };

	inline int debugFieldColour{ 0 };

	inline bool enableProfiler{ false };

	inline float profileInterval{ 5.0f };

	inline bool profileDrawScopes{ false };

	inline bool snowSparkle{ true };

	inline float snowSparkleRate{ 1000.0f };

	inline float snowSparkleFullSpeed{ 350.0f };
	inline float snowSparkleMinSpeed{ 40.0f };

	inline float snowSparkleSize{ 1.0f };

	inline float snowSparkleLife{ 1.0f };

	inline float snowSparkleThrow{ 500.0f };
	inline float snowSparkleRise{ 80.0f };

	inline float snowSparkleInherit{ 1.0f };

	inline float snowSparkleGravity{ 300.0f };
	inline float snowSparkleDrag{ 4.2f };
	inline float snowSparkleSwirl{ 100.0f };

	inline float snowSparkleBrightness{ 0.5f };
	inline float snowSparkleOpacity{ 0.5f };

	inline float snowSparkleShape{ 1.0f };

	void Load();

	bool PollForChanges(float a_deltaSeconds);

	void ApplyLogLevel();
}
