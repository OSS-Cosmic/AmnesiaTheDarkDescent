#pragma once
// Shared diffuse-shading helpers, used by both the composite / direct stack
// (MainCompositePass.cs, DirectLightingPass.cs) and the path tracer
// (PathTracePass.rt.slang):
//   * DiffuseShading + prepareDiffuseShading — surface shading state from a hit.
//   * shadeAnalyticLight / *Soft / unshadowedAnalyticLight — the per-light
//     diffuse shade (hard / jittered-disk / no shadow ray). These were Scene
//     methods, but they're lighting-evaluation utilities (not scene-data
//     accessors), so they live here next to the eval/reservoir helpers that use
//     them; they compose Scene's data primitives (getLight().sample /
//     getSourceRadius / getPosition, materials.traceShadowRay).
// Previously copy-pasted into each consumer.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;          // VertexData, DiffuseMaterial, kInvalidTextureIndex, kDefaultDiffuseRoughness
import Scene;               // scene data primitives (+ ILight/LightSample via __exported)
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;            // gTextures2D, gMaterialSampler
import Brdf;                // decodeMaterialRoughness, evalDiffuseDemodulated
import PerFrame.resource;   // gPerFrame.point/spotLightCount
import Random;              // RNG for the soft-shadow disk sample
import Utils.Color.ColorHelpers; // linearToSRGB / sRGBToLinear (rtCubeMapFold)

public struct DiffuseShading
{
    public float3 posW;
    public float3 normalW;
    public float3 albedo;
    public float3 emission;
    public float  rough;    // perceptual diffuse-lobe roughness
    public float  surfaceFootprintW; // world-space footprint DIAMETER at the hit
};

// emission = illumination-map texel (DiffuseMaterial slot 5) * per-object glow
// scalar (UniformObject.illuminationAmount). Without this the composite pass
// ignores illumination entirely and glowing textures render dark.
//
// decodeRoughness: the path tracer decodes per-texel roughness
// (decodeMaterialRoughness); the composite/direct stack uses the flat
// kDefaultDiffuseRoughness (decode was disabled there). Defaulting to false
// preserves the composite-side behavior for its callers.
//
// applyNormalMap: perturb normalW with the material's tangent-space normal map
// (tex[1], gated by kMaterialFlagEnableNormal). Defaults to false so existing
// callers (composite / water) are unchanged; the ReSTIR direct pass
// opts in so its lighting picks up bump detail. World-space TBN built from the
// interpolated normal + tangent: T is Gram-Schmidt orthogonalized against N,
// B = cross(N, T) with the tangent handedness dropped, and the map is decoded
// as 2*sample-1.
public DiffuseShading prepareDiffuseShading(VertexData v, uint materialID,
                                            float illuminationAmount,
                                            float texLodBase,
                                            float surfaceFootprintW,
                                            bool decodeRoughness = false,
                                            bool applyNormalMap   = false)
{
    DiffuseShading sd;
    sd.posW    = v.posW;
    sd.normalW = normalize(v.normalW);
    sd.surfaceFootprintW = surfaceFootprintW;
    DiffuseMaterial m = loadDiffuseMaterial(materialID);

    if (applyNormalMap && (m.materialConfig & kMaterialFlagEnableNormal) != 0u &&
        m.tex[1] != kInvalidTextureIndex)
    {
        float3 N = sd.normalW;
        float3 T = v.tangentW - N * dot(N, v.tangentW);   // Gram-Schmidt orthogonalize
        if (dot(T, T) > 1e-8f)
        {
            T = normalize(T);
            float3 B = cross(N, T);                        // world-space TBN (tangent.w dropped)
            float3 mn = sampleBindless2DCone(m.tex[1], v.texC, texLodBase).xyz * 2.0f - 1.0f;
            sd.normalW = normalize(mn.x * T + mn.y * B + mn.z * N);
        }
    }

    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        sd.albedo = float3(0.5f);
    else
        sd.albedo = sampleBindless2DCone(diffuseTex, v.texC, texLodBase).rgb;

    uint illumTex = m.tex[5];
    if (illumTex == kInvalidTextureIndex || illuminationAmount <= 0.f)
        sd.emission = float3(0.f);
    else
        sd.emission = sampleBindless2DCone(illumTex, v.texC, texLodBase).rgb
                      * illuminationAmount;

    sd.rough = decodeRoughness ? decodeMaterialRoughness(m, v.texC, texLodBase)
                               : kDefaultDiffuseRoughness;
    return sd;
}

