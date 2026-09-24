# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 NearMidnightNow (NMN).
#
# Byte scan of the *deployed* dll for the strings a build with these changes
# must carry and the ones it must not.  It answers one question - is the file
# in the game folder the file that was just built - and it has to be read that
# way: a green scan says nothing about whether the change is correct, only that
# the build reached disk.  `dtd_build.sh` swallowing an exit code, or a copy
# that silently did nothing, would both show up here and nowhere else.
#
# What this build changed, and therefore what has to be pinned:
#
#   * the build tag;
#   * the width knob actually reaches the mark.  An earlier build wired the
#     width to `LineWidth(thickness, 1.0f, 0.0f, 0.0f)`, whose last line is
#     `return scaled < a_max ? scaled : a_max;` - so a_max = 0 means "return
#     zero", not "no ceiling", and the footprint's half width came out as 0.0.
#     The ceiling then resolved to the bare ShaftLineMaxWidth 6.0 and every
#     mark was drawn 24 x 6.  `ContactPoint::FootprintHalfWidth` carries the
#     same arithmetic by name, and `ContactPoint::LengthDerivedWidth` is what
#     lets ShaftMarkFootAspect move the width at all - with the footprint's
#     half width left in as a floor, every value below 0.506 resolved back to
#     12.15 and the knob was dead.  Both calls are pinned, because the whole
#     point of that failure was arithmetic that no test and no pin could see;
#   * the rim edge of a *line* mark is jittered in world space.  The gate is
#     pinned rather than the arithmetic, because a footprint's mark must keep
#     its smooth edge - see the note below about why the switch itself has
#     to be pinned and not just the body.
#
# Pins that must not be used:
#
#   * anything naming an `inline` function (`ContactReach`, `HeightAboveSnow`,
#     `DropAt`, `HalfLengthFromStretch`, `Allow`, `GateIsBinding`,
#     `WidestLimit`, `TightestLimit`).  Inline definitions are either expanded
#     or dropped, so their names are not reliably in the image at all - and
#     `ContactReach` and `HeightAboveSnow` both measure **0** in this build,
#     which was checked rather than assumed.  A pin on one is a claim that
#     cannot fail, which is worse than no pin.  The arithmetic is asserted in
#     tools/offline/StampSurfaceTest.cpp (CheckContactReach) instead, where it
#     can actually be called.
#   * `groupshared` and `const float span = max(` as equality-1 pins.  The
#     update shader's body is embedded **twice**, and has been in every build
#     measured - across forty-odd archives, not assumed.
#
# Run:  python tools/offline/r54_scan.py
#
# A pin has to be a needle that can only match the thing it names.  A bare
# `t {:+.2f}` reads **2**, and the second match is `highes{t {:+.2f}} world
# units` in the shape census - a different literal that happens to end in the
# same characters.  Appending the NUL terminator pins the literal itself and
# the false match disappears.  A count that is right for the wrong reason is
# the same failure as a probe that cannot say no.
#
# The surface pins are shader text and are kept as they were: the changes since
# did not touch the outline measurement they name, so their counts are still
# the evidence for half of the image; the two shader pins below are the other
# half of the same argument, for the shader text that did change.

import datetime
import hashlib
import os
import sys

DLL = ("E:/EJ/mod/mods/Dynamic Terrain Deformation Skse EJ"
       "/SKSE/plugins/NMN_DeformableTerrain.dll")

# The source the walk's surface and the mark's shape are built in, scanned as
# TEXT rather than as bytes of the image.
#
# This is a second kind of pin, and it exists because the first kind cannot do
# this job.  The dll pins above can only see strings, and no string records an
# addition: a build where the walk's `land + lift` was flipped back to the bare
# `land` carried every string the image was supposed to carry, and the offline
# test - which does not link Clipmap.cpp - stayed green.  A change that passes
# both gates without being in them is exactly the kind of half-change these
# rules exist to catch.
#
# The arithmetic itself is assertable (ContactPoint::SurfaceAt, pinned by
# CheckContactReach in StampSurfaceTest.cpp).  What cannot be asserted is that
# the caller uses it, so that is what this checks: the gate and the walk in
# Clipmap.cpp must reach the surface through the named function, and the
# source must not carry the old bare-land expression anywhere on this path.
#
# A source pin is weaker than a test - it cannot know the value is right - so
# it is written to be exact rather than suggestive: the call must appear, and
# the pattern it replaced must not.
#
# Two files are read now.  Clipmap.cpp carries the weapon-path pins; the
# loose-object contact gate lives in ObjectStamps.cpp, and a pin that cannot
# name its file cannot follow the code it is supposed to be watching.
SRC = "D:/Modding/_dtd_src/src/Clipmap.cpp"
SRC_OBJECT_STAMPS = "D:/Modding/_dtd_src/src/ObjectStamps.cpp"
# The named rules themselves live in a header, and a rule that is only
# pinned at its call sites can be rewritten out from under them - the
# calls stay in the file and every pin stays green.
SRC_MESH_SHAPE = "D:/Modding/_dtd_src/src/MeshShape.h"
SRC_ACTOR_SHAPES = "D:/Modding/_dtd_src/src/ActorShapes.cpp"
# The header of the same walk, read with the comments left in.
#
# One measurement - that the engine's support readings do not carry the shape's
# base radius - has no code that can record it, because the code is the same
# either way once you decide.  What records it is the note saying the obvious
# alternative was tried and refuted, and that note is a comment, so the default
# comment-stripping check reads zero against it however it is written.
SRC_ACTOR_SHAPES_H = "D:/Modding/_dtd_src/src/ActorShapes.h"
# Settings.cpp carries the key names.  A knob whose parser branch is dropped
# still leaves its _use_ site compiling and still leaves every behavioural pin
# green - the code that reads the value runs, it just always reads the
# default.  That is the failure mode this file has already had once
# (C1061 forced the last two keys out of the else-if chain), so the branches
# are pinned here by name.
SRC_SETTINGS = "D:/Modding/_dtd_src/src/Settings.cpp"
# The offline test file is a source artifact too, and one assertion in it is
# the only place a reading that looks plausible from six directions is pinned.
# A test that is deleted leaves every dll pin green - nothing in the image
# records that a test existed - so the test that carries the measured refusal
# is named here.
SRC_STAMP_TEST = "D:/Modding/_dtd_src/tools/offline/StampSurfaceTest.cpp"

# (substring, expected count, what it is evidence of)
SRC_PINS = [
    ("ContactPoint::SurfaceAt(land,", 1,
     "the axis walk's land sampler reaches the surface by name"),
    ("SnowSurface::LiftAt(", 5,
     "every place the weapon path needs the blanket's height has one, in "
     "code: the gate, the mesh gap, the walk, the crossing, and the "
     "end-fallback.  Five, not seven - two more matches in the raw file are "
     "the comments that name this call while explaining it, which is why "
     "comments are stripped above"),
    ("a_z = land;", 0,
     "the bare-land sampler is not back"),
    ("= land + SnowSurface::LiftAt", 0,
     "and neither is the inline expression the name replaced"),
    ("GetLandHeight(RE::NiPoint3{ hit.x, hit.y, hit.z }, landAtHit)",
     1, "the crossing still samples the terrain and adds the blanket through "
        "HeightAboveSnow"),
    ("ContactPoint::FootprintHalfWidth(", 1,
     "the footprint's half width is computed by name, and this is the call "
     "that would have shown the bug: it used to be "
     "`LineWidth(thickness, 1.0f, 0.0f, 0.0f)`, whose `a_max = 0` means "
     "`return 0`, not `no ceiling` - so the half width was 0.0 and the "
     "ceiling resolved to the bare 6.0"),
    ("ContactPoint::LengthDerivedWidth(", 1,
     "and the mark's own width is derived from its length and its aspect, "
     "without the footprint's half width left in as a floor - that floor is "
     "what made ShaftMarkFootAspect a dead knob for every value below 0.506"),
    ("ContactPoint::AspectCappedLength(", 1,
     "the length is capped against that width, which is what stops a long "
     "thin mark from stacking into the rake of parallel teeth"),
    ("ContactPoint::AlignedWidthCeiling(", 1,
     "the ceiling is resolved rather than borrowed: ShaftLineMaxWidth is 6.0 "
     "and a footprint's own half width is 12.15, so the old constant would cap "
     "every aligned mark at half the shape it is aligning to"),
    # Both arms of the alignment branch, and the switch that guards them.
    #
    # A condition replaced by a literal `false` is not caught by anything
    # else: the calls above are still in the file, so their pins stay green,
    # and `Clipmap.cpp` is not linked into the offline test so no assertion
    # can see it either.  Pinning the switch itself and the else-arm is what
    # closes that - a branch that is never taken on the built configuration is
    # not covered by pinning its body.
    ("if (Settings::shaftMarkAlignToFoot) {", 1,
     "the alignment is actually branched on, not switched off with a literal"),
    ("if (false) {", 0,
     "and the switch was not replaced by something that never runs"),
    ("stamp.halfWidth = thicknessWidth;", 1,
     "the thickness width survives as the escape arm, so turning the "
     "alignment off really does restore the thickness shape"),

    # The weapon path asks the same acceptance question the dropped-object
    # path now asks.  Both call it by name so the two cannot drift, and the
    # bare factor written inline is what the name replaced.
    ("MeshShape::MeshTrusted(box, hull)", 1,
     "the weapon path accepts a mesh reading by the shared named rule"),
    ("constexpr float kMeshSanityFactor = MeshShape::kMeshSanityFactor;", 0,
     "the local copy of the factor is gone, so there is one rule and not two"),
    ("box <= kMeshSanityFactor * (hull + 1.0f)", 0,
     "and the inline expression it replaced is not back"),

]

