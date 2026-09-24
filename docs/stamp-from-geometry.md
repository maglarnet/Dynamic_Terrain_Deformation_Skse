# Deriving a carried object's mark from where it actually is

## Status

Shipped.  A carried weapon's mark is placed, sized and shaped from where the
object really is: its measured axes, its own buried stretch, and - when the mesh
can be read - its own vertices.  Nothing about the mark is keyed to a weapon by
name, so a new weapon needs no new tuning.

This document is the design record.  It is written to be read on its own: it
states what the problem was, what the code does now, why each decision was made,
and which parts are still policy rather than geometry.  It does not narrate the
order in which the decisions were reached.

## The problem

A carried weapon left a furrow that did not match the weapon.  The report was
concrete: *"walking, running and sprinting put the weapon in different places,
but the furrow is the same, so it feels wrong."*

The obvious reading is that the numbers feeding the stamp are stale - read once
at rest and never updated.  That reading is wrong, and the log says so.

`ActorShapes::GetExtent` and `ActorShapes::GetLongAxis` read the live
`hkpRigidBody` transform through `GetMaximumProjection`, and the animation
drives that body every frame.  Both numbers were therefore already tracking the
pose:

| Measured over 1224 frames with a bow drawn | Value |
|---|---|
| Bound bottom, lowest | -13.52 |
| Bound bottom, highest | +12.41 |
| Frames where the bottom was above ground | 919 of 1224 (75%) |
| Largest single-frame turn of the long axis | 22.6 degrees |
| Bound centre's offset to the character's right | +17 to +31 units |
| **Measured half length of the bow** | **67.89 units** |

Those are live numbers.  What `Clipmap.cpp` did with them was the problem: it
read the extent and the axis, **logged them, and then placed the stamp at the
bound centre** - one point rigid to the body, which does not care what the legs
are doing.

So the diagnosis is not "the measurement is stale".  It is:

1. **The mark was placed at the wrong point.** The bound centre, not the part
   of the weapon that is in the snow.  This is the whole of the "the furrow is
   the same" complaint, and it is also why the mark appeared *beside* the trail:
   the centre sits 17 to 31 units out to the character's right.
2. **The ground gate asked about a different point than the stamp used.** The
   gate tested the bound's bottom while the mark was drawn elsewhere, so a
   weapon could pass the gate and still stamp a point in midair.
3. **The shape was fixed by hand.** `ShaftLineHalfWidth` (1.1) and
   `ShaftContactLineLength` (12.0) are constants applied to every weapon.

Only (3) is a case for geometry. (1) and (2) are arithmetic on numbers that
already exist - which is why placement was solved before shape, and why the
shape work is layered on top of a placement that already worked.

## Placement: three approaches, and why the walk is the one

The placement question is "where on the ground does this object's mark go".
Three formulations were built; the third is shipped and the first two survive as
settings.

### The axis's lower end

```
low = (c + a*half).z <= (c - a*half).z ? c + a*half : c - a*half
```

This does move the mark off the bound centre, and for an object standing on its
end it lands on the ground.  But the bow's half length is **67.89 units**, and a
weapon carried at an angle has its lower end hanging in the air rather than in
the snow.  Stepping that far along the axis therefore moved the mark **further
out than the character is wide** - the same class of error as the bound centre it
replaced, only further away.  Caught before testing by computing what the step
would be.  Kept as `ContactPoint::LowestEnd`, the last-resort placement when no
ground height is available at all.

### Where the axis crosses the ground

```
p(t)   = c + t * a
p.z(t) = landZ        =>   t = (landZ - c.z) / a.z
|t| <= half           =>   the crossing is on the object
```

This is the placement that needs no distance bound.  Two things follow from
solving rather than stepping:

- **The offset cannot run away**, because the answer is at ground level by
  construction.  It is an output of the ground height, not a guessed distance.
- **"Is it touching at all" stops being a threshold.** A crossing beyond the
  measured half length means the object is too short to reach the ground, which
  is not touching.

What it cannot do is anything that needs the surface at more than one place.  A
single `landZ` is the ground under the **bound centre**, so:

- on a slope the crossing is wrong by the height change across the offset;
- a level axis (`p.z` constant, i.e. a rod held horizontal) has no crossing at
  all and falls back to the centre - so a rod lying across a slope is invisible
  to it;
- it answers "where", not "how much of the object is in".

Kept as `ContactPoint::AxisGroundHit`, the `ShaftSpanFromContact = 0` path.

### The stretch that is under the snow

```
for i in 0..steps:  t = -1 + 2i/steps
                    p = c + t * half * a
                    f = p.z - land(p.x, p.y)
buried = the t where f < 0
length = (refined t1 - refined t0) * half
mark   = the middle of that stretch
```

One walk answers all three of the remaining questions, and answers them from
the ground rather than from a constant:

| Question | Before | Now |
|---|---|---|
| Where does the mark go? | the single crossing = one **end** of the contact | the middle of the buried stretch |
| How long is it? | `ShaftContactLineLength`, 12.0 for every weapon and pose | the buried length itself |
| Is it touching? | `ShaftGroundClearance` on the bound's bottom | does any sample lie under the land |

The two ends are bisected after the walk, so the answer does not depend on how
finely the axis was walked - the walk only has to **find** the patch.  The
residual `|p.z - land|` at each refined end is reported, which is what makes the
claim "the ends are on the surface" checkable rather than asserted.

### The centre line is not where the weapon is

A walk along the axis follows the **centre line** - the dominant mesh's
principal axis - and a line can only cross the ground where the line itself
goes.  A bow carried at an angle has its limbs hanging below that middle, so the
axis stays clear along its whole length while the bow itself is already through
the surface:

```
lowest -10.05 above land  |  ... | place near-mesh t 0.00 | span 0.0 deep 0.00
```

Ten units under, and the walk found no buried stretch.  The shell, not the
centre line, is what meets the snow.

The offset that fixes this is the **axis-to-surface distance at the point in
question**, `ContactPoint::DropAt` / the `lowOffset` handed to `AxisSpan`:
project the object's lowest point onto the axis to get `t`, read the axis height
there (`cz + t * half * az`), and subtract the lowest point's own `z`.  For a
vertical axis there is no such offset - the centre *is* directly above the
lowest point - so it returns zero and the caller's floor of the half thickness
stands.

Two properties matter, and both are asserted:

- **It is a thickness, not a length.**  The distance from the dominant mesh's
  *centre* down to its lowest vertex is **not** this quantity: for a bow carried
  at an angle (`axis z = -0.72`) that distance folded the whole weapon length
  into the number, and a genuine few-unit thickness came out as **52.20**.  A
  four-times-longer rod must leave `DropAt` unchanged; that is the assertion the
  contract exists for.
- **It moves every sample, not just the gate.**  The walk's height function is
  `(z - a_lowOffset) - land` for every sample it takes, so the walk and the gate
  ask about the same surface.  A weapon the centre line already sees is measured
  the same way as one it does not - there is no second, offset-only route, and
  deliberately so: a guard that conditions behaviour on "did the other route
  find nothing" has to be re-read every time the other route changes.

Measured against the recorded session, the fault this removes is visible as a
pair of numbers that should not be equal:

```
drop 52.20  beside  deep 52.20        (a bow, before the fix)
drop  9.14  beside  deep  8.32        (the same bow, after)
```

A depth equal to the drop says nothing about the snow underneath - it is the
drop with zero added to it, because the depth was being measured *and then*
having the offset added back onto a figure already measured from the offset
surface.  That is why turning the drop down looked like it was doing nothing.
The depth is now the submergence on one surface, and nothing is added to it.

### Why a hull step is exact, not approximate

`half * a` looks like a bounding-box corner.  For this case it is exact:
`GetMaximumProjection` is a support function, and a capsule's support function
along its own axis **is** its half length.

## Shape: why the mesh, and how it is read

The collision hull carries a centre line, a thickness and a length, and reading
those properly was enough for placement.  It is not enough for shape, and a hull
cannot answer a shape question even in principle:

| Object | Hull | What is lost |
|---|---|---|
| Bow | capsule + a cylinder for the limbs | the **arc** - a bow and a staff with the same hull leave the same mark |
| Shield | a box or disc | the **face** - the rim and the boss are the parts that would touch |
| Axe, hammer, pick | capsule for the haft | the **head**, which is what actually lands |

