/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Editor / debug overlay batcher — flat vertex color. Pairs with
// debug.vert.slang / debug_2d.vert.slang (same DebugPass UBO declared at
// the same set/binding so the program layouts agree).

struct DebugPass
{
    float4x4 viewProjMat;
    float4x4 viewProj2DMat;
};

[vk::binding(0, 0)]
ConstantBuffer<DebugPass> pass;

struct PSIn
{
    [vk::location(0)] float4 v_color : TEXCOORD0;
};

struct PSOut
{
    [vk::location(0)] float4 out_color : SV_Target0;
};

[shader("fragment")]
PSOut psMain(PSIn pin)
{
    PSOut o;
    o.out_color = pin.v_color;
    return o;
}