# Pins against ObjectStamps.cpp, the loose-object contact gate.  Checked
# separately because a pin has to be counted in the file it is about; the
# weapon pins above would report a false zero if they were counted here.
SRC_PINS_OBJECT_STAMPS = [
    # A resting object's drop is negative by the blanket's whole thickness, so
    # the sink limit has to be at least that.  The bug was a fixed limit
    # tighter than the blanket: the honest case - a helmet sitting on the
    # ground under 35 units of snow - was refused, and it looked in game like
    # the helmet had fallen through the world and vanished.  These pins name
    # the fix so it cannot be flattened back to a single fixed number, which is
    # an arithmetic change no dll string would record.
    ("const float lift     = SnowSurface::LiftAt(probe.x, probe.y);", 1,
     "the gate measures the blanket at the object's own point, once, by name"),
    ("float sinkLimit = std::max(Settings::objectSinkLimit, lift);", 1,
     "the floor under the sink limit is the blanket's own thickness, so an "
     "object buried to the ground is never read as clipped through it"),
    ("if (Settings::objectSinkFollowsSnow) {", 1,
     "the follow-the-snow arm is really branched on, not switched off"),
    ("sinkLimit = std::max(sinkLimit, lift + Settings::objectSinkSlack);", 1,
     "and it allows a stated slack on top of the blanket, so the escape arm is "
     "not the same expression as the floor"),
    ("if (drop > Settings::objectContactTolerance || drop < -sinkLimit) {", 1,
     "the gate compares against the resolved limit, not a literal"),
    ("LogObjectRefused(a_ref, drop,", 1,
     "a refusal is named in the log - otherwise a rejected object and an "
     "unscanned object are the same silence"),

    # The object's mark is derived from its own mesh, not from its hull.
    #
    # A hull is a capsule or a box, so every object of a given size stamped
    # the same circle; the shape of the thing was never read.  These pins name
    # the mesh route so it cannot be flattened back to the hull, which is a
    # change no arithmetic assertion in the offline test could see: the test
    # cannot link ObjectStamps.cpp at all.
    ("MeshGeometry::Measure(", 1,
     "the object's mark is derived from its own mesh, so a helmet and a "
     "plank of the same size leave different marks"),
    ("if (Settings::objectStampFromMesh && shapes[0].collidable) {", 1,
     "the mesh route is really branched on, and is actually reached - a gate "
     "replaced by `if (false) {` leaves every call below it in the file, so "
     "the only pin that catches it is the generic `if (false)` one, which "
     "cannot say which branch went dead"),
    ("bound.collidable = a_object;", 1,
     "each bound carries the collidable it was measured from, so the mesh "
     "behind it can be read without a second walk of the scene graph - and "
     "so the gate above cannot pass on a bound that has no mesh"),
    ("a_out[0].collidable", 0,
     "the world-bound fallback never sets a collidable, and it must not: it "
     "has none to point at, and a stale one would make the mesh route "
     "measure whatever collidable was collected last"),
    ("a_out[0].centre = bound.center;", 1,
     "and that fallback is written field by field rather than as a "
     "positional braced list.  This struct gained a member between `radius` "
     "and `length`, and a two-value list then depends on which members the "
     "compiler chooses to skip - measured on this toolchain it lands "
     "correctly, but that is not a guarantee to build a fallback on"),
    ("MeshShape::MeshTrusted(box, hull)", 1,
     "the mesh reading is accepted by the same named rule the weapon path "
     "uses, not by a second copy of the factor that could drift from it"),
    ("stamp.halfWidth = std::max(radial, 0.05f);", 1,
     "a non-zero half width is what takes the stamp out of the circular "
     "branch - without this line every object still draws a circle"),
    ("stamp.forwardX = mesh.world.ax;", 1,
     "the long axis of the object's own vertex cloud aims the mark, so a "
     "curved or turned object points where it actually points"),
    ("stamp.forwardY = mesh.world.ay;", 1,
     "both components of that axis, or the direction is half a direction"),
    ("const float radial = MeshShape::WidthOver(mesh.local, -span, span)", 1,
     "the width across the mark is the mesh's own radial half width over the "
     "stretch the mark actually spans, not over the whole object: a helmet's "
     "crown is wider than its rim and the rim is what is touching"),
    # Both arms of the mesh branch.  Replacing a condition with a literal is
    # how a branch goes dead without any call leaving the file; the pins on
    # the body stay green while nothing runs, which is why the switch and the
    # else-arm are pinned as well as the body.
    ("if (haveMesh && i == 0 && mesh.local.halfLength > 0.0f) {", 1,
     "the mesh arm is really branched on, and is limited to the bound the "
     "mesh was measured for"),
    ("if (false) {", 0,
     "and it was not replaced by something that never runs"),
    ("bulk = std::clamp(", 2,
     "the depth is recomputed from the mesh when there is one - the "
     "hull-radius estimate it replaces said how big the object is and "
     "nothing about how far it lies below its own centre line.  Two, not "
     "one: the hull estimate stays as the value the mesh arm overwrites, "
     "which is also what keeps the later bounds and the no-mesh case on the "
     "old depth"),
    ("if (mesh.local.halfThin > 0.0f) {", 1,
     "that depth comes from the object's own thickness, guarded so a "
     "missing measurement cannot zero the depth"),
    ("stamp.radius = std::clamp(mesh.world.halfLength,", 1,
     "the long half axis of the mark is the mesh's own half length, clamped "
     "rather than taken raw: a staff's half length is many times its hull "
     "radius, and using it unclamped would stretch every long object's mark "
     "by that ratio in one step"),
    ("Settings::objectStampMaxRadius)", 2,
     "the ceiling on that axis is a named setting with a floor and a ceiling, "
     "so no single mesh reading can draw an unbounded mark.  Two call sites: "
     "the half length a mesh reading is clamped to, and the width a buried "
     "mark is widened to - both routes have to respect the same ceiling, or "
     "one of them can draw a mark the other cannot"),
    ("shape={}", 4,
     "all four stamp lines - the plain one, the one that also reports a capped "
     "depth, the one that reports a mark deepened to reach the object, and the "
     "buried reading - say which route drew the mark, so a run where every "
     "object fell back to its hull can be told from one where the mesh route "
     "was never reached.  Four, not one: each variant is its own format "
     "string, and a pin that expected one would break the moment either "
     "variant was logged rather than the moment the reporting was removed"),
    ("LogObjectMeshRefused(a_ref,", 2,
     "a refused mesh reading is named, on both the oversized-box arm and the "
     "unmeasurable arm - otherwise a hull-shaped mark and a mesh route that "
     "was never added are the same silence"),
    ("void LogObjectMeshRefused(RE::TESObjectREFR* a_ref, const char* a_why)", 1,
     "the mesh refusal has one reporter, so the two arms cannot drift apart"),
    ("LogBudget::Allow(g_objectMeshLineAt, NowMs(), kObjectMeshLineGapMs)", 1,
     "that reporter is rate limited by a time gap rather than given a "
     "one-shot budget - the shaft lines spent all of theirs in five seconds "
     "once and then went silent for the rest of the run"),

    # The depth of an object's mark, and what it is measured against.
    #
    # A fur helmet left radius=10.1 depth=13.4 while its collision was
    # length=9.7 thickness=4.1 radius=12.6, so the dent read as three times the
    # helmet's own thickness, and every mark was capped at a multiple of that
    # thickness so the object would stay proud of its own dent.
    #
    # That cap answers the wrong question.  The engine's collision is the bare
    # terrain and the raised snow is a mesh it knows nothing about, so a
    # dropped object is not standing in its dent at all - it is on the ground
    # under the blanket.  Measured in game: a steel arrow reads `drop = -35.9`
    # against a 35-unit blanket and a fur helmet -28.75, both on the ground
    # with the snow surface over their heads.  A cap of 11.2 units under 35 of
    # snow leaves the object invisible and draws the dent exactly where it
    # cannot be seen: the dent was never too deep, it was far too shallow, so
    # "what showed was a hole with nothing in it" was the object sitting below
    # the hole's own floor.
    #
    # So the ceiling is now the deeper of the object's own thickness and the
    # snow over the object's own underside, and the mark is also *raised* to
    # that reach - the arrow's own scaled depth was 2.5.
    #
    # These are comparisons between lengths, so no arithmetic assertion can
    # hold them: the offline test strips this file, and a change from one rule
    # to another is a change no dll string records.  Both branches of the rule
    # and both of its log wordings are pinned here.
    ("MeshShape::MarkDepthFor(stamp.depth, halfThinWorld,", 1,
     "the depth the mark is drawn with is computed by the named rule and not "
     "by the ceiling alone - the thickness ceiling is still in the rule, and "
     "so is the reach that gets the mark down to an object the blanket has "
     "swallowed"),
    ("Settings::objectStampMaxDepthPerThickness, snowOver);", 2,
     "and the snow over the object is fed to both the ceiling and the depth "
     "at the site.  Without this the rules above are correct and unused: a "
     "reach worked out and then not passed is a mark that still buries the "
     "object, with every string in this group still present"),
    ("MeshShape::CapDepthByThickness(ordinary,", 1,
     "and the rim is capped from the same uncapped depth, or a lip built for "
     "a dent three times too deep reads as a wall around a shallow hole"),
    ("float halfThinWorld = 0.0f;", 1,
     "the object's half thickness is kept in world units - a ceiling on how "
     "far snow may sink is compared against a distance and not a ratio"),
    ("halfThinWorld = mesh.local.halfThin * meshToWorld;", 1,
     "and it is filled from the mesh's own thinnest radial half width, "
     "converted to world units by the same scale the rest of the mesh uses"),
    ("Settings::objectStampMaxDepthPerThickness > 0.0f", 1,
     "the cap is switched on by name rather than by a literal, and is "
     "checked before halfThinWorld is consulted so the escape value zero "
     "really does restore the old depth"),
    ("depth capped to {:.1f} by the object's own thickness", 1,
     "a capped mark says so in the log - a capped depth and a depth that "
     "was computed that way are otherwise the same line, and there would be "
     "no way to tell whether the fix ran"),
    ("shoulder={:.2f} shape={} mark reaches the object, {:.1f} of snow ", 1,
     "a mark that had to be deepened to get down to the object says so, with "
     "the depth of snow it had to cross and the shoulder of the floor it was "
     "given.  The needle is the literal as the source splits it, not the "
     "joined form the dll holds: the two halves are separate literals around "
     "a newline, so the joined string does not occur in this file at all and "
     "a pin against it would read zero however right the source was.  The "
     "joined form is pinned in PINS, against the dll, where it does occur"),
    ("LogObject(a_ref, surface, stamp, drop, depthCeiling, snowOver);", 1,
     "the ceiling and the snow over the object are both passed to the "
     "reporter, so neither branch that prints them can be dropped silently"),
    ("if (a_reach > 0.0f) {", 1,
     "and the reach branch is guarded by the reach itself.  The strings below "
     "would all still be in the file if the guard were made false - a dead "
     "branch passes every string pin there is - and the run would then be "
     "missing the one line that says the mark got down to the object"),
    ("drop < 0.0f ? std::min(-drop, std::max(lift, 0.0f)) : 0.0f;", 1,
     "the snow over the object is the depth of blanket above its own "
     "underside, read off the same `drop` the gate used, and bounded by the "
     "blanket because a mark cannot remove snow that is not there - an "
     "object that has fallen through the world reads a drop of hundreds"),
    ("std::clamp(stamp.depth", 0,
     "the bounds are not written inline a second time; one named rule, one "
     "readable site"),

    # Which half extent measures how far down an object reaches.
    #
    # This is the case the offline test could not cover.  Putting the old
    # `centre.z - radius` back keeps every behavioural assertion green,
    # because `CheckVerticalReach` recomputes the arithmetic inside the test
    # rather than reading this file - and this file is not linked into that
    # test at all.  So the assertion checks its own copy of the arithmetic and
    # nothing else.  These pins are the counter: they read the line that
    # actually runs.
    ("bound.vertical = extent.vertical;", 1,
     "the vertical reach is carried from the shape reading onto the bound, "
     "so the drop test has a real measurement to subtract"),
    ("shapes[0].vertical > 0.0f ? shapes[0].vertical : shapes[0].radius", 1,
     "the drop test subtracts the shape's own vertical reach"), 
    ("shapes[i].vertical > 0.0f ? shapes[i].vertical :", 1,
     "and every later bound is judged the same way, or the lowest point of a "
     "multi-collidable object is taken from one rule and its depth from another"),
    ("float                 groundZ = shapes[0].centre.z - shapes[0].radius;", 0,
     "the diagonal is not used as a vertical half height again.  A box's "
     "radius is sqrt(hx^2+hy^2+hz^2); measured on a fur helmet that is 12.62 "
     "against a true vertical half extent of 4.16, so the old line placed the "
     "object's lowest point 8.47 units below where it was and the mark landed "
     "under the object instead of at its feet"),
    ("shapes[i].centre.z - shapes[i].radius", 0,
     "and neither is it on the later bounds"),

    # The object is raised onto the snow.
    #
    # The raise is the whole fix for "the item is buried", and it is a call to
    # a named rule plus a move - neither of which any arithmetic assertion can
    # see, because this file is not linked into the offline test.  The rule
    # itself is asserted in MeshShape.h; these pins are what say the rule is
    # actually reached and that its answer is used.
    ("MeshShape::LiftOntoSnow(", 1,
     "the lift is computed by the named rule rather than inline, so the "
     "test that asserts it is asserting the code that runs"),
    ("lowestZ, objectHeight, surfaceZ, lift, Settings::objectLiftSlack);", 1,
     "and the dead band handed to the rule is the same setting the caller "
     "branches on below.  Two different numbers here is the twitch again: a "
     "band smaller than the threshold leaves a window where the object is "
     "below the surface but not enough to act on, so it is never lifted out "
     "of it"),
    ("if (Settings::objectLiftToSnow && !shaft && lift > 0.0f &&", 1,
     "the lift is branched on by name, and is limited to loose objects: a "
     "shaft is resolved to its own contact point and moving it would throw "
     "the arrow off the thing it stuck into"),
    ("surface == Surfaces::Type::kSnow && shapes[0].vertical > 0.0f) {", 1,
     "and it only runs on snow, with a measured height - raising an object "
     "under a blanket of no thickness is an invented move"),
    ("a_ref->SetPosition(RE::NiPoint3{ pos.x, pos.y, wroteZ });", 1,
     "the object is actually moved, and only upwards.  The value written is "
     "the named one above, so the log can report the number that was passed "
     "to SetPosition instead of recomputing it and possibly disagreeing"),
    ("a_ref->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, false);", 0,
     "the keyframed instant is gone.  It was added on the reading that the "
     "solver otherwise puts the object back - measured as a fresh raise on "
     "every scan (1.6, 9.7, 3.1) while the centre fell 60 units.  That "
     "reading is arithmetic on two clocks (the scan runs ten times a second, "
     "the log line once), and with only the waking half of the pair removed "
     "the body is still awake - `motion=3`, a normal dynamic body - and still "
     "cycling, which no keyframed-instant theory predicts.  Pinned at zero so "
     "the instant cannot come back unexamined"),
    ("a_ref->Update3DPosition(true);", 0,
     "the warping update is gone, and this is the pin that holds the case.  "
     "One argument apart from `false`, and the difference is the one the "
     "symptoms were made of: a warp puts the body where the node was put, the "
     "solver integrates it against nothing, and gravity closes the gap - the "
     "raise is undone ten times a second, and the same warp overwrites the "
     "displacement a push had just produced.  The log shows `motion=3` with "
     "the mass centre moving between -9113 and -9126 and a non-zero speed on "
     "every line"),
    # Given back its dynamic body, and *forced*.
    #
    # `TESObjectREFR::SetMotionType` asks the node with `a_force = false`
    # (TESObjectREFR.cpp:1048), so the engine may decline - and on a dropped
    # object it does.  Measured on a fur helmet, with the motion type printed
    # every second: the body reads `motion = 3` after the restore has run, on
    # every raise, and 3 is box inertia - a body the solver simulates - so the
    # state the restore leaves behind is not the frozen one the velocity
    # clearing was meant to fix.
    ("if (a_ref->Get3D()) {", 1,
     "the single exit is still a single exit, and it still tests the node "
     "rather than the reference.  It is not a motion-type call any more: the "
     "write is written through with `Update3DPosition(false)`, so there is no "
     "body state to hand back and no local binding of the node left unused.  "
     "A branch added later still cannot skip the write-through by not "
     "repeating the call"),
    ("if (auto* node = a_ref->Get3D()) {", 0,
     "and the binding is pinned out.  With the motion-type call gone, a "
     "named `node` in that condition is a local nothing reads - under this "
     "project's zero-warning gate that is a build failure, not a style "
     "question, so the pin is a gate rather than a preference"),
    ("RE::hkpMotion::MotionType::kDynamic, true, true, false);", 0,
     "the restore is gone.  Not waking the body was held to stop the raise "
     "undoing itself, and the log refuses it: the body comes back awake "
     "anyway - `motion=3`, dynamic, with a non-zero speed - because the warp "
     "in the next statement is what wakes it, not the restore.  Pinned at "
     "zero so the argument cannot be re-made from the code alone"),
    ("a_ref->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, false);", 0,
     "the second pin on the same string, kept because the two sat in "
     "different readings of the pair - one as the call, one as the reasoning "
     "that justified it - and removing the call has to remove the reasoning "
     "with it"),
    ("if (MeshShape::LiftMustFreeBody(action)) {", 1,
     "the one place the body is handed back, and the decision is a named "
     "function rather than a repetition of the call at each exit - so the "
     "three actions can be asserted.  Behaviour pin: CheckLiftMustFreeBody"),
    ("auto action = MeshShape::LiftAction::kNone;", 1,
     "the scan records what it did instead of acting on it in place, so "
     "the free/keep decision is asked once.  A branch that forgets to set "
     "this leaves the action at kNone and the body keyframed, which is why "
     "both acting branches must set it - see the two assignments"),
    ("bool backToDynamic = false;", 0,
     "still not back.  The restore does not keep an answer for itself any "
     "more - it runs at one exit unconditionally behind LiftMustFreeBody, "
     "so a local holding one would be a value nothing reads, the exact "
     "shape of code this pin exists to keep out"),
    ("a_ref->AddChange(RE::TESObjectREFR::ChangeFlags::kHavokMoved);", 2,
     "the engine is told the havok transform moved on both exits, as the "
     "wrapper did on the way in - the node path bypasses that wrapper, so "
     "nothing else tells the engine the body was moved"),
    ("a_ref->SetMotionType(RE::hkpMotion::MotionType::kDynamic, true);", 0,
     "the old unforced restore is not back"),
    ("a_ref->Update3DPosition(false);", 1,
     "the non-warping update is what the write uses, at the block's single "
     "exit: the node is written through and the body is left where the solver "
     "had it.  Pinned at one so a warp cannot come back beside it as a second "
     "copy"),
    ("LogObjectLifted(a_ref, rise, objectHeight, lift);", 1,
     "the lift is named in the log, so a run with no lift and a run with the "
     "lift removed are not the same silence"),
    ("rise = std::clamp(", 0,
     "the clamp and the lower guard live in the named rule; a second copy "
     "here would be a rule that two places can disagree about"),

    # The physics state at the moment of the lift.
    #
    # A raise the solver undoes in the same frame prints identically on the
    # lift line to one that sticks, so that line cannot answer whether the
    # placement took.  An object found cycling in game - raised, seen back
    # near the ground, raised again, and higher each time - has two mechanisms
    # that explain it equally well, so the quantities that tell them apart are
    # read from the motion the solver actually integrates: the speed and spin
    # held before the move, the speed held after the write-through, and the
    # motion type it was left in.
    ("Lift physics: centre.z {:.2f} -> {:.2f} (raised {:.2f}) ", 1,
     "the centre is read back from the body that owns the transform, so "
     "'the move stuck' is a measurement taken in the same call rather than a "
     "hope pinned on the next scan's terms line"),
    ("| speed=({:.2f},{:.2f},{:.2f}) spin=({:.2f},{:.2f},{:.2f}) ", 1,
     "and the velocity is printed, because a large one here is the "
     "carried-impact story while a still body is the switch-injected one - "
     "the two need different fixes and are otherwise indistinguishable"),
    ("| lift={:.2f} motion={} rest={}", 1,
     "the motion type it was left in, and beside it the solver's own count of "
     "how many consecutive steps found the body still.  `rest` is the one "
     "reading on this line that changes on its own: unlike a velocity read at "
     "the instant of a write, it carries the last few hundred milliseconds, "
     "so it can contradict the rest of the line instead of always agreeing "
     "with it"),
    ("static_cast<int>(motion.deactivationNumInactiveFrames[0]));", 1,
     "and that count reaches the log - a value read and not printed is the "
     "same as not reading it"),
    ("| lift={:.2f} motion={} rest={}\",", 1,
     "the blanket's thickness is printed with it, because it is the other "
     "number that sat at its limit while nothing else moved: the surface the "
     "object is raised towards is landZ + lift"),
    ("float a_rise, float a_afterZ, float a_lift)", 1,
     "and the thickness is a parameter rather than read here, so the line "
     "reports the value the rule acted on rather than measuring a state "
     "afterwards"),
    ("const float beforeZ = shapes[0].centre.z;", 1,
     "the centre is captured before the move, so the read-back above has "
     "something to be compared against"),
    ("void LogLiftRestored(RE::bhkNiCollisionObject* a_object, RE::TESObjectREFR* a_ref)", 1,
     "the line that reports the body after the write-through.  It takes the "
     "collision object rather than the reference because the motion is read "
     "off the body - the node's position is a value this block wrote, so it "
     "cannot witness its own write-through"),
    ("int64_t           g_objectLiftRestoredAt{ 0 };", 1,
     "and it is under its own log budget, not the physics line's.  Two "
     "lines from one call site sharing a budget is how the second half of a "
     "measurement goes silent, and the silent half here is the one that "
     "says whether the write-through took"),
    ("LogLiftRestored(shapes[0].collidable, a_ref);", 1,
     "and it is called after the write-through, not before it.  The "
     "position is the whole point: called from the write it would print the "
     "state the write created, which says nothing about whether the "
     "placement reached the body"),
    ("afterZ = parts[2] * RE::bhkWorld::GetWorldScaleInverse();", 1,
     "and the read-back is converted the same way every other centre in this "
     "file is, so the two numbers are in the same units and can be subtracted"),
    ("constexpr int64_t kObjectLiftPhysGapMs = 1000;", 1,
     "the physics line is spaced a second, not the lift line's ten: the cycle "
     "being diagnosed fires a lift every scan, and a ten-second gap would "
     "report one of a hundred and name the count as one"),
    ("if (!LogBudget::Allow(g_objectLiftPhysAt, NowMs(), kObjectLiftPhysGapMs)) {", 1,
     "and the budget is actually consulted, so the spacing is a rule the code "
     "keeps and not a comment about one"),

    # The node and the mass centre, printed for the same instant.
    #
    # `SetPosition` moves the reference's node, while a scan's `centre` is
    # the physics body's centre of mass - two different layers, and they were
    # being compared as one quantity.  Measured on a helmet: a raise of 1.61
    # was logged next to a centre that had moved 45.45.  Nothing in the log
    # said which layer each number came from, so that gap reads as the move
    # being amplified just as well as it reads as the read-back measuring a
    # different object - and those two need opposite fixes.
    ("Lift frames: asked {:.2f} | node {:.2f} -> {:.2f} (moved {:.2f}) ", 1,
     "the node's before and after are printed next to the raise, so whether "
     "the reference moved by the amount that was asked for is a reading and "
     "not an inference drawn from a number taken off another layer"),
    ("| centre {:.2f} -> {:.2f} (moved {:.2f})", 1,
     "and the mass centre's own before and after follow it, so the two "
     "displacements can be compared directly instead of being subtracted "
     "across layers"),
    ("constexpr int64_t kObjectLiftFrameGapMs = 1000;", 1,
     "the frames line carries its own budget.  Sharing the physics line's "
     "would have the first line spend it and the second go silent - and the "
     "silent one is the one that names the gap it exists to measure"),
    ("LogLiftFrames(a_ref, nodeBefore.z, nodeAfter.z, beforeZ, afterZ, rise);", 1,
     "and it is actually called from the lift, so the reading exists in a run "
     "rather than only in the source"),

    # Zeroing the velocity the move itself produced, and why it is gone.
    #
    # A warp is a teleport, and a teleport is how a solver is told something
    # moved fast, so it answers with the relative motion implied by the jump.
    # Measured on a fur helmet: a raise of 1.61 was followed by a mass centre
    # 45 units higher, then 70, then 92 - rising every time - while the object
    # fell back to the same spot in between.  That is a body being thrown and
    # falling, not one being placed.
    #
    # A clear keyed on the size of the raise does not survive that either:
    # measured with the clear unconditional - one raise a second, every one of
    # them small - the object stopped answering to being kicked, because the
    # raise and the kick land in the same instant often enough that a clear at
    # any size is a clear of the kick.  The lines below are pinned out, so a
    # clearance cannot come back keyed on any size at all.
    ("constexpr float kLiftWarpSpeed = 8.0f;", 0,
     "not back.  The clear it gated is gone, and the constant went with it "
     "- an unused one at namespace scope is not diagnosed, so leaving it "
     "would leave live-looking tuning to build on"),
    ("if (rise > kLiftWarpSpeed &&", 0,
     "not back, for the same reason"),
    ("hkpRigid->SetLinearVelocity(still);", 0,
     "not back, and this is the one that matters most.  A clearance that "
     "fires on a raise the player caused is a clearance of the player's own "
     "push, which is the symptom the lift block is about"),
    ("hkpRigid->SetAngularVelocity(still);", 0,
     "not back, for the same reason"),
    ("const RE::hkVector4 still{ 0.0f, 0.0f, 0.0f, 0.0f };", 0,
     "not back: the vector and both calls went together.  A zero kept "
     "beside no call is the shape of half a removal"),
    # The write line, restated against one clock.
    #
    # The line below used to print a `from` read out of the scan and subtract
    # it from a value written a moment later.  The two clocks run at 0.1 s and
    # 1 s, so that subtraction is arithmetic across a tenth of a second of
    # drift - and it is what produced the phantom "a raise of 1.61 moved the
    # centre 45".  The three values that are read in one instant are the ones
    # printed now.
    ("Lift write: scan {:.2f} -> wrote {:.2f} | body after {:.2f} (landed {})", 1,
     "the values that can be compared are the ones printed: the scan's own "
     "reading is printed beside them only so the two clocks can be seen to be "
     "different, and it is named `scan` rather than `from` so it cannot be "
     "mistaken for the same instant a second time"),
    ("(reference moved {:.2f})", 0,
     "not back.  How far the reference node travelled is a question about the "
     "node while the raise is a question about the body, and restating every "
     "write against the wrong layer is how the phantom throw got a second "
     "round of code built on it"),
    ("constexpr int64_t kObjectLiftWriteGapMs = 1000;", 1,
     "the write line carries its own budget; sharing one would let a "
     "neighbouring line spend it and leave this one silent"),
    ("LogLiftWrite(a_ref, beforeZ, wroteZ, afterZ);", 1,
     "and it is called from the lift, at the write, with the scan reading, the "
     "value written and the body's own read-back - so the pair it compares was "
     "taken at the write and in one instant"),
    ("const float wroteZ = beforeZ + rise;", 1,
     "the write is stated against the mass centre - the same quantity the "
     "raise was computed from, so the two cannot be a layer apart.  A write "
     "stated against the reference's own position is stated against a "
     "different layer, which is how a sampling artefact came to read as a "
     "throw"),

    # The settle gate.  Without it the block below runs on every scan the
    # object is under the snow - which, for an object resting on the terrain
    # under a 35-unit blanket, is always - so its position is written ten
    # times a second and every write is re-derived by the solver in the same
    # frame.  A body written by this block and re-settled by the solver is a
    # body neither of them owns, and the one thing that never works on it is
    # the player's push.
    ("if (rise <= MeshShape::LiftSettledBand(", 1,
     "an object already standing where the rule wants it is left alone, and "
     "the test is on `rise` - the same quantity the branch below acts on - so "
     "the two cannot disagree about whether it is in place.  The slack is "
     "passed in because the band is clamped against the smallest raise the "
     "rule can make, and that is a function of the slack"),
    ("} else if (rise > Settings::objectLiftSlack) {", 1,
     "and the INI's own threshold is still the only way into the move, so an "
     "object inside that slack is still never touched"),

    # The height terms, printed term by term.
    #
    # `drop = 2.6` on a helmet that should read about -8.2 has two possible
    # readings and the old log could not tell them apart: either the bound the
    # height came from is not the helmet, or the terrain under the probe is not
    # the terrain the helmet is on.  Printing the answer alone is what made it
    # ambiguous, so the terms are pinned by name - a run where the line is
    # missing cannot be told from a run where the drop was never computed.
    ("\"Object height terms: land={:.2f} lift={:.2f} surface={:.2f} \"", 1,
     "every term of the height question is printed, not just its result"),
    ("\"| bound0 centre.z={:.2f} vertical={:.2f} radius={:.2f} \"", 1,
     "and the bound they came from, with the two quantities that are easily "
     "mistaken for one another - the shape's vertical half extent and its hull "
     "radius - printed side by side"),
    ("| groundZ={:.2f} drop={:.2f} \"", 1,
     "so a drop that disagrees with its own terms is visible in one line"),
    ("LogBudget::Allow(g_objectTermsAt, NowMs(), kObjectTermsGapMs)) {", 1,
     "and it is rate limited rather than given a one-shot budget - the terms "
     "must still be printed after the opening seconds, or a run that goes "
     "wrong later reads as a run with no objects"),

    # The buried widening.
    #
    # Source pins rather than dll pins, because the widening is a computation:
    # it leaves no string in the image, and a dll pin could not tell a wired
    # rule from one that was written and never called.  The condition is
    # pinned apart from the call it guards, because a string pin stays green
    # on `if (false && snowOver > 0.0f)` - only the condition's own text
    # catches a branch switched off rather than removed, and a branch switched
    # off leaves no other trace at all.
    ("if (snowOver > 0.0f) {", 1,
     "the widening is still guarded by the snow it exists for; disabling the "
     "condition instead of deleting the branch keeps every other pin green"),
    ("const float face = std::max(mesh.world.halfLength, shape.radius);", 1,
     "the width is still taken from the object's own geometry - the mesh half "
     "length where a reading exists, the hull radius otherwise - and not from "
     "a constant that happens to fit a helmet"),
    ("MeshShape::BuriedRadius(face)", 1,
     "and that width is still handed to the rule; a pin on the rule alone is "
     "green with the answer computed and thrown away"),
    ("stamp.shoulder = std::max(", 1,
     "and the mark's own shoulder is still raised with it, which is the other "
     "half of the fix: the depth was already right and the hole was still all "
     "slope, so a width alone would have been a wider dent and not a floor"),

]