The request was for the marks to stop needing per-weapon tuning - there are too
many weapons and their shapes differ.  That is a shape request, and the hull's
own limits are the case *for* walking the vertices, not against it.  Both routes
ship: `StampFromMesh = 1` uses the vertices, `0` is the hull-only behaviour.

The vertices are not a guess and they are not a GPU readback.  The engine keeps
a CPU copy of every skinned mesh because it skins on the CPU:

```
BSGeometry::GetGeometryRuntimeData()  -> BSGraphics::TriShape* rendererData
BSGraphics::TriShape::rawVertexData    -> the same bytes the renderer uploads
BSGraphics::TriShape::vertexDesc       -> the stride and the attribute offsets
```

`BSVisit::TraverseScenegraphGeometries` walks from the collision node's
`sceneObject`, so the mesh measured is the mesh the hull belongs to.

**The dangerous part is the stride.**  A stride that is wrong by four bytes does
not crash - it manufactures plausible floats out of a neighbouring attribute, and
the mark lands somewhere arbitrary with nothing in the log to say so.  Four
candidate layouts are therefore tried (`desc`, `size0`, `pos16`, `off16`) and the
winner is chosen by comparing the fitted mesh against the engine's own
`modelBound`, which is the one independent number available for the same mesh.
The winning layout's name and the error are printed.  A tolerance of
`0.45 * boundRadius + 1.0` is deliberately loose - this catches a wrong stride,
it does not certify a hundredth.

The fit is AABB, then the principal axis by covariance, then a half width per
1/16 of the length.  Only the **dominant** geometry supplies the axis and the
half length - the largest mesh by box diagonal.  A bow's two limbs and its grip
are three geometries; the largest one aims the mark.  The **lowest corner of the
union** is what the gate compares, so a small piece hanging low still counts as
touching.

## Width and length: the two knobs, and the bug that made one dead

The width a carried weapon's mark is drawn at comes from the mark's own length
and an aspect key, floored by the object's measured thickness and ceilinged by
the footprint's own half width:

```
halfWidth = LengthDerivedWidth(halfLength, ShaftMarkFootAspect,
                               floor, FootprintHalfWidth(...))
```

For a 24-unit mark, that is monotone in the key:

| `ShaftMarkFootAspect` | half width |
|---|---|
| 0.50 | 12.0 (a footprint's own width - "fully aligned") |
| 0.35 | 8.4 (narrower, between a sliver and a foot) |
| 0.30 | 7.2 (half a footprint) |
| 0.25 | 6.0 |

**A bug that is worth recording, because no test could see it.**  The mark's
width was originally computed by passing the footprint's half width through the
step/clamp helper as a *floor*:

```cpp
footHalfWidth = LineWidth(12.1485f, 1.0f, 0.0f, 0.0f);   // the buggy form
```

`LineWidth`'s last line is `return scaled < a_max ? scaled : a_max;`.  Passing
`a_max = 0.0f` therefore does **not** mean "no ceiling" - it means "return
zero".  The footprint's half width came back as `0.0`, the alignment's ceiling
fell back to the bare `ShaftLineMaxWidth` (6.0), and every mark was cut to half
the shape it was aligning to.  The visible result: of 113 marks, 109 came out at
exactly half length 24.00, half width 6.00 - every frame stamped the same 4:1
ellipse, so walking laid a row of identical blocks with evenly spaced ridges
between them.

Two things were done about it.  The arithmetic now has a name of its own- 
`ContactPoint::FootprintHalfWidth` - so the mistake cannot be made again by
passing the wrong constant to a generic helper.  And the offline test walks the
**caller's real arguments** rather than a hand-computed stand-in: an assertion
that only calls a pure function with idealised figures passes while the shipped
call site is wrong, which is exactly what happened.

**The length.**  The drawn half length is the buried stretch, scaled by
`ShaftSpanLengthScale`, then converted and capped:

```
drawn = DrawLength(HalfLengthFromStretch(span), scale, ceiling, maxLength, floor)
```

`HalfLengthFromStretch` exists because two quantities called "length" are not
the same quantity.  `Span::length` is `(t1 - t0) * half` with
`p(t) = centre + t * half * axis` and `t` in `[-1, 1]`, so it is the **full**
length along the object; the shader reads `StampShape[i].z` as a **half** axis
(`halfLength = max(s.z, 1e-3)`).  Halving a half length and handing it to a field
that is itself a half length loses a factor of two twice over, and dropping the
conversion entirely doubles the mark and pushes it past the ceiling on every
frame.  The conversion is a named function so it can be asserted; the property
`2 * HalfLengthFromStretch(s) == s` holds.

`ShaftSpanLengthScale` (2.2) widens the measurement on purpose.  The walk counts
only the samples that ended up *below* the land height, so a limb resting on the
surface contributes nothing, and the sampling is discrete, so the two samples
either side of a crossing are up to half a step apart either way.  The
measurement is therefore a lower bound on the footprint rather than the
footprint.  A scale rather than an offset, because the error grows with the
stretch: something barely dipped in touches over a short arc and something laid
down touches over a long one, and the disturbed snow scales with each.

**The shape test.**  A mark is drawn as a line only when it is longer than it is
wide, floored at `max(ShaftLineMinLength, half thickness)`.  The earlier rule
compared the drawn half length against `ShaftStampMaxRadius` - the *ceiling the
disc radius is clamped to*, a number chosen for how fat a foot's mark may be,
doing duty as how short a furrow may be.  Nothing connects the two, and the cost
was a bow's limb 0.79 units under the snow reported as a 4-unit circle.  A stout
object now needs a longer contact to earn a line than a thin one does, which is
deliberate: a 1.76-unit line beside a 4.90 half width is a blob with a
direction.

**The two routes must not share a ceiling.**  The hull route has no measurement
behind it, so it keeps `ShaftContactLineLength` exactly as it was - that constant
is the plank guard it always was.  A measured stretch may draw up to the object's
own length.  The rule lives in `ContactPoint::DrawCeiling` rather than as inline
ternaries in `Clipmap.cpp`, for a reason the reverse-check table records: a
ceiling chosen inside the caller cannot be asserted by a suite that does not link
the caller, so a sabotage that gave both routes the same ceiling survived.

## The rim: giving a groove the snow a footprint gets

A footprint reads as a footprint partly because loose snow is heaped around it.
A carried weapon's rut had the rim turned off - which is the same thing as
saying a weapon's rut could never have those piles.

The rim's *height* was never the problem: a shaft's `stamp.rim` is
`ShaftStampRim` (1.0) times the surface profile's `Rim` (2.0 for snow in
`A_Base/Surfaces.ini`), so the shader's rim branch ran at 2.0 against a
footprint's 2.4.  Its *band* was the problem, and only on an elongated mark.

### A distance is only a distance in the units its shape is

`ClipmapUpdateCS.h`'s `StampDistance` answers in the mark's own units.  For a
circle those are world units; for an ellipse it returns the normalised distance
multiplied back by the half length, so one unit of it measures the **half length
along the long axis** and the **half width along the short one**.  Both are just
a `float` called `d`.

The rim's band was handed to it directly as `halfWidth * rim.x` - a width - so a
band of a given size in those units came out in the world narrower along a
groove's two sides by the whole aspect ratio, and the sides are where the eye is
when it looks at a mark as long as a weapon.  The recorded marks make it
concrete: half length 5.3 to 12.6 against a half width of 2.6 to 2.9, so the
aspect ratio is 1.8 to 4.9, and a band of `halfWidth * 1.5 * (1 + 0.15)` is 4.49
to 5.00 of the mark's units - **6.0 to 6.7 cells** across the ends on a 0.75 unit
grid, against **1.2 to 3.7 cells** along the sides.  On the long thin mark the
line test uses (half length 24.5, half width 1.1, aspect 22) the bank reached
`1.65 * 1.15 * 1.1 / 24.5 = 0.085` of a world unit past the groove, under a tenth
of a cell: no texel of the grid ever fell inside the band, so the groove was cut
with no snow beside it at all.

