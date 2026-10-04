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

#include "sound/SoundEntityData.h"

#include "system/LowLevelSystem.h"
#include "resources/Resources.h"
#include "system/String.h"
#include "math/Math.h"

#include "sound/Sound.h"
#include "sound/SoundHandler.h"
#include "sound/SoundChannel.h"

#include "resources/Resources.h"
#include "resources/SoundManager.h"
#include "resources/LowLevelResources.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cSoundEntityData::cSoundEntityData(const tString& asName, cResources *apResources, cSound *apSound) : iResourceBase(asName, _W(""),0)
	{
		mpResources = apResources;
		mpSound = apSound;
		
		mbFadeStart = false;
		mbFadeStop = false;

		mfAIVolume = 1;
		mfVolume = 1;
		mfMaxDistance =0;
		mfMinDistance=0;

		mbStream  = false;
		mbLoop = false;
		mbUse3D = true;

		mbKeepPlayingOutOfRange = false;

		mfRandom = 1;
		mfInterval =0;

		for(int i=0; i<3; i++)
		{
			mlPrevious[i] = -1;
		}
	}

	//-----------------------------------------------------------------------

	cSoundEntityData::~cSoundEntityData()
	{

	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	static tString gsEmptyString = "";

	const tString& cSoundEntityData::GetSoundName(eSoundEntityType aType, int alIdx)
	{
		if((int)mvSoundNameVecs[aType].size() <= alIdx) return gsEmptyString;

		return mvSoundNameVecs[aType][alIdx];
	}

	const tString& cSoundEntityData::GetRandomSoundName(eSoundEntityType aType, bool abSkipPrevious)
	{
		if(mvSoundNameVecs[aType].empty()) return gsEmptyString;
		int lSize = (int)mvSoundNameVecs[aType].size();
		if(lSize==1) return mvSoundNameVecs[aType][0];
		
#ifdef AMFP
		// Skipping the previous variant also works with only two variants.
		int lRand = cMath::RandRectl(0, lSize - 1);
		if(abSkipPrevious && lRand == mlPrevious[aType])
		{
			lRand = (lRand + 1) % lSize;
		}
#else
		int lStart = -1;
		int lSizeAdd = -1;
		if(abSkipPrevious && lSize > 2 && mlPrevious[aType] < lSize && mlPrevious[aType] > 0)
		{
			lStart = mlPrevious[aType];
			lSizeAdd = lStart-1;
		}
		
		int lRand = cMath::RandRectl(lStart+1, lSize + lSizeAdd);
		if(lRand >= lSize) lRand = lRand - lSize;
#endif
		
		mlPrevious[aType] = lRand;

		return mvSoundNameVecs[aType][lRand];
	}

	//-----------------------------------------------------------------------

	int cSoundEntityData::GetSoundNum(eSoundEntityType aType)
	{
		return (int) mvSoundNameVecs[aType].size();
	}
	
	//-----------------------------------------------------------------------

	void cSoundEntityData::SetSoundName(const tString& asName, eSoundEntityType aType, int alIdx)
	{
		if((int)mvSoundNameVecs[aType].size() <= alIdx) return;

		mvSoundNameVecs[aType][alIdx] = asName;			
	}

	void cSoundEntityData::AddSoundName(const tString& asName, eSoundEntityType aType)
	{
		mvSoundNameVecs[aType].push_back(asName);			
	}


	//-----------------------------------------------------------------------

	void cSoundEntityData::PreloadSoundsOfType(eSoundEntityType aType)
	{
		for(size_t i=0; i<mvSoundNameVecs[aType].size(); ++i)
		{
			tString& sName = mvSoundNameVecs[aType][i];

			//No need to remove pointer as this is done when creating a channel!
			mpResources->GetSoundManager()->CreateSoundData(sName, false);
		}
	}

	void cSoundEntityData::PreloadSounds()
	{
		if(mbStream) return;
		
		for(int i=0; i<3; ++i) PreloadSoundsOfType( (eSoundEntityType)i );
	}

	//-----------------------------------------------------------------------

	void cSoundEntityData::LoadSoundsInElement(tinyxml2::XMLElement *apElement, tStringVec *apStringVec)
	{
		if(apElement == NULL) return;

		for(tinyxml2::XMLElement *pChild = apElement->FirstChildElement(); pChild != NULL; pChild = pChild->NextSiblingElement())
		{
            apStringVec->push_back(GetAttributeString(pChild, "File"));
		}
	}

	bool cSoundEntityData::CreateFromFile(const tWString &asFile)
	{
		SetFullPath(asFile);

		tinyxml2::XMLDocument xmlDoc;
		if(hpl::LoadXmlFile(xmlDoc, asFile)==false || xmlDoc.RootElement()==NULL)
		{
			Error("Couldn't load sound entity data '%s'!\n",cString::To8Char(asFile).c_str());
			return false;
		}
		tinyxml2::XMLElement *pDoc = xmlDoc.RootElement();

		////////////////////////////////////////////////
		// SOUNDS
		tinyxml2::XMLElement *pSoundsElem = pDoc->FirstChildElement("SOUNDS");
		if(pSoundsElem==NULL){
			Error("Couldn't find SOUNDS element in '%s'!\n",asFile.c_str());
			return false;
		}

		LoadSoundsInElement(pSoundsElem->FirstChildElement("Main"), &mvSoundNameVecs[eSoundEntityType_Main]);
		LoadSoundsInElement(pSoundsElem->FirstChildElement("Start"), &mvSoundNameVecs[eSoundEntityType_Start]);
		LoadSoundsInElement(pSoundsElem->FirstChildElement("Stop"), &mvSoundNameVecs[eSoundEntityType_Stop]);


		////////////////////////////////////////////////
		// PROPERTIES
		tinyxml2::XMLElement *pPropElem = pDoc->FirstChildElement("PROPERTIES");
		if(pPropElem==NULL){
			Error("Couldn't find PROPERTIES element in '%s'!\n",asFile.c_str());
			return false;
		}

		mbUse3D = GetAttributeBool(pPropElem, "Use3D",true);
		mbLoop = GetAttributeBool(pPropElem, "Loop",true);
		mbStream = GetAttributeBool(pPropElem, "Stream",true);

		mbBlockable = GetAttributeBool(pPropElem, "Blockable",false);
		mfBlockVolumeMul = GetAttributeFloat(pPropElem, "BlockVolumeMul",0.6f);

		mfVolume = GetAttributeFloat(pPropElem, "Volume",1);
		mfMaxDistance = GetAttributeFloat(pPropElem, "MaxDistance",1);
		mfMinDistance = GetAttributeFloat(pPropElem, "MinDistance",1);

		// Loudness enemies hear; defaults to the played volume.
		mfAIVolume = GetAttributeFloat(pPropElem, "AIVolume", mfVolume);

		mbFadeStart = GetAttributeBool(pPropElem, "FadeStart",true);
		mbFadeStop = GetAttributeBool(pPropElem, "FadeStop",true);

		mfRandom = GetAttributeFloat(pPropElem, "Random",1);
		mfInterval = GetAttributeFloat(pPropElem, "Interval",0);

		mlPriority = GetAttributeInt(pPropElem, "Priority",0);

		mbKeepPlayingOutOfRange = GetAttributeBool(pPropElem, "KeepPlayingOutOfRange",false);

        return true;
	}

	
	//-----------------------------------------------------------------------

}
