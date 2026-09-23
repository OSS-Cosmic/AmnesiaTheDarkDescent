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

#include "LuxDebugHandler.h"

#include "graphics/DebugDraw.h"
#include "graphics/Graphics.h"

#include "LuxMap.h"
#include "LuxPlayer.h"
#include "LuxPlayerHands.h"
#include "LuxPlayerHelpers.h"
#include "LuxMapHandler.h"
#include "LuxInputHandler.h"
#include "LuxMoveState_Normal.h"
#include "LuxSaveHandler.h"
#include "LuxPlayerState.h"
#include "LuxEntity.h"
#include "LuxJournal.h"
#include "LuxConfigHandler.h"
#include "LuxSavedGame.h"
#include "LuxInsanityHandler.h"
#include "LuxHelpFuncs.h"
#include "LuxAreaNodes.h"
#include "LuxConfigHandler.h"
#include "LuxGlobalDataHandler.h"
#include "LuxInventory.h"
#include "LuxLoadScreenHandler.h"



//////////////////////////////////////////////////////////////////////////
// STATIC FUNCTIONS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

static void LogMessageCallback(eLogOutputType aType, const char* asMessage)
{
	if (aType == eLogOutputType_Normal) return;

	gpBase->mpDebugHandler->AddErrorOrWarningMessage(cString::To16Char(asMessage));
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxDebugHandler::cLuxDebugHandler() : iLuxUpdateable("LuxDebugHandler")
{
	///////////////////////////////
	//Setup GUI stuff
	mpGui = gpBase->mpEngine->GetGui();

	if(gpBase->mpConfigHandler->mbLoadDebugMenu)
		mpGuiSkin = mpGui->CreateSkin("gui_default.skin");
	else
		mpGuiSkin = NULL;

	mpGuiSet = mpGui->CreateSet("DebugHandler",mpGuiSkin);

	mpInspectMeshEntity = NULL;

	mpCBPlayerStarts = NULL;

	msCurrentFilePath = _W("");

	mbWindowActive = false;
	
	mbFastForward = false;
	mpCBFastForward = NULL;

	mpCBRenderScale = NULL;
	mpCBRendererBackend = NULL;
	mLastSeenRendererBackend = eRendererBackend_Standard;
}

//-----------------------------------------------------------------------

cLuxDebugHandler::~cLuxDebugHandler()
{
	SetLogMessageCallback(NULL);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxDebugHandler::LoadUserConfig()
{
	////////////////////
	// DEBUG!!!

	//////////////////////
	//Load from config
	mbShowFPS = gpBase->mpUserConfig->GetBool("Debug", "ShowFPS", true);
	mbShowSoundPlaying = gpBase->mpUserConfig->GetBool("Debug", "ShowSoundPlaying", true);
	mbShowPlayerInfo = gpBase->mpUserConfig->GetBool("Debug", "ShowPlayerInfo", true);
	mbShowEntityInfo = gpBase->mpUserConfig->GetBool("Debug", "ShowEntityInfo", true);
	mbShowDebugMessages = gpBase->mpUserConfig->GetBool("Debug", "ShowDebugMessages", true);
	mbShowErrorMessages = gpBase->mpUserConfig->GetBool("Debug", "ShowErrorsAndWarnings", false);
	mbScriptDebugOn = gpBase->mpUserConfig->GetBool("Debug", "ScriptDebugOn", false);
	mbInspectionMode = gpBase->mpUserConfig->GetBool("Debug", "InspectionMode", false);
	mbDisableFlashBacks = gpBase->mpUserConfig->GetBool("Debug", "DisableFlashBacks", false);
	mbDrawPhysics = gpBase->mpUserConfig->GetBool("Debug", "DrawPhysics", false);

	mbReloadFromCurrentPosition = gpBase->mpUserConfig->GetBool("Debug", "ReloadFromCurrentPosition", true);

	mbAllowQuickSave = gpBase->mpUserConfig->GetBool("Debug", "AllowQuickSave", false);


	if(gpBase->mbPTestActivated)
	{
		#ifndef SKIP_PTEST_TESTS
			mbShowFPS = false;
			mbShowSoundPlaying = false;
			mbShowPlayerInfo = false;
			mbShowEntityInfo = false;
			mbShowDebugMessages = false;
			mbShowErrorMessages = false;
			mbScriptDebugOn = false;
			mbInspectionMode = false;
			mbDisableFlashBacks = false;
		#endif
	}

	/////////////////////////////////////////
	// Set callback for message
	SetLogMessageCallback(LogMessageCallback);

	//////////////////////
	//Create the debug window
	CreateGuiWindow();
}

void cLuxDebugHandler::SaveUserConfig()
{
	 gpBase->mpUserConfig->SetBool("Debug", "ShowFPS", mbShowFPS);
	 gpBase->mpUserConfig->SetBool("Debug", "ShowSoundPlaying", mbShowSoundPlaying);
	 gpBase->mpUserConfig->SetBool("Debug", "ShowPlayerInfo", mbShowPlayerInfo);
	 gpBase->mpUserConfig->SetBool("Debug", "ShowEntityInfo", mbShowEntityInfo);
	 gpBase->mpUserConfig->SetBool("Debug", "ShowDebugMessages", mbShowDebugMessages);
	 gpBase->mpUserConfig->SetBool("Debug", "ShowErrorsAndWarnings", mbShowErrorMessages);
	 gpBase->mpUserConfig->SetBool("Debug", "ScriptDebugOn", mbScriptDebugOn);
	 gpBase->mpUserConfig->SetBool("Debug", "InspectionMode", mbInspectionMode);
	 gpBase->mpUserConfig->SetBool("Debug", "DisableFlashBacks", mbDisableFlashBacks);
	 gpBase->mpUserConfig->SetBool("Debug", "DrawPhysics", mbDrawPhysics);

	 gpBase->mpUserConfig->SetBool("Debug", "ReloadFromCurrentPosition", mbReloadFromCurrentPosition);

	 gpBase->mpUserConfig->SetBool("Debug", "AllowQuickSave", mbAllowQuickSave);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::OnStart()
{
	//////////////////////
	//Init the gui
	gpBase->mpMapHandler->GetViewport()->AddGuiSet(mpGuiSet);
	mpGuiSet->SetActive(false);
}

//-----------------------------------------------------------------------


void cLuxDebugHandler::Reset()
{
	mbFirstUpdateOnMap = false;
	mpInspectMeshEntity = NULL;
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::Update(float afTimeStep)
{
	iCharacterBody *pCharBody = gpBase->mpPlayer->GetCharacterBody();

	if(mbFirstUpdateOnMap)// && mlTempCount>0)
	{
		mbFirstUpdateOnMap = false;

		//DEBUG:
		//gpBase->mpEngine->GetUpdater()->SetContainer("Journal");
		//gpBase->mpJournal->OpenNote(gpBase->mpJournal->GetNote(0));
		//gpBase->mpEngine->GetUpdater()->SetContainer("Inventory");
	}
	mlTempCount++;

	// Options can switch the backend too; follow it.
	SyncRendererBackendCombo();

	//////////////////////////////
	if(mlTempCount > 30 && m_lstBatchMaps.empty()==false)
	{
		mlTempCount =0;

		cLuxModelCache cache;
		cache.Create();

		tString sMap = m_lstBatchMaps.front();
		m_lstBatchMaps.pop_front();

        gpBase->Reset();
		gpBase->StartGame(sMap, "", "");

		cache.Destroy();
	}

	
	UpdateInspectionMeshEntity(afTimeStep);
	UpdateMessages(afTimeStep);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::OnMapEnter(cLuxMap *apMap)
{
	mbFirstUpdateOnMap = true;
	mlTempCount =0;
	mpInspectMeshEntity = NULL;

	if(mpCBPlayerStarts)
	{
		mpCBPlayerStarts->ClearItems();
		mpCBPlayerStarts->SetSelectedItem(-1);
		cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
		for(int i=0; i<pMap->GetPlayerStartNodeNum(); ++i)
		{
			cLuxNode_PlayerStart *pNode = pMap->GetPlayerStartNode(i);

			mpCBPlayerStarts->AddItem(pNode->GetName());
		}
		if(pMap->GetPlayerStartNodeNum()>0) mpCBPlayerStarts->SetSelectedItem(0);
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::OnMapLeave(cLuxMap *apMap)
{
	mpInspectMeshEntity = NULL;

	if(mpCBPlayerStarts)
	{
		mpCBPlayerStarts->ClearItems();
		mpCBPlayerStarts->SetSelectedItem(-1);
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::SetDebugWindowActive(bool abActive)
{
	if(gpBase->mpConfigHandler->mbLoadDebugMenu==false) return;

	//////////////////
	// Active
	if(abActive)
	{
		Log("-------- Debug window open!\n");
		mpGui->SetFocus(mpGuiSet);
		mpGuiSet->SetActive(true);
		gpBase->mpInputHandler->ChangeState(eLuxInputState_Debug);

		if(gpBase->mpConfigHandler->mbFullscreen==false) {
			gpBase->mpEngine->GetInput()->GetLowLevel()->LockInput(false);
			gpBase->mpEngine->GetInput()->GetLowLevel()->RelativeMouse(false);
		}
	}
	//////////////////
	// Disabled
	else
	{
		Log("-------- Debug window closed!\n");
		mpGui->SetFocus(NULL);
		mpGuiSet->SetActive(false);
		gpBase->mpInputHandler->ChangeState(eLuxInputState_Game);

		if(gpBase->mpConfigHandler->mbFullscreen==false) {
			gpBase->mpEngine->GetInput()->GetLowLevel()->LockInput(true);
			gpBase->mpEngine->GetInput()->GetLowLevel()->RelativeMouse(true);
		}
	}
}

//-----------------------------------------------------------------------

static tString PixelFormatToString(ePixelFormat aFormat)
{
	switch(aFormat)
	{
	case ePixelFormat_Unknown:			return "Unknown";
	case ePixelFormat_Alpha:			return "Alpha";
	case ePixelFormat_Luminance:		return "Luminance";
	case ePixelFormat_LuminanceAlpha:	return "LuminanceALpha";
	case ePixelFormat_RGB:				return "RGB";
	case ePixelFormat_RGBA:				return "RGBA";
	case ePixelFormat_BGR:				return "BGR";
	case ePixelFormat_BGRA:				return "BGRA";
	case ePixelFormat_DXT1:				return "DXT1";
	case ePixelFormat_DXT2:				return "DXT2";
	case ePixelFormat_DXT3:				return "DXT3";
	case ePixelFormat_DXT4:				return "DXT4";
	case ePixelFormat_DXT5:				return "DXT5";
	case ePixelFormat_Depth16:			return "Depth16";
	case ePixelFormat_Depth24:			return "Depth24";
	case ePixelFormat_Depth32:			return "Depth32";
	case ePixelFormat_Alpha16:			return "Alpha16";
	case ePixelFormat_Luminance16:		return "Luminance16";
	case ePixelFormat_LuminanceAlpha16:	return "LuminanceAlpha16";
	case ePixelFormat_RGB16:			return "RGB16";
	case ePixelFormat_RGBA16:			return "RGBA16";
	case ePixelFormat_Alpha32:			return "Alpha32";
	case ePixelFormat_Luminance32:		return "Luminance32";
	case ePixelFormat_LuminanceAlpha32:	return "LuminanceAlpha32";
	case ePixelFormat_RGB32:			return "RGB32";
	case ePixelFormat_RGBA32:			return "RGBA32";
	}	
	return "Unknown";
}

void cLuxDebugHandler::OnDraw(float afFrameTime)
{
	float fY = 5.0f;
	
	////////////////////
	// FPS
	if(mbShowFPS)
	{
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("FrameTime: %.1fms FPS: %.1f\n"),gpBase->mpEngine->GetAvgFrameTimeInMS(), gpBase->mpEngine->GetFPS());
		fY+=13.0f;

		// Per-pass GPU timing from RIGpuProfiler (timestamp queries, resolved each
		// frame in cGraphics::BeginActiveSet). Empty until a frame slot completes
		// (first couple of frames) or if the device lacks timestamp support.
		const std::vector<hpl::GpuPassTiming>& vGpuTimings = hpl::Interface<cGraphics>::Get()->profiler.lastResults();
		if(!vGpuTimings.empty())
		{
			const float fGpuTotal = hpl::Interface<cGraphics>::Get()->profiler.lastTotalMs();
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1,0,1),
				_W("GPU total: %.2f ms"), fGpuTotal);
			fY+=13.0f;

			const std::string sGpuMemory = hpl::Interface<cGraphics>::Get()->GpuMemoryDiagnostics();
			if(!sGpuMemory.empty())
			{
				gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(15,fY,10),13,cColor(0.6f,1,1,1),
					_W("%ls"), cString::To16Char(sGpuMemory).c_str());
				fY+=12.0f;
			}
			for(const hpl::GpuPassTiming& gpuPass : vGpuTimings)
			{
				const float fPct = fGpuTotal > 0.0f ? (gpuPass.ms / fGpuTotal * 100.0f) : 0.0f;
				gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(15,fY,10),13,cColor(1,1),
					_W("%ls: %.3f ms (%4.1f%%)"), cString::To16Char(gpuPass.name).c_str(), gpuPass.ms, fPct);
				fY+=12.0f;
			}
		}
	}

	////////////////////
	// Messages
	if(mbShowDebugMessages || mbShowErrorMessages)
	{
		float fY= Interface<cWindow>::Get()->GetSizeF().y - 40;
		for(tLuxDebugMessageListIt it = mlstMessages.begin(); it != mlstMessages.end();++it)
		{
			cLuxDebugMessage& debugMessage = *it;
			
			gpBase->mpGameDebugSet->DrawFont(debugMessage.msText,gpBase->mpDefaultFont.Get(),cVector3f(5,fY,10),14,cColor(1,1));
			fY -= 17;
		}
	}


	////////////////////
	// Player Info
	if(mbShowPlayerInfo)
	{
		cLuxPlayer *pPlayer = gpBase->mpPlayer;
		iCharacterBody *pCharBody = pPlayer->GetCharacterBody();

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("Position: %ls OnGround: %d Moved: %d Mass: %f ActiveSize: %d\n"),cString::To16Char(pCharBody->GetPosition().ToString()).c_str(), 
															pCharBody->IsOnGround(),pCharBody->GetMovedLastUpdate(),
															pCharBody->GetMass(),
															pCharBody->GetActiveSize());
		fY+=15.0f;
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("MoveSpeed: %f, %f (%f) Mul: %f AvgSpeed: %f\n"), pCharBody->GetMoveSpeed(eCharDir_Forward), pCharBody->GetMoveSpeed(eCharDir_Right),
																	pCharBody->GetVelocity(gpBase->mpEngine->GetStepSize()).Length(),
																	pPlayer->GetInteractionMoveSpeedMul(),
																	pPlayer->GetAvgSpeed());
		fY+=15.0f;

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("Climbing: %d\n"), pCharBody->IsClimbing());
		fY+=15.0f;

		if(pPlayer->GetCurrentMoveState()==eLuxMoveState_Normal)
		{
			cLuxMoveState_Normal *pMoveNormal = static_cast<cLuxMoveState_Normal*>(pPlayer->GetCurrentMoveStateData());
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
				_W("Crouching: %d Running: %d MoveSpeedMul %f RunSpeedMul %f\n"), pMoveNormal->IsCrouching(), pMoveNormal->IsRunning(), 
								pMoveNormal->GetMoveSpeedMul(), pMoveNormal->GetRunSpeedMul());
			fY+=15.0f;
		}

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("ForceVel: %ls\n"), cString::To16Char(pCharBody->GetForceVelocity().ToString()).c_str());
		fY+=15.0f;
		//gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,0),12,cColor(1,1),
		//							_W("Force Vel: %ls\n"), cString::To16Char(pCharBody->GetForceVelocity().ToString()).c_str() );

		//fY+=13.0f;
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
							_W("ExtLightLevel: %f NormalLightlevel: %f\n"), pPlayer->GetHelperLightLevel()->GetExtendedLightLevel(), pPlayer->GetHelperLightLevel()->GetNormalLightLevel() );
		fY+=15.0f;

		//Physical luminance before gain, and GPU result availability.
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
							_W("LightSensor: %ls Linear luminance: %f\n"),
							pPlayer->GetHelperLightLevel()->IsUsingProbe() ? _W("GPU") : _W("Held/default"),
							pPlayer->GetHelperLightLevel()->GetProbeIrradiance() );
		fY+=15.0f;

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
							_W("Health: %f Terror: %f\n"), pPlayer->GetHealth(), pPlayer->GetTerror() );
		fY+=15.0f;

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
							_W("Oil: %f Sanity: %f Tinderboxes: %d"), pPlayer->GetLampOil() , pPlayer->GetSanity(), pPlayer->GetTinderboxes());
		fY+=15.0f;

		//Insanity as the player experiences it, next to the sanity number that
		//drives it: which hallucination is running, and how loud the post
		//effect's wave is after the sanity ramp AND the light term
		//(Player_Sanity/Insanity*Mul) - so a wave that drops while the sanity
		//number holds still means the player just stepped into light.
		{
			int lEvent = gpBase->mpInsanityHandler->GetCurrentEvent();
			tString sEvent = lEvent >= 0 ? gpBase->mpInsanityHandler->GetEvent(lEvent)->GetName() : "none";

			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
								_W("Insanity event: '%ls' Wave: %f"),
								cString::To16Char(sEvent).c_str(),
								pPlayer->GetHelperSanity()->GetInsaneWaveAlpha());
			fY+=15.0f;
		}


		//gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,0),12,cColor(1,1),
		//					_W("FlashbackCount: %f Pulse: %f"), pPlayer->GetFlashbackCount() , pPlayer->GetFlashbackPulseCount());
		//fY+=13.0f;

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
										_W("State: %d\n"), pPlayer->GetCurrentState() );
		fY+=15.0f;

		tString sHandAnim = pPlayer->GetHands()->GetCurrentAnimation();
		cAnimationState *pAnim = pPlayer->GetHands()->GetHandsEntity()->GetAnimationStateFromName(sHandAnim);
		if(pAnim)
		{
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
				_W("HandAnim: '%ls' Time: %f/%f\n"), cString::To16Char(sHandAnim).c_str(), pAnim->GetTimePosition(), pAnim->GetRelativeTimePosition() );
			fY+=15.0f;
		}


		eLuxMoveState moveState = pPlayer->GetCurrentMoveState();
		if(moveState == eLuxMoveState_Normal)
		{
			cLuxMoveState_Normal* pNormalMoveState = static_cast<cLuxMoveState_Normal*>(pPlayer->GetMoveStateData(moveState));

			float fBob = pNormalMoveState->GetHeadBobCount();
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
									_W("HeadBob: %f cos: %f sin: %f\n"), fBob, cos(fBob), sin(fBob) );
			fY+=15.0f;
		}

		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("Sanity Between event time: %f AtLowSanityCount: %f\n"), gpBase->mpInsanityHandler->GetNewEventCount() , pPlayer->GetHelperSanity()->GetAtLowSanityCount());
		fY+=15.0f;

		////////////////////
		// HARDMODE
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),
			_W("Hardcore mode: %d "), gpBase->mbHardMode);
		fY+=15.0f;

        fY = pPlayer->GetStateData(pPlayer->GetCurrentState())->DrawDebug(gpBase->mpGameDebugSet,gpBase->mpDefaultFont.Get(), fY);		
	}

	////////////////////
	// Entity Info
	if(mbShowEntityInfo)
	{
		cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
		cLuxEntityIterator entIt = pMap->GetEntityIterator();
		while(entIt.HasNext())
		{
			iLuxEntity *pEntity = entIt.Next();

			if(pEntity->IsActive())
				fY = pEntity->DrawDebug(gpBase->mpGameDebugSet,gpBase->mpDefaultFont.Get(), fY);
		}
	}

	////////////////////////
	// Sound playing
	if(mbShowSoundPlaying)
	{
		tStringVec vSoundNames;
		std::vector<cSoundEntry*> vEntries;

		cSoundHandler *pSoundHandler = gpBase->mpEngine->GetSound()->GetSoundHandler();
		cMusicHandler *pMusicHandler = gpBase->mpEngine->GetSound()->GetMusicHandler();

		//////////////////////////////
		//Music
		cMusicEntry *pMusic = pMusicHandler->GetCurrentSong();
		if(pMusic)
		{
			fY+=5.0f;
			iSoundChannel *pChannel = pMusic->mpStream;
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(),cVector3f(5,fY,10),14,cColor(1,1),
				_W("Music: '%ls' vol: %.2f playing: %d prio: %d elapsed: %.2f total time: %.2f %ls"),
				cString::To16Char(pChannel->GetData()->GetName()).c_str(),
				pChannel->GetVolume(),
				pChannel->IsPlaying(),
				pChannel->GetPriority(),
				pChannel->GetElapsedTime(),
				pChannel->GetTotalTime(),
				pChannel->IsBufferUnderrun()? _W("*BUFFER UNDERRUN!!*") : _W("")
				);
			fY+=15.0f;
		}

		//////////////////////////////
		//Sounds

		//Get names and entries
		tSoundEntryList *pEntryList = pSoundHandler->GetEntryList();
		for(tSoundEntryListIt it = pEntryList->begin(); it != pEntryList->end();++it)
		{
			cSoundEntry *pEntry = *it;
			iSoundChannel *pSound = pEntry->GetChannel();
			vSoundNames.push_back(pSound->GetData()->GetName());
			vEntries.push_back(pEntry);
		}
		
		//Draw number of sounds
		gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),_W("Num of sounds: %d"),vSoundNames.size());
		fY+=15.0f;

		//Iterate sound entries and names
		int lRow=0, lCol=0;
		for(int i=0; i< (int)vSoundNames.size(); i++)
		{
			cSoundEntry *pEntry = vEntries[i];
			if(pEntry == NULL){
				lRow = 4;
				lCol =0;
				continue;
			}
			iSoundChannel* pChannel = pEntry->GetChannel();
			gpBase->mpGameDebugSet->DrawFont(gpBase->mpDefaultFont.Get(),cVector3f((float)lCol*250,fY+(float)lRow*15,10),14,cColor(1,1),
				_W("%ls%ls%ls(%.2f)(%d) (%.2f/%.2f)"),
				cString::To16Char(vSoundNames[i]).c_str(),
				pChannel->GetData()->IsStream()? _W("*st*") : _W(""),
				pChannel->IsBufferUnderrun()? _W("BUFFER UNDERRUN!!") : _W(""),
				pChannel->GetVolume(),
				pChannel->GetPriority(),
				pChannel->GetElapsedTime(),
				pChannel->GetTotalTime()
				);
			
			lCol++;
			if(lCol == 3)
			{
				lCol =0;
				lRow++;
			}
		}
		if(vSoundNames.empty()==false) fY+=15.0f * (lRow+1);
	}


	////////////////////////
	// InspectMode
	if(mbInspectionMode && mpInspectMeshEntity)
	{
		cGuiSet *pSet = gpBase->mpGameDebugSet;

		/////////////////////////////
		// Add text for the mesh entity
		cMaterial *pMaterial = mpInspectMeshEntity->GetMaterial();
		pSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),_W("EntityName: '%ls'"), cString::To16Char(mpInspectMeshEntity->GetName()).c_str()); 
		fY += 15;
		
		pSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),_W("MaterialFile: '%ls'"), pMaterial->GetFullPath().c_str()); 
		fY += 15;

		pSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),_W("---------------------")); 
		fY += 15;

		//for(int i=0; i<eMaterialTexture_LastEnum; ++i)
		//{
		//	Image* pTex = pMaterial->GetImage((eMaterialTexture)i);
		//	if(pTex==NULL) continue;

		//	//TODO: memory statstics ....
		//	//float fMemSize = ((float)pTex->GetMemorySize()) / (1024*1024);

		//	//pSet->DrawFont(gpBase->mpDefaultFont.Get(), cVector3f(5,fY,10),14,cColor(1,1),_W(" %d size: %d x %d x %d mem: %.2fmb type: %ls file: '%ls'"), 
		//	//	i, pTex->GetSize().x, pTex->GetSize().y, pTex->GetSize().z, fMemSize,
		//	//	cString::To16Char(PixelFormatToString(pTex->GetPixelFormat())).c_str(),
		//	//	pTex->GetFullPath().c_str()); 
		//	//fY += 15;
		//}
	}
}


