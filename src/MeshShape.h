// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <algorithm>
#include <cmath>

// The shape of the object's own mesh, as opposed to the shape of the
// collidable that stands in for it.
//
// Why this exists at all:
//
// Reading the collision hull alone was the right first step - the hull is live,
// it follows the animation, and the placement bug was that its measurements were
// being discarded rather than that they were wrong.  What the hull cannot do is
// tell two weapons apart.  It is a capsule or a box around the mesh, so a bow and
// a staff of the same length and the same thickness produce the same capsule, and
// therefore the same mark.  The bow's curve is simply not in the hull.  That is
// the whole of the remaining gap and it is what this header closes.
//
// The mesh is reachable.  BSTriShape keeps a CPU-side copy of its vertices
// (BSGraphics::TriShape::rawVertexData) and a descriptor that says where in
// each vertex the position lives, so the vertices can be read directly and
// nothing has to be locked, uploaded, or asked of the renderer.
//
// The cost is real - a NIF triangle mesh is thousands of vertices against a
// handful of hull planes - so the work is split the only way that is
// affordable:
//
//   * Fit()   runs in the object's own space, once per mesh, and is cached by
//             the caller.  A mesh does not change, so its local box, its long
//             axis and its width profile are fitted once and kept.
//   * Place() runs per frame and does eight point transforms.  Eight, not
//             thousands: the transform is applied to the local box corners
//             rather than to every vertex, which gives the exact world box of
//             the local box for any rotation.
//
// This file has no engine types in it, for the same reason ContactPoint.h has
// none: it is arithmetic that has to be asserted on, and arithmetic that
// cannot be called from the offline test is arithmetic that cannot be tested.
// The caller supplies the transform as a callable, so the offline test can
// hand it a rotation matrix and the game hands it a NiTransform.
//
// What "the long axis" means here, and why it is not the AABB's longest side:
//
// The slab profile is the point of the exercise.  It is the radial half width
// of the mesh measured in bands along the axis, so a rod produces sixteen
// nearly equal numbers and a bow produces sixteen that swell and shrink.  That
// profile is what lets the drawn mark narrow and widen along its own length
// instead of being one rectangle.  Taking the axis from the bounding box
// instead of from the vertex cloud would tie the bands to the box rather than
// to the object, which for a curved weapon puts the bands across the curve
// instead of along it.
namespace MeshShape
{
	// Bands along the axis.  Sixteen is enough to show a bow's taper and few
	// enough that each band still has vertices in it for a mesh of a few
	// hundred - a band that ends up empty keeps the previous band's width
	// rather than reading as zero.
	constexpr int kSlabs = 16;

	// A mesh fitted in its own space.  Everything here survives the object
	// being carried, swung, or rotated, which is why it can be cached.
	struct Local
	{
		float minX{ 0.0f };
		float minY{ 0.0f };
		float minZ{ 0.0f };
		float maxX{ 0.0f };
		float maxY{ 0.0f };
		float maxZ{ 0.0f };

		// Half extents of that box, so a caller reasoning about the box does
		// not have to halve three differences again in three places.
		float halfX{ 0.0f };
		float halfY{ 0.0f };
		float halfZ{ 0.0f };

		// Unit long axis of the vertex cloud, in local space.  The principal
		// direction rather than the box's longest side, so it follows a
		// curved weapon.
		float ax{ 1.0f };
		float ay{ 0.0f };
		float az{ 0.0f };

		// Half extent along that axis.
		float halfLength{ 0.0f };

		// Radial half widths: the largest and the smallest over the occupied
		// bands.  The largest is the outline the object presents across its
		// axis; the smallest is the material's own thickness where it is
		// thinnest, which is the number that describes a blade.
		float halfWidth{ 0.0f };
		float halfThin{ 0.0f };

		// Radial half width per band, band 0 at the -axis end.  Bands with no
		// vertices are filled from their nearest occupied neighbour afterwards
		// so the sequence has no holes.
		float slab[kSlabs]{};

		int   count{ 0 };
		bool  valid{ false };
	};

	// Where the mesh sits in the world, from its local box and a transform.
	struct World
	{
		// Centre of the world box formed by transforming the local box.
		float cx{ 0.0f };
		float cy{ 0.0f };
		float cz{ 0.0f };

		// The lowest corner of that box - the lowest point of the object, to
		// box accuracy.  Kept with its own x and y rather than paired with
		// the box's minima, because the corner that is lowest is not in
		// general also the corner that is furthest in x and y.
		float lowestX{ 0.0f };
		float lowestY{ 0.0f };
		float lowestZ{ 0.0f };

		// Unit long axis in world space, carried over from the local one.
		float ax{ 1.0f };
		float ay{ 0.0f };
		float az{ 0.0f };

		// The local half length scaled by the transform's scale, which is the
		// half length the world axis actually reaches.
		float halfLength{ 0.0f };

		// Half extents of the world box, for the log.
		float ex{ 0.0f };
		float ey{ 0.0f };
		float ez{ 0.0f };

		bool valid{ false };
	};

