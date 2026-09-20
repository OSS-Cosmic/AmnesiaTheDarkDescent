/// Copyright © 2009-2020 Frictional Games
/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Simple unlit textured forward renderer (cRendererSimple). Standalone like
// the wireframe shaders — no bindless set; the host binds one frame-scratch
// UBO per draw via RI.UpdateFrameUBO ("pass") plus a per-draw diffuse
// texture + sampler. Pairs with Simple.frag.slang, which redeclares
// SimplePass at the same set/binding.
//
// Vertex stream layout (host: RendererSimple.cpp pipeline desc):
//   loc 0 : float3 position (engine position stream is float4, stride 16;
//           only .xyz is read)
//   loc 1 : float4 color    (engine color stream, stride 16; single-vertex
//           white fallback when the renderable omits it)
//   loc 2 : float2 uv       (engine texcoord stream is float3, stride 12;
//           only .xy is read; single-vertex zero fallback when omitted)

struct SimplePass
{
    float4x4 mvp;
    float4x4 uvMtx;
    float4   color;
    float    alphaReject;
};

[vk::binding(0, 0)]
ConstantBuffer<SimplePass> pass;

struct VSIn
{
    [vk::location(0)] float3 a_position : TEXCOORD0;
    [vk::location(1)] float4 a_color : TEXCOORD1;
    [vk::location(2)] float2 a_uv : TEXCOORD2;
};

struct VSOut
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    [vk::location(1)] float4 v_color : TEXCOORD1;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(VSIn vin)
{
    VSOut o;
    o.v_uv = mul(pass.uvMtx, float4(vin.a_uv, 0.0f, 1.0f)).xy;
    o.v_color = vin.a_color;
    o.posH = mul(pass.mvp, float4(vin.a_position, 1.0f));
    return o;
}
