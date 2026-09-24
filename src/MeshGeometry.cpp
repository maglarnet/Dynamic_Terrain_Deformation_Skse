// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "MeshGeometry.h"

#include "RE/B/BSGeometry.h"
#include "RE/B/BSTriShape.h"
#include "RE/B/BSVisit.h"
#include "RE/N/NiCollisionObject.h"
#include "RE/N/NiTransform.h"
#include "RE/V/VertexDesc.h"

#include <cstring>
#include <unordered_map>

namespace MeshGeometry
{
	namespace
	{
		// Vertices sampled when ranking the candidate byte layouts.  Enough to
		// see the shape, far short of the whole mesh, and it only ever runs
		// once per mesh because the winner is cached.
		constexpr int kProbeSamples = 96;

		// Hard ceiling on the vertices gathered for a real fit.  A weapon mesh
		// is a few thousand at worst; a node that hands back a hundred thousand
		// is not what this path is for, and refusing is better than stalling
		// the frame that asked.
		constexpr int kMaxVertices = 32768;

		// How many meshes under one node are considered.  A weapon is one or
		// two - a bow and its string, a quiver and its arrows.
		constexpr std::size_t kMaxMeshes = 8;

		// Meshes held in the cache.  Its own shape never changes, so entries do
		// not need an expiry; the cap is only there so a long session that sees
		// a great many modded weapons cannot grow without bound.
		constexpr std::size_t kMaxEntries = 512;

		// How far the walked box may disagree with the engine's own bounding
		// sphere, as a fraction of that sphere.  Loose on purpose: this is
		// asked to catch a stride that is wrong, not to agree to a hundredth,
		// and the engine's bound may be inflated relative to the vertices.
		constexpr float kAcceptTolerance = 0.45f;

		// A position is four floats, so a stride below this would read the same
		// bytes twice and produce a degenerate box rather than a wrong one.
		constexpr std::uint32_t kMinStride = 12;

		struct Layout
		{
			std::uint32_t offset;
			std::uint32_t stride;
			const char*   name;
		};

		struct Entry
		{
			MeshShape::Local local{};
			const void*      vertexData{ nullptr };
			const char*      layout{ "none" };
			float            error{ -1.0f };
			std::uint16_t    vertices{ 0 };
			float            boundRadius{ 0.0f };
			bool             skinned{ false };
		};

		std::mutex                             g_lock;
		std::unordered_map<const void*, Entry> g_cache;

		// thread_local rather than shared: the gather buffer is written while
		// a pointer into it is handed to Fit, so it is state that a second
		// caller on another thread would corrupt.  The cache is shared and
		// is guarded; this is not shared and does not need to be.
		thread_local std::vector<float> g_scratch;

		std::atomic<std::uint32_t> g_fits{ 0 };
		std::atomic<std::uint32_t> g_hits{ 0 };
		std::atomic<std::uint32_t> g_misses{ 0 };

		float ReadF32(const std::uint8_t* a_p)
		{
			float v = 0.0f;
			std::memcpy(&v, a_p, sizeof(v));
			return v;
		}

		// Gather positions at one candidate layout into a tight xyz run.
		//
		// `a_step` of one gathers every vertex; a larger step subsamples for
		// the ranking pass.  The count returned is what was actually written,
		// which is what Fit() is told to read - a subsample that happened to
		// come up short must not leave Fit reading past the end.
		int Collect(const std::uint8_t* a_data, int a_count, const Layout& a_layout,
			int a_step, std::vector<float>& a_out)
		{
			a_out.clear();

			if (!a_data || a_count <= 0 || a_step <= 0 ||
				a_layout.stride < kMinStride) {
				return 0;
			}

			// Room for the vertices the loop below can reach, so the vector
			// does not reallocate while a pointer into it is held.
			a_out.reserve(static_cast<std::size_t>((a_count + a_step - 1) / a_step) * 3);

			int n = 0;
			for (int i = 0; i < a_count; i += a_step) {
				const std::uint8_t* p = a_data +
					static_cast<std::size_t>(i) * a_layout.stride + a_layout.offset;
				a_out.push_back(ReadF32(p));
				a_out.push_back(ReadF32(p + 4));
				a_out.push_back(ReadF32(p + 8));
				++n;
			}

			return n;
		}

