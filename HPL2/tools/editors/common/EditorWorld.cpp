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

#include "EditorWorld.h"
#include "EditorBaseClasses.h"
#include "EntityWrapper.h"
#include "EntityWrapperParticleSystem.h"
#include "EntityPicker.h"
#include "SurfacePicker.h"
#include "EditorClipPlane.h"
#include "EditorHelper.h"
#include "EditorFileWatcher.h"
#include "PrefabManager.h"

#include "resources/XmlHelper.h"
#include "scene/LightParameters.h"
#include <tinyxml2.h>

#include <algorithm>

static tString gsEmptyString = "";

//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

iEditorWorld::iEditorWorld(iEditorBase* apEditor, const tString& asElementName)
{
	mpEditor = apEditor;
	msElementName = asElementName;
	mpWorld = mpEditor->GetEngine()->GetScene()->CreateWorld("Level");
	mpWorld->SetSkyBoxActive(true);

	mpPicker = hplNew(cEntityPicker,(this));
	mpSurfacePicker = hplNew(cSurfacePicker,(this));

	mpFileWatcher = hplNew(cEditorFileWatcher,());
	mFileReloadHandler.Connect(mpFileWatcher->OnReloadEntities());

	// Prefab-definition changes are delivered per-instance now: each placed
	// cEntityWrapperEntity subscribes to its own cPrefabResource::OnModified() and
	// rebuilds itself (see UpdatePrefabRef / OnPrefabModified). No world-level
	// prefab broadcast subscription.

	mbClipPlaneNumUpdated = true;

	mlDefaultCategory = -1;

	//////////////////////////////////////////////
	// Global light creation
	mpGlobalPointLight = mpWorld->CreateLightPointLegacy("GlobalPoint");
	mpGlobalPointLight->SetPosition(cVector3f(0,100,0));
	mpGlobalPointLight->SetRadius(500);
	mpGlobalPointLight->SetDiffuseColor(cColor(1,1));

	mbVisibilityUpdated = true;

	mvMinPos = -10000;
	mvMaxPos = 10000;

	mbShowLoadErrorPopUp = false;
}

//----------------------------------------------------------------------------

iEditorWorld::~iEditorWorld()
{
	ClearEntities();

	hplDelete(mpFileWatcher);
	mpFileWatcher = NULL;

	STLDeleteAll(mlstEntityTypes);

	hplDelete(mpPicker);
	hplDelete(mpSurfacePicker);

	mpEditor->GetEngine()->GetScene()->DestroyWorld(mpWorld);
}

//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

void iEditorWorld::OnEditorUpdate(float afTimeStep)
{
	if(mpFileWatcher)
		mpFileWatcher->Poll(afTimeStep);

	if(mbClipPlanesUpdated)
	{
		mbClipPlanesUpdated = false;
		const tEntityWrapperMap& mapEntities = GetEntities();
		tEntityWrapperMapConstIt itEntities = mapEntities.begin();
		tEditorClipPlaneVec& vPlanes = GetClipPlanes();
		for(;itEntities!=mapEntities.end();++itEntities)
		{
			iEntityWrapper* pEnt = itEntities->second;
			pEnt->CheckCulledByClipPlanes(vPlanes);
		}
	}
}

//----------------------------------------------------------------------------

void iEditorWorld::Reset()
{
	SetBGDefaultColor(cColor(0.2f,1));

	ClearEntities();

	mmapFileIndices.clear();

	mlIDCounter = 0;

	mlNumModifications = 0;
	mlLastSavedModification = 0;

	///////////////////////////////////////
	// Global Lighting
	SetGlobalAmbientLightEnabled(true);
	SetGlobalPointLightEnabled(true);
	
	for(int i=0;i<GetEntityTypeNum();++i)
	{
		iEntityWrapperType* pType = GetEntityType(i);
		pType->SetVisible(true);
		pType->SetActive(true);
	}
	UpdateVisibility();

	ClearClipPlanes();

	SetShowSkybox(true);
	SetSkyboxTexture("");
	SetSkyboxColor(cColor(1));
	SetSkyboxActive(false);

	mbShowOtherRendererObjects = false;
	SetShowFog(true);
	SetFogActive(false);
	SetFogCulling(true);
	SetFogStart(0);
	SetFogEnd(20);
	SetFogFalloffExp(1);
	SetFogColor(cColor(1));
}

//----------------------------------------------------------------------------

bool iEditorWorld::AddObject(iEntityWrapper* apObject)
{
	if(apObject==NULL)
		return false;

	// Safety net for paths that skip CreateEntityWrapperFromData (particle editor
	// emitters): every object leaves AddObject with a unique nonzero GUID.
	if(apObject->GetGUID()==0)
		apObject->SetGUID(hash_random());

	mmapEntities.insert(std::pair<unsigned int, iEntityWrapper*>(apObject->GetID(), apObject));

	// Call on add stuff.
	apObject->OnAddToWorld();

	//Log("Added entity with key %d - map size:%d\n", apObject->GetID(), mmapEntities.size());

	if(mlIDCounter <= apObject->GetID())
		mlIDCounter = apObject->GetID()+1;

	return true;
}

//----------------------------------------------------------------------------

void iEditorWorld::RemoveObject(iEntityWrapper* apObject)
{
	if(apObject)
	{
		// Drop the file watches for this entity. Skipped during a full clear -
		// ClearEntities() tears the whole watcher down in one shot instead.
		if(mbIsClearingEntities==false && mpFileWatcher)
			mpFileWatcher->UnregisterEntity(apObject->GetID());

		// Free the GUID so delete-undo can restore the object with its original one.
		msetGUIDsInUse.erase(apObject->GetGUID());

		mmapEntities.erase(apObject->GetID());
	}
}

//----------------------------------------------------------------------------

bool iEditorWorld::IsIDInUse(int alID)
{
	if(alID<0)
		return true;
	if(alID>=mlIDCounter || mmapEntities.find(alID)==mmapEntities.end())
		return false;

	return true;
}
//----------------------------------------------------------------------------

iEntityWrapper* iEditorWorld::GetEntity(int alID)
{
	std::map<int,int>::iterator itDupIDs = mmapIDRedirectors.find(alID);
	if(itDupIDs!=mmapIDRedirectors.end())
        alID = itDupIDs->second;

	tEntityWrapperMapIt it = mmapEntities.find(alID);
	if(it==mmapEntities.end())
		return NULL;

	return it->second;
}

//--------------------------------------------------------------------

