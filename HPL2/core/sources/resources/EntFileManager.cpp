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

#include "resources/EntFileManager.h"

#include "system/String.h"
#include "system/LowLevelSystem.h"
#include "resources/Resources.h"
#include "resources/LowLevelResources.h"
#include "resources/XmlHelper.h"
#include "resources/XmlDelta.h"

#include <tinyxml2.h>


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// ENT FILE
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cEntFile::cEntFile(const tString& asName, const tWString& asFullPath, cResources *apResources) : iResourceBase(asName, asFullPath, 0)
	{
		mpXmlDoc = hplNew( tinyxml2::XMLDocument, () );
		mpResources = apResources;
	}
	cEntFile::~cEntFile()
	{
		hplDelete(mpXmlDoc);
	}

	bool cEntFile::CreateFromFile()
	{
		if(LoadXmlFile(*mpXmlDoc, GetFullPath())==false) return false;

		////////////////////////////////////
		// Apply any .ent_delta overlays to the parsed tree. Every placed instance
		// of this entity shares this document, so a patched definition reaches all
		// of them; the .ent on disk is untouched.
		tWStringVec vDeltaFiles;
		if(mpResources->FindDeltaFiles(GetFullPath(), "ent_delta", vDeltaFiles)==false) return true;

		int lNextAddID = HPL_XML_DELTA_FIRST_ADD_ID;
		for(size_t i=0; i<vDeltaFiles.size(); ++i)
		{
			tinyxml2::XMLDocument deltaDoc;
			if(LoadXmlFile(deltaDoc, vDeltaFiles[i])==false)
			{
				Error("Could not load ent delta '%s'!\n", cString::To8Char(vDeltaFiles[i]).c_str());
				continue;
			}

			cXmlDeltaStats stats;
			if(ApplyXmlDelta(mpXmlDoc->RootElement(), deltaDoc.RootElement(), lNextAddID, stats)==false)
			{
				Error("Could not apply ent delta '%s'!\n", cString::To8Char(vDeltaFiles[i]).c_str());
				continue;
			}

			Log("  Ent delta '%s' on '%s': %d added, %d modified, %d removed, %d skipped\n",
				cString::To8Char(cString::GetFileNameW(vDeltaFiles[i])).c_str(), GetName().c_str(),
				stats.mlAdded, stats.mlModified, stats.mlRemoved, stats.mlSkipped);
		}

		return true;
	}

	tinyxml2::XMLElement* cEntFile::GetXmlDoc()
	{
		return mpXmlDoc->RootElement();
	}

	//-----------------------------------------------------------------------


	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cEntFileManager::cEntFileManager(cResources *apResources)
		: iResourceManager(apResources->GetFileSearcher(), apResources->GetLowLevel(), apResources->GetLowLevelSystem(), true)
	{
		mpResources = apResources;
	}

	cEntFileManager::~cEntFileManager()
	{
		DestroyAll();
		Log(" Done with ent files\n");
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	SharedResourceHandle<cEntFile> cEntFileManager::CreateEntFile(const tString& asName)
	{
		tWString sPath;
		cEntFile* pEntFile;
		tString asNewName = cString::SetFileExt(cString::ToLowerCase(asName), "ent");

		BeginLoad(asName);
		
		pEntFile = static_cast<cEntFile*>(this->FindLoadedResource(asNewName,sPath));

		if(pEntFile==NULL && sPath!=_W(""))
		{
			pEntFile = hplNew(cEntFile, (asNewName, sPath.c_str(), mpResources));
			if(pEntFile->CreateFromFile()==false)
			{
				Error("Couldn't load entity file '%s'!\n",cString::To8Char(sPath).c_str());
				hplDelete(pEntFile);

				EndLoad();
				return {};
			}

			AddResource(pEntFile);
		}

		if(pEntFile==NULL)
			Error("Couldn't create ent file '%s'\n",asNewName.c_str());

		EndLoad();
		return AcquireResource<cEntFile>(this, pEntFile); // handle takes the reference (empty if null)
	}

	//-----------------------------------------------------------------------

	void cEntFileManager::Unload(iResourceBase* apResource)
	{

	}
	//-----------------------------------------------------------------------

	//-----------------------------------------------------------------------

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------


	//-----------------------------------------------------------------------
}