		// The candidate readings of one vertex, in the order they are tried.
		//
		// Only the first is the documented reading.  The other three are the
		// ways that reading can be wrong without anything crashing: the offset
		// field meaning something else, the stride calculation missing a flag,
		// and a vertex with no attributes after its position.  Which one wins
		// is decided by agreement with the engine's own bounding sphere rather
		// than by which one looks right, and the winner's name goes to the log
		// so the real layout is known rather than assumed.
		// GetSize() is not a const member, so the descriptor arrives by value
		// rather than by reference.  It is eight bytes.
		int CandidateLayouts(RE::BSGraphics::VertexDesc a_desc, Layout (&a_out)[4])
		{
			const auto size = static_cast<std::uint32_t>(a_desc.GetSize());
			const auto pos = a_desc.GetAttributeOffset(
				RE::BSGraphics::Vertex::Attribute::VA_POSITION);

			int n = 0;
			a_out[n++] = Layout{ pos, size, "desc" };
			a_out[n++] = Layout{ 0, size, "size0" };
			a_out[n++] = Layout{ 0, 16, "pos16" };
			a_out[n++] = Layout{ 16, size, "off16" };

			// Drop any candidate that repeats an earlier one, so a mesh whose
			// offset field reads zero does not spend the ranking pass on the
			// same bytes twice.
			for (int i = 0; i < n; ++i) {
				for (int j = i + 1; j < n; ++j) {
					if (a_out[i].offset == a_out[j].offset &&
						a_out[i].stride == a_out[j].stride) {
						for (int k = j; k < n - 1; ++k) {
							a_out[k] = a_out[k + 1];
						}
						--n;
						--j;
					}
				}
			}

			return n;
		}

		// Fit one geometry, or say why it could not be.  Cache-assisted: this
		// is the only place that ever reads vertices, and it reads them once.
		//
		// `a_hit` says whether the answer came out of the cache.  It is
		// reported rather than inferred from the cache's contents, because by
		// the time this returns a miss has already inserted its entry and the
		// cache therefore contains the mesh either way - a lookup that asked
		// the cache would answer yes for every mesh and tell nobody anything.
		bool FitGeometry(RE::BSGeometry* a_geometry, Entry& a_out, bool& a_hit)
		{
			a_out = Entry{};
			a_hit = false;

			if (!a_geometry) {
				return false;
			}

			auto* tri = a_geometry->AsTriShape();
			if (!tri) {
				return false;
			}

			auto&       runtime = a_geometry->GetGeometryRuntimeData();
			const auto  count = tri->GetTrishapeRuntimeData().vertexCount;

			if (count < 3) {
				return false;
			}

			auto* renderer = runtime.rendererData;
			if (!renderer || !renderer->rawVertexData) {
				// The CPU copy is not there.  Not an error - a mesh that has
				// not been built yet has none - so nothing is cached and the
				// next frame tries again.
				return false;
			}

			const auto* data = renderer->rawVertexData;

			{
				std::scoped_lock lock{ g_lock };
				const auto       it = g_cache.find(a_geometry);
				if (it != g_cache.end() &&
					it->second.vertexData == data &&
					it->second.vertices == count) {
					a_out = it->second;
					++g_hits;
					a_hit = true;
					return a_out.local.valid;
				}
			}

			++g_misses;

			const auto& desc = runtime.vertexDesc;
			if (!desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX)) {
				return false;
			}

			Layout candidates[4]{};
			const int candidateCount = CandidateLayouts(desc, candidates);
			if (candidateCount <= 0) {
				return false;
			}

			const auto& bound = a_geometry->GetModelData().modelBound;

			const int step = count > static_cast<std::uint16_t>(kProbeSamples) ?
			                     count / kProbeSamples :
			                     1;

			// Rank the candidates on a subsample and keep the one whose box
			// comes closest to the engine's bound.
			const Layout* winner = nullptr;
			float         winnerError = -1.0f;
			MeshShape::Local winnerLocal{};

			for (int i = 0; i < candidateCount; ++i) {
				const int gathered = Collect(data, count, candidates[i], step, g_scratch);
				if (gathered < 3) {
					continue;
				}

				MeshShape::Local local{};
				if (!MeshShape::Fit(g_scratch.data(), gathered, local)) {
					continue;
				}

				float error = -1.0f;
				if (!MeshShape::BoundAgrees(local, bound.center.x, bound.center.y,
						bound.center.z, bound.radius, kAcceptTolerance, error)) {
					continue;
				}

				if (!winner || error < winnerError) {
					winner = &candidates[i];
					winnerError = error;
					winnerLocal = local;
				}
			}

