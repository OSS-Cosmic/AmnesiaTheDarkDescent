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

#include "EntityWrapperLight.h"

#include "scene/LightParameters.h"

#include "EditorWorld.h"
#include "EditorClipPlane.h"
#include "EditorHelper.h"
#include "EditorBaseClasses.h"

#include "EditorWindowViewport.h"

#include "EditorWindowEntityEditBoxLight.h"

#include "EntityWrapperBillboard.h"

#include <tinyxml2.h>

//------------------------------------------------------------------------------

bool iEntityWrapperTypeLight::mbLightsVisible = true; 
bool iEntityWrapperTypeLight::mbLightsActive = true; 

//------------------------------------------------------------------------------

iIconEntityLight::iIconEntityLight(iEntityWrapper* apParent, const tString& asIconFile) : iIconEntity(apParent, "Light" + asIconFile)
{
}

iIconEntityLight::~iIconEntityLight()
{
	cWorld* pWorld = mpParent->GetEditorWorld()->GetWorld();
	if(mpEntity)
		pWorld->DestroyLight((iLight*)mpEntity);
}

//------------------------------------------------------------------------------

void iIconEntityLight::Update()
{
	iEditorWorld* pWorld = mpParent->GetEditorWorld();
	iLight* pLight = (iLight*)mpEntity;
	iEntityWrapperLight* pParent = (iEntityWrapperLight*)mpParent;
	// The editor shows both halves of a Standard/Overdrive light pair, but
	// only the half the editor renderer loads lights the scene.
	pLight->SetVisible(mpParent->IsVisible() && mpParent->IsActive() && mpParent->GetType()->IsActive() &&
					   pParent->IsLitByEditorRenderer());
	
	pParent->UpdateFlickerParams();

	tObjectVariabilityFlag lFlags =0;
	if(pParent->GetShadowsAffectDynamic())	lFlags |= eObjectVariabilityFlag_Dynamic;
	if(pParent->GetShadowsAffectStatic())	lFlags |= eObjectVariabilityFlag_Static;
	pLight->SetShadowCastersAffected(lFlags);
}

//------------------------------------------------------------------------------

void iEntityWrapperTypeLight::SetVisible(bool abX)
{
	if(mbLightsVisible==abX)
		return; 

	mbLightsVisible = abX; 
	mpWorld->SetVisibilityUpdated(); 
}

//------------------------------------------------------------------------------

bool iEntityWrapperTypeLight::IsVisible()
{
	const eEditorVisibilityType set = mbOverdrive ? eEditorVisibilityType_OverdriveLights
											  : eEditorVisibilityType_LegacyLights;
	return mbLightsVisible && cEditorHelper::GetVisibilityTypeState(set);
}

//------------------------------------------------------------------------------

void iEntityWrapperTypeLight::SetActive(bool abX)
{
	if(mbLightsActive==abX)
		return; 

	mbLightsActive = abX; 
	mpWorld->SetVisibilityUpdated(); 
}

//------------------------------------------------------------------------------

iEntityWrapperDataLight::iEntityWrapperDataLight(iEntityWrapperType* apType) : iEntityWrapperData(apType)
{
}

//------------------------------------------------------------------------------


void iEntityWrapperDataLight::CopyFromEntity(iEntityWrapper* apEntity)
{
	iEntityWrapperData::CopyFromEntity(apEntity);
	iEntityWrapperLight* pLight = (iEntityWrapperLight*)apEntity;

	tEntityWrapperList lstBBs;
	std::list<cEntityWrapperBillboard*>& lstConnectedBillboards = pLight->GetConnectedBillboards();
	lstBBs.insert(lstBBs.end(), lstConnectedBillboards.begin(), lstConnectedBillboards.end());

	cEditorHelper::GetIDsFromEntityList(lstBBs, mlstConnectedBBIds);
}


//------------------------------------------------------------------------------

void iEntityWrapperDataLight::CopyToEntity(iEntityWrapper* apEntity, int alCopyFlags)
{
	iEntityWrapperData::CopyToEntity(apEntity,alCopyFlags);
	iEntityWrapperLight* pLight = (iEntityWrapperLight*)apEntity;

	if(alCopyFlags==ePropCopyStep_PostDeployAll)
	{
		tIntListIt it = mlstConnectedBBIds.begin();
		for(;it!=mlstConnectedBBIds.end();++it)
		{
			int lID = *it;
			pLight->AddConnectedBillboard((cEntityWrapperBillboard*)mpType->GetWorld()->GetEntity(lID));
		}
	}
}