iEntityWrapper* iEditorWorld::GetEntityByName(const tString& asName)
{
	if(asName!="")
	{
		tEntityWrapperMapIt it = mmapEntities.begin();
		for(;it!=mmapEntities.end();++it)
		{
			iEntityWrapper* pEnt = it->second;
			if(pEnt->GetName()==asName)
				return it->second;
		}
	}

	return NULL;
}

//--------------------------------------------------------------------

iEntityWrapper* iEditorWorld::GetEntityByGUID(unsigned long long alGUID)
{
	if(alGUID!=0)
	{
		tEntityWrapperMapIt it = mmapEntities.begin();
		for(;it!=mmapEntities.end();++it)
		{
			iEntityWrapper* pEnt = it->second;
			if(pEnt->GetGUID()==alGUID)
				return it->second;
		}
	}

	return NULL;
}

//--------------------------------------------------------------------

bool iEditorWorld::HasEntity(iEntityWrapper* apObject)
{
	if(apObject==NULL)
		return false;

	tEntityWrapperMapIt it = mmapEntities.begin();
	for(;it!=mmapEntities.end();++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(pEnt==apObject)
			return true;
	}
	return false;
}

void iEditorWorld::ClearEntities()
{
	mbIsClearingEntities=true;

	tEntityWrapperMap mapEntitiesCopy = mmapEntities;
	tEntityWrapperMapIt it = mapEntitiesCopy.begin();
	for(;it!=mapEntitiesCopy.end();++it)
		DestroyEntityWrapper(it->second,false);

	mmapEntities.clear();
	msetGUIDsInUse.clear();

	// One-shot teardown of every watch (RemoveObject skips per-entity
	// unregisters while mbIsClearingEntities is set).
	if(mpFileWatcher)
		mpFileWatcher->Clear();

	mbIsClearingEntities=false;
}

//--------------------------------------------------------------------

void iEditorWorld::OnWatcherReload(const std::set<int>& asetIDs)
{
	// The watcher fires only for ON-DISK changes. A pending prefab doc that is
	// not dirty was persisted by us (content == old disk), so dropping it makes
	// the rebuild below read the NEW disk content. Dirty docs are unsaved MCP
	// edits — kept (OnExternalFileChange warns about the conflict).
	if(cPrefabManager* pMgr = mpEditor->GetPrefabManager())
	{
		for(std::set<int>::const_iterator it=asetIDs.begin(); it!=asetIDs.end(); ++it)
		{
			iEntityWrapper* pEnt = GetEntity(*it);
			if(pEnt && cString::ToLowerCase(cString::GetFileExt(pEnt->GetFilename()))=="ent")
				pMgr->OnExternalFileChange(pEnt->GetFilename(), false);
		}
	}

	ReloadEntities(asetIDs);
}

//--------------------------------------------------------------------

std::set<int> iEditorWorld::FindEntityIDsByFilename(const tString& asFile)
{
	std::set<int> setIDs;
	tString sBase = cString::ToLowerCase(cString::GetFileName(asFile));
	if(sBase.empty())
		return setIDs;

	for(tEntityWrapperMapIt it=mmapEntities.begin(); it!=mmapEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(cString::ToLowerCase(cString::GetFileName(pEnt->GetFilename()))==sBase)
			setIDs.insert(pEnt->GetID());
	}
	return setIDs;
}

//--------------------------------------------------------------------

void iEditorWorld::ReloadEntities(const std::set<int>& asetIDs)
{
	if(asetIDs.empty())
		return;

	////////////////////////////////////////////////////////////////////
	// Phase 1: snapshot each affected entity's state, then tear down ALL
	// their engine entities before rebuilding any. Resource managers cache
	// by path hash, so a shared mesh/material/texture only re-reads disk once
	// its refcount hits zero - which needs every holder torn down first.
	std::vector<iEntityWrapper*>		vEnts;
	std::vector<iEntityWrapperData*>	vData;
	vEnts.reserve(asetIDs.size());
	vData.reserve(asetIDs.size());

	for(std::set<int>::const_iterator it=asetIDs.begin(); it!=asetIDs.end(); ++it)
	{
		iEntityWrapper* pEnt = GetEntity(*it);
		if(pEnt==NULL)
			continue;

		vEnts.push_back(pEnt);
		vData.push_back(pEnt->CreateCopyData());
		pEnt->DestroyEngineEntity();
	}

	////////////////////////////////////////////////////////////////////
	// Phase 2: rebuild each entity from its snapshot. The first rebuild of a
	// given resource re-reads disk; the rest reuse the freshly loaded cache.
	for(size_t i=0; i<vEnts.size(); ++i)
	{
		iEntityWrapper* pEnt = vEnts[i];
		iEntityWrapperData* pData = vData[i];

		pData->CopyToEntity(pEnt, ePropCopyStep_PreEnt);
		pEnt->CreateEngineEntity();
		pData->CopyToEntity(pEnt, ePropCopyStep_PostEnt);
		pEnt->UpdateEntity();

		// Re-push the selection highlight onto the rebuilt engine entity.
		if(pEnt->IsSelected())
			pEnt->SetSelected(true);

		// Re-sync watches in case the reload changed the dependency set
		// (e.g. an edited .ent now points at a different mesh/material).
		// RegisterFileWatches() unregisters first, so this is a clean re-sync.
		pEnt->RegisterFileWatches();

		hplDelete(pData);
	}

	Log("[FileWatch] reloaded %d entit%s from disk\n",
		(int)vEnts.size(), vEnts.size()==1 ? "y" : "ies");

	// A live external reload must NOT dirty the map or push an undo action,
	// so no IncModifications()/action creation here.
}

//--------------------------------------------------------------------

std::set<int> iEditorWorld::FindParticleSystemIDsByFile(const tString& asFile)
{
	std::set<int> setIDs;
	// Normalize the extension on both sides: CreatePS() itself does
	// SetFileExt("ps") internally, and the editor stores the bare filename WITH
	// extension - normalizing guards against any legacy no-extension value.
	tString sBase = cString::ToLowerCase(cString::GetFileName(cString::SetFileExt(asFile,"ps")));
	if(sBase.empty())
		return setIDs;

	for(tEntityWrapperMapIt it=mmapEntities.begin(); it!=mmapEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(pEnt->GetTypeID()!=eEditorEntityType_ParticleSystem)
			continue;
		cEntityWrapperParticleSystem* pPS = static_cast<cEntityWrapperParticleSystem*>(pEnt);
		if(cString::ToLowerCase(cString::GetFileName(cString::SetFileExt(pPS->GetFile(),"ps")))==sBase)
			setIDs.insert(pEnt->GetID());
	}
	return setIDs;
}

