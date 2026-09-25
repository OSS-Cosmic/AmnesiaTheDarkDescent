#pragma once

#include "HostDefinitions.h"
#ifdef __cplusplus
#include <cmath>
#endif

HOST_NAMESPACE_BEGIN

// Reversible gamma compensation around RT forward blending. The final RT
// tone map powers the display peak and scales RGB together, so its inverse
// must do the same. Per-channel powers here darken a flame's weaker green/
// blue channels without the final tone map undoing that saturation change.
// Keep signed values and HDR headroom; alpha/coverage is never transformed.
SLANG_PUBLIC inline float3 rtForwardColorGamma(float3 display, float exponent)
{
    if (exponent == 1.0f) return display;
    float peak = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float magnitude = display[i] < 0.0f ? -display[i] : display[i];
        peak = magnitude > peak ? magnitude : peak;
    }
    if (peak == 0.0f) return display;
#ifdef __cplusplus
    const float mapped = std::pow(peak, exponent);
#else
    const float mapped = pow(peak, exponent);
#endif
    // Divide before multiplying to avoid a large scale factor near black.
    return float3((display.x / peak) * mapped,
                  (display.y / peak) * mapped,
                  (display.z / peak) * mapped);
}

// Legacy 8-bit clip for forward blending. The base game blended particles
// and translucent meshes into a display-space UNORM target, so stacked
// additive fire saturated its red channel first and then climbed toward
// yellow/white. The RT bracket keeps fp16 headroom and a peak-preserving
// tone map, which otherwise holds the fire at its authored orange ratio.
// Cap each channel the forward passes raised at display white, unless the
// scene underneath (`base`, the display value when the bracket opened) was
// already brighter: untouched pixels and scene HDR pass through unchanged.
// Both arguments are in the bracket's gamma-lifted display space.
SLANG_PUBLIC inline float3 rtForwardColorClip(float3 display, float3 base)
{
    float3 result = display;
    for (int i = 0; i < 3; ++i)
    {
        const float ceiling = base[i] > 1.0f ? base[i] : 1.0f;
        result[i] = display[i] < ceiling ? display[i] : ceiling;
    }
    return result;
}

HOST_NAMESPACE_END
