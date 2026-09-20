/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/gui.vert. Pairs with the still-GLSL
// amnesia/glsl/gui.frag — interface matching is by descriptor set+binding
// (UBO at set=0 binding=0) and by location number, not by source-level
// names, so this file can adopt Slang naming without disturbing the frag
// side.
//
// Vertex stream layout (host: PositionTexColor in GuiSet.cpp):
//   loc 0 : float3 position
//   loc 1 : float2 texcoord
//   loc 2 : float4 color
// stride 36 bytes, single binding 0 (VkVertexInputBindingDescription
// configured in cGuiSet::Render).

struct GuiPass
{
    float4x4 mvp;
    float4   clipPlane[4];
    int      textureConfig;
};

[vk::binding(0, 0)]
ConstantBuffer<GuiPass> pass;

struct VSIn
{
    [vk::location(0)] float3 a_position : POSITION;
    [vk::location(1)] float2 a_texcoord : TEXCOORD0;
    [vk::location(2)] float4 a_color : COLOR0;
};

struct VSOut
{
    [vk::location(0)] float2 v_texcoord : TEXCOORD0;
    [vk::location(1)] float4 v_color : COLOR0;
    // Screen-space (pre-MVP) position — the clip planes in pass.clipPlane
    // are computed in screen space (see cGuiSet::Render), so the frag
    // shader's clip test multiplies them against this rather than posH.
    [vk::location(2)] float4 v_position : TEXCOORD1;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    o.v_color    = vin.a_color;
    o.v_texcoord = vin.a_texcoord;
    o.v_position = float4(vin.a_position, 1.0f);
    o.posH       = mul(pass.mvp, float4(vin.a_position, 1.0f));
    return o;
}
