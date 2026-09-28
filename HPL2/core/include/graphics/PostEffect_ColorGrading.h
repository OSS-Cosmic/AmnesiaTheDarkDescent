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

#ifndef HPL_POSTEFFECT_COLOR_GRADING_H
#define HPL_POSTEFFECT_COLOR_GRADING_H

#include "graphics/Image.h"
#include "graphics/PostEffect.h"
#include "graphics/RIProgram.h"
#include "system/SystemTypes.h"

#include <map>

namespace hpl {

// AMFP colour grading: a 3D lookup table applied to the display-space image,
// optionally cross-faded between two tables. Tables are N x N*N strips
// (N slices of N x N stacked vertically, colorgrading_base.png is 32 x 1024),
// looked up in textures/gradingmaps with colorgrading_base.png as fallback.
// Only registered by the AMFP build (see cGraphics::Init).
class cPostEffectParams_ColorGrading : public iPostEffectParams {
public:
    cPostEffectParams_ColorGrading()
        : iPostEffectParams("ColorGrading"),
          msTextureFile1("colorgrading_base.png"), msTextureFile2(""),
          mfCrossFadeAlpha(0.0f), mbIsReinitialisation(false) {}

    kPostEffectParamsClassInit(cPostEffectParams_ColorGrading)

    tString msTextureFile1;
    tString msTextureFile2;
    float mfCrossFadeAlpha;     // 0 = table 1, 1 = table 2
    bool mbIsReinitialisation;  // drop all cached tables first
};

class cPostEffectType_ColorGrading : public iPostEffectType {
    friend class cPostEffect_ColorGrading;

public:
    cPostEffectType_ColorGrading(cGraphics *apGraphics, cResources *apResources);
    virtual ~cPostEffectType_ColorGrading();

    iPostEffect *CreatePostEffect(iPostEffectParams *apParams) override;

private:
    RIProgram m_program;
};

class cPostEffect_ColorGrading : public iPostEffect {
public:
    cPostEffect_ColorGrading(cGraphics *apGraphics, cResources *apResources,
                             iPostEffectType *apType);
    ~cPostEffect_ColorGrading();

    void SetCrossFadeAlpha(float afCrossFadeAlpha);

    void RenderEffect(const PostEffectRenderCtx &ctx) override;

private:
    void OnSetParams() override;
    iPostEffectParams *GetTypeSpecificParams() override { return &mParams; }

    Image *LoadLUT(tString asLUTName);
    void DestroyLUTs();

    cPostEffectType_ColorGrading *mpSpecificType;
    cPostEffectParams_ColorGrading mParams;

    Image *mpLUT1;
    Image *mpLUT2;
    std::map<tString, Image *> mLUTMap;
};

}; // namespace hpl
#endif // HPL_POSTEFFECT_COLOR_GRADING_H
