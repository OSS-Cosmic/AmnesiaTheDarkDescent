/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/translucency.frag.fsl —
// the non-particle (mesh) translucent path matching MaterialID::Translucent
// in RendererDeferred.cpp:4606-4671.
//
// Scope: diffuse × vertex color × tint, fog, per-blend-mode pre-bias,
// per-object affecting-light level, static cube-map Fresnel reflection with
// rim term, and the UseIlluminationTrans second-draw flow. Hardware blend
// state is selected on the C++ side (one pipeline per blend mode) and the
// shader applies the matching per-pixel color-space transform so the
// output going into the blender is in the right space.
//
// Aligned with the reference translucency.frag.fsl — the one deliberate
// deviation is refraction, which is NOT implemented in this port. The legacy
// bend-behind distortion (screen-color copy + per-fragment UV offset) was
// dropped, and the RT V-buffer that was meant to supply the refracted
// background by bending the primary ray was retired without ever being wired
// up. So the background behind a refractive pixel is unrefracted and this
// pass simply alpha-blends over it — no screen-color sample, no refraction
// block here.
// Reflection stays the material's static cube map, exactly as the reference.


#include "Constants.h"
import bindless;
import SceneTypes;
import PerFrame.resource;
import Fog;
import Scene;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
// Linear HDR source preparation and gamma-independent opacity.
import BlendModes;
import Utils.Color.ColorHelpers;
import DiffuseShading;

// Options bitfield mirroring TranslucencyFlags in RendererDeferred.cpp.
// blendMode stays as a separate uint so the existing switch is cheap; this
// field carries the *behaviour* flags the reference packs into the same
// root constant.
#define TRANS_OPT_USE_ILLUMINATION (1u << 0)
#define TRANS_OPT_HAS_TLAS (1u << 1)

struct TranslucentPushConstants
{
    uint  blendMode;
    float sceneAlpha;
    uint  options;
    uint  _pad;
};

[vk::push_constant]
TranslucentPushConstants gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct TranslucentVSOut
{
    [vk::location(0)] float3 viewPos                 : TEXCOORD0;
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] float3 normal                  : TEXCOORD2;
    [vk::location(3)] float3 tangent                 : TEXCOORD3;
    [vk::location(4)] float3 bitangent               : TEXCOORD4;
    [vk::location(5)] float4 color                   : TEXCOORD5;
    [vk::location(6)] nointerpolation uint objectId  : TEXCOORD6;
    float4 posH                    : SV_POSITION;
};

// Schlick-Fresnel — mirrors deferred_common.h.fsl::Fresnel(eDotN, bias, pow).
// The base is clamped to a tiny positive epsilon: eDotN of two normalize()d
// vectors can exceed 1.0 by a ULP at dead-on view angles, and pow() of a
// negative base is NaN (exp2(y·log2(x))) — with MUL blending that poisons the
// framebuffer pixel permanently (saturate doesn't help: FClamp(NaN) is
// undefined in Vulkan). Hit by amnesia_bottle.mat (FrenselPow=0.5).
float computeFresnel(float afEDotN, float afFresnelBias, float afFresnelPow)
{
    float fFacing = max(1.0f - afEDotN, 1.0e-6f);
    return saturate(afFresnelBias + (1.0f - afFresnelBias) * pow(fFacing, afFresnelPow));
}

// Malformed or missing tangent data must not reach lighting or shadow rays.
float3 translucentSafeNormal(float3 value, float3 fallback)
{
    float len2 = dot(value, value);
    return len2 > 1.0e-12f && len2 < 1.0e30f ? value * rsqrt(len2) : fallback;
}

