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

#ifndef HPL_POSTEFFECT_H
#define HPL_POSTEFFECT_H

#include "graphics/GraphicsTypes.h"
#include "graphics/RITypes.h"
#include "math/MathTypes.h"

#include "graphics/RIPreamble.h"

namespace hpl {

class cGraphics;
class cResources;
class cPostEffectComposite;
class iPostEffect;

//------------------------------------------

#define kPostEffectParamsClassInit(aClass)                                     \
    void CopyTo(iPostEffectParams *apDestParams) {                             \
        aClass *pCastParams = static_cast<aClass *>(apDestParams);             \
        *pCastParams = *this;                                                  \
    }                                                                          \
    void LoadFrom(iPostEffectParams *apSrcParams) {                            \
        aClass *pCastParams = static_cast<aClass *>(apSrcParams);              \
        *this = *pCastParams;                                                  \
    }

//------------------------------------------

class iPostEffectParams {
public:
    iPostEffectParams(const tString &asName) : msName(asName) {}
    virtual ~iPostEffectParams() {}
    const tString &GetName() { return msName; }

    virtual void CopyTo(iPostEffectParams *apDestParams) = 0;
    virtual void LoadFrom(iPostEffectParams *apSrcParams) = 0;

private:
    tString msName;
};

//------------------------------------------

// Per-effect render context. The composite builds one of these per draw
// and hands it to iPostEffect::RenderEffect. inputSrv is the just-written
// pogo half (sampled image); outputTexture and outputView refer to the
// composite-owned attachment and are borrowed for the duration of the call.
// Effects must not dispose, retain, or otherwise take ownership of either
// resource. outputView is a value copy so its complete RI view metadata is
// available to attachment consumers.
struct PostEffectRenderCtx {
    struct RICmd        *cmd;
    struct RIDescriptor  inputSrv;
    struct RITexture     *outputTexture; // Borrowed; valid only during RenderEffect.
    struct RITextureView  outputView;
    uint32_t               width;
    uint32_t               height;
    uint32_t               frameIndex;
    float                  frameTime;
    bool                   isLastEffect;
};

//------------------------------------------

class iPostEffectType {
public:
    iPostEffectType(const tString &asName, cGraphics *apGraphics,
                    cResources *apResources);
    virtual ~iPostEffectType();

    const tString &GetName() { return msName; }

    virtual iPostEffect *CreatePostEffect(iPostEffectParams *apParams) = 0;

protected:
    cGraphics  *mpGraphics;
    cResources *mpResources;

    tString msName;
};

//------------------------------------------

class iPostEffect {
public:
    iPostEffect(cGraphics *apGraphics, cResources *apResources,
                iPostEffectType *apType);
    virtual ~iPostEffect();

    void SetDisabled(bool abX) { mbDisabled = abX; }
    bool IsDisabled() { return mbDisabled; }

    void SetActive(bool abX);
    bool IsActive() { return mbDisabled == false && mbActive; }

    void SetParams(iPostEffectParams *apSrcParams);
    void GetParams(iPostEffectParams *apDestParams);

    virtual void Reset() {}

    // Driven by cPostEffectComposite::Render. Reads from ctx.inputSrv and
    // writes to ctx.outputView (or to an effect-owned scratch and copies
    // back). The composite handles the pogo toggle/barrier after the call.
    virtual void RenderEffect(const PostEffectRenderCtx &ctx) = 0;

protected:
    virtual void OnSetActive(bool abX) {}
    virtual void OnSetParams() = 0;
    virtual iPostEffectParams *GetTypeSpecificParams() = 0;

    cGraphics       *mpGraphics;
    cResources      *mpResources;
    iPostEffectType *mpType;

    bool mbDisabled;
    bool mbActive;
};

//------------------------------------------

}; // namespace hpl
#endif // HPL_POSTEFFECT_H
