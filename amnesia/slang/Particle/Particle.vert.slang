/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/particle.vert. Like its GLSL twin, this is a
// bindless rewrite of AmnesiaTheDarkDescent/HPL2/resource/translucency_particle.vert.fsl:
// particles share the OBJECT_SLOT bindless pool with solid geometry, so
// cHybridRenderer writes the particle vertex buffer's BDA into the same
// opaque*Handles[] arrays and SV_InstanceID selects the slot.
//
// Per-vertex stream strides come from iParticleEmitter::iParticleEmitter
// (ParticleEmitter.cpp:111-113):
//   position : float x 4  (stride 16, only .xyz read)
//   color    : float x 4  (stride 16, full rgba)
//   texture0 : float x 3  (stride 12, only .xy read)
// `[vk::buffer_reference]` is scalar-layout in Slang — same memory model as
// GLSL `layout(buffer_reference, scalar)` — so float4/float3 match those
// strides exactly. Using float3 for position / float2 for uv0 here would
// step 12 / 8 bytes per vertex instead of 16 / 12 and read garbage offsets
// (manifests as billboards anchored to the camera instead of the world).

import bindless;
import GeometryStream;
import SceneTypes;
import PerFrame.resource;

// BDA accesses go through the shared GeometryStream helpers, which preserve
// the target-specific address ABI while keeping these loads byte-offset based.

struct ParticleVSOut
{
    [vk::location(0)] float3 viewPos                 : TEXCOORD0;   // view-space position (for fog + lightLevelAt posW)
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] float4 color                   : TEXCOORD2;
    [vk::location(3)] nointerpolation uint objectId  : TEXCOORD3;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
ParticleVSOut vsMain(uint vertexId : SV_VertexID,
                     uint drawInst : SV_InstanceID,
                     uint baseInst : SV_StartInstanceLocation)
{
    // SV_InstanceID alone is the per-draw counter; slangc subtracts
    // BaseInstance from Vulkan's InstanceIndex to give DX semantics.
    // Bindless slot id arrives via firstInstance — reconstruct
    // gl_InstanceIndex by summing. Same fix as VBufferRaster.3d.slang.
    uint instanceId = drawInst;
    instanceId += baseInst;

    // Stream BDAs now live in the per-object UniformObject (folded in from the
    // former gOpaque*Handles buffers — for particles these point at the
    // per-viewport translucent scratch ring, written via ObjectSubmitDesc).
    UniformObject obj  = gSceneObjects[instanceId];
    uint64_t indexAddr = obj.indexHandle;
    uint64_t posAddr   = obj.posHandle;
    uint64_t uv0Addr   = obj.uv0Handle;
    uint64_t colAddr   = obj.colorHandle;

    uint idx = (indexAddr != 0ull)
        ? geometryLoad(indexAddr, vertexId * 4u)
        : vertexId;

    float3 a_position = (posAddr != 0ull)
        ? asfloat(geometryLoad4(posAddr, idx * 16u)).xyz
        : float3(0.0f);
    float2 a_texcoord = (uv0Addr != 0ull)
        ? asfloat(geometryLoad3(uv0Addr, idx * 12u)).xy
        : float2(0.0f);
    float4 a_color = (colAddr != 0ull)
        ? asfloat(geometryLoad4(colAddr, idx * 16u))
        : float4(1.0f);

    float4x4 modelView    = mul(gPerFrame.viewMat, obj.modelMat);
    float4x4 modelViewPrj = mul(gPerFrame.projMat, modelView);

    ParticleVSOut vsOut;
    vsOut.viewPos  = mul(modelView, float4(a_position, 1.0f)).xyz;
    vsOut.uv       = mul(obj.uvMat, float4(a_texcoord, 0.0f, 1.0f)).xy;
    vsOut.color    = a_color;
    vsOut.objectId = instanceId;
    vsOut.posH     = mul(modelViewPrj, float4(a_position, 1.0f));
    return vsOut;
}
