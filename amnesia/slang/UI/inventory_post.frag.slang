/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/inventory_post.frag.
//
// Inventory background post-effect. Remaps RGB through a desaturate-
// towards-green matrix, scales intensity down, then gamma-shapes the
// result to deepen the contrast. Originally ported from
// HPL2/resource/dds_inventory_posteffect.frag.fsl in the reference fork.
//
// Bindings match the GLSL twin:
//   set 0 binding 0 : SamplerState        inputSampler
//   set 0 binding 1 : Texture2D<float4>   sourceInput
//
// Push constant: `strength` crossfades the effect in (0 = untouched scene,
// 1 = full desaturate/darken). The menu drives it from its fade alpha so the
// backdrop dims in over the fade instead of popping. This replaces the old
// two-quad (sharp-under, darkened-over) GUI crossfade now that the effect runs
// live in the viewport post-effect chain.

#include "../HostDefinitions.h"

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct InvPostPC
{
    float strength;
    float3 _pad;
};

[vk::push_constant]
InvPostPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

struct PSOut
{
    [vk::location(0)] float4 out_color : SV_Target0;
};

[shader("fragment")]
PSOut psMain(PSIn pin)
{
    float3 color = sourceInput.Sample(inputSampler, pin.v_uv).rgb;

    const float3 colorToR  = float3(0.39f,  0.769f, 0.189f);
    const float3 colorToG  = float3(0.349f, 0.686f, 0.168f);
    const float3 colorToB  = float3(0.272f, 0.534f, 0.131f);
    const float  intensity = 0.35f;

    float3 mixed = float3(dot(color, colorToR),
                          dot(color, colorToG),
                          dot(color, colorToB)) * intensity;
    float3 desat = pow(mixed, float3(1.5f));

    PSOut o;
    o.out_color = float4(lerp(color, desat, saturate(gPushConstants.strength)), 1.0f);
    return o;
}
