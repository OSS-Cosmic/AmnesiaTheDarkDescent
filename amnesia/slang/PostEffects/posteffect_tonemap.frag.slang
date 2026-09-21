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
// ACES was previously removed because it crushed low-mids of a scene whose
// diffuse was capped at ≤1; with PBR lights pushing values >1 the filmic
// shoulder is wanted again. Tune `exposure` if mids read dim.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
// Push constant: exposure (scene-linear multiply, sourced from kSceneExposure);
// shadowLift unused (layout); gamma (user display-gamma setting, 1.0 = exact
// identity) applied as the final encode step — replaces the base game's SDL
// window-brightness ramp, which was identity at 1.0 too; shoulder (1 = roll
// highlights off above kToneMapShoulder, 0 = clip at display white like the
// base game's 8-bit buffer; the Standard backend uses 0).
//
// Brightness comes from `exposure`, NOT from gamma. The Standard particle/
// translucent path (BlendModes.slang encodeStandardBlendSource) emits
// sRGBToLinear(authoredDisplayColor) / kSceneExposure on the assumption that
// this pass emits exactly linearToSRGB(linear * exposure) — an inverse pair. A display-gamma curve breaks that identity and lifts faint content
// hardest, which reads as milky fog and blown-out halos; a linear exposure
// multiply preserves every blend ratio.

#include "../HostDefinitions.h"

import Utils.Color.ColorHelpers;   // piecewise linearToSRGB()

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

// Stephen Hill's "ACES fitted" RRT+ODT(sRGB): treat the linear Rec.709 scene as
// ACEScg working space, apply the tone curve, map back to linear sRGB. Output is
// linear [0,1]; the sRGB OETF is applied separately (linearToSRGB) for the UNORM
// swapchain. Matrices are HLSL row-major, used as mul(matrix, vector).
static const float3x3 ACESInputMat = {
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f },
};
static const float3x3 ACESOutputMat = {
    {  1.60475f, -0.53108f, -0.07367f },
    { -0.10208f,  1.10813f, -0.00605f },
    { -0.00327f, -0.07276f,  1.07602f },
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);        // linear sRGB [0,1]
}

struct ToneMapPC
{
    float exposure;
    float shadowLift;
    float gamma;     // user display-gamma (1.0 = identity)
    float shoulder;  // 1 = soft highlight shoulder, 0 = clip (Standard)
};

[vk::push_constant]
ToneMapPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

// Highlight shoulder knee. Below it the encode is bit-exact, so the particle
// path's sRGBToLinear/linearToSRGB round trip stays an identity for the faint
// range those effects live in (see BlendModes.slang). Above it, a Reinhard
// shoulder with unit slope at the knee, asymptotic to 1.0 — the output must
// stay bounded because the image trail's fp16 EMA accumulator does not clamp
// (posteffect_image_trail.frag.slang:33-36).
static const float kToneMapShoulder = 0.8f;

// Per channel, so a channel that reaches the shoulder first pulls the highlight
// toward white — the usual filmic desaturation at the top, and the one place
// this pass deliberately departs from the authored color.
float3 softShoulder(float3 x)
{
    const float k = kToneMapShoulder;
    const float r = 1.0f - k;
    float3 over = max(x - k, 0.0f);
    return min(x, k) + r * over / (r + over);
}

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 hdr = sourceInput.Sample(inputSampler, pin.v_uv).rgb * gPushConstants.exposure;
    // Display-encoded sRGB. With the shoulder it rolls off into [0,1]; without
    // it (Standard) values clip at display white exactly like the base game,
    // so authored display terms below 1 come back bit-exact. The saturate keeps
    // the output bounded for the display-space effects that follow.
    float3 display = max(linearToSRGB(hdr), 0.0f);
    if (gPushConstants.shoulder != 0.0f)
        display = softShoulder(display);
    display = saturate(display);
    // User gamma adjust (final): gamma 1.0 = identity, <1 darker / >1 brighter.
    display = pow(display, (1.0f / max(gPushConstants.gamma, 1e-4f)).xxx);
    return float4(display, 1.0f);
}
