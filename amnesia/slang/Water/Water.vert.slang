/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water surface — vertex stage. Mirrors Translucent.vert's fixed-function
// 5-stream layout + SV_InstanceID → bindless OBJECT slot, but outputs WORLD-space
// position / normal / tangent (the frag's wave normal + sampled denoised
// half-resolution reflection are world-space, like the reflection pass). Drawn over the
// refracted background already in the pogo after the scene/refraction work.

import bindless;
import SceneTypes;
import PerFrame.resource;

// 5-stream layout (matches WaterPipelineDesc / Translucent):
//   0:position 1:normal 2:tangent(w=handedness) 3:color 4:texcoord(.xy)
struct WaterVSIn
{
    float3 a_position : POSITION;
    float3 a_normal   : NORMAL;
    float4 a_tangent  : TANGENT;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct WaterVSOut
{
    [vk::location(0)] float3 posW                    : TEXCOORD0;
    [vk::location(1)] float3 normalW                 : TEXCOORD1;
    [vk::location(2)] float3 tangentW                : TEXCOORD2;
    [vk::location(3)] float2 uv                      : TEXCOORD3;
    [vk::location(4)] nointerpolation uint objectId  : TEXCOORD4;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
WaterVSOut vsMain(WaterVSIn vin,
                  uint drawInst : SV_InstanceID,
                  uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst;
    instanceId += baseInst;
    UniformObject obj = gSceneObjects[instanceId];

    float4x4 modelView    = mul(gPerFrame.viewMat, obj.modelMat);
    float4x4 modelViewPrj = mul(gPerFrame.projMat, modelView);

    // World-space normal matrix = inverse-transpose of model 3x3 = transpose(invModelRot).
    float3x3 invModelRot = float3x3(obj.invModelMat);
    float3x3 normalMat   = transpose(invModelRot);

    WaterVSOut o;
    o.posW     = mul(obj.modelMat, float4(vin.a_position, 1.0f)).xyz;
    o.normalW  = normalize(mul(normalMat, vin.a_normal));
    o.tangentW = normalize(mul((float3x3)obj.modelMat, vin.a_tangent.xyz));
    o.uv       = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.objectId = instanceId;
    // Category (a): this is the full-render primary raster sample, so the
    // host's jittered projMat deliberately stays on the hardware projection;
    // no second jitter term belongs in the vertex shader.
    o.posH     = mul(modelViewPrj, float4(vin.a_position, 1.0f));
    return o;
}
