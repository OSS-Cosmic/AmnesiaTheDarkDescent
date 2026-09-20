/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Editor / debug overlay batcher — textured quads / billboards (entity
// icons). Pairs with debug_uv.frag.slang.
//
// Unified vertex stream layout (host: DebugDraw::DebugVertex, stride 36):
//   loc 0 : float3 position (offset 0)
//   loc 1 : float2 uv       (offset 12)
//   loc 2 : float4 color    (offset 20)

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
    [vk::location(1)] float2 a_uv : TEXCOORD1;
    [vk::location(2)] float4 a_color : TEXCOORD2;
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
    o.v_uv = vin.a_uv;
    o.v_color = vin.a_color;
    o.posH = mul(pass.viewProjMat, float4(vin.a_position, 1.0f));
    return o;
}
