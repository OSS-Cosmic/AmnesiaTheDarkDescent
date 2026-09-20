/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Shared fullscreen-triangle vertex shader for every post-effect frag pass
// (Bloom, ColorConvTex, ImageTrail, RadialBlur, and the blit). No vertex input — UV and clip-space position are derived from
// SV_VertexID; the host issues vkCmdDraw(3, 1, 0, 0).
//
// v_uv = (0,0) is at the top-left of the framebuffer, matching how textures
// are stored (row 0 at the top). This matters because post-effects ping-pong
// through a pogo buffer that MainCompositePass.cs seeds — a compute pass
// writing gOutput[pixel] with row 0 at the top. Keeping the UV origin
// top-left means a downstream sampler reads pogo at the same row the
// upstream pass wrote.
//
// The clip position bakes the Y flip in, so it pairs with the engine's
// negative-height viewport ({0, H, W, -H}) like every other raster pass. That
// combination is backend-neutral: Vulkan's signed-height viewport and D3D12's
// native Y-up window mapping both land v_uv=(0,0) on row 0. Emitting the
// unflipped uv*2-1 here instead would only be correct under a positive-height
// viewport, which has no D3D12 equivalent and renders upside down there.

struct VSOut
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;
    o.v_uv = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    o.posH = float4(float2(2.0f, -2.0f) * o.v_uv + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