//--------------------------------------------------------------------

void iEditorWorld::ReloadParticleSystems(const std::set<int>& asetIDs)
{
	if(asetIDs.empty())
		return;

	std::vector<cEntityWrapperParticleSystem*> vPS;
	vPS.reserve(asetIDs.size());

	////////////////////////////////////////////////////////////////////
	// Phase 1: destroy EVERY matching live instance first. Each teardown drops
	// one cParticleSystemData reference; when the last matching instance goes,
	// cParticleManager auto-frees the cached data (refcount 0 -> FreeResource).
	// Two phases are MANDATORY: interleaving would let a recreate reuse the
	// stale cached data before it is evicted.
	//
	// NOTE: this only evicts when the LAST reference drops. If the same .ps is
	// also referenced elsewhere (e.g. a particle embedded in a loaded model),
	// its reference keeps the cached data alive and the recreate reuses it;
	// those embedded copies refresh when their model reloads instead.
	for(std::set<int>::const_iterator it=asetIDs.begin(); it!=asetIDs.end(); ++it)
	{
		iEntityWrapper* pEnt = GetEntity(*it);
		if(pEnt==NULL || pEnt->GetTypeID()!=eEditorEntityType_ParticleSystem)
			continue;
		cEntityWrapperParticleSystem* pPS = static_cast<cEntityWrapperParticleSystem*>(pEnt);
		vPS.push_back(pPS);
		pPS->DestroyEngineEntity();
	}

	////////////////////////////////////////////////////////////////////
	// Phase 2: rebuild. The cache is now empty for this path, so the first
	// ReCreatePS re-reads disk and the rest reuse the freshly loaded data.
	for(size_t i=0; i<vPS.size(); ++i)
	{
		cEntityWrapperParticleSystem* pPS = vPS[i];
		pPS->CreateEngineEntity();
		pPS->UpdatePS();        // mark mbTypeUpdated + mbDataUpdated
		pPS->UpdateEntity();    // synchronous icon Update() -> ReCreatePS (reads disk) + reapply color/fade + placement

		if(pPS->IsSelected())
			pPS->SetSelected(true);

		pPS->RegisterFileWatches();
	}

	Log("[FileWatch] reloaded %d particle system%s from disk\n",
		(int)vPS.size(), vPS.size()==1 ? "" : "s");

	// A live external reload must NOT dirty the map or push an undo action.
}

//--------------------------------------------------------------------

void iEditorWorld::SetGlobalAmbientLightEnabled(bool abX)
{
	// Box-light-based global ambient was removed; kept as a no-op so existing
	// editor toolbar/checkbox call sites stay unchanged.
}

//--------------------------------------------------------------------

void iEditorWorld::SetGlobalPointLightEnabled(bool abX)
{
	if(mpGlobalPointLight)
		mpGlobalPointLight->SetVisible(abX);
}


//--------------------------------------------------------------------

bool iEditorWorld::GetGlobalAmbientLightEnabled()
{
	// Box-light-based global ambient was removed; always reports disabled.
	return false;
}

//--------------------------------------------------------------------

bool iEditorWorld::GetGlobalPointLightEnabled()
{
	if(mpGlobalPointLight)
		return mpGlobalPointLight->IsVisible();
	
	return false;
}


//----------------------------------------------------------------------------

bool iEditorWorld::Load(tinyxml2::XMLElement* apDoc)
{
	mlIDCounter = 0;
	if(apDoc->Value()!=msElementName)
		return false;

	tinyxml2::XMLElement* pXmlWorldData = GetWorldDataElement(apDoc);
	if(pXmlWorldData==NULL)
		return false;
	LoadWorldData(pXmlWorldData);

	tinyxml2::XMLElement* pXmlObjectsData = GetWorldObjectsElement(pXmlWorldData);
	if(pXmlObjectsData)
		LoadWorldObjects(pXmlObjectsData);

	//////////////////////////////////////////////////
	// We just loaded the file, set modified to false
	mlNumModifications = 0;
	mlLastSavedModification = 0;

	mpEditor->SetWorldModified();
	mpEditor->SetLayoutNeedsUpdate(true);

	return true;
}

//----------------------------------------------------------------------------

void iEditorWorld::LoadWorldData(tinyxml2::XMLElement* apWorldDataElement)
{
	SetName(GetAttributeString(apWorldDataElement, "Name", ""));
}

//----------------------------------------------------------------------------

