/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Output-encode pass. Runs after every HDR-domain effect in the chain: takes
// the linear-HDR pogo, applies `exposure`, then sRGB-encodes into the pogo,
// clamped to a normalized [0,1] — display-space effects that run after it
// (the image trail's persistent fp16 EMA accumulator) rely on the output
// being bounded and finite. The tail blit copies this straight to the
// UNORM swapchain, which performs NO hardware OETF — so the encode MUST happen
// here. This brings the scene in line with the GUI, which is authored to write
// display-encoded values directly to the same UNORM swapchain.
//
// Both backends: exposure multiply, sRGB encode, clip at display white, user
// gamma, optional chroma scale. For Standard this is the exact inverse of its
// blend paths, so the base game's look is unchanged; the ray-traced backend
// only differs by its extra exposure (kRayTracedExposure, folded into
// `exposure` host-side) and saturation. Tune kRayTracedExposure if mids read
// dim.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
// Push constant: exposure (scene-linear multiply, sourced from kSceneExposure);
// shadowLift unused (layout); gamma (user display-gamma setting, 1.0 = exact
// identity) applied as the final encode step — replaces the base game's SDL
// window-brightness ramp, which was identity at 1.0 too; saturation (chroma
// scale toward Rec.709 luma, sourced from kRayTracedSaturation; the Standard
// backend uses 1.0, which this pass treats as an exact no-op).
//
// Brightness comes from `exposure`, NOT from gamma. The Standard particle/
// translucent path (BlendModes.slang encodeStandardBlendSource) emits
// sRGBToLinear(authoredDisplayColor) / kSceneExposure on the assumption that
// this pass emits exactly linearToSRGB(linear * exposure) — an inverse pair.
// A display-gamma curve breaks that identity and lifts faint content
// hardest, which reads as milky fog and blown-out halos; a linear exposure
// multiply preserves every blend ratio.

#include "../HostDefinitions.h"

import Utils.Color.ColorHelpers;   // piecewise linearToSRGB(), luminance()

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct ToneMapPC
{
    float exposure;
    float shadowLift;
    float gamma;      // user display-gamma (1.0 = identity)
    float saturation; // 1 = identity (Standard), < 1 = chroma toward luma
};

[vk::push_constant]
ToneMapPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 hdr = sourceInput.Sample(inputSampler, pin.v_uv).rgb * gPushConstants.exposure;
    // Legacy clip and per-channel user gamma.
    float3 display = max(linearToSRGB(hdr), 0.0f);
    display = pow(saturate(display),
                  (1.0f / max(gPushConstants.gamma, 1e-4f)).xxx);
    // Optional chroma scale; 1.0 (Standard, and RT by default) is an exact no-op.
    if (gPushConstants.saturation != 1.0f)
    {
        const float y = luminance(display);
        display = lerp(y.xxx, display, gPushConstants.saturation);
    }
    return float4(saturate(display), 1.0f);
}
