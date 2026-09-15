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

#ifndef HPL_POSTEFFECT_BLOOM_H
#define HPL_POSTEFFECT_BLOOM_H

#include "graphics/PostEffect.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgram.h"
#include "math/MathTypes.h"

#include <vector>

namespace hpl {

class cPostEffectParams_Bloom : public iPostEffectParams {
public:
    cPostEffectParams_Bloom()
        : iPostEffectParams("Bloom"),
          mvRgbToIntensity(0.3f, 0.58f, 0.12f),
          mfBlurSize(1.0f),
          mlBlurIterations(2),
          mfThreshold(1.0f),
          mfSoftKnee(0.5f),
          mfStrength(0.06f),
          mfFilterRadius(1.0f),
          mlMaxMips(6) {}

    kPostEffectParamsClassInit(cPostEffectParams_Bloom)

    // Legacy params from the old single-level Gaussian bloom. Kept so existing
    // .cfg / script setters still compile; the mip-chain implementation ignores
    // them.
    cVector3f mvRgbToIntensity;
    float     mfBlurSize;
    int       mlBlurIterations;

    // Modern mip-chain bloom params.
    float mfThreshold;    // soft-knee bright-pass threshold (max-channel)
    float mfSoftKnee;     // knee width as a fraction of threshold [0,1]
    float mfStrength;     // overall bloom contribution at composite
    float mfFilterRadius; // tent-filter spread (texels) during upsample
    int   mlMaxMips;      // cap on the number of mip-chain levels
};

class cPostEffectType_Bloom : public iPostEffectType {
    friend class cPostEffect_Bloom;

public:
    cPostEffectType_Bloom(cGraphics *apGraphics, cResources *apResources);
    virtual ~cPostEffectType_Bloom();

    iPostEffect *CreatePostEffect(iPostEffectParams *apParams) override;

private:
    // Three programs share the fullscreen vert. downsample builds the bloom
    // mip chain (with prefilter+Karis on the first mip); upsample tent-filters
    // a coarser mip and lerps it up (weights sum to 1); composite adds bloom mip[0]
    // back into the HDR scene.
    RIProgram m_downsampleProgram;
    RIProgram m_upsampleProgram;
    RIProgram m_compositeProgram;
};

class cPostEffect_Bloom : public iPostEffect {
public:
    cPostEffect_Bloom(cGraphics *apGraphics, cResources *apResources,
                      iPostEffectType *apType);
    ~cPostEffect_Bloom();

    void RenderEffect(const PostEffectRenderCtx &ctx) override;

private:
    void OnSetParams() override {}
    iPostEffectParams *GetTypeSpecificParams() override { return &mParams; }

    // Bloom mip chain owned by this effect instance. m_mips[0] is half the
    // viewport resolution, each subsequent mip half again. Allocated lazily on
    // first RenderEffect and rebuilt when the viewport size changes (compared
    // against m_mipsW / m_mipsH).
    std::vector<PostEffectColorTarget> m_mips;
    uint32_t m_mipsW = 0;
    uint32_t m_mipsH = 0;

    cPostEffectType_Bloom *mpBloomType;
    cPostEffectParams_Bloom mParams;
};

}; // namespace hpl
#endif