The fix measures the edge in world units.  `StampOutlineOvershoot` returns the
first-order distance to the outline, `(len - 1) / |grad len|` with
`n = (dot(delta, forward) / halfLength, dot(delta, right) / halfWidth)`.  That is
exact along both axes - `halfLength * (len - 1)` on the long one and
`halfWidth * (len - 1)` on the short one - and it is identically
`StampDistance - s.z` for a circle, so every round mark keeps its rim, its lip
band and its falloff unchanged.

`StampShape[i].z` is the half **width**, zero for a circle, so it is exactly the
reference that was missing:

```hlsl
const float bandRef = StampShape[i].z > 0.0f ? StampShape[i].z : s.z;
const float span = max(bandRef * rim.x, 2.0f * Window.z);
```

The zero cannot be stale: `params` is value-initialised (`ParamsCB params{}`),
`stampShape[i][2]` is written every frame for every stamp that is uploaded, and
the vector it is filled from is a fresh local per frame with an explicit
`stamp.halfWidth = 0.0f` on the non-line path.  A circle therefore cannot
inherit a previous frame's line width.

### A band narrower than the cell it is written to is not a band

The floor - `2.0f * Window.z`, two cells - is why the band is held to the size of
the grid it is written to.  A band narrower than the grid cannot be drawn at
all, because the texels either side of the outline miss it; and the coarse
level's cell is 3.0 rather than 0.75, so a floor that follows the cell is also
what stops that level writing a bank it cannot resolve.  At level 0 the floor
does not bind for a shipped weapon - the line's half width is floored at 1.1 and
`1.1 * 1.5 = 1.65` against a floor of 1.5 - so it is the coarse level and very
thin marks it is there for.

