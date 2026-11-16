#include "graphics/ToneMapBackendParams.h"

#include "Constants.h" // kRayTracedExposure, kRayTracedGammaBoost, kRayTracedSaturation

namespace hpl {

cToneMapBackendParams ResolveToneMapBackendParams(eRendererBackend aBackend,
                                                  float afUserGamma) {
    cToneMapBackendParams params{};
    if (aBackend == eRendererBackend_Standard) {
        // Every field an identity. The Standard blend paths (BlendModes.slang
        // encodeStandardBlendSource, StandardLighting.slang,
        // Standard.environment.3d) emit sRGBToLinear(display) / kSceneExposure
        // on the assumption that the tonemap is exactly linearToSRGB(linear *
        // exposure); a gamma curve or a chroma change here breaks
        // that pair and the base game's bit-for-bit look with it.
        params.mfGamma = afUserGamma;
        params.mfExposureMul = 1.0f;
        params.mfSaturation = 1.0f;
        return params;
    }

    // The ray-traced path composites physical radiance (color * intensity /
    // (d^2 + sourceRadiusSq)), which lands darker than Standard, mostly in the
    // shadows. It is lifted with a display-gamma boost (multiplied with the
    // player's slider) plus an optional extra exposure, and clips at display
    // white like Standard.
    params.mfGamma = afUserGamma * kRayTracedGammaBoost;
    params.mfExposureMul = kRayTracedExposure;
    // Keep authored chroma; any explicit grading is applied after the encode.
    params.mfSaturation = kRayTracedSaturation;
    return params;
}

} // namespace hpl
