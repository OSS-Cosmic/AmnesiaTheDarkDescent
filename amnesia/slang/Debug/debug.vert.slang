/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Editor / debug overlay batcher (DebugDraw.cpp) — 3D vertex-colored
// primitives (lines, wireframes, solid tris/quads). Standalone like the GUI
// shaders — no bindless set; the host binds one frame-scratch UBO per flush
// via RI.UpdateFrameUBO ("pass"). Pairs with debug.frag.slang.
//
// Unified vertex stream layout (host: DebugDraw::DebugVertex, stride 36):
//   loc 0 : float3 position (offset 0)
//   loc 1 : float4 color    (offset 20 — uv at 12 is skipped by this stage)

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
    o.posH = mul(pass.viewProjMat, float4(vin.a_position, 1.0f));
    return o;
}
