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

#ifndef HPL_WORLD_LOADER_HPL_MAP_H
#define HPL_WORLD_LOADER_HPL_MAP_H

#include <map>
#include <set>
#include <vector>
#include "resources/WorldLoader.h"

#include "resources/ResourcesTypes.h"
#include "scene/SceneTypes.h"
#include "graphics/GraphicsTypes.h"
#include "physics/PhysicsTypes.h"

namespace tinyxml2 { class XMLElement; }

namespace hpl {

	class iEntity3D;
	class iPhysicsWorld;
	class cRenderableContainer_BoxTree;
	class cSubMeshEntity;
	class iPhysicsMaterial;
	class cResourceVarsObject;
	class iPhysicsBody;

	//----------------------------------------

#if (defined(__ppc__) || defined(__PPC__)) && !defined(__LP64__)
	// PPC here
	#define MAP_CACHE_FORMAT_MAGIC_NUMBER		0xF4414550
#else
	// Only need to specialize PPC as 64bit and 32bit newton data is the same
	#define MAP_CACHE_FORMAT_MAGIC_NUMBER		0xF441451F
#endif

	// buzer: set it to some arbitrary large number so it won't interfere with other source mods
	// Rebuild caches containing unnormalized, scaled normals/tangents whose
	// signed-byte encoding could overflow (notably the Transept floor emblem).
	#define MAP_CACHE_FORMAT_VERSION			219676931
	
	//----------------------------------------
	
	class cHplMapStaticUserData
	{
	public:
		cVector3f mvScale;
		bool mbCollides;
		bool mbCharCollider;
		bool mbCombine;
		bool mbVisible;
	};

	typedef std::list<cHplMapStaticUserData*> tHplMapStaticUserDataList;
	typedef tHplMapStaticUserDataList::iterator tHplMapStaticUserDataListIt;

	
	//----------------------------------------
	
	class cHplMapPhysicsObject
	{
	public:
		cSubMeshEntity *mpObject;
		iPhysicsMaterial *mpPhysicsMaterial;
		cHplMapStaticUserData *mpUserData;
		bool mbCharCollider;
	};

	//----------------------------------------

	class cHplMapShape
	{
	public:
		eCollideShapeType mType;
		cVector3f mvSize;
		cMatrixf m_mtxOffset;
	};

	typedef std::vector<cHplMapShape*> tHplMapShapeVec;
	typedef tHplMapShapeVec::iterator tHplMapShapeVecIt;

	//----------------------------------------
	
	class cHplMapShapeBody
	{
	public:
		cHplMapShapeBody();
		~cHplMapShapeBody();

		tString msMaterial;
		bool mbCharCollider;
		bool mbBlocksLight;
		cMatrixf m_mtxTransform;
		
		tHplMapShapeVec mvColliders;
	};
	
	typedef std::list<cHplMapShapeBody*> tHplMapShapeBodyList;
	typedef tHplMapShapeBodyList::iterator tHplMapShapeBodyListIt;

	//----------------------------------------

	
	//-------------------------------------------------------

	// A named map object as the renderer mask load gate saw it.
	struct cRendererMaskObjectRecord
	{
		int mlID;
		tString msName; // lower case
		tString msKind; // element name; lights by shape, so a retail light matches its ray-traced element
		unsigned mlMask;
	};

	// Pairs each skipped object with the one loaded object of the same name and
	// kind whose mask shares no renderer with it -- the other half of a backend
	// split. Every skipped ID is reported; ambiguous or unmatched ones get no remap.
	void BuildRendererMaskIDRemap(	const std::vector<cRendererMaskObjectRecord>& avSkipped,
									const std::vector<cRendererMaskObjectRecord>& avLoaded,
									std::set<int>& aSkippedIDs, std::map<int,int>& aRemap);

	//-------------------------------------------------------

	class cWorldLoaderHplMap : public iWorldLoader
	{
	public:
		cWorldLoaderHplMap();
		~cWorldLoaderHplMap();

			
		cWorld* LoadWorld(const tWString& asFile, tWorldLoadFlag aFlags);

	private:
		unsigned long long ApplyMapDeltas(const tWString& asFile, tinyxml2::XMLElement* apXmlMapData);

