/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Editor / debug overlay batcher — 2D screen-space lines. Same DebugPass
// UBO and vertex layout as debug.vert.slang, but projects through the
// orthographic viewProj2DMat (target-pixel space). Pairs with
// debug.frag.slang.

struct DebugPass
{
    float4x4 viewProjMat;
    float4x4 viewProj2DMat;
};

[vk::binding(0, 0)]
ConstantBuffer<DebugPass> pass;

struct VSIn
{
    [vk::location(0)] float3 a_position : TEXCOORD0;
    [vk::location(1)] float4 a_color : TEXCOORD1;
};

struct VSOut
{
    [vk::location(0)] float4 v_color : TEXCOORD0;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    o.v_color = vin.a_color;
    o.posH = mul(pass.viewProj2DMat, float4(vin.a_position, 1.0f));
    return o;
}