//-----------------------------------------------------------------------

void cLuxDebugHandler::RenderSolid(DebugDraw* apDebugDraw)
{
	if(mbInspectionMode && mpInspectMeshEntity)
	{
		DebugDraw::DebugDrawOptions options;
		cMatrixf* pModelMtx = mpInspectMeshEntity->GetModelMatrix(NULL);
		if(pModelMtx) options.m_transform = *pModelMtx;

		apDebugDraw->DebugWireFrameFromVertexBuffer(mpInspectMeshEntity->GetVertexBuffer(), cColor(1,1,1,1), options);
	}

	if(mbDrawPhysics)
	{
		gpBase->mpMapHandler->GetCurrentMap()->GetPhysicsWorld()->RenderDebugGeometry(apDebugDraw, cColor(0.5f));
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::AddErrorOrWarningMessage(const tWString& asText)
{
	if(mbShowErrorMessages==false) return;

	/////////////////////////////
	// Check if message exists
	for(tLuxDebugMessageListIt it = mlstMessages.begin(); it != mlstMessages.end();++it)
	{
		cLuxDebugMessage& debugMessage = *it;
		if(debugMessage.msText == asText) return;
	}
	
	/////////////////////////////
	// Add message
	cLuxDebugMessage debugMessage;
	debugMessage.mfCount = 4;
	debugMessage.msText = asText;
	mlstMessages.push_front(debugMessage);

	if(mlstMessages.size()>100) mlstMessages.pop_back();
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::AddMessage(const tWString& asText, bool abCheckForDuplicates)
{
	if(mbShowDebugMessages==false) return;

	/////////////////////////////
	// Check if message exists
	if(abCheckForDuplicates)
	{
		for(tLuxDebugMessageListIt it = mlstMessages.begin(); it != mlstMessages.end();++it)
		{
			cLuxDebugMessage& debugMessage = *it;
			if(debugMessage.msText == asText) return;
		}
	}
	
	/////////////////////////////
	// Add message
	cLuxDebugMessage debugMessage;
	debugMessage.mfCount = 4;
	debugMessage.msText = asText;
	mlstMessages.push_front(debugMessage);

	if(mlstMessages.size()>100) mlstMessages.pop_back();
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::SetFastForward(bool abX)
{
	if(mbFastForward == abX) return;

	mbFastForward = abX;

	if(mpCBFastForward) mpCBFastForward->SetChecked(mbFastForward, false);

	gpBase->mpEngine->SetSpeedMul(mbFastForward ? 4.0f : 1.0f);

	gpBase->mpEngine->GetSound()->GetSoundHandler()->SetGlobalSpeed(mbFastForward ? 4.0f : 1.0f,eSoundEntryType_All, eLuxGlobalVolumeType_DebugMenu);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

static float gfMinT=0;

//-----------------------------------------------------------------------

void cLuxDebugHandler::CheckLineObjectIntersection(iRenderable *apObject, const cVector3f& avStart, const cVector3f& avEnd, cBoundingVolume *apBV)
{
	cBoundingVolume *pObjectBV = apObject->GetBoundingVolume();
	
	if(cMath::CheckBVIntersection(*pObjectBV, *apBV)==false) return;

	float fT=0;
	
	if(cMath::CheckPointInBVIntersection(avStart, *pObjectBV)==false)
	{
		if(cMath::CheckAABBLineIntersection(pObjectBV->GetMin(), pObjectBV->GetMax(), avStart, avEnd, NULL, &fT)==false) return;
		if(fT > gfMinT) return;
	}

	cMatrixf mtxInvModel = cMath::MatrixInverse(apObject->GetWorldMatrix());
	bool bIntersect = cMath::CheckLineTriVertexBufferIntersection(	avStart, avEnd,mtxInvModel, apObject->GetVertexBuffer(),NULL, &fT, NULL,true);
	if(bIntersect==false || fT > gfMinT) return;
	
	gfMinT = fT;
	mpInspectMeshEntity = static_cast<cSubMeshEntity*>(apObject);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::IterateRenderables(cRenderableSet *apSet, const cVector3f& avStart, const cVector3f& avEnd, cBoundingVolume *apBV)
{
	for(iRenderable *pObject : apSet->GetObjects())
	{
		if(pObject->GetRenderType() != eRenderableType_SubMesh) continue;

		CheckLineObjectIntersection(pObject, avStart, avEnd, apBV);
	}
}

//-----------------------------------------------------------------------



void cLuxDebugHandler::UpdateInspectionMeshEntity(float afTimeStep)
{
	if(mbInspectionMode==false) return;

	/////////////////////////////
	// Set up line
	cCamera *pCam = gpBase->mpPlayer->GetCamera();
	cVector3f vStart = pCam->GetPosition();
	cVector3f vEnd = vStart + pCam->GetForward()*15;
	
    cBoundingVolume lineBV;
	lineBV.SetLocalMinMax(cMath::Vector3Min(vStart, vEnd), cMath::Vector3Max(vStart, vEnd));
    
	/////////////////////////////
	// Set up variables
	gfMinT = 9999.0f;
	mpInspectMeshEntity = NULL;
	
	/////////////////////////////
	// Get renderable sets
    cWorld *pWorld = gpBase->mpMapHandler->GetCurrentMap()->GetWorld();
	cRenderableSet *pSets[2] ={
		pWorld->GetRenderableSet(eWorldContainerType_Dynamic),
		pWorld->GetRenderableSet(eWorldContainerType_Static),
	};

	/////////////////////////////
	// Scan the sets
	for(int i=0; i<2; ++i)
	{
		pSets[i]->UpdateBeforeRendering();
		IterateRenderables(pSets[i], vStart, vEnd, &lineBV);
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::UpdateMessages(float afTimeStep)
{
	for(tLuxDebugMessageListIt it = mlstMessages.begin(); it != mlstMessages.end();)
	{
		cLuxDebugMessage& debugMessage = *it;
		debugMessage.mfCount-=afTimeStep;
		if(debugMessage.mfCount <= 0)
		{
			it = mlstMessages.erase(it);
		}
		else
		{
			++it;
		}
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::CreateScriptOutputWindow()
{
	if(gpBase->mpConfigHandler->mbLoadDebugMenu==false) return;

	//////////////////////////
	//Set up variables
	cWidgetCheckBox *pCheckBox=NULL;
	cWidgetButton *pButton = NULL;
	cWidgetComboBox *pComboBox = NULL;
	cWidgetLabel *pLabel = NULL;
	cWidgetGroup *pGroup = NULL;
	cWidgetSlider *pSlider = NULL;

	cVector3f vGroupPos =0;
	cVector2f vGroupSize =0;

	///////////////////////////
	//Window
	cVector2f vSize = cVector2f(700, 550);
	cVector3f vPos = cVector3f(mpGuiSet->GetVirtualSize().x/2 - vSize.x/2, 50, 10);
	mpScriptOutputWindow = mpGuiSet->CreateWidgetWindow(0,vPos,vSize,_W("Script output") );

	///////////////////////////
	// Frame
	mpScriptOutputFrame = mpGuiSet->CreateWidgetFrame(cVector3f(10, 30, 1), vSize-cVector2f(20, 80), true, mpScriptOutputWindow, true, true);

	///////////////////////////
	// Button
	vGroupSize = cVector2f(110, 30);
	vGroupPos = cVector3f(vSize.x/2 - vGroupSize.x/2, vSize.y - vGroupSize.y - 10,1);
	pButton = mpGuiSet->CreateWidgetButton(vGroupPos, vGroupSize,_W("Close Window"), mpScriptOutputWindow); 
	pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressCloseScriptOutput));

	mpScriptOutputWindow->SetVisible(false);
	mpScriptOutputWindow->SetEnabled(false);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::CreateScriptOutputWindowText(const tWString& asOutput)
{
	if(gpBase->mpConfigHandler->mbLoadDebugMenu==false) return;

	//////////////////////////
	// Destroy all widgets
	if(mlstScriptOutputWidgets.size()>0)
	{
		tWidgetListIt it = mlstScriptOutputWidgets.begin(); 
		for(; it != mlstScriptOutputWidgets.end(); ++it)
		{
			iWidget *pWidget = *it;
            
			mpGuiSet->DestroyWidget(pWidget);
		}
	}
	mlstScriptOutputWidgets.clear();


	//////////////////////////
	// Create text widgets
	cWidgetLabel *pLabel = NULL;
	iFontData *pFont =mpGuiSet->GetSkin()->GetFont(eGuiSkinFont_Default)->mpFont.Get();
	
	cVector3f vGroupPos = cVector3f(5, 10, 1);
	cVector2f vGroupSize = cVector2f(200, 16);
	cVector2f vFontSize = 14;

	tWStringVec vRows;
    tWString sSepp = _W("\n");
	cString::GetStringVecW(asOutput, vRows,&sSepp);

    for(size_t i=0; i<vRows.size(); ++i)
	{
		float fWidth = pFont->GetLength(vFontSize, vRows[i].c_str());

		pLabel = mpGuiSet->CreateWidgetLabel(vGroupPos, cVector2f(fWidth ,vGroupSize.y),vRows[i], mpScriptOutputFrame);
		pLabel->SetDefaultFontSize(vFontSize);
		mlstScriptOutputWidgets.push_back(pLabel);

		vGroupPos.y += vFontSize.y +2.0f;
	}
}

//-----------------------------------------------------------------------


void cLuxDebugHandler::CreateGuiWindow()
{
	if(gpBase->mpConfigHandler->mbLoadDebugMenu==false) return;

	//////////////////////////
	//Set up variables
	cWidgetCheckBox *pCheckBox=NULL;
	cWidgetButton *pButton = NULL;
	cWidgetComboBox *pComboBox = NULL;
	cWidgetLabel *pLabel = NULL;
	cWidgetGroup *pGroup = NULL;
	cWidgetSlider *pSlider = NULL;

	cVector3f vGroupPos =0;
	cVector2f vGroupSize =0;

	///////////////////////////
	//Window
	cVector2f vSize = cVector2f(250, 901);
	vGroupSize.x = vSize.x - 20;
	cVector3f vPos = cVector3f(mpGuiSet->GetVirtualSize().x - vSize.x - 10, 10, 0);
	mpDebugWindow = mpGuiSet->CreateWidgetWindow(0,vPos,vSize,_W("Debug Toolbar") );

	vSize = cVector2f(vSize.x-30, 18);
	vPos = cVector3f(10, 30, 0.1f);


	//////////////////////////
	//Debug texts
	{
		//Group
		vGroupPos = cVector3f(5,10,0.1f);
		pGroup = mpGuiSet->CreateWidgetGroup(vPos,100,_W("Debug texts"),mpDebugWindow);

		//Show FPS
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show FPS"),pGroup);
		pCheckBox->SetChecked(mbShowFPS);
		pCheckBox->SetUserValue(0);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Show player info
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show player info"),pGroup);
		pCheckBox->SetChecked(mbShowPlayerInfo);
		pCheckBox->SetUserValue(1);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Show entity info
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show entity info"),pGroup);
		pCheckBox->SetChecked(mbShowEntityInfo);
		pCheckBox->SetUserValue(2);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Show sounds playing
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show sounds playing"),pGroup);
		pCheckBox->SetChecked(mbShowSoundPlaying);
		pCheckBox->SetUserValue(3);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Show debug messages
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show debug messages"),pGroup);
		pCheckBox->SetChecked(mbShowDebugMessages);
		pCheckBox->SetUserValue(4);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Show error messages
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show errors and warnings"),pGroup);
		pCheckBox->SetChecked(mbShowErrorMessages);
		pCheckBox->SetUserValue(12);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Inspection mode
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Inspection mode"),pGroup);
		pCheckBox->SetChecked(mbInspectionMode);
		pCheckBox->SetUserValue(5);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Occlusion culling
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Occlusion Culling"),pGroup);
		pCheckBox->SetChecked(gpBase->mpMapHandler->GetViewport()->GetRenderSettings()->mbUseOcclusionCulling);
		pCheckBox->SetUserValue(6);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Physics debug drawing
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Draw physics debug"), pGroup);
		pCheckBox->SetChecked(mbDrawPhysics);
		pCheckBox->SetUserValue(11);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Resource logging
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Resource Logging"), pGroup);
		pCheckBox->SetChecked(iResourceBase::GetLogCreateAndDelete(), false);
		pCheckBox->SetUserValue(7);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Print Container debug info
		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Print Container Debug Info"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressPrinfContDebugInfo));
		vGroupPos.y += 22;

		//Group end
		vGroupSize.y = vGroupPos.y + 15;
		pGroup->SetSize(vGroupSize);
		vPos.y += vGroupSize.y + 15;
	}

	//////////////////////////
	//Map
	{
		//Group
		vGroupPos = cVector3f(5,10,0.1f);
		pGroup = mpGuiSet->CreateWidgetGroup(vPos,100,_W("Map"),mpDebugWindow);

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Load Map"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressLoadWorld));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Recompile Script and Lang"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressRecompileScript));
		vGroupPos.y += 22;

		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Reload from current pos"), pGroup);
		pCheckBox->SetChecked(mbReloadFromCurrentPosition, false);
		pCheckBox->SetUserValue(8);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Fast Physics Load"), pGroup);
		pCheckBox->SetChecked(gpBase->mpConfigHandler->mbFastPhysicsLoad, false);
		pCheckBox->SetUserValue(9);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Reload Map"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressLevelReload));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Quick Map Reload (F2)"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressQuickLevelReload));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Test Change Map Save"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressTestChangeMapSave));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Teleport to start pos"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressTelportPlayer));
		vGroupPos.y += 22;

		mpCBPlayerStarts = mpGuiSet->CreateWidgetComboBox(vGroupPos,vSize,_W("None"),pGroup);
		mpCBPlayerStarts->SetSelectedItem(-1);
		vGroupPos.y += 22;
		
		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Map Batch"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressBatchLoad));
		vGroupPos.y += 22;
		

		
		//Group end
		vGroupSize.y = vGroupPos.y + 15;
		pGroup->SetSize(vGroupSize);
		vPos.y += vGroupSize.y + 15;
	}

	//////////////////////////
	//Game
	{
		//Group
		vGroupPos = cVector3f(5,10,0.1f);
		pGroup = mpGuiSet->CreateWidgetGroup(vPos,100,_W("Game"),mpDebugWindow);

		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Disable Flashbacks"), pGroup);
		pCheckBox->SetChecked(mbDisableFlashBacks, false);
		pCheckBox->SetUserValue(10);
		pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;
		
		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Reload Insanity Effects"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressReloadInsanityEffect));
		vGroupPos.y += 22;

		pButton = mpGuiSet->CreateWidgetButton(vGroupPos,vSize,_W("Start Insanity Effect"),pGroup);
		pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressStartInsanityEffect));
		vGroupPos.y += 22;

		mpCBInsanityEvents = mpGuiSet->CreateWidgetComboBox(vGroupPos,vSize,_W(""),pGroup);
		for(int i=0; i<gpBase->mpInsanityHandler->GetEventNum(); ++i)
		{
			iLuxInstanityEvent *pEvent = gpBase->mpInsanityHandler->GetEvent(i);
			mpCBInsanityEvents->AddItem(pEvent->GetName());            
		}
		if(gpBase->mpInsanityHandler->GetEventNum()>0) mpCBInsanityEvents->SetSelectedItem(0);
		vGroupPos.y += 22;

		mpCBFastForward = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Fast Forward (F3)"), pGroup);
		mpCBFastForward->SetChecked(mbFastForward, false);
		mpCBFastForward->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDebugText));
		mpCBFastForward->SetUserValue(17);
		vGroupPos.y += 22;

		//Enable fly camera
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(vGroupPos, vSize, _W("Fly camera"), pGroup);
		pCheckBox->SetChecked(false);
		pCheckBox->SetUserValue(13);
		pCheckBox->AddCallback(eGuiMessage_CheckChange, this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;

		//Set fly camera speed
		pSlider = mpGuiSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal, vGroupPos, vSize, 100, pGroup, ("Fly camera speed"));
		pSlider->SetValue(10, false);
		pSlider->SetUserValue(14);
		pSlider->AddCallback(eGuiMessage_SliderMove, this, kGuiCallback(ChangeDebugText));
		vGroupPos.y += 22;


		//Group end
		vGroupSize.y = vGroupPos.y + 15;
		pGroup->SetSize(vGroupSize);
		vPos.y += vGroupSize.y + 15;
	}

	//////////////////////////
	// Renderer
	{
		vGroupPos = cVector3f(5, 10, 0.1f);
		pGroup = mpGuiSet->CreateWidgetGroup(vPos, 100, _W("Renderer"), mpDebugWindow);

		cGraphics *pGraphics = gpBase->mpEngine->GetGraphics();

		// User values carry the backend so selection never depends on item order.
		mpCBRendererBackend = mpGuiSet->CreateWidgetComboBox(vGroupPos, vSize, _W(""), pGroup);
		mpCBRendererBackend->AddItem("Standard")->SetUserValue((int)eRendererBackend_Standard);
		mpCBRendererBackend->AddItem("Ray Traced")->SetUserValue((int)eRendererBackend_RayTraced);

		mLastSeenRendererBackend = pGraphics->GetRendererBackend();
		mpCBRendererBackend->SetSelectedItem(mLastSeenRendererBackend == eRendererBackend_RayTraced ? 1 : 0, false, false);

		// The engine already started Standard on a GPU without ray tracing.
		mpCBRendererBackend->SetEnabled(pGraphics->IsRayTracedSupported());
		mpCBRendererBackend->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(ChangeRendererBackend));
		vGroupPos.y += 22;

		// Group end
		vGroupSize.y = vGroupPos.y + 15;
		pGroup->SetSize(vGroupSize);
		vPos.y += vGroupSize.y + 15;
	}

	//////////////////////////
	// Render Scale (dev)
	{
		// Development-only control for the render/display resolution split: it only changes
		// the render extent so the spatial-fallback and display-depth-reconstruction paths
		// can be exercised; it wires no upscaler provider and enables no camera jitter.
		vGroupPos = cVector3f(5, 10, 0.1f);
		pGroup = mpGuiSet->CreateWidgetGroup(vPos, 100, _W("Render Scale (dev)"), mpDebugWindow);

		// Deliberately not persisted; each launch starts at native.
		mpCBRenderScale = mpGuiSet->CreateWidgetComboBox(vGroupPos, vSize, _W("Native (100%)"), pGroup);
		mpCBRenderScale->AddItem("Native (100%)");
		mpCBRenderScale->AddItem("90%");
		mpCBRenderScale->AddItem("75%");
		mpCBRenderScale->AddItem("66%");
		mpCBRenderScale->AddItem("50%");
		mpCBRenderScale->AddItem("33%");
		mpCBRenderScale->SetSelectedItem(0);
		mpCBRenderScale->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(ChangeRenderScale));
		vGroupPos.y += 22;

		// Group end
		vGroupSize.y = vGroupPos.y + 15;
		pGroup->SetSize(vGroupSize);
		vPos.y += vGroupSize.y + 15;
	}

	//////////////////////////
	// Script output window
	CreateScriptOutputWindow();
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::ShowScriptOutputWindow(const tWString& asName, const tString& asText)
{
	CreateScriptOutputWindowText(cString::To16Char(asText));

	mpScriptOutputWindow->SetText(_W("Script output for ")+asName);
	mpScriptOutputWindow->SetEnabled(true);
	mpScriptOutputWindow->SetVisible(true);
}

