/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Mip-chain bloom upsample. 9-tap tent filter (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"). Samples a smaller mip and the
// host lerps the result into the next-larger mip (pipeline blend =
// SRC_ALPHA/ONE_MINUS_SRC_ALPHA, load-op LOAD) by kUpsampleBlend. Each level
// halves the weight of everything below it, so the mip weights sum to 1 and
// the accumulated bloom never exceeds the prefiltered scene. A plain ONE/ONE
// add summed every level, scaling bloom by up to the mip count.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler  (bilinear, clamp)
//   binding 1 : Texture2D<float4> sourceInput   (the smaller mip being upsampled)

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct BloomUpsamplePC
{
    float filterRadius; // tent spread in texels of the sampled mip
    float3 _pad;
};

[vk::push_constant]
BloomUpsamplePC gPushConstants;

static const float kUpsampleBlend = 0.5f;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    uint w, h;
    sourceInput.GetDimensions(w, h);
    float2 r = (1.0f / float2(float(w), float(h))) * gPushConstants.filterRadius;
    float2 uv = pin.v_uv;

    float3 a = sourceInput.Sample(inputSampler, uv + float2(-1,  1) * r).rgb;
    float3 b = sourceInput.Sample(inputSampler, uv + float2( 0,  1) * r).rgb;
    float3 c = sourceInput.Sample(inputSampler, uv + float2( 1,  1) * r).rgb;
    float3 d = sourceInput.Sample(inputSampler, uv + float2(-1,  0) * r).rgb;
    float3 e = sourceInput.Sample(inputSampler, uv + float2( 0,  0) * r).rgb;
    float3 f = sourceInput.Sample(inputSampler, uv + float2( 1,  0) * r).rgb;
    float3 g = sourceInput.Sample(inputSampler, uv + float2(-1, -1) * r).rgb;
    float3 hh = sourceInput.Sample(inputSampler, uv + float2( 0, -1) * r).rgb;
    float3 i = sourceInput.Sample(inputSampler, uv + float2( 1, -1) * r).rgb;

    // 1 2 1 / 2 4 2 / 1 2 1, normalized by 1/16.
    float3 result = e * 4.0f;
    result += (b + d + f + hh) * 2.0f;
    result += (a + c + g + i);
    result *= (1.0f / 16.0f);

    return float4(result, kUpsampleBlend);
}
