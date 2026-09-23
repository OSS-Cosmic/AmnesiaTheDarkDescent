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

#include "scene/Light.h"
#include "scene/LightParameters.h"

#include "system/String.h"

#include "resources/XmlHelper.h"
#include <tinyxml2.h>


#include "system/LowLevelSystem.h"
#include "system/Platform.h"

#include "math/Math.h"

#include "graphics/Color.h"   // sRGBToLinear

//The light-model constants the reach derivation is defined against, shared with
//the shaders rather than mirrored (amnesia/slang is on the include path).
#include "Constants.h"        // kLightRadianceFloor, kPointLightSourceRadiusSq

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "resources/FileSearcher.h"
#include "graphics/Image.h"

#include "scene/ParticleSystem.h"
#include "scene/World.h"
#include "scene/BillBoard.h"
#include "scene/SoundEntity.h"
#include "scene/MeshEntity.h"
#include "scene/Camera.h"

#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/SubMesh.h"
#include "graphics/Renderer.h"



namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	float DeriveLightIntensityForReach(float afReach, const cColor &aLitDiffuseColor)
	{
		return DeriveLightIntensityForReach(afReach, aLitDiffuseColor.r, aLitDiffuseColor.g,
		                                    aLitDiffuseColor.b);
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iLight::iLight(tString asName, cResources *apResources) : iRenderable(asName)
	{
		///////////////////////////////
		//Managers and highlevel init
		mpWorld = NULL;
		mpTextureManager = apResources->GetTextureManager();
		mpFileSearcher = apResources->GetFileSearcher();

		///////////////////////////////
		//Render properties
		mbApplyTransformToBV = false;

		mShadowMapResolution = eShadowMapResolution_High;
		mfShadowMapBlurAmount = 6.0f;
		mbOcclusionCullShadowCasters = false;

		mfShadowMapBiasMul = 1;
		mfShadowMapSlopeScaleBiasMul = 1;
			
		///////////////////////////////
		//Fade and flicker init
		mSpecularColor = 0;
		mlShadowCastersAffected = eObjectVariabilityFlag_All;
		// Both tunings start dark and unauthored; the loader or the creating
		// code fills them in.
		mState = cLightState();
		for(int i=0; i<2; ++i)
		{
			cLightTuningState& tuning = mState.Tuning(i==0 ? eLightModel_Legacy : eLightModel_RayTraced);
			tuning.mfIntensity = 0;
			tuning.mfReach = 0;
			tuning.mfSourceRadius = 0;
			tuning.mfOnValue = 0;
			tuning.mfOffValue = 0;
			tuning.mDiffuseColor = 0;
			tuning.mDefaultDiffuseColor = 0;
			tuning.mbCastShadows = false;
		}
		InvalidateResolved();
		mfFadeTime=0;
		mbFlickering = false;
	
		mfFlickerStateLength = 0;
		mfFlickerOnLevel = 1.0f;

		mfFadeTime =0;

		///////////////////////////////
		//Data init
		// Forward+ uses a clamp-to-edge static sampler at the falloff binding.
		SetFalloffMap(mpTextureManager->Create1DImage("core_falloff_linear", false).Release());
	}

	//-----------------------------------------------------------------------

	iLight::~iLight()
	{
		// m_falloffMap / m_goboImage (SharedResourceHandle) free themselves.
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void iLight::OnChangeVisible()
	{ 
		for(size_t i =0; i<mvBillboards.size(); ++i)
		{
			mvBillboards[i].mpBillboard->SetVisible(mbIsVisible);
		}
	}

	bool iLight::IsVisible()
	{ 
		// Resolved for the active backend: a light authored dark on one backend
		// is invisible there and lit on the other.
		const cColor& diffuse = GetDiffuseColor();
		if(diffuse.r <=0 && diffuse.g <=0 && diffuse.b <=0 && diffuse.a <=0) 
			return false;
		if(GetAnimatedValue() <= 0) return false;

		return mbIsVisible; 
	}

	//-----------------------------------------------------------------------


	void iLight::SetDiffuseColor(cColor aColor)
	{
		const cColor was = GetDiffuseColor();
		const bool bWasVisble = (was.r >0 || was.g >0 || was.b >0 || was.a >0);

		// A DRIVE, not an authoring write. Scripts, fades and flicker all come
		// through here, and they mean "the light looks like this now" -- so it
		// is expressed as a scale over the active tuning's authored colour and
		// applied to BOTH tunings. Author a colour with SetTuning instead.
		//
		// Writing the active tuning here instead would leave the passive one at
		// its authored colour, and every lamp a script had turned off would
		// light up again the moment the renderer backend changed.
		const cLightTuningState& authored = mState.Tuning(GetActiveLightModel());
		const float channelsAuthored[4] = { authored.mDiffuseColor.r, authored.mDiffuseColor.g,
											authored.mDiffuseColor.b, authored.mDiffuseColor.a };
		const float channelsWanted[4] = { aColor.r, aColor.g, aColor.b, aColor.a };

		// A channel the tuning authored at zero has no ratio to scale, so a
		// non-zero request there can only be expressed outright.
		float scale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool bNeedsAbsolute = false;
		for(int i=0; i<4; ++i)
		{
			if(DeriveLightColorScale(channelsAuthored[i], channelsWanted[i], &scale[i])==false)
				bNeedsAbsolute = true;
		}

		if(bNeedsAbsolute)
		{
			mState.mColorDrive.mbColorAbsolute = true;
			mState.mColorDrive.mAbsoluteColor = aColor;
		}
		else
		{
			mState.mColorDrive.mbColorAbsolute = false;
			mState.mColorDrive.mColorScale = cColor(scale[0], scale[1], scale[2], scale[3]);
		}

		// The reach may follow the colour, so the bounds can move with it.
		OnTuningChanged();

		const bool bVisible = (aColor.r >0 || aColor.g >0 || aColor.b >0 || aColor.a >0);
		
		//Check if the light changed its visibility
		if(mbIsVisible && bVisible != bWasVisble)
		{
			OnChangeVisible();
		}

		OnSetDiffuse();
	}

	//-----------------------------------------------------------------------

	void iLight::UpdateLight(float afTimeStep)
	{	
		/////////////////////////////////////////////
		// Fade
		if(mfFadeTime>0)
		{
			//Log("Fading: %f / %f\n",afTimeStep,mfFadeTime);

			SetAnimatedValue(GetAnimatedValue() + mfIntensityAdd*afTimeStep);

			cColor faded = GetDiffuseColor();
			faded.r += mColAdd.r*afTimeStep;
			faded.g += mColAdd.g*afTimeStep;
			faded.b += mColAdd.b*afTimeStep;
			faded.a += mColAdd.a*afTimeStep;
			SetDiffuseColor(faded);

			mfFadeTime-=afTimeStep;

			//Set the dest values.
			if(mfFadeTime<=0)
				ApplyFadeDestination();
		}

		/////////////////////////////////////////////
		// Flickering
		if(mbFlickering && mfFadeTime<=0)
		{	
			//////////////////////
			//On
			if(mbFlickerOn)
			{
				if(mfFlickerTime >= mfFlickerStateLength)
				{
					mbFlickerOn = false;
					if(!mbFlickerFade)
					{
						SetDiffuseColor(mFlickerOffColor);
						// Level 0: each tuning drops to ITS OWN authored off
						// value, so the two backends keep their own depth.
						SetLevel(0.0f);
					}
					else
					{
						FadeTo(mFlickerOffColor, GetFlickerOffValue(),
							cMath::RandRectf(mfFlickerOffFadeMinLength, mfFlickerOffFadeMaxLength));
					}
					//Sound
					if(msFlickerOffSound!=""){
						cSoundEntity *pSound = mpWorld->CreateSoundEntity("FlickerOff",
																			msFlickerOffSound,true);
						if(pSound)
						{
							pSound->SetIsSaved(false);
							pSound->SetPosition(GetWorldPosition());
						}
					}

					OnFlickerOff();

					mfFlickerTime =0;
					mfFlickerStateLength = cMath::RandRectf(mfFlickerOffMinLength,mfFlickerOffMaxLength);
				}
			}
			//////////////////////
			//Off
			else 
			{
				if(mfFlickerTime >= mfFlickerStateLength)
				{
					mbFlickerOn = true;
					if(!mbFlickerFade)
					{
						SetColorDrive(mFlickerOnColorDrive);
						SetLevel(mfFlickerOnLevel);
					}
					else
					{
						FadeTo(GetFlickerOnColor(), GetFlickerOnValue(),
							cMath::RandRectf(mfFlickerOnFadeMinLength, mfFlickerOnFadeMaxLength));
					}
					if(msFlickerOnSound!=""){
						cSoundEntity *pSound = mpWorld->CreateSoundEntity("FlickerOn", msFlickerOnSound,true);
						if(pSound)
						{
							pSound->SetIsSaved(false);
							pSound->SetPosition(GetWorldPosition());
						}
					}

					OnFlickerOn();

					mfFlickerTime =0;
					mfFlickerStateLength = cMath::RandRectf(mfFlickerOnMinLength,mfFlickerOnMaxLength);
				}
			}

			mfFlickerTime += afTimeStep;
		}

		/*Log("Time: %f Length: %f FadeTime: %f Color: (%f %f %f %f)\n",mfFlickerTime, mfFlickerStateLength,
		mfFadeTime,
		diffuse.r,diffuse.g,
		diffuse.b,diffuse.a);*/
	}

	//-----------------------------------------------------------------------

	void iLight::FadeTo(const cColor& aCol, float afIntensity, float afTime)
	{
		if(afTime<=0) afTime = 0.0001f;

		mfFadeTime = afTime;

		const cColor from = GetDiffuseColor();
		mColAdd.r = (aCol.r - from.r)/afTime;
		mColAdd.g = (aCol.g - from.g)/afTime;
		mColAdd.b = (aCol.b - from.b)/afTime;
		mColAdd.a = (aCol.a - from.a)/afTime;

		mfIntensityAdd = (afIntensity - GetAnimatedValue())/afTime;

		mfDestIntensity = afIntensity;
		mDestCol = aCol;
	}

	void iLight::ApplyFadeDestination()
	{
		mfFadeTime = 0;
		SetDiffuseColor(mDestCol);
		SetAnimatedValue(mfDestIntensity);
	}

	// The destination is in the units of the backend that issued the fade --
	// reach in metres on Standard, intensity on ray-traced -- so a backend
	// switch finishes the fade while those units still mean something. The
	// level and colour scale it lands on are shared, and carry over.
	void iLight::SnapFadeToDestination()
	{
		if(mfFadeTime<=0) return;
		ApplyFadeDestination();
	}

	void iLight::StopFading()
	{
		mfFadeTime =0;
	}

	bool iLight::IsFading()
	{
		return mfFadeTime != 0;
	}

	//-----------------------------------------------------------------------

	void iLight::SetFlickerActive(bool abX)
	{
		mbFlickering = abX;
	}

	void iLight::SetFlicker(const cColor& aOffCol, float afOffIntensity,
		float afOnMinLength, float afOnMaxLength,const tString &asOnSound,const tString &asOnPS,
		float afOffMinLength, float afOffMaxLength,const tString &asOffSound,const tString &asOffPS,
		bool abFade,	float afOnFadeMinLength, float afOnFadeMaxLength, 
						float afOffFadeMinLength, float afOffFadeMaxLength)
	{
		mFlickerOffColor = aOffCol;
		// The OFF endpoint belongs to the tuning being driven. The passive one
		// keeps whatever the map authored for it (Re_FlickerOffIntensity), or
		// the value the loader promoted -- either way it holds its own depth.
		{
			cLightTuningState& tuning = mState.Tuning(GetActiveLightModel());
			tuning.mfOffValue = afOffIntensity;
			InvalidateResolved();
		}

		mfFlickerOnMinLength = afOnMinLength;
		mfFlickerOnMaxLength = afOnMaxLength;
		msFlickerOnSound = asOnSound;
		msFlickerOnPS = asOnPS;

		mfFlickerOffMinLength = afOffMinLength;
		mfFlickerOffMaxLength = afOffMaxLength;
		msFlickerOffSound = asOffSound;
		msFlickerOffPS = asOffPS;

		mbFlickerFade = abFade;

		mfFlickerOnFadeMinLength = afOnFadeMinLength;
		mfFlickerOnFadeMaxLength = afOnFadeMaxLength;
		mfFlickerOffFadeMinLength = afOffFadeMinLength;
		mfFlickerOffFadeMaxLength = afOffFadeMaxLength;

		// The drive, not the resolved colour: the passive backend's ON colour is
		// then its own authored one scaled the same way, exactly as the ON value
		// is its own authored value at mfFlickerOnLevel.
		mFlickerOnColorDrive = mState.mColorDrive;
		// Wherever the light sits now is the ON state, expressed as a level so
		// the other backend's ON value is its own authored one.
		mfFlickerOnLevel = mState.mfLevel;

		mbFlickerOn = true;
		mfFlickerTime =0;

		mfFadeTime =0;

		mfFlickerStateLength = cMath::RandRectf(mfFlickerOnMinLength,mfFlickerOnMaxLength);
	}

	//-----------------------------------------------------------------------
	
	bool iLight::CheckObjectIntersection(iRenderable *apObject)
	{
		//Log("------ Checking %s with light %s -----\n",apObject->GetName().c_str(), GetName().c_str());
		//Log(" BV: min: %s max: %s\n",	apObject->GetBoundingVolume()->GetMin().ToString().c_str(),
		//								apObject->GetBoundingVolume()->GetMax().ToString().c_str());
		
		//////////////////////////////////////////////////////////////
		// If the lights cast shadows, cull objects that are in shadow
		if(GetCastShadows())
		{
			return CollidesWithBV(apObject->GetBoundingVolume());
		}
		/////////////////////////////////////////////////
		//Light is not in shadow, do not do any culling
		else
		{
			//Log("No shadow, using BV\n");
			return CollidesWithBV(apObject->GetBoundingVolume());
		}

				
	}

	//-----------------------------------------------------------------------

	// Resolved per query against the world, so changing the world's backend
	// re-derives every light instead of needing a reload. Null-safe: a light
	// resolves its radius inside its own constructor, before cWorld::RegisterLight
	// has handed it a world.
	eLightModel iLight::GetActiveLightModel() const
	{
		if(mpWorld==NULL) return mActiveModel;
		return mpWorld->GetRendererBackend()==eRendererBackend_Standard
			? eLightModel_Legacy : eLightModel_RayTraced;
	}

	//-----------------------------------------------------------------------

	void iLight::SetWorld(cWorld *apWorld)
	{
		mpWorld = apWorld;
		// The world decides which tuning is active, so anything resolved
		// before this point was resolved against the fallback.
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	void iLight::OnRendererBackendChanged()
	{
		const bool bWasVisible = IsVisible();

		OnTuningChanged();
		// The spot's projection and frustum key off the reach and are otherwise
		// only invalidated by SetRadius.
		OnResolvedTuningChanged();
		// Per-backend colour means connected billboards re-tint.
		OnSetDiffuse();

		if(mbIsVisible && IsVisible() != bWasVisible)
			OnChangeVisible();
	}

	//-----------------------------------------------------------------------

	void iLight::InvalidateResolved()
	{
		mbResolvedDirty = true;
	}

	//-----------------------------------------------------------------------

	const cLightTuningState& iLight::Resolved() const
	{
		if(mbResolvedDirty==false) return mResolved;

		const eLightModel model = GetActiveLightModel();
		const cLightTuningState& authored = mState.Tuning(model);
		mResolved = authored;

		// Colour first: a derived reach is a function of it.
		if(mState.mColorDrive.mbColorAbsolute)
		{
			mResolved.mDiffuseColor = mState.mColorDrive.mAbsoluteColor;
		}
		else
		{
			mResolved.mDiffuseColor.r = authored.mDiffuseColor.r * mState.mColorDrive.mColorScale.r;
			mResolved.mDiffuseColor.g = authored.mDiffuseColor.g * mState.mColorDrive.mColorScale.g;
			mResolved.mDiffuseColor.b = authored.mDiffuseColor.b * mState.mColorDrive.mColorScale.b;
			mResolved.mDiffuseColor.a = authored.mDiffuseColor.a * mState.mColorDrive.mColorScale.a;
		}

		// The animated value is the reach on Standard and the intensity on
		// ray-traced; the level moves whichever one this tuning animates.
		const float fAnimated = ResolveLightLevelValue(authored.mfOnValue, authored.mfOffValue,
														mState.mfLevel);
		if(model==eLightModel_RayTraced)
		{
			mResolved.mfIntensity = fAnimated;
			mResolved.mfReach = authored.mbReachFollowsIntensity
				? DeriveLightReach(fAnimated, mResolved.mDiffuseColor.r,
									mResolved.mDiffuseColor.g, mResolved.mDiffuseColor.b)
				: authored.mfReach;
		}
		else
		{
			mResolved.mfReach = fAnimated;
		}

		mbResolvedDirty = false;
		return mResolved;
	}

	//-----------------------------------------------------------------------

	cColor iLight::GetDiffuseColorFor(eLightModel aModel) const
	{
		if(mState.mColorDrive.mbColorAbsolute)
			return mState.mColorDrive.mAbsoluteColor;

		const cColor& authored = mState.Tuning(aModel).mDiffuseColor;
		const cColor& scale = mState.mColorDrive.mColorScale;
		return cColor(authored.r * scale.r, authored.g * scale.g,
					  authored.b * scale.b, authored.a * scale.a);
	}

	//-----------------------------------------------------------------------

	cLightTuningState& iLight::AuthoringTuning()
	{
		cLightTuningState& tuning = mState.Tuning(GetActiveLightModel());
		tuning.mbAuthored = true;
		return tuning;
	}

	//-----------------------------------------------------------------------

	// The passive tuning follows the authored one until the map gives it values
	// of its own -- the same promotion the loader does, kept live so a light a
	// script turns up is not left dark on the other backend.
	void iLight::PromotePassiveTuning()
	{
		const eLightModel active = GetActiveLightModel();
		const eLightModel passive = active==eLightModel_RayTraced ? eLightModel_Legacy
																  : eLightModel_RayTraced;
		cLightTuningState& target = mState.Tuning(passive);
		if(target.mbAuthored || target.mbPresent==false) return;

		const cLightTuningState& source = mState.Tuning(active);
		target.mDiffuseColor = source.mDiffuseColor;
		target.mDefaultDiffuseColor = source.mDefaultDiffuseColor;
		target.mbCastShadows = source.mbCastShadows;

		if(passive==eLightModel_RayTraced)
		{
			// PromoteLegacyLightParameters: the retail radius IS the intensity,
			// and the reach derives from it.
			target.mfIntensity = source.mfReach;
			target.mfOnValue = source.mfOnValue;
			target.mfOffValue = source.mfOffValue;
			target.mbReachFollowsIntensity = true;
			target.mfSourceRadius = 0;
			target.mfReach = DeriveLightReach(target.mfIntensity, target.mDiffuseColor.r,
												target.mDiffuseColor.g, target.mDiffuseColor.b);
		}
		else
		{
			// Back the other way: the retail radius that would reach as far.
			target.mfReach = source.mfIntensity;
			target.mfOnValue = source.mfOnValue;
			target.mfOffValue = source.mfOffValue;
			target.mbReachFollowsIntensity = false;
			target.mfIntensity = source.mfIntensity;
			target.mfSourceRadius = 0;
		}
	}

	//-----------------------------------------------------------------------

	void iLight::OnTuningChanged()
	{
		InvalidateResolved();
		OnResolvedTuningChanged();
		mbUpdateBoundingVolume = true;

		//This is so that the render container is updated.
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	void iLight::SetTuning(eLightModel aModel, const cLightTuningState& aTuning)
	{
		mState.Tuning(aModel) = aTuning;
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	void iLight::SetCastShadows(bool afX)
	{
		cLightTuningState& tuning = AuthoringTuning();
		if(tuning.mbCastShadows == afX) return;

		tuning.mbCastShadows = afX;
		PromotePassiveTuning();
		InvalidateResolved();
	}

	//-----------------------------------------------------------------------

	void iLight::SetDefaultDiffuseColor(const cColor& aColor)
	{
		AuthoringTuning().mDefaultDiffuseColor = aColor;
		InvalidateResolved();
	}

	//-----------------------------------------------------------------------

	void iLight::SetIntensity(float afX)
	{ 
		cLightTuningState& tuning = AuthoringTuning();
		if(tuning.mfIntensity == afX) return;

		tuning.mfIntensity = afX;
		// Authoring the intensity of the tuning that animates it re-bases the
		// level: the light now sits at its own authored ON value.
		if(GetActiveLightModel()==eLightModel_RayTraced)
		{
			tuning.mfOnValue = afX;
			mState.mfLevel = 1.0f;
		}
		PromotePassiveTuning();
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	void iLight::SetReachFollowsIntensity(bool abX)
	{
		cLightTuningState& tuning = AuthoringTuning();
		if(tuning.mbReachFollowsIntensity == abX) return;

		// The derivation is applied when the tuning resolves, so there is
		// nothing to recompute here.
		tuning.mbReachFollowsIntensity = abX;
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	void iLight::SetRadius(float afX)
	{
		cLightTuningState& tuning = AuthoringTuning();
		if (tuning.mfReach == afX) return;

		tuning.mfReach = afX;
		if(GetActiveLightModel()==eLightModel_RayTraced)
		{
			// An authored reach stops following the intensity, as in the loader.
			tuning.mbReachFollowsIntensity = false;
		}
		else
		{
			// The retail radius IS the animated value, so authoring it re-bases
			// the level onto the new authored ON value.
			tuning.mfOnValue = afX;
			mState.mfLevel = 1.0f;
		}
		PromotePassiveTuning();
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	float iLight::GetAnimatedValue() const
	{
		return cLightState::GetTuningAnimatedValue(Resolved(), GetActiveLightModel());
	}

	// Fades, flicker and scripts come through here. They move the SHARED level
	// rather than one tuning's numbers, so the other backend follows in
	// proportion and a light dimmed to half stays half across a switch.
	void iLight::SetAnimatedValue(float afX)
	{
		const cLightTuningState& tuning = mState.Tuning(GetActiveLightModel());

		float fLevel = 1.0f;
		if(DeriveLightLevel(tuning.mfOnValue, tuning.mfOffValue, afX, &fLevel))
		{
			if(mState.mfLevel == fLevel) return;
			mState.mfLevel = fLevel;
			OnTuningChanged();
			return;
		}

		// No authored range to scale against -- a light the map authored dark.
		// Author the value instead and put every tuning back at level 1.
		mState.mfLevel = 1.0f;
		if(GetActiveLightModel()==eLightModel_RayTraced)
			SetIntensity(afX);
		else
			SetRadius(afX);
	}

	

	

	//-----------------------------------------------------------------------

	void iLight::SetRendererMask(unsigned aMask)
	{
		const unsigned masked = SanitizeRendererMask(aMask);
		if (mRendererMask == masked) return;

		mRendererMask = masked;

		mbUpdateBoundingVolume = true;

		// This is so that the render container is updated.
		SetTransformUpdated();
	}

	//-----------------------------------------------------------------------

	float iLight::GetFlickerOffValue() const
	{
		return mState.Tuning(GetActiveLightModel()).mfOffValue;
	}

	cColor iLight::GetFlickerOnColor() const
	{
		if(mFlickerOnColorDrive.mbColorAbsolute) return mFlickerOnColorDrive.mAbsoluteColor;

		const cColor& authored = mState.Tuning(GetActiveLightModel()).mDiffuseColor;
		const cColor& scale = mFlickerOnColorDrive.mColorScale;
		return cColor(authored.r*scale.r, authored.g*scale.g, authored.b*scale.b, authored.a*scale.a);
	}

	void iLight::SetColorDrive(const cLightColorDrive& aDrive)
	{
		const bool bWasVisible = IsVisible();

		mState.mColorDrive = aDrive;

		// The reach may follow the colour, so the bounds can move with it.
		OnTuningChanged();

		if(mbIsVisible && IsVisible() != bWasVisible)
			OnChangeVisible();

		OnSetDiffuse();
	}

	//-----------------------------------------------------------------------

	float iLight::GetFlickerOnValue() const
	{
		const cLightTuningState& tuning = mState.Tuning(GetActiveLightModel());
		return ResolveLightLevelValue(tuning.mfOnValue, tuning.mfOffValue, mfFlickerOnLevel);
	}

	//-----------------------------------------------------------------------

	void iLight::SetLevel(float afLevel)
	{
		if(mState.mfLevel == afLevel) return;

		mState.mfLevel = afLevel;
		OnTuningChanged();
	}

	//-----------------------------------------------------------------------

	void iLight::SetSourceRadius(float afX)
	{
		cLightTuningState& tuning = AuthoringTuning();
		if (tuning.mfSourceRadius == afX) return;

		tuning.mfSourceRadius = afX;
		InvalidateResolved();
	}

	//-----------------------------------------------------------------------

	void iLight::UpdateLogic(float afTimeStep)
	{
		UpdateLight(afTimeStep);
		if(mfFadeTime>0 || mbFlickering)
		{
			mbUpdateBoundingVolume = true;
			
			//This is so that the render container is updated.
			//SetTransformUpdated();
		}
	}

	//-----------------------------------------------------------------------

	cBoundingVolume* iLight::GetBoundingVolume()
	{
		if(mbUpdateBoundingVolume)
		{
			UpdateBoundingVolume();
			mbUpdateBoundingVolume = false;
		}

		return &mBoundingVolume;
	}
	
	//-----------------------------------------------------------------------

	cMatrixf* iLight::GetModelMatrix(cFrustum* apFrustum)
	{
		return &GetWorldMatrix();
	}
	
	//-----------------------------------------------------------------------
	
	//-----------------------------------------------------------------------

	void iLight::SetFalloffMap(Image* apImage)
	{
		m_falloffMap = SharedResourceHandle<Image>(mpTextureManager, apImage); // adopt transferred ref
	}

	Image* iLight::GetFalloffImage() const
	{
		return m_falloffMap.Get();
	}

	void iLight::SetGoboTexture(Image* apImage)
	{
		m_goboImage = SharedResourceHandle<Image>(mpTextureManager, apImage); // adopt transferred ref
	}

	Image* iLight::GetGoboImage() const
	{
		return m_goboImage.Get();
	}

	//-----------------------------------------------------------------------

	void iLight::AddShadowCaster(iRenderable *apObject)
	{
		m_mapShadowCasterCache.insert(tShadowCasterCacheMap::value_type(apObject, apObject->GetTransformUpdateCount()));
	}
	
	bool iLight::ShadowCasterIsValid(iRenderable *apObject)
	{
		tShadowCasterCacheMapIt it = m_mapShadowCasterCache.find(apObject);
		if(it == m_mapShadowCasterCache.end()) return false;

		return it->second == apObject->GetTransformUpdateCount();
	}
	
	bool iLight::ShadowCastersAreUnchanged(const tRenderableVec &avObjects)
	{
		size_t lDynObjectCount=0;
		for(size_t i=0; i<avObjects.size(); ++i)
		{
			iRenderable *pObject = avObjects[i];
			if(pObject->IsStatic()==false)
			{
				lDynObjectCount++;
				if(ShadowCasterIsValid(avObjects[i])==false)
				{
					return false;
				}
			}
		}
		
		if(lDynObjectCount != m_mapShadowCasterCache.size()) return false;
		
		return true;
	}

	void iLight::SetShadowCasterCacheFromVec(const tRenderableVec &avObjects)
	{
		m_mapShadowCasterCache.clear();

		for(size_t i=0; i<avObjects.size(); ++i)
		{
			if(avObjects[i]->IsStatic()==false) AddShadowCaster(avObjects[i]);
		}
	}
	
	void iLight::ClearShadowCasterCache()
	{
		m_mapShadowCasterCache.clear();
	}

	//-----------------------------------------------------------------------

	void iLight::LoadXMLProperties(const tString asFile)
	{
		tWString sPath = mpFileSearcher->GetFilePath(asFile);
		if(sPath != _W(""))
		{
			tinyxml2::XMLDocument *pDoc = hplNew( tinyxml2::XMLDocument,() );
			if(LoadXmlFile(*pDoc, sPath))
			{
				tinyxml2::XMLElement *pRootElem = pDoc->RootElement();

				tinyxml2::XMLElement *pMainElem = pRootElem->FirstChildElement("MAIN");
				if(pMainElem!=NULL)
				{
					SetCastShadows(GetAttributeBool(pMainElem, "CastsShadows", GetCastShadows()));

					cColor specular = GetDiffuseColor();
					specular.a = GetAttributeFloat(pMainElem, "Specular", specular.a);
					SetDiffuseColor(specular);

					tString sFalloffImage = GetAttributeString(pMainElem, "FalloffImage");
					Image *pImage = mpTextureManager->Create1DImage(sFalloffImage,false).Release();
					if(pImage) SetFalloffMap(pImage);

					ExtraXMLProperties(pMainElem);
				}
				else
				{
					Error("Cannot find main element in %s\n",asFile.c_str());
				}
			}
			else
			{
				Error("Couldn't load file '%s'\n",asFile.c_str());
			}
			hplDelete(pDoc);
		}
		else
		{
			Error("Couldn't find file '%s'\n",asFile.c_str());
		}

	}

	//-----------------------------------------------------------------------

	void iLight::AttachBillboard(cBillboard *apBillboard, const cColor &aBaseColor)
	{
		cLightBillboardConnection bbConnection;

		bbConnection.mpBillboard = apBillboard;
		bbConnection.mBaseColor = aBaseColor;
		bbConnection.mlRendererMaskBeforeConnect = apBillboard->GetRendererMask();

		const cColor& tint = GetDiffuseColor();
		apBillboard->SetColor(aBaseColor * cColor(tint.r,tint.g,tint.b,1));
		apBillboard->SetVisible(IsVisible());
		// The mask gate is read per gather from one global bit, so this survives
		// a runtime backend switch with no further bookkeeping.
		apBillboard->SetRendererMask(MaskWithoutRayTraced(apBillboard->GetRendererMask()));

		mvBillboards.push_back(bbConnection);
	}

	//-----------------------------------------------------------------------

	void iLight::RemoveBillboard(cBillboard *apBillboard)
	{
		std::vector<cLightBillboardConnection>::iterator it = mvBillboards.begin();
		for(; it != mvBillboards.end(); ++it)
		{
			cLightBillboardConnection &bbConnection = *it;
			if(bbConnection.mpBillboard == apBillboard)
			{
				apBillboard->SetRendererMask(bbConnection.mlRendererMaskBeforeConnect);
				mvBillboards.erase(it);
				break;
			}
		}
	}

	//-----------------------------------------------------------------------

	// The editor's path for a billboard whose ConnectLight resolves: it never
	// goes through AttachBillboard, so the ray-traced bit comes off here too.
	void iLight::UpdateBillboard(cBillboard* apBillboard, const cColor& aBaseColor)
	{
		const cColor& tint = GetDiffuseColor();
		apBillboard->SetColor(aBaseColor * cColor(tint.r, tint.g, tint.b, 1));
		apBillboard->SetVisible(IsVisible());
		apBillboard->SetRendererMask(MaskWithoutRayTraced(apBillboard->GetRendererMask()));
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void iLight::OnFlickerOff()
	{
		//Particle system
		if(msFlickerOffPS!=""){
			cParticleSystem *pPS = mpWorld->CreateParticleSystem(GetName() + "_PS", msFlickerOffPS, cVector3f(1,1,1));
			if(pPS) pPS->SetMatrix(GetWorldMatrix());
		}
	}

	//-----------------------------------------------------------------------

	void iLight::OnFlickerOn()
	{
		//Particle system
		if(msFlickerOnPS!=""){
			cParticleSystem *pPS = mpWorld->CreateParticleSystem(GetName() + "_PS", msFlickerOnPS, cVector3f(1,1,1));
			if(pPS) pPS->SetMatrix(GetWorldMatrix());
		}

	}

	//-----------------------------------------------------------------------

	void iLight::OnSetDiffuse()
	{
		const cColor tint = GetDiffuseColor();
		for(size_t i =0; i<mvBillboards.size(); ++i)
		{
			mvBillboards[i].mpBillboard->SetColor( mvBillboards[i].mBaseColor * cColor(tint.r,tint.g,tint.b,1));
		}
	}

	
	//-----------------------------------------------------------------------

}
