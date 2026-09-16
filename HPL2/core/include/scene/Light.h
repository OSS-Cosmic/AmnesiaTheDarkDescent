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

#ifndef HPL_LIGHT_H
#define HPL_LIGHT_H

#include "scene/Entity3D.h"
#include "graphics/GraphicsTypes.h"
#include "resources/ResourceBase.h"
#include "graphics/Renderable.h"

namespace tinyxml2 { class XMLElement; }

namespace hpl {

	//------------------------------------------

	class cRenderSettings;
	class cCamera;
	class cFrustum;
	class iGpuProgram;
	class Image;
	class cTextureManager;
	class cResources;
	class cFileSearcher;
	class cBillboard;
	class cSectorVisibilityContainer;
	class cWorld;
	
	//------------------------------------------

	// Intensity (PBR gain) that makes a point/spot light's radiance reach the
	// engine's cull floor at exactly `afReach` metres, given its lit diffuse
	// colour. The inverse of the reach derivation EngineFileLoading applies to
	// map lights:
	//     reach2     = maxChannel(linear colour) * intensity / kLightRadianceFloor - kPointLightSourceRadiusSq
	//     intensity  = (reach2 + kPointLightSourceRadiusSq) * kLightRadianceFloor / maxChannel(linear colour)
	//
	// Use it for lights created in code from an authored RADIUS, where the
	// legacy value means "how far the glow carries" and there is no authored
	// intensity to go with it. Feeding the radius straight into SetIntensity
	// instead treats a distance as a gain, which is arbitrary: the brightness
	// then depends on a number that was never tuned as one, and the glow's real
	// extent no longer matches the authored radius.
	//
	// `aLitDiffuseColor` must be the colour the light has when it is ON. A light
	// that fades in from black has to pass its target colour, not its current
	// one - a black colour carries no brightness to solve against and the
	// function falls back to returning `afReach`.
	float DeriveLightIntensityForReach(float afReach, const cColor &aLitDiffuseColor);

	//------------------------------------------
	
	enum eLightType
	{
		eLightType_Point,
		eLightType_Spot,
		eLightType_Area,
		eLightType_Box,
		eLightType_LastEnum
	};

	enum eShadowVolumeType
	{
		eShadowVolumeType_None,
		eShadowVolumeType_ZPass,
		eShadowVolumeType_ZFail,
		eShadowVolumeType_LastEnum,
	};

	// Which renderer a light class belongs to. Legacy lights (cLightPointLegacy,
	// cLightSpotLegacy, cLightBoxLegacy) carry the retail Radius and render in
	// Standard. Redux lights (cLightPoint, cLightSpot, cLightArea) carry
	// Intensity, Radius (reach) and SourceRadius and feed the ray-traced grid.
	enum eLightModel
	{
		eLightModel_Legacy,
		eLightModel_Overdrive,
	};

	//------------------------------------------

	typedef std::map<iRenderable*, int> tShadowCasterCacheMap;
	typedef tShadowCasterCacheMap::iterator tShadowCasterCacheMapIt;

	//------------------------------------------

	class cLightBillboardConnection
	{
	public:
		cBillboard *mpBillboard;
		cColor mBaseColor;
	};

	//------------------------------------------

	class iLight : public iRenderable
	{
	public:
		iLight(tString asName, cResources *apResources);
		virtual ~iLight();

		void UpdateLogic(float afTimeStep);

		bool CheckObjectIntersection(iRenderable *apObject);
		
		eLightType GetLightType(){ return mLightType;}
		eLightModel GetLightModel() const { return mLightModel;}

		// Stable per-type GPU light slot, assigned by the owning cWorld's
		// light-slot pool at creation and kept for the light's lifetime (returned
		// on destroy). Makes the light's packed GPU id (packLightId(type, slot))
		// identical every frame — the stable identity ReSTIR DI reservoirs persist
		// across temporal/spatial reuse. UINT32_MAX until assigned (and for light
		// types not uploaded to the GPU, e.g. box lights).
		uint32_t GetGpuLightSlot() const { return mGpuLightSlot; }
		void SetGpuLightSlot(uint32_t aSlot){ mGpuLightSlot = aSlot; }

		// Image* binding API.
		void SetFalloffMap(Image* apImage);
		Image* GetFalloffImage() const;

		void SetGoboTexture(Image* apImage);
		Image* GetGoboImage() const;

		///////////////////////////////
		//iEntity implementation
		tString GetEntityType(){ return "iLight";}

		virtual bool IsVisible();
		void OnChangeVisible();
		
		///////////////////////////////
		//Renderable implementation:
		cMaterial *GetMaterial(){ return NULL;}
		cVertexBuffer* GetVertexBuffer(){ return NULL;}