	// Fit the local box, the long axis and the width profile to a run of
	// positions.  `a_xyz` is three floats per vertex, in local space.
	//
	// The axis is found by power iteration on the covariance of the vertex
	// cloud, seeded with the local box's longest side.  A few dozen iterations
	// of a 3x3 multiply is nothing next to the vertex walk that just happened,
	// and it is deterministic - the same mesh always yields the same axis, so
	// a carried weapon cannot flip its mark's direction between frames when it
	// happens to sit near a tie.
	inline bool Fit(const float* a_xyz, int a_count, Local& out)
	{
		out = Local{};

		if (!a_xyz || a_count < 3) {
			return false;
		}

		float minX = 1.0e30f;
		float minY = 1.0e30f;
		float minZ = 1.0e30f;
		float maxX = -1.0e30f;
		float maxY = -1.0e30f;
		float maxZ = -1.0e30f;

		for (int i = 0; i < a_count; ++i) {
			const float x = a_xyz[i * 3 + 0];
			const float y = a_xyz[i * 3 + 1];
			const float z = a_xyz[i * 3 + 2];

			// One non-finite position is enough to make the whole box
			// meaningless, and a half-typed stride is exactly the way a
			// non-finite position gets in.  Refusing is the honest answer:
			// the caller falls back to the hull, which is the behaviour
			// before this file existed.
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
				return false;
			}

			if (x < minX) { minX = x; }
			if (y < minY) { minY = y; }
			if (z < minZ) { minZ = z; }
			if (x > maxX) { maxX = x; }
			if (y > maxY) { maxY = y; }
			if (z > maxZ) { maxZ = z; }
		}

		const float sx = maxX - minX;
		const float sy = maxY - minY;
		const float sz = maxZ - minZ;
		if (!(sx > 0.0f) || !(sy > 0.0f) || !(sz > 0.0f)) {
			// A mesh with no thickness in one direction is either a plane -
			// which has no meaningful "long axis and thickness" pair - or a
			// misread.  Neither is worth guessing at.
			return false;
		}

		out.minX = minX;
		out.minY = minY;
		out.minZ = minZ;
		out.maxX = maxX;
		out.maxY = maxY;
		out.maxZ = maxZ;
		out.halfX = 0.5f * sx;
		out.halfY = 0.5f * sy;
		out.halfZ = 0.5f * sz;
		out.count = a_count;

		const float cx = 0.5f * (minX + maxX);
		const float cy = 0.5f * (minY + maxY);
		const float cz = 0.5f * (minZ + maxZ);

		// Covariance of the vertex cloud about the box centre, unscaled.  Only
		// its dominant direction is wanted, so the normalisation does not
		// matter.
		float cxx = 0.0f;
		float cxy = 0.0f;
		float cxz = 0.0f;
		float cyy = 0.0f;
		float cyz = 0.0f;
		float czz = 0.0f;

		for (int i = 0; i < a_count; ++i) {
			const float dx = a_xyz[i * 3 + 0] - cx;
			const float dy = a_xyz[i * 3 + 1] - cy;
			const float dz = a_xyz[i * 3 + 2] - cz;

			cxx += dx * dx;
			cxy += dx * dy;
			cxz += dx * dz;
			cyy += dy * dy;
			cyz += dy * dz;
			czz += dz * dz;
		}

		// Seed with the box's longest side, which for a rod or a bow is
		// already nearly the answer and which keeps the iteration away from
		// the degenerate axes.
		float px = 1.0f;
		float py = 0.0f;
		float pz = 0.0f;
		if (sy >= sx && sy >= sz) {
			px = 0.0f;
			py = 1.0f;
		} else if (sz >= sx && sz >= sy) {
			px = 0.0f;
			pz = 1.0f;
		}

		for (int iter = 0; iter < 32; ++iter) {
			const float vx = cxx * px + cxy * py + cxz * pz;
			const float vy = cxy * px + cyy * py + cyz * pz;
			const float vz = cxz * px + cyz * py + czz * pz;

			const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
			if (!(len > 1.0e-20f) || !std::isfinite(len)) {
				break;
			}

			px = vx / len;
			py = vy / len;
			pz = vz / len;
		}

		// A principal direction is only defined up to sign.  Which sign the
		// iteration lands on depends on the seed and on floating point noise,
		// and that would make the band order a coin toss: band 0 would be the
		// wide end of a taper for one mesh and the narrow end for the next, so
		// a caller could not say which end of the object a band describes.
		//
		// Settling the sign by the largest component makes the band order a
		// property of the mesh rather than of the arithmetic.  The convention
		// carries no meaning - it does not know which end of a sword the point
		// is - but it is stable, and stable is what a caller can rely on when
		// it passes a range of bands back in.
		{
			const float mx = std::abs(px);
			const float my = std::abs(py);
			const float mz = std::abs(pz);

			const bool flip = (mx >= my && mx >= mz && px < 0.0f) ||
			                  (my > mx && my >= mz && py < 0.0f) ||
			                  (mz > mx && mz > my && pz < 0.0f);

			if (flip) {
				px = -px;
				py = -py;
				pz = -pz;
			}
		}

		out.ax = px;
		out.ay = py;
		out.az = pz;

		// Half length along the axis, and the radial distance from it.  A
		// vertex's radial component is its distance from the line minus its
		// component along the line, taken as the square root of the
		// difference so the projection is never recomputed with a second
		// square root.
		const auto radiusOf = [&](float a_dx, float a_dy, float a_dz, float a_along) {
			const float d2 = a_dx * a_dx + a_dy * a_dy + a_dz * a_dz;
			const float r2 = d2 - a_along * a_along;
			return r2 > 0.0f ? std::sqrt(r2) : 0.0f;
		};

		float halfLength = 0.0f;
		for (int i = 0; i < a_count; ++i) {
			const float dx = a_xyz[i * 3 + 0] - cx;
			const float dy = a_xyz[i * 3 + 1] - cy;
			const float dz = a_xyz[i * 3 + 2] - cz;
			const float along = dx * px + dy * py + dz * pz;
			const float mag = std::abs(along);
			if (mag > halfLength) {
				halfLength = mag;
			}
		}

