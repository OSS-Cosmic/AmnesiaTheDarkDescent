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

#ifndef HPLEDITOR_ENTITY_WRAPPER_LIGHT_H
#define HPLEDITOR_ENTITY_WRAPPER_LIGHT_H

#include "EntityWrapper.h"
#include "EngineEntity.h"

namespace tinyxml2 { class XMLElement; }

class cEditorWindowViewport;

class cEntityWrapperBillboard;

//---------------------------------------------------------------------

class iIconEntityLight : public iIconEntity
{
public:
	iIconEntityLight(iEntityWrapper* apParent, const tString& asIconFile);
	~iIconEntityLight();

	void Update();
};

//---------------------------------------------------------------------

#define LightPropIdStart 60

inline bool IsEditorPointLightType(int alType)
{
	return alType==eEditorEntityLightType_Point || alType==eEditorEntityLightType_OverdrivePoint;
}

inline bool IsEditorSpotLightType(int alType)
{
	return alType==eEditorEntityLightType_Spot || alType==eEditorEntityLightType_OverdriveSpot;
}

//////////////////////////////////////////////
// General Light properties
enum eLightBool
{
	eLightBool_FlickerActive = LightPropIdStart,
	eLightBool_FlickerFade,
	eLightBool_CastShadows,
	eLightBool_ShadowsAffectStatic,
	eLightBool_ShadowsAffectDynamic,
	eLightBool_RadiusDerived,     // Redux lights: no Radius in the file, reach follows intensity

	eLightBool_LastEnum,
};

enum eLightCol
{
	eLightCol_Diffuse = LightPropIdStart,
	eLightCol_FlickerOff,

	eLightCol_LastEnum,
};

enum eLightFloat
{
	eLightFloat_Intensity = LightPropIdStart, // Overdrive lights only
	eLightFloat_Radius,                       // legacy radius, or the Overdrive reach
	eLightFloat_SourceRadius,                 // Overdrive lights only

	eLightFloat_GoboAnimFrameTime,
	eLightFloat_FlickerOnMinLength,
	eLightFloat_FlickerOnMaxLength,
	eLightFloat_FlickerOffMinLength,
	eLightFloat_FlickerOffMaxLength,
	eLightFloat_FlickerOffRadius,             // legacy lights only
	eLightFloat_FlickerOnFadeMinLength,
	eLightFloat_FlickerOnFadeMaxLength,
	eLightFloat_FlickerOffFadeMinLength,
	eLightFloat_FlickerOffFadeMaxLength,
	eLightFloat_FlickerOffIntensity,          // Overdrive lights only

	eLightFloat_LastEnum,
};

enum eLightStr
{
	eLightStr_Gobo = LightPropIdStart,
	eLightStr_GoboAnimMode,
	eLightStr_FalloffMap,
	eLightStr_FlickerOnSound,
	eLightStr_FlickerOffSound,
	eLightStr_FlickerOnPS,
	eLightStr_FlickerOffPS,
	eLightStr_ShadowResolution,

	eLightStr_LastEnum,
};

//---------------------------------------------------------------------

class iEntityWrapperDataLight : public iEntityWrapperData
{
public:
	iEntityWrapperDataLight(iEntityWrapperType* apType);

	void CopyFromEntity(iEntityWrapper* apEntity);
	void CopyToEntity(iEntityWrapper* apEntity, int alCopyFlags);

	bool Load(tinyxml2::XMLElement* apElement);
	bool SaveSpecific(tinyxml2::XMLElement* apElement);

	const tIntList& GetConnectedBBIDS();
protected:
	tIntList mlstConnectedBBIds;

};

