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

#include "LuxPostEffects.h"

#include "LuxMapHandler.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/Hasher.h"

namespace
{
struct InfectionPushConstants
{
	float alpha;
	float time;
	float screenSize[2];
	float ampT;
	float waveAlpha;
	float zoomAlpha;
	float infectionFactor;
	float gradientThresholdOffset;
	float gradientFallofExponent;
	float infectionMapZoom;
	float vomitBlendFactor;
};
}

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// INFECTION
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffect_Infection::cLuxPostEffect_Infection(cGraphics *apGraphics, cResources *apResources) : iLuxPostEffect(apGraphics, apResources),
	mfVomitBlendFactor(0.0f)
{
	//////////////////////////////
	hpl::LoadSlangGraphics(&mpGraphics->device, m_program, apResources,
						"posteffect_fullscreen.vert", "posteffect_infection.frag");

	//////////////////////////////
	// Textures
	mvAmpMaps.resize(3);

	for(size_t i=0; i<mvAmpMaps.size(); ++i)
		mvAmpMaps[i] = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_ampmap"+cString::ToString((int)i), false);

	mpZoomMap = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_zoom.jpg", false);
	mpInfectionNormalBlendMap = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_normal_blend.tga", false);
	mpInfectionOverlayBlendMap = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_overlay_blend.tga", false);
	mpInfectionColorDodgeBlendMap = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_colordodge_blend.tga", false);
	mpInfectionGradientMap = mpResources->GetTextureManager()->Create2DImage("posteffect_infection_gradient.jpg", false);
	// The legacy vomit overlay resource remains intentionally unused here.

	//////////////////////////////
	// Cfg vars

	mfGradientThresholdOffset = gpBase->mpGameCfg->GetFloat("Player_Infection","GradientThresholdOffset",0);
	mfGradientFallofExponent = gpBase->mpGameCfg->GetFloat("Player_Infection","GradientFallofExponent",0);
	mfInfectionMapZoom = gpBase->mpGameCfg->GetFloat("Player_Infection","InfectionMapZoom",0);
	mfInfectionGrowSpeed = gpBase->mpGameCfg->GetFloat("Player_Infection","InfectionEffectGrowSpeed",1.0f);

	//////////////////////////////
	// Init vars
	mfT =0;
	mfAnimCount =0;
	mfWaveAlpha = 0.0f;
	mfZoomAlpha = 0.0f;
	mfWaveSpeed = 0.0f;
	mfInfectionFactor = 0.0f;
	mfInfectionGoal = 0.0f;
}

//-----------------------------------------------------------------------

cLuxPostEffect_Infection::~cLuxPostEffect_Infection()
{
	m_program.dispose(&mpGraphics->device);
}

//-----------------------------------------------------------------------

void cLuxPostEffect_Infection::Update(float afTimeStep)
{
	if ( mfInfectionFactor < mfInfectionGoal )
	{
		mfInfectionFactor +=afTimeStep * mfInfectionGrowSpeed;
		if ( mfInfectionFactor > mfInfectionGoal ) mfInfectionFactor = mfInfectionGoal;
	}
	else if ( mfInfectionFactor > mfInfectionGoal )
	{
		mfInfectionFactor -= afTimeStep * mfInfectionGrowSpeed;
		if ( mfInfectionFactor < mfInfectionGoal ) mfInfectionFactor = mfInfectionGoal;
	}

	mfT += afTimeStep * mfWaveSpeed;

	mfAnimCount += afTimeStep * 0.15f;

	float fMaxAnim = (float)mvAmpMaps.size();
	if(mfAnimCount >= fMaxAnim) mfAnimCount = mfAnimCount-fMaxAnim;
}

//-----------------------------------------------------------------------

