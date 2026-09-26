/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/decal.vert.fsl.
//
// Decals are a thin post-geometry overlay: a clipped mesh carrying
// position / uv / vertex-color, projected with the object's model matrix and
// the per-object uvMat, depth-tested (≤) against the GBuffer depth but never
// writing depth. The fragment shader (Decal.frag.slang) samples the diffuse
// map and multiplies by vertex colour — no lighting, fog, or reflection.
//
// Like the translucent mesh path this uses **fixed-function vertex bindings**
// (vkCmdBindVertexBuffers) declared in DecalPipelineDesc, and still binds the
// same 5 streams so the existing translucent fallback buffers and draw loop
// apply unchanged. Only position / color / texcoord are *declared* here: the
// normal and tangent streams stay bound but no longer appear in this entry
// point's input signature. That is deliberate -- D3D12 requires an input-layout
// element for every input-signature entry, including one the shader never
// reads, while Slang's SPIR-V backend prunes an unread input outright. Keeping
// the two in step means declaring exactly what is consumed.
//
// SV_InstanceID + SV_StartInstanceLocation reconstruct the bindless OBJECT
// slot id (firstInstance), which indexes gSceneObjects for the UniformObject
// (model / uv matrix, materialID). Same fix as Translucent.vert.slang.

import bindless;
import SceneTypes;
import PerFrame.resource;

// Per-vertex inputs — match the attributes declared in DecalPipelineDesc.
// Locations are assigned in declaration order, so they are 0..2 here, while the
// *binding* each one reads keeps the translucent 5-stream slot number:
//   binding 0 / location 0 : position  (R32G32B32_SFLOAT, stride 16)
//   binding 3 / location 1 : color     (R32G32B32A32_SFLOAT, stride 16)
//   binding 4 / location 2 : texcoord  (R32G32_SFLOAT, stride 12 — engine stream is float3, only .xy read)
// Bindings 1 (normal) and 2 (tangent) are still bound by BindMeshDecalStreams
// but no attribute references them; see the header note.
struct DecalVSIn
{
    float3 a_position : POSITION;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct DecalVSOut
{
    [vk::location(0)] float2 uv                      : TEXCOORD0;
    [vk::location(1)] float4 color                   : TEXCOORD1;
    [vk::location(2)] nointerpolation uint objectId  : TEXCOORD2;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
DecalVSOut vsMain(DecalVSIn vin,
                  uint drawInst : SV_InstanceID,
                  uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst;
    instanceId += baseInst;

    UniformObject obj     = gSceneObjects[instanceId];
    float4x4 modelViewPrj = mul(gPerFrame.projMat,
                                mul(gPerFrame.viewMat, obj.modelMat));

    DecalVSOut vsOut;
    vsOut.uv       = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    // Raw authored vertex color: the fragment shader runs the legacy
    // display-space source math (texel re-encoded × this tint) and decodes to
    // linear once at output — same scheme as Particle/Translucent.frag. The
    // old adobeRGBToLinear decode targeted a linear-albedo blend this VS never
    // feeds (the mesh pass draws onto the lit pogo).
    vsOut.color    = vin.a_color;
    vsOut.objectId = instanceId;
    vsOut.posH     = mul(modelViewPrj, float4(vin.a_position, 1.0f));
    return vsOut;
}

// Position-only entry for the billboard-halo occlusion queries
// (cStandardHaloPass). The queries only count samples that pass the depth
// test, so nothing but the clip position is needed; the halo pipeline declares
// location 0 alone to match.
struct DecalOcclusionVSIn
{
    float3 a_position : POSITION;
};

[shader("vertex")]
float4 vsOcclusion(DecalOcclusionVSIn vin,
                   uint drawInst : SV_InstanceID,
                   uint baseInst : SV_StartInstanceLocation) : SV_POSITION
{
    UniformObject obj     = gSceneObjects[drawInst + baseInst];
    float4x4 modelViewPrj = mul(gPerFrame.projMat,
                                mul(gPerFrame.viewMat, obj.modelMat));
    return mul(modelViewPrj, float4(vin.a_position, 1.0f));
}
