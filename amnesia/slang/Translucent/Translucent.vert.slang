/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/translucency.vert.fsl,
// adapted to use **traditional Vulkan vertex bindings** instead of the
// bindless BDA-pull pattern the opaque + particle paths use.
//
// Rationale: only refractive translucents live in the TLAS (World.cpp
// admits them as kRayMaskTranslucent and skips every other translucent), and
// that TLAS slot is the only consumer of the per-translucent vertex BDAs in
// gOpaque*Handles[]. Note the original reason for that membership — letting an
// RT V-buffer ray-bend the primary ray through glass — is gone with that pass;
// no ray path traces kRayMaskTranslucent today, since the shadow, reflection
// and path-trace rays (Material.slang, PathTracePass.rt) all trace against
// kRayMaskOpaque / kRayMaskShadow. The raster path here is low-traffic — it
// draws every translucent mesh once per frame and doesn't need the bindless slot
// machinery just to fetch its own vertex data. Fixed-function vertex
// fetch via vkCmdBindVertexBuffers + vkCmdBindIndexBuffer is simpler and
// matches the binding layout declared in `TranslucentMeshPipelineDesc`
// (HybridRenderer.cpp).
//
// SV_InstanceID is still consumed: the bindless OBJECT slot remains the
// source of `UniformObject` (model matrix, uv matrix, materialID, ...),
// because Translucent.frag.slang reads it for material lookup.

import bindless;
import SceneTypes;
import PerFrame.resource;

// Per-vertex inputs — match the bindings declared in
// TranslucentMeshPipelineDesc:
//   binding 0 / location 0 : position  (R32G32B32_SFLOAT, stride 16)
//   binding 1 / location 1 : normal    (R32G32B32_SFLOAT, stride 12)
//   binding 2 / location 2 : tangent   (R32G32B32A32_SFLOAT, stride 16, w = handedness)
//   binding 3 / location 3 : color     (R32G32B32A32_SFLOAT, stride 16)
//   binding 4 / location 4 : texcoord  (R32G32_SFLOAT, stride 12 — engine stream is float3, only .xy read)
struct TranslucentVSIn
{
    float3 a_position : POSITION;
    float3 a_normal   : NORMAL;
    float4 a_tangent  : TANGENT;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct TranslucentVSOut
{
    [vk::location(0)] float3 viewPos                 : TEXCOORD0;   // view-space position (for fog + refraction invDist)
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] float3 normal                  : TEXCOORD2;    // view-space normal
    [vk::location(3)] float3 tangent                 : TEXCOORD3;   // view-space tangent
    [vk::location(4)] float3 bitangent               : TEXCOORD4; // view-space bitangent
    [vk::location(5)] float4 color                   : TEXCOORD5;
    [vk::location(6)] nointerpolation uint objectId  : TEXCOORD6;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
TranslucentVSOut vsMain(TranslucentVSIn vin,
                        uint drawInst : SV_InstanceID,
                        uint baseInst : SV_StartInstanceLocation)
{
    // SV_InstanceID alone is the per-draw counter; slangc subtracts
    // BaseInstance from Vulkan's InstanceIndex to give DX semantics.
    // Bindless slot id arrives via firstInstance — reconstruct
    // gl_InstanceIndex by summing. Same fix as VBufferRaster.3d.slang.
    uint instanceId = drawInst;
    instanceId += baseInst;

    UniformObject obj     = gSceneObjects[instanceId];
    float4x4 modelView    = mul(gPerFrame.viewMat, obj.modelMat);
    // Category (a): translucent geometry is part of the primary full-render
    // raster, so projMat remains the host's jittered projection and receives no
    // duplicate shader-side offset.
    float4x4 modelViewPrj = mul(gPerFrame.projMat, modelView);

    // Normal matrix = (modelView^-1)^T = (invModel * invView)^T
    //                                  = invView^T * invModel^T
    // Mirrors the reference's ToNormalMat(invModelMat, invViewMat).
    float3x3 invModelRot = float3x3(obj.invModelMat);
    float3x3 invViewRot  = float3x3(gPerFrame.invViewMat);
    float3x3 normalMat   = mul(transpose(invViewRot), transpose(invModelRot));

    TranslucentVSOut vsOut;
    vsOut.viewPos   = mul(modelView, float4(vin.a_position, 1.0f)).xyz;
    vsOut.uv        = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    vsOut.normal    = normalize(mul(normalMat, vin.a_normal));
    vsOut.tangent   = normalize(mul(normalMat, vin.a_tangent.xyz));
    // Bitangent reconstructed as cross(tangent, normal) * handedness — keeps
    // exported handedness from the engine's tangent.w convention.
    vsOut.bitangent = normalize(mul(normalMat,
                                    cross(vin.a_tangent.xyz, vin.a_normal) * vin.a_tangent.w));
    vsOut.color     = vin.a_color;
    vsOut.objectId  = instanceId;
    vsOut.posH      = mul(modelViewPrj, float4(vin.a_position, 1.0f));
    return vsOut;
}
