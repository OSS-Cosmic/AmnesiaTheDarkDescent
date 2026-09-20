/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2023 Michael Pollind
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#include "graphics/PostEffect_RadialBlur.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIRenderer.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

namespace hpl {

namespace {
struct RadialBlurPushConstants {
  float size;
  float blurStartDist;
  float screenDim[2];
};
} // namespace

cPostEffectType_RadialBlur::cPostEffectType_RadialBlur(cGraphics *apGraphics,
                                                       cResources *apResources)
    : iPostEffectType("RadialBlur", apGraphics, apResources) {
  LoadSlangGraphics(&mpGraphics->device, m_program, apResources,
                    "posteffect_fullscreen.vert",
                    "posteffect_radial_blur.frag");
}

cPostEffectType_RadialBlur::~cPostEffectType_RadialBlur() {
  m_program.dispose(&mpGraphics->device);
}

iPostEffect *
cPostEffectType_RadialBlur::CreatePostEffect(iPostEffectParams *apParams) {
  // cGraphics::CreatePostEffect already calls SetParams on the returned
  // effect — leave initialisation to that single site.
  (void)apParams;
  return hplNew(cPostEffect_RadialBlur, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_RadialBlur::cPostEffect_RadialBlur(cGraphics *apGraphics,
                                               cResources *apResources,
                                               iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpRadialBlurType(static_cast<cPostEffectType_RadialBlur *>(mpType)) {}

cPostEffect_RadialBlur::~cPostEffect_RadialBlur() {}

void cPostEffect_RadialBlur::RenderEffect(const PostEffectRenderCtx &ctx) {
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

  const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
  mpRadialBlurType->m_program.bindPipeline(
      &mpGraphics->device, ctx.cmd, pipelineHash, "PostEffect_RadialBlur",
      pipelineDesc);

  auto samplerDesc = mpGraphics->resolve_filter_descriptor(
      eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
      eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

  RIProgram::DescriptorBinding bindings[2] = {};
  bindings[0].descriptor = *samplerDesc;
  bindings[0].handle = DescriptorBindingID::Create("inputSampler");
  bindings[1].descriptor = ctx.inputSrv;
  bindings[1].handle = DescriptorBindingID::Create("sourceInput");
  mpRadialBlurType->m_program.bindDescriptors(&mpGraphics->device, ctx.cmd,
                                              ctx.frameIndex, bindings, 2);

  RadialBlurPushConstants pc{};
  pc.size = mParams.mfSize;
  pc.blurStartDist = mParams.mfBlurStartDist;
  pc.screenDim[0] = static_cast<float>(ctx.width);
  pc.screenDim[1] = static_cast<float>(ctx.height);
  ctx.cmd->vk_d3d12_setPushConstants(
      &mpGraphics->device, mpRadialBlurType->m_program, 0, sizeof(pc), &pc);

  ctx.cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
  ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}

} // namespace hpl
