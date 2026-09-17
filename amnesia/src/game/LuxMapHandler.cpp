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

#include "LuxMapHandler.h"

#include "Constants.h"   // kSceneExposure

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include "LuxMap.h"
#include "LuxPlayer.h"
#include "LuxEffectRenderer.h"
#include "LuxEffectHandler.h"
#include "LuxDebugHandler.h"
#include "LuxHelpFuncs.h"
#include "LuxSavedGame.h"
#include "LuxSaveHandler.h"
#include "LuxConfigHandler.h"
#include "LuxLoadScreenHandler.h"
#include "LuxMainMenu.h"
#include "LuxPostEffects.h"

#include "LuxEnemy.h"
#include "LuxAchievementHandler.h"

//////////////////////////////////////////////////////////////////////////
// SOUND ENTITY CALLBACK
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMapHandlerSoundCallback::cMapHandlerSoundCallback()
{
	///////////////////////
	//Load document
	tString sFile = "sounds/EnemySounds.dat";
	tinyxml2::XMLElement* pXmlDoc = gpBase->mpEngine->GetResources()->LoadXmlDocument(sFile);
	if(pXmlDoc ==NULL)
	{
		Error("Couldn't load XML file '%s'!\n",sFile.c_str());
		return;
	}

	//////////////////////
	// Load data
	for(tinyxml2::XMLElement* pChildElem = pXmlDoc->FirstChildElement(); pChildElem != NULL; pChildElem = pChildElem->NextSiblingElement())
	{
		tString sName = hpl::GetAttributeString(pChildElem, "name");
		mvEnemyHearableSounds.push_back(sName);
	}

	gpBase->mpEngine->GetResources()->DestroyXmlDocument( pXmlDoc );
}

//-----------------------------------------------------------------------

void cMapHandlerSoundCallback::OnStart(cSoundEntity *apSoundEntity)
{
	cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
	if(pMap==NULL) return;
	
	///////////////////////////
	//Check if the sound is something to worry bout
	tString sTypeName = apSoundEntity->GetData()->GetName();

	bool bUsed=false;
	for(size_t i=0; i< mvEnemyHearableSounds.size(); ++i)
	{
		tString &sName = mvEnemyHearableSounds[i];
		if(sTypeName.size() >= sName.size() && sName == sTypeName.substr(0,sName.size()))
		{
			bUsed = true;
			break;
		}
	}
	if(bUsed == false) return;
	
	///////////////////////////
	//Iterate enemies and send sound message to those close enough
	float fMaxDist = apSoundEntity->GetMaxDistance();
	float fMinDist = apSoundEntity->GetMaxDistance();
	float fVolume = apSoundEntity->GetVolume();
	cVector3f vPos = apSoundEntity->GetWorldPosition();
	
	pMap->BroadcastEnemySoundMessage(vPos, fVolume, fMinDist, fMaxDist);
}	

//-----------------------------------------------------------------------