//-----------------------------------------------------------------------


bool cLuxDebugHandler::RecompileScript()
{
	cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
	
	tString sOutput;
	if(pMap->RecompileScript(&sOutput)==false)
	{
		ShowScriptOutputWindow(_W("Map"), sOutput);
		return false;
	}
	if(gpBase->mpInventory->RecompileScript(&sOutput)==false)
	{
		ShowScriptOutputWindow(_W("Inventory"), sOutput);
		return false;
	}
	if(gpBase->mpGlobalDataHandler->RecompileScript(&sOutput)==false)
	{
		ShowScriptOutputWindow(_W("Global"), sOutput);
		return false;
	}
	
	return true;
}

void cLuxDebugHandler::ReloadTranslations()
{
	gpBase->mpEngine->GetResources()->ClearTranslations();
	gpBase->LoadLanguage(gpBase->mpMainConfig->GetString("Game", "StartLanguage", gpBase->msDefaultGameLanguage), true);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::ReloadMap()
{
	///////////////////
	// Reload all resources
	gpBase->mpEngine->GetResources()->LoadResourceDirsFile(gpBase->msResourceConfigPath
#ifdef USERDIR_RESOURCES
														   ,gpBase->msUserResourceDir
#endif
														   );

	///////////////////
	// Reload all lang files
	ReloadTranslations();

	///////////////////
	//Script
	if(RecompileScript()==false)
	{
		return;
	}

	///////////////////
	// Stop music
	gpBase->mpEngine->GetSound()->GetMusicHandler()->Stop(10);

	///////////////////
	//Get char variables
	iCharacterBody *pCharBody = gpBase->mpPlayer->GetCharacterBody();
	cVector3f vPlayerPos = pCharBody->GetPosition();
	cCamera *pCam = gpBase->mpPlayer->GetCamera();
	cVector3f vCameraAngles = cVector3f(pCam->GetPitch(),pCam->GetYaw(), 0);

	///////////////////
	//Reload map
	tString sMapFile = gpBase->mpMapHandler->GetCurrentMap()->GetFileName();
	gpBase->Reset();
	
	gpBase->mpEngine->GetResources()->GetSoundManager()->DestroyUnused(0);
	gpBase->mpEngine->GetResources()->GetParticleManager()->DestroyUnused(0);
	gpBase->mpEngine->GetResources()->GetSoundEntityManager()->DestroyUnused(0);

	gpBase->StartGame(sMapFile, "", "");

	///////////////////
	//Set char variables
	if(mbReloadFromCurrentPosition)
	{
		pCharBody = gpBase->mpPlayer->GetCharacterBody();
		pCam = gpBase->mpPlayer->GetCamera();

		pCam->SetPitch(vCameraAngles.x);
		pCam->SetYaw(vCameraAngles.y);
		pCam->SetRoll(vCameraAngles.z);
		pCharBody->SetPosition(vPlayerPos);
		pCharBody->SetYaw(pCam->GetYaw());
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::QuickReloadMap()
{
	///////////////////
	// Save cache of all entity meshes
	cLuxModelCache cache;
	cache.Create();
	
	
	///////////////////
	// Reload
	ReloadMap();	

	///////////////////
	// Destroy mesh and animation cache
	cache.Destroy();	
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::TestChangeMapSave()
{
	gpBase->mpLoadScreenHandler->DrawMenuScreen();

	///////////////////
	//Get char variables
	iCharacterBody *pCharBody = gpBase->mpPlayer->GetCharacterBody();
	cVector3f vPlayerPos = pCharBody->GetPosition();
	cCamera *pCam = gpBase->mpPlayer->GetCamera();
	cVector3f vCameraAngles = cVector3f(pCam->GetPitch(),pCam->GetYaw(), 0);

	///////////////////
	// Save cache of all entity meshes
	cLuxModelCache cache;
	cache.Create();

	cLuxSavedGameMapCollection *pSavedMaps = gpBase->mpMapHandler->GetSavedMapCollection();
	cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
	tString sMapFile = pMap->GetFileName();

	pSavedMaps->SaveMap(pMap);

	//No need to call destroy or leave, as this in done in destroy!
	gpBase->mpMapHandler->DestroyMap(pMap, false);

	//Save, reset, and the load the saved maps so the saved data format is tested too!
	tWString sFile = _W("_debug_test_change_map.save");
	cSerializeClass::SaveToFile(pSavedMaps,sFile,"SavedMaps");
	pSavedMaps->Reset();
	cSerializeClass::LoadFromFile(pSavedMaps, sFile);

	pMap = gpBase->mpMapHandler->LoadMap(sMapFile,true);
	
	gpBase->mpMapHandler->SetCurrentMap(pMap, false, false, "");
	
	pSavedMaps->LoadMap(pMap);

	pMap->RunScript("OnEnter()");

	///////////////////
	// Destroy mesh and animation cache
	cache.Destroy();

	///////////////////
	//Set char variables
	if(mbReloadFromCurrentPosition)
	{
		pCharBody = gpBase->mpPlayer->GetCharacterBody();
		pCam = gpBase->mpPlayer->GetCamera();

		pCam->SetPitch(vCameraAngles.x);
		pCam->SetYaw(vCameraAngles.y);
		pCam->SetRoll(vCameraAngles.z);
		pCharBody->SetPosition(vPlayerPos);
		pCharBody->SetYaw(pCam->GetYaw());
	}
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::LoadBatchLoadFile(const tWString& asFilePath)
{
	tString sFileData;
	unsigned int lFileSize = cPlatform::GetFileSize(asFilePath);
	sFileData.resize(lFileSize);
	cPlatform::CopyFileToBuffer(asFilePath,&sFileData[0],lFileSize);

	tStringVec vStrings;
	cString::GetStringVec(sFileData, vStrings);

	m_lstBatchMaps.clear();
	for(size_t i=0; i<vStrings.size(); ++i) m_lstBatchMaps.push_back(vStrings[i]);
}

//-----------------------------------------------------------------------

void cLuxDebugHandler::DrawDynamicContainerDebugInfo()
{
	cRenderableSet* pDynSet = gpBase->mpMapHandler->GetCurrentMap()->GetWorld()->GetRenderableSet(eWorldContainerType_Dynamic);
	pDynSet->UpdateBeforeRendering();

	////////////////////////////////////////
	// Dynamic set output
	Log("---------- BEGIN DYNAMIC SET OUTPUT (%d objects) ---------------\n", pDynSet->Size());

	for(iRenderable *pObject : pDynSet->GetObjects())
	{
		cBoundingVolume *pBV = pObject->GetBoundingVolume();
		Log(" %s (%s) AABB: (%s)-(%s)\n", pObject->GetName().c_str(), pObject->GetEntityType().c_str(),
											pBV->GetMin().ToString().c_str(), pBV->GetMax().ToString().c_str());
	}

	Log("---------- END DYNAMIC SET OUTPUT ---------------\n");
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// GUI CALLBACKS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cLuxDebugHandler::ChangeDebugText(iWidget* apWidget, const cGuiMessageData& aData)
{
	int lNum = apWidget->GetUserValue();
	bool bActive = aData.mlVal == 1;

    if(lNum == 0)		mbShowFPS = bActive;
	else if(lNum == 1)	 mbShowPlayerInfo = bActive;
	else if(lNum == 2)	 mbShowEntityInfo = bActive;
	else if(lNum == 3)	 mbShowSoundPlaying = bActive;
	else if(lNum == 4)	 mbShowDebugMessages = bActive;
	else if(lNum == 5)	 mbInspectionMode = bActive;
	else if(lNum == 6)	 gpBase->mpMapHandler->GetViewport()->GetRenderSettings()->mbUseOcclusionCulling = bActive;
	else if(lNum == 7)	 iResourceBase::SetLogCreateAndDelete(bActive);
	else if(lNum == 8)	 mbReloadFromCurrentPosition = bActive;
	else if(lNum == 9)	 gpBase->mpConfigHandler->mbFastPhysicsLoad = bActive;
	else if(lNum == 10)	 mbDisableFlashBacks = bActive;
	else if(lNum == 11)  mbDrawPhysics = bActive;
	else if(lNum == 12)  mbShowErrorMessages = bActive;

	else if(lNum == 13)  gpBase->mpPlayer->SetFreeCamActive(bActive);
	else if(lNum == 14)  gpBase->mpPlayer->SetFreeCamSpeed( cMath::Max((float)aData.mlVal/ 100.0f, 0.001f) );

	else if(lNum == 17)  SetFastForward(bActive);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, ChangeDebugText);

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressPrinfContDebugInfo(iWidget* apWidget, const cGuiMessageData& aData)
{
	DrawDynamicContainerDebugInfo();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressPrinfContDebugInfo);

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressLevelReload(iWidget* apWidget, const cGuiMessageData& aData)
{
	ReloadMap();
	
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressLevelReload);

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressQuickLevelReload(iWidget* apWidget, const cGuiMessageData& aData)
{
	QuickReloadMap();
	
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressQuickLevelReload);

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressTestChangeMapSave(iWidget* apWidget,const cGuiMessageData& aData)
{
	TestChangeMapSave();

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressTestChangeMapSave); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressLoadWorld(iWidget* apWidget,const cGuiMessageData& aData)
{
	mvPickedFiles.clear();
	
	cGuiPopUpFilePicker* pPicker = mpGuiSet->CreatePopUpLoadFilePicker(mvPickedFiles,false,msCurrentFilePath,false, this, kGuiCallback(LoadWorldFromFilePicker));
	pPicker->AddCategory(_W("Scenes"),_W("*.map"));
	pPicker->AddFilter(0, _W("*.dae"));

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressLoadWorld); 

bool cLuxDebugHandler::LoadWorldFromFilePicker(iWidget* apWidget,const cGuiMessageData& aData)
{
	if(mvPickedFiles.empty()) return true;

	tWString& sFilePath = mvPickedFiles[0];

	msCurrentFilePath = cString::GetFilePathW(sFilePath);
	
	tString sMapFile = cString::To8Char(cString::GetFileNameW(sFilePath));
	gpBase->Reset();
	gpBase->StartGame(sMapFile, "", "");
	
	return true;		
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, LoadWorldFromFilePicker); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressTelportPlayer(iWidget* apWidget,const cGuiMessageData& aData)
{
	if(mpCBPlayerStarts->GetSelectedItem()<0) return true;

    cLuxMap *pMap = gpBase->mpMapHandler->GetCurrentMap();
	
	const cWidgetItem *pItem = mpCBPlayerStarts->GetItem(mpCBPlayerStarts->GetSelectedItem());
	pMap->PlacePlayerAtStartPos(cString::To8Char(pItem->GetText()));
	
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressTelportPlayer); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressRecompileScript(iWidget* apWidget,const cGuiMessageData& aData)
{
	if(RecompileScript())
	{
		ReloadTranslations();
	}
	
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressRecompileScript); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressReloadInsanityEffect(iWidget* apWidget,const cGuiMessageData& aData)
{
	mpCBInsanityEvents->ClearItems();
	mpCBInsanityEvents->SetSelectedItem(-1);

	gpBase->mpInsanityHandler->ReloadEvents();

	for(int i=0; i<gpBase->mpInsanityHandler->GetEventNum(); ++i)
	{
		iLuxInstanityEvent *pEvent = gpBase->mpInsanityHandler->GetEvent(i);
		mpCBInsanityEvents->AddItem(pEvent->GetName());            
	}
	if(gpBase->mpInsanityHandler->GetEventNum()>0) 
		mpCBInsanityEvents->SetSelectedItem(0);	

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressReloadInsanityEffect); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressStartInsanityEffect(iWidget* apWidget,const cGuiMessageData& aData)
{
	if(mpCBInsanityEvents->GetSelectedItem() <0) return true;

	gpBase->mpInsanityHandler->StartEvent(mpCBInsanityEvents->GetSelectedItem());

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressStartInsanityEffect); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressCloseScriptOutput(iWidget* apWidget,const cGuiMessageData& aData)
{
    mpScriptOutputWindow->SetVisible(false);
	mpScriptOutputWindow->SetEnabled(false);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressCloseScriptOutput); 

//-----------------------------------------------------------------------

bool cLuxDebugHandler::PressBatchLoad(iWidget* apWidget,const cGuiMessageData& aData)
{
	mvPickedFiles.clear();

	cGuiPopUpFilePicker* pPicker = mpGuiSet->CreatePopUpLoadFilePicker(mvPickedFiles,false,msCurrentFilePath,false, this, kGuiCallback(PressLoadBatchLoadFile));
	pPicker->AddCategory(_W("MapBatch"),_W("*.mapbatch"));

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressBatchLoad);

bool cLuxDebugHandler::PressLoadBatchLoadFile(iWidget* apWidget,const cGuiMessageData& aData)
{
	if(mvPickedFiles.empty()) return true;
	
	LoadBatchLoadFile(mvPickedFiles[0]);
	
	return true;		
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, PressLoadBatchLoadFile); 

//-----------------------------------------------------------------------

// Development-only control for the render/display resolution split: it only changes
// the render extent so the spatial-fallback and display-depth-reconstruction paths
// can be exercised; it wires no upscaler provider and enables no camera jitter.
bool cLuxDebugHandler::ChangeRenderScale(iWidget* apWidget, const cGuiMessageData& aData)
{
	static const float vRenderScales[] = {1.0f, 0.90f, 0.75f, 0.66f, 0.50f, 0.33f};
	if(aData.mlVal < 0 || aData.mlVal >= (int)(sizeof(vRenderScales) / sizeof(vRenderScales[0]))) return true;

	cGraphics *pGraphics = hpl::Interface<cGraphics>::Get();
	if(pGraphics) pGraphics->devRenderScale = vRenderScales[aData.mlVal];

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, ChangeRenderScale);

//-----------------------------------------------------------------------

bool cLuxDebugHandler::ChangeRendererBackend(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(aData.mlVal < 0 || aData.mlVal >= mpCBRendererBackend->GetItemNum()) return true;

	cWidgetItem* pItem = mpCBRendererBackend->GetItem(aData.mlVal);
	eRendererBackend backend = pItem->GetUserValue() == (int)eRendererBackend_RayTraced ?
									eRendererBackend_RayTraced : eRendererBackend_Standard;

	// Saved on exit, same as the Options menu.
	gpBase->mpConfigHandler->mRendererBackend = backend;
	// Deferred to the next frame boundary: this runs mid-frame and the switch
	// destroys a renderer.
	gpBase->mpEngine->GetGraphics()->RequestRendererBackend(backend);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, ChangeRendererBackend);

//-----------------------------------------------------------------------

void cLuxDebugHandler::SyncRendererBackendCombo()
{
	if(mpCBRendererBackend == NULL) return;

	eRendererBackend current = gpBase->mpEngine->GetGraphics()->GetRendererBackend();
	if(current == mLastSeenRendererBackend) return;

	mLastSeenRendererBackend = current;
	mpCBRendererBackend->SetSelectedItem(current == eRendererBackend_RayTraced ? 1 : 0, false, false);
}