# Pins against MeshShape.h, the header that owns the shared geometry rules.
SRC_PINS_MESH_SHAPE = [
    # The band the caller uses to decide whether to touch the body at all.  A
    # rule of its own, written as a named function so it can be asserted on
    # its own and so a reader can find it - a value buried inside the caller
    # is a rule nothing can test.
    ("inline float LiftSettledBand(float a_height, float a_slack)", 1,
     "the 'is it already where the rule wants it' band is a named rule, "
     "separate from the dead band inside LiftOntoSnow, because the two are "
     "asked different questions and have different sizes.  It takes the "
     "caller's slack because the band has to be clamped against the "
     "smallest raise the rule can make, and that depends on the slack - see "
     "the ceiling pin below"),
    ("inline float LiftSettledBand(float a_height)", 1,
     "the one-argument spelling is kept for callers and tests that do not "
     "carry a slack, and it forwards to the two-argument rule rather than "
     "restating it - two copies of a rule is how they come to disagree"),
    ("float band = a_height * 0.1f;", 1,
     "the band scales with the object, so it means the same thing to a coin "
     "and to a cart - the quantity it is compared against is a distance on "
     "the object, and a flat constant would be most of one and none of the "
     "other"),
    ("if (band < 0.5f) {", 1,
     "the floor, because a very small object's tenth is finer than the "
     "drift a resting body shows between two scans and the body would be "
     "written every frame"),
    ("if (band > ceiling) {", 1,
     "the ceiling, and this is the one that is not obvious.  A tenth of the "
     "height passes `slack + margin` at a height of 35 - an ordinary crate "
     "- and past that every raise the rule is willing to make is one the "
     "band calls settled: the rule and the caller contradict each other, "
     "nothing is moved, and nothing is written through.  Clamping to "
     "the smallest raise makes the two agree by construction"),
    ("const float margin = slack > kLiftSettleMargin ? slack : kLiftSettleMargin;", 2,
     "the ceiling is computed with the same margin rule LiftOntoSnow uses, "
     "so the two cannot drift apart about what the smallest raise is - "
     "counted as 2 because that is the point: one copy in the band and one "
     "in the rule, both spelled the same way"),
    ("if (!std::isfinite(a_height) || !(a_height > 0.0f)) {", 1,
     "an unmeasured height gets no band rather than a default one, so the "
     "caller cannot decide an object is settled on no evidence"),

    # The lift rule, with all three of its deliberate properties: the dead
    # band, the landing margin, and the slack being a parameter.  The
    # signature and the lines that state all three are pinned, because a band
    # and a margin are a couple of words changed in place and leave no string
    # of their own.
    ("inline float LiftOntoSnow(float a_lowestZ, float a_height, float a_surfaceZ,",
     1, "the lift is a named rule, so it can be asserted rather than only "
        "described"),
    ("float a_lift, float a_slack)", 1,
     "the rule takes the dead band as a parameter, so the raise can stop "
     "before the surface instead of hunting it forever"),
    ("inline constexpr float kLiftSettleMargin = 3.0f;", 1,
     "the landing margin is a named quantity of its own, not the caller's "
     "threshold reused: that threshold asks 'is this worth acting on' and "
     "wants to be small, while this asks 'how long until the body asks "
     "again' and wants to be seconds of settling"),
    ("const float slack = a_slack > 0.0f ? a_slack : 0.0f;", 1,
     "a negative band is clamped, because it would turn the test below into "
     "'always act' and could hand the caller a negative raise"),
    ("const float wantLowest = a_surfaceZ - a_height;", 1,
     "the target is the snow surface less the object's own height - the "
     "object's top comes level with the snow"),
    ("if (!(rise > slack)) {", 1,
     "a sink smaller than the band asks for nothing - this is the line that "
     "stops the one-raise-a-second twitch, and it never lowers, so gear on a "
     "rock or a floor is still left where it is"),
    ("const float margin = slack > kLiftSettleMargin ? slack : kLiftSettleMargin;",
     2, "the landing margin is the band or the settle margin, whichever is "
        "larger, so a caller that sized its band small for the sake of not "
        "ignoring real sinks still gets a body that stays put.  Counted as 2 "
        "because the band rule computes the same quantity with the same "
        "spelling - one copy in the rule, one in the band it is clamped "
        "against, which is what keeps them from drifting apart"),
    ("const float want = rise + margin;", 1,
     "the raise lands above the surface by that margin, not on it - a body "
     "aimed exactly at the surface settles below and asks to be lifted again, "
     "and the margin is what buys the seconds before it does"),
    ("return want < a_lift ? want : a_lift;", 1,
     "and it is clamped to the blanket, because a larger lift is a misread "
     "measurement rather than a deeper burial"),
    # The ceiling on a mark's depth is a named rule with both of its guards,
    # because a comparison between two lengths can be deleted without leaving
    # a trace in any string.
    ("inline float CapDepthByThickness(float a_depth, float a_halfThickness,",
     1, "the depth cap is a named function and not an inline expression, so a "
        "test can call it and a reader can find it"),
    ("const float ceiling = a_halfThickness * a_perThickness;", 1,
     "the ceiling is the object's own half thickness times a stated ratio - a "
     "flat constant here would bury a ring under a ceiling meant for a truck"),
    ("return a_depth < ceiling ? a_depth : ceiling;", 1,
     "and it can only ever take a mark down: a shallow scratch on a thick "
     "object keeps its own depth"),
    ("if (!(a_perThickness > 0.0f) || !(a_halfThickness > 0.0f)) {", 1,
     "zero means 'cap off' on both sides - a zero ratio is the escape hatch "
     "and a zero thickness is 'not measured'"),
    ("return a_depth;", 3,
     "both guard arms hand the raw depth back, so neither can zero a mark "
     "that used to have one - and the third is MarkDepthFor's own guard, "
     "which keeps a NaN depth a NaN rather than a number the shader draws"),

    # The mark has to be deep enough to reach the object.
    #
    # The rule above is about an object standing in its own dent.  A dropped
    # object is not in its dent - it is on the bare terrain under the blanket -
    # so a ceiling derived from its own thickness leaves it buried by the
    # difference, and the mark is drawn where it cannot be seen.  Both functions
    # are named and both are called from ObjectStamps.cpp, so the offline test
    # can assert them and the scan can hold the call site.
    ("inline float MarkCeiling(float a_halfThickness, float a_perThickness, float a_reach)",
     1, "the ceiling is a named function of the object's thickness and the snow "
        "over its own underside, not an expression at the call site"),
    ("return byThickness > reach ? byThickness : reach;", 1,
     "the deeper of the two answers wins, which is the whole change: the "
     "thickness cap still decides every object that is not under the snow"),
    ("const float reach = SnowOver(a_reach);", 2,
     "and both rules take the reach through the one normalisation, so a NaN "
     "cannot become a ceiling in one of them and a depth in the other"),
    ("return std::isfinite(a_reach) && a_reach > 0.0f ? a_reach : 0.0f;",
     1, "a reach that is not a usable measurement is none at all, in one "
        "place - the reading is ignored rather than becoming a ceiling"),
    ("inline float MarkDepthFor(float a_depth, float a_halfThickness,", 1,
     "and the depth itself is a named function, so the reach is asserted by a "
     "test and not only by the in-game line"),
    ("const float depth = a_depth > reach ? a_depth : reach;", 1,
     "the mark is raised to the reach.  A ceiling alone cannot do this: the "
     "steel arrow's own scaled depth was 2.5 under a 35-unit blanket, so the "
     "site was emitting a mark the arrow was not in, with every other field "
     "on the line reading as intended"),
    ("if (ceiling > 0.0f && depth > ceiling) {", 1,
     "and it is still cut down by the ceiling - zero meaning 'no ceiling', "
     "which is what the escape hatch needs to keep working"),

    # The rule that puts a floor under an object the blanket swallowed.
    #
    # A mark deep enough to reach the object still leaves the object in the
    # drift, because the shader sinks at full depth only out to
    # `radius * shoulder` and bare snow carries shoulder = 0.0 - so the whole
    # disc was falloff and the snow at the object's own edge had barely moved.
    # The pair is pinned together because either half alone reads as a fix:
    # the fraction without the division widens nothing, and the division
    # without the fraction leaves the floor at zero.
    ("constexpr float kBuriedShoulder = 0.6f;", 1,
     "the flat floor is a fraction of the radius and it is a named constant, "
     "so the offline test and the engine read the same number - the test "
     "asserts the rule, and a rule it cannot see is one it cannot fail"),
    ("inline float BuriedRadius(float a_halfExtent)", 1,
     "the width rule is a named function, which is what lets it be asserted "
     "directly instead of through a stamped mark"),
    ("return a_halfExtent / kBuriedShoulder;", 1,
     "and the floor reaches `extent` only if the radius is the extent over "
     "the shoulder; an unmeasured extent still returns zero, which is what "
     "leaves the caller's own radius alone rather than drawing nothing"),

]