void iEditorWorld::LoadWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement)
{
	LoadFileIndices(apWorldObjectsElement);

	mbShowLoadErrorPopUp = false;
	msLoadErrorMsg = _W("");

	for(int i=0; i<(int)mvEntityCategories.size(); ++i)
	{
		//////////////////////////////////////////////////////////////////
		// Iterate through category strings and all entities in each
		tinyxml2::XMLElement* pCurrentXmlCategory = apWorldObjectsElement->FirstChildElement(mvEntityCategories[i].c_str());

		if(pCurrentXmlCategory==NULL ||
			CustomCategoryLoader(apWorldObjectsElement, pCurrentXmlCategory)==true)
			continue;

		for(tinyxml2::XMLElement* pXmlEntity = pCurrentXmlCategory->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
		{
			iEntityWrapper* pEnt = NULL;

			pEnt = CreateEntityWrapperFromXMLElement(pXmlEntity);
			if(pEnt==NULL)
				mbShowLoadErrorPopUp = true;
		}
	}

	tEntityWrapperMap::const_iterator it = mmapEntities.begin();
	for(;it!=mmapEntities.end();++it)
	{
		iEntityWrapper* pEnt = it->second;
		pEnt->OnPostDeployAll(true);
	}

	///////////////////////////////////////////////////////////////
	// ID resolution (children, joint parent/child bodies) happens inside the
	// OnPostDeployAll loop above via GetEntity, which consults the redirector map.
	// Only now that all IDs are resolved is it safe to clear the redirectors —
	// clearing earlier would strand joints whose connected body got a redirected ID.
	mmapIDRedirectors.clear();

	if(mbShowLoadErrorPopUp)
	{
		mpEditor->ShowMessageBox(_W("Warning!"), _W("Found some errors while loading file. Check log for details. ") + msLoadErrorMsg , 
									_W("OK"), _W(""), NULL, NULL);

		mbShowLoadErrorPopUp = false;
	}
}

//----------------------------------------------------------------------------



//----------------------------------------------------------------------------

bool iEditorWorld::Save(tinyxml2::XMLElement* apDoc)
{
	apDoc->SetValue(msElementName.c_str());
	tinyxml2::XMLElement* pXmlWorldData = CreateWorldDataElement(apDoc);
	if(pXmlWorldData==NULL) return true;

	SaveWorldData(pXmlWorldData);

	tinyxml2::XMLElement* pXmlWorldObjects = CreateWorldObjectsElement(pXmlWorldData);
	if(pXmlWorldObjects)
	{
		tEntityWrapperList lstEntities;
		tEntityWrapperMapIt it = mmapEntities.begin();
		for(;it!=mmapEntities.end();++it)
			lstEntities.push_back(it->second);

		SaveWorldObjects(pXmlWorldObjects, lstEntities);
	}

	return true;
}

//----------------------------------------------------------------------------

void iEditorWorld::SaveWorldData(tinyxml2::XMLElement* apWorldDataElement)
{
	SetAttributeString(apWorldDataElement, "Name", msName);
}

void iEditorWorld::SaveWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement, tEntityWrapperList& alstEnts)
{
    SaveFileIndices(apWorldObjectsElement);

	std::vector<tinyxml2::XMLElement*> vParentElements;
	for(int i=0;i<(int)mvEntityCategories.size();++i)
	{
		tinyxml2::XMLElement* pCategory = apWorldObjectsElement->GetDocument()->NewElement(mvEntityCategories[i].c_str());
		apWorldObjectsElement->InsertEndChild(pCategory);
		vParentElements.push_back(pCategory);
	}

	///////////////////////////////////////////////////
	// Clear outliers before going through entities
	mlstOutliers.clear();

	CustomCategorySaver(apWorldObjectsElement);
	
	tEntityWrapperListIt it = alstEnts.begin();
	for(;it!=alstEnts.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		pEnt->Save(GetParentElementForObject(vParentElements, pEnt));
	}

	///////////////////////////////////////
	// Show warning if outliers found!
	if(mlstOutliers.empty()==false)
	{
		mpEditor->ShowMessageBox(_W("Warning!"), _W("Found some outlier entities while saving. Check log for details"), 
									_W("OK"), _W(""), NULL, NULL);



		Log("Outlier entities found: \n");
		tEntityWrapperListIt it = mlstOutliers.begin();
		for(;it!=mlstOutliers.end();++it)
		{
			iEntityWrapper* pEnt = *it;

			Log("\t %s \n", pEnt->GetName().c_str());
		}
	}

}

//----------------------------------------------------------------------------

typedef std::map<int, int> tIDRedirector;
typedef std::map<tString, tIDRedirector> tMapIDRedirectors;

//----------------------------------------------------------------------------

void iEditorWorld::ImportObjects(const tString& asX, tIntList& alstImportedIDs)
{
	tinyxml2::XMLElement* pDoc = mpEditor->GetEngine()->GetResources()->LoadXmlDocument(asX);
	if(pDoc==NULL)
		return;

	ImportObjects(pDoc, alstImportedIDs);
	mpEditor->GetEngine()->GetResources()->DestroyXmlDocument(pDoc);
}

bool iEditorWorld::ImportObjects(tinyxml2::XMLElement* apRootElem, tIntList& alstImportedIDs)
{
	if(apRootElem==NULL)
		return false;

	// Accept either a <Level> root (as .map / .expobj files have) or the
	// <MapData> element directly.
	tinyxml2::XMLElement* pWorldData = apRootElem->FirstChildElement("MapData");
	if(pWorldData==NULL && tString(apRootElem->Value())=="MapData")
		pWorldData = apRootElem;
	if(pWorldData==NULL)
		return false;

	tinyxml2::XMLElement* pXmlObjectsData = pWorldData->FirstChildElement("MapContents");
	if(pXmlObjectsData==NULL)
		return false;

	tStringVec vIndexStrings;
	vIndexStrings.push_back("StaticObjects");
	vIndexStrings.push_back("Entities");
	vIndexStrings.push_back("Decals");

	tStringVec vElementStrings;
	vElementStrings.push_back("StaticObject");
	vElementStrings.push_back("Entity");
	vElementStrings.push_back("Decal");

	tMapIDRedirectors mapFileIndexRedirectors;
	tIDRedirector mapIDRedirectors;

	for(int i=0;i<(int)vIndexStrings.size();++i)
	{
		tString& sIndex = vIndexStrings[i];
		tinyxml2::XMLElement* pIndex = pXmlObjectsData->FirstChildElement(("FileIndex_" + sIndex).c_str());
		if(pIndex)
		{
			tIDRedirector& mapRedirector = mapFileIndexRedirectors[vElementStrings[i]];
			tStringVec& vFiles = mmapFileIndices[sIndex];

			for(tinyxml2::XMLElement* pFile = pIndex->FirstChildElement(); pFile != NULL; pFile = pFile->NextSiblingElement())
			{
				int lOldIdx = GetAttributeInt(pFile, "Id");
				tString sFilename = GetFileRelativePath(GetAttributeString(pFile, "Path"));
				int lNewIdx = AddFilenameToIndex(vFiles, sFilename);
				mapRedirector.insert(std::pair<int,int>(lOldIdx, lNewIdx));
			}
		}
	}
	
	for(int i=0; i<(int)mvEntityCategories.size(); ++i)
	{
		//////////////////////////////////////////////////////////////////
		// Iterate through category strings and all entities in each
		tinyxml2::XMLElement* pCurrentXmlCategory = pXmlObjectsData->FirstChildElement(mvEntityCategories[i].c_str());

		if(pCurrentXmlCategory==NULL)
			continue;

		for(tinyxml2::XMLElement* pXmlEntity = pCurrentXmlCategory->FirstChildElement(); pXmlEntity != NULL; pXmlEntity = pXmlEntity->NextSiblingElement())
		{
			iEntityWrapper* pEnt = NULL;
			tString sVal = pXmlEntity->Value();

			int lFileIndex=GetAttributeInt(pXmlEntity, "FileIndex",-1);
			if(lFileIndex!=-1)
				SetAttributeInt(pXmlEntity, "FileIndex", mapFileIndexRedirectors[sVal][lFileIndex]);

			int lNewID = GetFreeID();
			int lID = GetAttributeInt(pXmlEntity, "ID", lNewID);
			tString sName = GetAttributeString(pXmlEntity, "Name", "");
			const hpl::cLightElementInfo lightInfo = hpl::GetLightElementInfo(sVal.c_str());
			const int lRendererMask = GetAttributeInt(pXmlEntity, "RendererMask",
				static_cast<int>(lightInfo.mbValid ? hpl::GetDefaultLightRendererMask(lightInfo) : hpl::kRendererMaskAll));
			if(IsNameAvailable(sName, lRendererMask)==false)
				SetAttributeString(pXmlEntity, "Name", GenerateName(sName));

			mapIDRedirectors.insert(std::pair<int, int>(lID, lNewID));
			SetAttributeInt(pXmlEntity, "ID", lNewID);

			pEnt = CreateEntityWrapperFromXMLElement(pXmlEntity);
			alstImportedIDs.push_back(lNewID);
		}
	}

	tIntListIt it = alstImportedIDs.begin();
	for(;it!=alstImportedIDs.end();++it)
	{
		iEntityWrapper* pEnt = GetEntity(*it);
		//pEnt->PostCreateAllLoadedObjects();
		//pEnt->PostCreationCleanUp();
	}

	return true;
}