float3 translucentDiffuseLighting(TranslucentVSOut psIn, TranslucentMaterial mat,
                                  float3 posW, float footprintW)
{
    if ((gPushConstants.options & TRANS_OPT_HAS_TLAS) == 0u)
        return float3(0.0f);

    float3 N = translucentSafeNormal(psIn.normal, float3(0, 0, 1));
    if ((mat.materialConfig & kMaterialFlagEnableNormal) != 0u &&
        mat.tex[1] != kInvalidTextureIndex)
    {
        float3 T = translucentSafeNormal(psIn.tangent - N * dot(N, psIn.tangent), float3(0));
        float3 B = translucentSafeNormal(psIn.bitangent - N * dot(N, psIn.bitangent), float3(0));
        if (dot(T, T) > 0.0f && dot(B, B) > 0.0f)
        {
            float3 mapN = sampleBindless2D(mat.tex[1], psIn.uv).xyz * 2.0f - 1.0f;
            N = translucentSafeNormal(mapN.x * T + mapN.y * B + mapN.z * N, N);
        }
    }
    N = translucentSafeNormal(mul(float3x3(gPerFrame.invViewMat), N), float3(0, 1, 0));
    float3 V = translucentSafeNormal(gPerFrame.posW - posW, N);
    // Match kShadeRayEps in the shared direct-lighting walker.
    float3 origin = posW + N * 1.0e-3f;
    CellLights cell = getCellLights(posW);
    float3 irradiance = float3(0.0f);
    for (uint k = 0u; k < cell.count; ++k)
        irradiance += shadeAnalyticLight(posW, N, V, footprintW,
            kDefaultDiffuseRoughness, cellLightAt(cell, k), origin);
    return irradiance;
}

