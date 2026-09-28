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

#include "PostEffect_Cells.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

namespace hpl
{
	cPostEffectType_Cells::cPostEffectType_Cells(cGraphics *apGraphics, cResources *apResources) : iPostEffectType("Cells",apGraphics,apResources)
	{
		LoadSlangGraphics(&mpGraphics->device, m_program, mpResources,
		                  "posteffect_fullscreen.vert", "posteffect_blit.frag");
	}

	//-----------------------------------------------------------------------

	cPostEffectType_Cells::~cPostEffectType_Cells()
	{
		m_program.dispose(&mpGraphics->device);
	}

	//-----------------------------------------------------------------------

	iPostEffect * cPostEffectType_Cells::CreatePostEffect(iPostEffectParams *apParams)
	{
		(void)apParams;
		return hplNew(cPostEffect_Cells, (mpGraphics,mpResources,this));
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// POST EFFECT
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cPostEffect_Cells::cPostEffect_Cells(cGraphics *apGraphics, cResources *apResources, iPostEffectType *apType) : iPostEffect(apGraphics,apResources,apType)
	{
	}

	//-----------------------------------------------------------------------

	cPostEffect_Cells::~cPostEffect_Cells()
	{

	}

	//-----------------------------------------------------------------------

	void cPostEffect_Cells::RenderEffect(const PostEffectRenderCtx &ctx)
	{
		cPostEffectType_Cells *pType = static_cast<cPostEffectType_Cells*>(mpType);

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
		pType->m_program.bindPipeline(&mpGraphics->device, ctx.cmd, pipelineHash,
		                              "PostEffect_Cells", pipelineDesc);

		auto samplerDesc = mpGraphics->resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = ctx.inputSrv;
		bindings[1].handle = DescriptorBindingID::Create("sourceInput");
		pType->m_program.bindDescriptors(&mpGraphics->device, ctx.cmd, ctx.frameIndex,
		                                 bindings, 2);

		ctx.cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
		ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
	}

	//-----------------------------------------------------------------------

	void cPostEffect_Cells::OnSetParams()
	{

	}
}