# Pins against ActorShapes.cpp, where the vertical reach is measured.
SRC_PINS_ACTOR_SHAPES = [
    ("a_extent.vertical = 0.0f;", 1,
     "the vertical reach is cleared with the other measured numbers, so a "
     "failed call cannot leave the previous shape's height behind"),
    ("a_extent.vertical = hz;", 1,
     "the reach is the raw z half extent.  Not `length`, not `radius`: a "
     "box's radius is its space diagonal and its length its longest "
     "horizontal side, and either one subtracted from the centre drops the "
     "object below the ground it is standing on"),
]

# Pins against ActorShapes.h, read with the comments left IN.
#
# The default checker strips comments, and that is what lets a pin tell "this
# code is here" from "this code is commented out".  It also means a pin against
# a note can never pass at any count - so this group opts out of the stripper
# rather than being written as a pin that reads zero by construction.
#
# The note below is the only record the measurement has.  Havok documents
# `hkpConvexShape::getMaximumProjection` as the core's projection plus
# `m_radius`, so the next person to open the header arrives at "the radius is
# added to all six readings, subtract it" and the code accepts it silently -
# a half extent that is short by the radius, on an object whose radius is
# larger than its height.  Nothing in the image records that this was tried,
# so the note is the whole defence.
SRC_PINS_ACTOR_SHAPES_H = [
    ("The engine's support readings do **not** carry the shape's base", 1,
     "the note refuting the radius subtraction is still in the header.  Read "
     "raw, because a comment is the only thing that can record a correction "
     "that was made and withdrawn - there is no line of code that differs "
     "between the right reading and the wrong one"),
    ("A single radius cannot describe an arrow: it is long and thin, and", 1,
     "and the note keeps the evidence, not only the conclusion.  The arrow is "
     "the object a single added constant cannot fit, and the reason the "
     "refutation is reproducible from one session's log rather than an "
     "appeal to authority"),
]