//------------------------------------------------------------------------------

bool iEntityWrapperDataLight::Load(tinyxml2::XMLElement* apElement)
{
	bool bRet = iEntityWrapperData::Load(apElement);

	// Same rules as cEngineFileLoading::LoadLight: an Overdrive light without a
	// Radius reaches as far as its intensity and colour carry, and it never
	// loads for the Standard renderer.
	if(static_cast<iEntityWrapperTypeLight*>(mpType)->IsOverdrive())
	{
		const bool bRadiusDerived = apElement->Attribute("Radius")==NULL;
		SetBool(eLightBool_RadiusDerived, bRadiusDerived);
		if(bRadiusDerived)
		{
			const cColor color = GetColor(eLightCol_Diffuse);
			SetFloat(eLightFloat_Radius, hpl::DeriveLightReach(GetFloat(eLightFloat_Intensity), color.r, color.g, color.b));
		}
		SetInt(eObjInt_RendererMask, GetInt(eObjInt_RendererMask) & static_cast<int>(hpl::kRendererMaskOverdrive));
	}

	return bRet;
}

//------------------------------------------------------------------------------

bool iEntityWrapperDataLight::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	bool bRet = iEntityWrapperData::SaveSpecific(apElement);

	// A derived reach stays out of the file so it keeps following the intensity.
	if(static_cast<iEntityWrapperTypeLight*>(mpType)->IsOverdrive() && GetBool(eLightBool_RadiusDerived))
		apElement->DeleteAttribute("Radius");

	return bRet;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
////////////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

iEntityWrapperLight::iEntityWrapperLight(iEntityWrapperData* apData) : iEntityWrapper(apData)
{
	mfIntensity = 1.0f;
	mfRadius = 1.0f;
	mfSourceRadius = 0.0f;
	mbRadiusDerived = false;
	mfFlickerOffRadius = 0.0f;
	mfFlickerOffIntensity = 0.0f;
	mcolDiffuseColor = cColor(1);
}

//------------------------------------------------------------------------------

iEntityWrapperLight::~iEntityWrapperLight()
{
	if(GetEditorWorld()->IsClearingEntities()==false)
	{
		std::list<cEntityWrapperBillboard*>::iterator it = mlstConnectedBBs.begin();
		for(;it!=mlstConnectedBBs.end();++it)
		{
			cEntityWrapperBillboard* pBB = *it;
			if(GetEditorWorld()->HasEntity(pBB))
			{
				pBB->SetConnectedLightName("");
				pBB->UpdateEntity();
			}
		}
		mlstConnectedBBs.clear();
	}

}

//------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
////////////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

bool iEntityWrapperLight::GetProperty(int alPropID, cColor& aX)
{
	if(iEntityWrapper::GetProperty(alPropID, aX))
		return true;

	switch(alPropID)
	{
	case eLightCol_Diffuse:
		aX = GetDiffuseColor();
		break;
	case eLightCol_FlickerOff:
		aX = GetFlickerOffColor();
		break;
	default:
		return false;
	}

	return true;
}

bool iEntityWrapperLight::GetProperty(int alPropID, float& afX)
{
	if(iEntityWrapper::GetProperty(alPropID, afX))
		return true;

	switch(alPropID)
	{
	case eLightFloat_Intensity:
		afX = GetIntensity();
		break;
	case eLightFloat_Radius:
		afX = GetRadius();
		break;
	case eLightFloat_SourceRadius:
		afX = GetSourceRadius();
		break;
	case eLightFloat_GoboAnimFrameTime:
		afX = GetGoboAnimFrameTime();
		break;
	case eLightFloat_FlickerOnMinLength:
		afX = GetFlickerOnMinLength();
		break;
	case eLightFloat_FlickerOnMaxLength:
		afX = GetFlickerOnMaxLength();
		break;
	case eLightFloat_FlickerOffMinLength:
		afX = GetFlickerOffMinLength();
		break;
	case eLightFloat_FlickerOffMaxLength:
		afX = GetFlickerOffMaxLength();
		break;
	case eLightFloat_FlickerOffRadius:
		afX = GetFlickerOffRadius();
		break;
	case eLightFloat_FlickerOffIntensity:
		afX = GetFlickerOffIntensity();
		break;
	case eLightFloat_FlickerOnFadeMinLength:
		afX = GetFlickerOnFadeMinLength();
		break;
	case eLightFloat_FlickerOnFadeMaxLength:
		afX = GetFlickerOnFadeMaxLength();
		break;
	case eLightFloat_FlickerOffFadeMinLength:
		afX = GetFlickerOffFadeMinLength();
		break;
	case eLightFloat_FlickerOffFadeMaxLength:
		afX = GetFlickerOffFadeMaxLength();
		break;
	default:
		return false;
	}


	return true;
}

