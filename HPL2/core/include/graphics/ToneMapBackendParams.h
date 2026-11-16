#ifndef HPL_TONEMAP_BACKEND_PARAMS_H
#define HPL_TONEMAP_BACKEND_PARAMS_H

#include "graphics/GraphicsTypes.h"

namespace hpl {

    //------------------------------------------------------------------------
    // The three tonemap push constants whose value depends on which renderer
    // backend produced the frame. Pulled out of cPostEffect_ToneMap so the
    // decision -- which is where the backend-matching rules live -- can be
    // exercised without a device.
    //
    // Standard is the identity case on every field: it emits
    // sRGBToLinear(display) / kSceneExposure and relies on the tonemap being
    // its exact inverse, so the base game's look returns bit-for-bit. The
    // ray-traced backend composites physical radiance and differs by a gamma
    // boost, an extra exposure and an optional chroma scale. See kRayTracedExposure / kRayTracedSaturation
    // in amnesia/slang/Constants.h for the reasoning behind each value.

    struct cToneMapBackendParams
    {
        // User display-gamma; times kRayTracedGammaBoost on the ray-traced path.
        float mfGamma = 1.0f;
        // Multiplies the tone map's scene exposure. kRayTracedExposure on the
        // ray-traced path, 1 (identity) on Standard.
        float mfExposureMul = 1.0f;
        // Chroma scale toward Rec.709 luma. 1 = exact no-op.
        float mfSaturation = 1.0f;
    };

    cToneMapBackendParams ResolveToneMapBackendParams(eRendererBackend aBackend, float afUserGamma);
}

#endif
