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

#include "impl/MeshLoaderFBX.h"
#include "impl/FbxImport.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"

#include "resources/MaterialManager.h"

#include "graphics/VertexBuffer.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Material.h"
#include "graphics/Skeleton.h"
#include "graphics/Bone.h"
#include "graphics/Animation.h"
#include "graphics/AnimationTrack.h"

#include <cstring>
#include <vector>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMeshLoaderFBX::cMeshLoaderFBX()
	{
		AddSupportedExtension("fbx");
	}

	//-----------------------------------------------------------------------

	cMeshLoaderFBX::~cMeshLoaderFBX()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// FILE-LOCAL HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	static cMatrixf ToMatrix(const fbx::Matrix& aMtx)
	{
		cMatrixf mtx;
		std::memcpy(mtx.v, aMtx.m, sizeof(aMtx.m));
		return mtx;
	}

	//-----------------------------------------------------------------------

	static bool ImportFile(const tWString& asFile, uint32_t aFlags, fbx::Scene& aScene)
	{
		const tString sFile = cString::To8Char(asFile);
		tString sError;
		if(fbx::Import(sFile, aFlags, aScene, sError) == false)
		{
			Error("FBX: failed to load '%s': %s\n", sFile.c_str(), sError.c_str());
			return false;
		}
		for(const std::string& sWarning : aScene.warnings)
			Warning("FBX: '%s': %s\n", sFile.c_str(), sWarning.c_str());
		return true;
	}

	//-----------------------------------------------------------------------

	// Bones come in creation order with parents first, which is also the
	// index order cSkeleton gives them (vertex bone pairs rely on it).
	static cSkeleton* CreateSkeleton(const fbx::Skeleton& aSkeleton)
	{
		cSkeleton* pSkeleton = hplNew(cSkeleton, ());
		std::vector<cBone*> vBones(aSkeleton.bones.size());
		for(size_t i=0; i<aSkeleton.bones.size(); ++i)
		{
			const fbx::Bone& bone = aSkeleton.bones[i];
			cBone* pParent = bone.parent >= 0 ? vBones[bone.parent] : pSkeleton->GetRootBone();
			cBone* pBone = pParent->CreateChildBone(bone.name, bone.name);
			pBone->SetTransform(ToMatrix(bone.local));
			pBone->SetTransformUnscaled(ToMatrix(bone.localUnscaled));
			vBones[i] = pBone;
		}
		return pSkeleton;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMesh* cMeshLoaderFBX::LoadMesh(const tWString& asFile, tMeshLoadFlag aFlags)
	{
		fbx::Scene scene;
		if(ImportFile(asFile, fbx::ImportFlag_Meshes, scene) == false) return NULL;

		cMesh* pMesh = hplNew( cMesh, (cString::To8Char(asFile), asFile, mpMaterialManager, mpAnimationManager) );

		if(scene.hasSkeleton) pMesh->SetSkeleton(CreateSkeleton(scene.skeleton));

		for(const fbx::SubMesh& subData : scene.subMeshes)
		{
			cSubMesh* pSubMesh = pMesh->CreateSubMesh(subData.name);
			pSubMesh->SetModelScale(cVector3f(1,1,1));

			//////////////////////////////
			// Vertex buffer, in the layout the MSH loader produces.
			const int lVtxNum = (int)(subData.positions.size() / 3);
			cVertexBuffer* pVtxBuffer = new cVertexBuffer(eVertexBufferType_Hardware, eVertexBufferDrawType_Tri,
														eVertexBufferUsageType_Static, lVtxNum, (int)subData.indices.size());

			pVtxBuffer->CreateElementArray(eVertexBufferElement_Position, eVertexBufferElementFormat_Float, 4);
			pVtxBuffer->CreateElementArray(eVertexBufferElement_Normal,   eVertexBufferElementFormat_Float, 3);
			pVtxBuffer->CreateElementArray(eVertexBufferElement_Texture0, eVertexBufferElementFormat_Float, 3);
			pVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,   eVertexBufferElementFormat_Float, 4);

			for(int i=0; i<lVtxNum; ++i)
			{
				const float* pPos = &subData.positions[i*3];
				const float* pNrm = &subData.normals[i*3];
				const float* pTex = &subData.texcoords[i*3];
				const float* pCol = &subData.colors[i*4];
				pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Position, cVector3f(pPos[0], pPos[1], pPos[2]));
				pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Normal, cVector3f(pNrm[0], pNrm[1], pNrm[2]));
				pVtxBuffer->AddVertexVec3f(eVertexBufferElement_Texture0, cVector3f(pTex[0], pTex[1], pTex[2]));
				pVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, cColor(pCol[0], pCol[1], pCol[2], pCol[3]));
			}

			for(uint32_t lIdx : subData.indices) pVtxBuffer->AddIndex(lIdx);

			// Tangents are derived, as the MSH loader does; the file's are not used.
			pVtxBuffer->Compile(eVertexCompileFlag_CreateTangents);
			pSubMesh->SetVertexBuffer(pVtxBuffer);

			//////////////////////////////
			// Bone weights
			for(const fbx::VertexBonePair& pair : subData.bonePairs)
			{
				cVertexBonePair vtxBonePair;
				vtxBonePair.vtxIdx = pair.vtx;
				vtxBonePair.boneIdx = pair.bone;
				vtxBonePair.weight = pair.weight;
				pSubMesh->AddVertexBonePair(vtxBonePair);
			}

			//////////////////////////////
			// Material: the name alone, resolved through the resource dirs (as
			// the retail caches store it).
			if(subData.material != "")
			{
				const tString sMaterial = cString::SetFileExt(subData.material, "mat");
				pSubMesh->SetMaterialName(sMaterial);
				if((aFlags & eMeshLoadFlag_NoMaterial) == 0)
				{
					SharedResourceHandle<cMaterial> pMaterial = mpMaterialManager->CreateMaterial(sMaterial);
					if(!pMaterial)
						Error("FBX: couldn't create material '%s' for object '%s'\n", sMaterial.c_str(), subData.name.c_str());
					pSubMesh->SetMaterial(std::move(pMaterial));
				}
			}
			else
			{
				pSubMesh->SetMaterialName("");
			}

			pSubMesh->Compile();
		}

		pMesh->CompileBonesAndSubMeshes();

		return pMesh;
	}

	//-----------------------------------------------------------------------

	cAnimation* cMeshLoaderFBX::LoadAnimation(const tWString& asFile)
	{
		fbx::Scene scene;
		if(ImportFile(asFile, fbx::ImportFlag_Animation, scene) == false) return NULL;

		if(scene.hasAnimation == false)
		{
			Error("FBX: '%s' has no animation\n", cString::To8Char(asFile).c_str());
			return NULL;
		}

		const tString sFile = cString::To8Char(asFile);
		cAnimation* pAnimation = hplNew( cAnimation, (sFile, asFile, cString::GetFileName(sFile)) );
		pAnimation->SetLength(scene.animation.length);
		pAnimation->ReserveTrackNum((int)scene.animation.tracks.size());

		for(const fbx::Track& track : scene.animation.tracks)
		{
			cAnimationTrack* pTrack = pAnimation->CreateTrack(track.name, track.flags);
			for(const fbx::KeyFrame& key : track.keys)
			{
				cKeyFrame* pKeyFrame = pTrack->CreateKeyFrame(key.time);
				pKeyFrame->trans = cVector3f(key.trans[0], key.trans[1], key.trans[2]);
				pKeyFrame->rotation = cQuaternion(key.rot[3], key.rot[0], key.rot[1], key.rot[2]);
			}
		}

		return pAnimation;
	}

	//-----------------------------------------------------------------------
}
