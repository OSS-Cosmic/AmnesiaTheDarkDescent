/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Alpha-tested outline fragment. Samples the material's Alpha (cutout) texture
// and discards below the 0.5 gate the engine uses (Material.slang materialAlphaTest), so
// the hover outline's stencil mark + rim follow the visible silhouette of
// cutout geometry instead of the full mesh quad. Channel selection mirrors the
// engine: single-channel alpha maps carry the mask in .r, otherwise .a
// (params.x set by the host from the texture's channel count). Surviving
// fragments emit the flat glow color (RGB additive, alpha 0). Pairs with
// outline_alpha.vert.slang.

[vk::binding(1, 0)]
SamplerState alphaSampler;

[vk::binding(2, 0)]
Texture2D<float4> alphaMap;

struct OutlineAlphaPC
{
    float4x4 model;
    float4   color;  // rgb = glow color, a = intensity
    float4   params; // x = alpha texture is single-channel (sample .r vs .a)
};

[vk::push_constant]
OutlineAlphaPC gPushConstants;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float4 s = alphaMap.Sample(alphaSampler, pin.v_uv);
    float a = (gPushConstants.params.x > 0.5f) ? s.r : s.a;
    if (a < 0.5f)
        discard;
    return float4(gPushConstants.color.rgb * gPushConstants.color.a, 0.0f);
}
