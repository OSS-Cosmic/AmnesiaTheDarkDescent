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
	// The editor shows both halves of a Standard/ray-traced light pair, but
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
	return mbLightsVisible && cEditorHelper::GetVisibilityTypeState(eEditorVisibilityType_Lights);
}

//------------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// RAY-TRACED OVERRIDE REGISTRATION
//////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

void iEntityWrapperTypeLight::AddReduxLink(eVariableType aType, int alBaseId, int alID, const tString& asName)
{
	cLightReduxLink link;
	link.mType = aType;
	link.mlBaseId = alBaseId;
	link.mlReduxId = alID + LightReduxPropIdOffset;
	link.mlSetId = mlNextReduxSetId++;
	link.msReduxName = tString(hpl::kLightOverrideAttributePrefix) + asName;
	mvReduxLinks.push_back(link);
}

//------------------------------------------------------------------------------

// Value first, flag second -- see the comment on these helpers in the header.
void iEntityWrapperTypeLight::AddLightFloat(int alID, const tString& asName, float afDefault)
{
	AddFloat(alID, asName, afDefault);
	if(SupportsReduxOverrides()==false) return;
	AddReduxLink(eVariableType_Float, alID, alID, asName);
	const cLightReduxLink& link = mvReduxLinks.back();
	AddFloat(link.mlReduxId, link.msReduxName, afDefault);
	AddBool(link.mlSetId, link.msReduxName + "_Set", false, ePropCopyStep_PostEnt, false);
}

void iEntityWrapperTypeLight::AddLightBool(int alID, const tString& asName, bool abDefault)
{
	AddBool(alID, asName, abDefault);
	if(SupportsReduxOverrides()==false) return;
	AddReduxLink(eVariableType_Bool, alID, alID, asName);
	const cLightReduxLink& link = mvReduxLinks.back();
	AddBool(link.mlReduxId, link.msReduxName, abDefault);
	AddBool(link.mlSetId, link.msReduxName + "_Set", false, ePropCopyStep_PostEnt, false);
}

void iEntityWrapperTypeLight::AddLightString(int alID, const tString& asName, const tString& asDefault)
{
	AddString(alID, asName, asDefault);
	if(SupportsReduxOverrides()==false) return;
	AddReduxLink(eVariableType_String, alID, alID, asName);
	const cLightReduxLink& link = mvReduxLinks.back();
	AddString(link.mlReduxId, link.msReduxName, asDefault);
	AddBool(link.mlSetId, link.msReduxName + "_Set", false, ePropCopyStep_PostEnt, false);
}

void iEntityWrapperTypeLight::AddLightColor(int alID, const tString& asName, const cColor& aDefault)
{
	AddColor(alID, asName, aDefault);
	if(SupportsReduxOverrides()==false) return;
	AddReduxLink(eVariableType_Color, alID, alID, asName);
	const cLightReduxLink& link = mvReduxLinks.back();
	AddColor(link.mlReduxId, link.msReduxName, aDefault);
	AddBool(link.mlSetId, link.msReduxName + "_Set", false, ePropCopyStep_PostEnt, false);
}

// Intensity, the reach, SourceRadius and FlickerOffIntensity exist only for the
// ray-traced backend, so they get no retail half to fall back on.
void iEntityWrapperTypeLight::AddReduxOnlyFloat(int alID, const tString& asName, float afDefault)
{
	AddReduxLink(eVariableType_Float, -1, alID, asName);
	const cLightReduxLink& link = mvReduxLinks.back();
	AddFloat(link.mlReduxId, link.msReduxName, afDefault);
	AddBool(link.mlSetId, link.msReduxName + "_Set", false, ePropCopyStep_PostEnt, false);
}

//------------------------------------------------------------------------------

const cLightReduxLink* iEntityWrapperTypeLight::GetReduxLinkByReduxId(eVariableType aType, int alID)
{
	for(size_t i=0; i<mvReduxLinks.size(); ++i)
		if(mvReduxLinks[i].mlReduxId==alID && mvReduxLinks[i].mType==aType)
			return &mvReduxLinks[i];
	return NULL;
}

int iEntityWrapperTypeLight::GetReduxSetId(eVariableType aType, int alBaseId)
{
	for(size_t i=0; i<mvReduxLinks.size(); ++i)
	{
		const cLightReduxLink& link = mvReduxLinks[i];
		if(link.mType!=aType) continue;
		const int lKey = link.mlBaseId>=0 ? link.mlBaseId : link.mlReduxId - LightReduxPropIdOffset;
		if(lKey==alBaseId) return link.mlSetId;
	}
	return -1;
}

