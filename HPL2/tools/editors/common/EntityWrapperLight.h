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

// One light object carries both backends' tuning. A ray-traced override of the
// property with id N is registered at N + LightReduxPropIdOffset under the XML
// name "Re_<Name>", alongside an unsaved bool that records whether the override
// is authored at all -- an unauthored one is stripped on save, so a light that
// was never given Redux values round-trips byte-identically.
#define LightReduxPropIdOffset 1000
// Ids for those "is it authored" flags. They are bools whatever the type of the
// value they guard, so they cannot be derived from the base id (the float and
// bool id spaces overlap); they come off this counter instead.
#define LightReduxSetPropIdStart 2000

//////////////////////////////////////////////
// How a light shape spells its values.
enum eLightSchema
{
	// PointLight / SpotLight: retail attributes plus optional Re_ overrides.
	eLightSchema_Dual,
	// AreaLight: no legacy class, so the plain attributes already are the
	// ray-traced ones and there is nothing to override.
	eLightSchema_RayTracedOnly,
	// BoxLight: no ray-traced class at all.
	eLightSchema_StandardOnly,
};

// A property and its Re_ twin.
class cLightReduxLink
{
public:
	eVariableType mType;
	int mlBaseId;		// -1 when the value only exists for the ray-traced backend
	int mlReduxId;
	int mlSetId;
	tString msReduxName;
};