//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxMapHandler::cLuxMapHandler() : iLuxUpdateable("LuxMapHandler")
{
	//////////////////////////
	//Create and setup view port
	mpViewport = gpBase->mpEngine->GetScene()->CreateViewport();
	gpBase->mpEngine->GetScene()->SetCurrentListener(mpViewport);

	//////////////////////////
	//Set up post effects
	cGraphics *pGraphics = gpBase->mpEngine->GetGraphics();
	cPostEffectComposite *pPostEffectComp = pGraphics->CreatePostEffectComposite();
	mpViewport->SetPostEffectComposite(pPostEffectComp);

	//Bloom
	cPostEffectParams_Bloom bloomParams;
	bloomParams.mfBlurSize = 1.0f;
	bloomParams.mvRgbToIntensity = bloomParams.mvRgbToIntensity * 1.0f;
	mpPostEffect_Bloom = pGraphics->CreatePostEffect(&bloomParams);
	pPostEffectComp->AddPostEffect(mpPostEffect_Bloom, 100);

	//Tonemap — sRGB encode (+exposure). Runs after every HDR-domain effect
	//(the multimap orders highest-priority first, so 0 places it below them).
	//Maps linear HDR -> normalized [0,1] sRGB-encoded display, matching the GUI
	//which writes display-encoded values to the same UNORM swapchain. It carries
	//the mandatory output encode, so it must always run — no config toggle.
	//The legacy color lookup, image trail and menu backdrop run after it
	//(negative priorities), in display space.
	cPostEffectParams_ToneMap tonemapParams;
	tonemapParams.mfExposure = kSceneExposure;
	tonemapParams.mfShadowLift = 1.0f;
	tonemapParams.mfGamma = gpBase->mpConfigHandler->GetGamma();
	mpPostEffect_ToneMap = pGraphics->CreatePostEffect(&tonemapParams);
	pPostEffectComp->AddPostEffect(mpPostEffect_ToneMap, 0);

	//Image trail — runs AFTER tonemap (negative priority), on display-encoded
	//[0,1] values like the legacy LDR game. Its accumulator is a persistent
	//fp16 mix of the previous frames: feeding it unbounded linear HDR let
	//bright hotspots accumulate past fp16 range (black NaN squares trailing
	//across the screen); the tonemapped range is bounded so it can't overflow.
	cPostEffectParams_ImageTrail imageTrailParams;
	mpPostEffect_ImageTrail = pGraphics->CreatePostEffect(&imageTrailParams);
	pPostEffectComp->AddPostEffect(mpPostEffect_ImageTrail, -10);
	mpPostEffect_ImageTrail->SetActive(false);

	//Radial
	cPostEffectParams_RadialBlur radialBlurParams;
	radialBlurParams.mfSize = 0.0f;
	mpPostEffect_RadialBlur = pGraphics->CreatePostEffect(&radialBlurParams);
	pPostEffectComp->AddPostEffect(mpPostEffect_RadialBlur, 9);
	mpPostEffect_RadialBlur->SetActive(false);

	//Sepia — the legacy 256x1 LUT maps display colors, not scene radiance.
	//Applying it before exposure/tonemap collapses dark HDR values onto the
	//first few brown LUT entries and then exposes that raised black level.
	cPostEffectParams_ColorConvTex sepiaParams;
	sepiaParams.msTextureFile = "colorconv_sepia.tga";
	sepiaParams.mfFadeAlpha = 0.0f;
	mpPostEffect_Sepia = pGraphics->CreatePostEffect(&sepiaParams);
	pPostEffectComp->AddPostEffect(mpPostEffect_Sepia, -1);
	mpPostEffect_Sepia->SetActive(false);

	//Menu backdrop — live blur/desaturate behind the inventory/journal/escape
	//menu. Negative priority so it runs AFTER tonemap, in display-encoded [0,1]
	//space (matching what the old snapshot captured: the final composited image).
	//Run after the image trail too, so opening a menu cannot feed its blur
	//back into the gameplay history.
	//Game-owned (not a registered cGraphics type), so we hplDelete it ourselves.
	mpPostEffect_MenuBackdrop = hplNew(cLuxPostEffect_MenuBackdrop, (pGraphics, gpBase->mpEngine->GetResources()));
	pPostEffectComp->AddPostEffect(mpPostEffect_MenuBackdrop, -20);
	mpPostEffect_MenuBackdrop->SetActive(false);

	//////////////////////////
	//Saving
	mpSavedGame = hplNew( cLuxSavedGameMapCollection, () );

	
	//////////////////////////
	//Callbacks
	mpSoundCallback = hplNew( cMapHandlerSoundCallback, () );
	cSoundEntity::AddGlobalCallback(mpSoundCallback);

	//////////////////////////
	//Threading
	mpSavedGameMutex = cPlatform::CreateMutEx();

	//////////////////////////
	//Variables
	mbPausedSoundsAndMusic = false;
	mpDataCache =NULL;

	Reset();
}

//-----------------------------------------------------------------------

