/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Trivial 1-tap blit. Used as the tail pass that copies the last pogo
// attachment into the swapchain image after the post-effect chain.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    return float4(sourceInput.Sample(inputSampler, pin.v_uv).rgb, 1.0f);
}
