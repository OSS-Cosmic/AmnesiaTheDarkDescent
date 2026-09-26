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

#include "graphics/MaterialType_BasicSolid.h"

#include "system/LowLevelSystem.h"
#include "system/PreprocessParser.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"

namespace hpl {

//////////////////////////////////////////////////////////////////////////
// STATIC OBJECTS
//////////////////////////////////////////////////////////////////////////

bool iMaterialType_SolidBase::mbGlobalDataCreated = false;
float iMaterialType_SolidBase::mfVirtualPositionAddScale = 0.03f;

//////////////////////////////////////////////////////////////////////////
// SOLID BASE
//////////////////////////////////////////////////////////////////////////

iMaterialType_SolidBase::iMaterialType_SolidBase(cGraphics *apGraphics,
                                                 cResources *apResources)
    : iMaterialType(apGraphics, apResources) {
  mbIsGlobalDataCreator = false;
}

iMaterialType_SolidBase::~iMaterialType_SolidBase() {}

void iMaterialType_SolidBase::LoadData() {
  //////////////
  // Create textures
  // The dissolve map is a dither table sampled at LOD 0; match the false
  // used in GlobalManagedSets.cpp, since the path-keyed cache makes a
  // disagreement here a first-loader-wins coin flip.
  mpDissolveTexture = mpResources->GetTextureManager()
                          ->Create2DImage("core_dissolve.tga", false)
                          .Release();
}

//////////////////////////////////////////////////////////////////////////
// SOLID DIFFUSE
//////////////////////////////////////////////////////////////////////////

cMaterialType_SolidDiffuse::cMaterialType_SolidDiffuse(cGraphics *apGraphics,
                                                       cResources *apResources)
    : iMaterialType_SolidBase(apGraphics, apResources) {
  AddUsedTexture(eMaterialTexture_Diffuse);
  AddUsedTexture(eMaterialTexture_NMap);
  AddUsedTexture(eMaterialTexture_Alpha);
  AddUsedTexture(eMaterialTexture_Specular);
  AddUsedTexture(eMaterialTexture_Height);
  AddUsedTexture(eMaterialTexture_Illumination);
  AddUsedTexture(eMaterialTexture_DissolveAlpha);
  AddUsedTexture(eMaterialTexture_CubeMap);
  AddUsedTexture(eMaterialTexture_CubeMapAlpha);

  AddVarFloat("HeightMapScale", 0.05f, "");
  AddVarFloat("HeightMapBias", 0, "");
  AddVarFloat("FrenselBias", 0.2f,
              "Bias for Fresnel term. values: 0-1. Higher means that more of "
              "reflection is seen when looking straight at object.");
  AddVarFloat("FrenselPow", 8.0f,
              "The higher the 'sharper' the reflection is, meaning that it is "
              "only clearly seen at sharp angles.");
  AddVarBool("AlphaDissolveFilter", false,
             "If alpha values between 0 and 1 should be used and dissolve the "
             "texture. This can be useful for things like hair.");
}

cMaterialType_SolidDiffuse::~cMaterialType_SolidDiffuse() {}

void cMaterialType_SolidDiffuse::LoadVariables(cMaterial *apMaterial,
                                               cResourceVarsObject *apVars) {
  MaterialDiffuseSolid *pData =
      std::get_if<MaterialDiffuseSolid>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  pData->m_heightMapScale = apVars->GetVarFloat("HeightMapScale", 0.1f);
  pData->m_heightMapBias = apVars->GetVarFloat("HeightMapBias", 0);
  pData->m_frenselBias = apVars->GetVarFloat("FrenselBias", 0.2f);
  pData->m_frenselPow = apVars->GetVarFloat("FrenselPow", 8.0f);
  pData->m_alphaDissolveFilter =
      apVars->GetVarBool("AlphaDissolveFilter", false);

  // AlphaMode is derived (cMaterial::GetAlphaMode): a bound alpha texture makes
  // a SolidDiffuse material alpha-tested. Textures are bound before LoadVariables.
  apMaterial->IncreaseGeneration();
}

void cMaterialType_SolidDiffuse::GetVariableValues(
    cMaterial *apMaterial, cResourceVarsObject *apVars) {
  const MaterialDiffuseSolid *pData =
      std::get_if<MaterialDiffuseSolid>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  apVars->AddVarFloat("HeightMapScale", pData->m_heightMapScale);
  apVars->AddVarFloat("HeightMapBias", pData->m_heightMapBias);
  apVars->AddVarFloat("FrenselBias", pData->m_frenselBias);
  apVars->AddVarFloat("FrenselPow", pData->m_frenselPow);
  apVars->AddVarBool("AlphaDissolveFilter", pData->m_alphaDissolveFilter);
}
} // namespace hpl