# Pins against Settings.cpp.  There is no string in the image that records
# "this key was parsed", so the branch has to be read from the source: a key
# whose parser branch is removed still leaves its reader compiling and its
# behavioural pins green, and the only visible symptom is a value that is
# always the default.
#
# Note also that the key NAME cannot be found in the binary on its own.
# `key == "ObjectLiftToSnow"` is compiled to immediate comparisons of 8-byte
# fragments, so the whole string never appears; the reader found `ftToSnow`
# and nothing else.  A check written as "is the key name in the dll" would
# report a false zero here.  This is why the pin is against the source.
SRC_PINS_SETTINGS = [
    ("if (key == \"ObjectLiftToSnow\") {", 1,
     "the lift's own switch is parsed, so turning it off in the ini reaches "
     "the reader"),
    ("if (key == \"ObjectLiftSlack\") {", 1,
     "and the slack has a parser, so its anti-chatter floor can be set"),
    ("objectLiftToSnow = AsBool(value);", 1,
     "the switch is stored, not merely recognised"),
    ("objectLiftSlack = Clamped(key, AsFloat(key, value, 0.5f), 0.0f, 64.0f);",
     1,
     "and the slack is stored with its 0.5 default, the same default the "
     "header declares - two different defaults would make the ini's own "
     "comment a lie"),
    ("} else if (key == \"ObjectLiftToSnow\") {", 0,
     "neither key is back inside the else-if chain whose length stops the "
     "file building (C1061)"),
]

