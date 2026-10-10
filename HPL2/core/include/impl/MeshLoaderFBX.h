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

#ifndef HPL_MESH_LOADER_FBX_H
#define HPL_MESH_LOADER_FBX_H

#include "resources/MeshLoader.h"

namespace hpl {

	class cMesh;
	class cAnimation;

	// Loads .fbx meshes, skeletons and animations (AMFP's characters and all of
	// their animations). Parsing lives in impl/FbxImport; this class only builds
	// the engine objects. It always reads the .fbx: many AMFP animations have no
	// .anm cache, and the caches that exist were made by the same rules.
	class cMeshLoaderFBX : public iMeshLoader
	{
	public:
		cMeshLoaderFBX();
		~cMeshLoaderFBX();

		cMesh* LoadMesh(const tWString& asFile, tMeshLoadFlag aFlags);
		bool SaveMesh(cMesh* apMesh, const tWString& asFile) { return false; }

		cAnimation* LoadAnimation(const tWString& asFile);
		bool SaveAnimation(cAnimation* apAnimation, const tWString& asFile) { return false; }
	};

};
#endif // HPL_MESH_LOADER_FBX_H
