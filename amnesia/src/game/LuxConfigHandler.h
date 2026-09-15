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
	static int GetRenderScalePresetIndex(float afScale); // index of the preset that NormalizeRenderScale(afScale) equals; never negative
	static float NormalizeRenderScale(float afScale);
	float GetRenderScale() const;
	void SetRenderScale(float afScale);

	// Display gamma (1.0 = no-op). Stored here (was the SDL window class);
	// consumed by the tonemap post-effect. SetGamma live-applies to the active
	// map's tonemap via cLuxMapHandler.
	float GetGamma() { return mfGamma; }
	void SetGamma(float afX);

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
	float mfGamma;
	int mlShadowQuality;
	int mlShadowRes;

	bool mbSSAOActive;
	int mlSSAOSamples;
	int mlSSAOResolution; //0= medium(div2), 1=high (same as screen resolution)
	
	int mlParallaxQuality;
	bool mbParallaxEnabled;

	bool mbOcclusionTestLights;
	
	bool mbEdgeSmooth;

	// Requested user preference; the renderer owns effective fallback.
	TemporalUpscalerSettings mSuperSampling;
	// Requested manual scene render scale (fraction of display size); a prepared
	// FSR/XeSS provider owns the extent instead.
	float mfRenderScale;
		
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