[shader("fragment")]
float4 psMain(TranslucentVSOut psIn) : SV_TARGET
{
    UniformObject       obj = gSceneObjects[psIn.objectId];
    TranslucentMaterial mat = loadTranslucentMaterial(obj.materialID);
    bool litDiffuse = (mat.materialConfig & kMaterialFlagLitDiffuse) != 0u &&
        (gPushConstants.blendMode == kBlendModeAlpha || gPushConstants.blendMode == kBlendModePremulAlpha);

    // Reference mask: when the second cube-map-only draw runs, the diffuse /
    // illumination / etc. sample paths are switched off so only the
    // reflection contribution is added on top of the main draw.
    uint textureConfig = mat.materialConfig;
    if ((gPushConstants.options & TRANS_OPT_USE_ILLUMINATION) != 0u)
    {
        textureConfig &= (kMaterialFlagEnableNormal
                        | kMaterialFlagEnableCubeMap
                        | kMaterialFlagEnableCubeMapAlpha);
    }

    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((textureConfig & kMaterialFlagEnableDiffuse) != 0u)
    {
        uint diffuseTex = mat.tex[0];
        if (gPushConstants.blendMode == kBlendModeAdd &&
            (mat.materialConfig & kMaterialFlagSmoothHalo) != 0u)
            diffuseColor = smoothHaloLinearSample(psIn.uv, psIn.color);
        else if (diffuseTex != kInvalidTextureIndex)
            // Legacy grayscale halos are authored RGB color, not linear masks.
            // Keep their hardware sRGB decode: re-encoding here lifts the dim
            // falloff and magnifies BC1 steps before additive HDR composition.
            diffuseColor = translucentLinearSample(
                sampleBindless2D(diffuseTex, psIn.uv), psIn.color,
                gPushConstants.blendMode,
                (mat.materialConfig & kMaterialFlagDiffuseIsMask) != 0u);
    }

    // Per-pixel fog (world + per-area). The shared Fog helper returns a
    // visibility scalar — translucents multiply it into the per-pixel alpha
    // so glass/decals fade behind fog without painting the fog colour.
    // Category (a): this world position comes from the view-space position
    // interpolated at the jittered primary raster sample, not from a pixel
    // index; preserving that interpolant keeps the fragment aligned with the
    // sample that produced its material UVs.
    float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
    // Derivatives precede divergent per-light/shadow control flow.
    float footprintW = max(length(ddx(surfacePosW)), length(ddy(surfacePosW)));
    if (litDiffuse)
        diffuseColor.rgb *= translucentDiffuseLighting(psIn, mat, surfacePosW, footprintW)
                          * max(mat.litDiffuseScale, 0.0f);
    FogResult fog = computeFog(surfacePosW, gPerFrame.posW);
    float finalAlpha = gPushConstants.sceneAlpha * fog.visibility;

    // AffectedByLightLevel materials dim with the surrounding analytic light
    // (base-game afLightLevel behavior — see lightLevelAt). Per-pixel,
    // gated on the raw material config (not the illumination-draw-masked
    // textureConfig) so the cube-map second draw dims its reflection too,
    // matching the legacy shader's afLightLevel use on the reflection terms.
    float lightLevel = 1.0f;
    if (!litDiffuse && (mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
    {
        lightLevel = lightLevelAt(surfacePosW);
    }

    float4 finalColor = diffuseColor;

    // Fresnel reflection. Gated on the material carrying a reflection cube map
    // (the "reflective" signal — same gate the legacy used), but the
    // reflection *source* is now the live ray-traced reflection buffer
    // (gReflectionResolve) instead of the static cube sample. The Fresnel /
    // rim weighting is unchanged.
    bool hasCubeMap = (textureConfig & kMaterialFlagEnableCubeMap) != 0u
                      && mat.cubeMapTextureIndex != kInvalidTextureIndex;
    if (hasCubeMap)
    {
        // Reconstruct view-space surface normal. If a normal map is bound,
        // sample it and rotate into view space via the tangent frame the VS
        // built; otherwise use the interpolated normal directly.
        float3 screenNormal;
        if ((textureConfig & kMaterialFlagEnableNormal) != 0u)
        {
            uint normalTex = mat.tex[1];
            if (normalTex != kInvalidTextureIndex)
            {
                float3 mapNormal = sampleBindless2D(normalTex, psIn.uv).xyz
                                   * 2.0f - 1.0f;
                screenNormal = normalize(mapNormal.x * psIn.tangent
                                       + mapNormal.y * psIn.bitangent
                                       + mapNormal.z * psIn.normal);
            }
            else
            {
                screenNormal = normalize(psIn.normal);
            }
        }
        else
        {
            screenNormal = normalize(psIn.normal);
        }

        // Category (a): the view vector is derived from the interpolated
        // primary-raster view position, not a reconstructed pixel-index ray;
        // no explicit jitter term or unjittered projection belongs here.
        float3 eyeVec   = normalize(psIn.viewPos);
        float  edotN    = max(dot(-eyeVec, screenNormal), 0.0f);
        float  fresnel  = computeFresnel(edotN, mat.frenselBias, mat.frenselPow);

        // reflect() takes the incoming vector — in view space the eye points
        // from camera (origin) toward fragment, which matches eyeVec.
        // Transform back to world via invViewRotationMat to sample the cube
        // (which lives in world space). Matches the reference
        // translucency.frag.fsl cube-map reflection.
        float3 envDirView  = reflect(eyeVec, screenNormal);
        float3 envDirWorld = mul(gPerFrame.invViewRotationMat,
                                 float4(envDirView, 1.0f)).xyz;
        // Cube maps use UNORM views, so decode their color once before
        // adding it to the linear diffuse source.
        float4 reflectionColor =
            gTexturesCube[NonUniformResourceIndex(mat.cubeMapTextureIndex)]
                .Sample(gMaterialSampler, envDirWorld);
        reflectionColor.rgb = sRGBToLinear(reflectionColor.rgb);

        // Optional 2D alpha mask that fades the reflection contribution by UV.
        if ((textureConfig & kMaterialFlagEnableCubeMapAlpha) != 0u)
        {
            uint cubeAlphaTex = mat.tex[7];
            if (cubeAlphaTex != kInvalidTextureIndex)
            {
                float fEnvMapAlpha =
                    sampleBindless2D(cubeAlphaTex, psIn.uv).w;
                reflectionColor *= fEnvMapAlpha;
            }
        }

        // Premultiplied materials require every source contribution to be
        // associated with coverage before ONE / ONE_MINUS_SRC_ALPHA blending.
        if (gPushConstants.blendMode == kBlendModePremulAlpha)
            reflectionColor.rgb *= finalColor.a;

        finalColor.xyz += reflectionColor.xyz
                        * fresnel;

        // Rim reflection term — view-space normal vs -Z. Base clamped positive
        // for the same ULP reason as computeFresnel (abs(dot) can exceed 1).
        if (mat.rimLightMul > 0.0f)
        {
            float fRim = dot(screenNormal, float3(0.0f, 0.0f, -1.0f));
            fRim = pow(max(1.0f - abs(fRim), 1.0e-6f), mat.rimLightPow) * mat.rimLightMul;
            finalColor.xyz += reflectionColor.xyz
                            * fRim;
        }
    }

    // Lit surfaces retain authored coverage; preserve existing unlit tuning.
    // Opacity modulation keeps additive fades and multiplicative neutrals.
    float translucentStrength = litDiffuse ? 1.0f : 0.5f;
    finalColor = applyBlendModulation(gPushConstants.blendMode, finalColor,
                                     finalAlpha * translucentStrength, lightLevel);
    if (finalColor.a <= 0.0f)
        discard;
    return encodeBlendOutputLinear(gPushConstants.blendMode, finalColor);
}
