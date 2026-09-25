#pragma once

#include "HostDefinitions.h"
#ifdef __cplusplus
#include <cmath>
#endif

HOST_NAMESPACE_BEGIN

// Shared CPU/shader sRGB transfer functions. Keep the linear toe: replacing
// this with a power curve would hide the dark-material regression we correct.
SLANG_PUBLIC inline float rtDiffuseEncode(float value)
{
    if (value <= 0.0031308f) return value * 12.92f;
#ifdef __cplusplus
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
#else
    return 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
#endif
}

SLANG_PUBLIC inline float rtDiffuseDecode(float value)
{
    if (value <= 0.04045f) return value / 12.92f;
#ifdef __cplusplus
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
#else
    return pow((value + 0.055f) / 1.055f, 2.4f);
#endif
}

// Presentation-only compatibility for legacy diffuse textures. Work AFTER
// denoising and decals, never on bounce albedo or the light transport buffers.
// Preserve RT's encoded peak, but take RGB proportions from display-space
// albedo * illumination. The RT peak-based tone map then preserves these
// proportions through its gamma/shoulder. This preserves peak brightness,
// not luminance; weaker channels may become brighter. No HDR clipping.
// exposure <= 0 (or non-finite) disables the correction.
SLANG_PUBLIC inline float3 rtDiffuseColor(float3 albedo, float3 lighting, float exposure)
{
    float3 physical;
    for (int i = 0; i < 3; ++i)
    {
        albedo[i] = albedo[i] > 0.0f ? albedo[i] : 0.0f;
        lighting[i] = lighting[i] > 0.0f ? lighting[i] : 0.0f;
        physical[i] = albedo[i] * lighting[i];
    }
#ifdef __cplusplus
    if (!(exposure > 0.0f) || !std::isfinite(exposure)) return physical;
#else
    if (!(exposure > 0.0f) || !isfinite(exposure)) return physical;
#endif

    float peak = 0.0f;
    float targetPeak = 0.0f;
    float3 target;
    for (int i = 0; i < 3; ++i)
    {
        const float encoded = rtDiffuseEncode(physical[i] * exposure);
        peak = encoded > peak ? encoded : peak;
        target[i] = rtDiffuseEncode(albedo[i]) * rtDiffuseEncode(lighting[i] * exposure);
        targetPeak = target[i] > targetPeak ? target[i] : targetPeak;
    }
    if (peak <= 0.0f || targetPeak <= 0.0f) return float3(0.0f);

    float3 corrected;
    for (int i = 0; i < 3; ++i)
        // Normalize first to avoid a large multiplier near black.
        corrected[i] = rtDiffuseDecode((target[i] / targetPeak) * peak) / exposure;
    return corrected;
}

HOST_NAMESPACE_END