// Legacy solid_diffuse environment-map fold: the cube-map reflection (masked
// by CubeMapAlpha, weighted by the material fresnel) is added to the albedo in
// display space, so it is lit like the surface. World-space twin of
// standardCubeMapFold (Standard/StandardRaster.slang) -- keep the two in sync.
// Compute callers have no derivatives, so the cube is read at its top mip and
// the mask through the ray-cone LOD.
public float3 rtCubeMapFold(uint materialID, float3 albedoLinear, float3 posW,
                            float3 normalW, float2 uv, float texLodBase)
{
    // Translucent / water blobs place cubeMapTextureIndex differently.
    if (loadMaterialHeader(materialID).type != MATERIAL_TYPE_DIFFUSE)
        return albedoLinear;
    DiffuseMaterial m = loadDiffuseMaterial(materialID);
    if ((m.materialConfig & kMaterialFlagEnableCubeMap) == 0u ||
        m.cubeMapTextureIndex == kInvalidTextureIndex)
        return albedoLinear;
    float3 toSurface = posW - gPerFrame.posW;
    float eyeLengthSq = dot(toSurface, toSurface);
    float normalLengthSq = dot(normalW, normalW);
    if (!isfinite(eyeLengthSq) || eyeLengthSq <= 1.0e-12f ||
        !isfinite(normalLengthSq) || normalLengthSq <= 1.0e-12f)
        return albedoLinear;
    float3 eye = toSurface * rsqrt(eyeLengthSq);
    float3 normal = normalW * rsqrt(normalLengthSq);
    float4 reflection = gTexturesCube[NonUniformResourceIndex(m.cubeMapTextureIndex)]
                            .SampleLevel(gMaterialSampler, reflect(eye, normal), 0.0f);
    float facing = 1.0f - max(dot(-eye, normal), 0.0f);
    float fresnel = max(m.frenselBias + (1.0f - m.frenselBias) *
                        pow(abs(facing), m.frenselPow), 0.0f);
    if ((m.materialConfig & kMaterialFlagEnableCubeMapAlpha) != 0u &&
        m.tex[7] != kInvalidTextureIndex)
        reflection *= sampleBindless2DCone(m.tex[7], uv, texLodBase).a;
    return sRGBToLinear(saturate(linearToSRGB(max(albedoLinear, 0.0f)) +
                                 reflection.rgb * fresnel));
}

// --- Per-light analytic diffuse shading -----------------------------------
// Demodulated throughout: Li·NdotL·f_d/albedo (no albedo — the caller applies
// it). f_d is the standard diffuse lobe (kDiffuseBrdf, carries its 1/π). V is
// toward the camera for the composite/direct stack.

// Shared core: sample + back-face cull + demodulated contribution. Returns false
// (contrib unset) when the sample fails or N·L ≤ 0. The three public variants
// differ ONLY in the shadow ray.
bool sampleLightShade(uint lightIndex, float3 posW, float3 normalW,
                      float surfaceFootprintW, float3 V, float rough,
                      out LightSample ls, out float3 contrib)
{
    contrib = float3(0.f);
    if (!getLight(lightIndex).sample(posW, surfaceFootprintW, ls)) return false;
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.f) return false;
    contrib = ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    return true;
}

// Hard shadow: trace one ray at the light centre. `origin` is the
// (epsilon-offset) shadow-ray origin the caller computes.
public float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 V,
                                 float surfaceFootprintW, float rough,
                                 uint lightIndex, float3 origin)
{
    LightSample ls;
    float3 contrib;
    if (!sampleLightShade(lightIndex, posW, normalW, surfaceFootprintW,
                          V, rough, ls, contrib))
        return float3(0.f);
    // Default policy: every light is shadow-casting, so every light traces a
    // shadow ray. The authored flag only decides in the legacy opt-out mode.
    if ((gPerFrame.allLightsCastShadows != 0u || getLight(lightIndex).castsShadow()) &&
        !traceShadowRay(origin, ls.dir, ls.distance)) return float3(0.f);
    return contrib;
}

