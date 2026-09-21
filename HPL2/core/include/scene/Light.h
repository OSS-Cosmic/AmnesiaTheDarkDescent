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
#include "scene/LightState.h"
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

	// eLightModel and the per-backend tuning live in scene/LightState.h: a light
	// carries BOTH tunings and resolves one per query.

	//------------------------------------------

	typedef std::map<iRenderable*, int> tShadowCasterCacheMap;
	typedef tShadowCasterCacheMap::iterator tShadowCasterCacheMapIt;

	//------------------------------------------

	class cLightBillboardConnection
	{
	public:
		cBillboard *mpBillboard;
		cColor mBaseColor;
		// What the billboard's renderer mask was before the light took the
		// ray-traced bit out of it, so RemoveBillboard can hand it back.
		unsigned mlRendererMaskBeforeConnect;
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
		eLightModel GetLightModel() const { return GetActiveLightModel();}

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
		// ray-traced lights); GetDestIntensity returns its destination.
		void FadeTo(const cColor& aCol, float afIntensity, float afTime);
		void StopFading();
		bool IsFading();
		// Finishes an in-flight fade where it stands. The destination is held in
		// the ACTIVE backend's units, so a backend switch calls this BEFORE the
		// world flips -- afterwards the same number means something else.
		void SnapFadeToDestination();
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

		// Resolved against the active tuning, like GetFlickerOnValue: the light
		// remembers the colour DRIVE it returns to, not one backend's colour.
		cColor GetFlickerOnColor() const;
		// Flicker endpoints of the animated value (radius on legacy lights, intensity on ray-traced lights).
		// Resolved for the active backend: each tuning keeps its own flicker
		// depth, and the shared level is what actually moves between them.
		float GetFlickerOffValue() const;
		float GetFlickerOnValue() const;

		// The shared drive: 1 is each tuning's authored ON value, 0 its authored
		// flicker OFF value. Fades and flicker move this, so both backends stay
		// in proportion and a switch mid-fade is coherent.
		float GetLevel() const { return mState.mfLevel; }
		void SetLevel(float afLevel);
		// The level a flicker returns to, i.e. where "on" sits.
		float GetFlickerOnLevel() const { return mfFlickerOnLevel; }
		void SetFlickerOnLevel(float afLevel) { mfFlickerOnLevel = afLevel; }

		//////////////////////////
		//Properties
		// Resolved for the backend this light is driving: the authored colour of
		// the active tuning, moved by any colour fade.
		const cColor& GetDiffuseColor() const { return Resolved().mDiffuseColor; }
		// The same resolution against one named tuning, whichever is active. For
		// a shape that only one backend has a tuning for (box lights).
		cColor GetDiffuseColorFor(eLightModel aModel) const;
		void SetDiffuseColor(cColor aColor);
		
		const cColor&  GetDefaultDiffuseColor() const { return Resolved().mDefaultDiffuseColor;}
		void SetDefaultDiffuseColor(const cColor& aColor);
		
		const cColor& GetSpecularColor(){ return mSpecularColor; }
		void SetSpecularColor(cColor aColor){ mSpecularColor = aColor; }

		bool GetCastShadows() const { return Resolved().mbCastShadows;}
		void SetCastShadows(bool afX);

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
		
		// Ray-traced intensity; unused by legacy lights.
		virtual void SetIntensity(float afX);
		float GetIntensity() const {return Resolved().mfIntensity;}

		// Reach: where attenuation ends. The retail Radius on legacy lights.
		virtual void SetRadius(float afX);
		float GetRadius() const { return Resolved().mfReach; }

		// Value animated by fades and flicker: radius on legacy lights,
		// intensity on ray-traced lights.
		float GetAnimatedValue() const;
		void SetAnimatedValue(float afX);

		// Also invalidates the bounds so the render container picks up the change.
		void SetRendererMask(unsigned aMask) override;

		void SetSourceRadius(float afX);
		float GetSourceRadius() const { return Resolved().mfSourceRadius; }

		// A Redux light whose map gives no Radius derives its reach from the
		// intensity and colour, and keeps deriving it as fades, flicker and
		// scripts change them -- so a light a script turns up from zero reaches.
		void SetReachFollowsIntensity(bool abX);

		// The authored tuning of ONE backend. Both are resident; the active one
		// is picked per query, so nothing has to be reloaded to change backend.
		const cLightTuningState& GetTuning(eLightModel aModel) const { return mState.Tuning(aModel); }
		void SetTuning(eLightModel aModel, const cLightTuningState& aTuning);
		// Which tuning is driving this light right now.
		eLightModel GetActiveLightModel() const;
		bool GetReachFollowsIntensity() const { return Resolved().mbReachFollowsIntensity; }

		void UpdateLight(float afTimeStep);

		void SetWorld(cWorld *apWorld);
		// The world's renderer backend changed, so a different tuning drives
		// this light now. Re-resolves and re-fires everything that depends on
		// the values: bounds, the spot projection, billboard tint, visibility.
		void OnRendererBackendChanged();


	protected:
		void OnFlickerOff();
		void OnFlickerOn();
		void OnSetDiffuse();

		// Puts a whole colour drive back, the way SetLevel puts a level back:
		// backend neutral, so the flicker's ON colour is not a snapshot of one
		// backend's resolved colour. SetDiffuseColor is the deriving path.
		void SetColorDrive(const cLightColorDrive& aDrive);
		// The fade's destination applied outright; SnapFadeToDestination and the
		// end of UpdateLight's fade both land here.
		void ApplyFadeDestination();

		// The active tuning with the shared level and colour drive applied.
		const cLightTuningState& Resolved() const;
		void InvalidateResolved();
		// Writes an authored value into the active tuning and re-derives the
		// passive one when the map never authored it.
		cLightTuningState& AuthoringTuning();
		void PromotePassiveTuning();
		// Bounds and render-container bookkeeping after a tuning change.
		void OnTuningChanged();
		// A subclass cache that keys off the resolved tuning -- the spot's
		// projection and frustum, which otherwise only invalidate on SetRadius.
		virtual void OnResolvedTuningChanged() {}
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

		cColor mSpecularColor;

		// Both backends' authored tuning, plus the level/colour drive shared
		// between them.
		cLightState mState;
		// Which tuning drives the light. Latched from the light class for now;
		// it becomes a query against the world's renderer backend.
		eLightModel mActiveModel = eLightModel_Legacy;

		// Resolving a tuning derives the reach, which costs three pow() and a
		// sqrt, and GetRadius() runs in per-light per-frame loops -- so the
		// result is cached and invalidated on every write.
		mutable cLightTuningState mResolved;
		mutable bool mbResolvedDirty = true;
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
		// The OFF endpoint lives in each tuning; the light only remembers the
		// level the flicker returns TO.
		float mfFlickerOnLevel;
		bool mbFlickerFade;
		float mfFlickerOnFadeMinLength;
		float mfFlickerOnFadeMaxLength;
		float mfFlickerOffFadeMinLength;
		float mfFlickerOffFadeMaxLength;

		cLightColorDrive mFlickerOnColorDrive;

		bool mbFlickerOn;
		float mfFlickerTime;
		float mfFlickerStateLength;
	};

	typedef std::list<iLight*> tLightList;
	typedef tLightList::iterator tLightListIt;
};
#endif // HPL_LIGHT_H
