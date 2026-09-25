#pragma once

#include "HostDefinitions.h"
#ifdef __cplusplus
#include <cmath>
#endif

HOST_NAMESPACE_BEGIN

// Operate on the display-space peak, then scale RGB together. Per-channel
// shoulders and gamma change authored light tints (and turn saturated lights
// white near their sources). Peak normalization also bounds every channel.
SLANG_PUBLIC inline float3 toneMapRayTracedColor(float3 display, float gamma)
{
    float peak = display.x > display.y ? display.x : display.y;
    peak = peak > display.z ? peak : display.z;
    if (peak <= 0.0f) return float3(0.0f);

    const float knee = 0.8f;
    float mapped = peak;
    if (peak > knee)
        mapped = knee + (1.0f - knee) *
            (1.0f - (1.0f - knee) / (peak - knee + (1.0f - knee)));
    float safeGamma = gamma > 1e-4f ? gamma : 1e-4f;
#ifdef __cplusplus
    mapped = std::pow(mapped, 1.0f / safeGamma);
#else
    mapped = pow(mapped, 1.0f / safeGamma);
#endif
    // Divide before multiplying: avoids a large scale factor near black.
    return float3((display.x / peak) * mapped,
                  (display.y / peak) * mapped,
                  (display.z / peak) * mapped);
}

HOST_NAMESPACE_END
