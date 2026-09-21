/// Copyright © 2009-2020 Frictional Games
/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Pickup-flash + enemy-darkness-glow vertex shader for cLuxEffectRenderer. The
// glow fills the object's visible surface (no rim), additively modulated by its
// own diffuse texture, so this stage only needs the texcoord. Streams:
//   binding 0 / loc 0 : float3 position (stride 16, .xyz consumed)
//   binding 1 / loc 1 : float2 texcoord (Texture0 stream, stride 12, .xy)
// A tiny clip-space depth bias (posH.z -= 0.0001) pulls the glow toward the
// camera so the front faces win the LEQUAL test against the scene depth. view/
// viewProj arrive via the shared "pass" UBO and the per-object model + tint via
// the push constant. Pairs with glow_object.frag.

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
    float4   color;  // rgb = linear tint, a = intensity (mfAlpha * pulse)
    float4   params; // x = alpha-test flag, y = diffuse single-channel flag
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