typedef std::vector<cLightReduxLink> tLightReduxLinkVec;

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
	eLightFloat_Intensity = LightPropIdStart, // ray-traced lights only
	eLightFloat_Radius,                       // legacy radius, or the ray-traced reach
	eLightFloat_SourceRadius,                 // ray-traced lights only

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
	eLightFloat_FlickerOffIntensity,          // ray-traced lights only

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
// One light shape, one editor type. The retail attributes drive the Standard
// renderer; the ray-traced backend reads the same attribute spelled
// "Re_<Name>" when the light authors one, and otherwise promotes the retail
// Radius exactly as cEngineFileLoading::LoadLight does.
//
// Overrides are declared through the AddLight* helpers below, so the base-type
// properties (Name, Tag, WorldPos, Rotation, Scale, RendererMask) are
// structurally excluded from twinning -- transform and identity can never vary
// per backend.
class iEntityWrapperTypeLight : public iEntityWrapperType
{
	friend class iIconEntityLight;
public:
	iEntityWrapperTypeLight(const tString& asElementString, int alSubType, eLightSchema aSchema) : iEntityWrapperType(eEditorEntityType_Light, _W("Light"), asElementString),
																								mlSubType(alSubType), mSchema(aSchema)
	{
		// No ray-traced twin: the ray-traced backend shadows every light anyway.
		AddBool(eLightBool_CastShadows, "CastShadows", false);
		AddLightString(eLightStr_ShadowResolution, "ShadowResolution", "High");
		AddLightBool(eLightBool_ShadowsAffectStatic, "ShadowsAffectStatic");
		AddLightBool(eLightBool_ShadowsAffectDynamic, "ShadowsAffectDynamic");

		if(aSchema==eLightSchema_RayTracedOnly)
		{
			// Plain names, because there is no legacy meaning to collide with.
			AddFloat(eLightFloat_Intensity, "Intensity", 1.0f);
			AddFloat(eLightFloat_Radius, "Radius", 1.0f);
			AddFloat(eLightFloat_SourceRadius, "SourceRadius", 0.0f);
			GetPropInt(eObjInt_RendererMask)->SetDefault(static_cast<int>(hpl::kRendererMaskRayTraced));
		}
		else
		{
			// The retail radius. On a Dual shape the ray-traced reach is a
			// DIFFERENT quantity and lives in Re_Radius -- see LightParameters.h.
			AddFloat(eLightFloat_Radius, "Radius", 1.0f);
			if(aSchema==eLightSchema_Dual)
			{
				AddReduxOnlyFloat(eLightFloat_Intensity, "Intensity", 1.0f);
				AddReduxOnlyFloat(eLightFloat_Radius, "Radius", 1.0f);
				AddReduxOnlyFloat(eLightFloat_SourceRadius, "SourceRadius", 0.0f);
			}
		}
		// True while the ray-traced reach follows the intensity, i.e. no reach
		// is authored. Never saved: its absence from the file IS the flag.
		AddBool(eLightBool_RadiusDerived, "RadiusDerived", false, ePropCopyStep_PostEnt, false);

		AddLightString(eLightStr_FalloffMap, "FalloffMap");
		AddLightString(eLightStr_Gobo, "Gobo");
		AddLightString(eLightStr_GoboAnimMode, "GoboAnimMode", "None");
		AddLightFloat(eLightFloat_GoboAnimFrameTime, "GoboAnimFrameTime");
		AddLightColor(eLightCol_Diffuse, "DiffuseColor", cColor(1));

		AddLightBool(eLightBool_FlickerActive, "FlickerActive", false);
		AddLightFloat(eLightFloat_FlickerOnMinLength, "FlickerOnMinLength");
		AddLightFloat(eLightFloat_FlickerOnMaxLength, "FlickerOnMaxLength");
		AddLightString(eLightStr_FlickerOnPS, "FlickerOnPS");
		AddLightString(eLightStr_FlickerOnSound, "FlickerOnSound");

		AddLightFloat(eLightFloat_FlickerOffMinLength, "FlickerOffMinLength");
		AddLightFloat(eLightFloat_FlickerOffMaxLength, "FlickerOffMaxLength");
		AddLightString(eLightStr_FlickerOffPS, "FlickerOffPS");
		AddLightString(eLightStr_FlickerOffSound, "FlickerOffSound");
		AddLightColor(eLightCol_FlickerOff, "FlickerOffColor", cColor(0));
		if(aSchema==eLightSchema_RayTracedOnly)
		{
			AddFloat(eLightFloat_FlickerOffIntensity, "FlickerOffIntensity");
		}
		else
		{
			AddFloat(eLightFloat_FlickerOffRadius, "FlickerOffRadius");
			if(aSchema==eLightSchema_Dual)
				AddReduxOnlyFloat(eLightFloat_FlickerOffIntensity, "FlickerOffIntensity");
		}

		AddLightBool(eLightBool_FlickerFade, "FlickerFade", false);
		AddLightFloat(eLightFloat_FlickerOnFadeMinLength, "FlickerOnFadeMinLength");
		AddLightFloat(eLightFloat_FlickerOnFadeMaxLength, "FlickerOnFadeMaxLength");
		AddLightFloat(eLightFloat_FlickerOffFadeMinLength, "FlickerOffFadeMinLength");
		AddLightFloat(eLightFloat_FlickerOffFadeMaxLength, "FlickerOffFadeMaxLength");
	}

	int GetLightType() { return mlSubType; }
	eLightSchema GetLightSchema() { return mSchema; }
	// The plain attributes are the ray-traced ones (area light).
	bool IsRayTracedOnly() { return mSchema==eLightSchema_RayTracedOnly; }
	// No ray-traced class exists, so Re_ values would never be read (box light).
	bool IsStandardOnly() { return mSchema==eLightSchema_StandardOnly; }
	bool SupportsReduxOverrides() { return mSchema==eLightSchema_Dual; }

	const tLightReduxLinkVec& GetReduxLinks() { return mvReduxLinks; }
	const cLightReduxLink* GetReduxLinkByReduxId(eVariableType aType, int alID);
	const cLightReduxLink* GetReduxLinkBySetId(int alID);
	// Id of the "is this override authored" flag guarding a property. The flag
	// ids come off a counter, so they cannot be computed from the base id.
	int GetReduxSetId(eVariableType aType, int alBaseId);

	// Also honours the light visibility toggle.
	bool IsVisible();
	void SetVisible(bool abX);

