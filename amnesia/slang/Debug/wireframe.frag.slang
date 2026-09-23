/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Wireframe debug renderer fragment stage. Flat color from the per-draw
// WireFramePass UBO — same struct/binding as wireframe.vert.slang.

struct WireFramePass
{
    float4x4 mvp;
    float4   color;
};

[vk::binding(0, 0)]
ConstantBuffer<WireFramePass> pass;

struct PSOut
{
    [vk::location(0)] float4 out_color : SV_Target0;
};

[shader("fragment")]
PSOut psMain()
{
    PSOut o;
    o.out_color = pass.color;
    return o;
}
