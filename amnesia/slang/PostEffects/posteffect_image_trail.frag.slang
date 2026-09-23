/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's image_trail_posteffect.frag.fsl. Outputs the source
// color with an alpha derived from the per-frame fade so it can be blended
// (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) into a persistent accumulation target on
// the host side.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct ImageTrailPC
{
    float alpha;
};

[vk::push_constant]
ImageTrailPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 color = sourceInput.Sample(inputSampler, pin.v_uv).rgb;
    return float4(color, gPushConstants.alpha);
}
