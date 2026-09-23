/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water reflection-guide capture — fragment stage.
//
// The host must bind three UNBLENDED MRTs in this order:
//   SV_TARGET0 positionViewZ  RGBA32F: .xyz = UN-BIASED world surface
//                                      position; .w = POSITIVE linear water
//                                      viewZ.
//   SV_TARGET1 normalWeight   RGBA16F: .xyz = normalized water reflection
//                                      normal; .w = Fresnel x authored
//                                      distance fade x water-surface fog
//                                      visibility.
//   SV_TARGET2 velocity       RG16F:   currentUV - previousUV, normalized to
//                                      the FULL overscan image.
//
// The attachments are zero-cleared and a zero texel means UNCOVERED. A surface
// with zero reflection contribution may legitimately write an invalid/zero
// guide because its tint and fog are still drawn by the original full-
// resolution water passes. If any guide input is invalid, all three targets
// are written as zero; no NaN or Inf is ever emitted.

#include "../Constants.h"

import SceneTypes;
import bindless;
import PerFrame.resource;
import Material;
import Fog;
import Water.WaterSurface;

struct WaterGuideVSOut
{
    [vk::location(0)] float3 posW                    : TEXCOORD0;
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] nointerpolation uint objectId  : TEXCOORD2;
    float4 posH                    : SV_POSITION;
    [vk::location(3)] float4 curClip                 : TEXCOORD3;
    [vk::location(4)] float4 prevClip                : TEXCOORD4;
};

struct WaterGuideOut
{
    float4 positionViewZ : SV_TARGET0;
    float4 normalWeight  : SV_TARGET1;
    float2 velocity      : SV_TARGET2;
};

// Comparisons against this bound reject NaN and Inf without special
// non-finite predicates, while also rejecting values too large for a finite MRT.
static const float kGuideFiniteLimit = 3.402823466e+38f;
static const float kGuideHalfLimit   = 65504.0f;   // RGBA16F / RG16F max
static const float kGuideMinLength2  = 1.0e-20f;
static const float kGuideClipWMin    = 1.0e-6f;

bool guideFiniteScalar(float v)
{
    return (v > -kGuideFiniteLimit) && (v < kGuideFiniteLimit);
}

bool guideFinite2(float2 v)
{
    return guideFiniteScalar(v.x) && guideFiniteScalar(v.y);
}

bool guideFinite3(float3 v)
{
    return guideFiniteScalar(v.x) && guideFiniteScalar(v.y)
        && guideFiniteScalar(v.z);
}

bool guideFinite4(float4 v)
{
    return guideFiniteScalar(v.x) && guideFiniteScalar(v.y)
        && guideFiniteScalar(v.z) && guideFiniteScalar(v.w);
}

bool guideHalfFiniteScalar(float v)
{
    return (v > -kGuideHalfLimit) && (v < kGuideHalfLimit);
}

bool guideHalfFinite2(float2 v)
{
    return guideHalfFiniteScalar(v.x) && guideHalfFiniteScalar(v.y);
}

bool guideHalfFinite3(float3 v)
{
    return guideHalfFiniteScalar(v.x) && guideHalfFiniteScalar(v.y)
        && guideHalfFiniteScalar(v.z);
}

bool guideLengthUsable(float3 v)
{
    float length2 = dot(v, v);
    return (length2 > kGuideMinLength2) && (length2 < kGuideFiniteLimit);
}

bool guideClipUsable(float4 clip)
{
    return guideFinite4(clip) && (abs(clip.w) > kGuideClipWMin);
}

