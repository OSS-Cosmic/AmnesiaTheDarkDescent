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

#include "LuxPostEffects.h"

#include "LuxMapHandler.h"

#include "graphics/Texture.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgramHelpers.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// INSANITY
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffect_Insanity::cLuxPostEffect_Insanity(cGraphics *apGraphics, cResources *apResources) : iLuxPostEffect(apGraphics, apResources)
{
	//////////////////////////////
	// Create program — Slang port of dds_insanity_posteffect.frag.fsl, sharing the
	// fullscreen-triangle vertex shader with the other post-effects.
	hpl::LoadSlangGraphics(&Interface<cGraphics>::Get()->device, m_program, apResources,
	                       "posteffect_fullscreen.vert",
	                       "posteffect_insanity.frag");

	//////////////////////////////
	// Textures
	mvAmpMaps.resize(3);
	
	for(size_t i=0; i<mvAmpMaps.size(); ++i)
		mvAmpMaps[i] = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_ampmap"+cString::ToString((int)i), false).Release();

	mpZoomMap = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_zoom.jpg", false).Release();

	//////////////////////////////
	// Init vars
	mfT =0;
	mfAnimCount =0;
	mfWaveAlpha = 0.0f;
	mfZoomAlpha = 0.0f;
	mfWaveSpeed =0.0f;
}

//-----------------------------------------------------------------------

cLuxPostEffect_Insanity::~cLuxPostEffect_Insanity()
{
	m_program.dispose(&mpGraphics->device);
}

//-----------------------------------------------------------------------

void cLuxPostEffect_Insanity::Update(float afTimeStep)
{
	mfT += afTimeStep * mfWaveSpeed;
	
	mfAnimCount += afTimeStep * 0.15f;

	float fMaxAnim = (float)mvAmpMaps.size();
	if(mfAnimCount >= fMaxAnim) mfAnimCount = mfAnimCount-fMaxAnim;
}

//-----------------------------------------------------------------------


namespace {
struct InsanityPushConstants
{
	float time;
	float amplitude;
	float waveAlpha;
	float zoomAlpha;
};
} // namespace

