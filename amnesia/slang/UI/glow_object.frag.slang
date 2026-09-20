/// Copyright © 2009-2020 Frictional Games
/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Pickup-flash + enemy-darkness-glow fragment for cLuxEffectRenderer. The glow
// FILLS the object's visible surface (front faces, cull BACK) so the mesh
// interior lights up, rather than hugging the silhouette. It is the object's own
// diffuse colour, tinted bluish, added into the linear-HDR scene:
//   out.rgb = tex.rgb * tint * intensity   (alpha 0 — RGB-only additive)
// Flash sets the alpha-test flag (params.x) to discard cutout texels (channel
// chosen by params.y, mirroring the engine's single-channel handling); enemy
// leaves it off. Written pre-tonemap, so the tint is a straight linear
// multiplier (no sRGB decode). Pairs with glow_object.vert.

[vk::binding(1, 0)]
SamplerState diffuseSampler;

[vk::binding(2, 0)]
Texture2D<float4> diffuseMap;

struct OutlineAlphaPC
{
    float4x4 model;
    float4   color;  // rgb = linear tint, a = intensity (mfAlpha * pulse)
    float4   params; // x = alpha-test flag, y = diffuse single-channel flag
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
    float4 tex = diffuseMap.Sample(diffuseSampler, pin.v_uv);
    if (gPushConstants.params.x > 0.5f)
    {
        float a = (gPushConstants.params.y > 0.5f) ? tex.r : tex.a;
        if (a < 0.5f)
            discard;
    }
    return float4(tex.rgb * gPushConstants.color.rgb * gPushConstants.color.a, 0.0f);
}