// Soft shadow: for a non-zero sourceRadius, aim the shadow ray at a jittered
// point on the light disk (facing the shading point), so one noisy sample per
// frame, accumulated temporally, resolves to a penumbra. Energy still uses the
// light centre — only occlusion is softened. sourceRadius == 0 falls back to the
// hard centre ray.
public float3 shadeAnalyticLightSoft(float3 posW, float3 normalW, float3 V,
                                     float surfaceFootprintW, float rough,
                                     uint lightIndex,
                                     float3 origin, inout RNG rng)
{
    LightSample ls;
    float3 contrib;
    if (!sampleLightShade(lightIndex, posW, normalW, surfaceFootprintW,
                          V, rough, ls, contrib))
        return float3(0.f);

    // Aim the shadow ray at a jittered point on the light's source via the ILight
    // interface — getLight() does the point/spot/area index slicing, so this
    // function no longer touches the typed SSBOs or knows the source shape. Spheres
    // (point/spot) return a disk point; area lights sample their rectangle; a
    // degenerate source returns the centre (hard ray).
    float3 target = getLight(lightIndex).sampleSourcePoint(posW, ls.dir, rng.next_float2());
    float3 toT  = target - origin;
    float  sMax = length(toT);
    float3 sDir = toT / max(sMax, 1.0e-6f);

    // Default policy: every light is shadow-casting, so every light traces a
    // shadow ray. The authored flag only decides in the legacy opt-out mode.
    if ((gPerFrame.allLightsCastShadows != 0u || getLight(lightIndex).castsShadow()) &&
        !traceShadowRay(origin, sDir, sMax)) return float3(0.f);
    return contrib;
}

// Undemodulated, BRDF-FREE light contribution: returns Li·NdotL plus the light
// direction `L`, so the caller can weight it with its own BSDF
// (`evalBrdf(s, V, L) * LiNdotL`). Unlike the three demodulated variants above
// — which bake in the diffuse lobe with albedo factored out — the path tracer
// evaluates the full Cook-Torrance GGX BSDF (diffuse + specular) at the hit and
// keeps albedo in, so it needs the raw radiance rather than a
// diffuse-only/albedo-less one. Same sample + NdotL cull + shadow-ray gate as
// shadeAnalyticLight, so the shadow policy stays in one place.
// Returns false (outs zeroed) on sample failure, back-face, or occlusion.
public bool shadeAnalyticLightRadiance(float3 posW, float3 normalW,
                                       float surfaceFootprintW, uint lightIndex,
                                       float3 origin, out float3 L,
                                       out float3 LiNdotL)
{
    L       = float3(0.f);
    LiNdotL = float3(0.f);

    LightSample ls;
    if (!getLight(lightIndex).sample(posW, surfaceFootprintW, ls)) return false;
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.f) return false;

    // Default policy: every light is shadow-casting, so every light traces a
    // shadow ray. The authored flag only decides in the legacy opt-out mode.
    if ((gPerFrame.allLightsCastShadows != 0u || getLight(lightIndex).castsShadow()) &&
        !traceShadowRay(origin, ls.dir, ls.distance)) return false;

    L       = ls.dir;
    LiNdotL = ls.Li * NdotL;
    return true;
}

// Unshadowed contribution (no ray). Used as the RIS target weight for stochastic
// light selection — keeping the lobe keeps the target proportional to the true
// integrand.
public float3 unshadowedAnalyticLight(float3 posW, float3 normalW, float3 V,
                                      float surfaceFootprintW, float rough,
                                      uint lightIndex)
{
    LightSample ls;
    float3 contrib;
    sampleLightShade(lightIndex, posW, normalW, surfaceFootprintW,
                     V, rough, ls, contrib);
    return contrib;   // contrib stays float3(0) on sample-fail / back-face
}
