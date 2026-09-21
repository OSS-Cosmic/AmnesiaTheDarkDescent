/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0

// Final display delivery only. Keep the history-copy blit free of noise.

#include "../HostDefinitions.h"

[vk::binding(0, 0)] SamplerState inputSampler;
[vk::binding(1, 0)] Texture2D<float4> sourceInput;

struct PresentConstants
{
    float4 quantizationStep;
};
[[vk::push_constant]] ConstantBuffer<PresentConstants> gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    float4 position : SV_Position;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 color = sourceInput.Sample(inputSampler, pin.v_uv).rgb;
    // Stable interleaved gradient noise, shared by RGB to avoid colored
    // speckles. Half a code value decorrelates rounding without changing
    // exposure. Pixel coordinates keep it at output resolution after upscale.
    float noise = frac(52.9829189 * frac(dot(floor(pin.position.xy),
                                          float2(0.06711056, 0.00583715)))) - 0.5;
    // Stay just inside half a step so exact black/white cannot round away
    // from their endpoint under round-to-even conversion.
    return float4(color + (noise * 0.999) * gPushConstants.quantizationStep.rgb, 1.0f);
}