			if (!winner) {
				return false;
			}

			// The winner was chosen on a subsample; fit it properly over the
			// whole mesh so the box, the axis and the width bands are built
			// from every vertex rather than from ninety-six of them.
			MeshShape::Local local = winnerLocal;
			float            error = winnerError;

			const int all = count > static_cast<std::uint16_t>(kMaxVertices) ?
			                    kMaxVertices :
			                    count;

			const int gathered = Collect(data, all, *winner, 1, g_scratch);
			if (gathered >= 3) {
				MeshShape::Local full{};
				if (MeshShape::Fit(g_scratch.data(), gathered, full) &&
					MeshShape::BoundAgrees(full, bound.center.x, bound.center.y,
						bound.center.z, bound.radius, kAcceptTolerance, error)) {
					local = full;
				}
			}

			a_out.local = local;
			a_out.vertexData = data;
			a_out.layout = winner->name;
			a_out.error = error;
			a_out.vertices = count;
			a_out.boundRadius = bound.radius;
			a_out.skinned = runtime.skinInstance != nullptr;

			{
				std::scoped_lock lock{ g_lock };
				if (g_cache.size() >= kMaxEntries) {
					// A mesh's shape never changes, so there is nothing worth
					// ageing out one entry at a time.  Dropping the lot costs
					// one refit per mesh still in view and keeps the cap.
					g_cache.clear();
				}
				g_cache[a_geometry] = a_out;
			}

			++g_fits;
			return a_out.local.valid;
		}

