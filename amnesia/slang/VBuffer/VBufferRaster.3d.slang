// Raster G-buffer pass — Slang port of amnesia/glsl/gbuffer.{vert,frag}.
//
// vsMain pulls position / uv from per-instance VkDeviceAddress arrays
// (gOpaque*Handles in bindless), keyed by SV_VertexID +
// SV_InstanceID. The pipeline is declared with zero VkVertexInputBindings;
// indexing is done in-shader via the optional per-instance index buffer,
// matching the GLSL twin.
//
// psMain alpha-tests against the diffuse material's alpha slot, then writes
// the closest hit (instanceID, primitiveIndex, barycentrics) into the
// project's packed TriangleHit format — the uint4 layout of HitInfo /
// TriangleHit in SceneTypes.slang. VBufferPomBary.cs copies it into
// gPackedHitInfo, and PathTracePass.rt, DirectLightingPass.cs,
// DirectSpatialReusePass.cs and MainCompositePass.cs decode it there via
// HitInfo(uint4).

import Scene;
import PerFrame.resource;     // gPerFrame (camera view-proj / eye position)
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import SceneTypes;
import bindless;
import GeometryStream;

// Per-stream reads go through the shared GeometryStream helpers, which keep
// the Vulkan device-address and DXIL descriptor/offset access paths behind a
// common byte-offset load interface.
struct VSOut
{
    float4 posH                          : SV_POSITION;
    [vk::location(0)] float2 texC                          : TEXCOORD0;
    [vk::location(1)] nointerpolation uint instanceID      : TEXCOORD1;
    [vk::location(2)] nointerpolation uint materialID      : TEXCOORD2;
    // No-jitter clip positions this frame and last frame, for motion vectors.
    // Interpolated (perspective-correct), divided by w in the FS.
    [vk::location(3)] float4 curClip                       : TEXCOORD3;
    [vk::location(4)] float4 prevClip                      : TEXCOORD4;
}

// Two render targets: packed TriangleHit + screen-space velocity (curUV-prevUV).
struct GBufferOut
{
    uint4  hit      : SV_TARGET0;
    float2 velocity : SV_TARGET1;
};

[shader("vertex")]
VSOut vsMain(uint vertexID    : SV_VertexID,
             uint drawInst    : SV_InstanceID,
             uint baseInst    : SV_StartInstanceLocation)
{
    // SV_InstanceID is the per-draw instance counter (always 0 here since
    // instanceCount=1): it excludes firstInstance on D3D12 natively and on
    // Vulkan because slangc emits InstanceIndex - BaseInstance. The bindless
    // slot id arrives via firstInstance, so add SV_StartInstanceLocation
    // (SM 6.8 on D3D12) on every target.
    uint instanceID = drawInst;
    instanceID += baseInst;
    VSOut o;

    // Stream BDAs now live in the per-object UniformObject (folded in from the
    // former gOpaque*Handles buffers).
    UniformObject obj = gSceneObjects[instanceID];
    uint64_t indexAddr = obj.indexHandle;
    uint64_t posAddr   = obj.posHandle;
    uint64_t uv0Addr   = obj.uv0Handle;

    // Missing index buffer → non-indexed (each vertexID is its own vertex).
    // Missing attribute streams → safe zero so the transform doesn't NaN out.
    uint idx = (indexAddr != 0ull)
        ? geometryLoad(indexAddr, vertexID * 4u)
        : vertexID;

    float3 aPos = (posAddr != 0ull)
        ? asfloat(geometryLoad4(posAddr, idx * 16u)).xyz
        : float3(0.0f);
    // UV0 stream is tightly packed vec3 (12-byte stride).
    float2 aUv  = (uv0Addr != 0ull)
        ? asfloat(geometryLoad3(uv0Addr, idx * 12u)).xy
        : float2(0.0f);

    float4 worldP     = mul(obj.modelMat,     float4(aPos, 1.0f));
    float4 prevWorldP = mul(obj.prevModelMat, float4(aPos, 1.0f));
    // Category a: the primary raster sample uses the host-side jittered
    // projection, which the matching full-extent ray reconstruction samples.
    float4x4 jitteredViewProj   = mul(gPerFrame.projMat, gPerFrame.viewMat);
    float4x4 unjitteredViewProj = mul(gPerFrame.unjitteredProjMat, gPerFrame.viewMat);
    float4x4 prevViewProj = mul(gPerFrame.prevProjMat, gPerFrame.prevViewMat);
    o.posH       = mul(jitteredViewProj, worldP);
    o.curClip    = mul(unjitteredViewProj, worldP);
    o.prevClip   = mul(prevViewProj, prevWorldP);
    o.texC       = mul(obj.uvMat, float4(aUv, 0.0f, 1.0f)).xy;
    o.instanceID = instanceID;
    o.materialID = gSceneObjects[instanceID].materialID;
    return o;
}

[shader("pixel")]
GBufferOut psMain(VSOut vsOut,
                 uint   primitiveID : SV_PrimitiveID,
                 float3 barys       : SV_Barycentrics)
{
    // Diffuse alpha gate + CoverageAmount dissolve — same reject the legacy
    // z-prepass applied before any pixel could survive (alpha cutout plus the
    // noise dissolve fade, solid_z.frag.fsl). Discarded pixels write neither
    // target.
    VertexData v = {};
    v.texC = vsOut.texC;
    if (materialAlphaTest(v, vsOut.materialID,
                          gSceneObjects[vsOut.instanceID].dissolveAmount))
        discard;

    GBufferOut o;

    // Packed TriangleHit — this layout is the packing contract every
    // downstream decoder depends on (VBufferPomBary.cs, the composite, the
    // path tracer's primary-hit recovery), so it must stay byte-exact:
    //   .x = instanceID  .y = primitiveIndex  .z = packHalf2x16(b1,b2)  .w = 0
    uint b1 = f32tof16(barys.y);
    uint b2 = f32tof16(barys.z);
    o.hit.x = vsOut.instanceID;
    o.hit.y = primitiveID;
    o.hit.z = b1 | (b2 << 16);
    o.hit.w = 1u;

    // Category b: screen-space velocity deliberately stays unjittered
    // curUV - prevUV (NRD and the upscaler expect this); camera jitter is
    // delivered separately via CommonSettings.cameraJitter and must not be
    // folded into this velocity. NDC→UV uses the engine's y-down convention
    // (uv = ndc * (0.5,-0.5) + 0.5, see computeNonNormalizedRayDirPinhole).
    float2 curUV  = (vsOut.curClip.xy  / vsOut.curClip.w)  * float2(0.5f, -0.5f) + 0.5f;
    float2 prevUV = (vsOut.prevClip.xy / vsOut.prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
    o.velocity = curUV - prevUV;
    return o;
}