//---------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////
// Light types come in two kinds, matching the engine light classes:
//  - legacy (PointLight, SpotLight): Radius and FlickerOffRadius, as the
//    retail maps author them.
//  - Overdrive (Re_PointLight, Re_SpotLight, Re_AreaLight):
//    Intensity, Radius as the reach, SourceRadius and FlickerOffIntensity.
//    They load only for the Overdrive renderer.
class iEntityWrapperTypeLight : public iEntityWrapperType
{
	friend class iIconEntityLight;
public:
	iEntityWrapperTypeLight(const tString& asElementString, int alSubType, bool abOverdrive) : iEntityWrapperType(eEditorEntityType_Light, _W("Light"), asElementString),
																								mlSubType(alSubType), mbOverdrive(abOverdrive)
	{
		AddBool(eLightBool_CastShadows, "CastShadows", false);
		AddString(eLightStr_ShadowResolution, "ShadowResolution", "High");
		AddBool(eLightBool_ShadowsAffectStatic, "ShadowsAffectStatic");
		AddBool(eLightBool_ShadowsAffectDynamic, "ShadowsAffectDynamic");

		if(abOverdrive)
		{
			AddFloat(eLightFloat_Intensity, "Intensity", 1.0f);
			AddFloat(eLightFloat_Radius, "Radius", 1.0f);
			AddFloat(eLightFloat_SourceRadius, "SourceRadius", 0.0f);
			AddBool(eLightBool_RadiusDerived, "RadiusDerived", false, ePropCopyStep_PostEnt, false);
			GetPropInt(eObjInt_RendererMask)->SetDefault(static_cast<int>(hpl::kRendererMaskOverdrive));
		}
		else
		{
			AddFloat(eLightFloat_Radius, "Radius", 1.0f);
		}
		AddString(eLightStr_FalloffMap, "FalloffMap");
		AddString(eLightStr_Gobo, "Gobo");
		AddString(eLightStr_GoboAnimMode, "GoboAnimMode", "None");
		AddFloat(eLightFloat_GoboAnimFrameTime, "GoboAnimFrameTime");
		AddColor(eLightCol_Diffuse, "DiffuseColor", cColor(1));

		AddBool(eLightBool_FlickerActive, "FlickerActive", false);
		AddFloat(eLightFloat_FlickerOnMinLength, "FlickerOnMinLength");
		AddFloat(eLightFloat_FlickerOnMaxLength, "FlickerOnMaxLength");
		AddString(eLightStr_FlickerOnPS, "FlickerOnPS");
		AddString(eLightStr_FlickerOnSound, "FlickerOnSound");

		AddFloat(eLightFloat_FlickerOffMinLength, "FlickerOffMinLength");
		AddFloat(eLightFloat_FlickerOffMaxLength, "FlickerOffMaxLength");
		AddString(eLightStr_FlickerOffPS, "FlickerOffPS");
		AddString(eLightStr_FlickerOffSound, "FlickerOffSound");
		AddColor(eLightCol_FlickerOff, "FlickerOffColor", cColor(0));
		if(abOverdrive)
			AddFloat(eLightFloat_FlickerOffIntensity, "FlickerOffIntensity");
		else
			AddFloat(eLightFloat_FlickerOffRadius, "FlickerOffRadius");

		AddBool(eLightBool_FlickerFade, "FlickerFade", false);
		AddFloat(eLightFloat_FlickerOnFadeMinLength, "FlickerOnFadeMinLength");
		AddFloat(eLightFloat_FlickerOnFadeMaxLength, "FlickerOnFadeMaxLength");
		AddFloat(eLightFloat_FlickerOffFadeMinLength, "FlickerOffFadeMinLength");
		AddFloat(eLightFloat_FlickerOffFadeMaxLength, "FlickerOffFadeMaxLength");
	}

	int GetLightType() { return mlSubType; }
	bool IsOverdrive() { return mbOverdrive; }

	// Also honours the per-set toggle (Legacy / Overdrive lights).
	bool IsVisible();
	void SetVisible(bool abX);

	bool IsActive() { return mbLightsActive; }
	void SetActive(bool abX);


protected:

	int mlSubType;
	bool mbOverdrive;

	static bool mbLightsVisible;
	static bool mbLightsActive;
};

//---------------------------------------------------------------------

class iEntityWrapperLight : public iEntityWrapper
{
public:
	iEntityWrapperLight(iEntityWrapperData* apData);
	virtual ~iEntityWrapperLight();

