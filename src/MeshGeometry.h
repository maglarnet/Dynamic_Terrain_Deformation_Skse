// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "MeshShape.h"

namespace RE
{
	class BSGeometry;
	class NiAVObject;
	class bhkNiCollisionObject;
}

// Reading the object's own mesh, through the engine types.
//
// MeshShape.h holds the arithmetic and knows nothing about Skyrim; this is the
// half that knows where the vertices live.  The split is the same one
// ContactPoint.h already uses, and for the same reason: the arithmetic has to
// be callable from the offline test, and the vertex walk cannot be.
//
// Where the vertices are:
//
//   bhkNiCollisionObject::sceneObject  -> the NiAVObject the collidable hangs
//                                         off, which is the mesh it stands for
//   BSVisit::TraverseScenegraphGeometries -> the BSGeometry leaves under it, as
//                                         an object can be several meshes (a bow
//                                         and its string, a quiver and its
//                                         arrows)
//   BSGeometry::GetGeometryRuntimeData().rendererData
//                                      -> BSGraphics::TriShape, which holds
//                                         rawVertexData: a CPU-side copy of the
//                                         vertices, so nothing has to be
//                                         locked or read back from the GPU
//   GetTrishapeRuntimeData().vertexCount   -> how many
//   GetGeometryRuntimeData().vertexDesc    -> the stride and the offset of the
//                                         position inside each vertex
//
// Cost, and why it does not matter where it is called:
//
// The mesh fit is cached per geometry and only ever computed once, because a
// mesh's own shape does not change - only the transform over it does.  So the
// expensive half runs on the first frame a mesh is seen and never again, and
// every later frame pays eight point transforms per mesh.
//
// Two callers read meshes now: the carried-weapon path in Clipmap.cpp, and
// the dropped-object path in ObjectStamps.cpp.  The second is bounded the
// same way the first is - it runs on the objects already classified as
// stampable, and the fit is cached per mesh - so a scene full of unmeasured
// objects still costs nothing but the collision walk it already did.
namespace MeshGeometry
{
	// The fitted mesh, in both spaces, plus what the walk had to say about
	// itself.  Every field here is meant to reach the log: a vertex walk whose
	// byte layout was guessed has to be able to report that it guessed, or a
	// wrong stride looks exactly like a right one.
	struct Result
	{
		MeshShape::World world;   // where the dominant mesh is, this frame
		MeshShape::Local local;   // its own shape, which is what WidthOver needs

		// The lowest corner over *all* the meshes under the node, not just the
		// dominant one.  The gate asks whether any part of the object is in the
		// snow, and for a bow that is a question about its limbs as much as
		// about its middle, so the answer comes from the union.
		float lowX{ 0.0f };
		float lowY{ 0.0f };
		float lowZ{ 0.0f };

		// The union box's half extents, so a caller can tell a mesh that was
		// measured from a node that turned out to contain the whole character.
		float unionX{ 0.0f };
		float unionY{ 0.0f };
		float unionZ{ 0.0f };

		// How far below its own centre line the object's lowest point sits.
		//
		// The axis walk follows the dominant mesh's centre line, so it can
		// only see a crossing that the line itself makes.  This is the
		// distance from that line down to the lowest corner of the union box -
		// what the axis walk has to subtract from each of its samples for the
		// question it asks to be the same one the gate asks.  Zero when the
		// dominant mesh is the lowest thing in the node, which is the common
		// case for a rod or a staff and not for a bow.
		float lowOffset{ 0.0f };

		// Which byte layout of a vertex was accepted, as a short label, and
		// how far it was from agreeing with the engine's own bounding sphere.
		// "none" means no layout agreed and the caller should keep using the
		// collision hull.
		const char* layout{ "none" };
		float       error{ -1.0f };

		int   geometries{ 0 };    // meshes found under the node and measured
		int   vertices{ 0 };      // vertices in the dominant one
		int   cacheHits{ 0 };     // of those meshes, how many were already fitted
		float boundRadius{ 0.0f };  // the engine's own bound of the dominant mesh

		// True when the dominant mesh is skinned.  A skinned mesh is deformed
		// on the GPU and its node transform says nothing about where its
		// vertices ended up, so the world box below would be wrong for it and
		// the caller is told rather than left to assume.
		bool skinned{ false };
		bool valid{ false };
	};

	// Measure the meshes under a node.
	//
	// Returns false when nothing could be measured - no geometry underneath,
	// no CPU-side vertices, no layout that agreed with the bounding sphere.  A
	// false is not a failure to report, it is the signal to fall back to the
	// hull, which is what the code did before this existed.
	bool Measure(RE::NiAVObject* a_root, Result& a_out);

	// The node a collidable is attached to.  Null when it has none, which
	// happens for collidables that are not part of a scene graph.
	RE::NiAVObject* SceneObject(RE::bhkNiCollisionObject* a_object);

	// Counters for the log: fits actually performed, lookups served from the
	// cache, lookups that had to fit, and how many meshes are being held.
	void Stats(std::uint32_t& a_fits, std::uint32_t& a_hits, std::uint32_t& a_misses,
		std::size_t& a_entries);

	// Forgets every fitted mesh.  Called when the plugin tears down, so the
	// cache does not outlive the meshes it describes.
	void Clear();
}