[shader("fragment")]
WaterGuideOut psMain(WaterGuideVSOut psIn)
{
    float2 dBaseUvDx = ddx(psIn.uv);
    float2 dBaseUvDy = ddy(psIn.uv);
    float3 dpdx = ddx(psIn.posW);
    float3 dpdy = ddy(psIn.posW);

    WaterGuideOut o;
    o.positionViewZ = float4(0.0f);
    o.normalWeight  = float4(0.0f);
    o.velocity      = float2(0.0f);

    UniformObject obj = gSceneObjects[psIn.objectId];
    WaterMaterial water = loadWaterMaterial(obj.materialID);

    // Keep the eye normalization out of the valid path when its input is
    // degenerate or non-finite. The shared frame module still owns all surface
    // normal construction for valid fragments.
    // Category (a): this view vector follows the world position interpolated
    // at the jittered full-resolution raster sample. It is not a camera ray
    // reconstructed from a pixel index, so the raster sample already supplies
    // the required primary-sample alignment.
    float3 eyeDelta = psIn.posW - gPerFrame.posW;
    float eyeLength2 = dot(eyeDelta, eyeDelta);
    if (!guideFinite3(psIn.posW) || !guideFinite3(eyeDelta)
        || !(eyeLength2 > kGuideMinLength2)
        || !(eyeLength2 < kGuideFiniteLimit))
        return o;

    float3 eye  = normalize(psIn.posW - gPerFrame.posW);
    WaterSurfaceFrame frame = waterSurfaceFrame(dpdx, dpdy, eye);
    if (!frame.valid || !guideFinite3(frame.N) || !guideFinite3(frame.T)
        || !guideFinite3(frame.B) || !guideLengthUsable(frame.N)
        || !guideLengthUsable(frame.T) || !guideLengthUsable(frame.B))
        return o;

    float3 waveN = waterWaveNormalWorld(water, psIn.uv,
                                        dBaseUvDx, dBaseUvDy,
                                        gPerFrame.afT, frame, eye);
    if (!guideFinite3(waveN) || !guideLengthUsable(waveN))
        return o;

    float fresnel = waterFresnelTerm(water, eye, waveN);
    float viewZ = -mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z;
    float fade = waterDistanceFade(water, viewZ);
    FogResult fog = computeFog(psIn.posW, gPerFrame.posW);

    // Guard the reflection-normal normalize in the shared helper, then use
    // that helper's exact result so this guide matches Water.frag.slang.
    float3 reflectionNormalRaw = lerp(frame.N, waveN, kWaterReflectionTurbulence);
    if (!guideFinite3(reflectionNormalRaw)
        || !guideLengthUsable(reflectionNormalRaw))
        return o;
    float3 reflectionNormal = waterReflectionNormal(frame, waveN);

    float weight = fresnel * fade * fog.visibility;
    if (!guideFiniteScalar(fresnel) || !guideFiniteScalar(fade)
        || !guideFiniteScalar(fog.visibility)
        || !(weight > 0.0f) || !guideFiniteScalar(weight)
        || !guideFinite3(reflectionNormal)
        || !guideLengthUsable(reflectionNormal)
        || !guideHalfFinite3(reflectionNormal)
        || !guideHalfFiniteScalar(weight)
        || !(viewZ > 0.0f) || !guideFiniteScalar(viewZ))
        return o;

    // Category (b): motion/NRD UVs are interpolated from unjittered clip
    // positions and converted in the full render extent. NDC -> UV is y-down;
    // the half-resolution reducer must copy this full-image-normalized value,
    // not add jitter or double it.
    if (!guideClipUsable(psIn.curClip) || !guideClipUsable(psIn.prevClip))
        return o;
    float2 currentUV = (psIn.curClip.xy / psIn.curClip.w)
                     * float2(0.5f, -0.5f) + 0.5f;
    float2 previousUV = (psIn.prevClip.xy / psIn.prevClip.w)
                      * float2(0.5f, -0.5f) + 0.5f;
    float2 velocity = currentUV - previousUV;
    if (!guideFinite2(currentUV) || !guideFinite2(previousUV)
        || !guideFinite2(velocity) || !guideHalfFinite2(velocity))
        return o;

    // The guide stores a world position produced by raster interpolation, not
    // a position reconstructed from a pixel index; the later half-resolution
    // ray therefore consumes a buffered surface point rather than rebuilding a
    // camera ray with a jitter term.
    o.positionViewZ = float4(psIn.posW, viewZ);
    o.normalWeight  = float4(reflectionNormal, weight);
    o.velocity      = velocity;
    return o;
}
