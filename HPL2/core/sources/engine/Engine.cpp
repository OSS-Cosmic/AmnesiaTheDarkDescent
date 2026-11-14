/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#include "engine/Engine.h"

#include "ai/AI.h"
#include "graphics/Graphics.h"
#include "gui/Gui.h"
#include "haptic/Haptic.h"
#include "physics/Physics.h"
#include "resources/Resources.h"
#include "scene/Scene.h"
#include "sound/Sound.h"
#include "system/System.h"

#include "system/LogicTimer.h"
#include "system/Mutex.h"
#include "system/Platform.h"
#include "system/String.h"
#include "system/Timer.h"

#include "input/Input.h"
#include "input/LowLevelInput.h"
#include "input/Mouse.h"

#include "graphics/Renderer.h"
#include "graphics/Window.h"

#include "engine/EngineInitVars.h"
#include "engine/Interface.h"
#include "engine/ScriptFuncs.h"
#include "engine/Updater.h"

#include "engine/LowLevelEngineSetup.h"
#include "system/LowLevelSystem.h"

#include "impl/SDLEngineSetup.h"

#include "SDL2/SDL.h"

namespace hpl {

//////////////////////////////////////////////////////////////////////////
// FPS COUNTER
//////////////////////////////////////////////////////////////////////////

cFPSCounter::cFPSCounter(iLowLevelSystem *apLowLevelSystem) {
  mfFPS = 60;

  mlFramecounter = 0;
  mfFrametimestart = 0;
  mfFrametime = 0;

  mfUpdateRate = 1;

  mpLowLevelSystem = apLowLevelSystem;

  mfFrametimestart = (float)cPlatform::GetApplicationTimeX();
}

void cFPSCounter::AddFrame() {
  mlFramecounter++;

  mfFrametime = (float)cPlatform::GetApplicationTimeX() - mfFrametimestart;

  // update the timer
  if (mfFrametime >= mfUpdateRate) {
    mfFPS = ((float)mlFramecounter) / mfFrametime;
    mlFramecounter = 0;
    mfFrametimestart = (float)cPlatform::GetApplicationTimeX();
  }
}

//////////////////////////////////////////////////////////////////////////
// SETUP VAR CONTAINER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cSetupVarContainer::cSetupVarContainer() { msBlank = ""; }

//-----------------------------------------------------------------------

void cSetupVarContainer::AddString(const tString &asName,
                                   const tString &asValue) {
  std::map<tString, tString>::value_type val(asName, asValue);
  m_mapVars.insert(val);
}

void cSetupVarContainer::AddInt(const tString &asName, int alValue) {
  AddString(asName, cString::ToString(alValue));
}
void cSetupVarContainer::AddFloat(const tString &asName, float afValue) {
  AddString(asName, cString::ToString(afValue));
}
void cSetupVarContainer::AddBool(const tString &asName, bool abValue) {
  AddString(asName, abValue ? "true" : "false");
}

//-----------------------------------------------------------------------

const tString &cSetupVarContainer::GetString(const tString &asName) {
  std::map<tString, tString>::iterator it = m_mapVars.find(asName);
  if (it == m_mapVars.end())
    return msBlank;
  else
    return it->second;
}

float cSetupVarContainer::GetFloat(const tString &asName, float afDefault) {
  const tString &sVal = GetString(asName);
  if (sVal == "")
    return afDefault;
  else
    return cString::ToFloat(sVal.c_str(), afDefault);
}
int cSetupVarContainer::GetInt(const tString &asName, int alDefault) {
  const tString &sVal = GetString(asName);
  if (sVal == "")
    return alDefault;
  else
    return cString::ToInt(sVal.c_str(), alDefault);
}
bool cSetupVarContainer::GetBool(const tString &asName, bool abDefault) {
  const tString &sVal = GetString(asName);
  if (sVal == "")
    return abDefault;
  else
    return cString::ToBool(sVal.c_str(), abDefault);
}

//////////////////////////////////////////////////////////////////////////
// GLOBAL FUCNTIONS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cEngine *CreateHPLEngine(eHplAPI aApi, tFlag alHplModuleFlags,
                         cEngineInitVars *apVars) {
  if (apVars == NULL)
    FatalError("CreateHPLEngine called with NULL init vars!\n");

  iLowLevelEngineSetup *pGameSetup = NULL;

  switch (aApi) {
  case eHplAPI_OpenGL:
    pGameSetup = hplNew(cSDLEngineSetup, (alHplModuleFlags));
    break;
  default:
    FatalError("CreateHPLEngine called with unknown API %d!\n", aApi);
  }

  return hplNew(cEngine, (pGameSetup, alHplModuleFlags, apVars));
}

