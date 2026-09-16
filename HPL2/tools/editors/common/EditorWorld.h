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

#ifndef HPLEDITOR_EDITOR_WORLD_H
#define HPLEDITOR_EDITOR_WORLD_H

#include "../common/StdAfx.h"

using namespace hpl;

#include "EditorTypes.h"

#include "system/Event.h"
#include "PrefabManager.h"	// ePrefabEvent (broadcast handler below)

#include <set>

namespace tinyxml2 { class XMLElement; }

//----------------------------------------------------------------------

class iEditorBase;
class iEntityWrapperType;
class iEntityWrapperData;
class iEntityWrapper;


class cEntityPicker;
class cSurfacePicker;
class cEditorFileWatcher;

//----------------------------------------------------------------------

class iEditorWorld
{
public:
	iEditorWorld(iEditorBase* apEditor, const tString& asElementName);
	virtual ~iEditorWorld();

	virtual void OnEditorUpdate(float afTimeStep);
	virtual void Reset();

	void SetName(const tString& asName);
	tString& GetName() { return msName; }

	iEditorBase* GetEditor() { return mpEditor; }
	cWorld* GetWorld() { return mpWorld; }

	cEntityPicker* GetPicker() { return mpPicker; }
	cSurfacePicker* GetSurfacePicker() { return mpSurfacePicker; }

	////////////////////////////////////////////////////////
	// ID management
	bool IsIDInUse(int alID);
	int GetFreeID () { return mlIDCounter++; }
	int AddIDRedirection(int alOldID);

	////////////////////////////////////////////////////////
	// Entity management
	virtual bool AddObject(iEntityWrapper* apObject);
	virtual void RemoveObject(iEntityWrapper* apObject);

	iEntityWrapper* GetEntity(int alID);
	iEntityWrapper* GetEntityByName(const tString& asName);
	iEntityWrapper* GetEntityByGUID(unsigned long long alGUID);
	tEntityWrapperMap& GetEntities() { return mmapEntities; }

	bool HasEntity(iEntityWrapper* apObject);

	bool IsClearingEntities() { return mbIsClearingEntities; }
	void ClearEntities();

	////////////////////////////////////////////////////////
	// Live file watching / reload
	// Re-read the given entities from disk in place (re-builds their engine
	// entities, preserving placement/selection). Does NOT dirty the map or
	// push an undo action - it's a live external reload, not an edit.
	void ReloadEntities(const std::set<int>& asetIDs);
	cEditorFileWatcher* GetFileWatcher() { return mpFileWatcher; }

	// IDs of entities whose source file matches asFile by BARE FILENAME
	// (case-insensitive) — the prefab identity cPrefabManager uses. Scanning on
	// demand (instead of tracked membership) cannot go stale or orphan.
	std::set<int> FindEntityIDsByFilename(const tString& asFile);

	// IDs of placed particle-system entities whose .ps matches asFile by bare
	// filename (case-insensitive). Particle wrappers store their path in msFile
	// (GetFile()), NOT msFilename, so FindEntityIDsByFilename cannot see them.
	std::set<int> FindParticleSystemIDsByFile(const tString& asFile);

	// Live-reload placed particle systems after their .ps changed on disk. Tears
	// down ALL matching instances first (dropping every cParticleSystemData
	// reference so cParticleManager auto-evicts the cached data at refcount 0),
	// THEN rebuilds each so the first recreate re-reads disk. Like ReloadEntities,
	// this does NOT dirty the map or push an undo action.
	void ReloadParticleSystems(const std::set<int>& asetIDs);

	// Watcher-driven reload: files changed ON DISK, so first drop any non-dirty
	// pending prefab docs shadowing the affected .ent files (disk becomes truth;
	// dirty = unsaved MCP edits are kept), then ReloadEntities.
	void OnWatcherReload(const std::set<int>& asetIDs);