No setting changes with this and none is added: `ShaftStampRim` still scales the
rim's height, the profile's `SnowStampRimSpan` still scales its width (and a
footprint's with it), and `ShaftStampRim = 0` still removes the ridge.

### The rim's own edge is no longer smooth

A mark aligned to a footprint's shape is an ellipse with a well-defined outline,
and a row of identical ellipses reads as a plank rather than as disturbed snow.
The region a line mark's rim is raised in is therefore jittered, by sampling a
value noise at the **world position** so that overlapping stamps agree about
where the edge is and the bank does not flicker:

```hlsl
float2 rimWorld = worldXY + StampNoise[i].y * 17.0f;
float  jitter   = RimEdgeJitter(rimWorld, MarkRimJitter.x, MarkRimJitter.y);
float  bandScale = 1.0f + jitter - 0.25f * MarkRimJitter.x;
```

**The gate is what keeps footprints untouched.**

```hlsl
if (StampShape[i].z > 0.0f && MarkRimJitter.x > 0.0f)
```

`StampShape[i].z` is the half width, non-zero only for a line, so every circle- 
every footprint, every fireball - takes the branch it always did, bit for bit.
The second term is an escape hatch: `ShaftMarkRimJitter = 0` restores the smooth
edge **exactly**, not approximately.  That is provable from the expression rather
than measured: with `amount = 0` the jitter term is zero and `bandScale` is
exactly `1.0`, which leaves `u = gap / reach` as it was.

The `- 0.25 * amount` bias is deliberate.  The noise is symmetric about zero, so
without it the band would narrow as often as it widened and a narrow band can
fall inside the outline and leave a gap in the bank.  Biasing outward means the
edge only ever moves away from the groove.

## The log: what it is for, and the budget that silenced it

A diagnostic line is evidence only if it can still be written when the thing it
describes goes wrong.  The shaft probes originally drew on a one-off budget, and
a 113-second run spent every one of them before it had finished loading:

| line | budget | last written |
|---|---|---|
| `Shaft stamp` | 24 | 23:09:56.940 |
| `Shaft gate` | 12 | 23:09:52.664 |
| `Shaft mesh` | 32 | 23:10:13.771 |
| `Shaft mesh: not measured` | 8 | never fired |

The last shaft line of any kind was written eleven seconds in, and the ninety
seconds after it held nothing.  Those silent frames are exactly the ones a player
is in when they notice the furrow has stopped - and from the outside a silent
frame that was fine is indistinguishable from a silent frame that was not.
`GatherStamps` already carried a note about the same fault being fixed once
before, for the collidable probe - *"The first run burned its whole allowance in
four frames and never saw the frames that had the bow drawn"* - but the four
shaft budgets had never been given that treatment.

Two changes:

* **A rate limit replaces the budget.**  `LogBudget::Allow` passes a line once
  400 ms have gone by since the last of its kind, and a refusal does **not**
  move the timestamp it was refused against.  That second half is the whole
  rule: a refusal that took the new timestamp would let a line called every
  frame push its own deadline forward on every frame and never be written
  again, which is the one-off budget wearing a different hat.  Each of the
  four lines has its own clock, for the reason the four budgets were separate.
* **A window summarises what the lines cannot.**  A weapon is judged tens of
  times a second and only a sample of those judgements can be written, so the
  refusal that explains a gap in the marks is usually not the one that happened
  to be logged.  `LogBudget::ShaftWindow` counts every decision and keeps the
  closest a refusal came to the limit; `ReportShaftTally` writes it every five
  seconds:

```
Shaft tally: 5s | seen N | judged N | marked N (line N, disc N) | refused N carried-clear |
  clearance over refusals N.NN best, N.NN worst (limit 2.00) | <verdict>
```

  The verdict is the point of the line.  A refusal that sits near the limit
  means the limit is what to move; a refusal far above it means the weapon was
  never near the snow and no limit would have helped.  Those two readings call
  for opposite fixes, and a set of refusal lines that carry no clearance figure
  cannot be told apart.

  `ShaftWindow::GateIsBinding` puts that line at three times the limit.  It is
  not a physical constant, but it is falsifiable: easing the limit to the
  clearance seen would have to more than triple it to change anything, so a
  refusal inside three times is the gate's doing and one outside is the
  carry's.  With no refusal sampled there is nothing to blame the gate for.

  The counts are nested - `seen`, then `judged`, then `marked` and `refused`- 
  so that a window in which a weapon was carried and produced nothing still
  says which stage it stopped at, instead of looking the same as a window in
  which nothing was carried at all.

## What shipped

| Piece | Where | What it does |
|---|---|---|
| `ContactPoint::LowestEnd` | `src/ContactPoint.h` | The axis's lower end. Last-resort placement when no ground height is available. |
| `ContactPoint::AxisGroundHit` | `src/ContactPoint.h` | Solves for the crossing. Kept as the `ShaftSpanFromContact = 0` path. |
| `ContactPoint::AxisSpan` | `src/ContactPoint.h` | Walks the axis, returns the buried stretch, its middle, its length, its depth, and the gap when nothing is buried. The primary path. |
| `ContactPoint::DropAt` | `src/MeshShape.h` | The axis-to-surface distance at a point - a thickness, not a length. |
| `ContactPoint::HalfLengthFromStretch` | `src/ContactPoint.h` | Converts the walk's full length to the shader's half axis. |
| `ContactPoint::FootprintHalfWidth` | `src/ContactPoint.h` | A footprint's half width, by name, so it cannot be passed through a generic clamp. |
| `ContactPoint::LengthDerivedWidth` | `src/ContactPoint.h` | The mark's half width from its own length and aspect, with the footprint as a ceiling rather than a floor. |
| `ContactPoint::AlignedWidthCeiling` | `src/ContactPoint.h` | Resolves the ceiling rather than borrowing a constant. |
| `ContactPoint::AspectCappedLength` | `src/ContactPoint.h` | Caps the length against the width, which stops a long thin mark stacking into a rake. |
| `ContactPoint::DrawCeiling` | `src/ContactPoint.h` | The measured route may draw up to the object's length; the hull route keeps the constant. |
| `ContactPoint::LineWidth` | `src/ContactPoint.h` | The drawn half width from a measured thickness. Note its `a_max` is a ceiling, not "no ceiling". |
| `MeshShape::Fit` | `src/MeshShape.h` | AABB, then the principal axis by covariance, then a half width per 1/16 of the length. Pure arithmetic, no engine types. |
| `MeshShape::Place` | `src/MeshShape.h` | Turns the fitted box into the world: lowest corner, world axis, world half length. Transforms **eight corners**, not every vertex. |
| `MeshShape::WidthOver` | `src/MeshShape.h` | The half width over a stretch of the object, weighted by how much of each band the stretch covers. |
| `MeshShape::BoundAgrees` | `src/MeshShape.h` | The stride check, against the engine's own `modelBound`. |
| `MeshGeometry::Measure` | `src/MeshGeometry.h/.cpp` | Walks the node's geometries, reads `rawVertexData` at the stride the winning layout gives, caches per geometry, and reports what it found. |
| `StampDistance` / `StampOutlineOvershoot` / `RimEdgeJitter` | `src/ClipmapUpdateCS.h` | The shader's distance, its world-unit outline distance, and the rim edge's noise. |
| Placement, gate, length, width | `Clipmap.cpp` `GatherStamps` | `place span`, else `near`, else `hit`, else `end`, else `centre`. |

All the pure arithmetic lives in headers with **no engine types in them** and
takes the land as a callable, so the offline test calls the same code the plugin
calls rather than a copy.  A reimplementation in the test would pass while the
shipped code was wrong.

## The settings

| Key | Default | Meaning |
|---|---|---|
| `ShaftFollowPose` | 1 | Place and aim the mark from the live pose. `0` restores the bound-centre placement exactly as it was. |
| `ShaftStampAtContact` | 1 | Place the mark at the derived point rather than at the bound centre. |
| `ShaftGateAtContact` | 1 | Take the fallback's ground gate at the derived point rather than at the bound's bottom. |
| `ShaftSpanFromContact` | 1 | Derive length and position from the buried stretch. `0` restores the single-crossing placement. |
| `ShaftSpanMaxGap` | 2.0 | How far the hull may be above the snow and still mark. `0` = strictly on contact. **This is the knob that decides whether a carried weapon marks at all.** |
| `ShaftSpanSteps` | 16 | How finely the axis is walked. Does not affect the measured ends. |
| `ShaftContactLineLength` | 12.0 | On the hull route, the hard cap. On the measured route, only the *floor* of the ceiling, so a long object may draw past it. |
| `ShaftSpanLengthScale` | 2.2 | How much longer than the buried stretch the trail is drawn. `1.0` draws exactly the measurement. **This is the knob for "the trail is too short".** |
| `ShaftLineMinLength` | 1.6 | Shortest half length worth drawing as a line. The effective floor is `max` of this and the object's own half thickness. |
| `ShaftLineWidthScale` / `MinWidth` / `MaxWidth` | 1.0 / 1.1 / 6.0 | Multiplier, floor and ceiling on the measured thickness - and on the mesh's half width when one was measured. |
| `ShaftMarkAlignToFoot` | 1 | Derive the mark's width from its own length, aligned to a footprint's shape. `0` restores the thickness-derived width and the uncapped length. |
| `ShaftMarkMaxAspect` | 4.0 | Ceiling on the width this alignment may ask for. A footprint itself sits at 2.0, and 64 is effectively no cap. |
| `ShaftMarkFootAspect` | 0.35 | The aspect the mark is drawn at when it is aligned. **This is the width knob.** |
| `ShaftMarkRimJitter` | 0.35 | How far a line mark's rim edge wanders, as a fraction of the band. `0` restores the smooth edge exactly. |
| `ShaftMarkRimJitterBand` | 6.0 | The wavelength of that wander, in world units. |
| `ShaftStampMinDepth` | 1.0 | Floor on the depth scale. The depth is otherwise derived from the mark's own width against `ShaftStampFootRadius`, which is right for a boot and wrong for a weapon. |
| `ShaftStampRim` | 1.0 | Whether a weapon's rut gets the loose snow a footprint gets. `0` removes the ridge entirely. |
| `ShaftLowOffset` | 1 | Whether the walk is told the weapon's own drop below its centre line. `0` restores the centre-line walk. |
| `StampFromMesh` | 1 | Shape the mark from the node's own vertices instead of the collision hull. `0` restores the hull-only behaviour. |

Everything except `ShaftFollowPose` is printed at startup on one `Carried
weapon:` line, next to the existing `Surfaces:` block, because a number typed
into the ini and a number the plugin is using are two different things and the
log has to say which one is in force.

### Two gates, and the difference between them

`ShaftGroundClearance` (2.0) is still in the path ahead of the span gate, and it
asks about the **bound's bottom** while the span gate asks about the **hull**.
The bound sphere contains the whole object, so its bottom is at or below the
object's lowest real point: the old gate is the looser of the two, and the span
gate is the one that decides.  Set `ShaftGroundClearance = 0` to take it out of
the path entirely and leave the geometry to speak alone.

## What is still a constant

- `ShaftStampRim` (1.0) is a policy: whether a weapon's rut gets the loose snow a
  footprint gets.  It could be 0 only while the rim's *band* was being read off
  the mark's reach - tens of units for a line - so any rim would have raised a
  bank as wide as the weapon is long.  With the band taken from the mark's half
  width instead, the piles hug the groove and the key can be on.  It scales
  `response.rimScale`, so the surface profile still has the last word on how much
  snow there is.
- `ShaftStampMinDepth` (1.0) is the floor over a depth derived from the mark's own
  width against `ShaftStampFootRadius`, which is right for a boot and wrong for a
  weapon: a 2-unit-wide mark out of an 18.69-unit foot came out at `x0.104`, so
  the rut was a scratch under a full-height ridge.  A weapon's contact patch is
  far smaller than a boot's and presses harder, so it should sink **at least** as
  far.  At 1.0 the width-derived depth is superseded and a carried weapon digs to
  a foot's depth; at 0 the depth behaves as it did before this key existed.
- `ShaftLowOffset` (1) decides whether the walk is told the weapon's own drop
  below its centre line.  The drop itself is measured - from the mesh where there
  is one, from the hull thickness otherwise - so this only decides whether it is
  used.
- `ShaftContactLineLength` (12.0) is a **floor for the ceiling**, not the source
  of the length.  The length comes from the ground; this is the smallest ceiling
  allowed, and the ceiling actually applied is the larger of it and the weapon's
  own length.  **On the hull route it is still the hard cap** - that route has no
  measurement behind it, so the constant stays the plank guard it always was, and
  the rule is `ContactPoint::DrawCeiling` rather than a pair of inline ternaries.
- `ShaftSpanLengthScale` (2.2) is the one place a judgement enters the *measured*
  length: the walk reports the strictly-buried stretch, and the trail is drawn
  longer than that on purpose.  It is a scale rather than an offset because the
  quantity it corrects - the samples either side of a crossing, and the snow
  pushed aside without being sunk into - grows with the stretch.
- `ShaftLineMinLength` (1.6) is the absolute floor on a drawable half length,
  used together with the object's own half thickness.  Below it the disc is kept,
  because a line shorter than it is wide reads as a dot with a direction.
- `ShaftSpanMaxGap` (2.0) is the last judgement call in the path.  It is not a
  geometric quantity, it is a policy: how far above the snow a carried object
  may be and still be considered to have brushed it.  The log prints the
  measured gap so the number can be set from data rather than by taste.
- The mesh's cross-section is a **half width per 1/16 of its length**, not a
  profile.  A blade that is wide in its last tenth and thin elsewhere gets a
  width from the band it falls in, so the taper is followed but a very local
  flare is averaged away.  `kSlabs = 16` in `MeshShape.h` is the resolution.
- Vertices above `kMaxVertices` (32768) are sampled by stride rather than read
  whole, and at most `kMaxMeshes` (8) geometries per node are walked.  Both are
  frame-budget guards, both are counted in the log's `verts` field.

## Test

`StampSurfaceTest` is the offline harness.  It links the same headers the plugin
does and calls the same functions with a land height it controls, so it can
assert the arithmetic rather than a reimplementation of it.  The suite reports
**32** `PASS` lines.

The groups, in the order they run:

1. **A rod standing in the ground** - centre ten units up, thirty-unit reach, so
   twenty units are buried.  Length, both crossings, depth, the mark's height
   and the residual are each asserted against arithmetic on the input.
2. **Carried clear is not touching** - a rod hanging ten units above the ground
   must report no contact and a gap of ten.  This is the assertion the reverse
   check has to break.
3. **A touch with no length** - the lower end exactly on the surface has no
   buried stretch and no gap, and the closest approach must be that end rather
   than the centre, or the relaxed gate would place the mark in the air.
4. **A rod laid across a slope** - buried over its outer half, ten units of
   contact with its middle fifteen units along.  This is the case the
   single-crossing placement could not see at all: a level axis has no
   `p.z(t) = landZ` solution with one land height, so it returned the centre- 
   fifteen units from where the rod is.  The slope makes the walk necessary; a
   level axis would have hidden the bug.
5. **Deeper in the snow draws longer** - the same rod at two heights must give
   two different buried lengths, more than four units apart.  **This is the
   assertion a length constant cannot pass**, and without it every other
   assertion here would pass on an implementation that returned a fixed length.
6. **Degenerate inputs** - zero half length, zero axis, an unreadable land, and
   a fully buried object.  The last one must report `resid < 0`: no end ran off
   the object, so there is no crossing to check and the field must decline to
   claim one rather than report a checked zero.
7. **The hull is what touches** - a 3-unit clearance with a 5-unit hull is
   negative; a thin hull is positive; an unmeasurable gap is not a contact.
8. **`PASS mesh shape:`** - a straight 34-unit rod with a 2-unit radius comes
   out with its axis along its length (`|ax| > 0.99`, unit length), its half
   length within 0.5, its half width within 0.2 of the ring radius, and its
   width bands **flat** (max/min `< 1.05`, so a uniform object is not handed a
   fake taper).  A taper built from 6.0 at one end to 0.6 at the other must show
   a band ratio `> 4.0` (the data's own ratio is 10x; the assertion is set below
   it because the bands average over 1/16 of the length) - and the axis sign
   must be settled by the dominant component (`ax > 0.99`), without which
   "which end is band 0" is a coin toss.  `WidthOver` must return three
   different answers over the wide end, the narrow end and the whole object
   (`wide > narrow * 3`), and a stretch asked for **past** the end must come
   back as the end band to within 0.01 rather than being extrapolated.  An arc- 
   48 stations on a 40-unit radius, 60 degrees each way, limbs 0.5 thick - must
   have a box more than twice its own limb thickness and bands that are not
   flat (`> 1.4`); that is the property a capsule cannot have, and it is the
   reason the request could not be answered by the hull.  NaN, an infinity and
   a two-vertex "mesh" are each refused.
9. **`PASS mesh place:`** - the lowest corner is exact under identity in all
   three coordinates (not merely at `minZ`), a quarter turn about Z swaps the
   box's extents and leaves the height alone, a 45-degree tilt about Y moves the
   lowest corner to `(minZ - maxX) * s` and carries the long axis to the tilt of
   the *local* axis (compared against the fit's own sign, since the transform is
   what is being tested, not the convention), and a transform that reports
   failure is refused rather than half-applied.  `BoundAgrees` is checked four
   ways: a box against its own sphere (error `< 0.01`), a sphere three radii
   away (must refuse), a zero radius (must refuse - otherwise any box passes),
   and a NaN radius (must refuse).
10. **`PASS axis span`'s offset groups** (three of them, folded into
    `CheckAxisSpan`):
    - **A weapon whose centre line clears the snow but whose own end does not.**
      A level line three units above flat ground with a 3.5-unit drop: the line
      touches nothing, the weapon's own end is half a unit down.  This must
      report a contact, a whole-length stretch, a depth of 0.5 (the submergence
      on one surface - *not* the drop) and a residual of `-1`, because a level
      contact has no crossing to bisect.  **This is the assertion that fails
      when the offset is not applied**, which is what makes it worth having.
    - **A surface that really crosses the weapon.**  A hill peaking under the
      middle, with the centre line held clear of the peak: only the offset can
      bring the line into it.  The stretch must be about 22.9 units and the
      residual must come out at zero, because both crossings are interior and
      both are bisected.  Note the geometry: the earlier attempts at this case
      used **valleys** and both reported the whole object - a valley is deepest
      under the middle, so the weapon is under the ground at its ends and clear
      in the middle, which puts both crossings on the weapon's own ends with
      nothing outside them to bisect against.  The geometry is written out in
      the test so the next reader does not have to rediscover it.
    - **A stretch running off the end of the weapon.**  A vertical axis crossing
      the surface on its own, with one end buried and no crossing to find: the
      walk must measure to the weapon's own end rather than shrinking the
      stretch to the middle.  This is what the `t0 = -1` default exists for.

    Three guards were written to separate that last case from ground that is
    simply lower than the walk was told - one on how many samples fell under,
    one on where the crossings landed, one on the whole object's length - and
    **all three rejected the contact they existed to find.**  On ground that is
    locally level the two cases are numerically identical, so the separation is
    not attempted in the walk at all; the walk reports and the caller decides,
    which is the right split because only the caller knows whether its land
    lookup can be trusted.  See the block comment in `CheckAxisSpan` for the
    full account.

    Also here: **a rod with a ten-unit drop**, chosen so that one end is buried
    and one end is a real crossing - the shape the group is about.  Its length
    is pinned to `38.0` rather than to a range, which is what makes it able to
    tell the two designs apart (`28.0` on the centre-line walk).