		eRenderableType GetRenderType(){ return eRenderableType_Light;}

		cBoundingVolume* GetBoundingVolume();

		int GetMatrixUpdateCount(){ return GetTransformUpdateCount();}

		cMatrixf* GetModelMatrix(cFrustum* apFrustum);

		void LoadXMLProperties(const tString asFile);

		void AttachBillboard(cBillboard *apBillboard, const cColor &aBaseColor);
		void RemoveBillboard(cBillboard *apBillboard);
		// Re-sync one connected billboard's colour (base × this light's diffuse)
		// and visibility — used by the editor when the connection's colour
		// updates so the billboard tracks the light instead of desyncing.
		void UpdateBillboard(cBillboard* apBillboard, const cColor& aBaseColor);
		std::vector<cLightBillboardConnection>* GetBillboardVec(){ return &mvBillboards;}

		//////////////////////////
		//Shadow caster cache
		void AddShadowCaster(iRenderable *apObject);
		bool ShadowCasterIsValid(iRenderable *apObject);
		bool ShadowCastersAreUnchanged(const tRenderableVec &avObjects);
		void SetShadowCasterCacheFromVec(const tRenderableVec &avObjects);
		void ClearShadowCasterCache();

        //////////////////////////
		//Fading
		// Fades the animated value (radius on legacy lights, intensity on
		// Overdrive lights); GetDestIntensity returns its destination.
		void FadeTo(const cColor& aCol, float afIntensity, float afTime);
		void StopFading();
		bool IsFading();
		cColor GetDestColor(){ return mDestCol;}
		float GetDestIntensity(){ return mfDestIntensity;}


		//////////////////////////
		//FLickering
		void SetFlickerActive(bool abX);
		bool GetFlickerActive(){return mbFlickering;}

		void SetFlicker(const cColor& aOffCol, float afOffIntensity,
			float afOnMinLength, float afOnMaxLength,const tString &asOnSound,const tString &asOnPS,
			float afOffMinLength, float afOffMaxLength,const tString &asOffSound,const tString &asOffPS,
			bool abFade,	float afOnFadeMinLength, float afOnFadeMaxLength, 
							float afOffFadeMinLength, float afOffFadeMaxLength);

		tString GetFlickerOffSound(){ return msFlickerOffSound;}
		tString GetFlickerOnSound(){ return msFlickerOnSound;}
		tString GetFlickerOffPS(){ return msFlickerOffPS;}
		tString GetFlickerOnPS(){ return msFlickerOnPS;}
		float GetFlickerOnMinLength(){ return mfFlickerOnMinLength;}
		float GetFlickerOffMinLength(){ return mfFlickerOffMinLength;}
		float GetFlickerOnMaxLength(){ return mfFlickerOnMaxLength;}
		float GetFlickerOffMaxLength(){ return mfFlickerOffMaxLength;}
		cColor GetFlickerOffColor(){ return mFlickerOffColor;}
		bool GetFlickerFade(){ return mbFlickerFade;}
		float GetFlickerOnFadeMinLength(){ return mfFlickerOnFadeMinLength;}
		float GetFlickerOnFadeMaxLength(){ return mfFlickerOnFadeMaxLength;}
		float GetFlickerOffFadeMinLength(){ return mfFlickerOffFadeMinLength;}
		float GetFlickerOffFadeMaxLength(){ return mfFlickerOffFadeMaxLength;}

		cColor GetFlickerOnColor(){ return mFlickerOnColor;}
		// Flicker endpoints of the animated value (radius on legacy lights, intensity on Overdrive lights).
		float GetFlickerOffValue() const { return mfFlickerOffValue; }
		float GetFlickerOnValue() const { return mfFlickerOnValue; }

		//////////////////////////
		//Properties
		const cColor& GetDiffuseColor(){ return mDiffuseColor; }
		void SetDiffuseColor(cColor aColor);
		
		const cColor&  GetDefaultDiffuseColor(){ return mDefaultDiffuseColor;}
		void SetDefaultDiffuseColor(const cColor& aColor) { mDefaultDiffuseColor = aColor; }
		
		const cColor& GetSpecularColor(){ return mSpecularColor; }
		void SetSpecularColor(cColor aColor){ mSpecularColor = aColor; }

		bool GetCastShadows(){ return mbCastShadows;}
		void SetCastShadows(bool afX){ mbCastShadows = afX;}

		tObjectVariabilityFlag GetShadowCastersAffected(){ return mlShadowCastersAffected;}
		void SetShadowCastersAffected(tObjectVariabilityFlag alX){ mlShadowCastersAffected = alX;}

		inline eShadowMapResolution GetShadowMapResolution() const{ return mShadowMapResolution;}
		inline void SetShadowMapResolution(eShadowMapResolution aQuality){ mShadowMapResolution  = aQuality;}

