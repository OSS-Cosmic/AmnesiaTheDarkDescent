/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Sorted water surfaces use two interleaved draws. The first tints the
// background (MUL), or samples and tints a per-surface scene copy when
// refractive (REPLACE). The second adds denoised ray-traced reflection and fog.
// Each scene copy includes the preceding surfaces, including their reflections.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Material;             // loadWaterMaterial
import bindless;
import PerFrame.resource;
import Fog;                  // computeFog — fade the water surface into world/area fog
import Water.WaterSurface;
import Water.WaterReflectionSample;
import Translucent.WaterCommon;
import LegacyEffectMath;
import Utils.Color.ColorHelpers;

[vk::binding(0, 2)] Texture2D<float4> gWaterReflectionSpecular;
[vk::binding(1, 2)] Texture2D<float4> gWaterReflectionGuidePosViewZ;
[vk::binding(2, 2)] Texture2D<float4> gWaterReflectionGuideNormalWeight;
[vk::binding(3, 2)] Texture2D<float4> sceneColorInput;
[vk::binding(4, 2)] Texture2D<float> sceneDepthInput;
[vk::binding(5, 2)] SamplerState sceneColorSampler;

struct WaterPC
{
    uint pass;   // 0 = tint (MUL), 1 = reflection (ADD), 2 = particle occlusion depth
    uint reflectionAvailable;
    uint refractive;
    uint _p2;
};
[vk::push_constant] WaterPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct WaterVSOut
{
    [vk::location(0)] float3 posW                    : TEXCOORD0;
    [vk::location(1)] float3 normalW                 : TEXCOORD1;
    [vk::location(2)] float3 tangentW                : TEXCOORD2;
    [vk::location(3)] float2 uv                      : TEXCOORD3;
    [vk::location(4)] nointerpolation uint objectId  : TEXCOORD4;
    float4 posH                    : SV_POSITION;
};

// The opaque scene blends fog in display space (applyFog). Water composites
// with fixed-function MUL/ADD, so reproduce that blend here: scaling a
// display-space colour by v is ~v^2.2 in linear, and the premultiplied fog
// colour is encoded to display before it is scaled by coverage.
float waterFogTransmission(float visibility)
{
    return pow(saturate(visibility), 2.2f);
}

float3 waterFogAdd(FogResult fog)
{
    float coverage = 1.0f - saturate(fog.visibility);
    if (coverage <= 0.0f)
        return float3(0.0f);
    float3 fogDisplay = linearToSRGB(max(fog.color / coverage, 0.0f)) * coverage;
    return sRGBToLinear(saturate(fogDisplay)) / kSceneExposure;
}

