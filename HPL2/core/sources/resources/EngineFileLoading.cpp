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

#include "resources/EngineFileLoading.h"
#include "graphics/Image.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/MaterialManager.h"

#include "math/Math.h"

#include "system/String.h"

#include "scene/World.h"
#include "scene/LightPoint.h"
#include "scene/LightSpot.h"
#include "scene/LightArea.h"
#include "scene/LightBox.h"
#include "scene/LightParameters.h"
#include "scene/MeshEntity.h"
#include "scene/SoundEntity.h"
#include "scene/ParticleEmitter.h"
#include "scene/ParticleSystem.h"
#include "scene/BillBoard.h"
#include "scene/Beam.h"
#include "scene/GuiSetEntity.h"
#include "scene/RopeEntity.h"
#include "scene/FogArea.h"

#include "graphics/Graphics.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// DEFINES
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	#define kBeginWorldEntityLoad()		\
		tString sName = GetAttributeString(apElement, "Name");
	
	#define kEndWorldEntityLoad(pEntity)		\
		SetupWorldEntity(pEntity, apElement);	\
		return pEntity;

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CREATE ENTITIES
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	unsigned cEngineFileLoading::GetElementRendererMask(tinyxml2::XMLElement* apElement)
	{
		const cLightElementInfo lightInfo = GetLightElementInfo(apElement->Value());
		unsigned lMask = apElement->Attribute("RendererMask")
			? SanitizeRendererMask(static_cast<unsigned>(GetAttributeInt(apElement, "RendererMask", 0)))
			: (lightInfo.mbValid ? GetDefaultLightRendererMask(lightInfo) : kRendererMaskAll);
		// The ray-traced-only shape never loads on Standard.
		if(lightInfo.mbValid && lightInfo.mbRayTracedOnly) lMask &= kRendererMaskRayTraced;
		return lMask;
	}

	bool cEngineFileLoading::IsElementEnabledForWorld(tinyxml2::XMLElement*, cWorld*)
	{
		// Nothing is filtered out at load any more. Every object is created and
		// its renderer mask gates whether it DRAWS (rendering::IsObjectIsVisible)
		// and, for game entities, whether it is active. That is what makes the
		// world's contents independent of the backend the session started in, so
		// the backend can change without reloading the map -- and it is why a
		// ConnectLight always resolves.
		return true;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// LIGHT ELEMENT ATTRIBUTES
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLightElementAttributes::cLightElementAttributes(tinyxml2::XMLElement* apElement, bool abUseOverrides)
		: mpElement(apElement), mbUseOverrides(abUseOverrides)
	{
	}

	tString cLightElementAttributes::Resolve(const tString& asName) const
	{
		if(mbUseOverrides)
		{
			const tString sOverride = tString(kLightOverrideAttributePrefix) + asName;
			if(mpElement && mpElement->Attribute(sOverride.c_str())) return sOverride;
		}
		return asName;
	}

	bool cLightElementAttributes::Has(const tString& asName) const
	{
		return mpElement && mpElement->Attribute(Resolve(asName).c_str()) != NULL;
	}

	tString cLightElementAttributes::GetStr(const tString& asName, const tString& asDefault) const
	{
		return GetAttributeString(mpElement, Resolve(asName), asDefault);
	}

	float cLightElementAttributes::GetFloat(const tString& asName, float afDefault) const
	{
		return GetAttributeFloat(mpElement, Resolve(asName), afDefault);
	}

	int cLightElementAttributes::GetInt(const tString& asName, int alDefault) const
	{
		return GetAttributeInt(mpElement, Resolve(asName), alDefault);
	}

	bool cLightElementAttributes::GetBool(const tString& asName, bool abDefault) const
	{
		return GetAttributeBool(mpElement, Resolve(asName), abDefault);
	}

	cColor cLightElementAttributes::GetColor(const tString& asName, const cColor& aDefault) const
	{
		return GetAttributeColor(mpElement, Resolve(asName), aDefault);
	}

	cVector3f cLightElementAttributes::GetVec3(const tString& asName, const cVector3f& avDefault) const
	{
		return GetAttributeVector3f(mpElement, Resolve(asName), avDefault);
	}

	bool cLightElementAttributes::HasOverride(const tString& asName) const
	{
		if(mbUseOverrides==false || mpElement==NULL) return false;
		return mpElement->Attribute((tString(kLightOverrideAttributePrefix) + asName).c_str()) != NULL;
	}

	float cLightElementAttributes::GetOverrideFloat(const tString& asName, float afDefault) const
	{
		if(mpElement==NULL) return afDefault;
		return GetAttributeFloat(mpElement, tString(kLightOverrideAttributePrefix) + asName, afDefault);
	}

	//-----------------------------------------------------------------------

	// Every attribute LoadLight reads, so a Re_ spelling of anything else can be
	// reported instead of silently doing nothing.
	static bool IsOverridableLightAttribute(const tString& asName)
	{
		// The per-backend tuning, and nothing else. A light is ONE object with
		// two tunings, so an attribute outside this set has one value shared by
		// both backends and a Re_ spelling of it would do nothing -- which is
		// what the caller warns about.
		static const char *kNames[] = {
			// photometry (Re_ only on shapes that have a legacy class)
			"Intensity", "Radius", "SourceRadius", "FlickerOffIntensity", "FlickerOffRadius",
			"CastShadows", "DiffuseColor",
		};
		for(const char *pName : kNames)
			if(asName == pName) return true;
		return false;
	}

	void cLightElementAttributes::WarnAboutUnusableOverrides(const tString& asLightName) const
	{
		if(mpElement==NULL) return;

		const size_t lPrefixLen = strlen(kLightOverrideAttributePrefix);
		for(const tinyxml2::XMLAttribute *pAttr = mpElement->FirstAttribute(); pAttr;
			pAttr = pAttr->Next())
		{
			const tString sName = pAttr->Name();
			if(sName.size() <= lPrefixLen) continue;
			if(sName.compare(0, lPrefixLen, kLightOverrideAttributePrefix) != 0) continue;

			const tString sSuffix = sName.substr(lPrefixLen);
			if(IsOverridableLightAttribute(sSuffix)) continue;

			Warning("Light '%s' has attribute '%s', which the loader never reads."
					" Only photometry, CastShadows and DiffuseColor differ per"
					" renderer; everything else is shared by both.\n",
					asLightName.c_str(), sName.c_str());
		}
	}

	//-----------------------------------------------------------------------

	cFogArea* cEngineFileLoading::LoadFogArea(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, bool abStatic)
	{
		kBeginWorldEntityLoad();

		cFogArea *pFog = apWorld->CreateFogArea(asNamePrefix+sName, abStatic);

		if(pFog)
		{
			pFog->SetRendererMask(GetElementRendererMask(apElement));
			pFog->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pFog->SetStart(GetAttributeFloat(apElement, "Start", 0));
			pFog->SetEnd(GetAttributeFloat(apElement, "End", 0));
			pFog->SetFalloffExp(GetAttributeFloat(apElement, "FalloffExp", 0));
			pFog->SetShowBacksideWhenInside(GetAttributeBool(apElement, "ShownBacksideWhenInside", true));
			pFog->SetShowBacksideWhenOutside(GetAttributeBool(apElement, "ShownBacksideWhenOutside", true));
		}

		kEndWorldEntityLoad(pFog);
	}

	//-----------------------------------------------------------------------

	cParticleSystem* cEngineFileLoading::LoadParticleSystem(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sFile = GetAttributeString(apElement, "File");

		cParticleSystem *pPS = apWorld->CreateParticleSystem(asNamePrefix+sName,sFile,1);

		if(pPS)
		{
			pPS->SetColor(GetAttributeColor(apElement, "Color",cColor(1,1)));
			pPS->SetFadeAtDistance(GetAttributeBool(apElement, "FadeAtDistance", false));
			pPS->SetMinFadeDistanceStart(GetAttributeFloat(apElement, "MinFadeDistanceStart"));
			pPS->SetMinFadeDistanceEnd(GetAttributeFloat(apElement, "MinFadeDistanceEnd"));
			pPS->SetMaxFadeDistanceStart(GetAttributeFloat(apElement, "MaxFadeDistanceStart"));
			pPS->SetMaxFadeDistanceEnd(GetAttributeFloat(apElement, "MaxFadeDistanceEnd"));
		}
		
		kEndWorldEntityLoad(pPS);
	}
	
	//-----------------------------------------------------------------------

	cSoundEntity* cEngineFileLoading::LoadSound(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld)
	{
		kBeginWorldEntityLoad();

		tString sSoundFile = GetAttributeString(apElement, "SoundEntityFile");
		bool bUseDefault = GetAttributeBool(apElement, "UseDefault");

		cSoundEntity *pSound = apWorld->CreateSoundEntity(asNamePrefix+sName,sSoundFile,false);
		if(pSound==NULL) return NULL;

		if(bUseDefault==false)
		{
			pSound->SetMinDistance(GetAttributeFloat(apElement, "MinDistance"));
			pSound->SetMaxDistance(GetAttributeFloat(apElement, "MaxDistance"));
			pSound->SetVolume(GetAttributeFloat(apElement, "Volume"));
		}


		kEndWorldEntityLoad(pSound);
	}

	
	//-----------------------------------------------------------------------

	static eBillboardType ToBillboardType(const tString& asType)
	{
		if(asType == "Axis") return eBillboardType_Axis;
		if(asType == "Point") return eBillboardType_Point;
		if(asType == "FixedAxis") return eBillboardType_FixedAxis;

		return eBillboardType_Point;
	}

	cBillboard* cEngineFileLoading::LoadBillboard(tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic,
													tEFL_LightBillboardConnectionList *apLightBillboardList)
	{
		kBeginWorldEntityLoad();

		cVector2f vSize = GetAttributeVector2f(apElement, "BillboardSize");
		tString sMat = GetAttributeString(apElement, "MaterialFile");
		eBillboardType bbType = ToBillboardType(GetAttributeString(apElement, "BillboardType"));

		cBillboard *pBillboard = apWorld->CreateBillboard(asNamePrefix+sName,vSize,bbType,sMat, abStatic);
		if(pBillboard==NULL) return NULL;
		pBillboard->SetRendererMask(GetElementRendererMask(apElement));

		pBillboard->SetForwardOffset(GetAttributeFloat(apElement, "BillboardOffset"));
		pBillboard->SetColor(GetAttributeColor(apElement, "BillboardColor",cColor(1,1)));

		pBillboard->SetIsHalo(GetAttributeBool(apElement, "IsHalo",false));
		pBillboard->SetHaloSourceSize(GetAttributeVector3f(apElement, "HaloSourceSize",1));

		tString sConnectLight = GetAttributeString(apElement, "ConnectLight");
		if(apLightBillboardList && sConnectLight!="")
		{
			cEFL_LightBillboardConnection lightBBConnection;
			lightBBConnection.msBillboardID = GetAttributeInt(apElement, "ID");
			lightBBConnection.msLightName = asNamePrefix+sConnectLight;
			apLightBillboardList->push_back(lightBBConnection);
		}

		kEndWorldEntityLoad(pBillboard);
	}

	//-----------------------------------------------------------------------
	
	static eShadowMapResolution ToShadowMapResolution(const tString& asType)
	{
		tString sLowType = cString::ToLowerCase(asType);

        if(sLowType == "high") return eShadowMapResolution_High;
		if(sLowType == "medium") return eShadowMapResolution_Medium;
		if(sLowType == "low") return eShadowMapResolution_Low;
		return eShadowMapResolution_High;
	}

	static eTextureAnimMode ToTextureAnimMode(const tString& asType)
	{
		if(cString::ToLowerCase(asType) == "none") return eTextureAnimMode_None;
		else if(cString::ToLowerCase(asType) == "loop") return eTextureAnimMode_Loop;
		else if(cString::ToLowerCase(asType) == "oscillate") return eTextureAnimMode_Oscillate;

		return eTextureAnimMode_None;
	}
	
	iLight* cEngineFileLoading::LoadLight(	tinyxml2::XMLElement* apElement, const tString& asNamePrefix, cWorld *apWorld, cResources *apResources, bool abStatic)
	{
		kBeginWorldEntityLoad();

		iLight *pLight = NULL;

		const cLightElementInfo info = GetLightElementInfo(apElement->Value());
		if(info.mbValid==false)
		{
			// Re_PointLight and friends were the pre-merge twin elements; one
			// light is one element now, with Re_-prefixed ray-traced overrides.
			Error("Unknown light type '%s'%s\n", apElement->Value(),
				strncmp(apElement->Value(), kLightOverrideAttributePrefix,
						strlen(kLightOverrideAttributePrefix))==0
					? " (pre-merge twin element -- use one light element with Re_ tuning attributes)" : "");
			return NULL;
		}

		// One class per shape: the light gets both tunings below and resolves
		// whichever the world's backend selects, so nothing here depends on the
		// backend any more.
		const unsigned int lElementMask = GetElementRendererMask(apElement);

		// Two readers over the same element: the retail attributes drive the
		// Standard tuning, the Re_-prefixed ones the ray-traced tuning. The
		// typo warning now runs on every backend, not just the ray-traced one.
		const cLightElementAttributes legacyAttr(apElement, false);
		const cLightElementAttributes reduxAttr(apElement, true);
		// Shape and behaviour attributes (FOV, gobo, falloff, shadow resolution,
		// flicker timings) are shared by both backends, so they are read from
		// the plain name only; a Re_ spelling of one is a typo and is reported.
		const cLightElementAttributes& attr = legacyAttr;
		reduxAttr.WarnAboutUnusableOverrides(sName);

		bool bStatic = abStatic;

		//////////////////////////
		// Area Light
		if(info.mShape == eLightElementShape_Area)
		{
			cLightArea *pLightArea = apWorld->CreateLightArea(asNamePrefix+sName, bStatic);
			pLight = pLightArea;

			pLightArea->SetWidth(attr.GetFloat("SourceWidth", 1.0f));
			pLightArea->SetHeight(attr.GetFloat("SourceHeight", 1.0f));
			pLightArea->SetBarnDoorAngle(attr.GetFloat("BarnDoorAngle", cMath::ToRad(45.0f)));
			pLightArea->SetBarnDoorLength(attr.GetFloat("BarnDoorLength", 0.0f));

			//Optional source texture (stored in the base gobo slot — a 2D image). Tints emission.
			tString sSourceTex = attr.GetStr("SourceTexture");
			if(sSourceTex != "")
			{
				Image *pTex = apResources->GetTextureManager()->Create2DImage(sSourceTex,true).Release();
				if(pTex) pLightArea->SetGoboTexture(pTex);
			}
		}
		//////////////////////////
		// Box Light
		else if(info.mShape == eLightElementShape_Box)
		{
			cLightBoxLegacy *pLightBox = apWorld->CreateLightBoxLegacy(asNamePrefix+sName, bStatic);
			pLight = pLightBox;

			pLightBox->SetSize(attr.GetVec3("Size", cVector3f(1,1,1)));
			pLightBox->SetBlendFunc((eLightBoxBlendFunc)attr.GetInt("BlendFunc", (int)eLightBoxBlendFunc_Add));
		}
		//////////////////////////
		// Spotlightt
		else if(info.mShape == eLightElementShape_Spot)
		{
			cLightSpot *pLightSpot = apWorld->CreateLightSpot(asNamePrefix+sName,"", bStatic);
			pLight = pLightSpot;

			//Frustum related
			pLightSpot->SetFOV(attr.GetFloat("FOV", 1.0f));
			pLightSpot->SetAspect(attr.GetFloat("Aspect", 1.0f));
			pLightSpot->SetNearClipPlane(attr.GetFloat("NearClipPlane", 0.1f));

			//Spot fall off
			tString sSpotFalloffMap = attr.GetStr("SpotFalloffMap");
			if(sSpotFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sSpotFalloffMap,true).Release();
				if(pFalloff) pLightSpot->SetSpotFalloffMap(pFalloff);
			}
		}
		//////////////////////////
		// Point Light
		else
		{
			pLight = apWorld->CreateLightPoint(asNamePrefix+sName,"", bStatic);
		}

		//////////////////////////
		// General properties
		eLightType lightType = pLight->GetLightType();
		const bool bSpotLight = lightType == eLightType_Spot;

		//Spot and point
		if(lightType == eLightType_Point || bSpotLight)
		{
			//Falloff
			tString sFalloffMap = attr.GetStr("FalloffMap");
			if(sFalloffMap != "")
			{
				Image *pFalloff = apResources->GetTextureManager()->Create1DImage(sFalloffMap,true).Release();
				if(pFalloff) pLight->SetFalloffMap(pFalloff);
			}

			//Gobo
			tString sGobo = attr.GetStr("Gobo","");
			if(sGobo  != "")
			{
				eTextureAnimMode animMode = ToTextureAnimMode(attr.GetStr("GoboAnimMode",""));
				float fAnimFrameTime = attr.GetFloat("GoboAnimFrameTime", 1);

				Image *pGoboTex=NULL;
				if(bSpotLight)
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->Create2DImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo, true, eTextureType_2D,
								eTextureUsage_Normal,0,false,animMode,fAnimFrameTime).Release();
				}
				else
				{
					if(animMode == eTextureAnimMode_None)
						pGoboTex = apResources->GetTextureManager()->CreateCubeMapImage(sGobo,true).Release();
					else
						pGoboTex = apResources->GetTextureManager()->CreateAnimImage(sGobo,true, eTextureType_CubeMap,
								eTextureUsage_Normal,0,false,animMode,fAnimFrameTime).Release();
				}

				if(pGoboTex)
				{
					pLight->SetGoboTexture(pGoboTex);
				}
			}
		}

		//All types
		const bool bHasRendererMask = apElement->Attribute("RendererMask") != NULL;
		const unsigned int lAuthoredMask = static_cast<unsigned int>(GetAttributeInt(apElement, "RendererMask", 0));

		if(info.mbRayTracedOnly && bHasRendererMask && (lAuthoredMask & kRendererMaskStandard))
			Warning("Light '%s' has no Standard class but sets the Standard renderer bit;"
					" it only loads on the ray-traced backend\n", sName.c_str());

		// The mask now gates rendering rather than creation; GetElementRendererMask
		// already clamps the ray-traced-only shape to its own bit.
		pLight->SetRendererMask(lElementMask);

		////////////////////////////////////////////////////////////////////
		// Both backends' tuning, from the one element. The Standard tuning
		// reads the retail attributes, the ray-traced one prefers Re_ and
		// otherwise promotes the retail values -- so a light with no Re_*
		// resolves exactly as it always did on either backend.
		const bool bPhotometryIsUnprefixed = info.mbRayTracedOnly;
		const auto HasPhotometry = [&](const char *apName) {
			return bPhotometryIsUnprefixed ? reduxAttr.Has(apName) : reduxAttr.HasOverride(apName);
		};
		const auto GetPhotometry = [&](const char *apName, float afDefault) {
			return bPhotometryIsUnprefixed ? reduxAttr.GetFloat(apName, afDefault)
										   : reduxAttr.GetOverrideFloat(apName, afDefault);
		};

		cLightTuningState standard;
		standard.mbPresent = !info.mbRayTracedOnly;
		standard.mbAuthored = standard.mbPresent;
		standard.mDiffuseColor = legacyAttr.GetColor("DiffuseColor", cColor(1));
		standard.mDefaultDiffuseColor = standard.mDiffuseColor;
		standard.mbCastShadows = legacyAttr.GetBool("CastShadows", false);
		{
			cLegacyLightInput input;
			input.mbHasRadius = legacyAttr.Has("Radius");
			input.mfRadius = legacyAttr.GetFloat("Radius", 1.0f);
			input.mbHasFlickerOffRadius = legacyAttr.Has("FlickerOffRadius");
			input.mfFlickerOffRadius = legacyAttr.GetFloat("FlickerOffRadius", 0.0f);
			const cLegacyLightParameters legacy = ResolveLegacyLightParameters(input);
			// The retail radius is both the reach and the animated value.
			standard.mfReach = legacy.mfRadius;
			standard.mfOnValue = legacy.mfRadius;
			standard.mfOffValue = legacy.mfFlickerOffRadius;
			standard.mfIntensity = legacy.mfRadius;
			standard.mfSourceRadius = 0.0f;
			standard.mbReachFollowsIntensity = false;
		}

		cLightTuningState rayTraced;
		rayTraced.mbPresent = info.mShape != eLightElementShape_Box;
		// Re_DiffuseColor is read before the photometry: a Re_Intensity with no
		// Re_Radius derives its reach from this colour.
		rayTraced.mDiffuseColor = reduxAttr.GetColor("DiffuseColor", cColor(1));
		rayTraced.mDefaultDiffuseColor = rayTraced.mDiffuseColor;
		// Shared with the retail half: the ray-traced backend has no cast-shadow
		// override of its own.
		rayTraced.mbCastShadows = legacyAttr.GetBool("CastShadows", false);
		if(rayTraced.mbPresent)
		{
			const bool bAuthored =
				HasPhotometry("Intensity") || HasPhotometry("Radius") ||
				HasPhotometry("SourceRadius") || HasPhotometry("FlickerOffIntensity");
			rayTraced.mbAuthored = bAuthored;

			cRayTracedLightParameters params;
			if(bAuthored)
			{
				cRayTracedLightInput input;
				input.mbHasIntensity = HasPhotometry("Intensity");
				input.mfIntensity = GetPhotometry("Intensity", 1.0f);
				input.mbHasRadius = HasPhotometry("Radius");
				input.mfRadius = GetPhotometry("Radius", 0.0f);
				input.mbHasSourceRadius = HasPhotometry("SourceRadius");
				input.mfSourceRadius = GetPhotometry("SourceRadius", 0.0f);
				input.mbHasFlickerOffIntensity = HasPhotometry("FlickerOffIntensity");
				input.mfFlickerOffIntensity = GetPhotometry("FlickerOffIntensity", 0.0f);
				input.mfRed = rayTraced.mDiffuseColor.r;
				input.mfGreen = rayTraced.mDiffuseColor.g;
				input.mfBlue = rayTraced.mDiffuseColor.b;
				params = ResolveRayTracedLightParameters(input);
				rayTraced.mbReachFollowsIntensity = input.mbHasRadius==false;
			}
			else
			{
				// No authored ray-traced tuning: derive it from the retail
				// radius, exactly as a retail light did before the merge.
				cLegacyLightParameters legacy;
				legacy.mfRadius = standard.mfReach;
				legacy.mfFlickerOffRadius = standard.mfOffValue;
				params = PromoteLegacyLightParameters(legacy, rayTraced.mDiffuseColor.r,
														rayTraced.mDiffuseColor.g,
														rayTraced.mDiffuseColor.b);
				// Scripts fade retail lights by radius; the promoted reach follows.
				rayTraced.mbReachFollowsIntensity = true;
			}
			// The intensity is the animated value on this backend.
			rayTraced.mfIntensity = params.mfIntensity;
			rayTraced.mfOnValue = params.mfIntensity;
			rayTraced.mfOffValue = params.mfFlickerOffIntensity;
			rayTraced.mfReach = params.mfRadius;
			rayTraced.mfSourceRadius = params.mfSourceRadius;
		}

		pLight->SetTuning(eLightModel_Legacy, standard);
		pLight->SetTuning(eLightModel_RayTraced, rayTraced);

		const float fFlickerOffValue =
			pLight->GetTuning(pLight->GetActiveLightModel()).mfOffValue;

		pLight->SetShadowMapResolution( ToShadowMapResolution(attr.GetStr("ShadowResolution", "High")) );

		bool bShadowsAffectDynamic = attr.GetBool("ShadowsAffectDynamic", true);
		bool bShadowsAffectStatic = attr.GetBool("ShadowsAffectStatic", true);
		tObjectVariabilityFlag lFlags =0;
		if(bShadowsAffectDynamic)	lFlags |= eObjectVariabilityFlag_Dynamic;
		if(bShadowsAffectStatic)	lFlags |= eObjectVariabilityFlag_Static;
		pLight->SetShadowCastersAffected(lFlags);

		//////////////////////
		// Backwards compitabilty:
		float fDefaultFadeOn = attr.GetFloat("FlickerOnFadeLength",0);
		float fDefaultFadeOff = attr.GetFloat("FlickerOffFadeLength",0);

		pLight->SetFlickerActive(attr.GetBool("FlickerActive", false));
		pLight->SetFlicker(
			attr.GetColor("FlickerOffColor"),
			fFlickerOffValue,

			attr.GetFloat("FlickerOnMinLength"),
			attr.GetFloat("FlickerOnMaxLength"),
			attr.GetStr("FlickerOnSound"),
			attr.GetStr("FlickerOnPS"),

			attr.GetFloat("FlickerOffMaxLength"),
			attr.GetFloat("FlickerOffMinLength"),
			attr.GetStr("FlickerOffSound"),
			attr.GetStr("FlickerOffPS"),

			attr.GetBool("FlickerFade"),
			attr.GetFloat("FlickerOnFadeMinLength", fDefaultFadeOn),
			attr.GetFloat("FlickerOnFadeMaxLength", fDefaultFadeOn),

			attr.GetFloat("FlickerOffFadeMinLength", fDefaultFadeOff),
			attr.GetFloat("FlickerOffFadeMaxLength", fDefaultFadeOff)
			);
 

		kEndWorldEntityLoad(pLight);
	}
	

	//-----------------------------------------------------------------------

	int glDecalNumOfElements[4] = {4,3,3,4};
	eVertexBufferElement glDecalElementType[4] = {	eVertexBufferElement_Position, 
													eVertexBufferElement_Normal,
													eVertexBufferElement_Texture0,
													eVertexBufferElement_Texture1Tangent};
	cMesh* cEngineFileLoading::LoadDecalMeshHelper(tinyxml2::XMLElement* apElement, cGraphics* apGraphics, cResources* apResources, const tString& asName, const tString& asMaterial, const cColor& aColor)
	{
		////////////////////////////////
		//Load Vertex data
		if(apElement==NULL)return NULL;

		int lNumOfVtx = GetAttributeInt(apElement, "NumVerts", 0);
		int lNumOfIdx = GetAttributeInt(apElement, "NumInds", 0);

		if(lNumOfIdx <=0 || lNumOfVtx<=0)
		{
			Warning("Decal %s is missing geometry, skipping!\n", asName.c_str());
			return NULL;
		}

		tinyxml2::XMLElement *pDataArrayElem[4];
		pDataArrayElem[0] = apElement->FirstChildElement("Positions");
		pDataArrayElem[1] = apElement->FirstChildElement("Normals");
		pDataArrayElem[2] = apElement->FirstChildElement("TexCoords");
		pDataArrayElem[3] = apElement->FirstChildElement("Tangents");
		tinyxml2::XMLElement *pIndicesElem = apElement->FirstChildElement("Indices");

		tFloatVec vDataArrays[4];
		tIntVec vIdxArray;
		tString sSepp=" ";
		for(int i=0; i<4; ++i)
		{
			vDataArrays->reserve(lNumOfVtx * glDecalNumOfElements[i]);
			cString::GetFloatVec(GetAttributeString(pDataArrayElem[i], "Array"), vDataArrays[i],&sSepp);
		}
		vIdxArray.reserve(lNumOfIdx);
		cString::GetIntVec(GetAttributeString(pIndicesElem, "Array"), vIdxArray,&sSepp);
	
		//////////////////////////////////
		// Create vertex buffer
		cVertexBuffer *pVtxBuffer = new cVertexBuffer(eVertexBufferType_Software, eVertexBufferDrawType_Tri, 
																					eVertexBufferUsageType_Static,lNumOfVtx, lNumOfIdx);

		//Create arrays	
		for(int i=0; i<4; ++i)
			pVtxBuffer->CreateElementArray(glDecalElementType[i],eVertexBufferElementFormat_Float, glDecalNumOfElements[i]);
		pVtxBuffer->CreateElementArray(eVertexBufferElement_Color0,eVertexBufferElementFormat_Float,4);
		
		//Copy the data!
		// TODO: This needs to be made faster so that data is loaded directly into mesh!
		for(int vtx=0; vtx<lNumOfVtx; ++vtx)
		{
			for(int i=0; i<4; ++i)
			{
				float *pData = &vDataArrays[i][vtx*glDecalNumOfElements[i]];

				if(glDecalNumOfElements[i]==2)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],0) );
				else if(glDecalNumOfElements[i]==3)
					pVtxBuffer->AddVertexVec3f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]) );
				else if(glDecalNumOfElements[i]==4)
					pVtxBuffer->AddVertexVec4f(glDecalElementType[i], cVector3f(pData[0],pData[1],pData[2]),pData[3]);
			}

			pVtxBuffer->AddVertexColor(eVertexBufferElement_Color0, aColor);
		}

		for(int i=0; i<lNumOfIdx; ++i)
			pVtxBuffer->AddIndex(vIdxArray[i]);

		//Compile
		pVtxBuffer->Compile(0);
		
		/////////////////////////
		// Create the mesh
		cMesh *pMesh = hplNew( cMesh, (asName, _W(""), apResources->GetMaterialManager(), apResources->GetAnimationManager()) );
		pMesh->AddReference(); // hand-built mesh: take the one owning reference the entity drops

		cSubMesh *pSubMesh = pMesh->CreateSubMesh("Main");
		
		pSubMesh->SetMaterial(apResources->GetMaterialManager()->CreateMaterial(asMaterial));
		pSubMesh->SetVertexBuffer(pVtxBuffer);
		pSubMesh->SetMaterialName(asMaterial);


		return pMesh;
	}

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cEngineFileLoading::SetupWorldEntity(iEntity3D *apEntity, tinyxml2::XMLElement* apElement)
	{
		if(apEntity==NULL) return;

		int lID = GetAttributeInt(apElement, "ID");
		cVector3f vPosition = GetAttributeVector3f(apElement, "WorldPos",0);
		cVector3f vScale = GetAttributeVector3f(apElement, "Scale",1);
		cVector3f vRotation = GetAttributeVector3f(apElement, "Rotation",0);

		cMatrixf mtxTransform = cMath::MatrixMul(cMath::MatrixRotate(vRotation, eEulerRotationOrder_XYZ),cMath::MatrixScale(vScale));
		mtxTransform.SetTranslation(vPosition);

		apEntity->SetMatrix(mtxTransform);
		apEntity->SetUniqueID(lID);
	}

    //-----------------------------------------------------------------------
}