	bool IsActive() { return mbLightsActive; }
	void SetActive(bool abX);


protected:
	// Registers the retail property and, on a Dual shape, its "Re_<name>" twin
	// plus the unsaved flag that records whether the twin is authored.
	//
	// ORDER MATTERS: the value twin is registered immediately before its flag,
	// and iEntityWrapperData walks properties in registration order. Copying a
	// light therefore writes the value (which marks it authored) and only then
	// the flag, which clears it again for an unauthored override. Swap the two
	// and every load silently drops its overrides.
	void AddLightFloat(int alID, const tString& asName, float afDefault=0.0f);
	void AddLightBool(int alID, const tString& asName, bool abDefault=true);
	void AddLightString(int alID, const tString& asName, const tString& asDefault="");
	void AddLightColor(int alID, const tString& asName, const cColor& aDefault=cColor(1));
	// A value the ray-traced backend has but the retail schema does not
	// (Intensity, SourceRadius, FlickerOffIntensity, and the reach).
	void AddReduxOnlyFloat(int alID, const tString& asName, float afDefault=0.0f);

	void AddReduxLink(eVariableType aType, int alBaseId, int alID, const tString& asName);

	int mlSubType;
	eLightSchema mSchema;
	tLightReduxLinkVec mvReduxLinks;
	int mlNextReduxSetId = LightReduxSetPropIdStart;

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
	eLightSchema GetLightSchema() { return ((iEntityWrapperTypeLight*)mpType)->GetLightSchema(); }
	bool IsRayTracedOnly() { return ((iEntityWrapperTypeLight*)mpType)->IsRayTracedOnly(); }
	bool SupportsReduxOverrides() { return ((iEntityWrapperTypeLight*)mpType)->SupportsReduxOverrides(); }
	// The area light, and any light previewed under the ray-traced backend, use
	// the ray-traced engine light class.
	bool UsesRayTracedLightClass();

	//////////////////////////////////////////////////////
	// Ray-traced overrides. A value is authored exactly when it is present in
	// the map, so there is no separate flag to drift out of step.
	bool HasReduxOverride(eVariableType aType, int alBaseId);
	int GetReduxSetPropId(eVariableType aType, int alBaseId)
		{ return ((iEntityWrapperTypeLight*)mpType)->GetReduxSetId(aType, alBaseId); }
	void ClearReduxOverride(eVariableType aType, int alBaseId);
	bool HasAnyReduxOverride();

	// What the ray-traced backend would use: the override when authored,
	// otherwise the promoted retail value.
	float GetEffectiveIntensity();
	float GetEffectiveReach();
	float GetEffectiveSourceRadius();
	float GetEffectiveFlickerOffValue();
	cColor GetEffectiveDiffuseColor();

	// Handles of both light sets show in every editor renderer; only the
	// engine light follows the renderer mask.
	bool FiltersByEditorRenderer() { return false; }
	bool IsLitByEditorRenderer();
	// Warm by default, cool once the light carries ray-traced overrides;
	// dimmed when this renderer does not light it.
	cColor GetIconTint();

	bool EntitySpecificCheckCulled(cEditorClipPlane* apPlane);

	// Ray-traced override plumbing, shared by every Get/SetProperty overload.
	bool GetReduxProperty(int alPropID, eVariableType aType, void* apOut);
	bool SetReduxProperty(int alPropID, eVariableType aType, const void* apValue);
	float GetInheritedReduxFloat(const cLightReduxLink& aLink);

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

	// The area light has no Standard class, so it never loads for that renderer.
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
	// light, the intensity of a ray-traced one.
	// What the flicker actually fades to under the renderer being previewed.
	float GetFlickerOffValue() { return UsesRayTracedLightClass() ? GetEffectiveFlickerOffValue()
																  : mfFlickerOffRadius; }
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

	// Authored ray-traced overrides, keyed by the BASE property id. One map per
	// variable type, because the id spaces overlap between types.
	std::map<int,float> mmapReduxFloat;
	std::map<int,bool> mmapReduxBool;
	std::map<int,tString> mmapReduxStr;
	std::map<int,cColor> mmapReduxCol;

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
