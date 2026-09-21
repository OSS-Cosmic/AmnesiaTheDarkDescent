/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's dds_insanity_posteffect.frag.fsl. Screen distortion shown
// when the player goes insane: animated sine waves (amplitude from two blended
// amp maps) plus a radial zoom warp (from a zoom map). UV-distortion only — no
// color/brightness operation. At waveAlpha == zoomAlpha == 0 it is an exact
// passthrough of the source, which is what keeps the post-effect composite's pogo
// ping-pong valid (this pass MUST write its output half every frame).
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput   (prior pogo half = tonemapped scene)
//   binding 2 : Texture2D<float4> ampMap0
//   binding 3 : Texture2D<float4> ampMap1
//   binding 4 : Texture2D<float4> zoomMap

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture2D<float4> ampMap0;

[vk::binding(3, 0)]
Texture2D<float4> ampMap1;

[vk::binding(4, 0)]
Texture2D<float4> zoomMap;

struct InsanityPC
{
    float time;        // wave animation phase (mfT)
    float amplitude;   // blend between ampMap0 and ampMap1 (frac of anim counter)
    float waveAlpha;   // sine-wave distortion strength
    float zoomAlpha;   // radial zoom-warp strength
};

[vk::push_constant]
InsanityPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float2 uv01 = pin.v_uv;
    uint w, h;
    sourceInput.GetDimensions(w, h);
    float2 size = float2(float(w), float(h));

    // Amplitude map: blend the two animated maps, scale to a pixel-space sine
    // amplitude (the 0.04 * screenHeight factor matches the legacy .fsl).
    float3 a0 = ampMap0.Sample(inputSampler, uv01).rgb;
    float3 a1 = ampMap1.Sample(inputSampler, uv01).rgb;
    float3 amp = a0 * (1.0f - gPushConstants.amplitude) + a1 * gPushConstants.amplitude;
    float2 ampDir = (gPushConstants.waveAlpha * 0.04f * size.y) * amp.xy;

    // Radial zoom warp: zoom.xy is a direction from center (0.5), zoom.z magnitude.
    float3 zoom = zoomMap.Sample(inputSampler, uv01).rgb;
    float2 pix = uv01 * size;
    pix += (zoom.xy - 0.5f) * 2.0f * 0.6f * zoom.z * size.y * gPushConstants.zoomAlpha;

    // Animated sine distortion, phase offset per axis (1.83 matches the legacy).
    float2 s = (pix / size.y) * 0.6f;
    pix.x += sin(gPushConstants.time + s.y) * ampDir.x;
    pix.y += sin(gPushConstants.time + s.x * 1.83f) * ampDir.y;

    return float4(sourceInput.Sample(inputSampler, pix / size).rgb, 1.0f);
}
