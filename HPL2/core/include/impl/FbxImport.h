/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_FBX_IMPORT_H
#define HPL_FBX_IMPORT_H

#include <cstdint>
#include <string>
#include <vector>

// FBX -> plain data, with no engine types. This is the part of the FBX loader
// that reproduces the KFbx-era cMeshLoaderFBX (AMFP's pipeline, whose .msh/.anm
// caches ship with the game), built on ufbx instead of the Autodesk SDK.
// cMeshLoaderFBX turns the result into cMesh/cSkeleton/cAnimation; keeping this
// half engine-free lets it be checked against the retail caches headlessly.
namespace hpl::fbx {

	// Matrices are HPL cMatrixf layout: row-major, column vectors, translation
	// in m[3], m[7], m[11].
	struct Matrix { float m[16]; };

	struct Bone {
		std::string name;
		int parent = -1;		// index into Skeleton::bones, -1 = child of the root bone
		Matrix local;			// rest transform relative to the parent
		Matrix localUnscaled;	// rotation-only rest transform, used to make keyframes relative
		bool linked = false;	// has a skin cluster (unlinked bones keep an identity local)
	};

	// Bones in creation order (depth first, file order), which is also the
	// order cSkeleton assigns bone indices in.
	struct Skeleton { std::vector<Bone> bones; };

	struct VertexBonePair { int vtx; int bone; float weight; };

	struct SubMesh {
		std::string name;
		std::string material;			// material/texture base name, no extension
		std::vector<float> positions;	// xyz per vertex
		std::vector<float> normals;		// xyz per vertex
		std::vector<float> texcoords;	// uv0 as (u, 1-v, 0) per vertex
		std::vector<float> colors;		// rgba per vertex
		std::vector<uint32_t> indices;	// triangles, engine (clockwise) winding
		std::vector<VertexBonePair> bonePairs;
	};

	struct KeyFrame { float time; float trans[3]; float rot[4]; };	// rot = x y z w

	struct Track {
		std::string name;
		int flags = 0;		// tAnimTransformFlag bits
		std::vector<KeyFrame> keys;
	};

	struct Animation {
		std::string name;
		float length = 0;
		std::vector<Track> tracks;
	};

	struct Scene {
		bool hasSkeleton = false;
		Skeleton skeleton;
		std::vector<SubMesh> subMeshes;
		bool hasAnimation = false;
		Animation animation;
		std::vector<std::string> warnings;
	};

	enum ImportFlags : uint32_t {
		ImportFlag_Meshes		= 1u << 0,	// build sub meshes (LoadMesh)
		ImportFlag_Animation	= 1u << 1,	// build the first non-empty take (LoadAnimation)
	};

	// Returns false (with asError set) if the file can't be parsed.
	bool Import(const std::string& asPath, uint32_t aFlags, Scene& aOut, std::string& asError);

	// tAnimTransformFlag values, mirrored so this header needs no engine include.
	constexpr int kAnimFlag_Translate = 1;
	constexpr int kAnimFlag_Rotate = 2;
	constexpr int kAnimFlag_Scale = 4;
}

#endif // HPL_FBX_IMPORT_H
