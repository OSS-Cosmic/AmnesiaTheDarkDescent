/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Editor / debug overlay batcher — textured quad / billboard fragment
// stage: diffuse sample modulated by the per-vertex tint. Alpha-blended by
// the pipeline (icons have cutout alpha). Pairs with debug_uv.vert.slang.
//
// Bindings (DebugDraw::flush configures these via DescriptorBindingID):
//   set 0 binding 0 : ConstantBuffer<DebugPass> pass
//   set 0 binding 1 : SamplerState              diffuseSampler
//   set 0 binding 2 : Texture2D<float4>         diffuseMap

struct DebugPass
{
    float4x4 viewProjMat;
    float4x4 viewProj2DMat;
};

[vk::binding(0, 0)]
ConstantBuffer<DebugPass> pass;

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
    o.out_color = pin.v_color * diffuseMap.Sample(diffuseSampler, pin.v_uv);
    return o;
}
