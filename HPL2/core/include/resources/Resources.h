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

#ifndef HPL_RESOURCES_H
#define HPL_RESOURCES_H

#include <map>
#include "system/SystemTypes.h"
#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/RendererMask.h"
#include "resources/ResourcesTypes.h"

#include "engine/Updateable.h"

namespace hpl {

	class cImageEntity;

	class iLowLevelResources;
	class iLowLevelSystem;
	class cSystem;
	class iResourceManager;
	class cFileSearcher;
	class cImageManager;
	class cParticleManager;
	class cSoundManager;
	class cFontManager;
	class cScriptManager;
	class cTextureManager;
	class cMaterialManager;
	class cSoundEntityManager;
	class cAnimationManager;
	class cEntFileManager;
	class cMeshManager;
	class cConfigFile;
	class cArea2D;
	class cSound;
	class cMeshLoaderHandler;
	class cBitmapLoaderHandler;
	class cWorldLoaderHandler;
	class cVideoLoaderHandler;
	class cScene;
	class cGraphics;
	class iRenderable;
	class cWorld;
	class iEntity3D;
	class cLanguageFile;
	class cGui;
	class cPhysics;
	class cBinaryBuffer;

	//-------------------------------------------------------

	typedef std::map<tString, tString> tResourceVarMap;
	typedef tResourceVarMap::iterator tResourceVarMapIt;
	
	//-------------------------------------------------------
	
	class cResourceVarsObject
	{
	public:
		void LoadVariables(tinyxml2::XMLElement *apRootElem);
		void SetUserVariable(const tString& asName, const tString& asValue);
		tString* GetUserVariable(const tString& asName);

		void AddVarBool(const tString& asName, bool abDefault=false);
		void AddVarInt(const tString& asName, int alDefault=0);
		void AddVarFloat(const tString& asName, float afDefault=0);
		void AddVarVector2f(const tString& asName, const cVector2f& avDefault=0);
		void AddVarVector3f(const tString& asName, const cVector3f& avDefault=0);
		void AddVarColor(const tString& asName, const cColor& aDefault=0);
		void AddVarString(const tString& asName, const tString& alDefault="");

		bool GetVarBool(const tString& asName, bool abDefault=false);
		int GetVarInt(const tString& asName, int alDefault=0);
		float GetVarFloat(const tString& asName, float afDefault=0);
		cVector2f GetVarVector2f(const tString& asName, const cVector2f& avDefault=0);
		cVector3f GetVarVector3f(const tString& asName, const cVector3f& avDefault=0);
		cColor GetVarColor(const tString& asName, const cColor& aDefault=cColor(1,1));
		tString GetVarString(const tString& asName, const tString& asDefault="");

	protected:
		tResourceVarMap m_mapVars;
	};


	//-------------------------------------------------------

	class iMapDataLoader : public cResourceVarsObject
	{
	public:
		iMapDataLoader(const tString& asName): msName(asName){}

		const tString& GetName(){ return msName;}

		
	protected:
		tString msName;
	};
	
	//-------------------------------------------------------
	
	class iEntityLoader : public iMapDataLoader
	{
	public:
		iEntityLoader(const tString& asName): iMapDataLoader(asName), mbCreatesStaticEntity(false), mlInstanceRendererMask(kRendererMaskAll){}
		virtual ~iEntityLoader(){}

		bool GetCreatesStaticEntity(){ return mbCreatesStaticEntity; }

		// RendererMask of the map object being loaded; cWorld::CreateEntity sets it
		// around Load so the created renderables and .ent children inherit it.
		void SetInstanceRendererMask(unsigned alMask){ mlInstanceRendererMask = SanitizeRendererMask(alMask); }
		unsigned GetInstanceRendererMask() const { return mlInstanceRendererMask; }
		
		virtual iEntity3D* Load(const tString &asName, int alID, bool abActive, tinyxml2::XMLElement* apRootElem,
								const cMatrixf &a_mtxTransform, const cVector3f &avScale, 
								cWorld *apWorld, const tString &asFileName, const tWString &asFullPath, cResourceVarsObject *apInstanceVars)=0;

