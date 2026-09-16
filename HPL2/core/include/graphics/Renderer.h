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

#ifndef HPL_RENDERER_H
#define HPL_RENDERER_H

#include "graphics/GraphicsTypes.h"
#include "graphics/TemporalUpscaler.h"
#include "math/MathTypes.h"
#include "scene/SceneTypes.h"

#include "graphics/Graphics.h"

namespace hpl {

//---------------------------------------------

class cGraphics;
class cResources;
class cEngine;
class iLowLevelResources;
class cMeshCreator;
class iRenderable;
class cWorld;
class cRenderSettings;
class cRenderList;
class iLight;
class cBoundingVolume;
class cViewport;
class cFrustum;

//---------------------------------------------

class cRenderSettings {
public:
  cRenderSettings(bool abIsReflection = false);
  ~cRenderSettings();

  ////////////////////////////
  // Helper methods
  void ResetVariables();
  void SetupReflectionSettings();
  void AddOcclusionPlane(const cPlanef &aPlane);
  void ResetOcclusionPlanes();

  ////////////////////////////
  // Data
  cRenderList *mpRenderList;
  cRenderSettings *mpReflectionSettings;

  ////////////////////////////
  // General settings
  bool mbLog;
  cColor mClearColor;

  ////////////////////////////
  // Render settings
  int mlMinimumObjectsBeforeOcclusionTesting;
  int mlSampleVisiblilityLimit;
  bool mbIsReflection;
  bool mbClipReflectionScreenRect;
  bool mbUseOcclusionCulling;
  bool mbUseEdgeSmooth;
  tPlanefVec mvOcclusionPlanes;
  bool mbUseCallbacks;
  eShadowMapResolution mMaxShadowMapResolution;
  bool mbUseScissorRect;
  cVector2l mvScissorRectPos;
  cVector2l mvScissorRectSize;
  bool mbRenderWorldReflection;

  // DESIRED temporal upscaler setting, pure data only. It never allocates
  // Vulkan resources; setting it from a menu callback must not create an SDK
  // context. The viewport turns it into an actual prepared provider later
  // in the frame.
  TemporalUpscalerSettings mTemporalUpscaler = {};

  ////////////////////////////
  // Shadow settings
  bool mbRenderShadows;
  float mfShadowMapBias;
  float mfShadowMapSlopeScaleBias;

  ////////////////////////////
  // Light settings
  bool mbSSAOActive;

  ////////////////////////////
  // Output
  int mlNumberOfLightsRendered;
  int mlNumberOfOcclusionQueries;
};

//---------------------------------------------

class iRenderer {
  friend class cRendererCallbackFunctions;
  friend class cRenderSettings;

public:
  iRenderer(const tString &asName, cGraphics *apGraphics,
            cResources *apResources);
  virtual ~iRenderer();

  void Update(float afTimeStep);

  virtual void Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) {}

  inline static int GetRenderFrameCount() { return mlRenderFrameCount; }
  inline static void IncRenderFrameCount() { ++mlRenderFrameCount; }

  static bool GetRefractionEnabled() { return mbRefractionEnabled; }
  static void SetRefractionEnabled(bool enabled) { mbRefractionEnabled = enabled; }
  // Legacy spotlight shadow filter: Low (single compare), Medium, High.
  static eShadowMapQuality GetShadowMapQuality() { return mShadowMapQuality; }
  static void SetShadowMapQuality(eShadowMapQuality quality) { mShadowMapQuality = quality; }

  float GetTimeCount() { return mfTimeCount; }

  virtual bool LoadData() = 0;
  virtual void DestroyData() = 0;

  // Legacy material-path accessors — still referenced by the
  // MaterialType_* program setup code; never populated by the Vulkan
  // Draw() path.
  cWorld *GetCurrentWorld() { return mpCurrentWorld; }
  cFrustum *GetCurrentFrustum() {
    return NULL;
  } // IMPORTANT: CHECK IF THIS IS PROBLEMATIC
  cRenderList *GetCurrentRenderList() { return mpCurrentRenderList; }

  // Temp variables used by material.
  float GetTempAlpha() { return mfTempAlpha; }

protected:
  cResources *mpResources;
  cGraphics *mpGraphics;

  tString msName;
  cWorld *mpCurrentWorld;
  cRenderSettings *mpCurrentSettings;
  cRenderList *mpCurrentRenderList;
  float mfTempAlpha;
  static int mlRenderFrameCount;
  static bool mbRefractionEnabled;
  static eShadowMapQuality mShadowMapQuality;
  float mfTimeCount;

  // Static variables
};
}; // namespace hpl
#endif // HPL_RENDERER_H