		// Where one fitted mesh sits this frame.
		bool PlaceEntry(const Entry& a_entry, RE::BSGeometry* a_geometry, MeshShape::World& a_out)
		{
			const RE::NiTransform& world = a_geometry->world;

			const auto toWorld = [&world](float a_x, float a_y, float a_z,
									  float& a_wx, float& a_wy, float& a_wz) {
				// The engine's own transform, so this file never has to know
				// whether NiMatrix3 is row or column major.
				const RE::NiPoint3 p = world * RE::NiPoint3{ a_x, a_y, a_z };
				a_wx = p.x;
				a_wy = p.y;
				a_wz = p.z;
				return std::isfinite(a_wx) && std::isfinite(a_wy) && std::isfinite(a_wz);
			};

			return MeshShape::Place(a_entry.local, toWorld, a_out);
		}
	}

	RE::NiAVObject* SceneObject(RE::bhkNiCollisionObject* a_object)
	{
		return a_object ? a_object->sceneObject : nullptr;
	}

	bool Measure(RE::NiAVObject* a_root, Result& a_out)
	{
		a_out = Result{};

		if (!a_root) {
			return false;
		}

		struct Found
		{
			MeshShape::World world{};
			MeshShape::Local local{};
			const char*      layout{ "none" };
			float            error{ -1.0f };
			int              vertices{ 0 };
			float            boundRadius{ 0.0f };
			bool             skinned{ false };
			float            diagonal{ 0.0f };
		};

		std::vector<Found> found;
		found.reserve(kMaxMeshes);

		int hits = 0;

		RE::BSVisit::TraverseScenegraphGeometries(
			a_root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				if (found.size() >= kMaxMeshes) {
					return RE::BSVisit::BSVisitControl::kStop;
				}

				Entry entry{};
				bool  hit = false;
				if (!FitGeometry(a_geometry, entry, hit)) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}

				MeshShape::World world{};
				if (!PlaceEntry(entry, a_geometry, world)) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}

				if (hit) {
					++hits;
				}

				Found f{};
				f.world = world;
				f.local = entry.local;
				f.layout = entry.layout;
				f.error = entry.error;
				f.vertices = entry.vertices;
				f.boundRadius = entry.boundRadius;
				f.skinned = entry.skinned;
				f.diagonal = std::sqrt(world.ex * world.ex + world.ey * world.ey +
									   world.ez * world.ez);
				found.push_back(f);

				return RE::BSVisit::BSVisitControl::kContinue;
			});

		if (found.empty()) {
			return false;
		}

		// The dominant mesh is the one with the largest box.  Its axis, its
		// half length and its width bands describe the object; the others are
		// attachments - a string, an arrow - and contribute their extent to
		// the union box but not their shape.
		std::size_t dominant = 0;
		float       lowX = found[0].world.lowestX;
		float       lowY = found[0].world.lowestY;
		float       lowZ = found[0].world.lowestZ;

		float minX = found[0].world.cx - found[0].world.ex;
		float minY = found[0].world.cy - found[0].world.ey;
		float minZ = found[0].world.cz - found[0].world.ez;
		float maxX = found[0].world.cx + found[0].world.ex;
		float maxY = found[0].world.cy + found[0].world.ey;
		float maxZ = found[0].world.cz + found[0].world.ez;

		for (std::size_t i = 0; i < found.size(); ++i) {
			const auto& w = found[i].world;

			if (w.lowestZ < lowZ) {
				lowX = w.lowestX;
				lowY = w.lowestY;
				lowZ = w.lowestZ;
			}

			if (w.cx - w.ex < minX) { minX = w.cx - w.ex; }
			if (w.cy - w.ey < minY) { minY = w.cy - w.ey; }
			if (w.cz - w.ez < minZ) { minZ = w.cz - w.ez; }
			if (w.cx + w.ex > maxX) { maxX = w.cx + w.ex; }
			if (w.cy + w.ey > maxY) { maxY = w.cy + w.ey; }
			if (w.cz + w.ez > maxZ) { maxZ = w.cz + w.ez; }

			if (found[i].diagonal > found[dominant].diagonal) {
				dominant = i;
			}
		}

		const auto& d = found[dominant];

		a_out.world = d.world;
		a_out.local = d.local;
		a_out.lowX = lowX;
		a_out.lowY = lowY;
		a_out.lowZ = lowZ;
		a_out.unionX = 0.5f * (maxX - minX);
		a_out.unionY = 0.5f * (maxY - minY);
		a_out.unionZ = 0.5f * (maxZ - minZ);

		// How much lower the node's lowest point is than the dominant mesh's
		// own centre line.
		//
		// The axis walk samples that centre line, so the only crossings it can
		// see are the line's.  For a bow, whose limbs hang below its middle,
		// the line can stay above the snow through the whole walk while the
		// limbs are already in it - the log has exactly that case, `lowest
		// -10.05 above land` with no buried stretch found.  This is the
		// distance between the two questions, so the caller can hand the walk
		// a line that represents the part that actually touches.
		//
		// The distance is a *thickness*, and the earlier version of this line
		// was not.  It read `d.world.cz - lowZ`: the dominant centre's height
		// minus the lowest corner's height, with the corner taken wherever it
		// happened to be.  For a long weapon at a slant those two are far
		// apart for a reason that has nothing to do with thickness - the
		// weapon's own length is spread in z - so a 68-unit object with a
		// 4.9-unit hull returned a drop of 52.20, which lowered every sample
		// of the walk by 52.20 and made the weapon read as buried along its
		// whole length whatever it was doing.  Measured on the object's own
		// axis instead, the same object's drop is its thickness.
		float dropT = 0.0f;
		a_out.lowOffset = MeshShape::DropAt(lowX, lowY, lowZ,
			d.world.cx, d.world.cy, d.world.cz,
			d.world.ax, d.world.ay, d.world.az, d.world.halfLength, dropT);
		if (!std::isfinite(a_out.lowOffset) || a_out.lowOffset < 0.0f) {
			a_out.lowOffset = 0.0f;
		}
		a_out.layout = d.layout;
		a_out.error = d.error;
		a_out.geometries = static_cast<int>(found.size());
		a_out.vertices = d.vertices;
		a_out.cacheHits = hits;
		a_out.boundRadius = d.boundRadius;
		a_out.skinned = d.skinned;
		a_out.valid = true;
		return true;
	}

	void Stats(std::uint32_t& a_fits, std::uint32_t& a_hits, std::uint32_t& a_misses,
		std::size_t& a_entries)
	{
		a_fits = g_fits.load();
		a_hits = g_hits.load();
		a_misses = g_misses.load();

		std::scoped_lock lock{ g_lock };
		a_entries = g_cache.size();
	}

	void Clear()
	{
		std::scoped_lock lock{ g_lock };
		g_cache.clear();
	}
}
