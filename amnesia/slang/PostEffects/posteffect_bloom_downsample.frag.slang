/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Mip-chain bloom downsample. 13-tap filter from Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014). Used to build
// the bloom mip chain: each mip samples the previous (larger) mip.
//
// On the FIRST downsample (applyThreshold != 0, sampling the full-res HDR scene)
// the 5 overlapping 2x2 boxes are combined with a normalized Karis average (1/(1+luma)) to
// stop a single fireflies from dominating the box, then a soft-knee threshold is
// applied so only bright pixels feed the bloom. Subsequent downsamples are a
// plain weighted 13-tap (applyThreshold == 0).
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler  (bilinear, clamp)
//   binding 1 : Texture2D<float4> sourceInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct BloomDownsamplePC
{
    float threshold;     // luminance threshold (max-channel)
    float knee;          // soft-knee width (threshold * softKnee + eps)
    uint  applyThreshold; // 1 on the first/prefilter downsample, 0 otherwise
    float _pad;
};

[vk::push_constant]
BloomDownsamplePC gPushConstants;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

float luma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Karis weight for partial-average firefly suppression.
float karisWeight(float3 c)
{
    return 1.0f / (1.0f + luma(c));
}

// Unity-style quadratic soft-knee bright-pass (operates on max channel).
float3 prefilter(float3 c)
{
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - gPushConstants.threshold + gPushConstants.knee, 0.0f, 2.0f * gPushConstants.knee);
    rq = (rq * rq) / (4.0f * gPushConstants.knee + 1e-4f);
    float contrib = max(rq, br - gPushConstants.threshold) / max(br, 1e-5f);
    return c * contrib;
}

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    uint w, h;
    sourceInput.GetDimensions(w, h);
    float2 t = 1.0f / float2(float(w), float(h));
    float2 uv = pin.v_uv;

    // 13-tap sample grid (a..m), centered on uv.
    float3 a = sourceInput.Sample(inputSampler, uv + float2(-2, -2) * t).rgb;
    float3 b = sourceInput.Sample(inputSampler, uv + float2( 0, -2) * t).rgb;
    float3 c = sourceInput.Sample(inputSampler, uv + float2( 2, -2) * t).rgb;
    float3 d = sourceInput.Sample(inputSampler, uv + float2(-2,  0) * t).rgb;
    float3 e = sourceInput.Sample(inputSampler, uv + float2( 0,  0) * t).rgb;
    float3 f = sourceInput.Sample(inputSampler, uv + float2( 2,  0) * t).rgb;
    float3 g = sourceInput.Sample(inputSampler, uv + float2(-2,  2) * t).rgb;
    float3 hh = sourceInput.Sample(inputSampler, uv + float2( 0,  2) * t).rgb;
    float3 i = sourceInput.Sample(inputSampler, uv + float2( 2,  2) * t).rgb;
    float3 j = sourceInput.Sample(inputSampler, uv + float2(-1, -1) * t).rgb;
    float3 k = sourceInput.Sample(inputSampler, uv + float2( 1, -1) * t).rgb;
    float3 l = sourceInput.Sample(inputSampler, uv + float2(-1,  1) * t).rgb;
    float3 m = sourceInput.Sample(inputSampler, uv + float2( 1,  1) * t).rgb;

    float3 result;
    if (gPushConstants.applyThreshold != 0u)
    {
        // 5 overlapping 2x2 boxes, Karis-averaged to suppress fireflies.
        float3 b0 = (a + b + d + e) * 0.25f; // top-left
        float3 b1 = (b + c + e + f) * 0.25f; // top-right
        float3 b2 = (d + e + g + hh) * 0.25f; // bottom-left
        float3 b3 = (e + f + hh + i) * 0.25f; // bottom-right
        float3 b4 = (j + k + l + m) * 0.25f; // center
        // Normalize by the summed weights: without it the output is roughly
        // c / (1 + luma(c)), so white sources never reach the threshold while
        // saturated ones do.
        float w0 = 0.125f * karisWeight(b0);
        float w1 = 0.125f * karisWeight(b1);
        float w2 = 0.125f * karisWeight(b2);
        float w3 = 0.125f * karisWeight(b3);
        float w4 = 0.5f   * karisWeight(b4);
        result = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) /
                 max(w0 + w1 + w2 + w3 + w4, 1e-5f);
        result = prefilter(result);
    }
    else
    {
        result  = e * 0.125f;
        result += (a + c + g + i) * 0.03125f;
        result += (b + d + f + hh) * 0.0625f;
        result += (j + k + l + m) * 0.125f;
    }

    return float4(max(result, 0.0f), 1.0f);
}