void cLuxPostEffect_Infection::RenderEffect(const hpl::PostEffectRenderCtx &ctx)
{
	int lAmp0 = (int)mfAnimCount;
	int lAmp1 = (int)(mfAnimCount+1);
	if(lAmp1 >= (int) mvAmpMaps.size()) lAmp1 = 0;
	float fAmpT = cMath::GetFraction(mfAnimCount);

	using namespace hpl;
	RIRenderingAttachment color = {};
	color.view = ctx.outputView;
	color.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
	RIBeginRenderingDesc beginDesc = {};
	beginDesc.renderArea.width = ctx.width;
	beginDesc.renderArea.height = ctx.height;
	beginDesc.colorCount = 1;
	beginDesc.colors = &color;
	ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

	RIViewport viewport = {};
	viewport.y = static_cast<float>(ctx.height);
	viewport.width = static_cast<float>(ctx.width);
	viewport.height = -static_cast<float>(ctx.height);
	viewport.depthMax = 1.0f;
	ctx.cmd->setViewport(&mpGraphics->device, viewport);
	RIRect scissor = {};
	scissor.width = ctx.width;
	scissor.height = ctx.height;
	ctx.cmd->setScissor(&mpGraphics->device, scissor);

	const RIGraphicsPipelineDesc pipelineDesc =
		MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);
	const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, 0u);
	m_program.bindPipeline(&mpGraphics->device, ctx.cmd, pipelineHash,
						   "PostEffect_Infection", pipelineDesc);
	auto samplerDesc = mpGraphics->resolve_filter_descriptor(
		eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	auto resolve = [&](hpl::Image *image) -> RIDescriptor {
		if(image && image->GetTexture() && !image->GetTexture()->view.isEmpty())
			return image->GetTexture()->descriptor();
		return ctx.inputSrv;
	};
	RIProgram::DescriptorBinding bindings[9] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = ctx.inputSrv;
	bindings[1].handle = DescriptorBindingID::Create("sourceInput");
	const RIDescriptor textures[7] = {
		resolve(mvAmpMaps[lAmp0].Get()), resolve(mvAmpMaps[lAmp1].Get()), resolve(mpZoomMap.Get()),
		resolve(mpInfectionNormalBlendMap.Get()), resolve(mpInfectionOverlayBlendMap.Get()),
		resolve(mpInfectionColorDodgeBlendMap.Get()), resolve(mpInfectionGradientMap.Get())};
	const char *handles[7] = {"ampMap0", "ampMap1", "zoomMap", "normalBlendMap",
		"overlayBlendMap", "colorDodgeBlendMap", "gradientMap"};
	for(int i = 0; i < 7; ++i)
	{
		bindings[i + 2].descriptor = textures[i];
		bindings[i + 2].handle = DescriptorBindingID::Create(handles[i]);
	}
	m_program.bindDescriptors(&mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 9);

	InfectionPushConstants constants = {};
	constants.alpha = 1.0f;
	constants.time = mfT;
	constants.screenSize[0] = static_cast<float>(ctx.width);
	constants.screenSize[1] = static_cast<float>(ctx.height);
	constants.ampT = fAmpT;
	constants.waveAlpha = mfWaveAlpha;
	constants.zoomAlpha = mfZoomAlpha;
	constants.infectionFactor = mfInfectionFactor;
	constants.gradientThresholdOffset = mfGradientThresholdOffset;
	constants.gradientFallofExponent = mfGradientFallofExponent;
	constants.infectionMapZoom = mfInfectionMapZoom;
	constants.vomitBlendFactor = mfVomitBlendFactor;
	ctx.cmd->vk_d3d12_setPushConstants(&mpGraphics->device, m_program, 0,
								   sizeof(constants), &constants);

	ctx.cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
	ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// POST EFFECT HANDLER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffectHandler::cLuxPostEffectHandler() : iLuxUpdateable("LuxPostEffectHandler")
{
	cGraphics *pGraphics = gpBase->mpEngine->GetGraphics();
	cResources *pResources = gpBase->mpEngine->GetResources();

	///////////////////////
	// Create post effects
	mpInfection = hplNew(cLuxPostEffect_Infection, (pGraphics, pResources) );
	AddEffect(mpInfection, 25);
	mpInfection->SetActive(false);
}

//-----------------------------------------------------------------------

cLuxPostEffectHandler::~cLuxPostEffectHandler()
{
	STLDeleteAll(mvPostEffects);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::OnStart()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Update(float afTimeStep)
{
	for(size_t i=0; i<mvPostEffects.size(); ++i)
	{
		iLuxPostEffect *pPostEffect = mvPostEffects[i];

        if(pPostEffect->IsActive()) pPostEffect->Update(afTimeStep);
	}
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Reset()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::LoadMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	mpInfection->SetDisabled(pMainCfg->GetBool("Graphics", "PostEffectInfection", true)==false);

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::SaveMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	pMainCfg->SetBool("Graphics", "PostEffectInfection", mpInfection->IsDisabled()==false);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::AddEffect(iLuxPostEffect *apPostEffect, int alPrio)
{
	mvPostEffects.push_back(apPostEffect);
	apPostEffect->SetActive(false);
	gpBase->mpMapHandler->GetViewport()->GetPostEffectComposite()->AddPostEffect(apPostEffect, alPrio);
}

//-----------------------------------------------------------------------
