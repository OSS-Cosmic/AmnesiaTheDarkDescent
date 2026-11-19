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

#ifndef HPL_ENGINE_H
#define HPL_ENGINE_H

#include "engine/EngineTypes.h"
#include "system/Event.h"
#include "system/SystemTypes.h"

// SDL's event union, forward-declared so the raw-event bus can be a cEngine
// member without pulling SDL into this widely-included header (Engine.cpp
// includes the full SDL). SDL_Event lives at global scope.
union SDL_Event;

namespace hpl {

class cUpdater;
class iLowLevelEngineSetup;
class iLowLevelSystem;
class cLogicTimer;

class cSystem;
class cInput;
class cResources;
class cGraphics;
class cScene;
class cSound;
class cPhysics;
class cAI;
class cHaptic;
class cGui;
class cEngine;
class cEngineInitVars;
class iTimer;
class iMutex;

//------------------------------------------------------

class cFPSCounter {
public:
  cFPSCounter(iLowLevelSystem *apLowLevelSystem);

  void AddFrame();

  float mfFPS;
  float mfUpdateRate;

private:
  iLowLevelSystem *mpLowLevelSystem;
  int mlFramecounter;
  float mfFrametimestart;
  float mfFrametime;
};

//---------------------------------------------------

class cSetupVarContainer {
public:
  cSetupVarContainer();

  void AddString(const tString &asName, const tString &asValue);

  void AddInt(const tString &asName, int alValue);
  void AddFloat(const tString &asName, float afValue);
  void AddBool(const tString &asName, bool abValue);

  const tString &GetString(const tString &asName);

  float GetFloat(const tString &asName, float afDefault);
  int GetInt(const tString &asName, int alDefault);
  bool GetBool(const tString &asName, bool abDefault);

private:
  std::map<tString, tString> m_mapVars;
  tString msBlank;
};

//---------------------------------------------------

extern cEngine *CreateHPLEngine(eHplAPI aApi, tFlag alHplModuleFlags,
                                cEngineInitVars *apVars);
extern void DestroyHPLEngine(cEngine *apGame);

//---------------------------------------------------

class cEngine {
public:
  cEngine(iLowLevelEngineSetup *apGameSetup, tFlag alHplSetupFlags,
          cEngineInitVars *apVars);
  ~cEngine();

private:
  void GameInit(iLowLevelEngineSetup *apGameSetup, tFlag alHplSetupFlags,
                cEngineInitVars *apVars);

public:
  /**
   * Starts the game loop. To make stuff run they must be added as updatables..
   */
  void Run();
  /**
   * Exists the game.
   * \todo is this a good way to do it? Should game be global. If so, make a
   * singleton.
   */
  void Exit();
  bool GetGameIsDone();

  cScene *GetScene() { return mpScene; }
  cResources *GetResources() { return mpResources; }
  cUpdater *GetUpdater() { return mpUpdater; }
  cSystem *GetSystem() { return mpSystem; }
  cInput *GetInput() { return mpInput; }
  cGraphics *GetGraphics() { return mpGraphics; }
  cSound *GetSound() { return mpSound; }
  cPhysics *GetPhysics() { return mpPhysics; }
  cAI *GetAI() { return mpAI; }
  cGui *GetGui() { return mpGui; }
  cHaptic *GetHaptic() { return mpHaptic; }

  void ResetLogicTimer();
  void SetUpdatesPerSec(int alUpdatesPerSec);
  int GetUpdatesPerSec();
  float GetStepSize();

  cLogicTimer *GetLogicTimer() { return mpLogicTimer; }

  void SetSpeedMul(float afX);

  float GetFPS();
  float GetAvgFrameTimeInMS();

  void SetFPSUpdateRate(float afSec);
  float GetFPSUpdateRate();

  void SetRenderOnce(bool abX) { mbRenderOnce = abX; }

  float GetFrameTime() { return mfFrameTime; }

  double GetGameTime() { return mfGameTime; }

  void SetLimitFPS(bool abX) { mbLimitFPS = abX; }
  bool GetLimitFPS() { return mbLimitFPS; }

  /**
   * How far (0-1) the current frame is between the last two logic ticks.
   * Rendering shows the world interpolated by this amount.
   */
  double GetRenderAlpha() { return mdRenderAlpha; }