void cLuxPostEffect_Insanity::RenderEffect(const hpl::PostEffectRenderCtx &ctx)
{
	using namespace hpl;
	VkCommandBuffer cmd = ctx.cmd->vk.cmd;

	// Animated amp-map pair: ampMap0->1->2->0 as mfAnimCount sweeps [0,3), blended
	// by its fractional part (matches the legacy afAmpT animation).
	const int count = (int)mvAmpMaps.size();
	const int i0 = count > 0 ? ((int)mfAnimCount) % count : 0;
	const int i1 = count > 0 ? (i0 + 1) % count : 0;
	float amplitude = mfAnimCount - (float)((int)mfAnimCount); // frac; mfAnimCount >= 0

	// Resolve texture bindings. If any insanity asset is missing, substitute the
	// scene texture and zero the distortion so this pass stays a valid passthrough:
	// it MUST write its pogo half every frame, or the composite's post-effect toggle
	// desyncs and presents a stale, pre-tonemap buffer (the brightness bug).
	bool valid = (count >= 2);
	auto resolve = [&](Image *img) -> RIDescriptor {
		if (img && img->GetTexture() &&
		    !img->GetTexture()->view.isEmpty())
			return img->GetTexture()->descriptor();
		valid = false;
		return ctx.inputSrv;
	};
	RIDescriptor amp0Desc = (count > 0) ? resolve(mvAmpMaps[i0]) : ctx.inputSrv;
	RIDescriptor amp1Desc = (count > 0) ? resolve(mvAmpMaps[i1]) : ctx.inputSrv;
	RIDescriptor zoomDesc = resolve(mpZoomMap);
	if (count <= 0) valid = false;

	RITextureView outView = ctx.outputView;
	RIRenderingAttachment color = {};
	color.view    = outView;
	color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

	RIBeginRenderingDesc beginDesc = {};
	beginDesc.renderArea.width  = ctx.width;
	beginDesc.renderArea.height = ctx.height;
	beginDesc.colorCount = 1;
	beginDesc.colors     = &color;
	ctx.cmd->vk_d3d12_beginRendering(&Interface<cGraphics>::Get()->device, beginDesc);

	// Negative height: the engine convention shared with posteffect_fullscreen.vert.
	VkViewport viewport = {0.0f, (float)ctx.height, (float)ctx.width, -(float)ctx.height, 0.0f, 1.0f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	const RIGraphicsPipelineDesc pipelineDesc =
	    MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);

	const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
	m_program.bindPipeline(&Interface<cGraphics>::Get()->device, ctx.cmd, pipelineHash, "PostEffect_Insanity",
	                       pipelineDesc);

	auto samplerDesc = Interface<cGraphics>::Get()->resolve_filter_descriptor(
	    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
	    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	RIProgram::DescriptorBinding bindings[5] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = ctx.inputSrv;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	bindings[2].descriptor = amp0Desc;
	bindings[2].handle     = DescriptorBindingID::Create("ampMap0");
	bindings[3].descriptor = amp1Desc;
	bindings[3].handle     = DescriptorBindingID::Create("ampMap1");
	bindings[4].descriptor = zoomDesc;
	bindings[4].handle     = DescriptorBindingID::Create("zoomMap");
	m_program.bindDescriptors(&Interface<cGraphics>::Get()->device, ctx.cmd, ctx.frameIndex, bindings, 5);

	InsanityPushConstants pc{};
	pc.time      = mfT;
	pc.amplitude = amplitude;
	pc.waveAlpha = valid ? mfWaveAlpha : 0.0f;
	pc.zoomAlpha = valid ? mfZoomAlpha : 0.0f;
	vkCmdPushConstants(cmd, m_program.getPipelineLayout(),
	                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vkCmdDraw(cmd, 3, 1, 0, 0);
	ctx.cmd->vk_d3d12_endRendering(&Interface<cGraphics>::Get()->device);
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// MENU BACKDROP (blur / desaturate — live, replaces the snapshot path)
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffect_MenuBackdrop::cLuxPostEffect_MenuBackdrop(cGraphics *apGraphics, cResources *apResources)
	: iLuxPostEffect(apGraphics, apResources)
{
	using namespace hpl;
	// Both share the fullscreen-triangle vertex shader with the other
	// post-effects (no vertex buffer). Same frags the old cLuxScreenEffect used.
	LoadSlangGraphics(&mpGraphics->device, m_blurProgram, apResources,
	                  "posteffect_fullscreen.vert",
	                  "posteffect_bloom_blur.frag");
	LoadSlangGraphics(&mpGraphics->device, m_desatProgram, apResources,
	                  "posteffect_fullscreen.vert",
	                  "inventory_post.frag");
}

//-----------------------------------------------------------------------

cLuxPostEffect_MenuBackdrop::~cLuxPostEffect_MenuBackdrop()
{
	m_blurProgram.dispose(&mpGraphics->device);
	m_desatProgram.dispose(&mpGraphics->device);
	hpl::DestroyPostEffectColorTarget(m_scratchA);
	hpl::DestroyPostEffectColorTarget(m_scratchB);
}

//-----------------------------------------------------------------------

void cLuxPostEffect_MenuBackdrop::EnsureScratch(const hpl::PostEffectRenderCtx &ctx)
{
	using namespace hpl;
	auto ensure = [&](PostEffectColorTarget &t, const char *asName){
		if(t.valid && t.width == ctx.width && t.height == ctx.height) return;
		DestroyPostEffectColorTarget(t);
		CreatePostEffectColorTarget(t, ctx.width, ctx.height,
		                            cGraphics::PogoColorFormat, RI_USAGE_NONE, asName);
		if(!t.valid) return;
		// Rest state: the scratch halves live in SHADER_RESOURCE between passes,
		// so every pass can name an honest `before`. Same one-shot init as
		// cPostEffect_Bloom does for its mip chain.
		RITextureBarrier init(&t.texture,
		        RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_SHADER_RESOURCE,
		        RI_STAGE_NONE, RI_STAGE_FRAGMENT);
		ctx.cmd->vk_d3d12_textureBarrier(init);
	};
	ensure(m_scratchA, "MenuBackdrop.blurA");
	ensure(m_scratchB, "MenuBackdrop.blurB");
}

//-----------------------------------------------------------------------

void cLuxPostEffect_MenuBackdrop::RenderEffect(const hpl::PostEffectRenderCtx &ctx)
{
	if(mMode == Mode::Blur) RenderBlur(ctx);
	else                    RenderDesaturate(ctx);
}

//-----------------------------------------------------------------------

namespace {
struct MenuDesatPushConstants { float strength; float _pad[3]; };
struct MenuBlurPushConstants  { float blurDir[2]; float _pad[2]; }; // matches BloomBlurPC
} // namespace

//-----------------------------------------------------------------------

void cLuxPostEffect_MenuBackdrop::RenderDesaturate(const hpl::PostEffectRenderCtx &ctx)
{
	using namespace hpl;
	VkCommandBuffer cmd = ctx.cmd->vk.cmd;

	// Single fullscreen pass: sample the display-space pogo input, desaturate/
	// darken lerped by strength, write the pogo output. The composite owns the
	// pogo toggle / barriers around this call (like cPostEffect_ToneMap).
	RITextureView outView = ctx.outputView;
	RIRenderingAttachment color = {};
	color.view    = outView;
	color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

	RIBeginRenderingDesc beginDesc = {};
	beginDesc.renderArea.width  = ctx.width;
	beginDesc.renderArea.height = ctx.height;
	beginDesc.colorCount = 1;
	beginDesc.colors     = &color;
	ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

	// Negative height: the engine convention shared with posteffect_fullscreen.vert.
	VkViewport viewport = {0.0f, (float)ctx.height, (float)ctx.width, -(float)ctx.height, 0.0f, 1.0f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	const RIGraphicsPipelineDesc pipelineDesc =
	    MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);
	const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 0u);
	m_desatProgram.bindPipeline(&mpGraphics->device, ctx.cmd, kHash,
	                            "MenuBackdrop.desat", pipelineDesc);

	auto samplerDesc = mpGraphics->resolve_filter_descriptor(
	    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
	    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	RIProgram::DescriptorBinding bindings[2] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = ctx.inputSrv;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	m_desatProgram.bindDescriptors(&mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 2);

	MenuDesatPushConstants pc{};
	pc.strength = mfStrength;
	vkCmdPushConstants(cmd, m_desatProgram.getPipelineLayout(),
	                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vkCmdDraw(cmd, 3, 1, 0, 0);
	ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}

//-----------------------------------------------------------------------

void cLuxPostEffect_MenuBackdrop::RenderBlur(const hpl::PostEffectRenderCtx &ctx)
{
	using namespace hpl;
	EnsureScratch(ctx);
	VkCommandBuffer cmd = ctx.cmd->vk.cmd;

	// Plain value: the `pass` lambda below captures it by reference and it
	// outlives every call, so there is no lifetime coupling to respect.
	const RIGraphicsPipelineDesc pipelineDesc =
	    MakePostEffectPipelineDesc(cGraphics::PogoColorFormat, false);
	const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 0u);

	auto samplerDesc = mpGraphics->resolve_filter_descriptor(
	    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
	    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	// Negative height: the engine convention shared with posteffect_fullscreen.vert.
	VkViewport viewport = {0.0f, (float)ctx.height, (float)ctx.width, -(float)ctx.height, 0.0f, 1.0f};
	VkRect2D   scissor  = {{0, 0}, {ctx.width, ctx.height}};

	// One separable-blur pass. Writes either an owned scratch target (barriered
	// here) or the composite's pogo output (barriered by the composite, like
	// cPostEffect_ToneMap — pass dstScratch==nullptr for that).
	auto pass = [&](PostEffectColorTarget *dstScratch,
	                const RIDescriptor &srcDesc, float dirX, float dirY)
	{
		if(dstScratch)
		{
			// dst rests in SHADER_RESOURCE between uses; this is the WAR edge
			// against the previous iteration sampling it. The `before` state must
			// be honest: D3D12 tracks no state of its own and writes it straight
			// into the barrier (old contents are discarded by the DONT_CARE loadOp,
			// not by the barrier).
			RITextureBarrier toTarget(&dstScratch->texture,
			        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_RENDER_TARGET,
			        RI_STAGE_FRAGMENT, RI_STAGE_NONE);
			ctx.cmd->vk_d3d12_textureBarrier(toTarget);
		}

		// D3D12 binds a render target through an RTV and rejects the sampled view.
		RITextureView dstView = {};
		if(dstScratch) dstView = dstScratch->attachmentView;
		else           dstView = ctx.outputView;

		RIRenderingAttachment color = {};
		color.view    = dstView;
		color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width  = ctx.width;
		beginDesc.renderArea.height = ctx.height;
		beginDesc.colorCount = 1;
		beginDesc.colors     = &color;
		ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		m_blurProgram.bindPipeline(&mpGraphics->device, ctx.cmd, kHash,
		                           "MenuBackdrop.blur", pipelineDesc);

		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = srcDesc;
		bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
		m_blurProgram.bindDescriptors(&mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 2);

		MenuBlurPushConstants pc = {{dirX, dirY}, {0.0f, 0.0f}};
		vkCmdPushConstants(cmd, m_blurProgram.getPipelineLayout(),
		                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

		vkCmdDraw(cmd, 3, 1, 0, 0);
		ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);

		if(dstScratch)
		{
			RITextureBarrier toSampled(&dstScratch->texture,
			        RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
			        RI_STAGE_NONE, RI_STAGE_FRAGMENT);
			ctx.cmd->vk_d3d12_textureBarrier(toSampled);
		}
	};

	// Radius scales with strength so strength 0 => 0-offset taps => identity
	// (sharp), giving a focus-pull crossfade as the menu fades in. The last V
	// pass targets the pogo output; every earlier pass ping-pongs A<->B.
	const float kBlurSize   = 2.0f;
	const int   kIterations = 6;
	const float r = kBlurSize * mfStrength;

	RIDescriptor src = ctx.inputSrv; // pogo read half (composite left it SHADER_RESOURCE)
	for(int i = 0; i < kIterations; ++i)
	{
		const bool lastV = (i == kIterations - 1);
		pass(&m_scratchA, src, r, 0.0f);                 // horizontal -> A
		if(lastV) pass(nullptr, m_scratchA.descriptor(), 0.0f, r);      // vertical -> pogo output
		else      pass(&m_scratchB, m_scratchA.descriptor(), 0.0f, r);  // vertical -> B
		src = m_scratchB.descriptor();
	}
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
	mpInsanity = hplNew(cLuxPostEffect_Insanity, (pGraphics, pResources) );
	AddEffect(mpInsanity, 25);
	mpInsanity->SetActive(true);
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

	mpInsanity->SetDisabled(pMainCfg->GetBool("Graphics", "PostEffectInsanity", true)==false);

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::SaveMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	pMainCfg->SetBool("Graphics", "PostEffectInsanity", mpInsanity->IsDisabled()==false);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::AddEffect(iLuxPostEffect *apPostEffect, int alPrio)
{
	mvPostEffects.push_back(apPostEffect);
	apPostEffect->SetActive(false);
	gpBase->mpMapHandler->GetViewport()->GetPostEffectComposite()->AddPostEffect(apPostEffect, alPrio);
}

//-----------------------------------------------------------------------
