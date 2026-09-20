/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2023 Michael Pollind
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

#include "graphics/PostEffect_ToneMap.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

namespace hpl {

namespace {
struct ToneMapPushConstants {
    float exposure;
    float shadowLift;
    float gamma;
    float shoulder;
};
} // namespace

cPostEffectType_ToneMap::cPostEffectType_ToneMap(cGraphics *apGraphics,
                                                 cResources *apResources)
    : iPostEffectType("ToneMap", apGraphics, apResources) {
    LoadSlangGraphics(&mpGraphics->device, m_program, apResources,
                      "posteffect_fullscreen.vert",
                      "posteffect_tonemap.frag");
}

cPostEffectType_ToneMap::~cPostEffectType_ToneMap() {
  // Runs in DestroyRenderObjects, before cGraphics::Dispose — device is alive.
  m_program.dispose(&mpGraphics->device);
}

iPostEffect *
cPostEffectType_ToneMap::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned effect.
    (void)apParams;
    return hplNew(cPostEffect_ToneMap, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_ToneMap::cPostEffect_ToneMap(cGraphics *apGraphics,
                                         cResources *apResources,
                                         iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpToneMapType(static_cast<cPostEffectType_ToneMap *>(mpType)) {}

cPostEffect_ToneMap::~cPostEffect_ToneMap() {}

void cPostEffect_ToneMap::RenderEffect(const PostEffectRenderCtx &ctx) {
    // Single fullscreen pass: sample the (bloom-composited) HDR pogo input, ACES
    // tonemap to linear [0,1], write the pogo output. The composite handles the
    // pogo toggle / barriers around this call.
    RIRenderingAttachment color = {};
    color.view    = ctx.outputView;
    color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc beginDesc = {};
    beginDesc.renderArea.width  = ctx.width;
    beginDesc.renderArea.height = ctx.height;
    beginDesc.colorCount = 1;
    beginDesc.colors     = &color;
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    mpToneMapType->m_program.bindPipeline(&mpGraphics->device, ctx.cmd, kHash,
                                          "PostEffect_ToneMap", pipelineDesc);

    auto samplerDesc = mpGraphics->resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
    {
        RIProgram::DescriptorBinding bindings[2] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = ctx.inputSrv;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        mpToneMapType->m_program.bindDescriptors(&mpGraphics->device, ctx.cmd,
                                                 ctx.frameIndex, bindings, 2);
    }

    ToneMapPushConstants pc{};
    pc.exposure = mParams.mfExposure;
    pc.shadowLift = mParams.mfShadowLift;
    // User display-gamma setting, applied in-shader as the final encode step
    // (replaces the deprecated SDL window-brightness ramp). Authored by the
    // game's cLuxConfigHandler and pushed in via the tonemap params.
    pc.gamma = mParams.mfGamma;
    // The Standard backend reproduces the base game's 8-bit buffer, which
    // clipped highlights at white; the ray-traced backend rolls them off.
    pc.shoulder = mpGraphics->GetRendererBackend() == eRendererBackend_Standard ? 0.0f : 1.0f;
    ctx.cmd->vk_d3d12_setPushConstants(
        &mpGraphics->device, mpToneMapType->m_program, 0, sizeof(pc), &pc);

    ctx.cmd->draw(&mpGraphics->device, 3, 1, 0, 0);
    ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}

} // namespace hpl