//----------------------------------------------------------------------------

void iEditorWorld::ExportObjects(const tString& asX, tEntityWrapperList& alstEntsToExport)
{
	tinyxml2::XMLDocument xmlDoc;
	tinyxml2::XMLElement* pDoc = xmlDoc.NewElement("Level");
	xmlDoc.InsertEndChild(pDoc);

	tinyxml2::XMLElement* pXmlWorldData = CreateWorldDataElement(pDoc);
	SaveWorldData(pXmlWorldData);

	tinyxml2::XMLElement* pXmlWorldObjects = CreateWorldObjectsElement(pXmlWorldData);
	if(pXmlWorldObjects)
		SaveWorldObjects(pXmlWorldObjects, alstEntsToExport);

	hpl::SaveXmlFile(xmlDoc, cString::To16Char(asX));
}

//----------------------------------------------------------------------------

void iEditorWorld::SetName(const tString& asName)
{
	if(msName==asName)
		return;

	msName = asName;
	IncModifications();
}

//----------------------------------------------------------------------------

void iEditorWorld::IncModifications()
{
	++mlNumModifications;
	mpEditor->SetWorldModified();
}

void iEditorWorld::DecModifications()
{
	--mlNumModifications;
	mpEditor->SetWorldModified();
}

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

void iEditorWorld::SetShowOtherRendererObjects(bool abX)
{
	if(mbShowOtherRendererObjects==abX) return;

	mbShowOtherRendererObjects = abX;
	SetVisibilityUpdated();
	UpdateVisibility();
}

//----------------------------------------------------------------------------

bool iEditorWorld::IsNameAvailable(const tString& asName, int alRendererMask)
{
	if(asName=="")
		return false;

	bool bAvailable = true;

	tEntityWrapperMapIt it = mmapEntities.begin();
	for(;it!=mmapEntities.end();++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(pEnt==NULL)continue;
		if(asName == pEnt->GetName() &&
			(static_cast<unsigned>(pEnt->GetRendererMask()) & static_cast<unsigned>(alRendererMask)) != 0)
		{
			bAvailable = false;
			break;
		}
	}
	return bAvailable;
}

//----------------------------------------------------------------------------

void iEditorWorld::AddEntityType(iEntityWrapperType* apType)
{
	if(apType==NULL)
		return;

	apType->SetWorld(this);
	//mmapEntityTypes.insert(pair<tString, iEntityWrapperType*>(apType->GetXmlElementName(), apType));
	mlstEntityTypes.push_back(apType);

	Log("Added object type: %s\n", apType->ToString().c_str());
}

//----------------------------------------------------------------------------

iEntityWrapperType* iEditorWorld::GetEntityTypeByName(const tString& asX)
{
	tWString sName = cString::To16Char(asX);
	iEntityWrapperType* pType = NULL;
	tEntityTypeListIt it = mlstEntityTypes.begin();
	for(;it!=mlstEntityTypes.end();++it)
	{
		iEntityWrapperType* pType = *it;
		if(pType->GetName()==sName)
			return pType;
	}
	
	Log("Type %s not created.\n", asX.c_str());

	return NULL;
}

//----------------------------------------------------------------------------

iEntityWrapperType* iEditorWorld::GetEntityTypeByID(int alID)
{
	tEntityTypeListIt it = mlstEntityTypes.begin();
	for(;it!=mlstEntityTypes.end();++it)
	{
		iEntityWrapperType* pType = *it;
		if(pType->GetID()==alID)
			return pType;
	}

	Error("Type with ID %d not created.\n", alID);

	return NULL;

}

//----------------------------------------------------------------------------

iEntityWrapperType* iEditorWorld::GetEntityTypeByElement(tinyxml2::XMLElement* apElement)
{
	iEntityWrapperType* pDefaultType = NULL;
	tEntityTypeListIt it = mlstEntityTypes.begin();
	for(;it!=mlstEntityTypes.end();++it)
	{
		iEntityWrapperType* pType = *it;
		if(pType->IsAppropriateType(apElement))
			return pType;

		if(pDefaultType==NULL && pType->IsAppropriateDefaultType(apElement))
			pDefaultType = pType;
	}

	SetShowLoadErrorPopUp();
	Error("No appropriate type found for %s \"%s\" with ID %d\n", apElement->Value(), GetAttributeString(apElement, "Name").c_str(),
																GetAttributeInt(apElement, "ID"));
	if(pDefaultType)
	{
		Log("Trying default...%s\n", pDefaultType->ToString().c_str());
		return pDefaultType;
	}
	else
		Log("No default type found\n");


	return NULL;
}

//----------------------------------------------------------------------------

int iEditorWorld::GetEntityTypeNum()
{
	return (int)mlstEntityTypes.size();
}

//----------------------------------------------------------------------------

iEntityWrapperType* iEditorWorld::GetEntityType(int alX)
{
	int i=0;
	tEntityTypeListIt it = mlstEntityTypes.begin();
	for(;it!=mlstEntityTypes.end();++it, ++i)
	{
		if(i==alX)
			return *it;
	}

	return NULL;
}

//----------------------------------------------------------------------------

void iEditorWorld::SetTypeVisibility(int alTypeID, bool abX)
{
	iEntityWrapperType* pType = GetEntityTypeByID(alTypeID);
	if(pType) pType->SetVisible(abX);

	UpdateVisibility();
}

//----------------------------------------------------------------------------

bool iEditorWorld::GetTypeVisibility(int alTypeID)
{
	iEntityWrapperType* pType = GetEntityTypeByID(alTypeID);
	if(pType) return pType->IsVisible();

	return false;
}

//----------------------------------------------------------------------------