	bool GetProperty(int, int&);
	bool GetProperty(int, float&);
	bool GetProperty(int, bool&);
	bool GetProperty(int, tString&);
	bool GetProperty(int, cColor&);

	bool SetProperty(int, const int&);
	bool SetProperty(int, const float&);
	bool SetProperty(int, const bool&);
	bool SetProperty(int, const tString&);
	bool SetProperty(int, const cColor&);

	int GetLightType() { return ((iEntityWrapperTypeLight*)mpType)->GetLightType(); }
	bool IsOverdrive() { return ((iEntityWrapperTypeLight*)mpType)->IsOverdrive(); }
	// Overdrive lights, and legacy ones previewed promoted in an Overdrive
	// editor, use the Overdrive engine light class.
	bool UsesOverdriveLightClass();

	// Handles of both light sets show in every editor renderer; only the
	// engine light follows the renderer mask.
	bool FiltersByEditorRenderer() { return false; }
	bool IsLitByEditorRenderer();
	// Legacy lights warm, Overdrive lights cool; dimmed when not lit.
	cColor GetIconTint();

	bool EntitySpecificCheckCulled(cEditorClipPlane* apPlane);

	bool GetCastShadows() { return mbCastShadows; }
	void SetCastShadows(bool abX);

	const tString& GetShadowResolution() { return msShadowResolution; }
	void SetShadowResolution(const tString& asX) { msShadowResolution = asX; }

	bool GetShadowsAffectStatic() { return mbShadowsAffectStatic; }
	void SetShadowsAffectStatic(bool abX);

	bool GetShadowsAffectDynamic() { return mbShadowsAffectDynamic; }
	void SetShadowsAffectDynamic(bool abX);

	virtual void SetGobo(const tString& asGoboFilename);
	const tString& GetGoboFilename() { return msGoboFilename; }

	const tString& GetGoboAnimMode() { return msGoboAnimMode; }
	void SetGoboAnimMode(const tString& asX);

	float GetGoboAnimFrameTime() { return mfGoboAnimFrameTime; }
	void SetGoboAnimFrameTime(float afX);

	virtual void SetIntensity(float afIntensity);
	float GetIntensity() { return mfIntensity; }

	virtual void SetRadius(float afRadius);
	float GetRadius() { return mfRadius; }

	virtual void SetSourceRadius(float afSourceRadius);
	float GetSourceRadius() { return mfSourceRadius; }

	// Redux lights without an authored Radius derive the reach from intensity
	// and colour; editing the radius makes it authored.
	void SetRadiusDerived(bool abX);
	bool IsRadiusDerived() { return mbRadiusDerived; }

	// Overdrive lights never load for the Standard renderer.
	void SetRendererMask(int alMask);

	void SetFalloffMap(const tString& asFalloffMap);
	const tString& GetFalloffMap() { return msFalloffMap; }

	void SetDiffuseColor(const cColor& aDiffuseColor);
	cColor GetDiffuseColor() { return mcolDiffuseColor; }


	//////////////////////////////////////////////////////
	// Flicker stuff
	bool GetFlickerActive() { return mbFlickerActive; }

	float GetFlickerOnMinLength() { return mfFlickerOnMinLength; }
	float GetFlickerOnMaxLength() { return mfFlickerOnMaxLength; }
	const tString& GetFlickerOnSound() { return msFlickerOnSound; }
	const tString& GetFlickerOnPS() { return msFlickerOnPS; }

	float GetFlickerOffMinLength() { return mfFlickerOffMinLength; }
	float GetFlickerOffMaxLength() { return mfFlickerOffMaxLength; }
	float GetFlickerOffRadius() { return mfFlickerOffRadius; }
	float GetFlickerOffIntensity() { return mfFlickerOffIntensity; }
	// What the light switches to when flickering off: the radius of a legacy
	// light, the intensity of an Overdrive one.
	float GetFlickerOffValue() { return IsOverdrive() ? mfFlickerOffIntensity : mfFlickerOffRadius; }
	cColor GetFlickerOffColor() { return mcolFlickerOffColor; }
	const tString& GetFlickerOffSound() { return msFlickerOffSound; }
	const tString& GetFlickerOffPS() { return msFlickerOffPS; }

