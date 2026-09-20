/// Copyright © 2009-2020 Frictional Games
/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Simple unlit textured forward renderer fragment stage: diffuse sample
// modulated by the per-vertex tint and the per-draw color. alphaReject > 0
// turns on cutout alpha (alpha-tested grates/foliage in the diffuse list);
// blended lists keep their hardware blend and leave it at 0. Pairs with
// Simple.vert.slang.
//
// Bindings (RendererSimple::Draw configures these via DescriptorBindingID):
//   set 0 binding 0 : ConstantBuffer<SimplePass> pass
//   set 0 binding 1 : SamplerState               diffuseSampler
//   set 0 binding 2 : Texture2D<float4>          diffuseMap

struct SimplePass
{
    float4x4 mvp;
    float4x4 uvMtx;
    float4   color;
    float    alphaReject;
};

[vk::binding(0, 0)]
ConstantBuffer<SimplePass> pass;

[vk::binding(1, 0)]
SamplerState diffuseSampler;

[vk::binding(2, 0)]
Texture2D<float4> diffuseMap;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    [vk::location(1)] float4 v_color : TEXCOORD1;
};

struct PSOut
{
    [vk::location(0)] float4 out_color : SV_Target0;
};

[shader("fragment")]
PSOut psMain(PSIn pin)
{
    PSOut o;
    float4 c = pin.v_color * pass.color * diffuseMap.Sample(diffuseSampler, pin.v_uv);
    if (c.a < pass.alphaReject) {
        discard;
    }
    o.out_color = c;
    return o;
}
