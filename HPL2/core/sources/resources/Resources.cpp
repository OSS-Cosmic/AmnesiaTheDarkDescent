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

#include "resources/Resources.h"

#include "resources/LowLevelResources.h"
#include "resources/FileSearcher.h"
#include "resources/ImageManager.h"
#include "resources/ParticleManager.h"
#include "resources/SoundManager.h"
#include "resources/FontManager.h"
#include "resources/ScriptManager.h"
#include "resources/TextureManager.h"
#include "resources/MaterialManager.h"
#include "resources/MeshManager.h"
#include "resources/MeshLoaderHandler.h"
#include "resources/SoundEntityManager.h"
#include "resources/AnimationManager.h"
#include "resources/EntFileManager.h"
#include "resources/ConfigFile.h"
#include "resources/LanguageFile.h"
#include "resources/XmlHelper.h"
#include "resources/XmlDelta.h"
#include "resources/BitmapLoaderHandler.h"
#include "resources/WorldLoaderHandler.h"
#include "resources/BinaryBuffer.h"

#include "resources/WorldLoaderHplMap.h"

#include "system/System.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Platform.h"

#include <tinyxml2.h>

#include <algorithm>
#include <vector>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	bool cResources::mbForceCacheLoadingAndSkipSaving = false;
	bool cResources::mbCreateAndLoadCompressedMaps= false; 
	bool cResources::mbDeltasEnabled = false;
	bool cResources::mbRendererMaskFilterEnabled = false;

	//-----------------------------------------------------------------------

	cResources::cResources(iLowLevelResources *apLowLevelResources)
		: iUpdateable("Resources")
	{
		mpLowLevelResources = apLowLevelResources;

		mpFileSearcher = hplNew( cFileSearcher, () );

		mpDefaultEntityLoader = NULL;
		mpDefaultAreaLoader = NULL;

		mpLanguageFile = NULL;
	}

	//-----------------------------------------------------------------------

	cResources::~cResources()
	{
		Log("Exiting Resources Module\n");
		Log("--------------------------------------------------------\n");

		STLMapDeleteAll(m_mEntityLoaders);
		STLMapDeleteAll(m_mAreaLoaders);

		STLDeleteAll(mlstXmlDocuments);
		STLDeleteAll(mlstBinBuffers);
		
		hplDelete(mpFontManager);
		hplDelete(mpScriptManager);
		hplDelete(mpParticleManager);
		hplDelete(mpSoundManager);
		hplDelete(mpMeshManager);
		hplDelete(mpMaterialManager);
		hplDelete(mpImageManager);
		hplDelete(mpTextureManager);
		hplDelete(mpSoundEntityManager);
		hplDelete(mpAnimationManager);
		hplDelete(mpEntFileManager);
		
		Log(" All resources deleted\n");

		hplDelete(mpFileSearcher);
		
		hplDelete(mpMeshLoaderHandler);
		hplDelete(mpBitmapLoaderHandler);
		hplDelete(mpWorldLoaderHandler);

		if(mpLanguageFile) hplDelete(mpLanguageFile);
		
		mlstManagers.clear();
		Log("--------------------------------------------------------\n\n");
	}

	//-----------------------------------------------------------------------
	
	//////////////////////////////////////////////////////////////////////////
	// MAP DATA VARS OBJECT
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------


	void cResourceVarsObject::LoadVariables(tinyxml2::XMLElement *apRootElem)
	{
		//////////////////////
		//Clear any existing variables
		m_mapVars.clear();

		////////////////////
		//Iterate variables and add them to map.
		for(tinyxml2::XMLElement *pVarElem = apRootElem->FirstChildElement(); pVarElem != NULL; pVarElem = pVarElem->NextSiblingElement())
		{
			tString sName = GetAttributeString(pVarElem, "Name");
			tString sValue = GetAttributeString(pVarElem, "Value");

			m_mapVars.insert(tResourceVarMap::value_type(sName, sValue));
		}
	}

	//-----------------------------------------------------------------------

	void cResourceVarsObject::SetUserVariable(const tString& asName, const tString& asValue)
	{
		m_mapVars[asName] = asValue;
	}

	//-----------------------------------------------------------------------

	tString* cResourceVarsObject::GetUserVariable(const tString& asName)
	{
		tResourceVarMapIt it = m_mapVars.find(asName);
		if(it == m_mapVars.end()) {
			//Warning("Did not find user variable '%s'\n", asName.c_str());
			return NULL;
		}

		return &(it->second);
	}
	//-----------------------------------------------------------------------

	void cResourceVarsObject::AddVarBool(const tString& asName, bool abDefault)
	{
		SetUserVariable(asName, abDefault? "true" : "false");
	}

	void cResourceVarsObject::AddVarInt(const tString& asName, int alDefault)
	{
		SetUserVariable(asName, cString::ToString(alDefault));
	}

	void cResourceVarsObject::AddVarFloat(const tString& asName, float afDefault)
	{
		SetUserVariable(asName, cString::ToString(afDefault,-1,true));
	}

	void cResourceVarsObject::AddVarVector2f(const tString& asName, const cVector2f& avDefault)
	{
		tString sVector = avDefault.ToString();
		sVector = cString::ReplaceCharTo(sVector,":"," ");
		SetUserVariable(asName, sVector);
	}

	void cResourceVarsObject::AddVarVector3f(const tString& asName, const cVector3f& avDefault)
	{
		tString sVector = avDefault.ToString();
		sVector = cString::ReplaceCharTo(sVector,":"," ");
		SetUserVariable(asName, sVector);
	}

	void cResourceVarsObject::AddVarColor(const tString& asName, const cColor& aDefault)
	{
		tString sColor = aDefault.ToString();
		sColor = cString::ReplaceCharTo(sColor,":"," ");
		SetUserVariable(asName, sColor);
	}

	void cResourceVarsObject::AddVarString(const tString& asName, const tString& asDefault)
	{
		SetUserVariable(asName, asDefault);
	}

	//-----------------------------------------------------------------------

	bool cResourceVarsObject::GetVarBool(const tString& asName, bool abDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToBool(pVal ? pVal->c_str() : NULL,abDefault);
	}

	int cResourceVarsObject::GetVarInt(const tString& asName, int alDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToInt(pVal ? pVal->c_str() : NULL,alDefault);
	}

	float cResourceVarsObject::GetVarFloat(const tString& asName, float afDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToFloat(pVal ? pVal->c_str() : NULL,afDefault);
	}

	cVector2f cResourceVarsObject::GetVarVector2f(const tString& asName, const cVector2f& avDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToVector2f(pVal ? pVal->c_str() : NULL,avDefault);
	}

	cVector3f cResourceVarsObject::GetVarVector3f(const tString& asName, const cVector3f& avDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToVector3f(pVal ? pVal->c_str() : NULL,avDefault);
	}

	cColor cResourceVarsObject::GetVarColor(const tString& asName, const cColor& aDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToColor(pVal ? pVal->c_str() : NULL,aDefault);
	}

	tString cResourceVarsObject::GetVarString(const tString& asName, const tString& asDefault)
	{
		tString *pVal = GetUserVariable(asName);
		return cString::ToString(pVal ? pVal->c_str() : NULL,asDefault);
	}

	//-----------------------------------------------------------------------
	

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void cResources::Init(	cGraphics* apGraphics,cSystem *apSystem, cSound* apSound, cScene *apScene,
							cGui *apGui, cPhysics *apPhysics)
	{
		Log("Initializing Resources Module\n");
		Log("--------------------------------------------------------\n");

		mpLowLevelSystem = apSystem->GetLowLevel();

		Log(" Creating loader handlers \n");

		mpMeshLoaderHandler = hplNew( cMeshLoaderHandler,(this, apScene) );
		mpBitmapLoaderHandler = hplNew( cBitmapLoaderHandler,(this, apGraphics) );
		mpWorldLoaderHandler = hplNew( cWorldLoaderHandler,(this, apGraphics,apScene,apPhysics) );

		Log(" Creating resource managers\n");

		mpImageManager = hplNew( cImageManager,(this,mpLowLevelSystem) );
		mlstManagers.push_back(mpImageManager);
		mpParticleManager = hplNew( cParticleManager,(apGraphics, this) );
		mlstManagers.push_back(mpParticleManager);
		mpSoundManager = hplNew( cSoundManager,(apSound, this) );
		mlstManagers.push_back(mpParticleManager);
		mpFontManager = hplNew( cFontManager,(apGraphics,apGui, this) );
		mlstManagers.push_back(mpFontManager);
		mpScriptManager = hplNew( cScriptManager,(apSystem, this) );
		mlstManagers.push_back(mpScriptManager);
		mpTextureManager = hplNew( cTextureManager,(apGraphics, this) );
		mlstManagers.push_back(mpTextureManager);
		mpMaterialManager = hplNew( cMaterialManager,(apGraphics, this) );
		mlstManagers.push_back(mpMaterialManager);
		mpMeshManager = hplNew( cMeshManager,(apGraphics, this) );
		mlstManagers.push_back(mpMeshManager);
		mpSoundEntityManager = hplNew( cSoundEntityManager,(apSound, this) );
		mlstManagers.push_back(mpSoundEntityManager);
		mpAnimationManager = hplNew( cAnimationManager,(apGraphics, this) );
		mlstManagers.push_back(mpAnimationManager);
		mpEntFileManager = hplNew( cEntFileManager,(this) );
		mlstManagers.push_back(mpEntFileManager);

		Log(" Adding loaders to handlers \n");

		//Low level resources will load non-propitary formats.
		mpLowLevelResources->AddBitmapLoaders(mpBitmapLoaderHandler);
		mpLowLevelResources->AddMeshLoaders(mpMeshLoaderHandler);
		
		//Add properitary formats directly
        mpWorldLoaderHandler->AddLoader(hplNew(cWorldLoaderHplMap, () ));		
		
		Log("--------------------------------------------------------\n\n");
	}
	
	//-----------------------------------------------------------------------

	void cResources::Update(float afTimeStep)
	{
		tResourceManagerListIt it = mlstManagers.begin();
		for(; it != mlstManagers.end(); ++it)
		{
			iResourceManager *pManager = *it;

			pManager->Update(afTimeStep);
		}
	}

	//-----------------------------------------------------------------------

	cFileSearcher* cResources::GetFileSearcher()
	{
		return mpFileSearcher;
	}

	//-----------------------------------------------------------------------

	/**
	 * Re-adding a directory is idempotent: cFileSearcher::AddDirectory keeps one
	 * index entry per (filename, path) and raises it to the highest priority seen.
	 * \param &asDir Directory to add.
	 * \param abAddSubDirectories Whether to add subdirectories.
	 * \param &asMask File mask.
	 * \param alPriority Directory priority.
	 * \return true always; no caller checks the result.
	 */
	bool cResources::AddResourceDir(const tWString &asDir, bool abAddSubDirectories, const tString &asMask, int alPriority, const tString &asScope)
	{
		mpFileSearcher->AddDirectory(asDir, asMask, abAddSubDirectories, alPriority, asScope);
		if(iResourceBase::GetLogCreateAndDelete())
			Log(" Added resource directory '%s'\n",cString::To8Char(asDir).c_str());
		return true;
	}

	void cResources::RemoveResourceDirScope(const tString &asScope)
	{
		mpFileSearcher->RemoveDirectoryScope(asScope);
	}

	void cResources::ClearResourceDirs()
	{	
		mpFileSearcher->ClearDirectories();
	}

	//-----------------------------------------------------------------------

	bool cResources::AddLanguageFile(const tString &asFilePath, bool abAddResourceDirs, const tWString &asAltPath, const tString &asResourceScope)
	{
		if(mpLanguageFile == NULL)
		{
			mpLanguageFile = hplNew( cLanguageFile, (this) );
		}

		return mpLanguageFile->AddFromFile(cString::To16Char(asFilePath), abAddResourceDirs, asAltPath, asResourceScope);
	}

	void cResources::ClearTranslations()
	{
		if(mpLanguageFile)
		{
			hplDelete(mpLanguageFile);
			mpLanguageFile = NULL;
		}
	}

	const tWString& cResources::Translate(const tString& asCat, const tString& asName)
	{
		if(mpLanguageFile)
		{
			return mpLanguageFile->Translate(asCat,asName);
		}
		else
		{
			return mwsEmptyString;
		}
	}

	//-----------------------------------------------------------------------

	void cResources::AddEntityLoader(iEntityLoader* apLoader, bool abSetAsDefault)
	{
		m_mEntityLoaders.insert(tEntityLoaderMap::value_type(apLoader->GetName(), apLoader));

		if(abSetAsDefault){
			mpDefaultEntityLoader = apLoader;
		}
	}

	iEntityLoader* cResources::GetEntityLoader(const tString& asName)
	{
		tEntityLoaderMapIt it = m_mEntityLoaders.find(asName);
		if(it == m_mEntityLoaders.end()){
			Warning("No loader for type '%s' found!\n",asName.c_str());
			
			if(mpDefaultEntityLoader){
				Log("Using default loader!\n");
				return mpDefaultEntityLoader;
			}
			else {
				return NULL;
			}
		}

		return it->second;
	}

	//-----------------------------------------------------------------------


	void cResources::AddAreaLoader(iAreaLoader* apLoader, bool abSetAsDefault)
	{
		m_mAreaLoaders.insert(tAreaLoaderMap::value_type(apLoader->GetName(), apLoader));

		if(abSetAsDefault){
			mpDefaultAreaLoader = apLoader;
		}
	}

	iAreaLoader* cResources::GetAreaLoader(const tString& asName)
	{
		tAreaLoaderMapIt it = m_mAreaLoaders.find(asName);
		if(it == m_mAreaLoaders.end()){
			Warning("No loader for area type '%s' found!\n",asName.c_str());

			if(mpDefaultAreaLoader){
				Log("Using default loader!\n");
				return mpDefaultAreaLoader;
			}
			else {
				return NULL;
			}
		}

		return it->second;
	}
	
	//-----------------------------------------------------------------------

	bool cResources::LoadResourceDirsFile(const tString &asFile, const tWString &asAltPath)
	{
		tinyxml2::XMLDocument xmlDoc;
		if(LoadXmlFile(xmlDoc, cString::To16Char(asFile))==false)
		{
			Error("Couldn't load XML file '%s'!\n",asFile.c_str());
			return false;
		}

		//Get the root.
		tinyxml2::XMLElement* pRootElem = xmlDoc.RootElement();
		for(tinyxml2::XMLElement *pChildElem = pRootElem ? pRootElem->FirstChildElement() : NULL; pChildElem != NULL; pChildElem = pChildElem->NextSiblingElement())
		{
			tString sPath = GetAttributeString(pChildElem, "Path");
			if(sPath==""){
				continue;
			}

			bool bAddSubDirs = GetAttributeBool(pChildElem, "AddSubDirs",false);
			//Higher priority shadows lower for files of the same name, whatever
			//order this file lists the dirs in -- same sense as the Priority on
			//an xml delta. Default 0 is what every dir got before, so a config
			//that sets none resolves exactly as it did.
			int lPriority = GetAttributeInt(pChildElem, "Priority", klFileSearchDefaultPriority);

			if(sPath[0]=='/' || sPath[0]=='\\') sPath = cString::Sub(sPath, 1);

			tWString tsPath = cString::To16Char(sPath);
			if (asAltPath.length() > 0) {
				AddResourceDir(asAltPath + tsPath,bAddSubDirs,"*.*",lPriority);
			}
			AddResourceDir(tsPath,bAddSubDirs,"*.*",lPriority);
		}

		return true;
	}

	//-----------------------------------------------------------------------

	namespace {

		// One candidate delta, kept with the sort keys so the whole set can be
		// ordered before any of it is applied.
		class cDeltaCandidate
		{
		public:
			int mlPriority;
			tWString msPath;

			bool operator<(const cDeltaCandidate& aOther) const
			{
				if(mlPriority != aOther.mlPriority) return mlPriority < aOther.mlPriority;
				return msPath < aOther.msPath;
			}
		};
	}

	bool cResources::FindDeltaFiles(const tWString& asBaseFile, const tString& asDeltaExt, tWStringVec& avPaths)
	{
		if(mbDeltasEnabled==false) return false;

		tWString sDeltaName = cString::GetFileNameW(cString::SetFileExtW(asBaseFile, cString::To16Char(asDeltaExt)));

		tWStringVec vCandidatePaths;
		if(mpFileSearcher->GetAllFilePaths(cString::To8Char(sDeltaName), vCandidatePaths)==0) return false;

		////////////////////////////////////
		// Peek at each candidate's root for its Priority
		std::vector<cDeltaCandidate> vCandidates;
		for(size_t i=0; i<vCandidatePaths.size(); ++i)
		{
			tinyxml2::XMLDocument xmlDoc;
			if(LoadXmlFile(xmlDoc, vCandidatePaths[i])==false)
			{
				Error("Could not parse delta file '%s'!\n", cString::To8Char(vCandidatePaths[i]).c_str());
				continue;
			}

			cDeltaCandidate candidate;
			candidate.mlPriority = GetXmlDeltaPriority(xmlDoc.RootElement());
			candidate.msPath = vCandidatePaths[i];
			vCandidates.push_back(candidate);
		}

		std::sort(vCandidates.begin(), vCandidates.end());

		for(size_t i=0; i<vCandidates.size(); ++i)
			avPaths.push_back(vCandidates[i].msPath);

		return avPaths.empty()==false;
	}

	//-----------------------------------------------------------------------

	tinyxml2::XMLElement* cResources::LoadXmlDocument(const tString& asFile)
	{
		tWString sPath = mpFileSearcher->GetFilePath(asFile);
		if(sPath == _W("")){
			Error("Could not load XML document '%s'\n",asFile.c_str());
			return NULL;
		}

		tinyxml2::XMLDocument *pDoc = hplNew( tinyxml2::XMLDocument, () );
		if(LoadXmlFile(*pDoc, sPath)==false || pDoc->RootElement()==NULL)
		{
			Error("Could not parse/load XML from %s\n",cString::To8Char(sPath).c_str());
			hplDelete( pDoc );
			return NULL;
		}

		mlstXmlDocuments.push_back(pDoc);

		return pDoc->RootElement();
	}

	void cResources::DestroyXmlDocument(tinyxml2::XMLElement* apDocRoot)
	{
		if(apDocRoot==NULL) return;
		STLFindAndDelete(mlstXmlDocuments, apDocRoot->GetDocument());
	}

	//-----------------------------------------------------------------------

	cBinaryBuffer* cResources::LoadBinaryBuffer(const tString& asFile)
	{
		tWString sPath = mpFileSearcher->GetFilePath(asFile);
		if(sPath == _W("")){
			Error("Could not find filepath for binary buffer '%s'\n",asFile.c_str());
			return NULL;
		}

		cBinaryBuffer *pFile = hplNew(cBinaryBuffer, (sPath) );
		if(pFile->Load()==false)
		{
			Error("Could not load binary file from %s\n",cString::To8Char(sPath).c_str());
			hplDelete( pFile );
			return NULL;
		}

		mlstBinBuffers.push_back(pFile);

		return pFile;
	}
	
	void cResources::DestroyBinaryBuffer(cBinaryBuffer* apFile)
	{
		STLFindAndDelete(mlstBinBuffers,apFile);
	}

	//-----------------------------------------------------------------------
	
	iLowLevelResources* cResources::GetLowLevel()
	{
		return mpLowLevelResources;
	}

	//-----------------------------------------------------------------------

}