//-----------------------------------------------------------------------

void DestroyHPLEngine(cEngine *apGame) { hplDelete(apGame); }

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cEngine::mbDevicePlugged = false;
bool cEngine::mbDeviceRemoved = false;

//-----------------------------------------------------------------------

cEngine::cEngine(iLowLevelEngineSetup *apGameSetup, tFlag alHplSetupFlags,
                 cEngineInitVars *apVars) {
  mvEngineTypeStrings.resize(eVariableType_LastEnum);
  mvEngineTypeStrings[eVariableType_Int] = "Int";
  mvEngineTypeStrings[eVariableType_Float] = "Float";
  mvEngineTypeStrings[eVariableType_Vec2] = "Vector2";
  mvEngineTypeStrings[eVariableType_Vec3] = "Vector3";
  mvEngineTypeStrings[eVariableType_Color] = "Color";
  mvEngineTypeStrings[eVariableType_String] = "String";
  mvEngineTypeStrings[eVariableType_Enum] = "Enum";
  mvEngineTypeStrings[eVariableType_Bool] = "Bool";

  // Registered before GameInit so code reached from the module Init
  // calls can see the engine (module pointers are still null until
  // their Create* runs). Unregistered at the top of ~cEngine.
  Interface<cEngine>::Register(this);

  GameInit(apGameSetup, alHplSetupFlags, apVars);
}

//-----------------------------------------------------------------------

