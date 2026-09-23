/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Alpha-tested variant of the outline geometry vertex shader. Used for
// renderables whose material carries an Alpha (cutout) texture so the hover
// outline follows the visible silhouette instead of the full mesh quad —
// matching the legacy cLuxEffectRenderer (deferred_base + UseUv/UseDiffuse).
// Streams: position (binding 0) + texcoord (binding 1, .xy). Pairs with
// outline_alpha.frag.slang.

#include "../HostDefinitions.h"

struct OutlinePass
{
    float4x4 viewProj;
    float4x4 view;
};

[vk::binding(0, 0)]
ConstantBuffer<OutlinePass> pass;

struct OutlineAlphaPC
{
    float4x4 model;
    float4   color;  // rgb = glow color, a = intensity
    float4   params; // x = alpha texture is single-channel (sample .r vs .a)
};

[vk::push_constant]
OutlineAlphaPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct VSIn
{
    [vk::location(0)] float3 a_position : TEXCOORD0;
    [vk::location(1)] float2 a_uv : TEXCOORD1;
};

struct VSOut
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    float4 worldPos = mul(gPushConstants.model, float4(vin.a_position, 1.0f));
    o.posH = mul(pass.viewProj, worldPos);
    o.v_uv = vin.a_uv;
    return o;
}