void iEditorWorld::UpdateVisibility()
{
	if(mbVisibilityUpdated==false)
		return;

	mbVisibilityUpdated = false;
	tEntityWrapperMapIt it = mmapEntities.begin();
	for(;it!=mmapEntities.end();++it)
	{
		iEntityWrapper* pEnt = it->second;
		pEnt->UpdateVisibility();
	}
}

//----------------------------------------------------------------------------

void iEditorWorld::SetTypeActive(int alTypeID, bool abX)
{
	iEntityWrapperType* pType = GetEntityTypeByID(alTypeID);
	if(pType) pType->SetActive(abX);

	UpdateVisibility();
}

//----------------------------------------------------------------------------

bool iEditorWorld::GetTypeActive(int alTypeID)
{
	iEntityWrapperType* pType = GetEntityTypeByID(alTypeID);
	if(pType) return pType->IsActive();

	return false;
}

//----------------------------------------------------------------------------

cEditorClipPlane* iEditorWorld::AddClipPlane()
{
	cEditorClipPlane* pPlane = hplNew(cEditorClipPlane,(this));
	mvClipPlanes.push_back(pPlane);

	SetClipPlanesUpdated();
	mbClipPlaneNumUpdated = true;

	return pPlane;
}

void iEditorWorld::RemoveClipPlane(cEditorClipPlane* apPlane, bool abDestroy)
{
	tEditorClipPlaneVecIt it = mvClipPlanes.begin();
	for(;it!=mvClipPlanes.end();++it)
	{
		cEditorClipPlane* pPlane = *it;
		if(apPlane==pPlane)
		{
			mvClipPlanes.erase(it);
			if(abDestroy)
				hplDelete(*it);

			break;
		}
	}

	SetClipPlanesUpdated();
	mbClipPlaneNumUpdated = true;
}

void iEditorWorld::RemoveClipPlane(int alIdx, bool abDestroy)
{
	RemoveClipPlane(mvClipPlanes[alIdx], abDestroy);
}

void iEditorWorld::ClearClipPlanes(bool abDestroyPlanes)
{
    if(abDestroyPlanes)
		STLDeleteAll(mvClipPlanes);
	mvClipPlanes.clear();

	SetClipPlanesUpdated();
	mbClipPlaneNumUpdated = true;
}

//----------------------------------------------------------------------------

void iEditorWorld::SetClipPlanes(const tEditorClipPlaneVec& avX)
{
	if(mvClipPlanes==avX)
		return;
	
	mvClipPlanes = avX;

	SetClipPlanesUpdated();
	mbClipPlaneNumUpdated = true;
}

//----------------------------------------------------------------------------

int iEditorWorld::AddEntityCategory(const tString& asX, int alType)
{
	if(find(mvEntityCategories.begin(), mvEntityCategories.end(), asX)==mvEntityCategories.end())
	{
		mvEntityCategories.push_back(asX);
		int lIndex = (int)mvEntityCategories.size()-1;
		mvEntityCategoryTypes.resize(lIndex+1);

		if(alType!=-1)
		{
			AddTypeToCategory(lIndex, alType);
		}
		else
		{
			if(mlDefaultCategory==-1) 
				mlDefaultCategory = lIndex;
		}

		return lIndex;
	}

	return -1;
}

//----------------------------------------------------------------------------

void iEditorWorld::AddTypeToCategory(int alCatID, int alType)
{
	mvEntityCategoryTypes[alCatID].push_back(alType);
}

//----------------------------------------------------------------------------

tinyxml2::XMLElement* iEditorWorld::GetParentElementForObject(std::vector<tinyxml2::XMLElement*>& avParentElements, iEntityWrapper* apEnt)
{
	int type = apEnt->GetTypeID();
	for(int i=0;i<(int)mvEntityCategoryTypes.size();++i)
	{
		tIntList& lstTypes = mvEntityCategoryTypes[i];
		if(find(lstTypes.begin(), lstTypes.end(), type)!=lstTypes.end())
			return avParentElements[i];
	}

	return avParentElements[mlDefaultCategory];
}

//----------------------------------------------------------------------------

void iEditorWorld::AddFileIndex(const tString& asName, int alTypeID)
{
	mvFileIndexNames.push_back(asName);
	mvFileIndexTypeIDs.push_back(alTypeID);
}

int iEditorWorld::AddFilenameToIndex(const tString& asIndexName, const tString& asFilename)
{
	// Debug
	//Log("Called AddFilenameToIndex %s\n", asFilename.c_str());

	tStringVec& vFileVec = mmapFileIndices[asIndexName];

	// Debug
	//Log("Filename vec before (size %d):\n", vFileVec.size());
	//for(int i=0;i<(int)vFileVec.size();++i)
	//{
	//	Log("   %d %s\n", i, vFileVec[i].c_str());
	//}

	return AddFilenameToIndex(vFileVec, asFilename);

	// Debug
	//int lIndex = AddFilenameToIndex(vFileVec, asFilename);
	//Log("Filename vec after (size %d):\n", vFileVec.size());
	//for(int i=0;i<(int)vFileVec.size();++i)
	//{
	//	Log("   %d %s\n", i, vFileVec[i].c_str());
	//}
	//Log("Returning %d\n", lIndex);

	//return lIndex;
}

int iEditorWorld::AddFilenameToIndex(tStringVec& avIndex, const tString& asFilename)
{
	if(asFilename=="")
		return -1;

	tString sFilename = GetFileRelativePath(asFilename);
	for(int i=0;i<(int)avIndex.size();++i)
	{
        if(sFilename==avIndex[i])
			return i;
	}
	avIndex.push_back(sFilename);

	return (int)avIndex.size()-1;
}

//----------------------------------------------------------------------------

const tString& iEditorWorld::GetFilenameFromIndex(const tString& asIndexName, int alIndex)
{
	std::map<tString,tStringVec>::iterator it = mmapFileIndices.find(asIndexName);
	if(it!=mmapFileIndices.end())
	{
		tStringVec& vFiles = it->second;
		if(alIndex>=0 && alIndex<(int)vFiles.size())
            return vFiles[alIndex];
	}

	return gsEmptyString;
}

//----------------------------------------------------------------------------

tString iEditorWorld::GetFileRelativePath(const tString& asX)
{
	cFileSearcher* pFS = mpEditor->GetEngine()->GetResources()->GetFileSearcher();
	tWString sFilenameFullPath = pFS->GetFilePath(asX);
	return cString::To8Char(mpEditor->GetPathRelToWD(sFilenameFullPath));
}

