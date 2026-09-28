/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// AMFP infection composite.  The CPU side advances the amp-map pair and
// supplies the current infection level, while the gradient map determines
// which parts of the composite have grown in.  The blend maps are the three
// authored layers used by the original effect: normal, overlay, and colour
// dodge.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
//   binding 2 : Texture2D<float4> ampMap0
//   binding 3 : Texture2D<float4> ampMap1
//   binding 4 : Texture2D<float4> zoomMap
//   binding 5 : Texture2D<float4> normalBlendMap
//   binding 6 : Texture2D<float4> overlayBlendMap
//   binding 7 : Texture2D<float4> colorDodgeBlendMap
//   binding 8 : Texture2D<float4> gradientMap

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

[vk::binding(5, 0)]
Texture2D<float4> normalBlendMap;

[vk::binding(6, 0)]
Texture2D<float4> overlayBlendMap;

[vk::binding(7, 0)]
Texture2D<float4> colorDodgeBlendMap;

[vk::binding(8, 0)]
Texture2D<float4> gradientMap;

struct InfectionPushConstants
{
    float alpha;
    float time;
    float screenSize[2];
    float ampT;
    float waveAlpha;
    float zoomAlpha;
    float infectionFactor;
    float gradientThresholdOffset;
    float gradientFallofExponent;
    float infectionMapZoom;
    float vomitBlendFactor;
};

[vk::push_constant]
InfectionPushConstants gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

float3 overlayBlend(float3 base, float3 blend)
{
    return lerp(2.0f * base * blend,
                1.0f - 2.0f * (1.0f - base) * (1.0f - blend),
                step(0.5f, blend));
}

float3 colorDodgeBlend(float3 base, float3 blend)
{
    return saturate(base / max(1.0f - blend, 0.001f));
}

float2 infectionMapUV(float2 uv)
{
    // The map is authored around the centre of the screen.  A value of one
    // leaves it unchanged; the configured 0.95 gives the intended slight
    // zoom without introducing a divide-by-zero path for default configs.
    return (uv - 0.5f) * gPushConstants.infectionMapZoom + 0.5f;
}

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float2 uv = pin.v_uv;
    float2 screenSize = max(float2(gPushConstants.screenSize[0],
                                   gPushConstants.screenSize[1]),
                            float2(1.0f, 1.0f));

    // Match the legacy infection animation: interpolate adjacent amplitude
    // maps, apply the radial zoom map, then add animated sine-wave distortion.
    float3 amp0 = ampMap0.Sample(inputSampler, uv).rgb;
    float3 amp1 = ampMap1.Sample(inputSampler, uv).rgb;
    float3 amp = lerp(amp0, amp1, saturate(gPushConstants.ampT));

    float2 pixel = uv * screenSize;
    float3 zoom = zoomMap.Sample(inputSampler, uv).rgb;
    pixel += (zoom.xy - 0.5f) * 2.0f * 0.6f * zoom.z
             * screenSize.y * gPushConstants.zoomAlpha;

    float2 wave = (pixel / screenSize.y) * 0.6f;
    float2 waveAmplitude = gPushConstants.waveAlpha * 0.04f
                           * screenSize.y * amp.xy;
    pixel.x += sin(gPushConstants.time + wave.y) * waveAmplitude.x;
    pixel.y += sin(gPushConstants.time + wave.x * 1.83f) * waveAmplitude.y;

    float2 distortedUV = pixel / screenSize;
    float3 source = sourceInput.Sample(inputSampler, distortedUV).rgb;

    float2 mapUV = infectionMapUV(distortedUV);
    float gradient = gradientMap.Sample(inputSampler, mapUV).r;

    // Infection grows wherever the animated level overtakes the authored
    // gradient.  The threshold offset and falloff exponent are intentionally
    // left as direct config controls; a non-positive exponent is treated as
    // a linear falloff so an omitted config cannot produce undefined output.
    float threshold = max(gPushConstants.gradientThresholdOffset, 0.0f);
    float gradientAmount = saturate((gPushConstants.infectionFactor - gradient)
                                    * threshold);
    float falloff = max(gPushConstants.gradientFallofExponent, 1.0f);
    float infectionMask = pow(gradientAmount, falloff);

    float4 normalLayer = normalBlendMap.Sample(inputSampler, mapUV);
    float4 overlayLayer = overlayBlendMap.Sample(inputSampler, mapUV);
    float4 dodgeLayer = colorDodgeBlendMap.Sample(inputSampler, mapUV);

    // Preserve each texture's authored alpha as its layer opacity.  This also
    // makes missing-map fallbacks benign because the CPU binds sourceInput for
    // unavailable resources.
    float3 composite = lerp(source, normalLayer.rgb, normalLayer.a);
    float3 overlaid = overlayBlend(composite, overlayLayer.rgb);
    composite = lerp(composite, overlaid, overlayLayer.a);
    float3 dodged = colorDodgeBlend(composite, dodgeLayer.rgb);
    composite = lerp(composite, dodged, dodgeLayer.a);

    // Vomit has no separate texture in the current resource set.  Its legacy
    // factor therefore reveals the same authored infection composite, while
    // infectionMask continues to control the normal infection progression.
    float effectAmount = saturate(infectionMask
                                 + gPushConstants.vomitBlendFactor
                                 * (1.0f - infectionMask));
    float3 outputColor = lerp(source, composite, effectAmount);
    return float4(lerp(source, outputColor, saturate(gPushConstants.alpha)), 1.0f);
}
