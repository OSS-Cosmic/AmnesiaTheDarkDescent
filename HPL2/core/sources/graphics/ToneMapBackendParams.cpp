#include "graphics/ToneMapBackendParams.h"

#include "Constants.h" // kRayTracedGammaBias, kRayTracedSaturation

namespace hpl {

cToneMapBackendParams ResolveToneMapBackendParams(eRendererBackend aBackend,
                                                  float afUserGamma) {
    cToneMapBackendParams params{};
    if (aBackend == eRendererBackend_Standard) {
        // Every field an identity. The Standard blend paths (BlendModes.slang
        // encodeStandardBlendSource, StandardLighting.slang,
        // Standard.environment.3d) emit sRGBToLinear(display) / kSceneExposure
        // on the assumption that the tonemap is exactly linearToSRGB(linear *
        // exposure); a gamma curve, a shoulder or a chroma change here breaks
        // that pair and the base game's bit-for-bit look with it.
        params.mfGamma = afUserGamma;
        params.mfShoulder = 0.0f;
        params.mfSaturation = 1.0f;
        return params;
    }

    // The ray-traced path composites physical radiance (color * intensity /
    // (d^2 + sourceRadiusSq)) with only kSceneExposure to lift it, so it lands
    // darker than Standard. The bias is additive rather than a scale so the
    // player's gamma slider still moves the result by the amount they chose --
    // it just starts higher.
    params.mfGamma = afUserGamma + kRayTracedGammaBias;
    // Highlights roll off instead of clipping: unlike the base game there is no
    // 8-bit buffer doing it for free.
    params.mfShoulder = 1.0f;
    // Keep authored chroma; any explicit grading is applied after the peak curve.
    params.mfSaturation = kRayTracedSaturation;
    return params;
}

} // namespace hpl