	////////////////////////////////////////////////////////
	// Loading / Saving
	virtual bool Load(tinyxml2::XMLElement* apXmlDoc);
	virtual tinyxml2::XMLElement* GetWorldDataElement(tinyxml2::XMLElement* apXmlDoc)=0;
	virtual void LoadWorldData(tinyxml2::XMLElement* apWorldDataElement);
	virtual tinyxml2::XMLElement* GetWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement)=0;
	virtual void LoadWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement);
	virtual bool CustomCategoryLoader(tinyxml2::XMLElement* apWorldObjectsElement, tinyxml2::XMLElement* apCategoryElement) { return false; }

	void SetShowLoadErrorPopUp() { mbShowLoadErrorPopUp = true; }
	void SetLoadErrorMessage(const tWString& asX) { msLoadErrorMsg = asX; }

	virtual bool Save(tinyxml2::XMLElement* apXmlDoc);
	virtual tinyxml2::XMLElement* CreateWorldDataElement(tinyxml2::XMLElement* apXmlDoc)=0;
	virtual void SaveWorldData(tinyxml2::XMLElement* apWorldDataElement);
	virtual tinyxml2::XMLElement* CreateWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement)=0;
	virtual void SaveWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement, tEntityWrapperList& alstEntities);
	virtual bool CustomCategorySaver(tinyxml2::XMLElement* apWorldObjectsElement) { return false; }


	virtual void ImportObjects(const tString& asX, tIntList& alstImportedIDs);
	// Same import, from an already-parsed XML root (<Level> or <MapData>).
	// Returns false when the expected MapData/MapContents shape is missing.
	// Used by the MCP import_map tool.
	bool ImportObjects(tinyxml2::XMLElement* apRootElem, tIntList& alstImportedIDs);
	virtual void ExportObjects(const tString& asX, tEntityWrapperList& alstEntsToExport);


	bool IsModified() { return mlNumModifications!=mlLastSavedModification; }
	unsigned int GetNumModifications() { return mlNumModifications; }
	unsigned int GetLastSavedModification() { return mlLastSavedModification; }
	void UpdateSavedModifications() { mlLastSavedModification = mlNumModifications; }
	void IncModifications();
	void DecModifications();

	///////////////////////////////////////////////////////
	// Object Naming
    // Objects that load for disjoint renderers may share a name.
    bool IsNameAvailable(const tString& asName, int alRendererMask=static_cast<int>(hpl::kRendererMaskAll));

	tString GenerateName(const tString& asBaseName);

	///////////////////////////////////////////////////////
	// Object Creation
	iEntityWrapper* CreateEntityWrapperFromData(iEntityWrapperData* apData);
	iEntityWrapper* CreateEntityWrapperFromXMLElement(tinyxml2::XMLElement* apElement);

	void SetUpEntityWrapper(iEntityWrapper* apEntity,
							const cVector3f& avPosition=0,
							const cVector3f& avRotation=0,
							const cVector3f& avScale=1);

	void DestroyEntityWrapper(iEntityWrapper* apEntity, bool abRemoveFromWorld=true);

	void AddEntityType(iEntityWrapperType* apType);
	iEntityWrapperType* GetEntityTypeByName(const tString&);
	iEntityWrapperType* GetEntityTypeByID(int alType);
	iEntityWrapperType* GetEntityTypeByElement(tinyxml2::XMLElement*);

	int GetEntityTypeNum();
	iEntityWrapperType* GetEntityType(int alX);

	////////////////////////////////////////////////////////////////
	// Data / Entity callbacks for a specific world

	virtual void CreateDataCallback(iEntityWrapperData* apData) {}
	virtual void CopyDataFromEntityCallback(iEntityWrapperData* apData, iEntityWrapper* apEnt) {}
	virtual void CopyDataToEntityCallback(iEntityWrapperData* apData, iEntityWrapper* apEnt, int alCopyStep) {}
	virtual void DestroyDataCallback(iEntityWrapperData* apData) {}

	virtual void SaveDataCallback(iEntityWrapperData* apData, tinyxml2::XMLElement* apElement) {}
	virtual void LoadDataCallback(iEntityWrapperData* apData, tinyxml2::XMLElement* apElement) {}

	virtual void DestroyEntityWrapperCallback(iEntityWrapper* apEnt) {}

	///////////////////////////////
	// Global Light stuff
	void SetGlobalAmbientLightEnabled(bool abX);
	void SetGlobalPointLightEnabled(bool abX);

	bool GetGlobalAmbientLightEnabled();
	bool GetGlobalPointLightEnabled();

	///////////////////////////////
	// Object Visibility
	void SetTypeVisibility(int alTypeID, bool abX);
	bool GetTypeVisibility(int alTypeID);

	void SetVisibilityUpdated() { mbVisibilityUpdated = true; }
	void UpdateVisibility();

	void SetTypeActive(int alTypeID, bool abX);
	bool GetTypeActive(int alTypeID);

	///////////////////////////////
	// Clip planes
	cEditorClipPlane* AddClipPlane();
	void RemoveClipPlane(int alIdx, bool abDestroy=true);
	void RemoveClipPlane(cEditorClipPlane* apPlane, bool abDestroy=true);
	void ClearClipPlanes(bool abDestroyPlanes=true);

	cEditorClipPlane* GetClipPlane(int alIdx) 
	{
		if(alIdx<0 || alIdx>(int)mvClipPlanes.size()) return NULL; 
		else return mvClipPlanes[alIdx]; 
	}
	tEditorClipPlaneVec& GetClipPlanes() { return mvClipPlanes; }
	void SetClipPlanes(const tEditorClipPlaneVec& avX);
	void SetClipPlanesUpdated() { mbClipPlanesUpdated=true; }

	bool ClipPlaneNumUpdated() { bool bUpdated = mbClipPlaneNumUpdated; mbClipPlaneNumUpdated = false; return bUpdated; }


	int AddEntityCategory(const tString& asX, int alType);
	void AddTypeToCategory(int alCatID, int alType);
	tinyxml2::XMLElement* GetParentElementForObject(std::vector<tinyxml2::XMLElement*>& avParentElements, iEntityWrapper* apEnt);

	///////////////////////////////
	// Filename indices
	void AddFileIndex(const tString& asName, int alTypeID);
	int AddFilenameToIndex(const tString& asIndexName, const tString& asFilename);
	int AddFilenameToIndex(tStringVec& avIndex, const tString& asFilename);
	const tString& GetFilenameFromIndex(const tString& asName, int alIndex);
	tString GetFileRelativePath(const tString& asX);

	void LoadFileIndices(tinyxml2::XMLElement* apWorldObjectsElement);
	void SaveFileIndices(tinyxml2::XMLElement* apWorldObjectsElement);

	void SanitizeFileIndex(tStringVec& avFileIndices, int alTypeID);
	void RelocateFileIndex(int alTypeID, int alOldIndex, int alNewIndex);

	void SetBGDefaultColor(const cColor& aX);
	const cColor& GetBGDefaultColor() { return mBGDefaultColor; }
	///////////////////////////////
	// Skybox
	void SetShowSkybox(bool abX);
	void SetSkyboxActive(bool abX);
	void SetSkyboxTexture(const tString& asX);
	void SetSkyboxColor(const cColor& aX);

	void UpdateSkybox();

	bool GetShowSkybox() { return mbShowSkybox; }
	bool GetSkyboxActive() { return mbSkyboxActive; }
	const tString& GetSkyboxTexture() { return msSkyboxTexture; }
	const cColor& GetSkyboxColor() { return mSkyboxColor; }

	///////////////////////////////
	// Fog
	void SetShowFog(bool abX);
	void SetFogActive(bool abX);
	void SetFogCulling(bool abX);
	void SetFogStart(float afX);
	void SetFogEnd(float afX);
	void SetFogFalloffExp(float afX);
	void SetFogColor(const cColor& aX);

	bool GetShowFog() { return mbShowFog; }

	// Show objects the editor renderer would skip in the game.
	void SetShowOtherRendererObjects(bool abX);
	bool GetShowOtherRendererObjects() { return mbShowOtherRendererObjects; }
	bool GetFogActive() { return mbFogActive; }
	bool GetFogCulling();
	float GetFogStart();
	float GetFogEnd();
	float GetFogFalloffExp();
	const cColor& GetFogColor();

	const cVector3f& GetMinPosition() { return mvMinPos; }
	const cVector3f& GetMaxPosition() { return mvMaxPos; }
	void AddOutlierEntity(iEntityWrapper* apEnt);

