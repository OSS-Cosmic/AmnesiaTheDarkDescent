#pragma once
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Shared water-surface frame, wave-normal, Fresnel, and reflection-normal math.
// The geometric derivative frame is used instead of the vertex normal/tangent
// because many water meshes ship without usable normal/tangent streams; the
// usual +Z/+X fallback points sideways for an XZ water plane. Keeping the
// derivative inputs at the fragment call site also makes this module stage-free.

#include "../Constants.h"
import SceneTypes;
import bindless;
import Translucent.WaterCommon;

public struct WaterSurfaceFrame
{
    public float3 N;    // geometric surface normal, already flipped to face the camera
    public float3 T;    // unit tangent in the surface plane
    public float3 B;    // UV-handed bitangent
    public bool   valid; // false when the derivatives are degenerate/nonfinite
};

// dpdx/dpdy are supplied by the caller so this module has no derivative
// instructions and remains usable from stages other than a fragment shader.
// eye = normalize(posW - cameraPosW), i.e. camera -> surface.
public WaterSurfaceFrame waterSurfaceFrame(float3 dpdx, float3 dpdy,
    float2 duvdx, float2 duvdy, float3 eye)
{
    WaterSurfaceFrame frame;
    float3 c  = cross(dpdx, dpdy);
    float  c2 = dot(c, c);

    // Comparisons deliberately catch NaN without isnan(), which can be
    // optimized away under fast math. The upper bound also rejects infinity
    // and squared-length overflow before normalize() can produce NaN.
    frame.valid = (c2 > 1.0e-20f) && (c2 < 3.402823466e+38f);
    if (!frame.valid)
    {
        // A finite up-axis fallback, oriented toward the camera where possible.
        frame.N = float3(0.f, (eye.y > 0.f) ? -1.f : 1.f, 0.f);
        frame.T = normalize(cross(float3(1.f, 0.f, 0.f), frame.N));
        frame.B = cross(frame.N, frame.T);
        return frame;
    }

    float3 N = normalize(cross(dpdx, dpdy));
    if (dot(eye, N) > 0.0f)
        N = -N;                                       // face the camera
    // Invert the screen-to-UV Jacobian. Unlike dpdx alone, this frame stays
    // attached to the material as the camera rotates.
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    float3 T = abs(N.y) < 0.99f ? normalize(cross(float3(0,1,0), N))
                              : normalize(cross(float3(1,0,0), N));
    float3 B = cross(N, T);
    if (isfinite(determinant) && abs(determinant) > 1.0e-12f)
    {
        float3 rawT = (dpdx * duvdy.y - dpdy * duvdx.y) / determinant;
        float3 rawB = (dpdy * duvdx.x - dpdx * duvdy.x) / determinant;
        rawT -= N * dot(N, rawT);
        float t2 = dot(rawT, rawT);
        float b2 = dot(rawB, rawB);
        if (all(isfinite(rawT)) && all(isfinite(rawB)) && t2 > 1.0e-20f
            && t2 < 1.0e30f && b2 > 1.0e-20f && b2 < 1.0e30f)
        {
            T = rawT * rsqrt(t2);
            B = cross(N, T) * (dot(cross(N, T), rawB) < 0.0f ? -1.0f : 1.0f);
        }
    }

    frame.N = N;
    frame.T = T;
    frame.B = B;
    return frame;
}

// Legacy tint is an authored background multiplier, not absorption per metre.
// Fade it in at the shore, then retain that tint in deep water so existing
// materials do not exponentially erase the submerged scene.
public float3 waterDepthTransmission(float3 tint, float thickness)
{
    return pow(clamp(tint, 0.001f, 1.0f), saturate(thickness));
}

public float waterReflectionWeight(float fresnel, float fade, bool valid)
{
    return valid ? saturate(fresnel * fade) : 0.0f;
}

public float waterShoreDistortion(float thickness)
{
    return smoothstep(0.0f, 0.15f, thickness);
}