		bool HasRendererMaskedStaticGeometry(tinyxml2::XMLElement* apXmlMapData);
		void RecordRendererMaskObject(tinyxml2::XMLElement* apElement, bool abLoaded);
		void LoadCacheFile(const tWString& asFile);
		void SaveCacheFile(const tWString& asFile);

		void LoadFileIndicies(tinyxml2::XMLElement* apXmlContents);

		void LoadStaticObjects(tinyxml2::XMLElement* apXmlContents);
		void BuildCombinedStaticMeshes(cRenderableContainer_BoxTree *apContainer);
		void CreateStaticObjectEntity(	tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
										cRenderableContainer_BoxTree *apContainer);
		void CreatePrimitive(tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
							cRenderableContainer_BoxTree *apContainer);
		void CreateDecal(	tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
							cRenderableContainer_BoxTree *apDecalContainer);
		void CreateStaticObjectCombo(tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
									cRenderableContainer_BoxTree *apDecalContainer);

		void IterateLeafNodesAndBuildMeshes(iRenderableContainerNode *apNode);
		void CombineAndCreateMeshesAndPhysics(tRenderableList *apObjectList);
		void CombineObjectsAndCreateMeshEntity(tRenderableVec &avObjects, int alFirstIdx, int alLastIdx);
		void CombineObjectsAndCreatePhysics(std::vector<cHplMapPhysicsObject> &avObjects, int alFirstIdx, int alLastIdx);

		void LoadEntities(tinyxml2::XMLElement* apXmlContents);
		void CreateLoadedEntity(tinyxml2::XMLElement* apElement, tEFL_LightBillboardConnectionList *apLightBillboardList);
		void CreateSubMeshShapeBodies(cSubMeshEntity *apSubEnt, const cMatrixf &a_mtxTransform, const cVector3f& avScale);
		void CreateShapeBody(cHplMapShapeBody* apShapeBody);

		void LoadEntity(const tString& asName, int alID, bool abActive,const cVector3f& avPos, const cVector3f& avRot, const cVector3f& avScale, tinyxml2::XMLElement* apElement);
		void LoadArea(const tString& asName, int alID, bool abActive,const cVector3f& avPos, const cVector3f& avRot,const cVector3f& avScale, tinyxml2::XMLElement* apElement);

		bool CheckTransformValidity(const tString& asName, const cVector3f& avPos, const cVector3f& avRot, const cVector3f& avScale);
		
		int mlCombinedMeshNameCount;
		int mlCombinedBodyNameCount;

		bool mbLoadedCache;

		tStringVec mvFileIndices_StaticObjects;
		tStringVec mvFileIndices_Entities;
		tStringVec mvFileIndices_Decals;

		int mlSortingTimeTotal;
		int mlCombineMeshTimeTotal;
		int mlCombineBodyTimeTotal;

		cWorld* mpCurrentWorld;
		iPhysicsWorld *mpCurrentPhysicsWorld;

		std::list<iPhysicsBody*> mlstStaticMeshBodies;
		tHplMapShapeBodyList mlstStaticShapeBodies;
		tMeshEntityList mlstStaticMeshEntities;
		
		int mlStaticMeshBodiesCreated;
		int mlStaticMeshEntitiesCreated;

		float *mpShortNegPosFloatTable;
		float *mpByteNegPosFloatTable;
		float *mpBytePosFloatTable;

		tWString msCacheFileExt;

		// Non-zero once .map_delta files have patched the map being loaded; keys
		// the cache file and disables the "ship a prebuilt cache" shortcut, which
		// cannot hold for a patched map.
		unsigned long long mlDeltaHash;
		bool mbRendererMaskCache;
		// Static objects skipped for the other renderer backend, so static
		// object combos referencing them stay quiet.
		std::set<int> msetSkippedStaticIDs;
		// Named objects the load gate skipped and loaded, paired up after loading.
		std::vector<cRendererMaskObjectRecord> mvRendererMaskSkipped;
		std::vector<cRendererMaskObjectRecord> mvRendererMaskLoaded;

		tWorldLoadFlag mlCurrentFlags;
		tHplMapStaticUserDataList mlstTempStaticUserData;
	};

};
#endif // HPL_WORLD_LOADER_HPL_MAP_H
