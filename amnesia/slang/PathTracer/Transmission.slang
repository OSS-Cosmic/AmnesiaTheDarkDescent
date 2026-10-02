#pragma once

// Specular transmission for path-traced hits on refractive translucents (glass)
// and water. These surfaces sit in the TLAS as kRayMaskTranslucent; their
// MaterialDataBlob is a TranslucentMaterial / WaterMaterial, so they must never
// reach loadDiffuseMaterial. A hit here is a delta interface: the path either
// reflects or refracts (Fresnel-picked, so the weight is just the tint) and
// continues without NEE or a BSDF evaluation.
//
// The IOR comes from the authored refractionScale, a screen-space distortion
// magnitude in the raster renderers. Glass uses
// ior = 1 + kGlassRefractionIorBoost * refractionScale: at the raw 1 + scale
// (~1.1) the entry and exit bends of a thin pane cancel and nothing visibly
// distorts. Water keeps ior = 1 + refractionScale with eta scaled by
// kWaterRefractionIntensity, as the deleted RT V-buffer (f3ac831e^
// VBuffer/SurfelVBuffer.rt.slang) did; it is an open sheet with no cancelling
// second bend.
//
// Open surfaces (a single-quad window pane) have no exit face; the ray stays
// bent by the entry refraction.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import MaterialRaster;       // loadMaterialHeader / loadTranslucentMaterial / loadWaterMaterial
import bindless;             // sampleBindless2DCone
import BlendModes;           // translucentLinearSample
import PerFrame.resource;    // gPerFrame.afT (wave animation time)
import Random;
import Translucent.WaterCommon;

public bool isTransmissiveMaterial(uint materialID)
{
    uint type = loadMaterialHeader(materialID).type;
    return type == MATERIAL_TYPE_TRANSLUCENT || type == MATERIAL_TYPE_WATER;
}

public struct TransmissionSample
{
    public float3 dir;
    public float3 tint;       // throughput multiplier for the continuation
    public bool   refracted;  // false: Fresnel reflection or total internal reflection
}

// Transmittance of a translucent layer, following how the raster pass composites
// it over the background (the layer's own radiance is not added).
float3 translucentTransmittance(TranslucentMaterial m, float2 uv, float texLodBase)
{
    uint rawBlend = (m.materialConfig >> kMaterialBlendModeShift) & kMaterialBlendModeMask;
    if (rawBlend == kMaterialBlendModeNone || m.tex[0] == kInvalidTextureIndex)
        return float3(1.0f);

    // Raw eMaterialBlendMode is the push-constant kBlendMode* scheme offset by one.
    uint blendMode = rawBlend - 1u;
    float4 texel = sampleBindless2DCone(m.tex[0], uv, texLodBase);
    float4 c = translucentLinearSample(texel, float4(1.0f), blendMode,
                                       (m.materialConfig & kMaterialFlagDiffuseIsMask) != 0u);
    switch (blendMode)
    {
        case kBlendModeMul:         return saturate(c.rgb);
        case kBlendModeMulX2:       return saturate(2.0f * c.rgb);
        case kBlendModeAlpha:
        case kBlendModePremulAlpha: return float3(saturate(1.0f - c.a));
        default:                    return float3(1.0f);   // Add: background passes untouched
    }
}

float schlickDielectric(float cosI, float eta)
{
    float r0 = (1.0f - eta) / (1.0f + eta);
    r0 *= r0;
    float m = 1.0f - saturate(cosI);
    return r0 + (1.0f - r0) * m * m * m * m * m;
}

// One glass / water crossing, resolved deterministically: the shading normal
// oriented against the ray, the relative IOR for refract(), and the layer tint.
public struct TransmissionInterface
{
    public float3 N;          // faces the incoming ray
    public float  eta;        // n_incident / n_transmitted, for refract()
    public float  ior;        // material IOR (Fresnel F0)
    public float3 tint;       // transmittance of the layer
}