// Device depth is 0..1 and raster UV is top-down, matching the flipped viewport.
// Missing/clear depth retains the one-unit authored tint instead of going black.
public float waterBackgroundThickness(float2 uv, float depth, float surfaceViewZ,
                                      float4x4 inverseProjection)
{
    if (!isfinite(depth) || depth < 0.0f || depth >= 1.0f) return 1.0f;
    float4 h = mul(inverseProjection, float4(uv.x*2.0f-1.0f, 1.0f-uv.y*2.0f, depth, 1));
    if (!all(isfinite(h)) || abs(h.w) < 1.0e-8f) return 1.0f;
    float3 background = h.xyz / h.w;
    float rayLength = length(background);
    if (!all(isfinite(background)) || !isfinite(rayLength) || background.z >= -1.0e-5f)
        return 1.0f;
    return clamp((surfaceViewZ-background.z) * rayLength / -background.z, 0.0f, 20.0f);
}

// RT water needs resolved, broad slopes in addition to the scrolling normal
// texture. UV advection alone can leave a flat/mip-filtered normal map looking
// like glass. Filter each analytic wave over the raster footprint to avoid
// introducing distant shimmer. Authored amplitude/speed still control it.
public float3 waterSurfaceNormalTangent(WaterMaterial water, float2 baseUv,
    float2 dBaseUvDx, float2 dBaseUvDy, float afT)
{
    float3 detail = waterWaveNormalTangent(water, baseUv, dBaseUvDx, dBaseUvDy, afT);
    float frequency = max(abs(water.waveFreq), 0.01f);
    float time = afT * water.waveSpeed;
    float2 k1 = float2(5.2f, 3.1f) * frequency;
    float2 k2 = float2(-3.7f, 6.4f) * frequency;
    float2 footprint = float2(
        abs(dot(k1, dBaseUvDx)) + abs(dot(k1, dBaseUvDy)),
        abs(dot(k2, dBaseUvDx)) + abs(dot(k2, dBaseUvDy)));
    float2 filtered = exp(-0.5f * footprint * footprint);
    float2 slopes = normalize(k1) * cos(dot(k1, baseUv) + time * 1.1f) * filtered.x
                  + normalize(k2) * cos(dot(k2, baseUv) - time * 0.8f) * filtered.y * 0.65f;
    slopes *= 0.16f * clamp(water.waveAmplitude, 0.0f, 2.0f);
    return normalize(float3(detail.xy * 1.6f + slopes * detail.z, detail.z));
}

// Wave normal in WORLD space: rotate the tangent-space wave normal through
// the derivative frame, then flip it to face the camera.
public float3 waterWaveNormalWorld(WaterMaterial water, float2 baseUv,
                                   float2 dBaseUvDx, float2 dBaseUvDy, float afT,
                                   WaterSurfaceFrame frame, float3 eye)
{
    float3 nT = waterSurfaceNormalTangent(water, baseUv, dBaseUvDx, dBaseUvDy, afT);
    float3 waveN = normalize(nT.x * frame.T + nT.y * frame.B + nT.z * frame.N);
    if (dot(eye, waveN) > 0.0f)
        waveN = -waveN;
    return waveN;
}

// Schlick-Fresnel — positive-clamped pow base, moved verbatim from
// Water.frag.slang::computeFresnel.
public float waterFresnel(float edotN, float bias, float pw)
{
    float facing = max(1.0f - edotN, 1.0e-6f);
    return saturate(bias + (1.0f - bias) * pow(facing, pw));
}

public float waterFresnelTerm(WaterMaterial water, float3 eye, float3 waveN)
{
    return waterFresnel(max(dot(-eye, waveN), 0.0f),
                        water.frenselBias, water.frenselPow);
}

public float waterDistanceFade(WaterMaterial water, float viewZ)
{
    if (water.reflectionFadeEnd > water.reflectionFadeStart)
    {
        float fadeLen = water.reflectionFadeEnd - water.reflectionFadeStart;
        return 1.0f - saturate((viewZ - water.reflectionFadeStart) / fadeLen);
    }
    return 1.0f;
}

public float3 waterReflectionNormal(WaterSurfaceFrame frame, float3 waveN)
{
    return normalize(lerp(frame.N, waveN, kWaterReflectionTurbulence));
}
