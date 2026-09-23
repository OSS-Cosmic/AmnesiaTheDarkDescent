/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Flat glow fragment for cLuxEffectRenderer. Used by:
//   - the outline silhouette mark pass (color writes masked off by the host),
//   - the outline rim color pass (writes the glow color where stencil != ref),
//   - the pickup flash pass (additive flat glow modulated by intensity).
// Output alpha is 0 so an RGB-only additive blend leaves the scene alpha
// untouched. Pairs with outline_geom.vert.slang (same push constant block).

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
    return float4(gPushConstants.color.rgb * gPushConstants.color.a, 0.0f);
}
