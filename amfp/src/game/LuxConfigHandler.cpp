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

#include "LuxConfigHandler.h"

#include "LuxMap.h"
#include "LuxMapHandler.h"
#include "LuxMainMenu.h"
#include "LuxInputHandler.h"
#include "LuxHintHandler.h"
#include "LuxHelpFuncs.h"
#include "graphics/Graphics.h"

// TODO: there is some cleanup to do here

static const float gvRenderScalePresets[] = {1.0f, 0.90f, 0.75f, 0.66f, 0.50f, 0.33f};

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxConfigHandler::cLuxConfigHandler()
{
	mbGameNeedsRestart = false;
	mbRestartDialogShown = false;
	mfRenderScale = 1.0f;
	mfGamma = 1.0f;
}

//-----------------------------------------------------------------------

cLuxConfigHandler::~cLuxConfigHandler()
{
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////


//-----------------------------------------------------------------------

void cLuxConfigHandler::LoadMainConfig()
{
	////////////////////////////////////////////////////////////////////////////////
	// Purpose of this is loading variables that need to be set on engine creation,
	// or set to restart.

	/////////////////////
	// Main
	mbLoadDebugMenu = gpBase->mpMainConfig->GetBool("Main","LoadDebugMenu", false);
	mbFirstStart = gpBase->CheckFirstStartFlag()==false;
	//mbFirstStart = gpBase->mpMainConfig->GetBool("Main","FirstStart", true);

	msLangFile = gpBase->mpMainConfig->GetString("Main", "StartLanguage", gpBase->msDefaultGameLanguage);

	msScreenShotExt = gpBase->mpMainConfig->GetString("Main","ScreenShotExt", "jpg");

	mbForceCacheLoadingAndSkipSaving = gpBase->mpMainConfig->GetBool("Main","ForceCacheLoadingAndSkipSaving", true);
	//mbCreateAndLoadCompressedMaps = gpBase->mpMainConfig->GetBool("Main","CreateAndLoadCompressedMaps", false);

	/////////////////////
	// Engine init variables
	mvScreenSize.x =	gpBase->mpMainConfig->GetInt("Screen","Width", 800);
	mvScreenSize.y =	gpBase->mpMainConfig->GetInt("Screen","Height", 600);
    mlDisplay =			gpBase->mpMainConfig->GetInt("Screen","Display", 0);
	mbFullscreen =		gpBase->mpMainConfig->GetBool("Screen","FullScreen", false);
	mbVSync =			gpBase->mpMainConfig->GetBool("Screen","Vsync", true);
	mbAdaptiveVSync =	gpBase->mpMainConfig->GetBool("Screen","AdaptiveVsync", true);

	mbFastPhysicsLoad=	gpBase->mpMainConfig->GetBool("MapLoad","FastPhysicsLoad", false);
	mbFastStaticLoad=	gpBase->mpMainConfig->GetBool("MapLoad","FastStaticLoad", false);
	mbFastEntityLoad =	gpBase->mpMainConfig->GetBool("MapLoad","FastEntityLoad", false);

	/////////////////////
	// Graphics variables

	// General
	mbOcclusionTestLights = gpBase->mpMainConfig->GetBool("Graphics", "OcclusionTestLights", true);
	mfGamma = gpBase->mpMainConfig->GetFloat("Graphics", "Gamma", 1.0f);

	// Shadows
	mbShadowsActive =	gpBase->mpMainConfig->GetBool("Graphics", "ShadowsActive", true);
	mlShadowQuality =	gpBase->mpMainConfig->GetInt("Graphics", "ShadowQuality", eShadowMapQuality_Medium);
	mlShadowRes =		gpBase->mpMainConfig->GetInt("Graphics", "ShadowResolution", eShadowMapResolution_High);
	
	// Misc
	mbWorldReflection = gpBase->mpMainConfig->GetBool("Graphics", "WorldReflection", true);
	mbRefraction =		gpBase->mpMainConfig->GetBool("Graphics", "Refraction", true);
	mbEdgeSmooth =		gpBase->mpMainConfig->GetBool("Graphics", "EdgeSmooth", false);

	mRendererBackend = RendererBackendFromString(gpBase->mpMainConfig->GetString("Graphics", "RendererBackend", "standard"));
	mSuperSampling.provider = SuperSamplingProviderFromString(gpBase->mpMainConfig->GetString("Graphics", "SuperSamplingProvider", "off"));
	mSuperSampling.quality = SuperSamplingQualityFromString(gpBase->mpMainConfig->GetString("Graphics", "SuperSamplingQuality", "quality"));
	mfRenderScale = NormalizeRenderScale(gpBase->mpMainConfig->GetFloat("Graphics", "RenderScale", 1.0f));

	// SSAO
	mbSSAOActive =		gpBase->mpMainConfig->GetBool("Graphics","SSAOActive", true);
	mlSSAOSamples =		gpBase->mpMainConfig->GetInt("Graphics","SSAOSamples", 8);
	mlSSAOResolution =	gpBase->mpMainConfig->GetInt("Graphics","SSAOResolution", 0);
	
	// Parallax
	mbParallaxEnabled = gpBase->mpMainConfig->GetBool("Graphics", "ParallaxEnabled", true);
	mlParallaxQuality = gpBase->mpMainConfig->GetInt("Graphics", "ParallaxQuality", 0);
	
	// Texture
	mlTextureQuality =	gpBase->mpMainConfig->GetInt("Graphics", "TextureQuality", 0);
	mlTextureFilter =	gpBase->mpMainConfig->GetInt("Graphics", "TextureFilter", eTextureFilter_Bilinear);
	mfTextureAnisotropy = gpBase->mpMainConfig->GetFloat("Graphics", "TextureAnisotropy", 1.0f);

	mbForceShaderModel3And4Off = gpBase->mpMainConfig->GetBool("Graphics", "ForceShaderModel3And4Off", false);

	/////////////////////
	// Sound variables
	mlSoundDevID = gpBase->mpMainConfig->GetInt("Sound", "Device", -1);
	mlMaxSoundChannels = gpBase->mpMainConfig->GetInt("Sound", "MaxChannels", 64);
	mlSoundStreamBuffers = gpBase->mpMainConfig->GetInt("Sound", "StreamBuffers", 8);
	mlSoundStreamBufferSize = gpBase->mpMainConfig->GetInt("Sound", "StreamBufferSize", 262144);
	mbHRTFActive = gpBase->mpMainConfig->GetBool("Sound", "HRTFActive", false);

	///////////////
	// Engine
	mlMaxFramesPerSec = gpBase->mpMainConfig->GetInt("Engine", "MaxFramesPerSec", 60);
}

//-----------------------------------------------------------------------

void cLuxConfigHandler::SaveMainConfig()
{
	/////////////////////
	// Main
	gpBase->mpMainConfig->SetBool("Main","LoadDebugMenu", mbLoadDebugMenu);
	if(mbFirstStart) gpBase->RaiseFirstStartFlag();
	//gpBase->mpMainConfig->SetBool("Main","FirstStart", false);


	gpBase->mpMainConfig->SetString("Main", "StartLanguage", msLangFile);

	gpBase->mpMainConfig->SetString("Main","ScreenShotExt", msScreenShotExt);

	gpBase->mpMainConfig->SetBool("Main","ForceCacheLoadingAndSkipSaving", mbForceCacheLoadingAndSkipSaving);

	/////////////////////
	// Engine init variables
	gpBase->mpMainConfig->SetInt("Screen","Width", mvScreenSize.x);
	gpBase->mpMainConfig->SetInt("Screen","Height", mvScreenSize.y);
    gpBase->mpMainConfig->SetInt("Screen","Display", mlDisplay);
	gpBase->mpMainConfig->SetBool("Screen","FullScreen", mbFullscreen);
	gpBase->mpMainConfig->SetBool("Screen","Vsync", mbVSync);
	gpBase->mpMainConfig->SetBool("Screen","AdaptiveVsync", mbAdaptiveVSync);

	gpBase->mpMainConfig->SetBool("MapLoad","FastPhysicsLoad", mbFastPhysicsLoad);
	gpBase->mpMainConfig->SetBool("MapLoad","FastStaticLoad", mbFastStaticLoad);
	gpBase->mpMainConfig->SetBool("MapLoad","FastEntityLoad", mbFastEntityLoad);

	/////////////////////
	// Graphics variables
	cMaterialManager* pMatMgr = gpBase->mpEngine->GetResources()->GetMaterialManager();
	cRenderSettings* pRenderSettings = gpBase->mpMapHandler->GetViewport()->GetRenderSettings();

	gpBase->mpMainConfig->SetBool("Graphics", "OcclusionTestLights", mbOcclusionTestLights);
	gpBase->mpMainConfig->SetFloat("Graphics", "Gamma", mfGamma);

	gpBase->mpMainConfig->SetInt("Graphics","TextureQuality", mlTextureQuality);
	gpBase->mpMainConfig->SetInt("Graphics","TextureFilter", mlTextureFilter);
	gpBase->mpMainConfig->SetFloat("Graphics","TextureAnisotropy", mfTextureAnisotropy);

	gpBase->mpMainConfig->SetBool("Graphics","SSAOActive",mbSSAOActive);
	gpBase->mpMainConfig->SetInt("Graphics","SSAOResolution",mlSSAOResolution);
	gpBase->mpMainConfig->SetInt("Graphics","SSAOSamples",mlSSAOSamples);
	
	gpBase->mpMainConfig->SetBool("Graphics", "WorldReflection", mbWorldReflection);
	gpBase->mpMainConfig->SetBool("Graphics", "Refraction", mbRefraction);
	
	gpBase->mpMainConfig->SetBool("Graphics", "ShadowsActive", mbShadowsActive);
	gpBase->mpMainConfig->SetInt("Graphics","ShadowQuality", mlShadowQuality);
	gpBase->mpMainConfig->SetInt("Graphics","ShadowResolution", mlShadowRes);

	gpBase->mpMainConfig->SetInt("Graphics","ParallaxQuality", mlParallaxQuality);
	gpBase->mpMainConfig->SetBool("Graphics", "ParallaxEnabled", mbParallaxEnabled);

	gpBase->mpMainConfig->SetBool("Graphics", "EdgeSmooth", mbEdgeSmooth);
	gpBase->mpMainConfig->SetString("Graphics", "RendererBackend", RendererBackendToString(mRendererBackend));
	gpBase->mpMainConfig->SetString("Graphics", "SuperSamplingProvider", SuperSamplingProviderToString(mSuperSampling.provider));
	gpBase->mpMainConfig->SetString("Graphics", "SuperSamplingQuality", SuperSamplingQualityToString(mSuperSampling.quality));
	gpBase->mpMainConfig->SetFloat("Graphics", "RenderScale", mfRenderScale);

	gpBase->mpMainConfig->SetBool("Graphics", "ForceShaderModel3And4Off", mbForceShaderModel3And4Off);

	/////////////////////
	// Sound variables
	cSound *pSound = gpBase->mpEngine->GetSound();
	gpBase->mpMainConfig->SetInt("Sound", "Device", mlSoundDevID);
	gpBase->mpMainConfig->SetFloat("Sound","Volume", pSound->GetLowLevel()->GetVolume());
	gpBase->mpMainConfig->SetInt("Sound", "MaxChannels", mlMaxSoundChannels);
	gpBase->mpMainConfig->SetInt("Sound", "StreamBuffers", mlSoundStreamBuffers);
	gpBase->mpMainConfig->SetInt("Sound", "StreamBufferSize", mlSoundStreamBufferSize);
	gpBase->mpMainConfig->SetBool("Sound", "HRTFActive", mbHRTFActive);

	/////////////////////
	// Engine properties
	gpBase->mpMainConfig->SetBool("Engine","LimitFPS", gpBase->mpEngine->GetLimitFPS());
	gpBase->mpMainConfig->SetBool("Engine","SleepWhenOutOfFocus",gpBase->mpEngine->GetWaitIfAppOutOfFocus());
	gpBase->mpMainConfig->SetInt("Engine", "MaxFramesPerSec", mlMaxFramesPerSec);
}

//-----------------------------------------------------------------------

tString cLuxConfigHandler::SuperSamplingProviderToString(hpl::TemporalUpscalerProvider aProvider)
{
	switch(aProvider)
	{
	case hpl::TemporalUpscalerProvider::Fsr: return "fsr";
	case hpl::TemporalUpscalerProvider::XeSS: return "xess";
	case hpl::TemporalUpscalerProvider::Off:
	default: return "off";
	}
}

hpl::TemporalUpscalerProvider cLuxConfigHandler::SuperSamplingProviderFromString(const tString& asValue)
{
	tString sValue = cString::ToLowerCase(asValue);
	if(sValue=="fsr") return hpl::TemporalUpscalerProvider::Fsr;
	if(sValue=="xess") return hpl::TemporalUpscalerProvider::XeSS;
	return hpl::TemporalUpscalerProvider::Off;
}

tString cLuxConfigHandler::SuperSamplingQualityToString(hpl::TemporalUpscalerQuality aQuality)
{
	switch(aQuality)
	{
	case hpl::TemporalUpscalerQuality::NativeAA: return "native_aa";
	case hpl::TemporalUpscalerQuality::Balanced: return "balanced";
	case hpl::TemporalUpscalerQuality::Performance: return "performance";
	case hpl::TemporalUpscalerQuality::UltraPerformance: return "ultra_performance";
	case hpl::TemporalUpscalerQuality::Quality:
	default: return "quality";
	}
}

hpl::TemporalUpscalerQuality cLuxConfigHandler::SuperSamplingQualityFromString(const tString& asValue)
{
	tString sValue = cString::ToLowerCase(asValue);
	if(sValue=="native_aa") return hpl::TemporalUpscalerQuality::NativeAA;
	if(sValue=="balanced") return hpl::TemporalUpscalerQuality::Balanced;
	if(sValue=="performance") return hpl::TemporalUpscalerQuality::Performance;
	if(sValue=="ultra_performance") return hpl::TemporalUpscalerQuality::UltraPerformance;
	if(sValue=="quality") return hpl::TemporalUpscalerQuality::Quality;
	return hpl::TemporalUpscalerQuality::Quality;
}

tString cLuxConfigHandler::RendererBackendToString(hpl::eRendererBackend aBackend)
{
	switch(aBackend)
	{
	case hpl::eRendererBackend_Standard: return "standard";
	case hpl::eRendererBackend_RayTraced:
	default: return "raytraced";
	}
}

hpl::eRendererBackend cLuxConfigHandler::RendererBackendFromString(const tString& asValue)
{
	tString sValue = cString::ToLowerCase(asValue);
	if(sValue=="raytraced") return hpl::eRendererBackend_RayTraced;
	return hpl::eRendererBackend_Standard;
}

//-----------------------------------------------------------------------

int cLuxConfigHandler::GetRenderScalePresetNum()
{
	return (int)(sizeof(gvRenderScalePresets) / sizeof(gvRenderScalePresets[0]));
}

float cLuxConfigHandler::GetRenderScalePreset(int alIdx)
{
	if(alIdx < 0 || alIdx >= GetRenderScalePresetNum()) return 1.0f;
	return gvRenderScalePresets[alIdx];
}

int cLuxConfigHandler::GetRenderScalePresetIndex(float afScale)
{
	float fNormalizedScale = NormalizeRenderScale(afScale);
	for(int i=0; i<GetRenderScalePresetNum(); ++i)
	{
		if(gvRenderScalePresets[i] == fNormalizedScale) return i;
	}

	return 0;
}

float cLuxConfigHandler::NormalizeRenderScale(float afScale)
{
	if(afScale != afScale || afScale > 1.0f || afScale < 0.33f) return 1.0f;

	float fBestScale = gvRenderScalePresets[0];
	float fBestDifference = cMath::Abs(afScale - fBestScale);
	for(int i=1; i<GetRenderScalePresetNum(); ++i)
	{
		float fDifference = cMath::Abs(afScale - gvRenderScalePresets[i]);
		if(fDifference < fBestDifference ||
			(fDifference == fBestDifference && gvRenderScalePresets[i] > fBestScale))
		{
			fBestScale = gvRenderScalePresets[i];
			fBestDifference = fDifference;
		}
	}

	return fBestScale;
}

float cLuxConfigHandler::GetRenderScale() const
{
	return mfRenderScale;
}

void cLuxConfigHandler::SetRenderScale(float afScale)
{
	mfRenderScale = NormalizeRenderScale(afScale);
	cGraphics* pGraphics = hpl::Interface<cGraphics>::Get();
	if(pGraphics) pGraphics->devRenderScale = mfRenderScale;
}

float cLuxConfigHandler::GetGamma() const
{
	return mfGamma;
}

void cLuxConfigHandler::SetGamma(float afGamma)
{
	mfGamma = afGamma;
	if(gpBase && gpBase->mpMapHandler)
		gpBase->mpMapHandler->RefreshToneMapGamma();
	if(gpBase && gpBase->mpMainMenu)
		gpBase->mpMainMenu->RefreshToneMapGamma();
}

//-----------------------------------------------------------------------

bool cLuxConfigHandler::ShowRestartWarning(cGuiSet* apSet, void* apObject, tGuiCallbackFunc apCallback)
{
	///////////////////////////////////////////////////////////////////////////////////
	// This will show a popup if really needed, once per "restarting" setting changed.
	// Also, will return true if the popup was actually shown.
	if(mbGameNeedsRestart && mbRestartDialogShown==false)
	{
		mbRestartDialogShown = true;
		cGuiPopUpMessageBox* pPopUp = apSet->CreatePopUpMessageBox(kTranslate("OptionsMenu", "ReqRestartLabel"),
									 kTranslate("OptionsMenu", "ReqRestartMessage"), 
									 kTranslate("MainMenu","OK"), _W(""),
									 apObject, apCallback);
		pPopUp->GetGuiSet()->SetDrawFocus(gpBase->mpInputHandler->IsGamepadPresent());
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
