#pragma once

// Shared vertex-stream and transform helpers for the Standard raster paths.
// Missing streams deliberately have the same safe defaults in every path.
#include "Constants.h"
import PerFrame.resource;
import bindless;
import Utils.Color.ColorHelpers;
import SceneTypes;

// Base game G-buffer colour: diffuse + cubemap reflection * fresnel, clipped by
// its 8-bit target (deferred_gbuffer_solid_frag.glsl). Reflection cubemaps
// load as UNORM display values, so the fold happens in display space.
public float3 standardCubeMapFold(DiffuseMaterial material, float3 albedoLinear,
                                  float3 viewPosition, float3 viewNormal, float2 uv)
{
    if ((material.materialConfig & kMaterialFlagEnableCubeMap) == 0u ||
        material.cubeMapTextureIndex == kInvalidTextureIndex)
        return albedoLinear;
    float eyeLengthSq = dot(viewPosition, viewPosition);
    float normalLengthSq = dot(viewNormal, viewNormal);
    if (!isfinite(eyeLengthSq) || eyeLengthSq <= 1.0e-12f ||
        !isfinite(normalLengthSq) || normalLengthSq <= 1.0e-12f)
        return albedoLinear;
    float3 eye = viewPosition * rsqrt(eyeLengthSq);
    float3 normal = viewNormal * rsqrt(normalLengthSq);
    float3 directionW = mul((float3x3)gPerFrame.invViewRotationMat, reflect(eye, normal));
    float4 reflection = gTexturesCube[NonUniformResourceIndex(material.cubeMapTextureIndex)]
                            .Sample(gMaterialSampler, directionW);
    float facing = 1.0f - max(dot(-eye, normal), 0.0f);
    float fresnel = max(material.frenselBias + (1.0f - material.frenselBias) *
                        pow(abs(facing), material.frenselPow), 0.0f);
    if ((material.materialConfig & kMaterialFlagEnableCubeMapAlpha) != 0u &&
        material.tex[7] != kInvalidTextureIndex)
        reflection *= sampleBindless2D(material.tex[7], uv).a;
    return sRGBToLinear(saturate(linearToSRGB(max(albedoLinear, 0.0f)) + reflection.rgb * fresnel));
}

public struct StandardRasterVertex
{
    public float3 position;
    public float3 normal;
    public float4 tangent;
    public float2 uv;
};

public uint standardRasterVertexIndex(UniformObject obj, uint vertexID)
{
    return obj.indexHandle != 0ull
        ? ((uint*)obj.indexHandle)[vertexID]
        : vertexID;
}

public StandardRasterVertex fetchStandardRasterVertex(UniformObject obj, uint vertexID)
{
    uint index = standardRasterVertexIndex(obj, vertexID);
    StandardRasterVertex v;
    v.position = obj.posHandle != 0ull
        ? ((float4*)obj.posHandle)[index].xyz
        : float3(0.0f);
    v.normal = obj.normalHandle != 0ull
        ? ((float3*)obj.normalHandle)[index]
        : float3(0.0f, 0.0f, 1.0f);
    v.tangent = obj.tangentHandle != 0ull
        ? ((float4*)obj.tangentHandle)[index]
        : float4(0.0f, 0.0f, 0.0f, 1.0f);
    v.uv = obj.uv0Handle != 0ull
        ? ((float3*)obj.uv0Handle)[index].xy
        : float2(0.0f);
    return v;
}

public float3 standardSafeNormalize(float3 value, float3 fallback)
{
    float l2 = dot(value, value);
    return isfinite(l2) && l2 > 1.0e-12f ? value * rsqrt(l2) : fallback;
}

public float3 standardOrthoTangent(float3 normal)
{
    float3 axis = abs(normal.z) < 0.999f
        ? float3(0, 0, 1) : float3(0, 1, 0);
    return standardSafeNormalize(cross(axis, normal), float3(1, 0, 0));
}