	protected:
		bool mbCreatesStaticEntity;
		unsigned mlInstanceRendererMask;
	};

	typedef std::map<tString,iEntityLoader*> tEntityLoaderMap;
	typedef tEntityLoaderMap::iterator tEntityLoaderMapIt;

	//-------------------------------------------------------

	class iAreaLoader : public iMapDataLoader
	{
	public:
		iAreaLoader(const tString& asName): iMapDataLoader(asName), mbCreatesStaticArea(false){}
		virtual ~iAreaLoader(){}

		bool GetCreatesStaticArea(){ return mbCreatesStaticArea; }
		const tString& GetName(){ return msName;}

		virtual void Load(const tString &asName, int alID, bool abActive, const cVector3f &avSize, const cMatrixf &a_mtxTransform,cWorld *apWorld)=0;

	protected:
		bool mbCreatesStaticArea;
	};

	typedef std::map<tString,iAreaLoader*> tAreaLoaderMap;
	typedef tAreaLoaderMap::iterator tAreaLoaderMapIt;

	//-------------------------------------------------------

	// Should this be made a map so you can do 
	// GetManager(tString) when getting a manager?
	// This way you would be able to add your own resource types
	// easily.
	typedef std::list<iResourceManager*> tResourceManagerList;
	typedef tResourceManagerList::iterator tResourceManagerListIt;

	//-------------------------------------------------------

	
	class cResources : public iUpdateable
	{
	public:
		cResources(iLowLevelResources *apLowLevelResources);
		~cResources();

		void Init(cGraphics* apGraphics,cSystem *apSystem, cSound* apSound, cScene *apScene, cGui *apGui, cPhysics *apPhysics);

		void Update(float afTimeStep);

		iLowLevelResources* GetLowLevel();
		cFileSearcher* GetFileSearcher();

		/**
		 * \param alPriority Higher-priority directories shadow lower-priority directories for files with the same name.
		 * \param asScope Ownership key for removable registrations; empty means permanent.
		 */
		bool AddResourceDir(const tWString &asDir, bool abAddSubDirectories, const tString &asMask = "*.*", int alPriority = klFileSearchDefaultPriority, const tString &asScope = "");
		void RemoveResourceDirScope(const tString &asScope);
		void ClearResourceDirs();

		// asResourceScope owns directories declared by this language file, including alternate paths.
		bool AddLanguageFile(const tString &asFilePath, bool abAddResourceDirs, const tWString &asAltPath = _W(""), const tString &asResourceScope = "");
		void ClearTranslations();
		const tWString& Translate(const tString& asCat, const tString& asName);

		void AddEntityLoader(iEntityLoader* apLoader, bool abSetAsDefault=false);
		iEntityLoader* GetEntityLoader(const tString& asName);

		void AddAreaLoader(iAreaLoader* apLoader, bool abSetAsDefault=false);
		iAreaLoader* GetAreaLoader(const tString& asName);

		bool LoadResourceDirsFile(const tString &asFile, const tWString &asAltPath = _W(""));

		// Collects every delta file that patches asBaseFile, sorted by the order
		// they must be applied in: Priority ascending, then path. asDeltaExt is the
		// extension to look for ("map_delta", "ent_delta"); the base file's own
		// extension is swapped for it and the result resolved by bare filename, so
		// any resource dir (a mod folder, a custom story) can contribute one.
		// Returns false, leaving avPaths untouched, when deltas are disabled.
		bool FindDeltaFiles(const tWString& asBaseFile, const tString& asDeltaExt, tWStringVec& avPaths);

		// Returns the root element of the loaded document. cResources owns the
		// underlying tinyxml2::XMLDocument (tracked in mlstXmlDocuments); pass the
		// returned root back to DestroyXmlDocument to release it.
		tinyxml2::XMLElement* LoadXmlDocument(const tString& asFile);
		void DestroyXmlDocument(tinyxml2::XMLElement* apDocRoot);

		cBinaryBuffer* LoadBinaryBuffer(const tString& asFile);
		void DestroyBinaryBuffer(cBinaryBuffer* apFile);