		inline float GetShadowMapBlurAmount() const{ return mfShadowMapBlurAmount;}
		inline void SetShadowMapBlurAmount(float afX){ mfShadowMapBlurAmount  = afX;}

		inline bool GetOcclusionCullShadowCasters() const{ return mbOcclusionCullShadowCasters;}
		inline void SetOcclusionCullShadowCasters(bool abX){ mbOcclusionCullShadowCasters  = abX;}

		float GetShadowMapBiasMul(){ return mfShadowMapBiasMul;}
		float GetShadowMapSlopeScaleBiasMul(){ return mfShadowMapSlopeScaleBiasMul;}
		void SetShadowMapBiasMul(float afX){ mfShadowMapBiasMul = afX;}
		void SetShadowMapSlopeScaleBiasMul(float afX){ mfShadowMapSlopeScaleBiasMul = afX;}
		
		// Overdrive intensity; unused by legacy lights.
		virtual void SetIntensity(float afX);
		float GetIntensity(){return mfIntensity;}

		// Reach: where attenuation ends. The retail Radius on legacy lights.
		virtual void SetRadius(float afX);
		float GetRadius() { return mfRadius; }

		// Value animated by fades and flicker: radius on legacy lights,
		// intensity on Overdrive lights.
		float GetAnimatedValue() const;
		void SetAnimatedValue(float afX);

		// Also invalidates the bounds so the render container picks up the change.
		void SetRendererMask(unsigned aMask) override;

		void SetSourceRadius(float afX);
		float GetSourceRadius(){ return mfSourceRadius; }

		// A Redux light whose map gives no Radius derives its reach from the
		// intensity and colour, and keeps deriving it as fades, flicker and
		// scripts change them -- so a light a script turns up from zero reaches.
		void SetReachFollowsIntensity(bool abX);
		bool GetReachFollowsIntensity() const { return mbReachFollowsIntensity; }

		void UpdateLight(float afTimeStep);

		void SetWorld(cWorld *apWorld){ mpWorld = apWorld;}


	protected:
		void OnFlickerOff();
		void OnFlickerOn();
		void OnSetDiffuse();
		void UpdateDerivedReach();
        virtual void ExtraXMLProperties(tinyxml2::XMLElement *apMainElem){}
		virtual void UpdateBoundingVolume()=0;
		
		eLightType mLightType;
		uint32_t mGpuLightSlot = UINT32_MAX;   // stable per-type GPU slot (cWorld light-slot pool)

		cTextureManager *mpTextureManager;
		cFileSearcher *mpFileSearcher;
		cWorld *mpWorld;

		// Image* texture storage.
		SharedResourceHandle<Image> m_falloffMap;
		SharedResourceHandle<Image> m_goboImage;

		eShadowMapResolution mShadowMapResolution;
		float mfShadowMapBlurAmount;
		bool mbOcclusionCullShadowCasters;

		std::vector<cLightBillboardConnection> mvBillboards;

		cColor mDiffuseColor;
		cColor mDefaultDiffuseColor;

		cColor mSpecularColor;
		float mfIntensity;
		float mfRadius;
		float mfSourceRadius;
		eLightModel mLightModel = eLightModel_Legacy;
		bool mbReachFollowsIntensity = false;

		bool mbCastShadows;
		tObjectVariabilityFlag mlShadowCastersAffected;

		tShadowCasterCacheMap m_mapShadowCasterCache;

		float mfShadowMapBiasMul;
		float mfShadowMapSlopeScaleBiasMul;

		///////////////////////////
		//Fading.
		cColor mColAdd;
		float mfIntensityAdd;
		cColor mDestCol;
		float mfDestIntensity;
		float mfFadeTime;

		///////////////////////////
		//Flicker
		bool mbFlickering;
		tString msFlickerOffSound;
		tString msFlickerOnSound;
		tString msFlickerOffPS;
		tString msFlickerOnPS;
		float mfFlickerOnMinLength;
		float mfFlickerOffMinLength;
		float mfFlickerOnMaxLength;
		float mfFlickerOffMaxLength;
		cColor mFlickerOffColor;
		float mfFlickerOffValue;
		bool mbFlickerFade;
		float mfFlickerOnFadeMinLength;
		float mfFlickerOnFadeMaxLength;
		float mfFlickerOffFadeMinLength;
		float mfFlickerOffFadeMaxLength;

		cColor mFlickerOnColor;
		float mfFlickerOnValue;

		bool mbFlickerOn;
		float mfFlickerTime;
		float mfFlickerStateLength;
	};

	typedef std::list<iLight*> tLightList;
	typedef tLightList::iterator tLightListIt;
};
#endif // HPL_LIGHT_H