11. **`PASS draw length`** - `CheckDrawLength`, over the pure length and width
    functions.  The bow's numbers come from the log (67.89 long, 4.90 half
    thickness, and the recorded spans), so the assertions are against the
    session that prompted the change rather than against invented figures.

    - **The scale reaches the length, and is a multiplier at all.**  At scale
      `1.0` a 20-unit stretch must come out at exactly 20; at `2.2` a 10-unit
      stretch must come out at 22.  The first is the one that stops a constant
      passing by coincidence.
    - **It grows with the measurement.**  Two stretches a factor of four apart
      must differ by more than a factor of three.  **This is the assertion a
      length constant cannot pass.**
    - **The floor follows the object.**  A 0.8-unit contact scaled to 1.76 must
      stay a disc on a bow (half width 4.90) and must *become a line* on a twig
      (half width 0.3).  Identical measurement, identical scale, opposite
      outcomes - which is the pair that proves the floor is not a constant.  The
      bow case is the honest one: 1.76 beside 4.90 really is a dot with a
      direction, and drawing it as a line would be worse than the dot.
    - **The ceilings hold.**  A 900-unit measurement must land on
      `ShaftLineMaxLength`, not lay a plank.
    - **The two routes must not share a ceiling.**  `DrawCeiling` is asserted
      directly: the hull route must return the constant, the measured route the
      object's own length, and a *short* object must still be capped by the
      constant.  Asserting this on `DrawCeiling` rather than on `DrawLength` is
      deliberate - see the reverse-check table.
    - **The width knob moves.**  `LengthDerivedWidth` must be monotone in the
      aspect key across the range the ini exposes, and the *caller's own
      arguments* are used rather than idealised ones.
    - **The bug cannot come back.**  `FootprintHalfWidth` is called with the
      caller's constants, and the assertion that separates it from the old
      clamping route is that `LineWidth` with a zero ceiling still returns zero.
      If a future edit makes the clamping route agree with the named one, the
      assertion fails and says the comment explaining the bug is now wrong- 
      which is the point: the test is also the guard on the explanation.
    - **A non-finite measurement is not a contact.**

12. **`PASS drop`** - `CheckDropAt`, six geometries over the axis-to-surface
    distance.  The numbers come from the log's bow: an object whose own centre
    sits far above its lowest vertex when it is tilted.

    - **The drop is a thickness, and it is measured at the point.**  A tilted rod
      whose axis point is 90 units up with its lowest vertex at 88 must report
      2.0, and the returned `t` must be `-1`.  The same geometry is arranged so
      that the centre-to-vertex distance is 10 units: the assertion therefore
      fails on the old `cz - lowZ` formula **and** on any formula that returns
      the centre's number, which is the pair that makes it a test rather than a
      restatement.
    - **A four times longer rod does not change it.**  Same tilt, same thickness,
      four times the length: still 2.0.  **This is the assertion the contract
      exists for** - the whole fault was length leaking into a thickness.
    - **A vertex under the middle** reports `t = 0` and its own drop.
    - **A vertex past the end** clamps to `t = -1` and reports the drop measured
      at the end, not at the vertex.
    - **A vertical axis reports zero.**  The centre is directly above the lowest
      point, so there is no offset to hand the walk; the caller's floor of the
      half thickness is the right answer there, and returning the centre-to-vertex
      distance for a vertical object would double-count the length again.
    - **Non-finite input reports zero** rather than propagating.