  /**
   * Logic ticks still to run this frame, counting the one running now (1
   * outside of ticks). With a low frame rate several ticks run back to back
   * per frame, input that is read once per frame can be spread over them.
   */
  int GetLogicTicksLeftThisFrame() {
    int lLeft = mlTicksThisFrame - mlUpdatesOnCurrentFrame;
    return lLeft > 0 ? lLeft : 1;
  }

  /**
   * Optional frame rate cap (0 = off). Helps frame pacing when running far
   * above the refresh rate without vsync.
   */
  void SetFrameRateLimit(int alFPS);
  int GetFrameRateLimit() { return mlFrameRateLimit; }

  void SetWaitIfAppOutOfFocus(bool abX) { mbWaitIfAppOutOfFocus = abX; }
  bool GetWaitIfAppOutOfFocus() { return mbWaitIfAppOutOfFocus; }

  void SetPaused(bool abPaused);
  bool GetPaused();

  static void SetDeviceWasPlugged() { mbDevicePlugged = true; }
  static void SetDeviceWasRemoved() { mbDeviceRemoved = true; }

  // Raw OS-event bus. cEngine::Run pumps SDL first thing each frame and
  // Signals every event here; consumers subscribe (the low-level input
  // classifies device events; each cWindow filters for its own window and
  // re-broadcasts a typed resize). This is the single OS-event source, so
  // it extends cleanly to multiple windows.
  Event<const SDL_Event &> &OnSDLEvent() { return mOnSDLEvent; }

  ///// SCRIPT VAR METHODS ////////////////////

  cScriptVar *CreateLocalVar(const tString &asName);
  cScriptVar *GetLocalVar(const tString &asName);
  tScriptVarMap *GetLocalVarMap();
  cScriptVar *CreateGlobalVar(const tString &asName);
  cScriptVar *GetGlobalVar(const tString &asName);
  tScriptVarMap *GetGlobalVarMap();

  void ClearAllVariables();

  ////// ENGINE VARIABLES /////////////////////

  eVariableType GetEngineTypeFromString(const tString &asType);
  eVariableType GetEngineTypeFromStringW(const tWString &asType);
  const tString &GetEngineTypeString(eVariableType aType) {
    return mvEngineTypeStrings[aType];
  }

private:
  void UpdateFrameTimer();
  void WaitForFrameLimit();

  void CheckAndBroadcastFocusChange();
  void CheckAndBroadcastDeviceChange();

  // Drains the SDL event queue and Signals mOnSDLEvent for each event.
  // Called first thing every frame in Run() (and in the out-of-focus wait
  // loop) so the OS queue is serviced once per rendered frame.
  void PumpEvents();

  Event<const SDL_Event &> mOnSDLEvent;

  bool mbGameIsDone = false;

  bool mbRenderOnce = false;

  bool mbPaused = false;

  float mfFrameTime = 0;

  double mfGameTime = 0;

  iLowLevelEngineSetup *mpGameSetup = nullptr;
  cUpdater *mpUpdater = nullptr;
  cLogicTimer *mpLogicTimer = nullptr;

  iMutex *mpMutex = nullptr;

  cFPSCounter *mpFPSCounter = nullptr;

  iTimer *mpFrameTimer = nullptr;

  bool mbLimitFPS = true;

  // Fixed timestep loop state
  double mdAccumulator = 0;
  double mdCurrentTime = 0;
  double mdRenderAlpha = 1;
  int mlTicksThisFrame = 0;
  int mlUpdatesOnCurrentFrame = 0;

  int mlFrameRateLimit = 0;
  double mdFrameLimitPeriod = 0;
  double mdNextFrameDeadline = 0;

  tScriptVarMap m_mapLocalVars;
  tScriptVarMap m_mapGlobalVars;

  bool mbApplicationHasInputFocus = false;
  bool mbApplicationHasMouseFocus = false;
  bool mbApplicationIsVisible = false;

  bool mbWaitIfAppOutOfFocus = false;

  static bool mbDevicePlugged;
  static bool mbDeviceRemoved;

  tStringVec mvEngineTypeStrings;

  // Modules that Game connnect to:
  cResources *mpResources = nullptr;
  cSystem *mpSystem = nullptr;
  cInput *mpInput = nullptr;
  cGraphics *mpGraphics = nullptr;
  cScene *mpScene = nullptr;
  cSound *mpSound = nullptr;
  cPhysics *mpPhysics = nullptr;
  cAI *mpAI = nullptr;
  cHaptic *mpHaptic = nullptr;
  cGui *mpGui = nullptr;
};

}; // namespace hpl
#endif // HPL_ENGINE_H