//----------------------------------------------------------------------------

void iEditorWorld::LoadFileIndices(tinyxml2::XMLElement* apWorldObjectsElement)
{
	for(int i=0;i<(int)mvFileIndexNames.size();++i)
	{
		const tString& sIndex = mvFileIndexNames[i];
		tinyxml2::XMLElement* pIndex = apWorldObjectsElement->FirstChildElement(("FileIndex_" + sIndex).c_str());
		if(pIndex)
		{
			tStringVec& vFiles = mmapFileIndices[sIndex];
			vFiles.resize(GetAttributeInt(pIndex, "NumOfFiles"));

			for(tinyxml2::XMLElement* pFile = pIndex->FirstChildElement(); pFile != NULL; pFile = pFile->NextSiblingElement())
			{
				int lIdx = GetAttributeInt(pFile, "Id");
				vFiles[lIdx] = GetFileRelativePath(GetAttributeString(pFile, "Path"));
			}
		}
	}
}

void iEditorWorld::SaveFileIndices(tinyxml2::XMLElement* apWorldObjectsElement)
{
	cFileSearcher* pFS = mpEditor->GetEngine()->GetResources()->GetFileSearcher();
	for(int i=0;i<(int)mvFileIndexNames.size();++i)
	{
		const tString& sIndex = mvFileIndexNames[i];
		tStringVec& vFiles = mmapFileIndices[sIndex];

		SanitizeFileIndex(vFiles, mvFileIndexTypeIDs[i]);

		tinyxml2::XMLElement* pIndex = apWorldObjectsElement->GetDocument()->NewElement(("FileIndex_"+sIndex).c_str());
		apWorldObjectsElement->InsertEndChild(pIndex);

		int lSize = (int)vFiles.size();
		SetAttributeInt(pIndex, "NumOfFiles", (lSize));
		for(int i=0;i<lSize;++i)
		{
			tinyxml2::XMLElement* pFile = pIndex->GetDocument()->NewElement("File");
			pIndex->InsertEndChild(pFile);
			SetAttributeInt(pFile, "Id", i);
			SetAttributeString(pFile, "Path", vFiles[i]);
		}
	}
}

void iEditorWorld::SanitizeFileIndex(tStringVec& avFileIndices, int alTypeID)
{
	if(avFileIndices.empty())
		return;

	// Set usage vector initially to '0'
	tIntVec vFileUsage;
	vFileUsage.resize(avFileIndices.size(), 0);

	// Populate file usage by entity
	tEntityWrapperMapIt itEntities = mmapEntities.begin();
	for(;itEntities!=mmapEntities.end();++itEntities)
	{
        iEntityWrapper* pEnt = itEntities->second;

		int lIndex = pEnt->GetFileIndexForType(alTypeID);

		if(lIndex==-1 || lIndex>=(int)vFileUsage.size())
			continue;

		++vFileUsage[lIndex];
	}

	// DEBUG
	/*
	Log("File index before sorting:\n");
	for(int i=0;i<(int)avFileIndices.size();++i)
		Log(" %s - used %d times\n", avFileIndices[i].c_str(), vFileUsage[i]);
	*/

	// Wherever there is 0 file usage, swap with a nonzero entry
	int lNewIndex = 0;
	int lOldIndex = (int)vFileUsage.size()-1;

	int lNumZeros = 0;

	while(lNewIndex < lOldIndex)
	{
		//Pick rightmost nonzero entry (if zero found, move "pointer" to left and count a zero)
		if(vFileUsage[lOldIndex]==0)
		{
			--lOldIndex;
			++lNumZeros;

			continue;
		}

		//Pick leftmost zero entry (if nonzeros found, move "pointer" to right)
		if(vFileUsage[lNewIndex]!=0)
		{
			++lNewIndex;
			continue;
		}

		// We found another zero, so swap it with the rightmost nonzero! Then relocate all entities to new index
		++lNumZeros;

		vFileUsage[lNewIndex] = vFileUsage[lOldIndex];
		vFileUsage[lOldIndex] = 0;

		tString sTemp = avFileIndices[lNewIndex];
		avFileIndices[lNewIndex] = avFileIndices[lOldIndex];
		avFileIndices[lOldIndex] = sTemp;

		RelocateFileIndex(alTypeID, lOldIndex, lNewIndex);

		++lNewIndex;
		--lOldIndex;
	}


	// Compute new size
	int lNewSize;
	for(lNewSize = 0; lNewSize<(int)vFileUsage.size() && vFileUsage[lNewSize]!=0; ++lNewSize);

	// DEBUG
	//Log("New size: %d (removed %d entries , counted %d zeros)\n", lNewSize, avFileIndices.size()-lNewSize, lNumZeros);

	avFileIndices.resize(lNewSize);

	// DEBUG
	
	Log("File index after removing empty entries:\n");
	for(int i=0;i<(int)avFileIndices.size();++i)
		Log(" %s - used %d times\n", avFileIndices[i].c_str(), vFileUsage[i]);
}

//----------------------------------------------------------------------------

void iEditorWorld::RelocateFileIndex(int alTypeID, int alOldIndex, int alNewIndex)
{
	tEntityWrapperMapIt itEntities = mmapEntities.begin();
	for(;itEntities!=mmapEntities.end();++itEntities)
	{
        iEntityWrapper* pEnt = itEntities->second;

		if(pEnt->GetFileIndexForType(alTypeID)==alOldIndex)
			pEnt->SetFileIndex(alNewIndex);
	}
}

//----------------------------------------------------------------------------

void iEditorWorld::SetBGDefaultColor(const cColor& aX)
{
	mBGDefaultColor = aX;
	mpEditor->SetViewportBGColor(aX);

	UpdateSkybox();
}

//----------------------------------------------------------------------------

void iEditorWorld::SetShowSkybox(bool abX)
{
	mbShowSkybox = abX;

	UpdateSkybox();
}

//----------------------------------------------------------------------------

void iEditorWorld::SetSkyboxActive(bool abX)
{
	mbSkyboxActive = abX;
	
	UpdateSkybox();
}

//----------------------------------------------------------------------------

void iEditorWorld::SetSkyboxTexture(const tString& asX)
{
	if(cEditorHelper::LoadTextureResource( eEditorTextureResourceType_CubeMap, asX, NULL))
		msSkyboxTexture = asX;
	else
		msSkyboxTexture = "";

	UpdateSkybox();
}

//----------------------------------------------------------------------------

void iEditorWorld::SetSkyboxColor(const cColor& aX)
{
	mSkyboxColor = aX;

	UpdateSkybox();
}