bool iEntityWrapperLight::GetProperty(int alPropID, int& alX)
{
	return iEntityWrapper::GetProperty(alPropID, alX);
}

bool iEntityWrapperLight::GetProperty(int alPropID, tString& asX)
{
	switch(alPropID)
	{
	case eLightStr_Gobo:
		asX = GetGoboFilename();
		break;
	case eLightStr_GoboAnimMode:
		asX = GetGoboAnimMode();
		break;
	case eLightStr_FalloffMap:
		asX = GetFalloffMap();
		break;
	case eLightStr_FlickerOnSound:
		asX = GetFlickerOnSound();
		break;
	case eLightStr_FlickerOffSound:
		asX = GetFlickerOffSound();
		break;
	case eLightStr_FlickerOnPS:
		asX = GetFlickerOnPS();
		break;
	case eLightStr_FlickerOffPS:
		asX = GetFlickerOffPS();
		break;
	case eLightStr_ShadowResolution:
		asX = GetShadowResolution();
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, asX);
	}

	return true;
}

bool iEntityWrapperLight::GetProperty(int alPropID, bool& abX)
{
	switch(alPropID)
	{
	case eLightBool_FlickerActive:
		abX = GetFlickerActive();
		break;
	case eLightBool_FlickerFade:
		abX = GetFlickerFade();
		break;
	case eLightBool_CastShadows:
		abX = GetCastShadows();
		break;
	case eLightBool_ShadowsAffectStatic:
		abX = GetShadowsAffectStatic();
		break;
	case eLightBool_ShadowsAffectDynamic:
		abX = GetShadowsAffectDynamic();
		break;
	case eLightBool_RadiusDerived:
		abX = IsRadiusDerived();
		break;
	default:
		return iEntityWrapper::GetProperty(alPropID, abX);
	}

	return true;
}

bool iEntityWrapperLight::SetProperty(int alPropID, const cColor& aX)
{
	switch(alPropID)
	{
	case eLightCol_Diffuse:
		SetDiffuseColor(aX);
		break;
	case eLightCol_FlickerOff:
		SetFlickerOffColor(aX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, aX);
	}

	return true;
}

bool iEntityWrapperLight::SetProperty(int alPropID, const float& afX)
{
	switch(alPropID)
	{
	case eLightFloat_Intensity:
		SetIntensity(afX);
		break;
	case eLightFloat_Radius:
		SetRadius(afX);
		break;
	case eLightFloat_SourceRadius:
		SetSourceRadius(afX);
		break;
	case eLightFloat_GoboAnimFrameTime:
		SetGoboAnimFrameTime(afX);
		break;
	case eLightFloat_FlickerOnMinLength:
		SetFlickerOnMinLength(afX);
		break;
	case eLightFloat_FlickerOnMaxLength:
		SetFlickerOnMaxLength(afX);
		break;
	case eLightFloat_FlickerOffMinLength:
		SetFlickerOffMinLength(afX);
		break;
	case eLightFloat_FlickerOffMaxLength:
		SetFlickerOffMaxLength(afX);
		break;
	case eLightFloat_FlickerOffRadius:
		SetFlickerOffRadius(afX);
		break;
	case eLightFloat_FlickerOffIntensity:
		SetFlickerOffIntensity(afX);
		break;
	case eLightFloat_FlickerOnFadeMinLength:
		SetFlickerOnFadeMinLength(afX);
		break;
	case eLightFloat_FlickerOnFadeMaxLength:
		SetFlickerOnFadeMaxLength(afX);
		break;
	case eLightFloat_FlickerOffFadeMinLength:
		SetFlickerOffFadeMinLength(afX);
		break;
	case eLightFloat_FlickerOffFadeMaxLength:
		SetFlickerOffFadeMaxLength(afX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, afX);
	}

	return true;
}