public float3 standardViewNormal(UniformObject obj, float3 normal)
{
    // invModelMat contains inverse rows; transpose is required for the
    // inverse-transpose normal transform. Keep this unnormalized until after
    // interpolation so raster and reconstructed normals use identical math.
    float3 worldNormal = mul(transpose((float3x3)obj.invModelMat), normal);
    return mul(gPerFrame.viewMat, float4(worldNormal, 0.0f)).xyz;
}

public void standardViewBasis(UniformObject obj, float3 normal, float4 tangent,
                              out float3 viewNormal, out float3 viewTangent,
                              out float3 viewBitangent)
{
    // Enhanced solid_diffuse.vert.fsl reference: transform N, T, and
    // cross(T,N) with the inverse-transpose normal matrix, then normalize
    // each result independently at the vertex. Do not Gram-Schmidt authored
    // tangent data; its orientation is part of the material contract.
    float3x3 normalMat = transpose((float3x3)obj.invModelMat);
    float3 localN = standardSafeNormalize(normal, float3(0, 0, 1));
    float3 localT = tangent.xyz;
    // The enhanced vertex input is float3: tangent.w is intentionally ignored.
    // A missing/invalid tangent gets one deterministic finite tangent, and B
    // is always derived from that same T and N before the inverse transpose.
    localT = standardSafeNormalize(localT, standardOrthoTangent(localN));
    float3 localB = cross(localT, localN);
    float3 fallbackN = float3(0, 0, 1);
    float3 n = standardSafeNormalize(mul(normalMat, localN), fallbackN);
    float3 t = standardSafeNormalize(mul(normalMat, localT), standardOrthoTangent(n));
    float3 b = standardSafeNormalize(mul(normalMat, localB), standardOrthoTangent(n));
    viewNormal = standardSafeNormalize(mul((float3x3)gPerFrame.viewMat, n), float3(0, 0, 1));
    viewTangent = standardSafeNormalize(mul((float3x3)gPerFrame.viewMat, t), standardOrthoTangent(viewNormal));
    viewBitangent = standardSafeNormalize(mul((float3x3)gPerFrame.viewMat, b), standardOrthoTangent(viewNormal));
}

public float2 standardTransformUV(UniformObject obj, float2 uv)
{
    return mul(obj.uvMat, float4(uv, 0.0f, 1.0f)).xy;
}

// Legacy solid_diffuse.frag.fsl cube-map reflection. The enhanced deferred
// renderer adds cube x Fresnel (x cube alpha) to the diffuse texel in display
// space and stores it in an RGBA8 target, so the sum clamps at 1 and is then
// lit like the rest of the albedo. The bindless cube view is not sRGB-decoded
// (the Standard translucent pass decodes it), while tex[0] arrives linear.
public float3 standardSolidCubeReflection(DiffuseMaterial material, float3 albedo,
                                          float3 positionW, float3 shadingNormalV,
                                          float2 uv)
{
    if ((material.materialConfig & kMaterialFlagEnableCubeMap) == 0u ||
        material.cubeMapTextureIndex == kInvalidTextureIndex)
        return albedo;
    float3 eyeV = standardSafeNormalize(mul(gPerFrame.viewMat, float4(positionW, 1.0f)).xyz,
                                        float3(0.0f, 0.0f, -1.0f));
    float3 dirW = mul((float3x3)gPerFrame.invViewRotationMat, reflect(eyeV, shadingNormalV));
    float4 reflection = gTexturesCube[NonUniformResourceIndex(material.cubeMapTextureIndex)]
                            .Sample(gMaterialSampler, dirW);
    if ((material.materialConfig & kMaterialFlagEnableCubeMapAlpha) != 0u &&
        material.tex[7] != kInvalidTextureIndex)
        reflection *= sampleBindless2D(material.tex[7], uv).a;
    float eDotN = max(dot(-eyeV, shadingNormalV), 0.0f);
    float fresnel = max(material.frenselBias + (1.0f - material.frenselBias) *
                        pow(max(abs(1.0f - eDotN), 1.0e-6f), material.frenselPow), 0.0f);
    return sRGBToLinear(saturate(linearToSRGB(albedo) + reflection.rgb * fresnel));
}

