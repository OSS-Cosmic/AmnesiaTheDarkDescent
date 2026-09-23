#pragma once

// Engine-owned bindless descriptor set (set 0). Slang mirror of the GLSL
// declarations in amnesia/glsl/bindless.resource.glsl — every SSBO / texture
// the host binds at set 0 is declared here, indexed by the BINDING_* macros
// from Constants.h (single source of truth shared with C++ and GLSL).
//
// Usage:
//   import bindless;
//   ...
//   PointLight p = gPointLights[i];
//
// Bindless texture arrays require descriptor indexing
// (SPV_EXT_descriptor_indexing + VK_EXT_descriptor_indexing); slangc emits
// the capability automatically when the array type is bound by `[]`.

#include "Constants.h"
import SceneTypes;
import PerFrame.resource;   // gPerFrame.afT for animated-texture frame selection

// GeometryStream imports this module for geometry pull loads.  Keep the
// reflected unbounded range here so bindless owns the descriptor boundary.
// DXIL requires an unbounded range to be last in its register space. Vulkan's
// geometry path remains BDA-only and must not reflect or bind this table.
#if defined(DXIL)
public ByteAddressBuffer gGeometryBuffers[] : register(t0, space36);
#endif

// -----------------------------------------------------------------------------
// Bindless texture pools (LRU-allocated by cHybridRenderer's bindless
// allocator). Combine with `gMaterialSampler` at sample sites via
// `Sampler2D(gTextures2D[i], gMaterialSampler)` (or HLSL-style
// `gTextures2D[i].Sample(gMaterialSampler, uv)`).
// -----------------------------------------------------------------------------
[vk::binding(kBindingTextures2D, 0)]
public Texture2D gTextures2D[] : register(t0, space32);

[vk::binding(kBindingTexturesCube, 0)]
public TextureCube gTexturesCube[] : register(t0, space33);

// Animated images: one Texture2DArray per animated Image (N frames = N layers).
// A texture index with kAnimatedTextureBit set selects this array; the bindless
// sample helpers below strip the bit and pick the current layer from gAnimTex +
// gPerFrame.afT. cTextureManager writes the descriptor once (no per-frame rewrite).
[vk::binding(kBindingTextures2DArray, 0)]
public Texture2DArray gTextures2DArray[] : register(t0, space34);

[vk::binding(kBindingAnimTex, 0)]
public StructuredBuffer<AnimTexRec> gAnimTex : register(t0, space35);

[vk::binding(kBindingMaterialSampler, 0)]
public SamplerState gMaterialSampler : register(s0, space41);

// Current animation layer from the accumulated animation-time `afT` (seconds).
public uint animFrame(float afT, AnimTexRec r)
{
    if (r.frameCount <= 1u) return 0u;
    uint t = (uint)max(afT / max(r.frameTime, 1e-6f), 0.0f);
    if (r.mode == kAnimModeOscillate)
    {
        uint period = 2u * (r.frameCount - 1u);     // 0..N-1..1
        uint p = t % period;
        return p < r.frameCount ? p : period - p;
    }
    return t % r.frameCount;                          // kAnimModeLoop (default)
}

// Sample a bindless 2D texture index. Static slot → gTextures2D; an index with
// kAnimatedTextureBit set → the matching gTextures2DArray layer for this frame.
// `idx == kInvalidTextureIndex` returns opaque black (callers usually guard first;
// the invalid check must precede the bit test since 0xffffffff also has bit 31).
public float4 sampleBindless2DLevel(uint idx, float2 uv, float lod)
{
    if (idx == kInvalidTextureIndex) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((idx & kAnimatedTextureBit) != 0u)
    {
        uint a = idx & ~kAnimatedTextureBit;
        float frame = (float)animFrame(gPerFrame.afT, gAnimTex[NonUniformResourceIndex(a)]);
        return gTextures2DArray[NonUniformResourceIndex(a)].SampleLevel(gMaterialSampler, float3(uv, frame), lod);
    }
    return gTextures2D[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, lod);
}

// Explicit-LOD twin of sampleBindless2D's SampleBias.  Ray-traced primary
// shading has no fragment derivatives, so it supplies a texture-size-
// independent ray-cone LOD base; resolve the concrete texture's dimensions
// here before sampling.  This is where the reduced-render-extent material
// bias reaches the ray-traced primary shading path, rather than only raster
// fragment passes.
//
// Negative resolved LOD values are intentionally passed through: hardware
// clamps them to the top mip, which is the correct behavior for a negative
// materialMipBias at the top mip.
public float4 sampleBindless2DCone(uint idx, float2 uv, float lodBase)
{
    if (idx == kInvalidTextureIndex) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((idx & kAnimatedTextureBit) != 0u)
    {
        uint a = idx & ~kAnimatedTextureBit;
        float frame = (float)animFrame(gPerFrame.afT, gAnimTex[NonUniformResourceIndex(a)]);
        uint w;
        uint h;
        uint elements;
        gTextures2DArray[NonUniformResourceIndex(a)].GetDimensions(w, h, elements);
        float lod = lodBase
                  + 0.5f * log2(max(float(w) * float(h), 1.0f))
                  + gPerFrame.materialMipBias;
        return gTextures2DArray[NonUniformResourceIndex(a)].SampleLevel(gMaterialSampler, float3(uv, frame), lod);
    }
    uint w;
    uint h;
    gTextures2D[NonUniformResourceIndex(idx)].GetDimensions(w, h);
    float lod = lodBase
              + 0.5f * log2(max(float(w) * float(h), 1.0f))
              + gPerFrame.materialMipBias;
    return gTextures2D[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, lod);
}

