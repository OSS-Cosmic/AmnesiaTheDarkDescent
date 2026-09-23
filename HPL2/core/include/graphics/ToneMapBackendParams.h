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
    // ray-traced backend composites physical radiance and needs all three
    // corrections. See kRayTracedGammaBias / kRayTracedSaturation in
    // amnesia/slang/Constants.h for the reasoning behind each value.

    struct cToneMapBackendParams
    {
        // User display-gamma, plus kRayTracedGammaBias on the ray-traced path.
        float mfGamma = 1.0f;
        // 1 = roll highlights off above kToneMapShoulder, 0 = clip at display
        // white like the base game's 8-bit buffer.
        float mfShoulder = 0.0f;
        // Chroma scale toward Rec.709 luma. 1 = exact no-op.
        float mfSaturation = 1.0f;
    };

    cToneMapBackendParams ResolveToneMapBackendParams(eRendererBackend aBackend, float afUserGamma);
}

#endif