void cEngine::GameInit(iLowLevelEngineSetup *apGameSetup, tFlag alHplSetupFlags,
                       cEngineInitVars *apVars) {
  mpGameSetup = apGameSetup;

  Log("Creating Engine Modules\n");
  Log("--------------------------------------------------------\n");

  // Create the modules that game connects to and init them!
  //  Each module registers with Interface<T> right after creation —
  //  before any of the Init calls below — so it is globally reachable
  //  from the moment it exists (cGraphics::Init and InitGlobalManagedSets
  //  already rely on Interface<cGraphics>::Get()). All are unregistered
  //  in one block at the very end of ~cEngine.
  Log(" Creating graphics module\n");
  mpGraphics = mpGameSetup->CreateGraphics();
  Interface<cGraphics>::Register(mpGraphics);
  // The window (owned by the engine setup, created before the cGraphics
  // ctor) is the single source of truth for the window/screen size.
  Interface<cWindow>::Register(mpGraphics->GetWindow());
  // Subscribe the window to the OS-event bus so it re-broadcasts its own
  // resize event (cEngine is already Interface-registered above).
  mpGraphics->GetWindow()->ConnectToEventBus();

  Log(" Creating system module\n");
  mpSystem = mpGameSetup->CreateSystem();
  Interface<cSystem>::Register(mpSystem);

  Log(" Creating resource module\n");
  mpResources = mpGameSetup->CreateResources(mpGraphics);
  Interface<cResources>::Register(mpResources);

  Log(" Creating input module\n");
  mpInput = mpGameSetup->CreateInput(mpGraphics);
  Interface<cInput>::Register(mpInput);
  // Subscribe the low-level input to the OS-event bus; it classifies device
  // events into its per-frame list for the keyboard/mouse/gamepad devices.
  mpInput->GetLowLevel()->ConnectToEventBus();

  Log(" Creating sound module\n");
  mpSound = mpGameSetup->CreateSound();
  Interface<cSound>::Register(mpSound);

  Log(" Creating physics module\n");
  mpPhysics = mpGameSetup->CreatePhysics();
  Interface<cPhysics>::Register(mpPhysics);

  Log(" Creating ai module\n");
  mpAI = mpGameSetup->CreateAI();
  Interface<cAI>::Register(mpAI);

  Log(" Creating gui module\n");
  mpGui = hplNew(cGui, ());
  Interface<cGui>::Register(mpGui);

  Log(" Creating haptic module\n");
#ifdef INCLUDE_HAPTIC
  mpHaptic = mpGameSetup->CreateHaptic();
  if (mpHaptic)
    Interface<cHaptic>::Register(mpHaptic);
#else
  mpHaptic = NULL;
#endif

  Log(" Creating scene module\n");
  mpScene =
      mpGameSetup->CreateScene(mpGraphics, mpResources, mpSound, mpPhysics,
                               mpSystem, mpAI, mpGui, mpHaptic);
  Interface<cScene>::Register(mpScene);

  Log("--------------------------------------------------------\n\n");

  // Init the resources
  mpResources->Init(mpGraphics, mpSystem, mpSound, mpScene, mpGui, mpPhysics);

  // Init the graphics
  mpGraphics->Init(apVars->mGraphics, mpResources, alHplSetupFlags);

  // Init Sound
  mpSound->Init(mpResources, apVars->mSound);

  // Init physics
  mpPhysics->Init(mpResources);

  // Init AI
  mpAI->Init();

  // Init Gui
  mpGui->Init(mpResources, mpGraphics, mpSound, mpScene, mpInput);

  // Init haptic
  if (mpHaptic)
    mpHaptic->Init(mpResources);

  Log("Initializing Game Module\n");
  Log("--------------------------------------------------------\n");
  // Create the updatehandler
  Log(" Adding engine updates\n");
  mpUpdater = hplNew(cUpdater, (mpSystem->GetLowLevel()));
  Interface<cUpdater>::Register(mpUpdater);

  // Add some loaded modules to the updater
  mpUpdater->AddGlobalUpdate(mpInput);
  mpUpdater->AddGlobalUpdate(mpPhysics);
  mpUpdater->AddGlobalUpdate(mpScene);
  mpUpdater->AddGlobalUpdate(mpGraphics);
  mpUpdater->AddGlobalUpdate(mpSound);
  mpUpdater->AddGlobalUpdate(mpAI);
  mpUpdater->AddGlobalUpdate(mpGui);
  mpUpdater->AddGlobalUpdate(mpResources);
  if (mpHaptic)
    mpUpdater->AddGlobalUpdate(mpHaptic);

  // Setup the "default" updater container
  mpUpdater->AddContainer("Default");
  mpUpdater->SetContainer("Default");

  // Create the logic timer.
  mpLogicTimer = mpSystem->CreateLogicTimer(apVars->mGame.mlUpdateRate);

  // Init some standard script funcs
  Log(" Initializing script functions\n");
  cScriptFuncs::Init(mpGraphics, mpResources, mpSystem, mpInput, mpScene,
                     mpSound, this);

  mpMutex = cPlatform::CreateMutEx();

  mpFPSCounter = hplNew(cFPSCounter, (mpSystem->GetLowLevel()));
  mpFrameTimer = cPlatform::CreateTimer();
  Log("--------------------------------------------------------\n\n");

  Log("User Initialization\n");
  Log("--------------------------------------------------------\n");
}

//-----------------------------------------------------------------------

