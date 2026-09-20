/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water reflection-guide capture — vertex stage. This keeps the water
// pass's five fixed-function streams and instance lookup, while also carrying
// current/previous homogeneous clip positions for the guide's private motion
// attachment.

import bindless;
import SceneTypes;
import PerFrame.resource;

// 5-stream layout (matches WaterPipelineDesc / Translucent):
//   0:position 1:normal 2:tangent(w=handedness) 3:color 4:texcoord(.xy)
// The guide builds its surface frame from ddx/ddy(posW), so normal/tangent/color
// go unread and slangc drops locations 1-3 from the emitted SPIR-V interface.
// That is fine: a vertex input state may declare attributes the shader does not
// consume. Do not add dummy reads to keep them alive — they would only put
// NaN-propagating arithmetic on the position path.
struct WaterVSIn
{
    float3 a_position : POSITION;
    float3 a_normal   : NORMAL;
    float4 a_tangent  : TANGENT;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct WaterGuideVSOut
{
    [vk::location(0)] float3 posW                    : TEXCOORD0;
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] nointerpolation uint objectId  : TEXCOORD2;
    float4 posH                    : SV_POSITION;
    [vk::location(3)] float4 curClip                 : TEXCOORD3;
    [vk::location(4)] float4 prevClip                : TEXCOORD4;
};

[shader("vertex")]
WaterGuideVSOut vsMain(WaterVSIn vin,
                       uint drawInst : SV_InstanceID,
                       uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst;
    instanceId += baseInst;
    UniformObject obj = gSceneObjects[instanceId];

    float4 pos = float4(vin.a_position, 1.0f);

    WaterGuideVSOut o;
    o.posW      = mul(obj.modelMat, pos).xyz;
    o.uv        = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.objectId  = instanceId;
    // Category (a): the guide coverage and interpolated posW must match the
    // full-render primary raster sample, so hardware rasterization uses the
    // host's jittered projMat without a manual shader-side offset.
    o.posH      = mul(mul(gPerFrame.projMat, gPerFrame.viewMat),
                      mul(obj.modelMat, pos));
    // Category (b): motion/NRD coordinates are intentionally unjittered, so
    // these clip positions use the current and previous unjittered matrices;
    // adding gPerFrame.jitterX/Y here would put the sample offset into motion.
    o.curClip   = mul(mul(gPerFrame.unjitteredProjMat, gPerFrame.viewMat),
                      mul(obj.modelMat, pos));
    o.prevClip  = mul(mul(gPerFrame.prevProjMat, gPerFrame.prevViewMat),
                      mul(obj.prevModelMat, pos));
    return o;
}