# Pins against the offline test file itself.
#
# The assertion below is the only record of a measurement that cannot be
# re-derived from the dll: that the engine's support readings do not carry the
# shape's base radius.  Deleting the test leaves every dll pin green, because
# nothing in the image knows a test ever existed.
SRC_PINS_STAMP_TEST = [
    ("void CheckProjectionReadings()", 1,
     "the assertion that pins what the six support readings actually are is "
     "still in the test file, and still dispatched from main"),
    ("CheckProjectionReadings();", 1,
     "and it is dispatched, so a test that is defined and never called "
     "cannot pass for a test that ran"),
    ("PASS projection readings:", 1,
     "and it prints its own pass line, so its absence from the output is "
     "visible rather than assumed"),
    ("void CheckMarkReachesTheObject()", 1,
     "the assertion that pins the rule which gets a mark down to an object "
     "the blanket has swallowed is still in the test file"),
    ("CheckMarkReachesTheObject();", 1,
     "and it is dispatched - a rule this one was added for, left defined and "
     "never called, would pass for a rule that was checked"),
    ("PASS mark reaches:", 1,
     "and it prints its own pass line, so its absence from the output is a "
     "failure the run says out loud"),
    ("void CheckBuriedWidth()", 1,
     "the assertion that pins the width rule is still defined - a mark deep "
     "enough to reach the object still leaves it in the drift, because a hole "
     "no wider than the object spends its whole disc on the shader's "
     "falloff"),
    ("CheckBuriedWidth();", 1,
     "and it is dispatched, so a width test written and never called cannot "
     "pass for a width that was checked"),
    ("PASS buried width:", 1,
     "and it prints its own pass line, so its absence is a failure the run "
     "says out loud rather than one the reader has to notice is missing"),
    ("buried * MeshShape::BuriedShoulder() >= helmetHalfWidth - 1e-3f", 1,
     "the width assertion still carries the arithmetic's tolerance.  The rule "
     "divides by the shoulder and the shoulder multiplies straight back, so "
     "the two sides are equal only up to the last bit of a float round trip "
     "and an exact comparison fails on a healthy build; this pin is what "
     "stops that being read as a broken rule and the rule being changed to "
     "suit it"),
]