// Explicit-LOD cube-map sampler for ray-traced projected gobos. Cube textures
// have no animated variant, so the texture index is always resolved directly
// from gTexturesCube. Resolve the cube face dimensions here so the supplied
// texture-size-independent LOD base can be completed before sampling.
//
// Negative resolved LOD values are intentionally passed through: hardware
// clamps them to the top mip, which is the correct behavior for a negative
// materialMipBias at the top mip.
public float4 sampleBindlessCubeCone(uint idx, float3 dir, float lodBase)
{
    if (idx == kInvalidTextureIndex) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint w;
    uint h;
    gTexturesCube[NonUniformResourceIndex(idx)].GetDimensions(w, h);
    float lod = lodBase
              + 0.5f * log2(max(float(w) * float(h), 1.0f))
              + gPerFrame.materialMipBias;
    return gTexturesCube[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, dir, lod);
}

// Auto-mip variant for fragment passes (gradient-based LOD), applying
// gPerFrame.materialMipBias, the temporal-upscaling material bias; zero on
// native-extent frames.
public float4 sampleBindless2D(uint idx, float2 uv)
{
    if (idx == kInvalidTextureIndex) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((idx & kAnimatedTextureBit) != 0u)
    {
        uint a = idx & ~kAnimatedTextureBit;
        float frame = (float)animFrame(gPerFrame.afT, gAnimTex[NonUniformResourceIndex(a)]);
        return gTextures2DArray[NonUniformResourceIndex(a)].SampleBias(gMaterialSampler, float3(uv, frame), gPerFrame.materialMipBias);
    }
    return gTextures2D[NonUniformResourceIndex(idx)].SampleBias(gMaterialSampler, uv, gPerFrame.materialMipBias);
}

// Explicit-gradient variant for fragment paths that have already propagated
// derivatives through a procedural UV transform. SampleGrad has no bias
// parameter, so scale the gradients by 2^materialMipBias: texture LOD is the
// log2 of gradient magnitude, making this scaling add materialMipBias exactly.
public float4 sampleBindless2DGrad(uint idx, float2 uv, float2 dUVdx, float2 dUVdy)
{
    if (idx == kInvalidTextureIndex) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float g = exp2(gPerFrame.materialMipBias);
    if ((idx & kAnimatedTextureBit) != 0u)
    {
        uint a = idx & ~kAnimatedTextureBit;
        float frame = (float)animFrame(gPerFrame.afT, gAnimTex[NonUniformResourceIndex(a)]);
        // Texture2DArray gradients are 2D: the array layer has no derivative.
        return gTextures2DArray[NonUniformResourceIndex(a)].SampleGrad(
            gMaterialSampler, float3(uv, frame), dUVdx * g, dUVdy * g);
    }
    return gTextures2D[NonUniformResourceIndex(idx)].SampleGrad(
        gMaterialSampler, uv, dUVdx * g, dUVdy * g);
}

// -----------------------------------------------------------------------------
// Per-stream BDA handle arrays were folded into UniformObject (posHandle /
// normalHandle / tangentHandle / uv0Handle / colorHandle / indexHandle in
// SceneTypes.slang) — they were always indexed by the same object slot. The
// kBindingOpaque*Handles bindings are now unused/reserved. fetchBindlessTriangle
// and the gbuffer / particle vertex shaders read the addresses off gSceneObjects.
// -----------------------------------------------------------------------------

// Per-object-slot reuse generation: the host bumps the slot's counter whenever
// m_diffuseBindless (re)assigns that bindless slot to a different object (see
// cHybridRenderer::Draw), so a consumer holding a cached slot index can detect
// that the slot now refers to different geometry.
[vk::binding(kBindingBindlessSlotGeneration, 0)]
public RWStructuredBuffer<uint> gBindlessSlotGeneration : register(u0, space70);

// Coarse world-space light grid (LightGridBuildPass writes, the shading passes'
// NEE reads). gLightGridList stores PACKED light ids (packLightId(type, slot) — the
// same value getLight() decodes; stable across frames so a reservoir's persisted
// id stays valid) for each cell, kLightsPerCellMax per cell; gLightGridCount is
// the (capped) per-cell length.
//
// gLightGridWeight runs PARALLEL to gLightGridList, same indexing, and holds the
// cell's sampling CDF: entry k is the running sum of estimated contribution over
// entries 0..k, so the last valid entry is the total. It exists so a one-sample
// NEE can importance-sample the cell instead of picking uniformly — with a
// uniform pick, the odds of drawing the light that actually matters are 1/count,
// and every miss still pays a full shadow-ray traversal.
[vk::binding(kBindingLightGridCount, 0)]
public RWStructuredBuffer<uint> gLightGridCount : register(u0, space72);

