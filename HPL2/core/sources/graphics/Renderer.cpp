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

#include "graphics/Renderer.h"
#include "system/LowLevelSystem.h"
#include "graphics/Graphics.h"
#include "resources/Resources.h"

namespace hpl {

	int iRenderer::mlRenderFrameCount = 0;
	bool iRenderer::mbRefractionEnabled = true;
	eShadowMapQuality iRenderer::mShadowMapQuality = eShadowMapQuality_Medium;
	
	//-----------------------------------------------------------------------
	
	//////////////////////////////////////////////////////////////////////////
	// RENDER SETTINGS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRenderSettings::cRenderSettings(bool abIsReflection)
	{
		////////////////////////
		// Setup general variables
		mbIsReflection = abIsReflection;
		mbLog = false;
		mClearColor = cColor(0,0);

		////////////////////////
		// Setup render variables
		// Change this later I assume:
		mlMinimumObjectsBeforeOcclusionTesting = 0; //8;//8 should be good default, giving a good amount of colliders, or? Clarifiction: Minium num of object rendered until node visibility tests start!
		mlSampleVisiblilityLimit = 3;
		mbUseCallbacks = true;
		mbUseEdgeSmooth = false;
		mbUseOcclusionCulling = true;

		mMaxShadowMapResolution = eShadowMapResolution_High;
		if(mbIsReflection)
			mMaxShadowMapResolution = eShadowMapResolution_Medium;

		mbClipReflectionScreenRect = true;
		mbUseScissorRect = false;
		mbRenderWorldReflection = true;

		////////////////////////
		// Setup shadow variables
		mbRenderShadows = true;
		mfShadowMapBias = 4;			// The constant bias
		mfShadowMapSlopeScaleBias = 2;	// The bias based on sloping of depth.

		////////////////////////////
		// Light settings
		mbSSAOActive = mbIsReflection ? false : true;
		
		////////////////////////
		// Setup output variables
		mlNumberOfLightsRendered =0;
		mlNumberOfOcclusionQueries =0;

		////////////////////////
		// Create reflection settings
		mpReflectionSettings = NULL;
		if(mbIsReflection == false)
		{
			mpReflectionSettings = hplNew(cRenderSettings, (true) );
		}
	}

	cRenderSettings::~cRenderSettings()
	{
		if(mpReflectionSettings) hplDelete(mpReflectionSettings);
	}

	void cRenderSettings::ResetVariables()
	{
        if(mpReflectionSettings) mpReflectionSettings->ResetVariables();
	}

	//////////////////////////////////////////////////
	// The render settings will use the default setup, except for the variables below
	// This means SSAO, edgesmooth, etc are always off for reflections.
	#define RenderSettingsCopy(aVar) mpReflectionSettings->aVar = aVar
	void cRenderSettings::SetupReflectionSettings()
	{
		if(mpReflectionSettings==NULL) return;
		RenderSettingsCopy(mbLog);
		RenderSettingsCopy(mClearColor);

		////////////////////////////
		// Render settings
		RenderSettingsCopy(mlMinimumObjectsBeforeOcclusionTesting);
		RenderSettingsCopy(mlSampleVisiblilityLimit);
		mpReflectionSettings->mbUseScissorRect = false;
		// Reflection/helper viewports must not inherit the main camera's temporal
		// history or incidentally enable an SDK.
		mpReflectionSettings->mTemporalUpscaler = {};
		
		////////////////////////////
		// Shadow settings
		RenderSettingsCopy(mbRenderShadows);
		RenderSettingsCopy(mfShadowMapBias);
		RenderSettingsCopy(mfShadowMapSlopeScaleBias);

		////////////////////////////
		// Output
		RenderSettingsCopy(mlNumberOfLightsRendered);
		RenderSettingsCopy(mlNumberOfOcclusionQueries);
	}

	void cRenderSettings::AddOcclusionPlane(const cPlanef &aPlane)
	{
		mvOcclusionPlanes.push_back(aPlane);
	}

	void cRenderSettings::ResetOcclusionPlanes()
	{
		mvOcclusionPlanes.clear();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iRenderer::iRenderer(const tString& asName, cGraphics *apGraphics,cResources* apResources)
	{
		mpGraphics = apGraphics;
		mpResources = apResources;

		//////////////
		// Set variables from arguments
		msName = asName;

		//////////////
		// Init variables
		mpCurrentWorld = NULL;
		mpCurrentSettings = NULL;
		mpCurrentRenderList = NULL;
		mfTempAlpha = 0;
		mfTimeCount = 0;
	}

	iRenderer::~iRenderer() {}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void iRenderer::Update(float afTimeStep)
	{
		mfTimeCount += afTimeStep;
	}
}