13. **`PASS shader rules`** - `CheckShaderStampRules`, which asserts on the
    **shader source text** that `Clipmap::UpdateShaderSource()` produces.  This
    is a different kind of test from the rest of the suite: the rim's band width
    and the edge jitter are not in pure functions, they are in the emitted HLSL,
    and the band width is what decides whether a groove can have snow heaped on
    it at all.

    - `StampShape[i].z > 0.0f ? StampShape[i].z : s.z` must appear **exactly
      once**, so circles keep taking `s.z`.
    - `s.z * rim.x` must appear **zero** times - the old band, which was a reach.
    - The radial-bulge term must still appear once, so the assertion cannot pass
      on a source that was truncated.
    - The jitter gate must appear, and the escape hatch must be a gate on the
      amount rather than a hard constant, so `ShaftMarkRimJitter = 0` really
      does restore the smooth edge.

14. **`PASS rim band`** - `CheckRimBand` dispatches the shipping shader twice
    for the same mark, once with its rim and once without, and reads the
    difference - so the groove, the blanket standing over it and the churned
    floor all cancel and what is left is the bank alone.  It is a behavioural
    check of the rim's geometry, not a source rule: its first assertion is how
    many cells of bank a line has along its side against how many it has across
    its end, and it fails on the old arithmetic with *"A line mark's bank is far
    narrower along its side than across its end"*.  It runs before
    `CheckShaderStampRules` deliberately, so that a change to the rim's geometry
    is reported as the thing it breaks rather than as a line of text that moved.

15. **`PASS log budget`** - the rate limiter's own behaviour, including the
    property that a refusal does not advance the clock it was refused against.

### Reverse verification

Every assertion in this suite was checked by breaking the code it covers and
confirming the suite fails **by name**.  A sabotage that passes is worth more
than one that fails: it is the only way to find out which half of a change is
not actually covered.