protected:
	///////////////////////////////////////////////////////
	// Data
	iEditorBase* mpEditor;
	cWorld* mpWorld;
	tString msElementName;

	tString msName;

	bool mbIsClearingEntities;

	unsigned int mlNumModifications;
	unsigned int mlLastSavedModification;

	tEntityWrapperMap mmapEntities;
	std::set<unsigned long long> msetGUIDsInUse; // collision accelerator only; truth = wrappers

	int mlIDCounter;
	
	int mlDefaultCategory;
	std::vector<tString> mvEntityCategories;
	std::vector<tIntList> mvEntityCategoryTypes;

	tEntityTypeList mlstEntityTypes;

	std::map<int, int> mmapIDRedirectors;

	tStringVec mvFileIndexNames;
	tIntVec mvFileIndexTypeIDs;
		
	std::map<tString,tStringVec> mmapFileIndices;

	bool mbVisibilityUpdated;

	cEntityPicker* mpPicker;
	cSurfacePicker* mpSurfacePicker;

	cEditorFileWatcher* mpFileWatcher;

	// Live-reload entities when the watcher reports their deps changed on disk.
	// The watcher publishes OnReloadEntities(); we subscribe (decoupled: the
	// watcher does not hold a back-pointer to the world). RAII-disconnects.
	Event<const std::set<int>&>::Handler mFileReloadHandler {
		[this](const std::set<int>& asetIDs){ OnWatcherReload(asetIDs); } };

	// (Prefab-definition changes no longer route through this world: each placed
	// cEntityWrapperEntity subscribes to its own cPrefabResource::OnModified() and
	// rebuilds itself — see EntityWrapperEntity. The filename scan below stays for
	// the on-disk filewatcher path and the MCP query tools.)

	////////////////////////////////
	// Global Lights
	iLightPoint* mpGlobalPointLight;

	bool mbWorldLightsEnabled;
	bool mbWorldPSEnabled;

	////////////////////////////////
	// Skybox
	cColor mBGDefaultColor;

	bool mbShowSkybox;
	bool mbSkyboxActive;
	cColor mSkyboxColor;
    tString msSkyboxTexture;

	bool mbSkyboxTextureUpdated;

	////////////////////////////////
	// Fog
	bool mbShowFog;
	bool mbShowOtherRendererObjects;
	bool mbFogActive;

	////////////////////////////////
	// Clip Planes
	bool mbClipPlanesUpdated;
	bool mbClipPlaneNumUpdated;
	tEditorClipPlaneVec mvClipPlanes;

	cEditorClipPlane* mpClipPlane;

	cVector3f mvMinPos;
	cVector3f mvMaxPos;

	tEntityWrapperList mlstOutliers;

	bool mbShowLoadErrorPopUp;
	tWString msLoadErrorMsg;
};

//----------------------------------------------------------------------

#endif //HPLEDITOR_EDITOR_WORLD_H