//----------------------------------------------------------------------------

void iEditorWorld::UpdateSkybox()
{
	if(GetSkyboxActive() && GetShowSkybox())
	{
		Image* pTex = NULL;
		cEditorHelper::LoadTextureResource(eEditorTextureResourceType_CubeMap, msSkyboxTexture, &pTex);
		mpWorld->SetSkyBox(pTex, true);
		mpWorld->SetSkyBoxColor(mSkyboxColor);	
	}
	else
	{
		mpWorld->SetSkyBox((Image*)NULL,true);
		mpWorld->SetSkyBoxColor(mBGDefaultColor);
	}
}

//----------------------------------------------------------------------------

void iEditorWorld::SetShowFog(bool abX)
{
	mbShowFog = abX;
	SetFogActive(GetFogActive());
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogActive(bool abX)
{
	mbFogActive = abX;
	mpWorld->SetFogActive(abX && mbShowFog &&
		mpEditor->GetVisibilityTypeState(eEditorVisibilityType_GlobalFog));
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogCulling(bool abX)
{
	mpWorld->SetFogCulling(abX);
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogStart(float afX)
{
	mpWorld->SetFogStart(afX);
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogEnd(float afX)
{
	mpWorld->SetFogEnd(afX);
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogFalloffExp(float afX)
{
	mpWorld->SetFogFalloffExp(afX);
}

//----------------------------------------------------------------------------

void iEditorWorld::SetFogColor(const cColor& aX)
{
	mpWorld->SetFogColor(aX);
}

//----------------------------------------------------------------------------

bool iEditorWorld::GetFogCulling()
{
	return mpWorld->GetFogCulling();
}

//----------------------------------------------------------------------------

float iEditorWorld::GetFogStart()
{
	return mpWorld->GetFogStart();
}

//----------------------------------------------------------------------------

float iEditorWorld::GetFogEnd()
{
	return mpWorld->GetFogEnd();
}

//----------------------------------------------------------------------------

float iEditorWorld::GetFogFalloffExp()
{
	return mpWorld->GetFogFalloffExp();
}

//----------------------------------------------------------------------------

const cColor& iEditorWorld::GetFogColor()
{
	return mpWorld->GetFogColor();
}

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

tString iEditorWorld::GenerateName(const tString& asName)
{
	tString sBaseName;
	tString sGeneratedName="";

	int lNum;
	int lIndex;

	lNum = cString::ToInt(cString::GetNumericSuffix(asName, &lIndex).c_str(),-1);
	if(lIndex==-1)
	{
		lNum = 0;
		sBaseName = asName;
	}
	else
	{
		if(asName.at(lIndex-1)!='_')
		{
			lIndex = 0;
			lNum = 0;
		}
		
		sBaseName = cString::Sub(asName, 0, lIndex-1);
	}

	do
	{
		sGeneratedName = sBaseName + "_" + cString::ToString(++lNum);
	}
	while(IsNameAvailable(sGeneratedName)==false);
	
	return sGeneratedName;
}

//----------------------------------------------------------------------------

iEntityWrapper* iEditorWorld::CreateEntityWrapperFromData(iEntityWrapperData* apData)
{
	if(apData==NULL)
		return NULL;

	int lCurrentID = apData->GetID();
	if(IsIDInUse(lCurrentID))
	{
		int lNewID = GetFreeID();
		if(lCurrentID!=-1)
			mmapIDRedirectors.insert(std::pair<int,int>(lCurrentID, lNewID));

		apData->SetID(lNewID);
	}

	// GUID: backfill legacy/new data (0) and dedupe copies (clone, re-import of the
	// same file). Mutates the data like the ID fix-up above, so undo/redo re-creates
	// the object with the same GUID.
	unsigned long long lGUID = apData->GetGUID();
	if(lGUID==0)
		apData->SetGUID(hash_random());

	iEntityWrapper* pEnt = apData->CreateEntity();
	if(AddObject(pEnt)==false)
	{
		DestroyEntityWrapper(pEnt, false);
		pEnt = NULL;
	}

	return pEnt;
}

//----------------------------------------------------------------------------

iEntityWrapper* iEditorWorld::CreateEntityWrapperFromXMLElement(tinyxml2::XMLElement* apElement)
{
	if(apElement==NULL)
		return NULL;

	iEntityWrapperType* pType = GetEntityTypeByElement(apElement);

	iEntityWrapper* pEnt = NULL;
	if(pType)
	{
		iEntityWrapperData* pData = pType->CreateData();

		if(pData && pData->Load(apElement))
			pEnt = CreateEntityWrapperFromData(pData);
	}

	if(pEnt==NULL)
		Log("Failed loading entity of type %s named %s with ID %d\n", apElement->Value(),
																	  GetAttributeString(apElement, "Name").c_str(),
																	  GetAttributeInt(apElement, "ID"));

	return pEnt;
}

//----------------------------------------------------------------------------

void iEditorWorld::SetUpEntityWrapper(iEntityWrapper* apEntity,
									  const cVector3f& avPosition,
									  const cVector3f& avRotation,
									  const cVector3f& avScale)
{
	if(apEntity==NULL)
		return;

	apEntity->SetAbsPosition(avPosition);
	apEntity->SetAbsRotation(avRotation);
	apEntity->SetAbsScale(avScale);

	apEntity->UpdateEntity();
}

//----------------------------------------------------------------------------

void iEditorWorld::DestroyEntityWrapper(iEntityWrapper* apEntity, bool abRemoveFromWorld)
{
	if(apEntity==NULL)
		return;

	if(abRemoveFromWorld) RemoveObject(apEntity);

    apEntity->OnDestroy();	
	hplDelete(apEntity);
}

//----------------------------------------------------------------------------

void iEditorWorld::AddOutlierEntity(iEntityWrapper* apEnt)
{
	if(apEnt==NULL)
		return;

	mlstOutliers.remove(apEnt);
	mlstOutliers.push_back(apEnt);
}

//----------------------------------------------------------------------------

static eEditorBodyShape StringToEditorBodyShape(const tString& asName)
{
	tString sLowName = cString::ToLowerCase(asName);

	if(sLowName == "box")		return eEditorBodyShape_Box;
	if(sLowName == "cylinder")	return eEditorBodyShape_Cylinder;
	if(sLowName == "sphere")	return eEditorBodyShape_Sphere;
	if(sLowName == "capsule")	return eEditorBodyShape_Capsule;

	return eEditorBodyShape_Box;
}

//----------------------------------------------------------------------------