		if (!(halfLength > 0.0f)) {
			return false;
		}

		out.halfLength = halfLength;

		// Band the vertices along the axis and keep the widest radius in each
		// band.  The widest rather than the mean: the outline of the object is
		// what the snow sees, and a mean would let a few thin vertices in a
		// band pull the outline in.
		float slab[kSlabs] = {};
		int   filled[kSlabs] = {};

		float halfWidth = 0.0f;
		for (int i = 0; i < a_count; ++i) {
			const float dx = a_xyz[i * 3 + 0] - cx;
			const float dy = a_xyz[i * 3 + 1] - cy;
			const float dz = a_xyz[i * 3 + 2] - cz;
			const float along = dx * px + dy * py + dz * pz;
			const float rad = radiusOf(dx, dy, dz, along);

			if (rad > halfWidth) {
				halfWidth = rad;
			}

			// t is in units of the half length, so the bands are spread
			// evenly over the object whatever its length.
			const float t = along / halfLength;  // -1 .. 1
			int         band = static_cast<int>((t + 1.0f) * 0.5f * static_cast<float>(kSlabs));
			if (band < 0) {
				band = 0;
			}
			if (band >= kSlabs) {
				band = kSlabs - 1;
			}

			if (rad > slab[band]) {
				slab[band] = rad;
			}
			filled[band] = 1;
		}

		// Fill any empty band from its nearest occupied neighbour, in three
		// sweeps: everything before the first occupied band takes its value,
		// everything between two occupied bands takes the one to its left, and
		// everything after the last takes that.  Bands are empty only where the
		// mesh has no vertices in that slice of its own length, which says
		// nothing about the object's width there; leaving one at zero would
		// draw the mark pinched to nothing in the middle of a weapon.
		int firstOccupied = -1;
		int lastOccupied = -1;
		for (int i = 0; i < kSlabs; ++i) {
			if (filled[i]) {
				if (firstOccupied < 0) {
					firstOccupied = i;
				}
				lastOccupied = i;
			}
		}
		if (lastOccupied < 0) {
			return false;
		}

		for (int i = firstOccupied - 1; i >= 0; --i) {
			slab[i] = slab[firstOccupied];
		}
		for (int i = firstOccupied + 1; i <= lastOccupied; ++i) {
			if (!filled[i]) {
				slab[i] = slab[i - 1];
			}
		}
		for (int i = lastOccupied + 1; i < kSlabs; ++i) {
			slab[i] = slab[lastOccupied];
		}

		float halfThin = 1.0e30f;
		for (int i = 0; i < kSlabs; ++i) {
			out.slab[i] = slab[i];
			if (slab[i] < halfThin) {
				halfThin = slab[i];
			}
		}
		if (!std::isfinite(halfThin)) {
			halfThin = 0.0f;
		}