# Read raw, for the same reason as the header's group above: the sentence
# pinned here is a comment, and the default checker would read zero against it
# regardless of whether it is present.
#
# It is a separate group from the one above because the two pin different
# things.  The group above pins that the test exists and runs.  This one pins
# that the test still says WHY - that the measurement was taken because the
# opposite reading looks right and had already been written once.  A test that
# keeps its name and loses its reason is the one that gets "cleaned up" by
# whoever next reads Havok's documentation and believes it.
SRC_PINS_STAMP_TEST_COMMENT = [
    ("They do not, and this assertion pins that because the opposite was written,",
     1,
     "the test still records that the refuted correction was written and "
     "built before it was refuted, not merely that the readings are what "
     "they are - the reason is what stops the correction being made again"),
]

# (needle, expected count, what it is evidence of)
PINS = [
    ("0.1.0.2", 1, "the build tag, matching the version declared in plugin.cpp"),
    ("width-knob-rim-jitter", 0, "the revision-scoped tag it replaced is gone"),
    ("mark-align-foot", 0, "and the one before that"),
    ("shaft-snow-surface", 0, "and the one before that"),
    ("rim-band-world", 0, "and the one before that"),
    ("one-surface", 0, "and the one before that"),
    ("shaft-log-rate", 0, "no revision-scoped tag survives in the image"),

    # The body's state, read after the write-through -----------------------
    #
    # The physics line is read before the write-through, so its `motion=` is
    # the state the write created and cannot show what the body was left in
    # afterwards.  This is the reading on the far side of that call, and it
    # is a dll pin rather than a source pin because a source pin cannot say
    # the line was compiled in: the format string is the thing that has to be
    # in the image.
    ("Lift restored: node {:.2f} | centre {:.2f} motion={} rest={} ", 1,
     "the reading taken after the write-through, and the only one that can "
     "say what state the body was left in.  It confirms that the body is "
     "exactly as the solver had it.  The value the line printed was 3, which "
     "is neither of the two the argument had been framed in (1 dynamic / 4 "
     "keyframed) - a reading that did not fit the question that was being "
     "asked of it"),
    ("| render {:.2f}", 1,
     "and the render position is in the image with it: the scene graph's own "
     "`world.translate`, what the renderer draws, as opposed to the "
     "reference record this block writes.  It is the reading that decides "
     "whether a raise written without a warp survives the frame - if the "
     "engine copies the body back over the node, this number follows the "
     "centre down and the object is under the snow however right every "
     "other number on the line looks"),

    # --- the surface -------------------------------------------------------
    # The two shape-dependent formats, one per route.  Two is the count
    # because LogShaftStamp carries a line form and a disc form, exactly as
    # `rim {:.2f}` does below.
    ("lowest {:.2f} above snow (limit {:.2f})", 2,
     "the mark line reports the height above the SNOW on both routes"),
    ("end {:.2f} above snow | ", 1,
     "and the end field names the snow, on the route that has one"),
    ("end {:.2f} above land | ", 0,
     "the old spelling is gone - it named a surface the gate no longer uses"),
    # Exactly one `above land` must survive, and it is the foot path's own
    # format in LogCollidable.  Zero would mean the foot diagnostic was
    # renamed along with the weapon one, which would be a silent change to a
    # line this round had no business touching; two or more means a weapon
    # format was missed.
    ("above land", 1,
     "exactly the foot path's own field keeps the old spelling - see the note"),
    # NOTE (why there is no bare `above snow` pin here): counting that phrase
    # by hand gives 3, not 2, and the extra one is real - the collision probe
    # format reads `{} lowest corner {:.2f} above it`, whose "it" is the snow.
    # A bare-phrase pin would therefore either read 3 (and look, wrongly, like
    # the foot format had been renamed) or, pinned at 3, would silently absorb
    # a genuine fourth site.  The phrase cannot distinguish sites, which is
    # what rule V is about; the formats above name each site instead.

    # --- the mark alignment, width knob, and rim jitter --------------------
    ("MarkAlignToFoot=", 1,
     "the startup line reports the alignment switch"),
    ("FootAspect={:.2f} MaxAspect={:.2f} RimJitter={:.2f} RimJitterBand={:.2f}"
     " ", 1,
     "and the four numbers that decide the aligned shape and the wobble of "
     "its edge - FootAspect is the width knob, so a build that lost it would "
     "silently fall back to the checkbox's hard-coded default"),

    # --- the rim jitter, shader half ---------------------------------------
    #
    # The gate is pinned here rather than in the source list because the gate
    # is *shader text*, and shader text is what this half of the file scans.
    # It is worth pinning because it is a caller rule: the arithmetic lives in
    # RimEdgeJitter, the decision to apply it to a line and not to a footprint
    # lives in this one `if`, and flipping the `if` to always-on would leave
    # every other string in the image intact.  Both halves of the condition
    # are in the pin, so neither the shape test nor the amount test can be
    # dropped without it going red.
    #
    # Counts are the measured ones, not the header counts.  `RimEdgeJitter`
    # reads 3 in ClipmapUpdateCS.h - a definition, its call, and the comment
    # above the definition - and 6 in the image, because the shader body is
    # embedded twice and the comment survives in the emitted text.  Writing
    # the header count here is what the first run of this script caught: it
    # went red on both, which is the scan doing its job.
    ("if (StampShape[i].z > 0.0f && MarkRimJitter.x > 0.0f) {", 2,
     "the rim jitter is gated on the mark being a line, so a footprint keeps "
     "the smooth edge it has always had - twice, once per embedded copy"),
    ("float jitter = 0.0f;", 2,
     "and a mark that is not a line leaves both terms at their identity, so "
     "the footprint's rim is byte-for-byte what it was"),
    ("RimEdgeJitter", 6,
     "the rim jitter's definition, its comment and its call, in both copies "
     "of the shader"),
    ("MarkRimJitter", 10,
     "the constant buffer field it reads: the declaration, and each use, in "
     "both copies of the shader"),
    ("0.5f + 0.5f * n) - 0.25f * a_amount", 2,
     "the jitter is biased outward, so the edge wobbles without leaving gaps "
     "inside the band - an unbiased wobble eats the band it is drawn on"),

    # --- the gate and the window -------------------------------------------
    ("Shaft gate: carried clear", 1, "the refusal line is kept"),
    ("Shaft tally:", 1, "the tally window line is kept"),
    # Three, not two: the tally's own column plus the two shape-dependent
    # forms in LogShaftStamp.  Written as 2 first and measured as 3 - the
    # count is easy to get wrong here precisely because the stamp's two are
    # already inside the `lowest ...` pin above, so they are easy to forget
    # while reading the list.  Measured on the built image, then written down.
    ("(limit {:.2f})", 3, "the tally and both mark routes still print a limit"),

    # --- carried forward, untouched ----------------------------------------
    ("t {:+.2f}\0", 1, "the crossing parameter, formatted only where there is one"),
    ("t -\0", 1, "and the spelling for a mark that solved no crossing"),
    ("place {} {} | ", 2, "the column it goes in, in the line and the disc format strings"),
    ("place {} t {:+.2f}", 0, "the old inline form is gone from both routes"),
    ("rim {:.2f}", 2, "the rim is printed on the line route and the disc route"),
    ("ShaftStampMinDepth", 1, "the depth floor is kept"),
    ("StampMinDepth={:.2f}", 1, "and is still reported at startup"),
    ("Shaft stamp: LINE half length", 1, "the mark line"),
    ("drop {:.2f} of radial {:.2f}, axis z {:+.2f}", 1, "the thickness probe"),
    ("(drop {:.2f})", 0, "the old length-shaped drop format is gone"),
    ("SpanLengthScale={:.2f}", 1, "the startup line still reports the scale"),

    # --- the shader half, untouched ----------------------------------------
    ("StampOutlineOvershoot", 4, "the rim measures the mark's outline in world units"),
    ("(len - 1.0f) / invGrad", 2, "with the first-order distance to that outline"),
    ("max(bandRef * rim.x, 2.0f * Window.z)", 2, "and the band is held to two cells"),
    ("d - s.z", 0, "the mark's own units are gone from the rim"),
    ("groupshared", 2, "the shader body is embedded twice - see the note above"),

    # --- the mark that had to be deepened to get down to the object --------
    #
    # Read from the built image and not from the source, because the source
    # pin above is satisfied by the text existing in the file - it says
    # nothing about whether the branch that prints it was reached.  This is
    # the one field that separates the run where the object is out of the
    # snow from the run where it is not, and those two runs are otherwise the
    # same line with one number changed.
    ("mark reaches the object, {:.1f} of snow over its own underside", 1,
     "the wording that reports a mark deepened to reach the object is in the "
     "dll, and not only in the source"),

    # --- the buried reading, with no budget per object ---------------------
    #
    # The reach line can be in the image and the run still carry no reach
    # line for the object it was written for.  A per-object budget spent on
    # the first, airborne scan is one of the two readings that fits, and it
    # leaves the page silent either way - so the quantity the widening rule
    # is made of is now printed on a line with no per-object budget at all,
    # rate limited instead.
    ("Object buried: {} drop={:.1f} snowOver={:.1f} radius={:.1f} ", 1,
     "the buried reading is in the dll, carrying the drop, the reach derived "
     "from it, and the radius, depth and shoulder the mark ended up with: a "
     "run then says whether the widening ran, instead of leaving it to be "
     "inferred from a line that was never written"),
]