//------------------------------------------------------------------------------

const cLightReduxLink* iEntityWrapperTypeLight::GetReduxLinkBySetId(int alID)
{
	for(size_t i=0; i<mvReduxLinks.size(); ++i)
		if(mvReduxLinks[i].mlSetId==alID)
			return &mvReduxLinks[i];
	return NULL;
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

	iEntityWrapperTypeLight* pType = static_cast<iEntityWrapperTypeLight*>(mpType);

	// Presence in the file IS the flag: a Re_ attribute that is not written is
	// not authored, and the light falls back to its promoted retail value.
	const tLightReduxLinkVec& vLinks = pType->GetReduxLinks();
	for(size_t i=0; i<vLinks.size(); ++i)
		SetBool(vLinks[i].mlSetId, apElement->Attribute(vLinks[i].msReduxName.c_str())!=NULL);

	// Same rule as cEngineFileLoading::LoadLight: with no authored reach the
	// light reaches as far as its intensity and colour carry it. The area light
	// spells that reach "Radius", every other shape spells it "Re_Radius".
	if(pType->IsRayTracedOnly())
	{
		const bool bRadiusDerived = apElement->Attribute("Radius")==NULL;
		SetBool(eLightBool_RadiusDerived, bRadiusDerived);
		if(bRadiusDerived)
		{
			const cColor color = GetColor(eLightCol_Diffuse);
			SetFloat(eLightFloat_Radius, hpl::DeriveLightReach(GetFloat(eLightFloat_Intensity), color.r, color.g, color.b));
		}
		SetInt(eObjInt_RendererMask, GetInt(eObjInt_RendererMask) & static_cast<int>(hpl::kRendererMaskRayTraced));
	}
	else if(pType->SupportsReduxOverrides())
	{
		SetBool(eLightBool_RadiusDerived, apElement->Attribute("Re_Radius")==NULL);
	}

	return bRet;
}

//------------------------------------------------------------------------------

bool iEntityWrapperDataLight::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	bool bRet = iEntityWrapperData::SaveSpecific(apElement);

	iEntityWrapperTypeLight* pType = static_cast<iEntityWrapperTypeLight*>(mpType);

	// Strip every override the light does not actually author, so a light that
	// was never given Redux values round-trips exactly as it came in.
	const tLightReduxLinkVec& vLinks = pType->GetReduxLinks();
	for(size_t i=0; i<vLinks.size(); ++i)
	{
		if(GetBool(vLinks[i].mlSetId)==false)
			apElement->DeleteAttribute(vLinks[i].msReduxName.c_str());
	}

	// A derived reach stays out of the file so it keeps following the intensity.
	if(GetBool(eLightBool_RadiusDerived))
		apElement->DeleteAttribute(pType->IsRayTracedOnly() ? "Radius" : "Re_Radius");

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

//------------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// RAY-TRACED OVERRIDES
//////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

bool iEntityWrapperLight::HasReduxOverride(eVariableType aType, int alBaseId)
{
	switch(aType)
	{
	case eVariableType_Float:	return mmapReduxFloat.find(alBaseId)!=mmapReduxFloat.end();
	case eVariableType_Bool:	return mmapReduxBool.find(alBaseId)!=mmapReduxBool.end();
	case eVariableType_String:	return mmapReduxStr.find(alBaseId)!=mmapReduxStr.end();
	case eVariableType_Color:	return mmapReduxCol.find(alBaseId)!=mmapReduxCol.end();
	default:					return false;
	}
}

void iEntityWrapperLight::ClearReduxOverride(eVariableType aType, int alBaseId)
{
	switch(aType)
	{
	case eVariableType_Float:	mmapReduxFloat.erase(alBaseId); break;
	case eVariableType_Bool:	mmapReduxBool.erase(alBaseId); break;
	case eVariableType_String:	mmapReduxStr.erase(alBaseId); break;
	case eVariableType_Color:	mmapReduxCol.erase(alBaseId); break;
	default: break;
	}
}

bool iEntityWrapperLight::HasAnyReduxOverride()
{
	return mmapReduxFloat.empty()==false || mmapReduxBool.empty()==false ||
		   mmapReduxStr.empty()==false || mmapReduxCol.empty()==false;
}

//------------------------------------------------------------------------------