[vk::binding(kBindingLightGridList, 0)]
public RWStructuredBuffer<uint> gLightGridList : register(u0, space73);

// space = 32 + binding (GlobalManagedSets.cpp), so kBindingLightGridWeight 45 → 77.
[vk::binding(kBindingLightGridWeight, 0)]
public RWStructuredBuffer<float> gLightGridWeight : register(u0, space77);

// -----------------------------------------------------------------------------
// Scene-object + per-material-variant tables. Slot-allocated by the same
// bindless pool as opaque materials; each variant has its own table so
// structs stay at their natural size and shaders never branch on a runtime
// `type` discriminator merely to interpret field offsets (the discriminator
// is still present in `type` for cross-array iteration).
//
// Declared RW for uniform descriptor-set layout across all passes — passes
// that only read get write capability "for free" without changing the
// descriptor type. Host upload still goes through the staging-copy path,
// not via shader writes.
// -----------------------------------------------------------------------------
[vk::binding(kBindingSceneObjects, 0)]
public RWStructuredBuffer<UniformObject> gSceneObjects : register(u0, space52);

// Flat material table (Falcor MaterialSystem model). gMaterials[materialID] is a
// fixed-size MaterialDataBlob; the Material.slang loaders read the type from
// its header and reinterpret<T> it into the concrete material struct. Replaces the
// old typed DiffuseMaterial / TranslucentMaterial / WaterMaterial / DecalMaterial SSBOs.
[vk::binding(kBindingMaterials, 0)]
public StructuredBuffer<MaterialDataBlob> gMaterials : register(t0, space53);

// -----------------------------------------------------------------------------
// Per-world light/fog SSBOs. These live on the dedicated per-world set kWorldSet
// (not the engine-global set 0): cWorld owns the buffers, and every pass that
// reads them binds this set via RIProgram::bindDescriptors (cached). Counts come
// from SceneConstants on set 1 (the per-frame UBO) — iterate up to that count,
// not the array length.
// -----------------------------------------------------------------------------
[vk::binding(kBindingWorldPointLights, kWorldSet)]
public RWStructuredBuffer<PointLight> gPointLights;

[vk::binding(kBindingWorldSpotLights, kWorldSet)]
public RWStructuredBuffer<SpotLight> gSpotLights;

[vk::binding(kBindingWorldAreaLights, kWorldSet)]
public RWStructuredBuffer<RectLight> gAreaLights;

// -----------------------------------------------------------------------------
// V-buffer. Written by VBufferPomBary.cs (fed by the raster VBufferRaster.3d
// hit), consumed by the shading passes.
//
// This image and the TLAS live on **set 1** (not the persistent
// bindless set 0). They're swapchain-indexed views / per-frame-rebuilt
// acceleration structures, so they'd collide with the previous frame's
// in-flight command buffer if rewritten in place on set 0. Set 1 is
// allocated by RIProgram from a per-frame-rotated descriptor pool
// (RI_NUMBER_FRAMES_FLIGHT), so each `bindDescriptors` write lands on
// a fresh, idle set. The host pushes them per-dispatch via the same
// mechanism `gPerFrame` uses.
//
//   gPackedHitInfo   : full-resolution V-buffer; uint4 = HitInfo.packed.
//                      Written by VBufferPomBary.cs, which copies the raster
//                      hit from gPackedHitInfoRaster and perturbs the
//                      barycentrics for parallax-occlusion mapping.
// -----------------------------------------------------------------------------
[vk::binding(kBindingPackedHitInfo, 1)]
public RWTexture2D<uint4> gPackedHitInfo;

// (Per-bounce refraction/reflection V-buffers removed: water refraction now
// clobbers gPackedHitInfo like glass, and water reflection moved to the raster
// water pass.)

// Scene depth (opaque geometry) — depth-aspect SRV of the main depth buffer.
// Bound per-frame on set 1 by the particle pass for the soft-particle fade.
// Only the particle shader samples it; other importers of `bindless` that
// don't reference it emit no binding (slang strips the unused declaration).
[vk::binding(kBindingSceneDepth, 1)]
public Texture2D<float> gSceneDepth;

// core_dissolve.tga — the legacy 128×128 dissolve noise (solid_z.frag.fsl).
// Immutable, written once at init; sampled UV-space (wrap) by the shared
// Material.alphaTest for the CoverageAmount dissolve fade.
[vk::binding(kBindingDissolveMap, 0)]
public Texture2D<float4> gDissolveMap : register(t0, space80);

// Scene top-level acceleration structure (TLAS). Pushed on set 1
// per-dispatch (the TLAS is rebuilt every frame).
[vk::binding(kBindingTlas, 1)]
public RaytracingAccelerationStructure gRtAccel;