def main():
    if not os.path.exists(DLL):
        print("MISSING:", DLL)
        return 2
    with open(DLL, "rb") as f:
        blob = f.read()

    print("dll   :", DLL)
    print("size  :", len(blob))
    print("md5   :", hashlib.md5(blob).hexdigest())
    print("mtime :", datetime.datetime.fromtimestamp(os.path.getmtime(DLL)))
    print()

    bad = 0
    for needle, want, why in PINS:
        got = blob.count(needle.encode("utf-8"))
        ok = want is None or got == want
        if not ok:
            bad += 1
        tag = "OK " if ok else "!! "
        shown = "?" if want is None else str(want)
        print(f"{tag}{needle!r:46s} got {got:3d}  want {shown:>3s}   {why}")

    print()
    # Comments are stripped before counting, and that is not tidiness.
    #
    # The first version counted the raw text and read 7 where 5 was meant:
    # two of the matches were the explanatory comments that name the same
    # call while describing it.  A pin that counts prose is a pin that goes
    # red when a comment is reworded and green when a line is deleted with a
    # comment added in its place - wrong in both directions, and it would
    # have been read as evidence about the code either way.
    #
    # This is a line-based strip rather than a parser: it removes `//` to end
    # of line outside string literals, which is all this file's comments are.
    # It is deliberately conservative - a `//` inside a literal is left alone
    # - because a stripper that eats code would silently shorten the haystack
    # and turn every pin into a weaker claim.
    def strip_comments(raw_text):
        code_lines = []
        for line in raw_text.splitlines():
            in_literal = False
            quote = ""
            cut = None
            i = 0
            while i < len(line):
                ch = line[i]
                if in_literal:
                    if ch == "\\":
                        i += 2
                        continue
                    if ch == quote:
                        in_literal = False
                elif ch in "\"'":
                    in_literal = True
                    quote = ch
                elif ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
                    cut = i
                    break
                i += 1
            code_lines.append(line if cut is None else line[:cut])
        return "\n".join(code_lines)

    def check(header, path, pins, raw=False):
        nonlocal bad
        print(header)
        if not os.path.exists(path):
            print("MISSING:", path)
            bad += 1
            return
        with open(path, encoding="utf-8") as f:
            # `raw=True` keeps the comments in the haystack.
            #
            # Stripping them is the right default: it is what makes a pin able
            # to tell "the code is here" from "the code is commented out".  But
            # a note that records a *refuted* correction lives only in a
            # comment, and the default makes every pin against it read zero no
            # matter what the comment says - a pin that can never pass is not a
            # stricter pin, it is a broken one.  So the stripper is a choice at
            # the call site rather than a property of the file.
            text = f.read()
            if not raw:
                text = strip_comments(text)
        for needle, want, why in pins:
            got = text.count(needle)
            ok = want is None or got == want
            if not ok:
                bad += 1
            tag = "OK " if ok else "!! "
            shown = "?" if want is None else str(want)
            print(f"{tag}{needle!r:52s} got {got:3d}  want {shown:>3s}   {why}")
        print()

    check("--- source pins (Clipmap.cpp) ---", SRC, SRC_PINS)
    check("--- source pins (ObjectStamps.cpp) ---", SRC_OBJECT_STAMPS,
        SRC_PINS_OBJECT_STAMPS)
    check("--- source pins (MeshShape.h) ---", SRC_MESH_SHAPE,
        SRC_PINS_MESH_SHAPE)
    check("--- source pins (ActorShapes.cpp) ---", SRC_ACTOR_SHAPES,
        SRC_PINS_ACTOR_SHAPES)
    check("--- source pin, comments kept (ActorShapes.h) ---", SRC_ACTOR_SHAPES_H,
        SRC_PINS_ACTOR_SHAPES_H, raw=True)
    check("--- source pins (Settings.cpp) ---", SRC_SETTINGS,
        SRC_PINS_SETTINGS)
    check("--- source pins (StampSurfaceTest.cpp) ---", SRC_STAMP_TEST,
        SRC_PINS_STAMP_TEST)
    check("--- source pin, comments kept (StampSurfaceTest.cpp) ---", SRC_STAMP_TEST,
        SRC_PINS_STAMP_TEST_COMMENT, raw=True)

    print()
    print("MISMATCH" if bad else "ALL OK", f"({bad} mismatched)" if bad else "")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
