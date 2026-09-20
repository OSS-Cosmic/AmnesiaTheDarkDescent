/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Wireframe debug renderer (cRendererWireFrame). Standalone like the GUI
// shaders — no bindless set; the host binds one frame-scratch UBO per draw
// via RI.UpdateFrameUBO ("pass"). Pairs with wireframe.frag.slang, which
// redeclares WireFramePass at the same set/binding.
//
// Vertex stream layout (host: RendererWireFrame.cpp pipeline desc):
//   loc 0 : float3 position (engine position stream is float4, stride 16;
//           only .xyz is read)

struct WireFramePass
{
    float4x4 mvp;
    float4   color;
};

[vk::binding(0, 0)]
ConstantBuffer<WireFramePass> pass;

struct VSIn
{
    [vk::location(0)] float3 a_position : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    o.posH = mul(pass.mvp, float4(vin.a_position, 1.0f));
    return o;
}
