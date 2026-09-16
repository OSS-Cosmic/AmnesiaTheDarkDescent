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

#ifndef HPL_POSTEFFECT_TONEMAP_H
#define HPL_POSTEFFECT_TONEMAP_H

#include "graphics/PostEffect.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgram.h"

namespace hpl {

class cPostEffectParams_ToneMap : public iPostEffectParams {
public:
    cPostEffectParams_ToneMap()
        : iPostEffectParams("ToneMap"), mfExposure(1.0f), mfShadowLift(0.0f),
          mfGamma(1.0f) {}

    kPostEffectParamsClassInit(cPostEffectParams_ToneMap)

    // Linear exposure multiply applied before the display encode. This is the
    // scene brightness control (kSceneExposure in Constants.h); a linear scale
    // preserves the particle/translucent blend ratios that a gamma curve breaks.
    float mfExposure;
    // Post black-point lift applied after the curve (0 = none); raises pitch-
    // blacks back into the visible range so deep shadows aren't crushed.
    float mfShadowLift;
    // User display-gamma (1.0 = identity) applied as the final encode step;
    // authored game-side in cLuxConfigHandler. Scene brightness comes from
    // mfExposure (kSceneExposure), NOT from this — see the shader header.
    float mfGamma;
};

class cPostEffectType_ToneMap : public iPostEffectType {
    friend class cPostEffect_ToneMap;

public:
    cPostEffectType_ToneMap(cGraphics *apGraphics, cResources *apResources);
    virtual ~cPostEffectType_ToneMap();

    iPostEffect *CreatePostEffect(iPostEffectParams *apParams) override;

private:
    RIProgram m_program; // posteffect_fullscreen.vert + posteffect_tonemap.frag
};

class cPostEffect_ToneMap : public iPostEffect {
public:
    cPostEffect_ToneMap(cGraphics *apGraphics, cResources *apResources,
                        iPostEffectType *apType);
    ~cPostEffect_ToneMap();

    void RenderEffect(const PostEffectRenderCtx &ctx) override;

private:
    void OnSetParams() override {}
    iPostEffectParams *GetTypeSpecificParams() override { return &mParams; }

    cPostEffectType_ToneMap *mpToneMapType;
    cPostEffectParams_ToneMap mParams;
};

}; // namespace hpl
#endif