bool iEntityWrapperLight::SetProperty(int alPropID, const int& aX)
{
	return iEntityWrapper::SetProperty(alPropID, aX);
}

bool iEntityWrapperLight::SetProperty(int alPropID, const tString& asX)
{
	switch(alPropID)
	{
	case eLightStr_Gobo:
		this->SetGobo(asX);
		break;
	case eLightStr_GoboAnimMode:
		SetGoboAnimMode(asX);
		break;
	case eLightStr_FalloffMap:
		SetFalloffMap(asX);
		break;
	case eLightStr_FlickerOnSound:
		SetFlickerOnSound(asX);
		break;
	case eLightStr_FlickerOffSound:
		SetFlickerOffSound(asX);
		break;
	case eLightStr_FlickerOnPS:
		SetFlickerOnPS(asX);
		break;
	case eLightStr_FlickerOffPS:
		SetFlickerOffPS(asX);
		break;
	case eLightStr_ShadowResolution:
		SetShadowResolution(asX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, asX);
	}


	return true;
}

bool iEntityWrapperLight::SetProperty(int alPropID, const bool& abX)
{
	switch(alPropID)
	{
	case eLightBool_FlickerActive:
		SetFlickerActive(abX);
		break;
	case eLightBool_FlickerFade:
		SetFlickerFade(abX);
		break;
	case eLightBool_CastShadows:
		SetCastShadows(abX);
		break;
	case eLightBool_ShadowsAffectStatic:
		SetShadowsAffectStatic(abX);
		break;
	case eLightBool_ShadowsAffectDynamic:
		SetShadowsAffectDynamic(abX);
		break;
	case eLightBool_RadiusDerived:
		SetRadiusDerived(abX);
		break;
	default:
		return iEntityWrapper::SetProperty(alPropID, abX);
	}

	return true;
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetCastShadows(bool abX)
{
	mbCastShadows = abX;

	((iLight*)mpEngineEntity->GetEntity())->SetCastShadows(abX);
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetShadowsAffectStatic(bool abX)
{
	mbShadowsAffectStatic = abX;
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetShadowsAffectDynamic(bool abX)
{
	mbShadowsAffectDynamic = abX;
}

//------------------------------------------------------------------------------

bool iEntityWrapperLight::UsesOverdriveLightClass()
{
	if(IsOverdrive()) return true;
	cWorld* pWorld = GetEditorWorld()->GetWorld();
	return pWorld->GetRendererBackend() != eRendererBackend_Standard;
}

//------------------------------------------------------------------------------

bool iEntityWrapperLight::IsLitByEditorRenderer()
{
	iEditorWorld* pWorld = GetEditorWorld();
	if(pWorld==NULL || pWorld->GetWorld()==NULL) return true;
	return (static_cast<unsigned>(GetRendererMask()) & pWorld->GetWorld()->GetRendererMaskBit()) != 0;
}

//------------------------------------------------------------------------------

cColor iEntityWrapperLight::GetIconTint()
{
	cColor col = IsOverdrive() ? cColor(0.45f, 0.7f, 1.0f, 1.0f) : cColor(1.0f, 0.75f, 0.4f, 1.0f);
	if(IsLitByEditorRenderer()==false)
	{
		col.r *= 0.4f; col.g *= 0.4f; col.b *= 0.4f;
	}
	return col;
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::ApplyLightValues()
{
	iLight* pLight = (iLight*)mpEngineEntity->GetEntity();

	if(IsOverdrive())
	{
		if(mbRadiusDerived)
			mfRadius = hpl::DeriveLightReach(mfIntensity, mcolDiffuseColor.r, mcolDiffuseColor.g, mcolDiffuseColor.b);
		pLight->SetReachFollowsIntensity(mbRadiusDerived);
		pLight->SetIntensity(mfIntensity);
		pLight->SetRadius(mfRadius);
		pLight->SetSourceRadius(mfSourceRadius);
	}
	else if(pLight->GetLightModel() == eLightModel_Overdrive)
	{
		// A legacy light previewed in an Overdrive editor is promoted the way
		// the game promotes it (PromoteLegacyLightParameters).
		pLight->SetIntensity(mfRadius);
		pLight->SetRadius(hpl::DeriveLightReach(mfRadius, mcolDiffuseColor.r, mcolDiffuseColor.g, mcolDiffuseColor.b));
		pLight->SetSourceRadius(0.0f);
		pLight->SetReachFollowsIntensity(true);
	}
	else
	{
		// SetRadius is virtual on the runtime light, so spotlights also rebuild
		// their frustum and bounding volume here.
		pLight->SetRadius(mfRadius);
	}
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetIntensity(float afIntensity)
{
	mfIntensity = afIntensity;

	ApplyLightValues();
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetRadius(float afRadius)
{
	mfRadius = afRadius;
	mbRadiusDerived = false;

	ApplyLightValues();
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetRendererMask(int alMask)
{
	iEntityWrapper::SetRendererMask(IsOverdrive() ? (alMask & static_cast<int>(hpl::kRendererMaskOverdrive)) : alMask);
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetRadiusDerived(bool abX)
{
	mbRadiusDerived = abX && IsOverdrive();

	ApplyLightValues();
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetSourceRadius(float afSourceRadius)
{
	mfSourceRadius = afSourceRadius;

	ApplyLightValues();
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetDiffuseColor(const cColor& aDiffuseColor)
{
	mcolDiffuseColor =  aDiffuseColor;

	((iLight*)mpEngineEntity->GetEntity())->SetDiffuseColor(mcolDiffuseColor);
	// A promoted legacy light's reach follows its colour.
	ApplyLightValues();
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetFlickerActive(bool abX)
{
	mbFlickerActive = abX;

	mbFlickerUpdated = true;
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetFlickerOnMinLength(float afX)
{
	mfFlickerOnMinLength = afX;
	if(mfFlickerOnMaxLength<afX)
		SetFlickerOnMaxLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOnMaxLength(float afX)
{
	mfFlickerOnMaxLength = afX;
	if(mfFlickerOnMinLength>afX)
		SetFlickerOnMinLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffMinLength(float afX)
{
	mfFlickerOffMinLength = afX;
	if(mfFlickerOffMaxLength<afX)
		SetFlickerOffMaxLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffMaxLength(float afX)
{
	mfFlickerOffMaxLength = afX;
	if(mfFlickerOffMinLength>afX)
		SetFlickerOffMinLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffRadius(float afX)
{
	mfFlickerOffRadius = afX;

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffIntensity(float afX)
{
	mfFlickerOffIntensity = afX;

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerFade(bool abX)
{
	mbFlickerFade = abX;

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOnFadeMinLength(float afX)
{
	mfFlickerOnFadeMinLength = afX;
	if(mfFlickerOnFadeMaxLength<afX)
		SetFlickerOnFadeMaxLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOnFadeMaxLength(float afX)
{
	mfFlickerOnFadeMaxLength = afX;
	if(mfFlickerOnFadeMinLength>afX)
		SetFlickerOnFadeMinLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffFadeMinLength(float afX)
{
	mfFlickerOffFadeMinLength = afX;
	if(mfFlickerOffFadeMaxLength<afX)
		SetFlickerOffFadeMaxLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffFadeMaxLength(float afX)
{
	mfFlickerOffFadeMaxLength = afX;
	if(mfFlickerOffFadeMinLength>afX)
		SetFlickerOffFadeMinLength(afX);

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffColor(const cColor& aCol)
{
	mcolFlickerOffColor = aCol;

	mbFlickerUpdated = true;
}


void iEntityWrapperLight::SetFlickerOnSound(const tString& asStr)
{
	msFlickerOnSound = asStr;

	mbFlickerUpdated=true;
}

void iEntityWrapperLight::SetFlickerOnPS(const tString& asStr)
{
	msFlickerOnPS = asStr;

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffSound(const tString& asStr)
{
	msFlickerOffSound = asStr;

	mbFlickerUpdated = true;
}

void iEntityWrapperLight::SetFlickerOffPS(const tString& asStr)
{
	msFlickerOffPS = asStr;

	mbFlickerUpdated = true;
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::UpdateFlickerParams()
{
	if(mbFlickerUpdated==false)
		return;

	mbFlickerUpdated = false;
	/////////////////////////////////////////////
	// Reset the animated value and color to the authored ones; SetFlicker
	// captures them as the on state.
	SetDiffuseColor(mcolDiffuseColor);

	iLight* pLight = (iLight*)mpEngineEntity->GetEntity();

    pLight->SetFlickerActive(mbFlickerActive);
	pLight->SetFlicker(mcolFlickerOffColor, GetFlickerOffValue(),
							mfFlickerOnMinLength, mfFlickerOnMaxLength, msFlickerOnSound, msFlickerOnPS,
							mfFlickerOffMinLength, mfFlickerOffMaxLength, msFlickerOffSound, msFlickerOffPS,
							mbFlickerFade, mfFlickerOnFadeMinLength, mfFlickerOnFadeMaxLength, mfFlickerOffFadeMinLength, mfFlickerOffFadeMaxLength);
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::AddConnectedBillboard(cEntityWrapperBillboard* apBB)
{
	if(apBB==NULL || apBB->GetEngineEntity()==NULL)return;

	std::list<cEntityWrapperBillboard*>::iterator it = find(mlstConnectedBBs.begin(), mlstConnectedBBs.end(), apBB);
	if(it!=mlstConnectedBBs.end()) return;

	// The connection is editor/serialization state only — the engine ignores it
	// (light-billboard color sync was a fake-bloom hack; real bloom replaced it).
	mlstConnectedBBs.push_back(apBB);
	apBB->SetConnectedLight(this);
}

void iEntityWrapperLight::RemoveConnectedBillboard(cEntityWrapperBillboard* apBB)
{
	if(apBB==NULL)return;

	mlstConnectedBBs.remove(apBB);
}

//------------------------------------------------------------------------------

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetFalloffMap(const tString& asFalloffMap)
{
	// The engine no longer consumes falloff maps (analytic falloff); load only
	// to validate the file and keep the attribute round-tripping in the .map.
	Image* pTex = NULL;

	cEditorHelper::LoadTextureResource(eEditorTextureResourceType_1D, asFalloffMap, &pTex);
	if(pTex)
	{
		msFalloffMap = cString::To8Char(GetEditorWorld()->GetEditor()->GetPathRelToWD(asFalloffMap));
		GetEditorWorld()->GetEditor()->GetEngine()->GetResources()->GetTextureManager()->Destroy(pTex);
	}
	else
		msFalloffMap = "";
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetGobo(const tString& asGoboFilename)
{
	Image* pTex = NULL;

	cEditorHelper::LoadTextureResource(eEditorTextureResourceType_2D, asGoboFilename, &pTex, msGoboAnimMode, mfGoboAnimFrameTime);
	if(pTex)
		msGoboFilename = asGoboFilename;
	else
		msGoboFilename = "";

	((iLight*)mpEngineEntity->GetEntity())->SetGoboTexture(pTex);	
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetGoboAnimMode(const tString& asX)
{
	msGoboAnimMode = asX;

	SetGobo(msGoboFilename);
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetGoboAnimFrameTime(float afX)
{
	mfGoboAnimFrameTime = afX;

	SetGobo(msGoboFilename);
}

//------------------------------------------------------------------------------

bool iEntityWrapperLight::EntitySpecificCheckCulled(cEditorClipPlane* apPlane)
{
	cBoundingVolume* pBV = mpEngineEntity->GetRenderBV();

	return apPlane->PointIsOnCullingSide(pBV->GetMin()) && apPlane->PointIsOnCullingSide(pBV->GetMax());
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelected, const cColor& aHighlightCol, const cColor& aDisabledCol)
{
	iEntityWrapper::Draw(apViewport, apFunctions, apEditMode, abIsSelected, aHighlightCol);

	if(IsActive() && abIsSelected)
	{
		DrawLightTypeSpecific(apViewport, apFunctions, apEditMode, abIsSelected);
	}
}

//------------------------------------------------------------------------------

cEditorWindowEntityEditBox* iEntityWrapperLight::CreateEditBox(cEditorEditModeSelect* apEditMode)
{
	cEditorWindowEntityEditBox* pEditBox = hplNew(cEditorWindowEntityEditBoxLight,(apEditMode,this));

	return pEditBox;
}

//------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
////////////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

void iEntityWrapperLight::OnSetActive(bool abX)
{
	mpEngineEntity->Update();
}