cEngine::~cEngine() {
  Log("--------------------------------------------------------\n\n");

  // Capture the window pointer while cGraphics is still alive; the deletes
  // below free cGraphics (and the setup that owns the window), so the
  // UnRegister block can only pass this dangling value (never deref it).
  cWindow *pWindow = mpGraphics ? mpGraphics->GetWindow() : nullptr;

  hplDelete(mpLogicTimer);
  hplDelete(mpFPSCounter);
  hplDelete(mpFrameTimer);
  hplDelete(mpMutex);

  hplDelete(mpUpdater);

  hplDelete(mpGui);
  hplDelete(mpScene);
  if (mpHaptic)
    hplDelete(mpHaptic);
  hplDelete(mpInput);
  hplDelete(mpSound);
  // Two-phase graphics teardown around cResources:
  // Phase 1 destroys the render objects (materials, renderers, managed
  // sets) and drains the deferral queue — this needs the cResources
  // managers alive, since drained pins call their manager's FreeResource.
  mpGraphics->DestroyRenderObjects();
  // The manager destructors free their remaining GPU resources through
  // Interface<cGraphics>::Get()->device and push final deferrals into
  // graphicsDefer — cGraphics must still be alive and registered here.
  hplDelete(mpResources);
  // Phase 2 (~cGraphics -> Dispose): drain those final deferrals, then
  // tear down the swapchain, uploader, device, and RI instance.
  hplDelete(mpGraphics);
  hplDelete(mpPhysics);
  hplDelete(mpAI);
  hplDelete(mpSystem);

  Log(" Deleting game setup provided by user\n");
  hplDelete(mpGameSetup);

  // Unregister everything only now, after all deletes: unregistering
  // earlier would break teardown — GPU-resource destructors that run
  // inside ~cResources, ~cGraphics and the other module deletes reach
  // the device via Interface<cGraphics>::Get(). The stored pointers are
  // dangling at this point, but UnRegister only compares the pointer
  // value (never dereferences); the calls exist so a subsequent engine
  // instance can register cleanly.
  if (mpHaptic)
    Interface<cHaptic>::UnRegister(mpHaptic);
  Interface<cUpdater>::UnRegister(mpUpdater);
  Interface<cScene>::UnRegister(mpScene);
  Interface<cGui>::UnRegister(mpGui);
  Interface<cAI>::UnRegister(mpAI);
  Interface<cPhysics>::UnRegister(mpPhysics);
  Interface<cSound>::UnRegister(mpSound);
  Interface<cInput>::UnRegister(mpInput);
  Interface<cResources>::UnRegister(mpResources);
  Interface<cSystem>::UnRegister(mpSystem);
  Interface<cWindow>::UnRegister(pWindow);
  Interface<cGraphics>::UnRegister(mpGraphics);
  Interface<cEngine>::UnRegister(this);

  Log("HPL Exit was successful!\n");
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHOD
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

int glClearUpdateCheck = 0;

// Per-iteration sleep used by the "throttle when out of focus" background mode.
// Small enough to keep the MCP drain / live view responsive, large enough to
// stop the loop busy-spinning the CPU while the window is unfocused.
static const int kBackgroundThrottleSleepMs = 5;

void cEngine::Run() {
  // Log line that ends user init.
  Log("--------------------------------------------------------\n\n");

  double fNumOfTimes = 0;
  double fMediumTime = 0;

  mpUpdater->BroadcastMessageToAll(eUpdateableMessage_OnStart);

  mpLogicTimer->Reset();

  // Loop the game... fix the var...
  double fTempTime = cPlatform::GetApplicationTimeX();

  Log("Game Running\n");
  Log("--------------------------------------------------------\n");

  mfFrameTime = 0;
  mpFrameTimer->Start();

  bool bIsUpdated = true;
  bool bBufferSwap = false;
  bool bSwappedOnce = false;

  // cMemoryManager::SetLogCreation(true);

  mpGraphics->BeginActiveSet();
  while (!GetGameIsDone()) {
    //////////////////////////
    // Drain the OS event queue first thing so resize/quit/input are
    // serviced once per rendered frame, then broadcast on the bus.
    PumpEvents();
    cWindow *pWindow = mpGraphics->GetWindow();

    bool bHasInputFocus = true;
    bool bCanRender = true;
    if (mbWaitIfAppOutOfFocus) {
      bHasInputFocus = pWindow->HasInputFocus();
      bCanRender = pWindow->IsVisible() && pWindow->IsMinimized() == false;
    }

    // Hard pause: fully stop the loop until input focus returns.
    if (mbWaitIfAppOutOfFocus) {
      while (pWindow->HasInputFocus() == false) {
        cPlatform::Sleep(100);
        PumpEvents();
        mpInput->Update(1.0f / 10.0f);
      }
    }

    //////////////////////////
    // Check if paused
    if (GetPaused()) {
      cPlatform::Sleep(10);
      mpInput->Update(1.0f / 100.0f);
      bIsUpdated = true;

      mpUpdater->RunMessage(eUpdateableMessage_OnPauseUpdate, 1.0f / 100.0f);
    } else {
      //////////////////////////
      // Update logic.
      while (mpLogicTimer->WantUpdate() && !GetGameIsDone()) {
        /////////////////////////////////////////////
        // Run Update callback in updater
        mpUpdater->RunMessage(eUpdateableMessage_PreUpdate, GetStepSize());
        mpUpdater->RunMessage(eUpdateableMessage_Update, GetStepSize());
        mpUpdater->RunMessage(eUpdateableMessage_PostUpdate, GetStepSize());
        bIsUpdated = true;

        if (mpInput->isQuitMessagePosted()) {

          mpUpdater->RunMessage(eUpdateableMessage_OnQuit);

          mpInput->resetQuitMessagePosted();
        }

        /////////////////////////////////////////////
        // Run Check if any application focus changed
        CheckAndBroadcastFocusChange();

        /////////////////////////////////////////////
        // Run Check if devices were plugged/unplugged
        CheckAndBroadcastDeviceChange();

        /////////////////////////////////////////////
        // If log update is active, clear it regularly.
        if (GetUpdateLogActive()) {
          glClearUpdateCheck++;
          if (glClearUpdateCheck % 20 == 0) {
            if (mpUpdater->GetCurrentContainerName() == "Default")
              ClearUpdateLogFile();
          }
        }

        // Increase game time.
        mfGameTime += GetStepSize();
      }
      mpLogicTimer->EndUpdateLoop();
    }

    // if(GetGameIsDone()) Log("1\n");

    ////////////////////////////////////
    // If rendering once, make a check and if already drawn screen, just
    // continue
    if (mbRenderOnce && bSwappedOnce) {
      continue;
    }

    // if(GetGameIsDone()) Log("2\n");

    ////////////////////////////////////////////////
    // Swap buffers and call callback, do this after update, so hardware can
    // work during update
    if (bBufferSwap) {
      bBufferSwap = false;

      // Log("Swap done: %d\n", cPlatform::GetApplicationTime());
      mpGraphics->CloseAndSubmitActiveSet();
      mpUpdater->RunMessage(eUpdateableMessage_OnPostBufferSwap);
      mpGraphics->BeginActiveSet();
      bSwappedOnce = true;
      if (mbRenderOnce)
        continue;
    }

    ////////////////////////////////////
    // Render frame
    // While throttling, behave as if FPS were limited (render only on update
    // frames). Skip entirely when the window can't be presented to (minimized).
    if (bCanRender && (mbLimitFPS == false || bIsUpdated)) {
      // Get the the from the last frame.
      UpdateFrameTimer();

      // On draw callback sending that to gui, etc
      START_TIMING(OnDraw)
      mpUpdater->RunMessage(eUpdateableMessage_OnDraw, mfFrameTime);
      STOP_TIMING(OnDraw)

      // Render this frame
      START_TIMING(RenderAll)
      mpScene->Render(mfFrameTime, tSceneRenderFlag_All);
      STOP_TIMING(RenderAll)

      START_TIMING(PostRender)
      mpUpdater->RunMessage(eUpdateableMessage_OnPostRender, mfFrameTime);
      STOP_TIMING(PostRender)

      START_TIMING(FlushRender)
      // IMPORTANT: CHECK IF THIS IS PROBLEMATIC
      STOP_TIMING(FlushRender)

      // Update fps counter.
      mpFPSCounter->AddFrame();

      fNumOfTimes++;
      bIsUpdated = false;
      bBufferSwap = true;
    }

    // if(GetGameIsDone()) Log("4\n");
  }
  Log("--------------------------------------------------------\n\n");

  Log("Statistics\n");
  Log("--------------------------------------------------------\n");

  double fTime = cPlatform::GetApplicationTimeX() - fTempTime;
  fMediumTime = fNumOfTimes / fTime;

  Log(" Medium framerate: %f\n", fMediumTime);
  Log("--------------------------------------------------------\n\n");

  Log("User Exit\n");
  Log("--------------------------------------------------------\n");

  mpUpdater->BroadcastMessageToAll(eUpdateableMessage_OnExit);
}
//-----------------------------------------------------------------------

void cEngine::Exit() {
  mpMutex->Lock();

  mbGameIsDone = true;
  // Log("Exit!\n");

  mpMutex->Unlock();
}

bool cEngine::GetGameIsDone() {
  mpMutex->Lock();

  bool bDone = mbGameIsDone;

  mpMutex->Unlock();

  return bDone;
}
//-----------------------------------------------------------------------

void cEngine::ResetLogicTimer() { mpLogicTimer->Reset(); }

void cEngine::SetUpdatesPerSec(int alUpdatesPerSec) {
  mpLogicTimer->SetUpdatesPerSec(alUpdatesPerSec);
}

int cEngine::GetUpdatesPerSec() { return mpLogicTimer->GetUpdatesPerSec(); }

float cEngine::GetStepSize() { return mpLogicTimer->GetStepSize(); }

//-----------------------------------------------------------------------

float cEngine::GetFPS() { return mpFPSCounter->mfFPS; }

//-----------------------------------------------------------------------

void cEngine::SetSpeedMul(float afX) { mpLogicTimer->SetSpeedMul(afX); }

//-----------------------------------------------------------------------

float cEngine::GetAvgFrameTimeInMS() {
  return (1.0f / mpFPSCounter->mfFPS) * 1000.0f;
}

//-----------------------------------------------------------------------

void cEngine::SetFPSUpdateRate(float afSec) {
  mpFPSCounter->mfUpdateRate = afSec;
}
float cEngine::GetFPSUpdateRate() { return mpFPSCounter->mfUpdateRate; }

//-----------------------------------------------------------------------

cScriptVar *cEngine::CreateLocalVar(const tString &asName) {
  cScriptVar *pVar;
  pVar = GetLocalVar(asName);
  if (pVar == NULL) {
    cScriptVar Var;
    Var.msName = asName;
    m_mapLocalVars.insert(
        tScriptVarMap::value_type(cString::ToLowerCase(asName), Var));
    pVar = GetLocalVar(asName);
    if (pVar == NULL)
      FatalError("Very strange error when creating script var!\n");
  }
  return pVar;
}

//-----------------------------------------------------------------------

cScriptVar *cEngine::GetLocalVar(const tString &asName) {
  tScriptVarMapIt it = m_mapLocalVars.find(cString::ToLowerCase(asName));
  if (it == m_mapLocalVars.end())
    return NULL;

  return &it->second;
}

//-----------------------------------------------------------------------

tScriptVarMap *cEngine::GetLocalVarMap() { return &m_mapLocalVars; }

//-----------------------------------------------------------------------

cScriptVar *cEngine::CreateGlobalVar(const tString &asName) {
  cScriptVar *pVar;
  pVar = GetGlobalVar(asName);
  if (pVar == NULL) {
    cScriptVar Var;
    Var.msName = asName;
    m_mapGlobalVars.insert(
        tScriptVarMap::value_type(cString::ToLowerCase(asName), Var));
    pVar = GetGlobalVar(asName);
    if (pVar == NULL)
      FatalError("Very strange error when creating script var!\n");
  }
  return pVar;
}

//-----------------------------------------------------------------------

cScriptVar *cEngine::GetGlobalVar(const tString &asName) {
  tScriptVarMapIt it = m_mapGlobalVars.find(cString::ToLowerCase(asName));
  if (it == m_mapGlobalVars.end())
    return NULL;

  return &it->second;
}

//-----------------------------------------------------------------------

tScriptVarMap *cEngine::GetGlobalVarMap() { return &m_mapGlobalVars; }

//-----------------------------------------------------------------------

void cEngine::ClearAllVariables() {
  m_mapGlobalVars.clear();
  m_mapLocalVars.clear();
}

//-----------------------------------------------------------------------

eVariableType cEngine::GetEngineTypeFromString(const tString &asType) {
  for (int i = 0; i < (int)mvEngineTypeStrings.size(); ++i) {
    if (asType == mvEngineTypeStrings[i])
      return (eVariableType)i;
  }

  return eVariableType_LastEnum;
}

eVariableType cEngine::GetEngineTypeFromStringW(const tWString &asType) {
  return GetEngineTypeFromString(cString::To8Char(asType));
}

//-----------------------------------------------------------------------

void cEngine::SetPaused(bool abPaused) {
  mpMutex->Lock();

  if (mbPaused != abPaused) {
    mbPaused = abPaused;
    if (mbPaused == false)
      mpLogicTimer->Reset();
  }

  mpMutex->Unlock();
}

//-----------------------------------------------------------------------

bool cEngine::GetPaused() {
  mpMutex->Lock();

  bool bRet = mbPaused;

  mpMutex->Unlock();

  return bRet;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHOD
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cEngine::UpdateFrameTimer() {
  mpFrameTimer->Stop();
  mfFrameTime = (float)mpFrameTimer->GetTimeInSec();

  /*mlstFrameTimes.push_back(mpFrameTimer->GetTimeInMilliSec());
  if((int)mlstFrameTimes.size() >= mlMaxFrameTimes)
  {
          double fFrameSum=0.0;
          for(tDoubleListIt it = mlstFrameTimes.begin(); it !=
  mlstFrameTimes.end(); it++)
          {
                  fFrameSum += *it;
          }
          mfAvgFrameTimeInMS = (float)(fFrameSum /
  (double)mlstFrameTimes.size()); mlstFrameTimes.clear();
  }*/

  mpFrameTimer->Start();
}

//-----------------------------------------------------------------------

void cEngine::CheckAndBroadcastFocusChange() {
  bool bHadInputFocus = mbApplicationHasInputFocus;
  bool bHadMouseFocus = mbApplicationHasMouseFocus;
  bool bHadVisibility = mbApplicationIsVisible;

  cWindow *pWindow = mpGraphics->GetWindow();
  mbApplicationHasInputFocus = pWindow->HasInputFocus();
  mbApplicationHasMouseFocus = pWindow->HasMouseFocus();
  mbApplicationIsVisible = pWindow->IsVisible();

  if (bHadInputFocus && !mbApplicationHasInputFocus)
    mpUpdater->RunMessage(eUpdateableMessage_AppLostInputFocus);
  if (!bHadInputFocus && mbApplicationHasInputFocus)
    mpUpdater->RunMessage(eUpdateableMessage_AppGotInputFocus);

  if (bHadMouseFocus && !mbApplicationHasMouseFocus)
    mpUpdater->RunMessage(eUpdateableMessage_AppLostMouseFocus);
  if (!bHadMouseFocus && mbApplicationHasMouseFocus)
    mpUpdater->RunMessage(eUpdateableMessage_AppGotMouseFocus);

  if (bHadVisibility && !mbApplicationIsVisible)
    mpUpdater->RunMessage(eUpdateableMessage_AppLostVisibility);
  if (!bHadVisibility && mbApplicationIsVisible)
    mpUpdater->RunMessage(eUpdateableMessage_AppGotVisibility);
}

void cEngine::CheckAndBroadcastDeviceChange() {
  if (mbDevicePlugged) {
    mbDevicePlugged = false;
    mpUpdater->RunMessage(eUpdateableMessage_AppDeviceWasPlugged);
  }

  if (mbDeviceRemoved) {
    mbDeviceRemoved = false;
    mpUpdater->RunMessage(eUpdateableMessage_AppDeviceWasRemoved);
  }
}

//-----------------------------------------------------------------------

void cEngine::PumpEvents() {
  SDL_Event sdlEvent;
  while (SDL_PollEvent(&sdlEvent) != 0) {
    mOnSDLEvent.Signal(sdlEvent);
  }
}

} // namespace hpl