// What the ray-traced backend uses when nothing is authored: the retail values,
// promoted exactly as PromoteLegacyLightParameters does.
float iEntityWrapperLight::GetEffectiveIntensity()
{
	if(IsRayTracedOnly()) return mfIntensity;
	std::map<int,float>::iterator it = mmapReduxFloat.find(eLightFloat_Intensity);
	return it!=mmapReduxFloat.end() ? it->second : mfRadius;
}

float iEntityWrapperLight::GetEffectiveReach()
{
	if(IsRayTracedOnly()) return mfRadius;
	std::map<int,float>::iterator it = mmapReduxFloat.find(eLightFloat_Radius);
	if(it!=mmapReduxFloat.end()) return it->second;
	const cColor col = GetEffectiveDiffuseColor();
	return hpl::DeriveLightReach(GetEffectiveIntensity(), col.r, col.g, col.b);
}

float iEntityWrapperLight::GetEffectiveSourceRadius()
{
	if(IsRayTracedOnly()) return mfSourceRadius;
	std::map<int,float>::iterator it = mmapReduxFloat.find(eLightFloat_SourceRadius);
	return it!=mmapReduxFloat.end() ? it->second : 0.0f;
}

float iEntityWrapperLight::GetEffectiveFlickerOffValue()
{
	if(IsRayTracedOnly()) return mfFlickerOffIntensity;
	std::map<int,float>::iterator it = mmapReduxFloat.find(eLightFloat_FlickerOffIntensity);
	return it!=mmapReduxFloat.end() ? it->second : mfFlickerOffRadius;
}

cColor iEntityWrapperLight::GetEffectiveDiffuseColor()
{
	if(UsesRayTracedLightClass())
	{
		std::map<int,cColor>::iterator it = mmapReduxCol.find(eLightCol_Diffuse);
		if(it!=mmapReduxCol.end()) return it->second;
	}
	return mcolDiffuseColor;
}

//------------------------------------------------------------------------------

// The value an unauthored override shows: the retail half for a shared
// property, the promoted value for one the retail schema does not have.
float iEntityWrapperLight::GetInheritedReduxFloat(const cLightReduxLink& aLink)
{
	if(aLink.mlBaseId>=0)
	{
		float fValue = 0.0f;
		GetProperty(aLink.mlBaseId, fValue);
		return fValue;
	}

	switch(aLink.mlReduxId - LightReduxPropIdOffset)
	{
	case eLightFloat_Intensity:				return GetEffectiveIntensity();
	case eLightFloat_Radius:				return GetEffectiveReach();
	case eLightFloat_SourceRadius:			return GetEffectiveSourceRadius();
	case eLightFloat_FlickerOffIntensity:	return GetEffectiveFlickerOffValue();
	default:								return 0.0f;
	}
}

//------------------------------------------------------------------------------

