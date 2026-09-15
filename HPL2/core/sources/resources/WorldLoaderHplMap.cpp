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

#include "resources/WorldLoaderHplMap.h"

#include "system/String.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"

#include "resources/Resources.h"
#include "resources/MeshManager.h"
#include "resources/MaterialManager.h"
#include "resources/TextureManager.h"
#include "resources/LowLevelResources.h"
#include "resources/EngineFileLoading.h"
#include "resources/BinaryBuffer.h"
#include "resources/XmlHelper.h"
#include "resources/XmlDelta.h"
#include "scene/LightParameters.h"

#include <tinyxml2.h>

#include "scene/Scene.h"
#include "scene/World.h"
#include "scene/MeshEntity.h"
#include "scene/Decal.h"
#include "scene/SubMeshEntity.h"
#include "scene/Light.h"
#include "scene/BillBoard.h"
#include "scene/SoundEntity.h"
#include "scene/ParticleSystem.h"
#include "scene/RenderableContainer_BoxTree.h"

#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/VertexBuffer.h"
#include "graphics/MeshCreator.h"

#include "physics/Physics.h"
#include "physics/PhysicsWorld.h"
#include "physics/CollideShape.h"
#include "physics/PhysicsBody.h"

#include "math/Math.h"

#include <algorithm>
#include <cstring>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC DATA
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	bool gbLogCacheLoad = false;
	
	static bool gbLog = false;
	static bool gbLogTiming = true;

	// A vertex buffer is usable in the static-mesh combine only if it actually
	// advertises a position stream. CreateCopy() selects elements by the source's
	// flag bits, so a buffer whose position flag is unset (even if a stale
	// position element lingers) yields a copy with no position element and makes
	// GetVertexNum() assert. Mirror the guard already in
	// cVertexBuffer::CreateBoundingVolume().
	static inline bool HasPositionStream(cVertexBuffer *apVtxBuffer)
	{
		return apVtxBuffer && (apVtxBuffer->GetVertexElementFlags() & eVertexElementFlag_Position);
	}

	#define kEncryptKey 0x4E5F16F0

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cHplMapShapeBody::cHplMapShapeBody()
	{

	}
	
	cHplMapShapeBody::~cHplMapShapeBody()
	{
		STLDeleteAll(mvColliders);
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cWorldLoaderHplMap::cWorldLoaderHplMap()
	{
		AddSupportedExtension("map");
		AddSupportedExtension("cmap");

		mpCurrentWorld = NULL;
		mpCurrentPhysicsWorld = NULL; 
		mlDeltaHash = 0;

		///////////////////////////////////
		// Generate some tables used by map loading
		mpShortNegPosFloatTable = hplNewArray(float, 0xFFFF + 2);
		mpByteNegPosFloatTable = hplNewArray(float, 257);
		mpBytePosFloatTable = hplNewArray(float, 257);

		///////////////////////////////////
		// Short to float conversion
		for(int i=0; i<=0xFFFF; ++i)
		{
			short lX = *((short*)&i);
			float fX = ((float)lX);
            mpShortNegPosFloatTable[i] = fX / 32767.0f;			
		}

		///////////////////////////////////
		// Signed char to float
		for(int i=0; i<=255; ++i)
		{
			char lX = *((char*)&i);
			float fX = ((float)lX);
			mpByteNegPosFloatTable[i] = fX / 127.0f;		
		}

		///////////////////////////////////
		// Unsigned char to float
		for(int i=0; i<=255; ++i)
		{
			float fX = (float)i;
			mpBytePosFloatTable[i] = fX / 255.0f;		
		}
	}
	
	//-----------------------------------------------------------------------

	cWorldLoaderHplMap::~cWorldLoaderHplMap()
	{
		hplDeleteArray(mpShortNegPosFloatTable);
		hplDeleteArray(mpByteNegPosFloatTable);
		hplDeleteArray(mpBytePosFloatTable);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cWorld* cWorldLoaderHplMap::LoadWorld(const tWString& asFile,tWorldLoadFlag aFlags)
	{
		unsigned long lLoadStartTime = cPlatform::GetApplicationTime();
		mlCurrentFlags = aFlags;
		bool bLoadedFromNormalFile=false;

		///////////////////////
		//Load the map file
		tinyxml2::XMLDocument xmlDoc;
		tWString sExt = cString::ToLowerCaseW(cString::GetFileExtW(asFile));
		if(sExt != _W("cmap"))
		{
			if(LoadXmlFile(xmlDoc, asFile)==false)
			{
				return NULL;
			}
			bLoadedFromNormalFile = true;
		}
		///////////////////////
		//Load compressed
		else if(cResources::GetCreateAndLoadCompressedMaps())
		{
			cBinaryBuffer compBuffer;
			if(compBuffer.Load(asFile)==false)
			{
				//Log("Could not load compressed map!\n");
				return NULL;
			}

			int lKey = kEncryptKey;
			compBuffer.XorTransform((char*)&lKey, sizeof(lKey));

			cBinaryBuffer textBuff;
			if(textBuff.DecompressAndAddFromBuffer(&compBuffer, false)==false)
			{
				//Log("Could not decompress map!\n");
				return NULL;
			}

			if(xmlDoc.Parse(textBuff.GetDataPointer())!=tinyxml2::XML_SUCCESS)
			{
				//Log("Could not parse map!\n");
				return NULL;
			}
		}

		///////////////////////
		//Save to compressed
		if(cResources::GetCreateAndLoadCompressedMaps() && bLoadedFromNormalFile)
		{
			tWString sCompFile = cString::SetFileExtW(asFile,_W("cmap"));

			//Only recreate if file does not exist or if out of date.
			if(	cPlatform::FileExists(sCompFile)==false || 
				cPlatform::FileModifiedDate(sCompFile) < cPlatform::FileModifiedDate(asFile))
			{
				tinyxml2::XMLPrinter printer;
				xmlDoc.Print(&printer);
				tString sData = printer.CStr();

				cBinaryBuffer textBuff;
				textBuff.AddCharArray(sData.c_str(), sData.size()+1);

				cBinaryBuffer compBuff;
				compBuff.CompressAndAdd(textBuff.GetDataPointer(), textBuff.GetSize());
				
				int lKey = kEncryptKey;
				compBuff.XorTransform((char*)&lKey, sizeof(lKey));

				compBuff.Save(sCompFile);
			}
		}

		////////////////////////////////////
		// Read map data (name for now). The map root (<Level> for editor-saved
		// maps) wraps the actual data — the legacy iXmlDocument wrapper exposed
		// the root element's children, so "MapData" was found one level down.
		tinyxml2::XMLElement* pXmlMapData =
			xmlDoc.RootElement() ? xmlDoc.RootElement()->FirstChildElement("MapData") : NULL;
		if(pXmlMapData==NULL)
		{
			Error("File '%s' has no MapData element!\n", cString::To8Char(asFile).c_str());
			return NULL;
		}

		////////////////////////////////////
		// Apply any .map_delta overlays to the parsed tree.
		//
		// This happens after the .cmap round trip above (so patched content is
		// never baked into a compressed map) and before anything reads the tree,
		// so every per-object loader below stays delta-unaware. The file on disk
		// is untouched -- only this in-memory document is patched.
		mlDeltaHash = ApplyMapDeltas(asFile, pXmlMapData);

		////////////////////////////////
		// Init general vars
		mbLoadedCache= false;
		mbRendererMaskCache = false;
		mvRendererMaskSkipped.clear();
		mvRendererMaskLoaded.clear();
		mlstStaticMeshBodies.clear();
		mlstStaticMeshEntities.clear();
		mlStaticMeshBodiesCreated = 0;
		mlStaticMeshEntitiesCreated =0;

		msCacheFileExt = _W("map_cache");
		if( (mlCurrentFlags & eWorldLoadFlag_FastPhysicsLoad) || (mlCurrentFlags & eWorldLoadFlag_FastStaticLoad) )
		{
			msCacheFileExt = _W("map_cache_fastload");
		}

		//////////////////////////////////////////////
		// The cache holds baked combined static meshes and collision data, so a
		// delta that touches static geometry invalidates it. Key the cache on the
		// contents of the delta set: each combination of mods gets its own cache
		// file, and editing a delta changes the hash rather than going stale.
		if(mlDeltaHash!=0)
		{
			char vHashBuf[32];
			snprintf(vHashBuf, sizeof(vHashBuf), "_d%016llx", mlDeltaHash);
			msCacheFileExt += cString::To16Char(vHashBuf);
		}
		
		
		////////////////////////////////
		// Init some timing variables
		unsigned long lStartTime;
		unsigned long lDeltaTime;

		mlSortingTimeTotal =0;
		mlCombineMeshTimeTotal=0;
		mlCombineBodyTimeTotal=0;
		
		
		if(gbLogTiming) Log(" -------- Loading map '%s' ---------\n", cString::To8Char(cString::GetFileNameW(asFile)).c_str());

		///////////////////////
		//Create world and set up physics world with default values
		mpCurrentWorld = mpScene->CreateWorld(cString::To8Char(cString::GetFileNameW(asFile)));
		mpCurrentWorld->SetFilePath(asFile);

		mpCurrentPhysicsWorld = mpPhysics->CreateWorld(true);
        
		//Set up some default values.
		mpCurrentPhysicsWorld->SetAccuracyLevel(ePhysicsAccuracy_Medium);
		mpCurrentPhysicsWorld->SetWorldSize(-300,300);
		mpCurrentPhysicsWorld->SetMaxTimeStep(1.0f / 60.0f);

		mpCurrentWorld->SetPhysicsWorld(mpCurrentPhysicsWorld);

		//////////////////////////////////////////////
		// Static geometry tagged for one renderer is skipped on the other, so the
		// baked combined meshes differ per backend: such a map keeps one cache
		// per backend. Without the load filter (tools) everything loads and the
		// plain cache is used.
		if(cResources::GetRendererMaskFilterEnabled() && HasRendererMaskedStaticGeometry(pXmlMapData))
		{
			msCacheFileExt += mpCurrentWorld->GetRendererMaskBit()==kRendererMaskStandard ? _W("_rs") : _W("_ro");
			mbRendererMaskCache = true;
		}
		
		////////////////////////////////////
		// Try loading cache
		LoadCacheFile(asFile);


		////////////////////////////////////
		// Load fog
		mpCurrentWorld->SetFogActive(GetAttributeBool(pXmlMapData, "FogActive", false));
		mpCurrentWorld->SetFogColor(GetAttributeColor(pXmlMapData, "FogColor", cColor(1,1) ));
		mpCurrentWorld->SetFogFalloffExp(GetAttributeFloat(pXmlMapData, "FogFalloffExp", 1.0f ));
		mpCurrentWorld->SetFogStart(GetAttributeFloat(pXmlMapData, "FogStart", 0.0f ) );
		mpCurrentWorld->SetFogEnd(GetAttributeFloat(pXmlMapData, "FogEnd", 0.0f ) );
		mpCurrentWorld->SetFogCulling(GetAttributeBool(pXmlMapData, "FogCulling", true ) );

		////////////////////////////////////
		// Load skybox
		mpCurrentWorld->SetSkyBoxActive(GetAttributeBool(pXmlMapData, "SkyBoxActive", false) );
		mpCurrentWorld->SetSkyBoxColor(GetAttributeColor(pXmlMapData, "SkyBoxColor", cColor(1,1)) );

		tString sSkyBoxTexture = GetAttributeString(pXmlMapData, "SkyBoxTexture","");
		if(sSkyBoxTexture!="")
		{
			Image *pSkyBoxImage = mpResources->GetTextureManager()->CreateCubeMapImage(sSkyBoxTexture,false).Release();
			if(pSkyBoxImage) mpCurrentWorld->SetSkyBox(pSkyBoxImage, true);
		}


		//////////////////////////////////////////////
		// Load map contents
		tinyxml2::XMLElement* pXmlContents = pXmlMapData->FirstChildElement("MapContents");
		if(pXmlContents==NULL)
		{
			return NULL;
		}

		///////////////////////////////////
		// Load File Indices
		LoadFileIndicies(pXmlContents);

		///////////////////////////////////
		// Load Static objects
		if(mbLoadedCache==false)
		{
			lStartTime = cPlatform::GetApplicationTime();
			LoadStaticObjects(pXmlContents);
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("  Static Objects: %d ms\n", lDeltaTime);
		}
		
		
		//////////////////////////////
		// Load rest of entities
		if( (mlCurrentFlags & eWorldLoadFlag_NoEntities)==0)
		{
			lStartTime = cPlatform::GetApplicationTime();
			LoadEntities(pXmlContents);
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("  Entities: %d ms\n", lDeltaTime);
		}
			
		//////////////////////////////
		// Compile
		lStartTime = cPlatform::GetApplicationTime();
		mpCurrentWorld->Compile(true);
		lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
		if(gbLogTiming) Log("  Compilation: %d ms\n", lDeltaTime);

		//////////////////////////////
		// Save cache
		SaveCacheFile(asFile);

		//////////////////////////////
		// Final clean up
		STLDeleteAll(mlstStaticShapeBodies);

		lDeltaTime = cPlatform::GetApplicationTime() - lLoadStartTime;
		if(gbLogTiming) Log("  Total: %d ms\n", lDeltaTime);

		if(gbLogTiming) Log("  Meshes created: %d\n", mlStaticMeshEntitiesCreated);
		if(gbLogTiming) Log("  Bodies created: %d\n", mlStaticMeshBodiesCreated);
	
		if(gbLogTiming) Log(" -------- Loading complete ---------\n");

		return mpCurrentWorld;
	}

	//-----------------------------------------------------------------------
	
	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	static void* GetVertexBufferWithFormat(cVertexBuffer *apVtxBuffer, eVertexBufferElement aElement, eVertexBufferElementFormat aFormat)
	{
		switch(aFormat)
		{
		case eVertexBufferElementFormat_Int:		return (void*)apVtxBuffer->GetIntArray(aElement);
		case eVertexBufferElementFormat_Float:		return (void*)apVtxBuffer->GetFloatArray(aElement);
		case eVertexBufferElementFormat_Byte:		return (void*)apVtxBuffer->GetByteArray(aElement);
		}

		Error("Vertex buffer has incorrect format when getting vertexbuffer data during loading of MSH file!\n");
		return NULL;
	}

	//-----------------------------------------------------------------------

	static void AddBinaryBufferDataWithFormat(cBinaryBuffer* apBuffer, void *apSrcData, size_t alSize, eVertexBufferElementFormat aFormat)
	{
		switch(aFormat)
		{
		case eVertexBufferElementFormat_Int:
			apBuffer->AddInt32Array((int*)apSrcData, alSize);
			break;
		case eVertexBufferElementFormat_Float:
			apBuffer->AddFloat32Array((float*)apSrcData, alSize);
			break;
		case eVertexBufferElementFormat_Byte:		
			apBuffer->AddCharArray((char*)apSrcData, alSize);
			break;
		default:
			Error("Vertex buffer has incorrect format when adding binary data during loading of MSH file!\n");
			break;
		}
	}

	//-----------------------------------------------------------------------

	static void GetBinaryBufferDataWithFormat(cBinaryBuffer* apBuffer, void *apDestData, size_t alSize, eVertexBufferElementFormat aFormat)
	{
		switch(aFormat)
		{
		case eVertexBufferElementFormat_Int:
			apBuffer->GetInt32Array((int*)apDestData, alSize);
			break;
		case eVertexBufferElementFormat_Float:
			apBuffer->GetFloat32Array((float*)apDestData, alSize);
			break;
		case eVertexBufferElementFormat_Byte:		
			apBuffer->GetCharArray((char*)apDestData, alSize);
			break;
		default:
			Error("Vertex buffer has incorrect format when getting binary data during loading of MSH file!\n");
			break;
		}
	}
	
	//-----------------------------------------------------------------------

	unsigned long long cWorldLoaderHplMap::ApplyMapDeltas(const tWString& asFile, tinyxml2::XMLElement* apXmlMapData)
	{
		tWStringVec vDeltaFiles;
		if(mpResources->FindDeltaFiles(asFile, "map_delta", vDeltaFiles)==false) return 0;

		int lNextAddID = HPL_XML_DELTA_FIRST_ADD_ID;
		cXmlDeltaStats totalStats;
		int lApplied =0;

		for(size_t i=0; i<vDeltaFiles.size(); ++i)
		{
			tinyxml2::XMLDocument deltaDoc;
			if(LoadXmlFile(deltaDoc, vDeltaFiles[i])==false)
			{
				Error("Could not load map delta '%s'!\n", cString::To8Char(vDeltaFiles[i]).c_str());
				continue;
			}

			cXmlDeltaStats stats;
			if(ApplyXmlDelta(apXmlMapData, deltaDoc.RootElement(), lNextAddID, stats)==false)
			{
				Error("Could not apply map delta '%s'!\n", cString::To8Char(vDeltaFiles[i]).c_str());
				continue;
			}

			Log("  Map delta '%s': %d added, %d modified, %d removed, %d skipped\n",
				cString::To8Char(cString::GetFileNameW(vDeltaFiles[i])).c_str(),
				stats.mlAdded, stats.mlModified, stats.mlRemoved, stats.mlSkipped);

			totalStats.Add(stats);
			++lApplied;
		}

		if(lApplied==0) return 0;

		Log(" Applied %d map delta(s) to '%s': %d added, %d modified, %d removed, %d skipped\n",
			lApplied, cString::To8Char(cString::GetFileNameW(asFile)).c_str(),
			totalStats.mlAdded, totalStats.mlModified, totalStats.mlRemoved, totalStats.mlSkipped);

		return HashFileSet(vDeltaFiles);
	}

	//-----------------------------------------------------------------------

	void BuildRendererMaskIDRemap(	const std::vector<cRendererMaskObjectRecord>& avSkipped,
									const std::vector<cRendererMaskObjectRecord>& avLoaded,
									std::set<int>& aSkippedIDs, std::map<int,int>& aRemap)
	{
		typedef std::multimap<tString, const cRendererMaskObjectRecord*> tLoadedByName;
		tLoadedByName mapLoaded;
		for(size_t i=0; i<avLoaded.size(); ++i) mapLoaded.insert(tLoadedByName::value_type(avLoaded[i].msName, &avLoaded[i]));

		for(size_t i=0; i<avSkipped.size(); ++i)
		{
			const cRendererMaskObjectRecord& skipped = avSkipped[i];
			aSkippedIDs.insert(skipped.mlID);
			if(skipped.mlMask==0) continue;

			int lMatchID = -1;
			int lMatches = 0;
			std::pair<tLoadedByName::const_iterator, tLoadedByName::const_iterator> range = mapLoaded.equal_range(skipped.msName);
			for(tLoadedByName::const_iterator it = range.first; it != range.second; ++it)
			{
				const cRendererMaskObjectRecord* pLoaded = it->second;
				if(pLoaded->msKind != skipped.msKind || (pLoaded->mlMask & skipped.mlMask) != 0) continue;
				lMatchID = pLoaded->mlID;
				++lMatches;
			}
			if(lMatches==1) aRemap[skipped.mlID] = lMatchID;
		}
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::RecordRendererMaskObject(tinyxml2::XMLElement* apElement, bool abLoaded)
	{
		cRendererMaskObjectRecord record;
		record.mlID = GetAttributeInt(apElement, "ID", -1);
		record.msName = cString::ToLowerCase(GetAttributeString(apElement, "Name", ""));
		if(record.mlID<0 || record.msName=="") return;

		const cLightElementInfo lightInfo = GetLightElementInfo(apElement->Value());
		record.msKind = lightInfo.mbValid ? "Light" + cString::ToString(static_cast<int>(lightInfo.mShape))
										  : tString(apElement->Value());
		record.mlMask = cEngineFileLoading::GetElementRendererMask(apElement);

		if(abLoaded)	mvRendererMaskLoaded.push_back(record);
		else			mvRendererMaskSkipped.push_back(record);
	}

	//-----------------------------------------------------------------------

	bool cWorldLoaderHplMap::HasRendererMaskedStaticGeometry(tinyxml2::XMLElement* apXmlMapData)
	{
		tinyxml2::XMLElement* pContents = apXmlMapData->FirstChildElement("MapContents");
		if(pContents==NULL) return false;

		static const char* vCategories[] = { "StaticObjects", "Primitives" };
		for(size_t i=0; i<sizeof(vCategories)/sizeof(vCategories[0]); ++i)
		{
			tinyxml2::XMLElement* pCategory = pContents->FirstChildElement(vCategories[i]);
			if(pCategory==NULL) continue;
			for(tinyxml2::XMLElement* pObj = pCategory->FirstChildElement(); pObj; pObj = pObj->NextSiblingElement())
			{
				if(cEngineFileLoading::GetElementRendererMask(pObj) != kRendererMaskAll) return true;
			}
		}
		return false;
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::LoadCacheFile(const tWString& asFile)
	{
#if (defined(__PPC__) || defined(__ppc__))
		return;
#endif
		tWString sCacheFile = cString::SetFileExtW(asFile, msCacheFileExt);
		
		////////////////////////////////////////
		// Check if there is a cache file
		cDate currentDate = cPlatform::FileModifiedDate(asFile);
		cDate cacheDate = cPlatform::FileModifiedDate(sCacheFile);
		
		// ForceCacheLoadingAndSkipSaving promises a prebuilt cache shipped beside
		// the map. That promise cannot cover a delta-patched map -- its cache is
		// keyed on the delta set and has to be built on the player's machine --
		// so fall back to the normal existence/date check for those.
		// A per-backend cache is not shipped either.
		if(cResources::GetForceCacheLoadingAndSkipSaving()==false || mlDeltaHash!=0 || mbRendererMaskCache)
		{
			if(cacheDate < currentDate || cPlatform::FileExists(sCacheFile)==false)
			{
				return;
			}
		}

		////////////////////////////////////////
		// Load file
		cBinaryBuffer binBuff(sCacheFile);
		if(binBuff.Load()==false)
		{
			Error("Could not map cache file '%s'.", cString::To8Char(asFile).c_str());
			return;
		}

		/////////////////////////////////////////////////
		// Header
		int lMagicNum = binBuff.GetInt32();
		int lVersion = binBuff.GetInt32();

		//Check magic number
		if(lMagicNum != MAP_CACHE_FORMAT_MAGIC_NUMBER)
		{
			Error("File '%s' does not have right MAP_CACHE magic number (%X instead of %X)! Invalid header!\n", cString::To8Char(asFile).c_str(),lMagicNum,MAP_CACHE_FORMAT_MAGIC_NUMBER);
			return;
		}

		//Check so file has he right version
		if(lVersion != MAP_CACHE_FORMAT_VERSION)
		{
			Error("File '%s' does not have right MAP_CACHE version! Is %d, newest is %d\n", cString::To8Char(asFile).c_str(),lVersion,MAP_CACHE_FORMAT_VERSION);
			return;
		}
		
		////////////////////////////////////////
		// General Data
		unsigned long lStartTime = cPlatform::GetApplicationTime();
		mbLoadedCache = true; //Cache is now loaded!

		int lStaticMeshBodyNum = binBuff.GetInt32();
		int lStaticShapeBodyNum = binBuff.GetInt32();
		int lStaticMeshEntities = binBuff.GetInt32();
		mlStaticMeshBodiesCreated = lStaticMeshBodyNum;
		mlStaticMeshEntitiesCreated = lStaticMeshEntities;

		////////////////////////////////////////
		// Iterate Mesh Bodies
		for(int i=0; i<lStaticMeshBodyNum; ++i)
		{
			//////////////////////////////////////
			// General properties
			tString sName, sMaterial;
			binBuff.GetString(&sName);
			binBuff.GetString(&sMaterial);
			bool bBlocksLight = binBuff.GetBool();
			bool bColliderCharacer = binBuff.GetBool();

            //int iNewtonSize = binBuff.GetInt32();

			//////////////////////////////////////
			// Load Shape
            iCollideShape *pShape = mpCurrentPhysicsWorld->LoadMeshShapeFromBuffer(&binBuff);

			//////////////////////////////////////
			// Create Body
            iPhysicsBody *pBody = mpCurrentPhysicsWorld->CreateBody(sName, pShape);

			pBody->SetMaterial(mpCurrentPhysicsWorld->GetMaterialFromName(sMaterial));
			pBody->SetBlocksLight(bBlocksLight);
			pBody->SetCollide(!bColliderCharacer); //A character collider only collides with characters
		}

		////////////////////////////////////////
		// Iterate Shape Bodies
		for(int i=0; i<lStaticShapeBodyNum; ++i)
		{
			cHplMapShapeBody shapeBody;

			binBuff.GetString(&shapeBody.msMaterial);
			binBuff.GetMatrixf(&shapeBody.m_mtxTransform);
			shapeBody.mbCharCollider = binBuff.GetBool();
			shapeBody.mbBlocksLight = binBuff.GetBool();

			int lColliderNum = binBuff.GetInt32();
			shapeBody.mvColliders.resize(lColliderNum);

			for(int i=0; i<shapeBody.mvColliders.size(); ++i)
			{
				cHplMapShape *pShape = hplNew(cHplMapShape, ());
				shapeBody.mvColliders[i] = pShape;

				pShape->mType = (eCollideShapeType)binBuff.GetInt32();
				binBuff.GetVector3f(&pShape->mvSize);
				binBuff.GetMatrixf(&pShape->m_mtxOffset);
			}

			CreateShapeBody(&shapeBody);
		}

		////////////////////////////////////////
		// Iterate Meshes
		for(int mesh=0; mesh<lStaticMeshEntities; ++mesh)
		{
			/////////////////////////
			// General Data
			tString sName, sMaterial;
			binBuff.GetString(&sName);
			binBuff.GetString(&sMaterial);
			bool bCastShadows = binBuff.GetBool();

			if(gbLogCacheLoad) Log("Mesh %d: '%s' '%s'\n", mesh, sName.c_str(), sMaterial.c_str());

			//////////////////////////////
			// Create mesh and submesh
			cMesh* pMesh = hplNew( cMesh, (sName, asFile,mpResources->GetMaterialManager(),mpResources->GetAnimationManager()) );
			pMesh->AddReference(); // hand-built mesh: take the one owning reference the entity drops
			cSubMesh *pSubMesh = pMesh->CreateSubMesh("SubMesh");

			//////////////////
			//Set material
			pSubMesh->SetMaterialName(sMaterial);
			if(sMaterial != "")
			{
				if((mlCurrentFlags & eWorldLoadFlag_FastStaticLoad))
					sMaterial = mpResources->GetMeshManager()->GetFastloadMaterial();

				pSubMesh->SetMaterial(mpResources->GetMaterialManager()->CreateMaterial(sMaterial));
			}
			
			////////////////////
			// Vertex data
			cVertexBuffer* pVtxBuff = new cVertexBuffer(eVertexBufferType_Hardware, eVertexBufferDrawType_Tri,
																					eVertexBufferUsageType_Static, 0, 0);
			{
				int lVtxNum = binBuff.GetInt32();
				int lVtxTypeNum = binBuff.GetInt32();

				if(gbLogCacheLoad) Log(" VertexBuffers num: %d typenum: %d\n",lVtxNum, lVtxTypeNum);

				////////////////////
				// Get vertex arrays
				for(int i=0; i< lVtxTypeNum; ++i)
				{
					//Get the settings
					eVertexBufferElement arrayType = (eVertexBufferElement)binBuff.GetShort16();
					eVertexBufferElementFormat elementFormat = (eVertexBufferElementFormat)binBuff.GetShort16();
					int lProgramVarIndex = binBuff.GetInt32();
					int lElementNum = binBuff.GetInt32();
					int lCompressionType = binBuff.GetInt32();

					if(gbLogCacheLoad) Log("   Vtx %d: %d %d %d\n", i, arrayType, lProgramVarIndex, lElementNum);

					//Create the array
					pVtxBuff->CreateElementArray(arrayType, elementFormat, lElementNum, lProgramVarIndex);
					pVtxBuff->ResizeArray(arrayType, lVtxNum * lElementNum);

					/////////////////////////
					//Uncompressed: Get and fill the array data
					if(lCompressionType ==0)
					{
						void *pData = GetVertexBufferWithFormat(pVtxBuff, arrayType, elementFormat);
						GetBinaryBufferDataWithFormat(&binBuff, pData, (size_t)(lVtxNum * lElementNum), elementFormat);
					}
					/////////////////////////
					// Compressed Get Data
					else
					{
						float *pDestData = pVtxBuff->GetFloatArray(arrayType);
						int lElemCount = lVtxNum * lElementNum;
						
						//////////////////////
						// Byte Array
						if(lCompressionType <= 2)
						{
							///////////////
							//0 - 255 -> 0-1
							if(lCompressionType == 1)
							{
								while(lElemCount > 0)
								{
									*pDestData = mpBytePosFloatTable[binBuff.GetUnsignedChar()];
									
									++pDestData; --lElemCount;
								}
							}
							///////////////
							//-127 - 127 -> -1 - 1
							else
							{
								while(lElemCount > 0)
								{
									*pDestData = mpByteNegPosFloatTable[binBuff.GetUnsignedChar()];
									
									++pDestData; --lElemCount;
								}
							}
						}
						//////////////////////
						// Short Array
						else
						{
							while(lElemCount > 0)
							{
								*pDestData = mpShortNegPosFloatTable[binBuff.GetUnsignedShort16()];

								++pDestData; --lElemCount;
							}
						}
					}
				}
			}

			////////////////////
			//Get Indices
			{
				int lIdxNum =  binBuff.GetInt32();

				if(gbLogCacheLoad) Log("Indices: %d\n", lIdxNum);

				pVtxBuff->ResizeIndices(lIdxNum);
				binBuff.GetInt32Array((int*)pVtxBuff->GetIndices(), lIdxNum);
			}
			
			///////////////////
			//Compile vertex buffer and set to sub mesh
			pVtxBuff->Compile(0);

			pSubMesh->SetVertexBuffer(pVtxBuff);
			pSubMesh->Compile();

			///////////////////
			//Create mesh entity
            cMeshEntity *pMeshEntity = mpCurrentWorld->CreateMeshEntity(sName, pMesh, true);	
			pMeshEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster, bCastShadows);
		}


		////////////////////////////////////////
		// Done loading
		Log("    Cache Loading: %d ms\n", cPlatform::GetApplicationTime() - lStartTime);
	}
	
	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::SaveCacheFile(const tWString& asFile)
	{
#if (defined(__PPC__) || defined(__ppc__))
		return;
#endif
		if(mbLoadedCache) return; //No need to save if cache was loaded!
		if(cResources::GetForceCacheLoadingAndSkipSaving() && mlDeltaHash==0 && mbRendererMaskCache==false) return;

		Log("Saving cache file for '%s'\n", cString::To8Char(asFile).c_str());

        size_t iNewtonTotal = 0;
		tWString sCacheFile = cString::SetFileExtW(asFile, msCacheFileExt);
		cBinaryBuffer binBuff(sCacheFile);

		////////////////////////////////////////
		// Header
		binBuff.AddInt32(MAP_CACHE_FORMAT_MAGIC_NUMBER);
		binBuff.AddInt32(MAP_CACHE_FORMAT_VERSION);

		////////////////////////////////////////
		// General Data
		binBuff.AddInt32((int)mlstStaticMeshBodies.size());
		binBuff.AddInt32((int)mlstStaticShapeBodies.size());
		binBuff.AddInt32((int)mlstStaticMeshEntities.size());

		////////////////////////////////////////
		// Iterate Mesh Bodies
		for(tPhysicsBodyListIt it = mlstStaticMeshBodies.begin(); it != mlstStaticMeshBodies.end(); ++it)
		{
            size_t iNewtonStart;
			iPhysicsBody *pBody = *it;

			binBuff.AddString(pBody->GetName());
			binBuff.AddString(pBody->GetMaterial() ? pBody->GetMaterial()->GetName() : "");
			binBuff.AddBool(pBody->GetBlocksLight());
			binBuff.AddBool(pBody->GetCollide()==false); //If it is a character collider

            // Marker for size of newton blob
            //binBuff.AddInt32(0);
			iNewtonStart = binBuff.GetPos();
			mpCurrentPhysicsWorld->SaveMeshShapeToBuffer(pBody->GetShape(), &binBuff);
            //binBuff.SetInt32(binBuff.GetPos()-iNewtonStart, iNewtonStart-4);
            iNewtonTotal += binBuff.GetPos()-iNewtonStart;
            if (gbLog) Log("Newton: %d, %d\n", iNewtonStart, binBuff.GetPos()-iNewtonStart);
		}
        if (gbLog) Log("Newton Total: %d\n",iNewtonTotal);

		////////////////////////////////////////
		// Iterate Mesh Bodies
		for(tHplMapShapeBodyListIt it = mlstStaticShapeBodies.begin(); it != mlstStaticShapeBodies.end(); ++it)
		{
			cHplMapShapeBody *pShapeBody = *it;

			binBuff.AddString(pShapeBody->msMaterial);
			binBuff.AddMatrixf(pShapeBody->m_mtxTransform);
			binBuff.AddBool(pShapeBody->mbCharCollider);
			binBuff.AddBool(pShapeBody->mbBlocksLight);

			binBuff.AddInt32((int)pShapeBody->mvColliders.size());

			for(int i=0; i<pShapeBody->mvColliders.size(); ++i)
			{
				cHplMapShape *pShape = pShapeBody->mvColliders[i];

				binBuff.AddInt32(pShape->mType);
				binBuff.AddVector3f(pShape->mvSize);
				binBuff.AddMatrixf(pShape->m_mtxOffset);
			}
		}

		////////////////////////////////////////
		// Iterate Meshes
		for(tMeshEntityListIt it = mlstStaticMeshEntities.begin(); it != mlstStaticMeshEntities.end(); ++it)
		{
			//////////////////////////////
			// Get Data
			cMeshEntity *pEntity = *it;
			cSubMeshEntity *pSubEnt = pEntity->GetSubMeshEntity(0);
			cSubMesh *pSubMesh = pSubEnt->GetSubMesh();
			cVertexBuffer *pVtxBuff = pSubMesh->GetVertexBuffer();

			////////////////////////////
			//Add variables
			binBuff.AddString(pEntity->GetName());
			binBuff.AddString(pSubMesh->GetMaterialName());
			binBuff.AddBool(pSubEnt->GetRenderFlagBit(eRenderableFlag_ShadowCaster));

			
			////////////////////////////
			//Add Vertices
			{
				int lVtxNum =  pVtxBuff->GetVertexNum();
				binBuff.AddInt32(lVtxNum);

				//////////////////////////////
				// Calculate the number of vertex buffer types
				int lVtxTypeNum =  0;
				for(int i=0; i < eVertexBufferElement_LastEnum;i++)
				{
					eVertexBufferElement arrayType = (eVertexBufferElement)i;
					
					if(pVtxBuff->GetElementNum((eVertexBufferElement)i) > 0) ++lVtxTypeNum;
				}
				binBuff.AddInt32(lVtxTypeNum);

				//////////////////////////////
				// Iterate the Vertices
				for(int i=0; i < eVertexBufferElement_LastEnum;i++)
				{
					eVertexBufferElement arrayType = (eVertexBufferElement)i;
					
					if(pVtxBuff->GetElementNum(arrayType) <= 0) continue;

					int lElementNum = pVtxBuff->GetElementNum(arrayType);
					eVertexBufferElementFormat elementFormat = pVtxBuff->GetElementFormat(arrayType);

					binBuff.AddShort16(arrayType);
					binBuff.AddShort16(elementFormat);
					binBuff.AddInt32(pVtxBuff->GetElementProgramVarIndex(arrayType));
					binBuff.AddInt32(lElementNum);

					////////////////////////////////////
					//Determine compression type
					 //0=none, 1= 0-1->byte, 2= -1-1->byte 3=-1-1->short
					int lCompressionType =0;
					if(arrayType == eVertexBufferElement_Color0) 
						lCompressionType = 1;
					if(arrayType == eVertexBufferElement_Normal || arrayType == eVertexBufferElement_Texture1Tangent) 
						lCompressionType = 2;

					binBuff.AddInt32(lCompressionType);

					////////////////////////////////////
					//Add Uncompressed data
					if(lCompressionType ==0)
					{
						void *pData = GetVertexBufferWithFormat(pVtxBuff, arrayType, elementFormat);
						AddBinaryBufferDataWithFormat(&binBuff, pData, (size_t)(lVtxNum * lElementNum), elementFormat);
					}
					////////////////////////////////////
					//Add Compressed data
					else
					{
						float* pData = pVtxBuff->GetFloatArray(arrayType);
						int lElementCount = lElementNum * lVtxNum;

						while(lElementCount>0)
						{
							switch(lCompressionType)
							{
							case 1:
								{
									unsigned char lChar = (unsigned char)cMath::FastPositiveFloatToInt(*pData * 255.0f);
									binBuff.AddUnsignedChar(lChar);
									break;
								}
							case 2:
								{
									char lChar = (char)cMath::FastPosAndNegFloatToInt(*pData * 127.0f);
									binBuff.AddChar(lChar);
									break;
								}
							case 3:
								{
									short lX = (short)cMath::FastPosAndNegFloatToInt(*pData * 32767.0f);
									binBuff.AddShort16(lX);
									break;
								}
							}

							++pData;
							--lElementCount;
						}
					}
				}
			}

			////////////////////////////
			//Add Indices
			{
				int lIdxNum =  pVtxBuff->GetIndexNum();

				binBuff.AddInt32(lIdxNum);
				binBuff.AddInt32Array((int*)pVtxBuff->GetIndices(), lIdxNum);
			}
		}
		
		////////////////////////////////////////
		// Save
		bool bRet = binBuff.Save();
		if(bRet==false) 	Error("Couldn't save map cache to '%s'", cString::To8Char(sCacheFile).c_str());
	}
	
	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::LoadFileIndicies(tinyxml2::XMLElement* apXmlContents)
	{

		/////////////////////////////
		// Decals
		tinyxml2::XMLElement* pXmlDecals = apXmlContents->FirstChildElement("FileIndex_Decals");
		if(pXmlDecals)
		{
			mvFileIndices_Decals.resize(GetAttributeInt(pXmlDecals, "NumOfFiles",0));

			for(tinyxml2::XMLElement* pXmlFileIdx = pXmlDecals->FirstChildElement(); pXmlFileIdx != NULL; pXmlFileIdx = pXmlFileIdx->NextSiblingElement())
			{
				int lIdx = GetAttributeInt(pXmlFileIdx, "Id", 0);

				mvFileIndices_Decals[lIdx] = GetAttributeString(pXmlFileIdx, "Path", "");
			}
		}

		/////////////////////////////
		// Entities
		tinyxml2::XMLElement* pXmlEntities = apXmlContents->FirstChildElement("FileIndex_Entities");
		if(pXmlEntities)
		{
			mvFileIndices_Entities.resize(GetAttributeInt(pXmlEntities, "NumOfFiles",0));

			for(tinyxml2::XMLElement* pXmlFileIdx = pXmlEntities->FirstChildElement(); pXmlFileIdx != NULL; pXmlFileIdx = pXmlFileIdx->NextSiblingElement())
			{
				int lIdx = GetAttributeInt(pXmlFileIdx, "Id", 0);

				mvFileIndices_Entities[lIdx] = GetAttributeString(pXmlFileIdx, "Path", "");
			}
		}

		/////////////////////////////
		// Static Objects
		tinyxml2::XMLElement* pXmlStaticObjects = apXmlContents->FirstChildElement("FileIndex_StaticObjects");
		if(pXmlStaticObjects)
		{
			mvFileIndices_StaticObjects.resize(GetAttributeInt(pXmlStaticObjects, "NumOfFiles",0));

			for(tinyxml2::XMLElement* pXmlFileIdx = pXmlStaticObjects->FirstChildElement(); pXmlFileIdx != NULL; pXmlFileIdx = pXmlFileIdx->NextSiblingElement())
			{
				int lIdx = GetAttributeInt(pXmlFileIdx, "Id", 0);

				mvFileIndices_StaticObjects[lIdx] = GetAttributeString(pXmlFileIdx, "Path", "");
			}
		}

		

	}
	
	//-----------------------------------------------------------------------
	
	void cWorldLoaderHplMap::LoadStaticObjects(tinyxml2::XMLElement* apXmlContents)
	{
		unsigned long lStartTime;
		unsigned long lDeltaTime;

		/////////////////////////////////
		//Create and setup mesh data
		cRenderableContainer_BoxTree* pTempContainer = hplNew( cRenderableContainer_BoxTree, () );

		//TODO: These vars need to be tweaked!
		pTempContainer->SetMinLeafObjects(200);//12);
		pTempContainer->SetMinSideLength(3);//3.0f);
		pTempContainer->SetMaxSideLength(30);//3.0f);
		pTempContainer->SetMaxIntersectionAmount(0.03f);
		pTempContainer->SetMaxVolumeCalcObjects(400);
		pTempContainer->SetMinForceIntersectionRelativeSize(0.8f);

		tMeshEntityList lstMeshEntities;

		mlCombinedMeshNameCount =0;
		mlCombinedBodyNameCount =0;
		msetSkippedStaticIDs.clear();

		/////////////////////////////////
		//Create and setup 

		if(mlCurrentFlags & eWorldLoadFlag_FastStaticLoad)
			mpResources->GetMeshManager()->SetUseFastloadMaterial(true);

		/////////////////////////////////
		//Iterate and load static objects to a container
		tinyxml2::XMLElement* pXmlStaticObjects = apXmlContents->FirstChildElement("StaticObjects");
		if(pXmlStaticObjects)
		{
			lStartTime = cPlatform::GetApplicationTime();
			for(tinyxml2::XMLElement* pXmlEntity = pXmlStaticObjects->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
			{
				if(cEngineFileLoading::IsElementEnabledForWorld(pXmlEntity, mpCurrentWorld)==false)
				{
					msetSkippedStaticIDs.insert(GetAttributeInt(pXmlEntity, "ID", -1));
					continue;
				}
				CreateStaticObjectEntity(pXmlEntity, lstMeshEntities, pTempContainer);
			}
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("    MeshEntity Loading: %d ms\n", lDeltaTime);
		}

		///////////////////////////////////////
		//Iterate and load primitives
		tinyxml2::XMLElement* pXmlPrimitives = apXmlContents->FirstChildElement("Primitives");
		if(pXmlPrimitives)
		{
			lStartTime = cPlatform::GetApplicationTime();
			for(tinyxml2::XMLElement* pXmlEntity = pXmlPrimitives->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
			{
				if(cEngineFileLoading::IsElementEnabledForWorld(pXmlEntity, mpCurrentWorld)==false) continue;
				CreatePrimitive(pXmlEntity, lstMeshEntities, pTempContainer);
			}
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("    Primitive Loading: %d ms\n", lDeltaTime);
		}

		///////////////////////////////////////
		//Iterate and load decals (skip when fast loading!)
		tinyxml2::XMLElement* pXmlDecals = apXmlContents->FirstChildElement("Decals");
		if(pXmlDecals && !(mlCurrentFlags & eWorldLoadFlag_FastStaticLoad))
		{
			lStartTime = cPlatform::GetApplicationTime();
			for(tinyxml2::XMLElement* pXmlEntity = pXmlDecals->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
			{
				if(cEngineFileLoading::IsElementEnabledForWorld(pXmlEntity, mpCurrentWorld)==false) continue;
				CreateDecal(pXmlEntity, lstMeshEntities, pTempContainer);
			}
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("    Decal Loading: %d ms\n", lDeltaTime);
		}
		

		if(mlCurrentFlags & eWorldLoadFlag_FastStaticLoad)
			mpResources->GetMeshManager()->SetUseFastloadMaterial(false);

		///////////////////////////////////////
		//Iterate and combine groups
		tinyxml2::XMLElement* pXmlObjectCombos = apXmlContents->FirstChildElement("StaticObjectCombos");
		if(pXmlObjectCombos)
		{
			lStartTime = cPlatform::GetApplicationTime();
			for(tinyxml2::XMLElement* pXmlCombo = pXmlObjectCombos->FirstChildElement(); pXmlCombo != NULL; pXmlCombo = pXmlCombo->NextSiblingElement())
			{
				CreateStaticObjectCombo(pXmlCombo, lstMeshEntities, pTempContainer);
			}
			lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
			if(gbLogTiming) Log("    Object Combining: %d ms\n", lDeltaTime);
		}

		///////////////////////////////////////
		//Compile the container
		//Log(" ==================== COMPILING BEGIN ===========================\n");
		lStartTime = cPlatform::GetApplicationTime();
		pTempContainer->Compile();
		lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
		if(gbLogTiming) Log("    Compilation: %d ms\n", lDeltaTime);
		//Log(" ==================== COMPILING END ===========================\n");

		/////////////////////////////////
		//Go through all leaves of the container, and combine objects found there.
		lStartTime = cPlatform::GetApplicationTime();
		IterateLeafNodesAndBuildMeshes(pTempContainer->GetRoot());
		lDeltaTime = cPlatform::GetApplicationTime() - lStartTime;
		if(gbLogTiming)
		{
			Log("    Combining: %d ms\n", lDeltaTime);
			Log("     Sorting: %d ms\n", mlSortingTimeTotal);
			Log("     Meshes: %d ms\n", mlCombineMeshTimeTotal);
			Log("     Bodies: %d ms\n", mlCombineBodyTimeTotal);
		}
		
		/////////////////////////////////
		//Clean up
        STLDeleteAll(lstMeshEntities);
		STLDeleteAll(mlstTempStaticUserData);
		hplDelete(pTempContainer);
	}

	//-----------------------------------------------------------------------

	static bool SortStaticSubMeshesForMeshes(iRenderable* apObjectDataA, iRenderable* apObjectDataB)
	{
		//Is shadow caster check
		if(	apObjectDataA->GetRenderFlagBit(eRenderableFlag_ShadowCaster) !=
			apObjectDataB->GetRenderFlagBit(eRenderableFlag_ShadowCaster))
		{
			return	apObjectDataA->GetRenderFlagBit(eRenderableFlag_ShadowCaster) <
					apObjectDataB->GetRenderFlagBit(eRenderableFlag_ShadowCaster);
		}
		//Material check
		if( apObjectDataA->GetMaterial() != apObjectDataB->GetMaterial())
		{
			return apObjectDataA->GetMaterial() < apObjectDataB->GetMaterial();
		}

		return apObjectDataA->GetVertexBuffer() < apObjectDataB->GetVertexBuffer();
	}

	//-----------------------------------------------------------------------

	static bool SortStaticSubMeshesForBodies(const cHplMapPhysicsObject& aObjectDataA, const cHplMapPhysicsObject& aObjectDataB)
	{
		//Is shadow caster check
		if(	aObjectDataA.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster) !=
			aObjectDataB.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster))
		{
			return	aObjectDataA.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster) <
					aObjectDataB.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster);
		}
		//Material check
		//Check if next object is not part of sequence, if so combine current sequence.
		if( aObjectDataA.mpPhysicsMaterial != aObjectDataB.mpPhysicsMaterial)
		{
			return aObjectDataA.mpPhysicsMaterial < aObjectDataB.mpPhysicsMaterial;
		}

		//Char collider or not
		if( aObjectDataA.mbCharCollider  != aObjectDataB.mbCharCollider)
		{
			return aObjectDataA.mbCharCollider < aObjectDataB.mbCharCollider;
		}

		return aObjectDataA.mpObject->GetVertexBuffer() < aObjectDataA.mpObject->GetVertexBuffer();
	}

	//-----------------------------------------------------------------------
	
	void cWorldLoaderHplMap::IterateLeafNodesAndBuildMeshes(iRenderableContainerNode *apNode)
	{
		////////////////////////
		//Iterate children
		if(apNode->HasChildNodes())
		{
			tRenderableContainerNodeListIt childIt = apNode->GetChildNodeList()->begin();
			for(; childIt != apNode->GetChildNodeList()->end(); ++childIt)
			{
				iRenderableContainerNode *pChildNode = *childIt;
				IterateLeafNodesAndBuildMeshes(pChildNode);
			}
		}

		/////////////////////////////
		//Iterate objects
		if(apNode->HasObjects())
		{
			CombineAndCreateMeshesAndPhysics(apNode->GetObjectList());
		}	
	}
	
	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CombineAndCreateMeshesAndPhysics(tRenderableList *apObjectList)
	{
		unsigned long lStartTime;
		lStartTime = cPlatform::GetApplicationTime();

		bool bCreateBodies = !mbLoadedCache;

		////////////////////
		//Set up vectors
		tRenderableVec vMeshObjects;
		std::vector<cHplMapPhysicsObject> vPhysicsObjects;

		vMeshObjects.resize(apObjectList->size());
		if(bCreateBodies)
			vPhysicsObjects.resize(apObjectList->size());

		if(gbLog) Log("Trying to combine %d objects for list %d\n",vMeshObjects.size(),apObjectList);

		////////////////////
		//Iterate the objects and add to vector
		tRenderableListIt it = apObjectList->begin();
		for(int i=0; it != apObjectList->end(); ++it, ++i)
		{
			iRenderable *pObject = *it;

			if(gbLog) Log("  Adding '%s'\n",pObject->GetName().c_str());

			/////////////////////
			//Preload and save physics material
			iPhysicsMaterial *pPhysicsMat = NULL;
			if(pObject->GetMaterial())
			{
				const tString& sMat = pObject->GetMaterial()->GetPhysicsMaterial();
				if(sMat != "")	pPhysicsMat = mpCurrentPhysicsWorld->GetMaterialFromName(sMat);
			}

			/////////////////////
			//Set object to arrays	
			vMeshObjects[i] = pObject;

			if(bCreateBodies)
			{
				cSubMeshEntity *pSubEnt =static_cast<cSubMeshEntity*>(pObject);
				cHplMapStaticUserData* pUserData = (cHplMapStaticUserData*)pSubEnt->GetUserData();

				vPhysicsObjects[i].mpObject = pSubEnt;
				vPhysicsObjects[i].mpPhysicsMaterial = pPhysicsMat;
				vPhysicsObjects[i].mpUserData = pUserData;
				vPhysicsObjects[i].mbCharCollider = pUserData->mbCharCollider;

				//Log("'%s': charcollider: %d\n", vPhysicsObjects[i].mpObject->GetName().c_str(), vPhysicsObjects[i].mbCharCollider);
			}
		}

		////////////////////
		//Sort the objects
		if(gbLog) Log(" Sorting objects!\n");
		std::sort(vMeshObjects.begin(), vMeshObjects.end(), SortStaticSubMeshesForMeshes);
		std::sort(vPhysicsObjects.begin(), vPhysicsObjects.end(), SortStaticSubMeshesForBodies);

		mlSortingTimeTotal += cPlatform::GetApplicationTime() - lStartTime;

		////////////////////
		// Create Meshes
		//  Iterate and combine when a found of combinable objects are found
		lStartTime = cPlatform::GetApplicationTime();
		if(gbLog) Log(" Check for mesh combination sequences!\n");

		int lFirstInSequence = 0; //Index of first object to be combined.
		for(size_t i=0; i< vMeshObjects.size(); ++i)
		{
			iRenderable *pMeshObject = vMeshObjects[i];


			if(gbLog) Log("  %d Checking '%s', material: %d, cast shadows: %d\n",i,pMeshObject->GetName().c_str(),
				pMeshObject->GetMaterial(),
				pMeshObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster));

			////////////////////////
			//Check so this is not the last element in array
			if(i < vMeshObjects.size()-1)
			{
				////////////////////////////////////
				//Get the next object
				iRenderable *pNextObject = vMeshObjects[i+1];
				cSubMeshEntity *pNextSubEnt =static_cast<cSubMeshEntity*>(pNextObject);
				cHplMapStaticUserData *pNextObjectUserData = (cHplMapStaticUserData*)pNextSubEnt->GetUserData();

				////////////////////////////////////////
				//Check if next object is not part of sequence, if so combine current sequence.
				if(	pNextObject->GetMaterial() != pMeshObject->GetMaterial() ||
					pNextObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster) != pMeshObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster) ||
					pNextObjectUserData->mbCombine == false)
				{
					CombineObjectsAndCreateMeshEntity(vMeshObjects, lFirstInSequence, (int) i);
					lFirstInSequence = (int)i+1;
				}
			}
			////////////////////////
			//If last just combine all left.
			else
			{
				CombineObjectsAndCreateMeshEntity(vMeshObjects, lFirstInSequence, (int)i);
			}
		}
		mlCombineMeshTimeTotal += cPlatform::GetApplicationTime() - lStartTime;

		////////////////////
		// Create Physics Bodies
		//  Iterate physics, combine and create!
		if(bCreateBodies==false) return;

		const int lIndexCountLimit = 50000;
		int lIndexCount =0;
		lStartTime = cPlatform::GetApplicationTime();
		if(gbLog) Log(" Check for body combination sequences!\n");
		lFirstInSequence = 0;
		for(size_t i=0; i< vPhysicsObjects.size(); ++i)
		{
			cHplMapPhysicsObject& physicsObject = vPhysicsObjects[i];

			if(physicsObject.mpUserData->mbCollides)
				lIndexCount += physicsObject.mpObject->GetVertexBuffer()->GetIndexNum();

			if(gbLog) Log("  %d Checking '%s', physics material: %d\n",i,physicsObject.mpObject->GetName().c_str(),
				physicsObject.mpPhysicsMaterial);

			////////////////////////
			//Check so this is not the last element in array
			if(i < vMeshObjects.size()-1)
			{
				cHplMapPhysicsObject& nextObject = vPhysicsObjects[i+1];

				///////////////////////////////
				//Check if next object is not part of sequence, if so combine current sequence.
				//Also check if max number of polys are now reached, if so compile too!
				if(	lIndexCount > lIndexCountLimit ||
					nextObject.mbCharCollider != physicsObject.mbCharCollider ||
					nextObject.mpPhysicsMaterial != physicsObject.mpPhysicsMaterial ||
					nextObject.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster) != physicsObject.mpObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster))
				{
					CombineObjectsAndCreatePhysics(vPhysicsObjects, lFirstInSequence, (int) i);
					lFirstInSequence = (int)i+1;
					lIndexCount = 0;
				}
			}
			////////////////////////
			//If last just combine all left.
			else
			{
				CombineObjectsAndCreatePhysics(vPhysicsObjects, lFirstInSequence, (int)i);
			}
		}
		mlCombineBodyTimeTotal += cPlatform::GetApplicationTime() - lStartTime;
	}
	
	//-----------------------------------------------------------------------

	struct cVertexDataArray
	{
		eVertexBufferElement mType;
		int mlElementNum;
	};

	void cWorldLoaderHplMap::CombineObjectsAndCreateMeshEntity(tRenderableVec &avObjects, int alFirstIdx, int alLastIdx)
	{
		if(gbLog) Log("  Combining objects %d -> %d\n", alFirstIdx, alLastIdx);

		///////////////////////////////////////////
		//Iterate objects to get the total amount of vertex data
		int lTotalVtxAmount =0;
		int lTotalIdxAmount =0;
		tString sName = "CombinedObjects"+cString::ToString(mlCombinedMeshNameCount);
		for(int i=alFirstIdx; i<=alLastIdx; ++i)
		{
			//Check if the sub mesh is visible, else skip
			cHplMapStaticUserData*pUserData = (cHplMapStaticUserData*)static_cast<cSubMeshEntity*>(avObjects[i])->GetUserData();
			if(pUserData->mbVisible==false) continue;

			//Check add the vertex num and index num from vertex buffer
			cVertexBuffer *pVtxBuffer = avObjects[i]->GetVertexBuffer();

			//Discard objects whose vertex buffer has no position stream — they
			//would produce a position-less CreateCopy() below and assert.
			if(HasPositionStream(pVtxBuffer)==false)
			{
				Warning("Static object '%s' has no position vertex stream — discarding from combined mesh\n", avObjects[i]->GetName().c_str());
				continue;
			}

			lTotalVtxAmount += pVtxBuffer->GetVertexNum();
			lTotalIdxAmount += pVtxBuffer->GetIndexNum();

			if(gbLog) Log("   '%s' has %d vtx and %d idx\n",avObjects[i]->GetName().c_str(),pVtxBuffer->GetVertexNum(),pVtxBuffer->GetIndexNum());
		}
		if(gbLog) Log("   Total amount %d vtx and %d idx\n",lTotalVtxAmount, lTotalIdxAmount);

		///////////////////////////////////////////
		//If no vertices, return and skip creation
		if(lTotalVtxAmount<=0 || lTotalIdxAmount <=0)
		{
			return;
		}

		//Increase the name count
		mlCombinedMeshNameCount++; 

		///////////////////////////////////////////
		//Create the vertex buffer (skipping color!)
		cVertexBuffer *pVtxBuffer = new cVertexBuffer(eVertexBufferType_Hardware, eVertexBufferDrawType_Tri,
																					eVertexBufferUsageType_Static,lTotalVtxAmount, lTotalIdxAmount);

		//Set up what data arrays to use
		const int lDataArrayNum = 5;
		cVertexDataArray lDataArrayTypes[lDataArrayNum] = 
		{ 
			{eVertexBufferElement_Position, 4},
			{eVertexBufferElement_Normal, 3},
			{eVertexBufferElement_Color0, 4},
			{eVertexBufferElement_Texture0, 3},
			{eVertexBufferElement_Texture1Tangent, 4}
		};
		float *pDataArray[lDataArrayNum];

		//Set up the data arrays
		for(int i=0;i<lDataArrayNum; ++i)
		{
			pVtxBuffer->CreateElementArray(lDataArrayTypes[i].mType,eVertexBufferElementFormat_Float, lDataArrayTypes[i].mlElementNum);
			pVtxBuffer->ResizeArray(lDataArrayTypes[i].mType, lTotalVtxAmount * lDataArrayTypes[i].mlElementNum);
			pDataArray[i] = pVtxBuffer->GetFloatArray(lDataArrayTypes[i].mType);		
		}
				
		//Set up and get indices
		pVtxBuffer->ResizeIndices(lTotalIdxAmount);
		unsigned int* pIndexArray = pVtxBuffer->GetIndices();
		
		///////////////////////////////////////////
		//Fill vertex buffer with data
		int lIdxOffset =0;
		for(int vtxbuffer=alFirstIdx; vtxbuffer<=alLastIdx; ++vtxbuffer)
		{
			//Check if the sub mesh is visible, else skip
			cHplMapStaticUserData*pUserData = (cHplMapStaticUserData*)static_cast<cSubMeshEntity*>(avObjects[vtxbuffer])->GetUserData();
			if(pUserData->mbVisible==false) continue;

			iRenderable *pObject = avObjects[vtxbuffer];

			if(gbLog) Log("   Copying data from '%s'\n", pObject->GetName().c_str());

			/////////////////////////////////////
			// Create a copy of the vertex buffer and transform it according to object
			cVertexBuffer *pSubVtxBuffer = pObject->GetVertexBuffer();

			//Skip the same objects discarded in the sizing loop (warned there) so
			//the buffer fill stays consistent with the allocated size.
			if(HasPositionStream(pSubVtxBuffer)==false) continue;

			cVertexBuffer *pTransformedVtxBuffer = pSubVtxBuffer->CreateCopy(	eVertexBufferType_Software, eVertexBufferUsageType_Static,
																				pSubVtxBuffer->GetVertexElementFlags());
			pTransformedVtxBuffer->Transform(pObject->GetWorldMatrix());
			
			//////////////////////////////////////////////////
			//Copy to each data array and increase the data pointer
            for(int i=0; i<lDataArrayNum;++i)
			{
				int lAmount = lDataArrayTypes[i].mlElementNum * pTransformedVtxBuffer->GetVertexNum();
				if(gbLog) Log("    copy from data %d: %d elements\n", i, lAmount);
				
				memcpy(pDataArray[i], pTransformedVtxBuffer->GetFloatArray(lDataArrayTypes[i].mType), lAmount * sizeof(float));

				pDataArray[i] += lAmount;
			}

			//////////////////////////////////////////////
			//Copy to index array (using offset from previous max) and increase index pointer and offset
			unsigned int* pTransIdxArray = pTransformedVtxBuffer->GetIndices();
			for(int i=0; i<pTransformedVtxBuffer->GetIndexNum(); ++i)
			{
				pIndexArray[i] = pTransIdxArray[i] + lIdxOffset;
			}

			lIdxOffset += pTransformedVtxBuffer->GetVertexNum();
			pIndexArray += pTransformedVtxBuffer->GetIndexNum();

			hplDelete(pTransformedVtxBuffer);
		}

		///////////////////////
		// All meshes batched into one buffer, compile it.
		pVtxBuffer->Compile(0);

		///////////////////////////////////////////
		//Create the mesh
		iRenderable *pFirstObject = avObjects[alFirstIdx];
		cSubMeshEntity *pFirstSubEnt = static_cast<cSubMeshEntity*>(pFirstObject);
		cMesh *pMesh = hplNew( cMesh, (sName, _W("") ,mpResources->GetMaterialManager(),mpResources->GetAnimationManager()) );
		pMesh->AddReference(); // hand-built mesh: take the one owning reference the entity drops

		cSubMesh *pSubMesh = pMesh->CreateSubMesh("SubMesh");
		
		//Set the vertex buffer
		pSubMesh->SetVertexBuffer(pVtxBuffer);

		//Set material
		cMaterial *pMaterial = pFirstObject->GetMaterial();
		if(pMaterial)
		{
			pSubMesh->SetMaterial(RetainResource<cMaterial>(mpResources->GetMaterialManager(), pMaterial));
		}
		pSubMesh->SetMaterialName(pFirstSubEnt->GetSubMesh()->GetMaterialName());
		
		//Compile
		pSubMesh->Compile();
		
		///////////////////////////////////////////
		//Create the mesh entity
		cMeshEntity *pMeshEntity = mpCurrentWorld->CreateMeshEntity(sName,pMesh, true);
		
		//Set up variables
		pMeshEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster, pFirstObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster));

		//Add to list
		mlstStaticMeshEntities.push_back(pMeshEntity);
		mlStaticMeshEntitiesCreated++;
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CombineObjectsAndCreatePhysics(std::vector<cHplMapPhysicsObject> &avObjects, int alFirstIdx, int alLastIdx)
	{
		if(gbLog) Log("  Combining objects %d -> %d\n", alFirstIdx, alLastIdx);
		const int lMaxIndices= 30;

		///////////////////////////////////////////
		//Iterate objects to get the total amount of vertex data
		int lTotalVtxAmount =0;
		int lTotalIdxAmount =0;
		tString sName = "CombinedObjects"+cString::ToString(mlCombinedBodyNameCount);
		for(int i=alFirstIdx; i<=alLastIdx; ++i)
		{
			if(avObjects[i].mpUserData->mbCollides==false) continue;

			cVertexBuffer *pVtxBuffer = avObjects[i].mpObject->GetVertexBuffer();

			//Discard objects whose vertex buffer has no position stream (see
			//HasPositionStream) — a position-less CreateCopy() below would assert.
			if(HasPositionStream(pVtxBuffer)==false)
			{
				Warning("Static physics object '%s' has no position vertex stream — discarding from combined body\n", avObjects[i].mpObject->GetName().c_str());
				continue;
			}

			//Do a special debug test and skip highpoly entities, loading the map faster.
            if((mlCurrentFlags & eWorldLoadFlag_FastPhysicsLoad) && pVtxBuffer->GetIndexNum() > lMaxIndices)	continue;
			

			lTotalVtxAmount += pVtxBuffer->GetVertexNum();
			lTotalIdxAmount += pVtxBuffer->GetIndexNum();

			if(gbLog) Log("   '%s' has %d vtx and %d idx\n",avObjects[i].mpObject->GetName().c_str(),pVtxBuffer->GetVertexNum(),pVtxBuffer->GetIndexNum());
		}
		if(gbLog) Log("   Total amount %d vtx and %d idx\n",lTotalVtxAmount, lTotalIdxAmount);

		///////////////////////////////////////////
		//If no vertex buffers, then just exit
		if(lTotalVtxAmount<=0 || lTotalIdxAmount<=0)
		{
			return;
		}
		mlCombinedBodyNameCount++;


		///////////////////////////////////////////
		//Create the vertex buffer (skipping color!)
		cVertexBuffer *pVtxBuffer = new cVertexBuffer(	eVertexBufferType_Software, eVertexBufferDrawType_Tri,
																					eVertexBufferUsageType_Dynamic,lTotalVtxAmount, lTotalIdxAmount);

		pVtxBuffer->CreateElementArray(eVertexBufferElement_Position,eVertexBufferElementFormat_Float, 4);
		pVtxBuffer->ResizeArray(eVertexBufferElement_Position, lTotalVtxAmount * 4);
		float *pDataArray = pVtxBuffer->GetFloatArray(eVertexBufferElement_Position);		
		
		//Set up and get indices
		pVtxBuffer->ResizeIndices(lTotalIdxAmount);
		unsigned int* pIndexArray = pVtxBuffer->GetIndices();

		///////////////////////////////////////////
		//Fill vertex buffer with data
		int lIdxOffset =0;
		for(int vtxbuffer=alFirstIdx; vtxbuffer<=alLastIdx; ++vtxbuffer)
		{
			if(avObjects[vtxbuffer].mpUserData->mbCollides==false) continue;

			cSubMeshEntity *pObject = avObjects[vtxbuffer].mpObject;

			////////////////////////////
			//Add colliders if any
			cSubMesh *pSubMesh =  avObjects[vtxbuffer].mpObject->GetSubMesh();
			
			//Do a special debug test and skip highpoly entities, loading the map faster.
			if((mlCurrentFlags & eWorldLoadFlag_FastPhysicsLoad) && pSubMesh->GetVertexBuffer()->GetIndexNum() > lMaxIndices) continue;
			
			if(gbLog) Log("   Copying position data from '%s'\n", pObject->GetName().c_str());

			////////////////////////
			//Get vertex copy and transform
			cVertexBuffer *pSubVtxBuffer = pObject->GetVertexBuffer();

			//Skip the same objects discarded in the sizing loop (warned there).
			if(HasPositionStream(pSubVtxBuffer)==false) continue;

			cVertexBuffer *pTransformedVtxBuffer = pSubVtxBuffer->CreateCopy(	eVertexBufferType_Software, eVertexBufferUsageType_Static,
																				eVertexElementFlag_Position);
			pTransformedVtxBuffer->Transform(pObject->GetWorldMatrix());
			
			////////////////////////
			//Copy the data
			int lAmount = pTransformedVtxBuffer->GetVertexNum() * 4;

			if(gbLog) Log("    Amount: %d\n", lAmount);

			memcpy(pDataArray, pTransformedVtxBuffer->GetFloatArray(eVertexBufferElement_Position), lAmount * sizeof(float));
			pDataArray += lAmount;

			//Copy to index array and increase index pointer
			unsigned int* pTransIdxArray = pTransformedVtxBuffer->GetIndices();
			for(int i=0; i<pTransformedVtxBuffer->GetIndexNum(); ++i)
			{
				pIndexArray[i] = pTransIdxArray[i] + lIdxOffset;
			}

			lIdxOffset += pTransformedVtxBuffer->GetVertexNum();
			pIndexArray += pTransformedVtxBuffer->GetIndexNum();

			hplDelete(pTransformedVtxBuffer);
		}

		pVtxBuffer->Compile(0);


		///////////////////////////////////////////
		//Create the mesh physics body
		iRenderable *pFirstObject = avObjects[alFirstIdx].mpObject;
		
		iCollideShape *pShape = mpCurrentPhysicsWorld->CreateMeshShape(pVtxBuffer);
		hplDelete(pVtxBuffer);
		
		iPhysicsBody *pBody = mpCurrentPhysicsWorld->CreateBody(sName,pShape);
		pBody->SetMass(0);

		bool bCastShadows = pFirstObject->GetRenderFlagBit(eRenderableFlag_ShadowCaster);
		pBody->SetBlocksLight(bCastShadows);

		pBody->SetCollide(!avObjects[alFirstIdx].mbCharCollider);

		mlstStaticMeshBodies.push_back(pBody);
		mlStaticMeshBodiesCreated++;

		if(avObjects[alFirstIdx].mpPhysicsMaterial) pBody->SetMaterial(avObjects[alFirstIdx].mpPhysicsMaterial);
	}


	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CreateStaticObjectEntity(tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
														cRenderableContainer_BoxTree *apContainer)
	{
		////////////////////////////////
		//Load properties
		tString sName = GetAttributeString(apElement, "Name");
		tString sFileName;

		//File name
		int lFileNameIdx = GetAttributeInt(apElement, "FileIndex",-1);
		if(lFileNameIdx < 0)
		{
			sFileName = GetAttributeString(apElement, "MeshFilename");
			if(sFileName=="") sFileName = GetAttributeString(apElement, "Filename");
		}
		else
		{
			if(lFileNameIdx < (int)mvFileIndices_StaticObjects.size())
			{
				sFileName = mvFileIndices_StaticObjects[lFileNameIdx];
			}
			else
			{
				Error("Could not load static object '%s', File index: %d does is out of bounds!\n", sName.c_str(), lFileNameIdx);
				return;
			}
		}
		
		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
		cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

		bool bCollides = GetAttributeBool(apElement, "Collides", true);
		bool bCastsShadows = GetAttributeBool(apElement, "CastShadows", true);

		int lID = GetAttributeInt(apElement, "ID",-1);

		//Make sure the transform is valid
		if(CheckTransformValidity(sName, vPosition, vRotation, vScale)==false)
		{
			return;
		}

		////////////////////////////////
		//Create mesh entity
		cMesh* pMesh = mpResources->GetMeshManager()->CreateMesh(sFileName);
		if(pMesh == NULL){
			Error("Could not load mesh '%s'\n", sFileName.c_str());
			return;
		}
		
		//////////////////////
		//RENDER_DEBUG
		/*cMeshEntity* pDebugMeshEntity = mpCurrentWorld->CreateMeshEntity(sName, pMesh);
		pDebugMeshEntity->SetWorldMatrix(cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale)));
		pDebugMeshEntity->SetPosition(vPosition);
		return;*/
		
		
		cMeshEntity* pMeshEntity = hplNew( cMeshEntity, (sName,pMesh,
														mpResources->GetMaterialManager(),
														mpResources->GetMeshManager(), 
														mpResources->GetAnimationManager()) );
		pMeshEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster, bCastsShadows);
		pMeshEntity->SetUniqueID(lID);

		alstMeshEntities.push_back(pMeshEntity);


		///////////////////////
		// Check if there is a collider (non character collider!)
		// NOTE: Even though cache is loaded, we still want to add all of these to keep the tree the same!
		bool bHasNormalColliderShapeMesh = false;
		if(bCollides)
		{
			for(int i=0; i<pMeshEntity->GetSubMeshEntityNum(); ++i)
			{
				cSubMeshEntity *pSubEnt = pMeshEntity->GetSubMeshEntity(i);
				cSubMesh *pSubMesh = pSubEnt->GetSubMesh();
				if( cString::Sub(pSubMesh->GetName(),0,1)== "_")
				{
					if( cString::Sub(pSubMesh->GetName(),0,9)== "_collider")
					{
						bHasNormalColliderShapeMesh = true;
						break;
					}
				}
			}
		}

		///////////////////////
		// Add all sub meshes to a new vector
		for(int i=0; i<pMeshEntity->GetSubMeshEntityNum(); ++i)
		{
			cSubMeshEntity *pSubEnt = pMeshEntity->GetSubMeshEntity(i);
			cSubMesh *pSubMesh = pSubEnt->GetSubMesh();
			cMaterial *pMaterial = pSubEnt->GetMaterial();

            ////////////////////////////////
			//If there are (non-char) collide shapes, do not create body from mesh! 
			bool bCreateMeshCollider = bCollides; 
			if(bCreateMeshCollider)
			{
				for(int j=0; j<pSubMesh->GetColliderNum(); ++j)
				{
					if(pSubMesh->GetCollider(j)->mbCharCollider==false) 
						bCreateMeshCollider = false;
				}
			}

			////////////////////////////////
			//Is this a collider shape mesh?
			bool bIsColliderMeshShape = false;
			bool bIsCharCollider = false;
			{
				//All non mesh shapes shall skip creating a collider if there is a normal collider shape.
				if(bHasNormalColliderShapeMesh)	bCreateMeshCollider = false; 

				if( cString::Sub(pSubMesh->GetName(),0,1)== "_")
				{
					if(cString::Sub(pSubMesh->GetName(),0,9)== "_collider")
					{
						bCreateMeshCollider = true;
						bIsColliderMeshShape = true;
					}
					else if(cString::Sub(pSubMesh->GetName(),0,13)== "_charcollider")
					{
						bCreateMeshCollider = true;
						bIsColliderMeshShape = true;
						bIsCharCollider = true;
					}
				}
			}

			//No need to create mesh since it would be non-colliding and invisible!
			if(bCollides == false && bIsColliderMeshShape) continue;
			
			////////////////////////////////
			//Create user data
			cHplMapStaticUserData *pUserData = hplNew(cHplMapStaticUserData, ());
			pUserData->mvScale = vScale;
			pUserData->mbCollides = bCreateMeshCollider;
			pUserData->mbVisible = !bIsColliderMeshShape;
			pUserData->mbCharCollider = bIsCharCollider;

			//If one sided and world reflector, do not combine!
			if(pSubMesh->GetIsOneSided() && pMaterial && pMaterial->HasWorldReflection())
				pUserData->mbCombine = false;
			else
				pUserData->mbCombine = true;

			
			pSubEnt->SetUserData(pUserData);
			mlstTempStaticUserData.push_back(pUserData);

			apContainer->Add(pSubEnt);
		}
		
		////////////////////////////////
		//Create shape bodies
		if(bCollides)
		{
			cMatrixf mtxBodyTransform = cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ);
			mtxBodyTransform.SetTranslation(vPosition);

			for(int i=0; i<pMeshEntity->GetSubMeshEntityNum(); ++i)
			{
				cSubMeshEntity *pSubEnt = pMeshEntity->GetSubMeshEntity(i);
				CreateSubMeshShapeBodies(pSubEnt, mtxBodyTransform, vScale);
			}
		}

		
		////////////////////////////////
		//Setup mesh entity
		pMeshEntity->SetWorldMatrix(cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale)));
		pMeshEntity->SetPosition(vPosition);
	}

	//-----------------------------------------------------------------------
	
	void cWorldLoaderHplMap::CreateSubMeshShapeBodies(cSubMeshEntity *apSubEnt, const cMatrixf &a_mtxTransform, const cVector3f& avScale)
	{
		cSubMesh *pSubMesh = apSubEnt->GetSubMesh();
		if(pSubMesh->GetColliderNum() <= 0) return;
		
		std::vector<cHplMapShape*> vCharColliders;
		std::vector<cHplMapShape*> vNormalColliders;

		vCharColliders.reserve(pSubMesh->GetColliderNum());
		vNormalColliders.reserve(pSubMesh->GetColliderNum());

		//////////////////////////////////
		//Iterate colliders and create shapes
		for(int i=0; i<pSubMesh->GetColliderNum(); ++i)
		{
			cMeshCollider *pMeshCollider = pSubMesh->GetCollider(i);

			cHplMapShape *pMapShape = hplNew(cHplMapShape, ());
			
			pMapShape->m_mtxOffset = pMeshCollider->m_mtxOffset;
			pMapShape->m_mtxOffset.SetTranslation(pMeshCollider->m_mtxOffset.GetTranslation() * avScale);
			pMapShape->mType = pMeshCollider->mType;
			pMapShape->mvSize = pMeshCollider->mvSize * avScale;

			if(pMeshCollider->mbCharCollider)
				vCharColliders.push_back(pMapShape);
			else
				vNormalColliders.push_back(pMapShape);
		}

		//////////////////////////////////
		//Iterate shapes and create bodies
		for(int i=0; i<2; ++i)
		{
			if(i==0 && vNormalColliders.empty()) continue;
			if(i==1 && vCharColliders.empty()) continue;

			std::vector<cHplMapShape*> *pColliderVec = i==0 ? &vNormalColliders : &vCharColliders;
			
			/////////////////////////////////////
			// Create the saved data (to save in cache(
			cHplMapShapeBody *pShapeBody = hplNew(cHplMapShapeBody, () );
            pShapeBody->mbCharCollider = (i==1);			
			pShapeBody->m_mtxTransform = a_mtxTransform;
			pShapeBody->mvColliders = *pColliderVec;
			pShapeBody->mbBlocksLight = apSubEnt->GetRenderFlagBit(eRenderableFlag_ShadowCaster);
			pShapeBody->msMaterial = apSubEnt->GetMaterial()->GetPhysicsMaterial();
			
			mlstStaticShapeBodies.push_back(pShapeBody);

			/////////////////////////////////////
			// Create Shapes Body
			CreateShapeBody(pShapeBody);	
		}
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CreateShapeBody(cHplMapShapeBody* apShapeBody)
	{
		////////////////////////////////////
		// Create sub shapes
		std::vector<iCollideShape*> vShapes;
		vShapes.resize(apShapeBody->mvColliders.size());
		
		for(size_t i=0; i<vShapes.size(); ++i)
		{
			cHplMapShape* pMapShape = apShapeBody->mvColliders[i];
			cMatrixf *pOffset = &pMapShape->m_mtxOffset;

			switch(pMapShape->mType)
			{
			case eCollideShapeType_Box: 
				vShapes[i] = mpCurrentPhysicsWorld->CreateBoxShape(pMapShape->mvSize,pOffset);
				break;
			case eCollideShapeType_Sphere: 
				vShapes[i] = mpCurrentPhysicsWorld->CreateSphereShape(pMapShape->mvSize,pOffset);
				break;
			case eCollideShapeType_Cylinder: 
				vShapes[i] = mpCurrentPhysicsWorld->CreateCylinderShape(pMapShape->mvSize.x,pMapShape->mvSize.y,pOffset);
				break;
			case eCollideShapeType_Capsule: 
				vShapes[i] = mpCurrentPhysicsWorld->CreateCapsuleShape(pMapShape->mvSize.x,pMapShape->mvSize.y,pOffset);
				break;
			}
		}

		/////////////////////////////////////
		// Create final shape
		iCollideShape *pShape = NULL;
		if(vShapes.size() > 1)	pShape = mpCurrentPhysicsWorld->CreateCompundShape(vShapes);
		else					pShape = vShapes[0];

		/////////////////////////////////////
		// Create Body
		iPhysicsBody *pBody = mpCurrentPhysicsWorld->CreateBody("ShapeBody",pShape);
		pBody->SetCollide(!apShapeBody->mbCharCollider);

		pBody->SetBlocksLight(apShapeBody->mbBlocksLight);

		iPhysicsMaterial *pPhysicsMat = mpCurrentPhysicsWorld->GetMaterialFromName(apShapeBody->msMaterial);
		pBody->SetMaterial(pPhysicsMat);
		pBody->SetMatrix(apShapeBody->m_mtxTransform);
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CreatePrimitive(	tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
												cRenderableContainer_BoxTree *apContainer)
	{
		////////////////////////////////
		//Load Main Properties
		cMeshEntity *pMeshEntity = NULL;

		tString sType = apElement->Value();
		tString sName = GetAttributeString(apElement, "Name");
		tString sMaterial = GetAttributeString(apElement, "Material");
		tString sMaterialName = sMaterial;
		bool bCastsShadows = GetAttributeBool(apElement, "CastShadows", true);
		bool bCollides = GetAttributeBool(apElement, "Collides", true);
		int lID = GetAttributeInt(apElement, "ID",-1);

		if((mlCurrentFlags & eWorldLoadFlag_FastStaticLoad))
			sMaterial = mpResources->GetMeshManager()->GetFastloadMaterial();

		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
		cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

		//Make sure the transform is valid
		if(CheckTransformValidity(sName, vPosition, vRotation, vScale)==false)
		{
			return;
		}

		//////////////////////////////////
		// Plane
		if(sType == "Plane")
		{
			cVector3f vStartCorner = GetAttributeVector3f(apElement, "StartCorner",0);
			cVector3f vEndCorner = GetAttributeVector3f(apElement, "EndCorner",0);
			tVector2fVec vUVCorners;
			for(int i=0;i<4;++i)
				vUVCorners.push_back(GetAttributeVector2f(apElement, "Corner" + cString::ToString(i+1) + "UV"));

			//Create the mesh
			cMesh *pMesh = mpGraphics->GetMeshCreator()->CreatePlane(sName,vStartCorner,vEndCorner,
																	 vUVCorners[0],vUVCorners[1], vUVCorners[2], vUVCorners[3], 
																	 sMaterial);


			//////////////////////
			//RENDER_DEBUG
			/*cMeshEntity* pDebugMeshEntity = mpCurrentWorld->CreateMeshEntity(sName, pMesh);
			pDebugMeshEntity->SetWorldMatrix(cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale)));
			pDebugMeshEntity->SetPosition(vPosition);
			return;*/

			//Create mesh entity
			pMeshEntity = hplNew( cMeshEntity, (sName,pMesh,
												mpResources->GetMaterialManager(),
												mpResources->GetMeshManager(), 
												mpResources->GetAnimationManager()) );
			pMeshEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster, bCastsShadows);
			pMeshEntity->GetSubMeshEntity(0)->GetSubMesh()->SetMaterialName(sMaterialName);
		}

		//////////////////////////////////
		// General Final Stuff
		if(pMeshEntity)
		{
			//Set the transform
			pMeshEntity->SetWorldMatrix(cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale)));
			pMeshEntity->SetPosition(vPosition);

			//Id
			pMeshEntity->SetUniqueID(lID);

			//Add the mesh entity.
			alstMeshEntities.push_back(pMeshEntity);

			// Add all sub meshes to a new vector
			for(int i=0; i<pMeshEntity->GetSubMeshEntityNum(); ++i)
			{
				cSubMeshEntity *pSubEnt = pMeshEntity->GetSubMeshEntity(i);
				cSubMesh *pSubMesh = pSubEnt->GetSubMesh();
				cMaterial *pMaterial = pSubEnt->GetMaterial();

				cHplMapStaticUserData *pUserData = hplNew(cHplMapStaticUserData, ());
				pUserData->mvScale = 1;
				pUserData->mbCollides = bCollides;
				pUserData->mbVisible = true;
				pUserData->mbCharCollider = false;
				
				//If one sided and world reflector, do not combine!
				if(pSubMesh->GetIsOneSided() && pMaterial->HasWorldReflection())
					pUserData->mbCombine = false;
				else
					pUserData->mbCombine = true;
				

				pSubEnt->SetUserData(pUserData);
				mlstTempStaticUserData.push_back(pUserData);
				
				apContainer->Add(pSubEnt);
			}
		}

	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CreateDecal(	tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
											cRenderableContainer_BoxTree *apDecalContainer)
	{
		////////////////////////////////
		//Load Main Properties

		///Properties
		tString sName = GetAttributeString(apElement, "Name");
		cColor decalColor = GetAttributeColor(apElement, "Color", cColor(1,1));
		int lID = GetAttributeInt(apElement, "ID",-1);

		///Material
		tString sMaterial="";
		int lFileNameIdx = GetAttributeInt(apElement, "MaterialIndex",-1);
		if(lFileNameIdx < 0)
		{
			sMaterial = GetAttributeString(apElement, "Material");
		}
		else
		{
			if(lFileNameIdx < (int)mvFileIndices_Decals.size())
			{
				sMaterial = mvFileIndices_Decals[lFileNameIdx];			
			}
			else
			{
				Error("Could not load decal '%s'! File index %d is out of bounds!\n", sName.c_str(), lFileNameIdx);
				return;
			}
			
		}
		
		////////////////////////////////
		// Oriented-box decal. The baked <DecalMesh> is no longer used: the decal
		// volume is rebuilt from the editor's WorldPos/Rotation/Scale (Scale =
		// full box size; box centered on WorldPos). The world owns the cDecal and
		// its material; the renderer packs these into a GPU array + grid and
		// projects them in screen space (no geometry is rasterized here).
		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos", 0);
		cVector3f vScale    = GetAttributeVector3f(apElement, "Scale", 1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation", 0);
		cVector2f vSubDivF  = GetAttributeVector2f(apElement, "SubDiv", cVector2f(1,1));
		cVector2l vSubDiv((int)vSubDivF.x, (int)vSubDivF.y);
		// Selected atlas cell (editor's randomized/authored CurrentSubDiv). The
		// baked mesh carried this in its UVs; the OOB projection re-derives it.
		int lCurrentSubDiv = GetAttributeInt(apElement, "CurrentSubDiv", 0);

		// Receiver filter (was edit-time IsAffectedByDecal clipping). The
		// projection shader tests the hit object's category against this mask.
		int lReceiverMask = 0;
		if(GetAttributeBool(apElement, "OnStatic", true))    lReceiverMask |= eDecalReceiver_Static;
		if(GetAttributeBool(apElement, "OnPrimitive", true)) lReceiverMask |= eDecalReceiver_Primitive;
		if(GetAttributeBool(apElement, "OnEntity", true))    lReceiverMask |= eDecalReceiver_Entity;

		cDecal* pDecal = mpCurrentWorld->CreateDecal(sName, sMaterial, decalColor, vSubDiv);
		if(pDecal == NULL)
			return;

		pDecal->SetRendererMask(cEngineFileLoading::GetElementRendererMask(apElement));
		pDecal->SetCurrentSubDiv(lCurrentSubDiv);
		pDecal->SetReceiverMask(lReceiverMask);
		pDecal->SetUniqueID(lID);
		pDecal->SetWorldMatrix(cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),
												cMath::MatrixScale(vScale)));
		pDecal->SetPosition(vPosition);
	}
	
	//-----------------------------------------------------------------------

	static cMeshEntity* GetAndRemoveMeshEntity(tMeshEntityList& alstMeshEntities, int alID)
	{
		for(tMeshEntityListIt it = alstMeshEntities.begin(); it != alstMeshEntities.end(); ++it)
		{
			cMeshEntity *pEntity = *it;
			if(pEntity->GetUniqueID() == alID)
			{
				alstMeshEntities.erase(it);
				return pEntity;
			}
		}
		return NULL;
	}

	void cWorldLoaderHplMap::CreateStaticObjectCombo(	tinyxml2::XMLElement* apElement, tMeshEntityList& alstMeshEntities,
														cRenderableContainer_BoxTree *apContainer)
	{
		tMeshEntityList lstCombineMeshes;
		tRenderableList lstCombineSubMeshes;

		////////////////////////////
		// Get the list of Ids
		int lGroupID =  GetAttributeInt(apElement, "ID", -1);
		tString sObjIds = GetAttributeString(apElement, "ObjIds","");
		tIntVec vObjIds;
		cString::GetIntVec(sObjIds, vObjIds);

		////////////////////////////
		// Iterate object ids
        for(size_t i=0; i<vObjIds.size(); ++i)
		{
			int lID = vObjIds[i];

			///////////////////////////
			//Get the mesh entity
			cMeshEntity *pMeshEnt = GetAndRemoveMeshEntity(alstMeshEntities, lID);
			if(pMeshEnt==NULL)
			{
				if(msetSkippedStaticIDs.count(lID)==0)
					Warning(" Object id %d in group %d does not exist!\n", lID, lGroupID);
				continue;
			}
		
			lstCombineMeshes.push_back(pMeshEnt);
			for(int i=0; i<pMeshEnt->GetSubMeshEntityNum(); ++i)
			{
				lstCombineSubMeshes.push_back(pMeshEnt->GetSubMeshEntity(i));
			}
		}

		if(lstCombineMeshes.empty()) return;

		////////////////////////////
		// Combine the sub meshes and create meshes and bodies
		CombineAndCreateMeshesAndPhysics(&lstCombineSubMeshes);
		
		////////////////////////////
		// Destroy all the mesh entities
		for(tMeshEntityListIt it = lstCombineMeshes.begin(); it != lstCombineMeshes.end(); ++it)
		{
			cMeshEntity *pMeshEnt = *it;
            
			for(int i=0; i<pMeshEnt->GetSubMeshEntityNum(); ++i)
			{
				apContainer->Remove(pMeshEnt->GetSubMeshEntity(i));
			}
			hplDelete(pMeshEnt);
		}
	}
	
	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::LoadEntities(tinyxml2::XMLElement* apXmlContents)
	{
		if(mlCurrentFlags & eWorldLoadFlag_FastEntityLoad)
			mpResources->GetMeshManager()->SetUseFastloadMaterial(true);

		//List that contain light and billboard connections
		tEFL_LightBillboardConnectionList lstLightBillboardListConnections;

		/////////////////////////////////////
		//Iterate all entities in contents
		tinyxml2::XMLElement* pXmlEntities = apXmlContents->FirstChildElement("Entities");
		if(pXmlEntities)
		{
			for(tinyxml2::XMLElement* pXmlEntity = pXmlEntities->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
			{
				CreateLoadedEntity(pXmlEntity, &lstLightBillboardListConnections);
			}
		}

		/////////////////////////////////////
		//Set up light and billboard connections
		for(tEFL_LightBillboardConnectionListIt connIt = lstLightBillboardListConnections.begin(); connIt != lstLightBillboardListConnections.end(); ++connIt)
		{
			cEFL_LightBillboardConnection& lightConnect = *connIt;

			cBillboard *pBB = mpCurrentWorld->GetBillboardFromUniqueID(lightConnect.msBillboardID);
			iLight *pLight = mpCurrentWorld->GetLight(lightConnect.msLightName);

			// The light may be tagged for the other renderer backend and not loaded.
			if(pLight==NULL || pBB==NULL) continue;

			pLight->AttachBillboard(pBB, pBB->GetColor());
		}

		//////////////////////////////
		// Pair skipped objects with their stand-ins for save games.
		{
			std::set<int> setSkippedIDs;
			std::map<int,int> mapRemap;
			BuildRendererMaskIDRemap(mvRendererMaskSkipped, mvRendererMaskLoaded, setSkippedIDs, mapRemap);
			mpCurrentWorld->SetRendererMaskSkippedIDs(setSkippedIDs, mapRemap);
			mvRendererMaskSkipped.clear();
			mvRendererMaskLoaded.clear();
		}

		//Set fast entity load
		if(mlCurrentFlags & eWorldLoadFlag_FastEntityLoad)
			mpResources->GetMeshManager()->SetUseFastloadMaterial(false);
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::CreateLoadedEntity(tinyxml2::XMLElement* apElement, tEFL_LightBillboardConnectionList *apLightBillboardList)
	{
		tString sObjectType = apElement->Value();

		// Objects tagged for the other renderer backend are not created. Both
		// outcomes are recorded so saves made on the other backend can find the
		// same-named object that stands in for a skipped one.
		const bool bEnabledForWorld = cEngineFileLoading::IsElementEnabledForWorld(apElement, mpCurrentWorld);
		if(cResources::GetRendererMaskFilterEnabled()) RecordRendererMaskObject(apElement, bEnabledForWorld);
		if(bEnabledForWorld==false) return;

		//////////////////////////
		//Entity or Area
		if(sObjectType == "Entity" || sObjectType == "Area")
		{
			tString sName = GetAttributeString(apElement, "Name");
			int lID = GetAttributeInt(apElement, "ID");
			bool bActive = GetAttributeBool(apElement, "Active", true);
			cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
			cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
			cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

			//Make sure the transform is valid
			if(CheckTransformValidity(sName, vPosition, vRotation, vScale)==false)
			{
				return;
			}

			//////////////////////////
			//Entity or Area
			if(sObjectType == "Entity")
			{
				if(mlCurrentFlags & eWorldLoadFlag_NoGameEntities) return;
				
				LoadEntity(sName,lID,bActive, vPosition, vRotation, vScale,apElement);
			}
			//////////////////////////
			//Area
			else
			{
				if(mlCurrentFlags & eWorldLoadFlag_NoGameEntities) return;
				
				LoadArea(sName,lID,bActive, vPosition, vRotation, vScale,apElement);
			}
		}
		//////////////////////////
		//Fog Area
		else if(sObjectType == "FogArea")
		{
			cEngineFileLoading::LoadFogArea(apElement,"", mpCurrentWorld, true);
		}
		//////////////////////////
		//Particle System
		else if(sObjectType == "ParticleSystem")
		{
			cEngineFileLoading::LoadParticleSystem(apElement,"", mpCurrentWorld);
		}
		//////////////////////////
		//Sound
		else if(sObjectType == "Sound")
		{
			cEngineFileLoading::LoadSound(apElement,"", mpCurrentWorld);
		}
		//////////////////////////
		//Billboard
		else if(sObjectType == "Billboard")
		{
			cEngineFileLoading::LoadBillboard(apElement,"", mpCurrentWorld, mpResources, true, apLightBillboardList);
		}
		//////////////////////////
		//Light
		else if(cString::GetLastStringPos(sObjectType,"Light")>0)
		{
			if(mlCurrentFlags & eWorldLoadFlag_NoLights) return;

			cEngineFileLoading::LoadLight(apElement,"", mpCurrentWorld, mpResources, true);
		}
		//////////////////////////
		// Unknown
		else
		{
			Error("Unknown entity type '%s'\n", sObjectType.c_str());
			return;
		}
	}

	//-----------------------------------------------------------------------

	
	void cWorldLoaderHplMap::LoadEntity(const tString& asName, int alID, bool abActive, const cVector3f& avPos, const cVector3f& avRot, const cVector3f& avScale, tinyxml2::XMLElement* apElement)
	{
		cMatrixf mtxTransform = cMath::MatrixRotate(avRot,eEulerRotationOrder_XYZ);
		mtxTransform.SetTranslation(avPos);

		//File name
		tString sFilename;
		int lFileNameIdx = GetAttributeInt(apElement, "FileIndex",-1);
		if(lFileNameIdx < 0)
		{
			sFilename = GetAttributeString(apElement, "Filename");
		}
		else
		{
			if(lFileNameIdx < (int)mvFileIndices_Entities.size())
			{
				sFilename = mvFileIndices_Entities[lFileNameIdx];			
			}
			else
			{
				Error("Could not load entity '%s'!\n File index %d is out of bounds!\n", asName.c_str(), lFileNameIdx);
				return;
			}
			
		}

		//User variables
		cResourceVarsObject userVars;
		tinyxml2::XMLElement *pUserVarsElem = apElement->FirstChildElement("UserVariables");
		if(pUserVarsElem) userVars.LoadVariables(pUserVarsElem);
		
        //Create in world
		bool bSkipNonStatic = (mlCurrentFlags & eWorldLoadFlag_NoDynamicGameEntities)!=0;
		mpCurrentWorld->CreateEntity(asName, mtxTransform, sFilename,alID, abActive, avScale, &userVars, bSkipNonStatic,
									 cEngineFileLoading::GetElementRendererMask(apElement));
	}

	//-----------------------------------------------------------------------

	void cWorldLoaderHplMap::LoadArea(const tString& asName, int alID, bool abActive,const cVector3f& avPos, const cVector3f& avRot,const cVector3f& avScale, tinyxml2::XMLElement* apElement)
	{
		cMatrixf mtxTransform = cMath::MatrixRotate(avRot,eEulerRotationOrder_XYZ);
		mtxTransform.SetTranslation(avPos);

        tString sType = GetAttributeString(apElement, "AreaType","");

		iAreaLoader *pLoader  = mpResources->GetAreaLoader(sType);
		if(pLoader==NULL) return;

		//Skip any dynamic area if a flag is set.
		if( (mlCurrentFlags & eWorldLoadFlag_NoDynamicGameEntities)!=0 && pLoader->GetCreatesStaticArea()==false) return;

		//Load variables
		tinyxml2::XMLElement *pVarRootElem = apElement->FirstChildElement("UserVariables");
		if(pVarRootElem) pLoader->LoadVariables(pVarRootElem);

		//Create the area
		pLoader->Load(asName,alID, abActive,avScale,mtxTransform,mpCurrentWorld);
	}

	//-----------------------------------------------------------------------
	
	bool cWorldLoaderHplMap::CheckTransformValidity(const tString& asName, const cVector3f& avPos, const cVector3f& avRot, const cVector3f& avScale)
	{
		if(cMath::Abs(avPos.x)>10000.0f || cMath::Abs(avPos.y)>10000.0f || cMath::Abs(avPos.z)>10000.0f)
		{
			Warning("The object %s has an invalid position: (%s)!\n", asName.c_str(), avPos.ToString().c_str());
			return false;
		}
		return true;
	}

	//-----------------------------------------------------------------------
}