| Fix sabotaged | Changed back to | Result |
|---|---|---|
| `DropAt` measures at the point | `lineZ = a_cz` (the centre's height) | 1 FAIL, *"The drop is not the object's thickness at the point"* |
| `HalfLengthFromStretch` | `return a_stretch;` | 1 FAIL, *"The buried stretch was not halved"* |
| the rim's band reference | `const float span = max(s.z * rim.x, 1e-4f);` | 1 FAIL, *"The stamp shader no longer takes the rim's band from the mark's half width"* |
| the walk's reference surface | `a_f = (z - a_lowOffset) - land;` → `a_f = z - land;` | 1 FAIL, *"A shape whose own lowest point was in the snow reported no contact - the walk is still asking about the centre line"* |
| the depth's reference surface | `out.depth = deepest;` → `out.depth = deepest + a_lowOffset;` | 1 FAIL, *"The depth is not how far the object's own lowest surface went under…"* |
| the rim's band, its reference | `StampOutlineOvershoot(...)` → `d - s.z` | 1 FAIL, *"A line mark's bank is far narrower along its side than across its end"* |
| the rim's band, its floor | `max(bandRef * rim.x, 2.0f * Window.z)` → `bandRef * rim.x` | 1 FAIL, *"A mark thinner than the grid threw up no snow along its side either…"* |
| the rim's band, its floor's **value** | `2.0f * Window.z` → `1.0f * Window.z` | 1 FAIL, but **from the source rule only** - see below |
| the alignment switch | `if (Settings::shaftMarkAlignToFoot)` → `if (false)` | **survived both the behavioural and the byte-level checks at first** - see below |
| the walk's whole-body report | `AxisSpan` reports the whole object buried | 1 FAIL, *"A rod hanging ten units clear of the ground reported a contact - the touch rule is not geometric"* |
| `WidthOver` returns the overall width | one number for the whole weapon | 1 FAIL, *"The width over a stretch does not follow the bands"* |
| the length scale | `drawn = a_measured * a_scale` → `drawn = a_measured` | 1 FAIL, *"The length scale does not reach the drawn length"* |
| the borrowed floor | `if (!(drawn > a_minLength))` → `if (!(drawn > 4.0f))` | 1 FAIL, *"A thin object's short contact was refused - the floor is not following the object's own width"* |
| the ceiling rule, while it lived in `Clipmap.cpp` | both routes given the same ceiling | **BUILD_EXIT=0, ST_EXIT=0 - the suite did not notice** |
| the same rule, after moving to `DrawCeiling` | the same sabotage | 1 FAIL, *"The measured route's ceiling is still the constant - a long object is capped at a short one's stub"* |

**Three sabotages are worth keeping as warnings.**

*The ceiling rule that survived.*  While it lived as two lines inside
`Clipmap.cpp`, a sabotage that gave both routes the same ceiling was **not
caught** - the test does not link the caller, so it can only assert
`DrawLength`'s behaviour given a ceiling, never which ceiling was passed.  That
is precisely the "the copy passes while the real path is wrong" failure this
codebase has been bitten by before, and the fix is structural: the rule moved
into the header where it can be asserted.

*The alignment switch.*  Replacing the condition with a literal `false` left the
suite green, because the calls it guards are still in the file and therefore
still satisfy the source pins - and `Clipmap.cpp` is not linked, so no
assertion could see it either.  What closed it was pinning the **switch itself
and the else-arm**, not just the bodies: a branch that is never taken on the
built configuration is not covered by pinning what it contains.

*The floor's value.*  Weakening the two-cell floor to one left the behavioural
group **passing** - a one-cell floor still leaves a cell of bank, so "a floor
exists" is all the geometry can say.  The exact number is covered by the source
rule and by nothing else, and that is now written down rather than assumed.

`Require` stops the run at the first failure, so a sabotage reporting one
failure is the expected shape: the failures after it would all be the same
cause.  Every sabotage was restored from a copy taken immediately before the
edit and the file came back byte-identical.

**What this does not cover.**  The `DropAt` and `HalfLengthFromStretch` call
sites are in `Clipmap.cpp`, which the offline tests do not link; only the
functions themselves are covered.  Their call sites are therefore argued for
from the log rather than asserted, and the log lines (`drop ... of radial ...`
and the `half length` column) are what to read to see whether they were applied.

## Verification

| Step | Result |
|---|---|
| `cmake --build` | `BUILD_EXIT=0`, no `error C`, no `warning C` |
| `ShaderGen.exe` | `ALL PASS (0 failures)` |
| `StampSurfaceTest.exe` | `ALL STAMP SURFACE TESTS PASS`, **32** PASS lines, 0 FAIL |
| md5, build output vs deployed | identical, 1273856 bytes |
| Timestamps | every `src/**` file older than `NMN_DeformableTerrain.dll`, and `StampSurfaceTest.exe` newer than both `ClipmapUpdateCS.h` and `StampSurfaceTest.cpp` - so the passing suite is the edited suite, not the previous one.  The dll is deleted before a final build whenever one before it compiled a sabotage; without that, `ninja` would be free to leave the sabotaged link in place and the "rebuilt" claim would be empty. |
| Embedded build tag | present once.  The revision-scoped tags it replaced are absent, which is what the scan pins |
| Embedded shader strings | `StampOutlineOvershoot` ×4 - the shader body is embedded twice and each copy holds the definition plus the single call, so the pin is 4 and it was **measured on the built image before being written down**; `(len - 1.0f) / invGrad` ×2; `max(bandRef * rim.x, 2.0f * Window.z)` ×2; `d - s.z` ×0 |
| Pins that read 2, not 1 | `groupshared` → 2, `place {} {} \| ` → 2, `rim {:.2f}` → 2: the update shader's body is embedded **twice**, and has been in every build measured, across forty-odd archives. |
| Pin that was wrong first time | `t {:+.2f}` reads 2, not 1 - and the second match is `highes**t {:+.2f}** world units` in the shape census, a different literal that happens to end in the same characters. Pinning it with its NUL terminator gives 1. |
| Pins deliberately not used | anything naming an `inline` function (`ContactReach`, `HeightAboveSnow`, `DropAt`, `HalfLengthFromStretch`, `Allow`, `GateIsBinding`, `WidestLimit`, `TightestLimit`): inlined or dropped, so a pin on one is a claim that cannot fail. Their behaviour is covered by the `drop`, `shader rules` and `log budget` test groups instead. |
| ini | comments only, both the shipped template and the deployed copy.  No key is added and no value changes. |
| Probe residue | `grep -c "PROBE"` → 0 in every source file. |
| Known gap | `ReportShaftTally` stays silent when its window is empty, so a five-second stretch with no shaft candidate leaves no line at all. That is deliberate - an idle window would otherwise print every five seconds - but it does mean "no line" and "no weapon near the snow" read the same. |
| Known gap | The band's floor is **two** cells, and only the source rule pins the value: weakening it to one cell leaves the behavioural group passing, because a one-cell floor still leaves a cell of bank. What the geometry proves is that a floor exists; the number is text. |

## What the log shows

```
Carried weapon: FollowPose=... StampAtContact=... GateAtContact=... SpanFromContact=...
  SpanMaxGap=... SpanSteps=... ContactLineLength=... LowOffset=...
  SpanLengthScale=... LineMinLength=... StampRim=... StampMinDepth=... FromMesh=...
  MarkAlignToFoot=... MarkMaxAspect=... MarkFootAspect=...
  MarkRimJitter=... MarkRimJitterBand=...

Shaft gate: carried clear of the snow - surface N.NN above it against a limit of N.NN,
  so no mark (axis N.NN, half thickness N.NN, half length N.NN, N steps) |
  mesh yes|no lowest corner N.NN above it

Shaft mesh: accepted | layout desc err 0.03 | meshes 1 verts 1420 cached 0 |
  bound r 31.20 | union 40.1 vs hull 59.7 | skin no |
  lowest corner above by 0.42 (drop 3.10 of radial 2.04, axis z -0.72) |
  bands 0.61..2.04 over t -0.38..0.44 x1.000 -> width 2.04

Shaft mesh: not measured (no geometry, no CPU-side vertices, or no layout agreed) -
  the collision hull's numbers are in use

Shaft stamp: LINE half length ... half width ... (hull length ..., hull thickness ...) |
  depth x... rim ... | at (x, y) N.N from bound centre |
  lowest N.NN above land (limit 2.00) |
  axis mesh|live|centre | place span|near-mesh|near|hit|end|centre t N.NN|t - |
  span N.N deep N.NN gap N.NN resid N.NN

Shaft tally: 5s | seen N | judged N | marked N (line N, disc N) | refused N carried-clear |
  clearance over refusals N.NN best, N.NN worst (limit 2.00) | <verdict>
```

| Field | What to check |
|---|---|
| `Carried weapon: ... FromMesh=1` | The ini key was read. If this reads `false`, the mesh path is off and the run says nothing about it. |
| `Carried weapon: ... StampRim=1.00 StampMinDepth=1.00` | `StampRim=0.00` means the groove cannot have snow heaped on it; `StampMinDepth=1.00` means the depth is not being scaled down by the mark's narrowness. Both being 1.00 is what makes a rut read like a footprint. |
| `MarkFootAspect` / `MarkRimJitter` | The two knobs for the shape of a weapon's mark. `MarkFootAspect` sets the width; `MarkRimJitter=0` is the documented escape hatch back to a smooth rim edge. |
| `depth x0.10` in `Shaft stamp:` | **The other field to read when a rut looks like a scratch.** It is what the depth is multiplied by; `x1.00` is a foot's full depth. |
| `Shaft mesh: accepted` / `refused: ...` / `not measured (...)` | Which of the three happened. `accepted` is the mesh path; the other two mean the hull route took over. The reason is in the brackets - `skinned mesh` and `union box far larger than the hull` are refusals by design, not faults. |
| `layout desc` | Which byte layout won the vote. `desc` is the documented one; a different winner means the documented offsets were wrong for this mesh, which is worth knowing. |
| `err 0.03` | The mesh-vs-`modelBound` error, against the tolerance `0.45 * bound r + 1.0`. Small means the stride is right. A large one means the vertices were read wrong and the mark was shaped from garbage. |
| `union 40.1 vs hull 59.7` | The union box's three half extents summed, against the hull's length plus thickness. The refusal fires when the first exceeds `6 * (second + 1)`. |
| `bands 0.61..2.04 over t -0.38..0.44 x1.000 -> width 2.04` | The mesh's width across the buried stretch, the stretch itself in mesh parameter space, the mesh-to-world scale, and the half width that came out. For a bow the two band figures should differ; for a rod they should not. |
| `place span` | The buried stretch was found and the mark is in its middle. The intended path. |
| `drop N.NN of radial N.NN, axis z N.NN` | **The field to read first when a weapon is clearly in the snow and there is no mark.** It is how far the object's own lowest point hangs *below the axis at that point* - a thickness - and the `radial` figure beside it is the half width the bands measured, so the drop should be of that order and not larger. `axis z` is the axis's vertical component: near zero means the object is horizontal and the drop is a pure thickness; large negative means it is tilted, which is exactly the case the old `centre - lowest vertex` formula inflated to 52-64. A drop of tens of units on a thin object is a fault, not a big weapon. |
| `span N.N deep N.NN` | The buried length and the deepest sample. `span` should be smaller than the weapon and should change with the terrain, not stay at one number. **`deep` must not equal `drop`**: a depth equal to the drop is the drop with zero added to it, not a measurement. |
| `rim N.NN` | Whether the mark was given the raised snow a footprint gets. It is `stamp.rim`, the number the shader reads; `0.00` means the groove cannot have anything heaped along it however the profile is set, because the shader only raises a rim when `p.w` is non-zero (`ClipmapUpdateCS.h`). |
| `t N.NN` / `t -` | The axis parameter of the crossing, and `-` where no crossing was solved. Only the `hit` route solves one, and it is the only route that prints a number. |
| `resid N.NN` | Must be near zero whenever at least one end was refined: the bisected crossing is on the surface. A large value means the two terms disagree and the terrain is not being read where the sample is. `resid -1.00` means no end was refined (fully buried), not an error. |
| `end N.NN above land` | **Only printed on the paths other than `span`**, where it is the object's height *above* the surface. On the `span` path it would mean the opposite of what it says, so it is omitted there and `deep` carries the figure. |
| `place near-mesh` / `near` | Nothing was buried, but the object was within `ShaftSpanMaxGap` of the snow. `near-mesh` means the gate used the mesh's own lowest corner. |
| `Shaft gate: carried clear` | The object is above the snow along its whole length. Read the `surface` number, and the `mesh ... lowest corner` beside it: the mesh figure is the one the gate actually compared when `FromMesh=1`. |
| `half length` / `half width` | What the mark was drawn at. Compare them with the `(hull length ..., hull thickness ...)` beside them, and with the `Shaft mesh:` line above: when `axis mesh` is on, the length came from the mesh's own half length and the width from the `-> width` figure, and the hull's numbers are shown only for contrast. |
| Line count | Expected to fall sharply. 75% of the recorded frames had the weapon in midair, and those are rejected. |

## Still open

- **The distance rule for projectiles and explosions is not implemented.**  A
  thrown or fired object's mark does not yet fall off with distance travelled.
- **One other consumer still reads `d` as though it were a distance: the lip
  band.**  `ClipmapUpdateCS.h` places the thrown snow with
  `inner = s.z * saturate(p.x)`, `outer = s.z * (1 + lipBand)` and
  `u = (d - inner) / (outer - inner)` - three multiples of `s.z` compared
  against `d`, which is only a single number for a disc.  It is **not a live bug
  today**, and the proof is reachability rather than shape: the branch is entered
  only when `StampNoise[i].y` is non-zero, which is `stamp.lipBand`, which
  `MagicImpacts.cpp` sets only for `Kind::kExplosion`, and that file never
  assigns `halfWidth`, so every stamp that can reach the branch has
  `shape.z == 0` and `d` is in world units.  It is the same trap the rim was in,
  one branch below it, and it stays safe only for as long as that stays true.
  The general form, which is exactly equivalent for a disc (including a bulged
  one, because the bulge is already folded into `d`) and correct for an ellipse,
  is to count in outline-normalised units instead:
  `u = ((d / s.z) - saturate(p.x)) / ((1 + lipBand) - saturate(p.x))`.
  Worth doing the moment a line is given thrown snow, and not before - as a
  change today it is a no-op on every reachable input, and a no-op that costs a
  build, a redeploy and a playthrough is not worth making.
- **The log formatting has no test.**  `LogShaftStamp` builds its `end` and `t`
  columns as strings so that "not measured" has its own spelling, and only the
  embedded-string scan can tell whether the build carries them.  A formatting
  mistake there would be invisible to the suite and visible only in a run.
- **`Require(explosion[probe] < -0.1f, ...)` in the magic-impact test** still
  uses a hard-coded `probe = 10` rather than searching for the crater centre.
- **The crater's depth is shallower than intended**, and the crater centre is
  found by a fixed probe rather than dynamically.
- **The mesh walk shapes the width along the object and the lowest corner of its
  box, but the stamped shape is still a swept line of that width.**  A shield's
  face or a hammer's head is not yet expressed as a second dimension - that would
  need the mark's cross-section to vary along the stretch, which the stamp's
  parameters (a radius, a half width, an axis) cannot currently carry.

## Gotchas worth keeping

These are the traps this code has actually fallen into.  Each one cost a build
and a playthrough, and several cost more than one.

- **A value written unconditionally somewhere and overwritten conditionally
  somewhere else.**  Before inserting an assignment, grep the downstream for an
  unconditional write of the same field.  A field can also be cleared by whole-
  struct assignment - `g_motion[a_form] = { x, y, z }` silently clears any
  sibling member - so adding a field is not the same as making it effective.
- **Scaling the upstream is not scaling the downstream.**  If a value's meaning
  changes, every reader has to be re-read: an upstream change of "length" can
  arrive downstream as "width" duty.
- **A probe's criterion has to be falsifiable.**  A condition that is true of
  every input - `f >= half`, or a pin on an `inline` function - is a claim that
  cannot fail, which is worse than no probe.
- **Verify an embedded string by searching the bytes with Python**, not with
  `strings`, and note that `grep -c $'\r'` misreports zero in this environment.
- **A backup file is not the original.**  For a "compare against the original"
  claim, use the untouched copy, not a `.bak-*` left by an editor.
- **Before inserting a function, confirm its namespace and that every symbol it
  uses is in scope** - and that no same-named local is shadowing one.  A shadowed
  `measured` compiled silently and meant something different from the enclosing
  one.  Iron law: check for shadowing as well as for scope.
- **When rewriting a region, the match string has to cover every line that will
  be replaced.**  A partial match leaves orphaned fragments - the same class of
  failure as a mechanical search-and-replace that produces a doubled half
  sentence and a stray token.
- **Test thresholds are only meaningful for the parameters they were computed
  with.**  Changing a parameter means recomputing the margin, and a branch's
  reachable interval is the **intersection** of its inequalities - selecting a
  test point against only one of them lands outside the branch.
- **Every new assertion has to be reverse-verified**: break the implementation,
  confirm the failure, restore, confirm the pass.  For an arithmetic-only change
  no embedded string moves, so the byte scan stays green and a source-level pin
  is the only second rule available - and that pin has to cover **both arms** of
  a branch, with comments stripped before counting.
- **A sentinel initial value that would itself pass the "has this been written"
  test is a bug.**  A field initialised to a plausible value cannot be told apart
  from one that was never set.
- **A quantity and the quantity you are judging it against have to be the same
  physical quantity.**  Two reference surfaces compared as though they were one
  produced a `drop 9.14` beside a `span 6.6`, and a "thickness" that was
  measuring a length.
- **"Subtract it and add it back" is a warning sign** - it usually means two
  reference surfaces are being reconciled.  Unifying them means deleting the
  compensation, or the correction is counted twice.
- **A main direction is only defined up to sign** - the sign has to be settled
  explicitly, or "which end is band 0" is a coin toss.
- **An early-return guard can make a new branch dead code.**  Before adding a new
  path, check whether an `if (... < 0) return;` in front of it makes the new path
  unreachable.
- **A sabotage that survives is worth more than one that fails.**  It is the only
  way to find out which half of a change is actually covered, and the surviving
  half has to be **named** as belonging to whichever check covers it - behaviour
  or source rule.  Some changes are covered only by one of the two.
- **A diagnostic log must not have a one-off budget.**  An allowance spent early
  leaves the rest of the session silent, and "silent" is indistinguishable from
  "broken".  The tell is a log whose last line of a given kind is minutes before
  the end of the file.
- **Writing a file and reading it back without waiting for a flush reads the old
  contents** - a false failure, and a false pass if the assertion was inverted.
- **The same physical quantity reached by two routes is two reference surfaces.**
  A gate that asks about the shell and a walk that asks about the centre line
  will disagree, and the disagreement is not a threshold to tune.
- **Deleting a parameter is three edits**: the signature, the call site, and the
  parameter names inside the function's own format-argument lists.  `grep` the
  old name afterwards, before building.
- **A byte-scan needle has to be a needle.**  If the needle is a substring of an
  unrelated literal the count is wrong *for a reason*, which is more dangerous
  than reading zero because it looks like evidence.  Pin the literal with its NUL
  terminator, and measure the expected count on the built image rather than
  counting it in the header - comments and definitions both survive into the
  emitted text.
- **An old assertion can pin an old bug.**  When a change makes an existing test
  fail, the first question is whether it was expecting the *wrong* value.
- **A distance is only a distance in the units its shape is.**  Anything that
  wants a width out of a shape-dependent distance has to ask which shape it is
  looking at first.  Fixing one reader of an ambiguous quantity is not fixing the
  quantity: enumerate every reader and prove each one either is shape-safe or is
  unreachable.
- **A band narrower than the cell it is written to is a band that is not there.**
  Quantisation is not a cosmetic detail: the band has to be floored in cells, and
  the floor has to move with the level, because the coarse level's cell is four
  times the fine one's.
- **A rule has to be a named function, or it is not testable.**  An inline
  expression is invisible both to the test suite and to the byte scan, and the
  caller's own rule has to be in a header for the same reason.
- **When aligning to X, re-derive every constant in X's recipe.**  Borrowing X's
  *constants* is usually wrong: a ceiling chosen for one shape's maximum is not
  the ceiling the aligned shape needs.
- **After a sabotage, rebuild before delivering.**  The last deployed binary may
  be the contaminated one - a restored source with an un-rebuilt binary is still
  the sabotage on disk, and neither the exit code nor a source read will show it.
- **An assertion that only exercises a pure function with idealised arguments is
  not enough**: at least one has to walk the caller's real arguments, or the
  function passes while the call site is wrong.
- **`LineWidth(x, s, min, max)` with `max = 0` is "ceiling of zero", not "no
  ceiling".**  The last line is `return scaled < a_max ? scaled : a_max;`, so a
  zero maximum returns zero.  A generic clamp helper is the wrong place to
  express "no limit"; give the arithmetic its own name instead.
- **`far`, `near` and `small` are empty macros in the Windows headers.**  A local
  named `near` compiles to nothing, and the diagnostic points at the member name
  or the type rather than at the macro.  This has been hit three times, always in
  test code.
- **A piped build swallows the exit code.**  Never pipe the build or a test
  harness; and note that `grep -c` exits 1 when the count is zero, so a
  `grep -c` guard at the end of a chain reports a false failure.  Judge the suite
  by `ALL PASS (0 failures)` / `ALL STAMP SURFACE TESTS PASS`, not by counting
  lines that contain "fail" - a test *name* can contain it.
- **GNU sed's BRE does not interpret `\t`**, so an indentation change made with
  `sed -i` is a silent no-op.  Use Python and count bytes.
- **A template in a header is how a pure function gets tested.**  The land height
  is a callable parameter, so the test hands it a plane.  Anything that reaches
  into the engine cannot be asserted on offline, and arithmetic that cannot be
  asserted on is arithmetic that drifts.
- **`Hooks.cpp.obj: Permission denied`** appears intermittently and is a
  transient scanner lock on the object file, not a code error.  Re-running the
  build clears it.  The object file does not exist afterwards, which is how to
  tell the two apart.
