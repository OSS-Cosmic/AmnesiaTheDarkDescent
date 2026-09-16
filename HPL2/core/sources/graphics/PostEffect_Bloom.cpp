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

#include "graphics/PostEffect_Bloom.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

#include <algorithm>

namespace hpl {

namespace {
// Must match the [vk::push_constant] blocks in the bloom .frag.slang shaders.
struct BloomDownsamplePushConstants {
    float    threshold;
    float    knee;
    uint32_t applyThreshold;
    float    _pad;
};

struct BloomUpsamplePushConstants {
    float filterRadius;
    float _pad[3];
};

struct BloomCompositePushConstants {
    float strength;
    float _pad[3];
};

} // namespace

cPostEffectType_Bloom::cPostEffectType_Bloom(cGraphics *apGraphics,
                                             cResources *apResources)
    : iPostEffectType("Bloom", apGraphics, apResources) {
    LoadSlangGraphics(&mpGraphics->device, m_downsampleProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_downsample.frag.spv");
    LoadSlangGraphics(&mpGraphics->device, m_upsampleProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_upsample.frag.spv");
    LoadSlangGraphics(&mpGraphics->device, m_compositeProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_composite.frag.spv");
}

cPostEffectType_Bloom::~cPostEffectType_Bloom() {
  m_downsampleProgram.dispose(&mpGraphics->device);
  m_upsampleProgram.dispose(&mpGraphics->device);
  m_compositeProgram.dispose(&mpGraphics->device);
}

iPostEffect *
cPostEffectType_Bloom::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned
    // effect — leave initialisation to that single site.
    (void)apParams;
    return hplNew(cPostEffect_Bloom, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_Bloom::cPostEffect_Bloom(cGraphics *apGraphics,
                                     cResources *apResources,
                                     iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpBloomType(static_cast<cPostEffectType_Bloom *>(mpType)) {}

cPostEffect_Bloom::~cPostEffect_Bloom() {
    for (auto &mip : m_mips)
        DestroyPostEffectColorTarget(mip);
    m_mips.clear();
}

void cPostEffect_Bloom::RenderEffect(const PostEffectRenderCtx &ctx) {
    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    // ----- (Re)build the bloom mip chain on first use / viewport resize -----
    // N = number of mips, capped by mlMaxMips and by how far we can halve the
    // viewport while keeping both dimensions >= 1. mip[i] is (w >> (i+1)).
    const int maxMips = std::max(mParams.mlMaxMips, 1);
    int mipCount = 0;
    while (mipCount < maxMips &&
           (ctx.width >> (mipCount + 1)) >= 1u &&
           (ctx.height >> (mipCount + 1)) >= 1u) {
        ++mipCount;
    }
    mipCount = std::max(mipCount, 1);

    if (m_mips.size() != (size_t)mipCount || m_mipsW != ctx.width ||
        m_mipsH != ctx.height) {
        for (auto &mip : m_mips)
            DestroyPostEffectColorTarget(mip);
        m_mips.assign((size_t)mipCount, PostEffectColorTarget{});
        for (int i = 0; i < mipCount; ++i) {
            const uint32_t mw = std::max(ctx.width >> (i + 1), 1u);
            const uint32_t mh = std::max(ctx.height >> (i + 1), 1u);
            char name[48];
            snprintf(name, sizeof(name), "PostEffect_Bloom.mip%d", i);
            CreatePostEffectColorTarget(m_mips[(size_t)i], mw, mh,
                                        cGraphics::PogoColorFormat, 0u, name);
            // Rest state: every mip lives in SHADER_RESOURCE between frames.
            RITextureBarrier init(&m_mips[(size_t)i].texture,
                             RI_RESOURCE_STATE_UNDEFINED,
                             RI_RESOURCE_STATE_SHADER_RESOURCE,
                             RI_STAGE_NONE, RI_STAGE_FRAGMENT);
            ctx.cmd->vk_d3d12_textureBarrier(init);
        }
        m_mipsW = ctx.width;
        m_mipsH = ctx.height;
    }

    auto samplerDesc = mpGraphics->resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    // Pipelines: downsample + composite are opaque; upsample alpha-blends
    // (SRC_ALPHA/ONE_MINUS_SRC_ALPHA) onto the destination mip's own downsample
    // content. The shader's alpha is the lerp weight, so the mip weights sum to
    // 1 instead of adding up every level.
    PostEffectPipelineState downState{};
    InitPostEffectPipelineState(downState, cGraphics::PogoColorFormat, false);
    PostEffectPipelineState upState{};
    InitPostEffectPipelineState(upState, cGraphics::PogoColorFormat, true);
    PostEffectPipelineState compState{};
    InitPostEffectPipelineState(compState, cGraphics::PogoColorFormat, false);

    const hash_t kDownHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    const hash_t kUpHash   = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
    const hash_t kCompHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/2u);

    auto barrier = [&](RITexture *tex, enum RIResourceState_e before,
                       uint32_t beforeStage, enum RIResourceState_e after,
                       uint32_t afterStage) {
        RITextureBarrier b(tex, before, after, beforeStage, afterStage);
        ctx.cmd->vk_d3d12_textureBarrier(b);
    };

    // Render a fullscreen-triangle pass into `destView` (size mw x mh) sampling
    // `inputDesc` through `prog`/`pipeline`, with `pc` push constants. `loadOp`
    // is LOAD for the blended upsample, DONT_CARE otherwise.
    auto fullscreenPass = [&](RIProgram &prog, hash_t pipeHash,
                              VkGraphicsPipelineCreateInfo *pipeCI,
                              const char *dbgName, VkImageView destView,
                              uint32_t mw, uint32_t mh,
                              const RIDescriptor &inputDesc,
                              enum RIAttachmentLoadOp_e loadOp, const void *pc,
                              uint32_t pcSize) {
        RITextureView destViewRi = {};
        destViewRi.vk.image = destView;

        RIRenderingAttachment color = {};
        color.view    = destViewRi;
        color.loadOp  = loadOp;
        color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

        RIBeginRenderingDesc beginDesc = {};
        beginDesc.renderArea.width  = (int16_t)mw;
        beginDesc.renderArea.height = (int16_t)mh;
        beginDesc.colorCount = 1;
        beginDesc.colors     = &color;
        ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

        VkViewport vp = {0.0f, 0.0f, (float)mw, (float)mh, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc = {{0, 0}, {mw, mh}};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        prog.bindPipeline(&mpGraphics->device, ctx.cmd, pipeHash, dbgName, pipeCI);

        RIProgram::DescriptorBinding bindings[2] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = inputDesc;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        prog.bindDescriptors(&mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 2);

        if (pc && pcSize)
            vkCmdPushConstants(cmd, prog.getPipelineLayout(),
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
    };

    auto mipW = [&](int i) { return std::max(ctx.width >> (i + 1), 1u); };
    auto mipH = [&](int i) { return std::max(ctx.height >> (i + 1), 1u); };

    // ----- 1. Prefilter + downsample the HDR scene into mip[0] -----
    barrier(&m_mips[0].texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
            RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
    {
        BloomDownsamplePushConstants pc{};
        pc.threshold      = mParams.mfThreshold;
        pc.knee           = mParams.mfThreshold * mParams.mfSoftKnee + 1e-4f;
        pc.applyThreshold = 1u;
        fullscreenPass(mpBloomType->m_downsampleProgram, kDownHash,
                       &downState.createInfo, "PostEffect_Bloom.downsample",
                       m_mips[0].view.vk.image, mipW(0), mipH(0), ctx.inputSrv,
                       RI_ATTACHMENT_LOAD_OP_DONT_CARE, &pc, sizeof(pc));
    }

    // ----- 2. Downsample chain: mip[i-1] -> mip[i] -----
    for (int i = 1; i < mipCount; ++i) {
        barrier(&m_mips[(size_t)(i - 1)].texture, RI_RESOURCE_STATE_RENDER_TARGET,
                RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE,
                RI_STAGE_FRAGMENT);
        barrier(&m_mips[(size_t)i].texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

        BloomDownsamplePushConstants pc{};
        pc.applyThreshold = 0u;
        fullscreenPass(mpBloomType->m_downsampleProgram, kDownHash,
                       &downState.createInfo, "PostEffect_Bloom.downsample",
                       m_mips[(size_t)i].view.vk.image, mipW(i), mipH(i),
                       m_mips[(size_t)(i - 1)].descriptor(),
                       RI_ATTACHMENT_LOAD_OP_DONT_CARE, &pc, sizeof(pc));
    }
    // After the loop: mip[N-1] is RENDER_TARGET, mips[0..N-2] are SHADER_RESOURCE.

    // ----- 3. Upsample chain: lerp mip[i] into mip[i-1] -----
    for (int i = mipCount - 1; i >= 1; --i) {
        barrier(&m_mips[(size_t)i].texture, RI_RESOURCE_STATE_RENDER_TARGET,
                RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE,
                RI_STAGE_FRAGMENT);
        barrier(&m_mips[(size_t)(i - 1)].texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

        BloomUpsamplePushConstants pc{};
        pc.filterRadius = mParams.mfFilterRadius;
        fullscreenPass(mpBloomType->m_upsampleProgram, kUpHash,
                       &upState.createInfo, "PostEffect_Bloom.upsample",
                       m_mips[(size_t)(i - 1)].view.vk.image, mipW(i - 1),
                       mipH(i - 1), m_mips[(size_t)i].descriptor(),
                       RI_ATTACHMENT_LOAD_OP_LOAD, &pc, sizeof(pc));
    }
    // After the loop: mip[0] is RENDER_TARGET, mips[1..N-1] are SHADER_RESOURCE.

    // ----- 4. Composite: scene + bloom(mip[0]) * strength -> pogo output -----
    barrier(&m_mips[0].texture, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

    RITextureView outView = {};
    outView.vk.image = ctx.outputView;

    RIRenderingAttachment outColor = {};
    outColor.view    = outView;
    outColor.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    outColor.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc outBeginDesc = {};
    outBeginDesc.renderArea.width  = (int16_t)ctx.width;
    outBeginDesc.renderArea.height = (int16_t)ctx.height;
    outBeginDesc.colorCount = 1;
    outBeginDesc.colors     = &outColor;
    ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, outBeginDesc);

    VkViewport viewport = {0.0f, 0.0f, (float)ctx.width, (float)ctx.height, 0.0f,
                           1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    mpBloomType->m_compositeProgram.bindPipeline(&mpGraphics->device, ctx.cmd, kCompHash,
                                                 "PostEffect_Bloom.composite",
                                                 &compState.createInfo);
    {
        RIProgram::DescriptorBinding bindings[3] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = ctx.inputSrv;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        bindings[2].descriptor = m_mips[0].descriptor();
        bindings[2].handle     = DescriptorBindingID::Create("bloomInput");
        mpBloomType->m_compositeProgram.bindDescriptors(
            &mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 3);
    }

    BloomCompositePushConstants cpc{};
    cpc.strength = mParams.mfStrength;
    vkCmdPushConstants(cmd, mpBloomType->m_compositeProgram.getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cpc), &cpc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
    // Exit invariant restored: every mip is back in SHADER_RESOURCE.
}

} // namespace hpl