		out.halfWidth = halfWidth;
		out.halfThin = halfThin;
		out.valid = true;
		return true;
	}

	// Put the fitted mesh into the world.
	//
	// `a_toWorld` is called with a local position and must return the world
	// position - twelve floats in, nothing out but the answer.  Passing the
	// transform as a callable rather than as a matrix is deliberate: the
	// engine's own NiTransform::operator* is then what does the arithmetic, so
	// this file never has to assume whether NiMatrix3 is row or column major.
	// Guessing that convention is exactly the kind of assumption that compiles
	// and then puts the mark somewhere else.
	template <class ToWorldFn>
	inline bool Place(const Local& a_local, ToWorldFn&& a_toWorld, World& out)
	{
		out = World{};

		if (!a_local.valid) {
			return false;
		}

		// The eight corners of the local box are all that need transforming.
		// Transforming every vertex instead would be the same box for a
		// rotation and a tighter one for a shear, and a transform here is a
		// rotation, a translation and one uniform scale.
		float minX = 1.0e30f;
		float minY = 1.0e30f;
		float minZ = 1.0e30f;
		float maxX = -1.0e30f;
		float maxY = -1.0e30f;
		float maxZ = -1.0e30f;

		float lowX = 0.0f;
		float lowY = 0.0f;
		float lowZ = 0.0f;

		for (int i = 0; i < 8; ++i) {
			const float x = (i & 1) ? a_local.maxX : a_local.minX;
			const float y = (i & 2) ? a_local.maxY : a_local.minY;
			const float z = (i & 4) ? a_local.maxZ : a_local.minZ;

			float wx = 0.0f;
			float wy = 0.0f;
			float wz = 0.0f;
			if (!a_toWorld(x, y, z, wx, wy, wz)) {
				return false;
			}
			if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) {
				return false;
			}

			if (i == 0 || wz < minZ) {
				lowX = wx;
				lowY = wy;
				lowZ = wz;
			}

			if (wx < minX) { minX = wx; }
			if (wy < minY) { minY = wy; }
			if (wz < minZ) { minZ = wz; }
			if (wx > maxX) { maxX = wx; }
			if (wy > maxY) { maxY = wy; }
			if (wz > maxZ) { maxZ = wz; }
		}

		// The axis is carried as a direction, so it is the difference of two
		// transformed points rather than a transformed direction: a difference
		// of points is a vector for any affine transform, while transforming a
		// direction only works if the translation is left out, and getting
		// that wrong is invisible until the object moves off the origin.
		const float midX = 0.5f * (a_local.minX + a_local.maxX);
		const float midY = 0.5f * (a_local.minY + a_local.maxY);
		const float midZ = 0.5f * (a_local.minZ + a_local.maxZ);

		float ox = 0.0f;
		float oy = 0.0f;
		float oz = 0.0f;
		float tx = 0.0f;
		float ty = 0.0f;
		float tz = 0.0f;

		if (!a_toWorld(midX, midY, midZ, ox, oy, oz)) {
			return false;
		}
		if (!a_toWorld(midX + a_local.ax * a_local.halfLength,
				midY + a_local.ay * a_local.halfLength,
				midZ + a_local.az * a_local.halfLength,
				tx, ty, tz)) {
			return false;
		}

		const float dx = tx - ox;
		const float dy = ty - oy;
		const float dz = tz - oz;
		const float len = std::sqrt(dx * dx + dy * dy + dz * dz);

		float ax = a_local.ax;
		float ay = a_local.ay;
		float az = a_local.az;
		float worldHalfLength = a_local.halfLength;

		if (len > 1.0e-6f && std::isfinite(len)) {
			ax = dx / len;
			ay = dy / len;
			az = dz / len;
			worldHalfLength = len;
		}

		out.cx = 0.5f * (minX + maxX);
		out.cy = 0.5f * (minY + maxY);
		out.cz = 0.5f * (minZ + maxZ);

		out.lowestX = lowX;
		out.lowestY = lowY;
		out.lowestZ = lowZ;

		out.ax = ax;
		out.ay = ay;
		out.az = az;
		out.halfLength = worldHalfLength;

		out.ex = 0.5f * (maxX - minX);
		out.ey = 0.5f * (maxY - minY);
		out.ez = 0.5f * (maxZ - minZ);

		out.valid = true;
		return true;
	}

	// The mesh's radial half width over a stretch of its own axis, where the
	// stretch is given in the same units AxisSpan reports - t of -1 at the
	// -axis end and +1 at the other.
	//
	// A mark that spans band 14 to 16 of a bow is a mark in the tapering part,
	// and drawing it with the bow's widest measurement would make the mark
	// wider than the part of the bow that made it.  Averaging the bands it
	// actually covers is what makes the drawn width follow the object.
	//
	// Bands are averaged with weight proportional to how much of each band the
	// stretch covers, so a stretch covering a tenth of one band is not counted
	// as if it covered the whole of it.
	//
	// A stretch that the object does not reach into has no width of its own,
	// and reporting the object's *widest* measurement for it - which is what a
	// naive fallback does - would draw a mark the full width of the weapon at
	// a place where the weapon is not.  The width reported is therefore the
	// width of the nearest part of the object to what was asked for: the
	// tapering end for a stretch beyond that end, and the band containing a
	// zero-width stretch inside it.  Both are the same rule, applied to the
	// point of the object closest to the question.
	inline float WidthOver(const Local& a_local, float a_t0, float a_t1)
	{
		if (!a_local.valid) {
			return 0.0f;
		}

		float lo = a_t0;
		float hi = a_t1;
		if (lo > hi) {
			const float swap = lo;
			lo = hi;
			hi = swap;
		}

		if (lo < -1.0f) { lo = -1.0f; }
		if (hi > 1.0f) { hi = 1.0f; }
		if (lo > 1.0f) { lo = 1.0f; }
		if (hi < -1.0f) { hi = -1.0f; }

		if (!(hi > lo)) {
			const float at = std::isfinite(lo) && std::isfinite(hi) ? 0.5f * (lo + hi) : 0.0f;
			lo = at;
			hi = at;
		}

		// A zero-width stretch still lands in exactly one band, so the loop
		// below is given a hair of width to intersect with.
		const float edge = 1.0f / static_cast<float>(kSlabs) * 0.5f;

		float weighted = 0.0f;
		float total = 0.0f;

		for (int i = 0; i < kSlabs; ++i) {
			const float bandLo = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kSlabs);
			const float bandHi = -1.0f + 2.0f * static_cast<float>(i + 1) / static_cast<float>(kSlabs);

			const float qLo = lo - edge;
			const float qHi = hi + edge;

			const float ovLo = qLo > bandLo ? qLo : bandLo;
			const float ovHi = qHi < bandHi ? qHi : bandHi;
			const float over = ovHi - ovLo;
			if (over <= 0.0f) {
				continue;
			}

			weighted += a_local.slab[i] * over;
			total += over;
		}

		if (!(total > 0.0f)) {
			return a_local.halfWidth;
		}

		return weighted / total;
	}

	// Whether a fitted mesh agrees with the bounding sphere the engine
	// already computed for it.
	//
	// This is the check that makes the vertex walk trustworthy rather than
	// hopeful.  Reading vertices means reading raw bytes at a stride, and a
	// stride that is wrong by four bytes does not crash - it produces
	// plausible-looking floats made out of someone else's attribute, and the
	// mark ends up somewhere arbitrary with nothing in the log to say so.
	// modelBound is the engine's own answer for the same mesh, so it is the
	// one independent number available to check the walk against.
	//
	// The comparison is deliberately loose.  modelBound is the bound of the
	// geometry rather than of its vertices, so it can be inflated, and the
	// walk here may be over a subset.  This is a sanity gate, not a
	// measurement: it is asked to catch a stride that is wrong, not to agree
	// to a hundredth.
	inline bool BoundAgrees(const Local& a_local, float a_bx, float a_by, float a_bz,
		float a_br, float a_tolerance, float& a_outError)
	{
		a_outError = -1.0f;

		if (!a_local.valid || !std::isfinite(a_bx) || !std::isfinite(a_by) ||
			!std::isfinite(a_bz) || !std::isfinite(a_br) || !(a_br > 0.0f)) {
			return false;
		}

		const float cx = 0.5f * (a_local.minX + a_local.maxX);
		const float cy = 0.5f * (a_local.minY + a_local.maxY);
		const float cz = 0.5f * (a_local.minZ + a_local.maxZ);

		// The box's own bounding sphere: the centre of the box, and the
		// distance to one of its corners.
		const float ex = 0.5f * (a_local.maxX - a_local.minX);
		const float ey = 0.5f * (a_local.maxY - a_local.minY);
		const float ez = 0.5f * (a_local.maxZ - a_local.minZ);
		const float br = std::sqrt(ex * ex + ey * ey + ez * ez);

		const float ddx = cx - a_bx;
		const float ddy = cy - a_by;
		const float ddz = cz - a_bz;
		const float centreError = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
		const float radiusError = std::abs(br - a_br);

		a_outError = centreError > radiusError ? centreError : radiusError;

		// Scale the tolerance by the bound so a large mesh is allowed a
		// proportionally larger disagreement, with a floor so a tiny one is
		// still held to a real distance.
		const float allow = a_tolerance * a_br + 1.0f;
		return a_outError <= allow;
	}

	// How far an object's lowest point sits below its own centre line,
	// measured vertically and taken *at that point's own place*.
	//
	// This is the number the axis walk wants.  It samples the centre line and
	// asks `sample.z - offset - land(sample.x, sample.y)`, so the offset means
	// "here, at this (x, y), the object's surface is this far under its centre
	// line".  That is a thickness.  It does not grow with the object's length.
	//
	// The shortcut of taking the centre's height minus the lowest corner's
	// height is not that quantity.  For a weapon 68 units long tilted so that
	// its axis is 0.72 in z, the centre stands about 48 units above its own
	// lower end, so the shortcut returns 48 where the thickness is 5.  Handing
	// the walk 48 lowers every sample by 48, so the whole weapon reads as
	// buried along its entire length whatever it is actually doing.  The log
	// had exactly that: `drop 52.20` beside a mesh whose own `lowest corner`
	// was `above by 24.73`, and `span 45.9 deep 52.20` for an object whose
	// collidable put its lowest point 7.81 units under the snow.
	//
	// Locating the point on the axis first is what removes the length: the
	// axis's height is read at the point's own (x, y) instead of at the
	// object's centre.  A vertical axis has no (x, y) to project onto - a rod
	// held upright reaches its lowest point along the very line the walk
	// already follows - and returns zero, which is correct rather than a
	// missing answer.
	//
	// `a_outT` reports where on the axis the point landed, in the same units
	// the walk uses.  It is an output rather than a return because the log
	// needs it to show that the projection happened at all.
	inline float DropAt(float a_pointX, float a_pointY, float a_pointZ,
		float a_cx, float a_cy, float a_cz,
		float a_ax, float a_ay, float a_az, float a_halfLength,
		float& a_outT)
	{
		a_outT = 0.0f;
		if (!std::isfinite(a_pointX) || !std::isfinite(a_pointY) ||
			!std::isfinite(a_pointZ) || !std::isfinite(a_cx) ||
			!std::isfinite(a_cy) || !std::isfinite(a_cz) ||
			!std::isfinite(a_ax) || !std::isfinite(a_ay) ||
			!std::isfinite(a_az) || !std::isfinite(a_halfLength)) {
			return 0.0f;
		}

		const float horizontal2 = a_ax * a_ax + a_ay * a_ay;
		if (!(horizontal2 > 1.0e-4f) || !(a_halfLength > 0.0f)) {
			return 0.0f;
		}

		float t = ((a_pointX - a_cx) * a_ax + (a_pointY - a_cy) * a_ay) /
			(a_halfLength * horizontal2);
		t = std::clamp(t, -1.0f, 1.0f);
		a_outT = t;

		const float lineZ = a_cz + t * a_halfLength * a_az;
		const float drop = lineZ - a_pointZ;
		return drop > 0.0f ? drop : 0.0f;
	}

	// How far above the snow a raised object is laid, in world units.
	//
	// This is not the same quantity as the caller's threshold for "this raise
	// is worth doing", and letting one number serve as both is what made the
	// first attempt at the dead band still twitch.
	//
	// The threshold asks "is this reading worth acting on?" and wants to be
	// small - about the size of the noise in the measurement, or a real sink
	// gets ignored.  The landing margin asks "how long until this body asks
	// again?" and wants to be several seconds of settling: a body is laid
	// `margin` above the surface and asks again once it is `threshold` below
	// it, so it has `margin + threshold` to fall through before the next
	// raise.  A margin no larger than the threshold gives it about two
	// thresholds of fall, which against a measured sink of 0.57 a second is
	// under two seconds - still a twitch, only slower.
	//
	// Walking it: at 0.57 a second, three units is a little over five
	// seconds between raises, which is slow enough to read as settled while
	// still low enough not to look like the object is hovering.  The cost of
	// being generous is a slightly raised object; the cost of being mean is
	// a body frozen and re-zeroed every second, which is what "the object
	// cannot be kicked" was.
	inline constexpr float kLiftSettleMargin = 3.0f;

	// How far to raise a dropped object so that its own top comes level with
	// the snow, instead of it resting on the terrain underneath the blanket.
	//
	// The raise displaces the terrain *mesh*; the engine's collision does not
	// know about it, so a dropped object falls until its collision meets the
	// bare terrain and stops there, with the snow surface `a_lift` units above
	// its head.  Measured in game: a steel arrow read `drop = -35.9`, and
	// since `drop = groundZ - (landZ + a_lift)`, that puts its lowest point at
	// -0.9 against a land height of about 0.  Anything shorter than the
	// blanket is buried whole, which is what an object "vanishing when it
	// lands" has actually been - the mark was never the problem.
	//
	// The answer is `snow surface - object's own height`, expressed as a
	// distance to move rather than as a target, because the caller has the
	// object's current position and this has only the measurements.
	//
	// Three properties are deliberate:
	//
	//   * It never lowers.  An object already standing above the snow - gear
	//     on a rock, on a floor, on a table - must be left where it is, and a
	//     rule that could pull it down would sink it into a surface it was
	//     never on.
	//   * It is clamped to the blanket's own thickness.  A lift larger than
	//     the snow is a misread measurement rather than a buried object, and
	//     the blanket is the honest bound because it is the distance between
	//     the two surfaces being confused.
	//   * It is idempotent.  Once the object is where this asks it to be, the
	//     next call returns zero and nothing moves - which matters because
	//     the scan runs five times a second and a rule that always returned
	//     the same lift would ratchet the object up out of the world.
	//
	// `a_slack` is a dead band supplied by the caller, and it is half of the
	// difference between an object that settles and one that twitches
	// forever.
	//
	// Aiming exactly at the surface is not stable.  A body resting with its
	// top level with the snow still sinks a fraction of a unit between
	// scans, so "am I below the surface?" is true again a second later and
	// the object is lifted again - measured on a fur helmet: twenty lifts in
	// twenty seconds, each 0.5 to 1.8 units, none of them wrong on its own.
	// Every one of those lifts also wrote the body, so the object both
	// twitched and stopped answering to being kicked: the two faults are
	// the same one.
	//
	// A sink smaller than the band therefore asks for nothing, and the raise
	// carries a landing margin as well - see `kLiftSettleMargin` for why the
	// margin is its own quantity and not the band again.
	//
	// `a_lowestZ` is the object's lowest point and `a_height` its own full
	// vertical extent; both in world units.  A non-finite input, a blanket of
	// no thickness, or an unmeasured height returns zero - the object is then
	// left alone rather than moved by an invented amount.
	// How close to its wanted height an object has to be before the caller
	// stops touching it.
	//
	// A separate rule from the dead band inside `LiftOntoSnow`, and it has to
	// be, because the two are asked different questions.  `LiftOntoSnow`
	// answers "how far should this move?"; this answers "is the object
	// already where the answer puts it?", and the caller uses it to decide
	// whether to touch the body at all.
	//
	// They are also different sizes.  The dead band is a fixed slack the
	// caller sets in the INI; this one is derived from the object's own
	// height, so that it means the same thing to a coin and to a cart - the
	// quantity it is compared against is a distance on the object, and a flat
	// constant would be most of a coin and none of a cart.
	//
	// One tenth of the object's height, with a floor of half a unit and a
	// ceiling of the smallest raise the lift rule can make.  A fur helmet
	// measured in game has a height of 8.22, so its band is 0.82: an object
	// within a unit of where the rule wants it is standing still as far as
	// this rule is concerned, and one that has been pushed into the snow is
	// not.  The floor keeps a very small object - a ring, a coin - from
	// having a band so fine that drift alone crosses it every scan.
	//
	// The ceiling is the part that is not obvious, and it is why this is not
	// simply a tenth of the height.  `LiftOntoSnow` refuses a sink at or
	// under the caller's slack and otherwise asks for `rise + margin`, where
	// the margin is at least `kLiftSettleMargin`.  So the smallest raise the
	// rule can produce is `slack + margin`, and for an object whose band is
	// larger than that, every raise it is willing to make is one the band
	// calls "already settled" - the rule and the caller contradict each
	// other, the caller acts on nothing, and the object is neither moved nor
	// freed.  A tenth of the height crosses `slack + margin` at a height of
	// 35, which is an ordinary crate, and a cart is well past it.
	//
	// Clamping the band to the smallest raise makes the two agree by
	// construction: a band can never swallow a raise the rule would make.
	inline float LiftSettledBand(float a_height, float a_slack)
	{
		if (!std::isfinite(a_height) || !(a_height > 0.0f)) {
			return 0.0f;
		}

		const float slack = (std::isfinite(a_slack) && a_slack > 0.0f) ? a_slack : 0.0f;
		const float margin = slack > kLiftSettleMargin ? slack : kLiftSettleMargin;
		const float ceiling = slack + margin;

		float band = a_height * 0.1f;
		if (band < 0.5f) {
			band = 0.5f;
		}
		if (band > ceiling) {
			band = ceiling;
		}
		return band;
	}

	// The band for the shipped slack, kept for callers and tests that do not
	// carry one.  The two-argument form is the rule; this is the convenience
	// spelling of it.
	inline float LiftSettledBand(float a_height)
	{
		return LiftSettledBand(a_height, 0.5f);
	}

	// What the lift rule did to the body on this scan, as the caller sees it.
	//
	// The three states are the whole of the rule's vocabulary, and naming
	// them is what lets the "who hands the body back to the solver" question
	// be asked of a function instead of being scattered across the branches
	// that produced them.
	enum class LiftAction
	{
		// The object is outside the rule's reach this scan - not snow, being
		// held, or already settled by an earlier scan.  Nothing was done to
		// the body, so nothing has to be undone.
		kNone,
		// The object was already standing where the rule wants it.  The
		// placement the rule is holding may still have to be written through.
		kLeftInPlace,
		// The rule wrote a new height onto the object's position.
		kWrote,
	};

	// Whether this scan leaves a placement the body has to be told about.
	//
	// A position written onto the referenced object does not reach the
	// physics body on its own, so the body only learns about it at the node
	// write-through.  Both exits that can leave a placement in place - a new
	// height, and an object already standing where the rule wants it - ask
	// for it, and `kNone` is the only exit that has nothing to do.
	//
	// This exists as a function because the failure it guards against is
	// invisible in the source: without the write-through the object is
	// placed correctly, sits at the right height, and cannot be kicked, and
	// no behavioural assertion about height or about the write will notice.
	// The assertion has to be about *this* answer, which is why the answer is
	// a function rather than two branches that happen to agree.
	constexpr bool LiftMustFreeBody(LiftAction a_action)
	{
		return a_action != LiftAction::kNone;
	}

	inline float LiftOntoSnow(float a_lowestZ, float a_height, float a_surfaceZ,
		float a_lift, float a_slack)
	{
		if (!std::isfinite(a_lowestZ) || !std::isfinite(a_height) ||
			!std::isfinite(a_surfaceZ) || !std::isfinite(a_lift)) {
			return 0.0f;
		}
		if (!(a_lift > 0.0f) || !(a_height > 0.0f)) {
			return 0.0f;
		}

		// A negative band would turn the comparison below into "always act"
		// and could make a raise negative, which is a move the caller would
		// carry out because it only tests the answer against a positive
		// threshold.  Clamped here so the sign is impossible rather than
		// merely unlikely.
		const float slack = a_slack > 0.0f ? a_slack : 0.0f;

		// A sink smaller than the dead band asks for nothing.
		const float wantLowest = a_surfaceZ - a_height;
		const float rise = wantLowest - a_lowestZ;

		if (!(rise > slack)) {
			return 0.0f;
		}

		// Land above the surface, not on it: a body aimed exactly at the
		// surface settles a little below it and asks to be lifted again.
		// The landing margin is its own quantity and the band is only its
		// floor, so a caller that has sized its band small for the sake of
		// not ignoring real sinks still gets a body that stays put.
		const float margin = slack > kLiftSettleMargin ? slack : kLiftSettleMargin;
		const float want = rise + margin;
		return want < a_lift ? want : a_lift;
	}

	// How much larger than the collision hull a measured mesh box may be
	// before it is treated as a reading of something other than the object.
	// A hull is fitted around its mesh and is usually slightly larger, so a
	// box a little bigger than the hull is expected; a box several times
	// bigger means the node held the character and not the object, and a
	// mark derived from it would put a furrow the width of a body through
	// the snow.  The hull is what a refused reading falls back to, so the
	// cost of refusing a good reading is the old look.
	//
	// A named function rather than a constant at one call site, because two
	// paths now ask this question - the carried weapon and the dropped
	// object - and a rule asserted in one place and applied differently in
	// the real path is the failure this codebase has already been bitten by.
	inline constexpr float kMeshSanityFactor = 6.0f;

	// Whether a measured mesh may be used in place of the collision hull.
	//
	// `a_union` is the sum of the measured union box's half extents and
	// `a_hull` the sum of the hull's own half length and thickness, both
	// already in the same world units.  The +1 keeps a genuinely small
	// object (a ring, a key) from being refused by a factor applied to
	// zero, which is the case a bare multiplication would lose.
	inline bool MeshTrusted(float a_union, float a_hull)
	{
		if (!std::isfinite(a_union) || !std::isfinite(a_hull)) {
			return false;
		}
		if (!(a_union > 0.0f)) {
			return false;
		}
		return a_union <= kMeshSanityFactor * (a_hull + 1.0f);
	}

	// How deep a mark made by an object may be, given how thick that object is.
	//
	// A mark deeper than the object that made it cannot be seen: the snow
	// surface sinks below the object's underside and the object is buried in
	// its own dent.  Measured on a fur helmet, whose collision hull is 4.1
	// units thick: the mesh arm drew depth 13.4 under a mark 10.1 wide, so
	// the depression was three times the helmet's thickness and wider than
	// the helmet itself, and what showed was a hole with nothing in it.
	//
	// `a_halfThickness` and `a_depth` are both world units.  `a_perThickness`
	// is how many half thicknesses deep the mark may be; zero switches the
	// ceiling off and gives the raw depth back, so this is an escape hatch and
	// not a silent clamp.  A half thickness of zero means "not measured" and
	// also returns the depth unchanged, because a ceiling derived from an
	// unmeasured thickness would be a number invented in the caller.
	//
	// Nothing here rounds the depth up: a mark that was already shallower than
	// the ceiling keeps its own value, so this can only ever take a mark down.
	inline float CapDepthByThickness(float a_depth, float a_halfThickness,
		float a_perThickness)
	{
		if (!std::isfinite(a_depth) || !std::isfinite(a_halfThickness) ||
			!std::isfinite(a_perThickness)) {
			return a_depth;
		}
		if (!(a_perThickness > 0.0f) || !(a_halfThickness > 0.0f)) {
			return a_depth;
		}
		const float ceiling = a_halfThickness * a_perThickness;
		return a_depth < ceiling ? a_depth : ceiling;
	}

	// Snow that is over an object, from a raw reach.
	//
	// A reach that is not a usable measurement is none at all: a NaN would
	// become a ceiling and then a depth, which the shader draws as a shaft of
	// undefined size, and a negative one is above the object's own underside
	// and says nothing about what is over it.
	//
	// It is a named function because two rules below need the same answer, and
	// a normalisation written out twice is a normalisation that will disagree
	// with itself the first time one of the two is edited.
	inline float SnowOver(float a_reach)
	{
		return std::isfinite(a_reach) && a_reach > 0.0f ? a_reach : 0.0f;
	}

	// How deep a mark may be, when the object that made it may be *under* the
	// snow rather than standing in its own dent.
	//
	// `CapDepthByThickness` above answers "how deep may a mark be before the
	// object stops being visible in it", and that is the right answer for an
	// object at the surface.  It is the wrong answer for one below it: there
	// the object is not standing in its own dent at all, it is buried by the
	// blanket, and no depth derived from its own thickness can reach it.
	//
	// The engine's collision is the bare terrain and the raised snow is a mesh
	// it knows nothing about, so a dropped object falls through the blanket and
	// stops on the ground under it.  Measured in game: a steel arrow read
	// `drop = -35.9` against a 35-unit blanket - on the ground, with the snow
	// surface 35 units over its head - and anything shorter than the blanket is
	// then buried whole.  A mark capped at twice the object's own half
	// thickness, which is 4.1 units on a fur helmet, sinks 4.1 of the 35 and
	// leaves the object 30 units under it: the object stays invisible and the
	// only thing on screen is a shallow dent exactly where it is not.
	//
	// So the ceiling is the deeper of the two answers.  `a_reach` is how much
	// snow is stacked over the object's own underside - zero for an object at
	// or above the surface, the blanket's whole thickness for one lying on the
	// ground below it - and a mark that deep ends level with the object's own
	// bottom, so the object ends up standing in the hole it made instead of
	// under it.  The caller bounds `a_reach` by the snow that is actually there,
	// because a mark cannot remove snow that does not exist.
	//
	// Only ever raises the ceiling: the object's own thickness still decides
	// every mark whose object is not under the snow, and a thickness of zero or
	// a scale of zero - both meaning "not measured" and "ceiling switched off"
	// respectively - leave the reach as the only answer.
	inline float MarkCeiling(float a_halfThickness, float a_perThickness, float a_reach)
	{
		const float reach = SnowOver(a_reach);
		if (!std::isfinite(a_halfThickness) || !std::isfinite(a_perThickness) ||
			!(a_perThickness > 0.0f) || !(a_halfThickness > 0.0f)) {
			return reach;
		}
		const float byThickness = a_halfThickness * a_perThickness;
		return byThickness > reach ? byThickness : reach;
	}

	// The depth to give a mark: no deeper than the ceiling above, and no
	// shallower than the snow the object is under.
	//
	// Both halves are needed and they fail in opposite directions.  Without the
	// ceiling an object at the surface is buried in its own dent; without the
	// floor a mark comes out at whatever the object's own thickness and the INI
	// scales happen to multiply to, which on a steel arrow is 2.5 units under a
	// 35-unit blanket - a dent the object is not visible in, on a line whose
	// depth, radius and shape all read as intended.
	//
	// `MarkCeiling` guarantees the ceiling is at least the reach, so the two
	// bounds cannot cross and the result is never above the ceiling.
	inline float MarkDepthFor(float a_depth, float a_halfThickness,
		float a_perThickness, float a_reach)
	{
		if (!std::isfinite(a_depth)) {
			return a_depth;
		}
		const float reach = SnowOver(a_reach);
		const float ceiling = MarkCeiling(a_halfThickness, a_perThickness, reach);
		const float depth = a_depth > reach ? a_depth : reach;
		if (ceiling > 0.0f && depth > ceiling) {
			return ceiling;
		}
		return depth;
	}

	// How much of a mark's disc is flat floor, for a mark that has to uncover
	// an object rather than dent the snow around one.
	//
	// The shader's falloff is `1 - smoothstep(radius * shoulder, radius, d)`:
	// the sink is full depth for every `d` up to `radius * shoulder` and
	// falls to nothing at `radius`.  Bare snow carries shoulder = 0.0 - read
	// off the A_Base profile in game - so the whole disc is slope, and a mark
	// no wider than the object leaves the object's own edges in the drift.
	//
	// Measured in game on a fur helmet: marked radius = 10.1 into 23.4 units
	// of snow over it, its own half width 9.8.  At d = 0.6 * radius the sink
	// is already down to 65% of the depth and at d = radius it is nothing, so
	// at the helmet's own edge the snow had moved about 0.2 units and only a
	// circle roughly four units across came out of the drift.  That is what
	// "the helmet shows but only just" was: the depth was right, the shape of
	// the hole was not.
	//
	// Not 0.95, which would leave a wall a fraction of a unit wide and read as
	// a cylinder cut into the drift; the object should sit in a bowl that has
	// been widened, not in a shaft that has been sunk.
	constexpr float kBuriedShoulder = 0.6f;

	inline float BuriedShoulder()
	{
		return kBuriedShoulder;
	}

	// The radius that puts that floor under the whole object.
	//
	// `f` is exactly 1 for every `d` up to `radius * shoulder`, so a floor
	// that reaches `a_halfExtent` needs `radius = a_halfExtent / shoulder`.
	// The extent is the object's own half width, not its hull radius: the
	// mesh route already measures how wide the object is where it is lying,
	// while a hull radius is the corner of its box.
	//
	// A extent that is not a usable measurement leaves the caller's own
	// radius alone rather than replacing it with a zero, which would draw
	// nothing at all.
	inline float BuriedRadius(float a_halfExtent)
	{
		if (!std::isfinite(a_halfExtent) || !(a_halfExtent > 0.0f)) {
			return 0.0f;
		}
		return a_halfExtent / kBuriedShoulder;
	}
}
