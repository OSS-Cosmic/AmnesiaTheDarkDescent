/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/inventory_post.vert.
//
// Fullscreen-triangle vertex shader. No vertex input — UV and clip-space
// position are derived from SV_VertexID; the host issues vkCmdDraw(3,1,0,0).
//
// Vulkan clip-space has Y pointing down compared to GL; we keep UV.y = 0
// at the top of the texture and flip Y in clip space so a (0,0)→(1,1) UV
// sweep covers the screen top-to-bottom.

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
    o.posH = float4(o.v_uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
                    0.0f, 1.0f);
    return o;
}