	bool GetFlickerFade() { return mbFlickerFade; }

	float GetFlickerOnFadeMinLength() { return mfFlickerOnFadeMinLength; }
	float GetFlickerOnFadeMaxLength() { return mfFlickerOnFadeMaxLength; }
	float GetFlickerOffFadeMinLength() { return mfFlickerOffFadeMinLength; }
	float GetFlickerOffFadeMaxLength() { return mfFlickerOffFadeMaxLength; }


	void SetFlickerActive(bool abX);

	void SetFlickerOnMinLength(float afX);
	void SetFlickerOnMaxLength(float afX);
	void SetFlickerOnSound(const tString& asStr);
	void SetFlickerOnPS(const tString& asStr);


	void SetFlickerOffMinLength(float afX);
	void SetFlickerOffMaxLength(float afX);
	void SetFlickerOffSound(const tString& asStr);
	void SetFlickerOffPS(const tString& asStr);
	void SetFlickerOffRadius(float afX);
	void SetFlickerOffIntensity(float afX);
	void SetFlickerOffColor(const cColor& aCol);

	void SetFlickerFade(bool abX);

	void SetFlickerOnFadeMinLength(float afX);
	void SetFlickerOnFadeMaxLength(float afX);
	void SetFlickerOffFadeMinLength(float afX);
	void SetFlickerOffFadeMaxLength(float afX);

	void UpdateFlickerParams();

	//////////////////////////////////////////////////
	// Billboard connection
	void AddConnectedBillboard(cEntityWrapperBillboard* apBB);
	void RemoveConnectedBillboard(cEntityWrapperBillboard* apBB);
	std::list<cEntityWrapperBillboard*>& GetConnectedBillboards() { return mlstConnectedBBs; }


	void Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelected, const cColor& aHighlightCol, const cColor& aDisabledCol);

	void SaveToElement(tinyxml2::XMLElement* apElement);

	cEditorWindowEntityEditBox* CreateEditBox(cEditorEditModeSelect* apEditMode);

protected:
	void OnSetVisible(bool abX) {}
	void OnSetCulled(bool abX) {}
	void OnSetActive(bool abX);

	// Pushes intensity, radius and source radius to the engine light the way
	// the game loads this kind of light.
	void ApplyLightValues();

	///////////////////////////
	// To be implemented
	virtual void DrawLightTypeSpecific(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelectedc)=0;


	//////////////////////
	// Data

	bool mbCastShadows;
	tString msShadowResolution;
	bool mbShadowsAffectStatic;
	bool mbShadowsAffectDynamic;

	tString msGoboFilename;
	tString msGoboAnimMode;
	float mfGoboAnimFrameTime;

	float mfIntensity;
	float mfRadius;
	float mfSourceRadius;
	bool mbRadiusDerived;

	cColor mcolDiffuseColor;

	tString msFalloffMap;

	//////////////////////
	// Flicker stuff
	bool mbFlickerUpdated;
	bool mbFlickerActive;

	float mfFlickerOnMinLength;
	float mfFlickerOnMaxLength;
	tString msFlickerOnSound;
	tString msFlickerOnPS;

	float mfFlickerOffMinLength;
	float mfFlickerOffMaxLength;
	tString msFlickerOffSound;
	tString msFlickerOffPS;
	float mfFlickerOffRadius;
	float mfFlickerOffIntensity;
	cColor mcolFlickerOffColor;

	bool mbFlickerFade;

	float mfFlickerOnFadeMinLength;
	float mfFlickerOnFadeMaxLength;
	float mfFlickerOffFadeMinLength;
	float mfFlickerOffFadeMaxLength;

	std::list<cEntityWrapperBillboard*> mlstConnectedBBs;
};

//---------------------------------------------------------------------

#endif // HPLEDITOR_ENTITY_WRAPPER_STATIC_OBJECT_H