// Legacy enhanced deferred parallax (deferred_common.h.fsl ParallaxAdvance:
// 32-step search plus contact refinement, heightMapScale x
// PARALLAX_MULTIPLIER 0.7). Inputs are view space, like the legacy
// solid_diffuse vertex outputs. The legacy search loops are unbounded; these
// stop at the step count so a degenerate height field or basis can never hang
// the device. Gradients come from the unshifted UV so sampling inside the
// loops stays well defined.
static const float kStandardParallaxMultiplier = 0.7f;
static const uint kStandardParallaxSteps = 32u;

float standardParallaxDepth(DiffuseMaterial material, float2 uv, float2 dUVdx, float2 dUVdy)
{
    float4 s = sampleBindless2DGrad(material.tex[4], uv, dUVdx, dUVdy);
    return (material.materialConfig & kMaterialFlagIsHeightMapSingleChannel) != 0u ? s.r : s.a;
}

public float2 standardParallaxUV(DiffuseMaterial material, float2 uv, float3 positionV,
                                 float3 normalV, float3 tangentV, float3 bitangentV)
{
    if ((material.materialConfig & kMaterialFlagEnableHeight) == 0u ||
        material.tex[4] == kInvalidTextureIndex)
        return uv;
    float heightMapScale = material.heightMapScale * kStandardParallaxMultiplier;
    // Legacy GetNormalizedDepth yields 0 for a collapsed depth range.
    if (!(heightMapScale > 0.0001f))
        return uv;
    float3 dirToCameraTS = float3(dot(tangentV, positionV), dot(bitangentV, positionV),
                                  dot(normalV, positionV));
    float lengthTS = length(dirToCameraTS);
    if (!(lengthTS > 1.0e-6f))
        return uv;
    dirToCameraTS /= lengthTS;
    // Legacy divides by z unguarded; grazing views have no meaningful march.
    if (!(abs(dirToCameraTS.z) > 1.0e-3f))
        return uv;

    float2 dUVdx = ddx(uv);
    float2 dUVdy = ddy(uv);
    float step = 1.0f / float(kStandardParallaxSteps);
    float3 delta = -dirToCameraTS * heightMapScale * (1.0f / dirToCameraTS.z) * step;
    float3 parallaxOffset = float3(0.0f, 0.0f, 0.0f);
    float currentStep = 0.0f;
    float currentSample = standardParallaxDepth(material, uv, dUVdx, dUVdy);
    float prevSample = currentSample;
    [loop]
    for (uint i = 0u; i <= kStandardParallaxSteps && currentSample > currentStep; ++i)
    {
        currentStep += step;
        parallaxOffset += delta;
        prevSample = currentSample;
        currentSample = max(standardParallaxDepth(material, uv + parallaxOffset.xy,
                                                  dUVdx, dUVdy), 0.0f);
    }
    if (currentStep > 0.0f)
    {
        // Contact refinement: roll back one step and re-march it at step^2.
        parallaxOffset -= delta;
        currentStep -= step;
        currentSample = prevSample;
        float3 adjustedDelta = delta * step;
        float adjustedStep = step * step;
        [loop]
        for (uint i = 0u; i <= kStandardParallaxSteps && currentSample > currentStep; ++i)
        {
            currentStep += adjustedStep;
            parallaxOffset += adjustedDelta;
            currentSample = max(standardParallaxDepth(material, uv + parallaxOffset.xy,
                                                      dUVdx, dUVdy), 0.0f);
        }
    }
    if (parallaxOffset.z > 0.0f)
        return uv;
    return uv + parallaxOffset.xy;
}
