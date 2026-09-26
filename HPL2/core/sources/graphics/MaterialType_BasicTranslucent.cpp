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

#include "graphics/MaterialType_BasicTranslucent.h"

#include "system/LowLevelSystem.h"
#include "system/PreprocessParser.h"
#include "system/String.h"

#include "resources/Resources.h"

#include "scene/World.h"
#include "scene/Light.h"

#include "math/Math.h"
#include "math/Frustum.h"

#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"

namespace hpl {

//////////////////////////////////////////////////////////////////////////
// TRANSLUCENT
//////////////////////////////////////////////////////////////////////////

cMaterialType_Translucent::cMaterialType_Translucent(cGraphics *apGraphics,
                                                     cResources *apResources)
    : iMaterialType(apGraphics, apResources) {
  mbIsTranslucent = true;

  AddUsedTexture(eMaterialTexture_Diffuse);
  AddUsedTexture(eMaterialTexture_NMap);
  AddUsedTexture(eMaterialTexture_CubeMap);
  AddUsedTexture(eMaterialTexture_CubeMapAlpha);

  AddVarBool("Refraction", false,
             "If the material has refraction (distortion of bg). Uses NMap "
             "and/or normals of mesh");
  AddVarBool("RefractionEdgeCheck", true,
             "If true, there is no bleeding with foreground objects, but takes "
             "some extra power.");
  AddVarBool("RefractionNormals", false,
             "If normals should be used when refracting. If no NMap is set "
             "this is forced true!");
  AddVarBool("RefractionNormals", false,
             "If normals should be used when refracting. If no NMap is set "
             "this is forced true!");
  AddVarFloat("RefractionScale", 0.1f,
              "The amount refraction offsets the background");
  AddVarFloat("FrenselBias", 0.2f,
              "Bias for Fresnel term. values: 0-1. Higher means that more of "
              "reflection is seen when looking straight at the surface.");
  AddVarFloat("FrenselPow", 8.0f,
              "The higher the 'sharper' the reflection is, meaning that it is "
              "only clearly seen at sharp angles.");
  AddVarFloat(
      "RimLightMul", 0.0f,
      "The amount of rim light based on the reflection. This gives an edge to "
      "the object. Values: 0 - inf (although 1.0f should be used for max)");
  AddVarFloat("RimLightPow", 8.0f, "The sharpness of the rim lighting.");
  AddVarBool("AffectedByLightLevel", false,
             "The the material alpha is affected by the light level.");
  AddVarBool("DiffuseIsMask", false,
             "Interpret diffuse red as a linear emission mask; vertex RGB "
             "supplies color.");
  AddVarBool("SmoothHalo", false,
             "Replace additive diffuse with a continuous white halo falloff, "
             "colored by vertex RGB.");
  AddVarBool("LitDiffuse", false,
             "Light alpha-blended translucent meshes with direct diffuse "
             "lighting and ray-traced shadows.");
  AddVarFloat("LitDiffuseScale", 1.0f,
              "Nonnegative linear diffuse-light multiplier for LitDiffuse "
              "meshes; does not change alpha or add ambient light.");
  AddVarFloat(
      "ParticleOpacityScale", 1.0f,
      "Particle opacity multiplier (0-1), independent of display gamma.");
  AddVarFloat("ParticleBrightnessScale", 1.0f,
              "Nonnegative linear particle brightness multiplier, independent "
              "of display gamma.");
}

cMaterialType_Translucent::~cMaterialType_Translucent() {}

void cMaterialType_Translucent::LoadVariables(cMaterial *apMaterial,
                                              cResourceVarsObject *apVars) {
  MaterialTranslucent *pData =
      std::get_if<MaterialTranslucent>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  pData->m_refraction = apVars->GetVarBool("Refraction", false);
  pData->m_refractionEdgeCheck =
      apVars->GetVarBool("RefractionEdgeCheck", true);
  pData->m_refractionNormals = apVars->GetVarBool("RefractionNormals", true);
  pData->m_refractionScale = apVars->GetVarFloat("RefractionScale", 1.0f);
  pData->m_frenselBias = apVars->GetVarFloat("FrenselBias", 0.2f);
  pData->m_frenselPow = apVars->GetVarFloat("FrenselPow", 8.0);
  pData->m_rimLightMul = apVars->GetVarFloat("RimLightMul", 0.0f);
  pData->m_rimLightPow = apVars->GetVarFloat("RimLightPow", 8.0f);
  pData->m_isAffectedByLightLevel =
      apVars->GetVarBool("AffectedByLightLevel", false);
  pData->m_diffuseIsMask = apVars->GetVarBool("DiffuseIsMask", false);
  pData->m_smoothHalo = apVars->GetVarBool("SmoothHalo", false);
  pData->m_litDiffuse = apVars->GetVarBool("LitDiffuse", false);
  pData->m_litDiffuseScale = apVars->GetVarFloat("LitDiffuseScale", 1.0f);
  pData->m_particleOpacityScale =
      apVars->GetVarFloat("ParticleOpacityScale", 1.0f);
  pData->m_particleBrightnessScale =
      apVars->GetVarFloat("ParticleBrightnessScale", 1.0f);

  // HasRefraction() is derived from the authored m_refraction above (read
  // CPU-side by HybridRenderer to route the refraction path / shader flags).
  apMaterial->IncreaseGeneration();
}

void cMaterialType_Translucent::GetVariableValues(cMaterial *apMaterial,
                                                  cResourceVarsObject *apVars) {
  const MaterialTranslucent *pData =
      std::get_if<MaterialTranslucent>(&apMaterial->Data());
  if (pData == nullptr)
    return;

  apVars->AddVarBool("Refraction", pData->m_refraction);
  apVars->AddVarBool("RefractionEdgeCheck", pData->m_refractionEdgeCheck);
  apVars->AddVarBool("RefractionNormals", pData->m_refractionNormals);
  apVars->AddVarFloat("RefractionScale", pData->m_refractionScale);
  apVars->AddVarFloat("FrenselBias", pData->m_frenselBias);
  apVars->AddVarFloat("FrenselPow", pData->m_frenselPow);
  apVars->AddVarFloat("RimLightMul", pData->m_rimLightMul);
  apVars->AddVarFloat("RimLightPow", pData->m_rimLightPow);
  apVars->AddVarBool("AffectedByLightLevel", pData->m_isAffectedByLightLevel);
  apVars->AddVarBool("DiffuseIsMask", pData->m_diffuseIsMask);
  apVars->AddVarBool("SmoothHalo", pData->m_smoothHalo);
  apVars->AddVarBool("LitDiffuse", pData->m_litDiffuse);
  apVars->AddVarFloat("LitDiffuseScale", pData->m_litDiffuseScale);
  apVars->AddVarFloat("ParticleOpacityScale", pData->m_particleOpacityScale);
  apVars->AddVarFloat("ParticleBrightnessScale",
                      pData->m_particleBrightnessScale);
}
} // namespace hpl