cLuxMapHandler::~cLuxMapHandler()
{
	hplDelete(mpSavedGame);
	hplDelete(mpSavedGameMutex);
	hplDelete(mpSoundCallback);
	hplDelete(mpPostEffect_MenuBackdrop);
	STLDeleteAll(mlstMaps);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxMapHandler::OnStart()
{
	//////////////////////
	//Set up viewport 	
	mpViewport->SetCamera(gpBase->mpPlayer->GetCamera());

	mpViewport->AddGuiSet(gpBase->mpGameDebugSet);
	mpViewport->AddGuiSet(gpBase->mpGameHudSet);

	// RI path: enqueue all game debug overlays into the global DebugDraw
	// batcher right before the renderer Draw — the Hybrid renderer never
	// runs iRendererCallback messages.
	mPreWorldDrawHandler = EventHandler<const WorldDrawCtx&>([this](const WorldDrawCtx&){ OnPreWorldDraw(); });
	mPreWorldDrawHandler.Connect(mpViewport->OnPreWorldDraw());

	// cLuxEffectRenderer composites the hover outline / pickup flash / enemy
	// glow over the final lit image via its own OnPostWorldDraw handler.
	gpBase->mpEffectRenderer->AttachViewport(mpViewport);

	UpdateViewportRenderProperties();
}

//-----------------------------------------------------------------------

void cLuxMapHandler::OnPreWorldDraw()
{
	DebugDraw* pDebugDraw = gpBase->mpEngine->GetGraphics()->GetDebugDraw();
	if(pDebugDraw==NULL) return;

	gpBase->mpDebugHandler->RenderSolid(pDebugDraw);
	RenderSolid(pDebugDraw);
	gpBase->mpPlayer->RenderSolid(pDebugDraw);
	// NOTE: cLuxEffectRenderer (flash/glow/outline) is a real translucent
	// effects pass, not debug shapes — ported separately.
}

//-----------------------------------------------------------------------

void cLuxMapHandler::UpdateViewportRenderProperties()
{
	cRenderSettings *pRenderSettings = mpViewport->GetRenderSettings();
	pRenderSettings->mbRenderWorldReflection = gpBase->mpConfigHandler->mbWorldReflection;
	pRenderSettings->mbRenderShadows = gpBase->mpConfigHandler->mbShadowsActive;
	pRenderSettings->mbSSAOActive = gpBase->mpConfigHandler->mbSSAOActive;
	// The viewport resolves provider availability itself; unavailable providers fall back to native extent and are reported via cViewport::GetTemporalUpscalerStatus().
	mpViewport->SetTemporalUpscalerSettings(gpBase->mpConfigHandler->mSuperSampling);

}

//-----------------------------------------------------------------------

void cLuxMapHandler::RefreshToneMapGamma()
{
	if(mpPostEffect_ToneMap == NULL) return;

	// Rebuild with the constants used at creation (exposure/shadowLift = 1) plus
	// the current config gamma, then re-apply.
	cPostEffectParams_ToneMap tonemapParams;
	tonemapParams.mfExposure = kSceneExposure;
	tonemapParams.mfShadowLift = 1.0f;
	tonemapParams.mfGamma = gpBase->mpConfigHandler->GetGamma();
	mpPostEffect_ToneMap->SetParams(&tonemapParams);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::Update(float afTimeStep)
{
	//TODO: Bad placement! Moooove!
	gpBase->mpEffectRenderer->ClearRenderLists();

	// The renderer backend is applied at a frame boundary inside cGraphics, so
	// notice it here rather than where the option was changed: entities
	// authored for the other renderer go inert, or come back.
	const eRendererBackend currentBackend =
		gpBase->mpEngine->GetGraphics()->GetRendererBackend();
	if(currentBackend != mLastRendererBackend)
	{
		mLastRendererBackend = currentBackend;
		if(mpCurrentMap)
			mpCurrentMap->UpdateBackendDormancy();
	}

	CheckMapChange(afTimeStep);

	if(mpCurrentMap && mMapChangeData.mbActive==false)
		mpCurrentMap->Update(afTimeStep);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::Reset()
{
	// Stop all sounds (deleting maps will stop world entries, but will let GUI ones live)
	cSound *pSound = gpBase->mpEngine->GetSound();
	pSound->GetSoundHandler()->StopAll(eSoundEntryType_All);

	STLDeleteAll(mlstMaps);
	mpCurrentMap = NULL;

	ResumeSoundsAndMusic(); //Make sure that all sounds and music are resumed!

	msMapFolder = "";

	mbUpdateActive = true;

	mMapChangeData.mbActive = false;

	mpSavedGame->Reset();

	gpBase->mpHelpFuncs->CleanupData();

	DestroyDataCache();
}

//-----------------------------------------------------------------------

void cLuxMapHandler::OnQuit()
{
    gpBase->mpEngine->GetUpdater()->SetContainer("MainMenu");

    gpBase->mpLoadScreenHandler->DrawMenuScreen();
    
    //Destroy map
    cLuxMapHandler *mpMapHandler = gpBase->mpMapHandler;
    if(mpMapHandler->GetCurrentMap())
    {
        //Save
        gpBase->mpSaveHandler->AutoSave();
        
        mpMapHandler->DestroyMap(mpMapHandler->GetCurrentMap(),false);

        //Reset game
        gpBase->mpEngine->GetUpdater()->BroadcastMessageToAll(eUpdateableMessage_Reset);
        gpBase->SetCustomStory(NULL);
    }

    //Start up menu again
    gpBase->mpMainMenu->OnLeaveContainer("");
    gpBase->mpMainMenu->OnEnterContainer("");
}


//-----------------------------------------------------------------------

void cLuxMapHandler::LoadUserConfig()
{
	mbShowCommentary = gpBase->mpUserConfig->GetBool("Game","ShowCommentary", false);
}

void cLuxMapHandler::SaveUserConfig()
{
	gpBase->mpUserConfig->SetBool("Game","ShowCommentary", mbShowCommentary);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::CreateDataCache()
{
	if(mpDataCache) DestroyDataCache();
	
	mpDataCache = hplNew(cLuxModelCache, () );
	mpDataCache->Create();
}

void cLuxMapHandler::DestroyDataCache()
{
	if(mpDataCache==NULL) return;
	hplDelete(mpDataCache);
	mpDataCache = NULL;
}

//-----------------------------------------------------------------------

void cLuxMapHandler::SetUpdateActive(bool abX)
{
	mbUpdateActive = abX;
	
	if(mpCurrentMap) mpCurrentMap->GetWorld()->SetActive(mbUpdateActive);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::RenderSolid(DebugDraw* apDebugDraw)
{
	//mpViewport->GetRenderSettings()->mbLog = false;
	if(mpCurrentMap) mpCurrentMap->OnRenderSolid(apDebugDraw);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::OnEnterContainer(const tString& asOldContainer)
{
	mpViewport->SetActive(true);
	mpViewport->SetVisible(true);

	// Back in gameplay: restore the HUD and resume world simulation. The live
	// menu backdrop is disabled by the closing menu state itself.
	if(gpBase->mpGameHudSet)   gpBase->mpGameHudSet->SetActive(true);
	if(gpBase->mpGameDebugSet) gpBase->mpGameDebugSet->SetActive(true);

	if(mpCurrentMap) mpCurrentMap->GetWorld()->SetActive(true);

	ResumeSoundsAndMusic();
}

void cLuxMapHandler::OnLeaveContainer(const tString& asNewContainer)
{
	mpViewport->SetActive(false); // input/listener semantics; no-op for rendering

	// Pause the world simulation whenever we leave gameplay (as the original did).
	if(mpCurrentMap) mpCurrentMap->GetWorld()->SetActive(false);

	// Keep the gameplay viewport VISIBLE (re-rendering the paused world) only
	// behind the in-game overlay menus — inventory / journal / escape menu — which
	// now show a live blurred/desaturated backdrop via the menu-backdrop post-effect
	// instead of a frozen snapshot. Suppress the gameplay HUD there so it doesn't
	// draw over the menu. For any other destination (load screen, credits, demo end)
	// or when no map is loaded (the initial "Default" the engine sets before the
	// pre-menu), hide the viewport exactly as before so the destination's own
	// viewport is the sole swapchain-compositing one.
	const bool bOverlayMenu = mpCurrentMap!=NULL &&
		(asNewContainer=="Inventory" || asNewContainer=="Journal" || asNewContainer=="MainMenu");

	if(bOverlayMenu)
	{
		if(gpBase->mpGameHudSet)   gpBase->mpGameHudSet->SetActive(false);
		if(gpBase->mpGameDebugSet) gpBase->mpGameDebugSet->SetActive(false);
	}
	else
	{
		mpViewport->SetVisible(false);
	}
}

//-----------------------------------------------------------------------

void cLuxMapHandler::EnableBackdrop(eLuxMenuBackdrop aType)
{
	if(mpPostEffect_MenuBackdrop==NULL) return;

	mpPostEffect_MenuBackdrop->SetMode(aType==eLuxMenuBackdrop_Blur
		? cLuxPostEffect_MenuBackdrop::Mode::Blur
		: cLuxPostEffect_MenuBackdrop::Mode::DesaturateDarken);
	mpPostEffect_MenuBackdrop->SetStrength(0.0f);
	mpPostEffect_MenuBackdrop->SetActive(true);
}

void cLuxMapHandler::DisableBackdrop()
{
	if(mpPostEffect_MenuBackdrop==NULL) return;
	mpPostEffect_MenuBackdrop->SetActive(false);
}

void cLuxMapHandler::SetBackdropStrength(float afX)
{
	if(mpPostEffect_MenuBackdrop==NULL) return;
	mpPostEffect_MenuBackdrop->SetStrength(afX);
}


//-----------------------------------------------------------------------

void cLuxMapHandler::ChangeMap(const tString& asMapName, const tString& asStartPos, const tString& asStartSound, const tString& asEndSound)
{
	mMapChangeData.mbActive = true;
	mMapChangeData.msMapFile = cString::SetFileExt(asMapName, "map");
	mMapChangeData.msStartPos = asStartPos;
    mMapChangeData.msSound = asEndSound;

    gpBase->mpHelpFuncs->PlayGuiSoundData(asStartSound, eSoundEntryType_Gui);

	gpBase->mpEffectHandler->GetFade()->FadeOut(1.5f);

	gpBase->mpPlayer->SetActive(false);
}

//-----------------------------------------------------------------------

cLuxMap* cLuxMapHandler::LoadMap(const tString& asFileName, bool abLoadEntities)
{
	cLuxMap *pMap = hplNew( cLuxMap, ( FileToMapName(asFileName)) );
	
	pMap->LoadFromFile(msMapFolder+asFileName, abLoadEntities);

	mlstMaps.push_back(pMap);

	return pMap;
}
//-----------------------------------------------------------------------

void cLuxMapHandler::DestroyMap(cLuxMap* apMap, bool abLoadingSaveGame)
{
	////////////////////////////////
	//If the map do me destroyed is current, make sure it is not current
	if(mpCurrentMap == apMap) SetCurrentMap(NULL, abLoadingSaveGame, false,"");

    STLFindAndDelete(mlstMaps,apMap);
}

//-----------------------------------------------------------------------

void cLuxMapHandler::SetCurrentMap(cLuxMap* apMap, bool abRunScript, bool abFirstTime, const tString& asPlayerPos)
{
	if(mpCurrentMap == apMap) return;

	//////////////////////////////////
	//Unload stuff from previous map
    if(mpCurrentMap)
	{
		mpCurrentMap->OnLeave(abRunScript);
		
		//Leave callback for modules
		gpBase->RunModuleMessage(eLuxUpdateableMessage_DestroyWorldEntities, mpCurrentMap);
		gpBase->RunModuleMessage(eLuxUpdateableMessage_OnMapLeave, mpCurrentMap);
	}

	mpCurrentMap = apMap;

	//////////////////////////////////
	//Setup stuff for previous map
	if(mpCurrentMap)
	{
		//Enter callback for modules
		gpBase->RunModuleMessage(eLuxUpdateableMessage_CreateWorldEntities, mpCurrentMap);
		gpBase->RunModuleMessage(eLuxUpdateableMessage_OnMapEnter, mpCurrentMap);

		//Set the player position
		mpCurrentMap->PlacePlayerAtStartPos(asPlayerPos);

		//Create an automatic checkpoint
		mpCurrentMap->SetCheckPoint("_auto", asPlayerPos, "");
		
		//Map enter callback
		mpCurrentMap->OnEnter(abRunScript, abFirstTime);

		//Set this as world in viewport
		mpViewport->SetWorld(mpCurrentMap->GetWorld());
	}
	else
	{
		//If no map, set NULL as world
		mpViewport->SetWorld(NULL);
	}
}

//-----------------------------------------------------------------------

void cLuxMapHandler::PauseSoundsAndMusic()
{
	if(mbPausedSoundsAndMusic) return;

	cSound *pSound = gpBase->mpEngine->GetSound();
	pSound->GetSoundHandler()->PauseAll(eSoundEntryType_All);
	pSound->GetMusicHandler()->Pause();
	mbPausedSoundsAndMusic = true;
}

void cLuxMapHandler::ResumeSoundsAndMusic()
{
	if(mbPausedSoundsAndMusic)
	{
		cSound *pSound = gpBase->mpEngine->GetSound();
		pSound->GetSoundHandler()->ResumeAll(eSoundEntryType_All);
		pSound->GetMusicHandler()->Resume();
		mbPausedSoundsAndMusic = false;
	}
}

//-----------------------------------------------------------------------

void cLuxMapHandler::ClearSaveMapCollection()
{
	mpSavedGameMutex->Lock();
		mpSavedGame->Reset();
	mpSavedGameMutex->Unlock();
}

//-----------------------------------------------------------------------

void cLuxMapHandler::SetSavedMapCollection(cLuxSavedGameMapCollection *apMaps)
{
	mpSavedGameMutex->Lock();
	{
		hplDelete(mpSavedGame);

		mpSavedGame = apMaps;
	}
	mpSavedGameMutex->Unlock();
}

//-----------------------------------------------------------------------

void cLuxMapHandler::AppLostInputFocus()
{
	PauseSoundsAndMusic();
}

//-----------------------------------------------------------------------

void cLuxMapHandler::AppGotInputFocus()
{
	ResumeSoundsAndMusic();
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxMapHandler::LoadMainConfig()
{
	mpPostEffect_Bloom->SetDisabled(gpBase->mpMainConfig->GetBool("Graphics", "PostEffectBloom", true) == false);
	mpPostEffect_ImageTrail->SetDisabled(gpBase->mpMainConfig->GetBool("Graphics", "PostEffectImageTrail", true)==false);
	mpPostEffect_Sepia->SetDisabled(gpBase->mpMainConfig->GetBool("Graphics", "PostEffectSepia", true)==false);
	mpPostEffect_RadialBlur->SetDisabled(gpBase->mpMainConfig->GetBool("Graphics", "PostEffectRadialBlur", true)==false);

	cRenderSettings *pRenderSettings = mpViewport->GetRenderSettings();
	pRenderSettings->mbUseEdgeSmooth = gpBase->mpConfigHandler->mbEdgeSmooth; //This is saved in config handler!
}

void cLuxMapHandler::SaveMainConfig()
{
	/* gpBase->mpMainConfig->SetBool("Graphics", "PostEffectBloom", mpPostEffect_Bloom->IsDisabled() == false);
	gpBase->mpMainConfig->SetBool("Graphics", "PostEffectImageTrail", mpPostEffect_ImageTrail->IsDisabled()==false);
	gpBase->mpMainConfig->SetBool("Graphics", "PostEffectSepia", mpPostEffect_Sepia->IsDisabled()==false);
	gpBase->mpMainConfig->SetBool("Graphics", "PostEffectRadialBlur", mpPostEffect_RadialBlur->IsDisabled()==false); */
}

//-----------------------------------------------------------------------

tString cLuxMapHandler::FileToMapName(const tString& asFile)
{
	return cString::ToLowerCase(cString::GetFileName(cString::SetFileExt(asFile, "")));
}

//-----------------------------------------------------------------------

void cLuxMapHandler::SetShowCommentary(bool abX)
{
	mbShowCommentary = abX;

	if(mbShowCommentary==false)
	{
		gpBase->mpEffectHandler->GetPlayCommentary()->Stop();
	}
}

//-----------------------------------------------------------------------

void cLuxMapHandler::CheckMapChange(float afTimeStep)
{
	if(mMapChangeData.mbActive==false) return;
	if(gpBase->mpEffectHandler->GetFade()->IsFading()) return;

	///////////////////////////////////////
	// Setup variables
    float fTimeTaken =0;

	///////////////////////////////////////
	// Fade out and disable player
	mMapChangeData.mbActive = false;
	gpBase->mpEffectHandler->GetFade()->FadeIn(2.0f);
	gpBase->mpPlayer->SetActive(true);

	///////////////////////////////////////
	// Write pending savegame queries
	cLuxSaveHandlerThreadClass* pThreadClass = gpBase->mpSaveHandler->GetThreadClass();
	if(pThreadClass->IsRunning())
		pThreadClass->ProcessPendingSaves();

	//////////////////////////////////
	//Clean up
	// Must do this before OnEnter!
	gpBase->mpHelpFuncs->CleanupData();

	///////////////////////
	// Load map
	tString sNewMapName = FileToMapName(mMapChangeData.msMapFile);
	if(mpCurrentMap->GetName() != sNewMapName)
	{
		mpSavedGameMutex->Lock();

		//////////////////////
		// Run onleave before saving!
		mpCurrentMap->RunScript("OnLeave()");//since script is not run in SetCurrenMap

		///////////////////////////////////////
		// Draw loading screen
		unsigned long lLoadStartTime = cPlatform::GetApplicationTime();
		gpBase->mpLoadScreenHandler->DrawGameScreen();

		//////////////////////
		// Save old map
		mpSavedGame->SaveMap(mpCurrentMap);

		//////////////////////
		// Fadeout sounds and disable stop (meaning they will fade even when sound entities are destroyed)
		gpBase->mpEngine->GetSound()->GetSoundHandler()->FadeOutAll(eSoundEntryType_World, 0.5f, true);
		
		//////////////////////
		// Load new map
		cLuxMap *pLastMap = mpCurrentMap;
		cLuxMap *pMap = LoadMap(mMapChangeData.msMapFile,true);
		if(pMap == NULL)
		{
			Error("Could not load map '%s'!\n", mMapChangeData.msMapFile.c_str());
			return;
		}
		
		if(pLastMap)
		{
			if (pLastMap->GetName() == "08_cellar_maze" && pMap->GetName() == "09_back_hall")
			{
				gpBase->mpAchievementHandler->UnlockAchievement(eLuxAchievement_EscapeArtist);
			}

			if (pLastMap->GetName() == "14_elevator" && pMap->GetName() == "15_prison_south")
			{
				gpBase->mpAchievementHandler->UnlockAchievement(eLuxAchievement_Descendant);
			}

			//////////////
			// HARDMODE
			if (gpBase->mbHardMode &&
				pLastMap->GetName() == "27_torture_chancel_redux" && pMap->GetName() == "28_inner_sanctum")
			{
				gpBase->mpPlayer->AddSanity(100.f, true);
			}
		}

		//////////////////////
		// Set new and destroy old
		bool bFirstTime = mpSavedGame->MapExists(sNewMapName)==false;	
		
		SetCurrentMap(pMap, false, bFirstTime, mMapChangeData.msStartPos);
		DestroyMap(pLastMap, false);

		//////////////////////
		// Load new map data
		mpSavedGame->LoadMap(mpCurrentMap);

		//////////////////////
		// Run enter script! (otherwise a save in oneter will not be correct!)
		if(bFirstTime) mpCurrentMap->RunScript("OnStart()");
		mpCurrentMap->RunScript("OnEnter()");


		mpSavedGameMutex->Unlock();

		//////////////////////////////////
		//Check if any more load time needed
		fTimeTaken = (float)(cPlatform::GetApplicationTime() - lLoadStartTime)/1000.0f;
		
		ProgLog(eLuxProgressLogLevel_High, "Entering map "+ mpCurrentMap->GetName());
	}
	///////////////////////
	// Map already loaded.
	else
	{
		mpCurrentMap->PlacePlayerAtStartPos(mMapChangeData.msStartPos);
	}

	//////////////////////////////////
	// Check if text should be left on a bit longer
	if(fTimeTaken>0)
	{
		gpBase->mpLoadScreenHandler->GameScreenLoadDone(mMapChangeData.msSound, fTimeTaken);
	}
	else
	{
		//Play finished sound
		gpBase->mpHelpFuncs->PlayGuiSoundData(mMapChangeData.msSound, eSoundEntryType_Gui);

	}
}

//-----------------------------------------------------------------------
