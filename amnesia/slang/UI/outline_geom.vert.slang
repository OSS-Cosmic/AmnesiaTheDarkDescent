/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Shared geometry vertex shader for cLuxEffectRenderer's object passes
// (hover-outline silhouette/rim, pickup flash, enemy darkness glow). Pulls
// the renderable's fixed-function streams the same way the translucent raster
// pass does:
//   binding 0 / loc 0 : float3 position (stride 16, .xyz consumed)
//   binding 1 / loc 1 : float3 normal   (stride 12; fallback +Z when absent)
// The host feeds view/viewProj via a frame-scratch UBO ("pass", set 0 b0,
// RI.UpdateFrameUBO) and the per-object model matrix + glow color via a push
// constant. The view-space normal is forwarded for the enemy rim term.

#include "../HostDefinitions.h"

struct OutlinePass
{
    float4x4 viewProj;
    float4x4 view;
};

[vk::binding(0, 0)]
ConstantBuffer<OutlinePass> pass;

struct OutlinePC
{
    float4x4 model;
    float4   color; // rgb = glow color, a = intensity
};

[vk::push_constant]
OutlinePC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct VSIn
{
    [vk::location(0)] float3 a_position : TEXCOORD0;
    [vk::location(1)] float3 a_normal : TEXCOORD1;
};

struct VSOut
{
    [vk::location(0)] float3 v_viewNormal : TEXCOORD0;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    float4 worldPos = mul(gPushConstants.model, float4(vin.a_position, 1.0f));
    o.posH = mul(pass.viewProj, worldPos);
    // Ignore non-uniform scale (good enough for a rim term).
    float3 worldN = mul((float3x3)gPushConstants.model, vin.a_normal);
    o.v_viewNormal = mul((float3x3)pass.view, worldN);
    return o;
}