		cMeshLoaderHandler* GetMeshLoaderHandler(){ return mpMeshLoaderHandler;}
		cBitmapLoaderHandler* GetBitmapLoaderHandler(){ return mpBitmapLoaderHandler;}
		cWorldLoaderHandler* GetWorldLoaderHandler(){ return mpWorldLoaderHandler;}

		cImageManager* GetImageManager(){return mpImageManager;}
		cParticleManager* GetParticleManager(){ return mpParticleManager;}
		cSoundManager* GetSoundManager(){ return mpSoundManager;}
		cFontManager* GetFontManager(){ return mpFontManager;}
		cScriptManager* GetScriptManager(){ return mpScriptManager;}
		cTextureManager* GetTextureManager(){ return mpTextureManager;}
		cMaterialManager* GetMaterialManager(){ return mpMaterialManager;}
		cMeshManager* GetMeshManager(){ return mpMeshManager;}
		cSoundEntityManager* GetSoundEntityManager(){ return mpSoundEntityManager;}
		cAnimationManager* GetAnimationManager(){ return mpAnimationManager;}
		cEntFileManager* GetEntFileManager(){ return mpEntFileManager; }

		iLowLevelSystem* GetLowLevelSystem(){ return mpLowLevelSystem;}

		static void SetForceCacheLoadingAndSkipSaving(bool abX){ mbForceCacheLoadingAndSkipSaving = abX;}
		static bool GetForceCacheLoadingAndSkipSaving(){ return mbForceCacheLoadingAndSkipSaving ;}

		static void SetCreateAndLoadCompressedMaps(bool abX){ mbCreateAndLoadCompressedMaps = abX;}
		static bool GetCreateAndLoadCompressedMaps(){ return mbCreateAndLoadCompressedMaps ;}

		// Map/ent delta overlays (.map_delta / .ent_delta). Off by default so the
		// editors and the offline tools keep seeing unpatched files; the game turns
		// it on at startup.
		static void SetDeltasEnabled(bool abX){ mbDeltasEnabled = abX;}
		static bool GetDeltasEnabled(){ return mbDeltasEnabled ;}

		// Skip map/entity objects whose RendererMask excludes the running backend,
		// so a Standard/Overdrive pair sharing one name loads as one object.
		// Off by default so the editors keep both halves of a pair editable; the
		// game turns it on at startup.
		static void SetRendererMaskFilterEnabled(bool abX){ mbRendererMaskFilterEnabled = abX;}
		static bool GetRendererMaskFilterEnabled(){ return mbRendererMaskFilterEnabled ;}
		
	private:
		iLowLevelResources *mpLowLevelResources;
		iLowLevelSystem *mpLowLevelSystem;
		cFileSearcher *mpFileSearcher;

        tResourceManagerList mlstManagers;
		cImageManager *mpImageManager;
		cParticleManager* mpParticleManager;
		cSoundManager* mpSoundManager;
		cFontManager* mpFontManager;
		cScriptManager* mpScriptManager;
		cTextureManager* mpTextureManager;
		cMaterialManager* mpMaterialManager;
		cSoundEntityManager* mpSoundEntityManager;
		cAnimationManager *mpAnimationManager;
		cEntFileManager *mpEntFileManager;

		cLanguageFile *mpLanguageFile;

		cMeshManager* mpMeshManager;
		
		cMeshLoaderHandler* mpMeshLoaderHandler;
		cBitmapLoaderHandler* mpBitmapLoaderHandler;
		cWorldLoaderHandler *mpWorldLoaderHandler;
				
		tEntityLoaderMap m_mEntityLoaders;
		iEntityLoader* mpDefaultEntityLoader;

		tAreaLoaderMap m_mAreaLoaders;
		iAreaLoader* mpDefaultAreaLoader;

		tXmlDocumentList mlstXmlDocuments;
		tBinaryBufferList mlstBinBuffers;

		tWString mwsEmptyString;

		static bool mbForceCacheLoadingAndSkipSaving;
		static bool mbCreateAndLoadCompressedMaps;
		static bool mbDeltasEnabled;
		static bool mbRendererMaskFilterEnabled;
	};

};
#endif // HPL_RESOURCES_H
