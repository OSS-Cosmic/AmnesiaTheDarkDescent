/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Enemy darkness-glow fragment for cLuxEffectRenderer — a soft view-space
// rim so an approaching enemy reads against the dark. The silhouette edge
// (view-space normal nearly perpendicular to the view axis, |N.z| -> 0) lights
// up; the facing interior fades out. Additive, RGB-only (alpha 0). Pairs with
// outline_geom.vert.slang (shared push constant block).

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

struct PSIn
{
    [vk::location(0)] float3 v_viewNormal : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 N = normalize(pin.v_viewNormal);
    float rim = pow(saturate(1.0f - abs(N.z)), 2.5f);
    return float4(gPushConstants.color.rgb * (rim * gPushConstants.color.a), 0.0f);
}