[shader("fragment")]
float4 psMain(WaterVSOut psIn) : SV_TARGET
{
    // Independent of reflection validity, Fresnel, fog and wave shading.
    // MIN blending records the nearest rasterized water surface per pixel.
    if (gPushConstants.pass == 2u)
        return float4(-mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z, 0, 0, 0);

    float2 dBaseUvDx = ddx(psIn.uv);
    float2 dBaseUvDy = ddy(psIn.uv);
    float3 dpdx = ddx(psIn.posW);
    float3 dpdy = ddy(psIn.posW);
    UniformObject obj   = gSceneObjects[psIn.objectId];
    WaterMaterial water = loadWaterMaterial(obj.materialID);

    // Transmission and reflection share visibility; the ADD pass contributes
    // fog once after the MUL/REPLACE pass attenuates the background.
    FogResult fog = computeFog(psIn.posW, gPerFrame.posW);
    float fogTransmission = waterFogTransmission(fog.visibility);

    // Shared by refraction and reflection; derivatives were captured above.
    float3 eye = normalize(psIn.posW - gPerFrame.posW);
    WaterSurfaceFrame frame = waterSurfaceFrame(dpdx, dpdy, dBaseUvDx, dBaseUvDy, eye);
    float3 tangentNormal = waterSurfaceNormalTangent(water, psIn.uv,
        dBaseUvDx, dBaseUvDy, gPerFrame.afT);
    float3 waveN = normalize(tangentNormal.x * frame.T + tangentNormal.y * frame.B
                             + tangentNormal.z * frame.N);
    if (dot(eye, waveN) > 0.0f) waveN = -waveN;
    float2 fallback = legacyWaterFallback(tangentNormal);

    // pass 1 — Fresnel-weighted denoised reflection (ADD).
    float fresnel = waterFresnelTerm(water, eye, waveN);

    // Distance fade: fade the reflection out with view depth. Only when a real
    // fade range is authored — without this guard a 0 reflectionFadeEnd makes
    // fadeLen ~1e-6 and saturate()→1, which zeroes the reflection everywhere.
    float viewZ = -mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z;
    fresnel *= waterDistanceFade(water, viewZ);

    // Reflection normal: calmer than refraction (blend wave normal toward flat).
    float3 reflN = waterReflectionNormal(frame, waveN);

    float3 sampledReflection = float3(0.0f);
    bool reflectionValid = false;
    // Both retail games use the same fallback when reflections are absent.
    float fakeSpecular = 0.0f;
    if (gPushConstants.reflectionAvailable == 0u)
        fakeSpecular = sRGBToLinear(float3(fallback.y)).x / kSceneExposure;
    if (gPushConstants.reflectionAvailable != 0u)
    {
        WaterReflectionSample s = sampleWaterReflectionHalfRes(
            gWaterReflectionSpecular,
            gWaterReflectionGuidePosViewZ,
            gWaterReflectionGuideNormalWeight,
            uint2(psIn.posH.xy),
            psIn.posW,
            frame.N,
            reflN);
        if (s.valid)
        {
            sampledReflection = s.radiance;
            reflectionValid = true;
        }
    }

    fresnel = waterReflectionWeight(fresnel, 1.0f, reflectionValid);

    if (gPushConstants.pass == 0u)
    {
        // Linear tint of the refracted background (MUL),
        // faded to zero as fog thickens.
        float3 tint = float3(1.f);
        uint   diffuseTex = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            tint = sampleBindless2D(diffuseTex, psIn.uv).rgb;
        float2 uv = psIn.posH.xy * gPerFrame.viewTexel;
        float surfaceViewZ = mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z;
        float thickness = 1.0f;
        if (gPushConstants.refractive != 0u)
            thickness = waterBackgroundThickness(uv,
                sceneDepthInput.SampleLevel(sceneColorSampler, uv, 0),
                surfaceViewZ, gPerFrame.invProjMat);
        float3 multiplier = waterDepthTransmission(tint, thickness)
                          * kWaterRefractionExposure * fogTransmission * (1.0f-fresnel);
        // Retail fallback is authored in display space; decode for HDR blending.
        if (gPushConstants.reflectionAvailable == 0u)
            multiplier *= sRGBToLinear(float3(fallback.x));
        if (gPushConstants.refractive != 0u)
        {
            float viewZ = surfaceViewZ;
            // Strengthen RT water refraction while retaining the authored scale
            // and shallow-water fade. Reject foreground/out-of-screen samples below.
            float aspect = gPerFrame.viewTexel.y / gPerFrame.viewTexel.x;
            float2 distorted = uv + legacyWaterDistortion(tangentNormal.xy,
                2.0f * water.refractionScale * waterShoreDistortion(thickness), viewZ, aspect);
            if (!all(isfinite(distorted)) || any(distorted < 0.0f) || any(distorted > 1.0f))
                distorted = uv;
            else
            {
                float sceneZ = sceneDepthInput.SampleLevel(sceneColorSampler, distorted, 0);
                if (!isfinite(sceneZ) || sceneZ < psIn.posH.z - 1.0e-3f)
                    distorted = uv;
            }
            // Re-evaluate absorption at the accepted sample, including fallback UV.
            thickness = waterBackgroundThickness(distorted,
                sceneDepthInput.SampleLevel(sceneColorSampler, distorted, 0),
                surfaceViewZ, gPerFrame.invProjMat);
            multiplier = waterDepthTransmission(tint, thickness)
                       * kWaterRefractionExposure * fogTransmission * (1.0f-fresnel);
            if (gPushConstants.reflectionAvailable == 0u)
                multiplier *= sRGBToLinear(float3(fallback.x));
            return float4(sceneColorInput.SampleLevel(sceneColorSampler, distorted, 0).rgb
                          * multiplier, 1.0f);
        }
        return float4(multiplier, 1.0f);
    }

    // Fade the reflection by fog visibility, then add the fog colour exactly
    // once (the MUL pass already zeroed the surface at full fog) so the water
    // composites to the authored fog color in the distance. Fog is blended in
    // display space (waterFogAdd/waterFogTransmission) to match opaque geometry.
    // Alpha 0: this blend is additive in alpha too (srcAlpha=ONE, dstAlpha=ONE),
    // so 0 is the identity that preserves the a = 1 the composite wrote.
    float3 reflection = sampledReflection * kWaterReflectionExposure * fresnel
                        + float3(fakeSpecular);
    return float4(reflection * fogTransmission + waterFogAdd(fog), 0.0f);
}
