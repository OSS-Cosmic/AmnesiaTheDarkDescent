/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Fullscreen composite for cLuxEffectRenderer's hover outline: samples the
// blurred glow scratch target and emits it for an RGB-only additive blend
// into the pogo read half (the final lit image). Alpha 0 keeps the scene
// alpha intact. Pairs with the shared posteffect_fullscreen.vert.slang.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> blurInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> blurInput;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 c = blurInput.Sample(inputSampler, pin.v_uv).rgb;
    return float4(c, 0.0f);
}
