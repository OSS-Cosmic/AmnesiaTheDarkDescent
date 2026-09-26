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

#include "graphics/MaterialType_Water.h"

#include "system/LowLevelSystem.h"
#include "system/PreprocessParser.h"

#include "resources/Resources.h"

#include "math/Math.h"
#include "math/Frustum.h"

#include "scene/World.h"

#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"
#include "graphics/Renderer.h"

namespace hpl {

//////////////////////////////////////////////////////////////////////////
// TRANSLUCENT
//////////////////////////////////////////////////////////////////////////

cMaterialType_Water::cMaterialType_Water(cGraphics *apGraphics,
                                         cResources *apResources)
    : iMaterialType(apGraphics, apResources) {
  mbIsTranslucent = true;

  AddUsedTexture(eMaterialTexture_Diffuse);
  AddUsedTexture(eMaterialTexture_NMap);
  AddUsedTexture(eMaterialTexture_CubeMap);

  AddVarFloat(
      "RefractionScale", 0.1f,
      "The amount reflection and refraction is offset by ripples in water.");
  AddVarFloat("FrenselBias", 0.2f,
              "Bias for Fresnel term. values: 0-1. Higher means that more of "
              "reflection is seen when looking straight at water.");
  AddVarFloat("FrenselPow", 8.0f,
              "The higher the 'sharper' the reflection is, meaning that it is "
              "only clearly seen at sharp angles.");
  AddVarFloat("WaveSpeed", 1.0f, "The speed of the waves.");
  AddVarFloat("WaveAmplitude", 1.0f, "The size of the waves.");
  AddVarFloat("WaveFreq", 1.0f, "The frequency of the waves.");
  AddVarFloat("ReflectionFadeStart", 0, "Where the reflection starts fading.");
  AddVarFloat("ReflectionFadeEnd", 0,
              "Where the reflection stops fading. 0 or less means no fading.");

  AddVarBool("HasReflection", true, "If a reflection should be shown or not.");
  AddVarBool("OcclusionCullWorldReflection", true,
             "If occlusion culling should be used on reflection.");
  AddVarBool("LargeSurface", true,
             "If the water will cover a large surface and will need special "
             "sorting when rendering other transperant objects.");
}

cMaterialType_Water::~cMaterialType_Water() {}

void cMaterialType_Water::LoadVariables(cMaterial *apMaterial,
                                        cResourceVarsObject *apVars) {
  MaterialWater *pData = std::get_if<MaterialWater>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  pData->m_hasReflection = apVars->GetVarBool("HasReflection", true);
  pData->m_refractionScale = apVars->GetVarFloat("RefractionScale", 0.1f);
  pData->m_frenselBias = apVars->GetVarFloat("FrenselBias", 0.2f);
  pData->m_frenselPow = apVars->GetVarFloat("FrenselPow", 8.0f);
  pData->m_reflectionFadeStart = apVars->GetVarFloat("ReflectionFadeStart", 0);
  pData->m_reflectionFadeEnd = apVars->GetVarFloat("ReflectionFadeEnd", 0);
  pData->m_waveSpeed = apVars->GetVarFloat("WaveSpeed", 1.0f);
  pData->m_waveAmplitude = apVars->GetVarFloat("WaveAmplitude", 1.0f);
  pData->m_waveFreq = apVars->GetVarFloat("WaveFreq", 1.0f);

  apMaterial->SetWorldReflectionOcclusionTest(
      apVars->GetVarBool("OcclusionCullWorldReflection", true));
  apMaterial->SetLargeTransperantSurface(
      apVars->GetVarBool("LargeSurface", false));

  /////////////////////////////////////
  // Pipeline-side settings derived from the global refraction toggle and the
  // bound cubemap (textures are bound before LoadVariables, so GetImage works).
  //if(iRenderer::GetRefractionEnabled())
  apMaterial->SetBlendMode(eMaterialBlendMode_None);
  //else
  //	apMaterial->SetBlendMode(eMaterialBlendMode_Mul);

  // HasRefraction() (water always refracts) and HasWorldReflection()
  // (m_hasReflection above + no bound cubemap) are derived from the data blob.
  apMaterial->IncreaseGeneration();
}

void cMaterialType_Water::GetVariableValues(cMaterial *apMaterial,
                                            cResourceVarsObject *apVars) {
  const MaterialWater *pData = std::get_if<MaterialWater>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  apVars->AddVarBool("HasReflection", pData->m_hasReflection);
  apVars->AddVarFloat("RefractionScale", pData->m_refractionScale);
  apVars->AddVarFloat("FrenselBias", pData->m_frenselBias);
  apVars->AddVarFloat("FrenselPow", pData->m_frenselPow);
  apVars->AddVarFloat("ReflectionFadeStart", pData->m_reflectionFadeStart);
  apVars->AddVarFloat("ReflectionFadeEnd", pData->m_reflectionFadeEnd);
  apVars->AddVarFloat("WaveSpeed", pData->m_waveSpeed);
  apVars->AddVarFloat("WaveAmplitude", pData->m_waveAmplitude);
  apVars->AddVarFloat("WaveFreq", pData->m_waveFreq);

  apVars->AddVarBool("OcclusionCullWorldReflection",
                     apMaterial->GetWorldReflectionOcclusionTest());
  apVars->AddVarBool("LargeSurface", apMaterial->GetLargeTransperantSurface());
}
} // namespace hpl
