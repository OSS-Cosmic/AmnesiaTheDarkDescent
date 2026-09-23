/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's blur_posteffect.frag.fsl. 5-tap separable blur — the
// host runs this twice per iteration with `blurDir = (size, 0)` then
// `blurDir = (0, size)` to get horizontal + vertical passes.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
//
// Push constant: 8 bytes, blur direction (texel-scaled before sampling).

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct BloomBlurPC
{
    float2 blurDir;
    float2 _pad;
};

[vk::push_constant]
BloomBlurPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    uint w, h;
    sourceInput.GetDimensions(w, h);
    float2 texel = 1.0f / float2(float(w), float(h));

    static const float vMul[5]    = { 0.25f, 0.30f, 0.50f, 0.30f, 0.25f };
    static const float fOffset[5] = { -2.5f, -0.75f, 0.0f, 0.75f, 2.5f  };
    const float mulSum = 1.6f; // sum of vMul[]

    float3 amount = float3(0.0f);
    for (int i = 0; i < 5; ++i)
    {
        float2 offset = (float2(fOffset[i], fOffset[i]) * texel) * gPushConstants.blurDir;
        amount += sourceInput.Sample(inputSampler, pin.v_uv + offset).rgb
                  * vMul[i];
    }
    amount /= mulSum;
    return float4(amount, 1.0f);
}
