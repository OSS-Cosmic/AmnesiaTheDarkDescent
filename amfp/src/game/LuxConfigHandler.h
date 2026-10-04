/*
 * Copyright © 2011-2020 Frictional Games
 * 
 * This file is part of Amnesia: A Machine For Pigs.
 * 
 * Amnesia: A Machine For Pigs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 

 * Amnesia: A Machine For Pigs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: A Machine For Pigs.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LUX_CONFIG_HANDLER_H
#define LUX_CONFIG_HANDLER_H

//----------------------------------------------

#include "LuxBase.h"
#include "graphics/TemporalUpscalerTypes.h"

//----------------------------------------------

class cLuxConfigHandler
{
public:	
	cLuxConfigHandler();
	~cLuxConfigHandler();

	void LoadMainConfig();
	void SaveMainConfig();

	static tString SuperSamplingProviderToString(hpl::TemporalUpscalerProvider aProvider);
	static hpl::TemporalUpscalerProvider SuperSamplingProviderFromString(const tString& asValue);
	static tString SuperSamplingQualityToString(hpl::TemporalUpscalerQuality aQuality);
	static hpl::TemporalUpscalerQuality SuperSamplingQualityFromString(const tString& asValue);
	static tString RendererBackendToString(hpl::eRendererBackend aBackend);
	static hpl::eRendererBackend RendererBackendFromString(const tString& asValue);
	static int GetRenderScalePresetNum();
	static float GetRenderScalePreset(int alIdx);
	static int GetRenderScalePresetIndex(float afScale);
	static float NormalizeRenderScale(float afScale);
	float GetRenderScale() const;
	void SetRenderScale(float afScale);
	float GetGamma() const;
	void SetGamma(float afGamma);

	bool ShowRestartWarning(cGuiSet* apSet, void* apObject, tGuiCallbackFunc apCallback);

	void SetGameNeedsRestart() { mbGameNeedsRestart=true; mbRestartDialogShown=false; }

	// Variables
	bool mbLoadDebugMenu;
	bool mbFirstStart;
	tString msScreenShotExt;

	bool mbCreateAndLoadCompressedMaps;
	bool mbForceCacheLoadingAndSkipSaving;

	tString msLangFile;
	
	cVector2l mvScreenSize;
    int mlDisplay;
	bool mbFullscreen;
	bool mbVSync;
	bool mbAdaptiveVSync;
	hpl::eRendererBackend mRendererBackend;
	int mlTextureQuality;
	int mlTextureFilter;
	float mfTextureAnisotropy;
	int mlShadowQuality;
	int mlShadowRes;

	int mlMaxFramesPerSec;

	bool mbSSAOActive;
	int mlSSAOSamples;
	int mlSSAOResolution; //0= medium(div2), 1=high (same as screen resolution)
	
	int mlParallaxQuality;
	bool mbParallaxEnabled;

	bool mbOcclusionTestLights;
	
	bool mbEdgeSmooth;

	TemporalUpscalerSettings mSuperSampling;
	float mfRenderScale;
	float mfGamma;
		
	bool mbWorldReflection;
	bool mbRefraction;
	bool mbShadowsActive;

	bool mbForceShaderModel3And4Off;

	bool mbFastPhysicsLoad;
	bool mbFastStaticLoad;
	bool mbFastEntityLoad;

	int mlSoundDevID;
	int mlMaxSoundChannels;
	int mlSoundStreamBuffers;
	int mlSoundStreamBufferSize;
	bool mbHRTFActive;

	
private:
	bool mbGameNeedsRestart;
	bool mbRestartDialogShown;
};

//----------------------------------------------


#endif // LUX_DEBUG_HANDLER_H