// Every Get/SetProperty overload runs this first. An override id reads the
// authored value when there is one and the inherited value otherwise; a "_Set"
// flag id reads and writes whether the override is authored at all.
bool iEntityWrapperLight::GetReduxProperty(int alPropID, eVariableType aType, void* apOut)
{
	iEntityWrapperTypeLight* pType = static_cast<iEntityWrapperTypeLight*>(mpType);

	if(aType==eVariableType_Bool)
	{
		const cLightReduxLink* pFlag = pType->GetReduxLinkBySetId(alPropID);
		if(pFlag)
		{
			*static_cast<bool*>(apOut) = HasReduxOverride(pFlag->mType, pFlag->mlBaseId>=0
				? pFlag->mlBaseId : pFlag->mlReduxId - LightReduxPropIdOffset);
			return true;
		}
	}

	const cLightReduxLink* pLink = pType->GetReduxLinkByReduxId(aType, alPropID);
	if(pLink==NULL) return false;

	const int lKey = pLink->mlBaseId>=0 ? pLink->mlBaseId : pLink->mlReduxId - LightReduxPropIdOffset;
	switch(aType)
	{
	case eVariableType_Float:
		{
			std::map<int,float>::iterator it = mmapReduxFloat.find(lKey);
			*static_cast<float*>(apOut) = it!=mmapReduxFloat.end() ? it->second
																  : GetInheritedReduxFloat(*pLink);
			break;
		}
	case eVariableType_Bool:
		{
			std::map<int,bool>::iterator it = mmapReduxBool.find(lKey);
			if(it!=mmapReduxBool.end()) *static_cast<bool*>(apOut) = it->second;
			else GetProperty(pLink->mlBaseId, *static_cast<bool*>(apOut));
			break;
		}
	case eVariableType_String:
		{
			std::map<int,tString>::iterator it = mmapReduxStr.find(lKey);
			if(it!=mmapReduxStr.end()) *static_cast<tString*>(apOut) = it->second;
			else GetProperty(pLink->mlBaseId, *static_cast<tString*>(apOut));
			break;
		}
	case eVariableType_Color:
		{
			std::map<int,cColor>::iterator it = mmapReduxCol.find(lKey);
			if(it!=mmapReduxCol.end()) *static_cast<cColor*>(apOut) = it->second;
			else GetProperty(pLink->mlBaseId, *static_cast<cColor*>(apOut));
			break;
		}
	default:
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------

bool iEntityWrapperLight::SetReduxProperty(int alPropID, eVariableType aType, const void* apValue)
{
	iEntityWrapperTypeLight* pType = static_cast<iEntityWrapperTypeLight*>(mpType);

	if(aType==eVariableType_Bool)
	{
		const cLightReduxLink* pFlag = pType->GetReduxLinkBySetId(alPropID);
		if(pFlag)
		{
			const int lKey = pFlag->mlBaseId>=0 ? pFlag->mlBaseId
												: pFlag->mlReduxId - LightReduxPropIdOffset;
			if(*static_cast<const bool*>(apValue))
			{
				// Seed a freshly enabled override with the value it inherits, so
				// turning it on changes nothing until something is edited.
				if(HasReduxOverride(pFlag->mType, lKey)==false)
				{
					switch(pFlag->mType)
					{
					case eVariableType_Float:	mmapReduxFloat[lKey] = GetInheritedReduxFloat(*pFlag); break;
					case eVariableType_Bool:	{ bool b=false; GetProperty(pFlag->mlBaseId,b); mmapReduxBool[lKey]=b; break; }
					case eVariableType_String:	{ tString v; GetProperty(pFlag->mlBaseId,v); mmapReduxStr[lKey]=v; break; }
					case eVariableType_Color:	{ cColor c; GetProperty(pFlag->mlBaseId,c); mmapReduxCol[lKey]=c; break; }
					default: break;
					}
				}
			}
			else
			{
				ClearReduxOverride(pFlag->mType, lKey);
			}
			ApplyLightValues();
			return true;
		}
	}

	const cLightReduxLink* pLink = pType->GetReduxLinkByReduxId(aType, alPropID);
	if(pLink==NULL) return false;

	const int lKey = pLink->mlBaseId>=0 ? pLink->mlBaseId : pLink->mlReduxId - LightReduxPropIdOffset;
	switch(aType)
	{
	case eVariableType_Float:
		mmapReduxFloat[lKey] = *static_cast<const float*>(apValue);
		// Authoring a reach stops it following the intensity, as in the loader.
		if(pLink->mlBaseId<0 && (pLink->mlReduxId - LightReduxPropIdOffset)==eLightFloat_Radius)
			mbRadiusDerived = false;
		break;
	case eVariableType_Bool:	mmapReduxBool[lKey] = *static_cast<const bool*>(apValue); break;
	case eVariableType_String:	mmapReduxStr[lKey] = *static_cast<const tString*>(apValue); break;
	case eVariableType_Color:	mmapReduxCol[lKey] = *static_cast<const cColor*>(apValue); break;
	default:					return false;
	}
	ApplyLightValues();
	return true;
}

//------------------------------------------------------------------------------

bool iEntityWrapperLight::GetProperty(int alPropID, cColor& aX)
{
	if(GetReduxProperty(alPropID, eVariableType_Color, &aX)) return true;

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
	if(GetReduxProperty(alPropID, eVariableType_Float, &afX)) return true;

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
	if(GetReduxProperty(alPropID, eVariableType_String, &asX)) return true;

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
	if(GetReduxProperty(alPropID, eVariableType_Bool, &abX)) return true;

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
	if(SetReduxProperty(alPropID, eVariableType_Color, &aX)) return true;

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
	if(SetReduxProperty(alPropID, eVariableType_Float, &afX)) return true;

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
	if(SetReduxProperty(alPropID, eVariableType_String, &asX)) return true;

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
	if(SetReduxProperty(alPropID, eVariableType_Bool, &abX)) return true;

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

bool iEntityWrapperLight::UsesRayTracedLightClass()
{
	// The area light has no legacy class; the box light has no ray-traced one.
	if(IsRayTracedOnly()) return true;
	if(static_cast<iEntityWrapperTypeLight*>(mpType)->IsStandardOnly()) return false;
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
	// Cool once the light carries ray-traced overrides, warm while both backends
	// read the same authored values.
	cColor col = HasAnyReduxOverride() ? cColor(0.45f, 0.7f, 1.0f, 1.0f)
									   : cColor(1.0f, 0.75f, 0.4f, 1.0f);
	if(IsLitByEditorRenderer()==false)
	{
		col.r *= 0.4f; col.g *= 0.4f; col.b *= 0.4f;
	}
	return col;
}

//------------------------------------------------------------------------------

// The engine light carries BOTH backends' tuning, so the editor writes both and
// lets the world's backend decide which one is previewed. That also means
// toggling the editor's backend re-lights the viewport without a reload.
void iEntityWrapperLight::ApplyLightValues()
{
	iLight* pLight = (iLight*)mpEngineEntity->GetEntity();

	if(IsRayTracedOnly())
	{
		// The area light has no retail half: its plain values ARE the
		// ray-traced ones.
		if(mbRadiusDerived)
			mfRadius = hpl::DeriveLightReach(mfIntensity, mcolDiffuseColor.r, mcolDiffuseColor.g, mcolDiffuseColor.b);

		hpl::cLightTuningState rayTraced;
		rayTraced.mfIntensity = mfIntensity;
		rayTraced.mfOnValue = mfIntensity;
		rayTraced.mfReach = mfRadius;
		rayTraced.mfSourceRadius = mfSourceRadius;
		rayTraced.mbReachFollowsIntensity = mbRadiusDerived;
		rayTraced.mfOffValue = mfFlickerOffIntensity;
		rayTraced.mDiffuseColor = mcolDiffuseColor;
		rayTraced.mDefaultDiffuseColor = mcolDiffuseColor;
		rayTraced.mbCastShadows = mbCastShadows;
		rayTraced.mbAuthored = true;
		pLight->SetTuning(hpl::eLightModel_RayTraced, rayTraced);

		hpl::cLightTuningState absent;
		absent.mbPresent = false;
		pLight->SetTuning(hpl::eLightModel_Legacy, absent);
		return;
	}

	// Retail half: what the Standard renderer reads.
	hpl::cLightTuningState standard;
	standard.mfReach = mfRadius;
	standard.mfOnValue = mfRadius;
	standard.mfIntensity = mfRadius;
	standard.mfOffValue = mfFlickerOffRadius;
	standard.mDiffuseColor = mcolDiffuseColor;
	standard.mDefaultDiffuseColor = mcolDiffuseColor;
	standard.mbCastShadows = mbCastShadows;
	standard.mbAuthored = true;
	pLight->SetTuning(hpl::eLightModel_Legacy, standard);

	if(static_cast<iEntityWrapperTypeLight*>(mpType)->IsStandardOnly())
	{
		// A box light has no ray-traced representation at all.
		hpl::cLightTuningState absent;
		absent.mbPresent = false;
		pLight->SetTuning(hpl::eLightModel_RayTraced, absent);
		return;
	}

	// Redux half: the authored Re_ values where the light has them, otherwise
	// the retail radius promoted exactly as the loader promotes it.
	hpl::cLightTuningState rayTraced;
	rayTraced.mfIntensity = GetEffectiveIntensity();
	rayTraced.mfOnValue = rayTraced.mfIntensity;
	rayTraced.mfReach = GetEffectiveReach();
	rayTraced.mfSourceRadius = GetEffectiveSourceRadius();
	rayTraced.mbReachFollowsIntensity =
		HasReduxOverride(eVariableType_Float, eLightFloat_Radius)==false;
	rayTraced.mfOffValue = GetEffectiveFlickerOffValue();
	rayTraced.mDiffuseColor = GetEffectiveDiffuseColor();
	rayTraced.mDefaultDiffuseColor = rayTraced.mDiffuseColor;
	rayTraced.mbCastShadows = mbCastShadows;
	rayTraced.mbAuthored = HasAnyReduxOverride();
	pLight->SetTuning(hpl::eLightModel_RayTraced, rayTraced);
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
	// Only the area light is pinned to one renderer; a merged light loads on
	// both and picks its values from the backend in play.
	iEntityWrapper::SetRendererMask(IsRayTracedOnly() ? (alMask & static_cast<int>(hpl::kRendererMaskRayTraced)) : alMask);
}

//------------------------------------------------------------------------------

void iEntityWrapperLight::SetRadiusDerived(bool abX)
{
	mbRadiusDerived = abX && (IsRayTracedOnly() || SupportsReduxOverrides());

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
