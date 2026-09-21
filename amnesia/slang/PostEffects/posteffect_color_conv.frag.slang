/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's color_conv_posteffect.frag.fsl. Per-channel 1D LUT
// color remap with an alpha-fade blend between the LUT result and the
// untouched source.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
//   binding 2 : Texture2D<float4> colorConv
//
// The LUT is a 256x1 horizontal ramp. cTexture::LoadBitmap creates every
// bitmap as a 2D image with a VK_IMAGE_VIEW_TYPE_2D view (so it stays
// compatible with the bindless textures_2d[] array), so colorConv must be
// sampled as a Texture2D — binding a 2D view into a Texture1D slot fails
// VUID-vkCmdDraw-viewType-07752. The color value drives u; v is the row
// center (0.5) of the single-row LUT.

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture2D<float4> colorConv;

struct ColorConvPC
{
    float alphaFade;
};

[vk::push_constant]
ColorConvPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 diffuseColor = sourceInput.Sample(inputSampler, pin.v_uv).rgb;
    float3 outputColor  = float3(
        colorConv.Sample(inputSampler, float2(diffuseColor.x, 0.5f)).x,
        colorConv.Sample(inputSampler, float2(diffuseColor.y, 0.5f)).y,
        colorConv.Sample(inputSampler, float2(diffuseColor.z, 0.5f)).z);
    return float4(outputColor * gPushConstants.alphaFade
                  + diffuseColor * (1.0f - gPushConstants.alphaFade),
                  1.0f);
}
