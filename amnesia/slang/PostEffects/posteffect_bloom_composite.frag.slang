/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Mip-chain bloom composite. Adds the accumulated bloom (mip[0], the largest
// bloom mip after the upsample chain) back into the HDR scene, scaled by an
// overall strength. Runs in the linear HDR pogo space, before the tonemap.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler  (bilinear, clamp)
//   binding 1 : Texture2D<float4> sourceInput  (the HDR scene)
//   binding 2 : Texture2D<float4> bloomInput   (bloom mip[0])

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture2D<float4> bloomInput;

struct BloomCompositePC
{
    float strength; // bloom contribution
    float3 _pad;
};

[vk::push_constant]
BloomCompositePC gPushConstants;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float4 scene = sourceInput.Sample(inputSampler, pin.v_uv);
    float3 bloom = bloomInput.Sample(inputSampler, pin.v_uv).rgb;
    return float4(scene.rgb + bloom * gPushConstants.strength, scene.a);
}