// `incident` points toward the surface. `texLodBase` is the ray-cone LOD base
// for this hit (RayCone.slang).
public TransmissionInterface resolveTransmissionInterface(VertexData v, uint materialID,
                                                          float3 incident, float texLodBase)
{
    TransmissionInterface ti;
    ti.tint = float3(1.0f);

    float3 N = normalize(v.normalW);
    float3 T = v.tangentW - N * dot(N, v.tangentW);
    bool   hasTbn = dot(T, T) > 1e-8f;
    T = hasTbn ? normalize(T) : T;
    float3 B = cross(N, T);

    MaterialHeader hdr = loadMaterialHeader(materialID);
    float refractionScale = 0.0f;
    float etaScale = 1.0f;
    if (hdr.type == MATERIAL_TYPE_WATER)
    {
        WaterMaterial w = loadWaterMaterial(materialID);
        refractionScale = w.refractionScale;
        etaScale = kWaterRefractionIntensity;
        if (hasTbn)
        {
            // No screen derivatives outside a pixel shader: zero gradients
            // sample the top mip. The wave map is low frequency, so the cone
            // footprint is not threaded through here.
            float3 nT = waterWaveNormalTangent(w, v.texC, float2(0.0f), float2(0.0f),
                                               gPerFrame.afT);
            N = normalize(nT.x * T + nT.y * B + nT.z * N);
        }
    }
    else
    {
        TranslucentMaterial m = loadTranslucentMaterial(materialID);
        refractionScale = kGlassRefractionIorBoost * m.refractionScale;
        // Legacy semantics (translucency.frag, Standard.translucent.3d):
        // RefractionNormals bends by the mesh normal; otherwise the normal map
        // is used when the material has one.
        if (hasTbn && (m.materialConfig & kMaterialFlagUseRefractionNormals) == 0u &&
            (m.materialConfig & kMaterialFlagEnableNormal) != 0u &&
            m.tex[1] != kInvalidTextureIndex)
        {
            float3 mn = sampleBindless2DCone(m.tex[1], v.texC, texLodBase).xyz * 2.0f - 1.0f;
            N = normalize(mn.x * T + mn.y * B + mn.z * N);
        }
        ti.tint = translucentTransmittance(m, v.texC, texLodBase);
    }

    ti.ior = 1.0f + max(refractionScale, 0.0f);
    // Water is an open sheet, not a closed volume: treat every crossing as
    // entering (as the old V-buffer did) so rays from below don't spuriously
    // total-internal-reflect.
    // Side from the geometric normal: a normal-mapped N can flip it per texel.
    bool backSide = dot(incident, v.normalW) > 0.0f;
    bool exiting  = hdr.type != MATERIAL_TYPE_WATER && backSide;
    // A map normal tilted past the incident ray would break refract(); fall
    // back to the geometric normal for that texel.
    if ((dot(incident, N) > 0.0f) != backSide)
        N = normalize(v.normalW);
    ti.N   = backSide ? -N : N;
    ti.eta = (exiting ? ti.ior : 1.0f / ti.ior) * etaScale;
    return ti;
}

// refract() through the interface; total internal reflection falls back to the
// mirror direction and reports refracted = false.
public float3 refractThrough(TransmissionInterface ti, float3 incident, out bool refracted)
{
    float3 refr = refract(incident, ti.N, ti.eta);
    refracted = dot(refr, refr) >= 1.0e-6f;
    return refracted ? normalize(refr) : reflect(incident, ti.N);
}

public TransmissionSample sampleTransmission(VertexData v, uint materialID,
                                             float3 incident, float texLodBase,
                                             inout RNG rng)
{
    TransmissionInterface ti = resolveTransmissionInterface(v, materialID, incident, texLodBase);

    TransmissionSample ts;
    bool refracted;
    float3 dir = refractThrough(ti, incident, refracted);
    float F = refracted ? schlickDielectric(-dot(incident, ti.N), ti.ior) : 1.0f;

    if (rng.next_float() < F)
    {
        ts.dir = reflect(incident, ti.N);
        ts.refracted = false;
        // The layer tint models absorption through the pane, not the reflection.
        ts.tint = float3(1.0f);
    }
    else
    {
        ts.dir = dir;
        ts.refracted = true;
        ts.tint = ti.tint;
    }
    return ts;
}
