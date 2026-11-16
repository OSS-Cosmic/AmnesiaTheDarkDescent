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

#include "LuxGlobalDataHandler.h"

#include "LuxMap.h"
#include "LuxMapHandler.h"
#include "LuxPlayer.h"
#include "LuxEnemy.h"


//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxGlobalDataHandler::cLuxGlobalDataHandler() : iLuxUpdateable("LuxGlobalDataHandler")
{
	mbEnemyActivateSoundEnabled = true;
	mbInsanityRingSoundEnabled = true;
	mbTerrorSoundEnabled = true;

	mpScript = {};

	mfLightLampMinSanityIncrease = gpBase->mpGameCfg->GetFloat("Player_Sanity", "LightLampMinSanityIncrease",0);
	mfLightLampMaxSanityIncrease = gpBase->mpGameCfg->GetFloat("Player_Sanity", "LightLampMaxSanityIncrease",0);
}

//-----------------------------------------------------------------------

cLuxGlobalDataHandler::~cLuxGlobalDataHandler()
{
	// mpScript handle auto-releases.
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::LoadAndInitGlobalScript()
{
	LoadScript();

	RunScript("OnGameStart()");
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::OnGameStart()
{
	
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::OnStart()
{
	
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::Reset()
{
	m_mapVars.clear();

	mpScript = {};

	mfEnemyActivateSoundCount =0;
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::Update(float afTimeStep)
{
	if(mfEnemyActivateSoundCount>0)
	{
		mfEnemyActivateSoundCount-=afTimeStep;
	}
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::OnMapEnter(cLuxMap *apMap)
{
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::OnMapLeave(cLuxMap *apMap)
{
}


//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::LoadScript()
{
	/////////////////////
	// Destroy old
	mpScript = {};

	/////////////////////
	// Load script
	tString sFile = gpBase->mpMapHandler->GetMapFolder() + "global.hps";
	mpScript  = gpBase->mpEngine->GetResources()->GetScriptManager()->CreateScript(sFile);
	if(!mpScript)
	{
		Error("Global script '%s' could not be created!\n", sFile.c_str());
	}
}

bool cLuxGlobalDataHandler::RecompileScript(tString *apOutput)
{
	mpScript = {}; // release old script

	tString sFile = gpBase->mpMapHandler->GetMapFolder() + "global.hps";
	mpScript = gpBase->mpEngine->GetResources()->GetScriptManager()->CreateScript(sFile, apOutput);

	return mpScript.IsValid();
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::RunScript(const tString& asCommand)
{
	if(!mpScript) return;

	mpScript->Run(asCommand);
}

//-----------------------------------------------------------------------

cLuxScriptVar* cLuxGlobalDataHandler::GetVar(const tString &asName)
{
	tLuxScriptVarMapIt it = m_mapVars.find(asName);
	if(it != m_mapVars.end()) return &(it->second);

	m_mapVars.insert(tLuxScriptVarMap::value_type(asName, cLuxScriptVar(asName)));
	it = m_mapVars.find(asName);
	return &(it->second);
}

//-----------------------------------------------------------------------

bool cLuxGlobalDataHandler::GetEnemyActivateSoundAllowed()
{
	return mbEnemyActivateSoundEnabled && mfEnemyActivateSoundCount<=0;
}

void cLuxGlobalDataHandler::SetInsanityRingSoundEnabled(bool abX)
{
	mbInsanityRingSoundEnabled = abX;

	// The ear ringing is the insanity collapse sleep loop sound; block it in the sound handler
	tString sSound = gpBase->mpGameCfg->GetString("Player_General", "InsanityCollapse_SleepLoopSound", "");
	sSound = cString::SetFileExt(cString::GetFileName(sSound), "");
	if(sSound == "") return;

	cSoundHandler *pSoundHandler = gpBase->mpEngine->GetSound()->GetSoundHandler();
	if(msBlockedInsanityRingSound != "" && msBlockedInsanityRingSound != sSound)
		pSoundHandler->SetSoundBlocked(msBlockedInsanityRingSound, false);
	pSoundHandler->SetSoundBlocked(sSound, abX==false);

	msBlockedInsanityRingSound = abX ? tString("") : sSound;
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::LoadUserConfig()
{
	mbEnemyActivateSoundEnabled = gpBase->mpUserConfig->GetBool("Sound", "EnemyActivateSound", true);
	SetInsanityRingSoundEnabled(gpBase->mpUserConfig->GetBool("Sound", "InsanityRingSound", true));
	mbTerrorSoundEnabled = gpBase->mpUserConfig->GetBool("Sound", "TerrorSound", true);
}

void cLuxGlobalDataHandler::SaveUserConfig()
{
	gpBase->mpUserConfig->SetBool("Sound", "EnemyActivateSound", mbEnemyActivateSoundEnabled);
	gpBase->mpUserConfig->SetBool("Sound", "InsanityRingSound", mbInsanityRingSoundEnabled);
	gpBase->mpUserConfig->SetBool("Sound", "TerrorSound", mbTerrorSoundEnabled);
}

//-----------------------------------------------------------------------

void cLuxGlobalDataHandler::SetEnemyActivateSoundMade()
{
	mfEnemyActivateSoundCount = 5.0f;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------


//-----------------------------------------------------------------------

