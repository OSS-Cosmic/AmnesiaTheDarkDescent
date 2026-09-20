/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's radial_blur_posteffect.frag.fsl. Five-tap radial blur
// from the screen-center outward; intensity ramps with distance from center
// past `blurStartDist` (in half-screen-width units).
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct RadialBlurPC
{
    float  size;
    float  blurStartDist;
    float2 screenDim;
};

[vk::push_constant]
RadialBlurPC gPushConstants;

struct PSIn
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
};

static float3 SampleTextureBlur(float2 viewTexel, float2 screenCoord,
                                 float2 dir, float sizeMul, float colorMul)
{
    float2 pos = (screenCoord + dir * sizeMul) * viewTexel;
    return sourceInput.Sample(inputSampler, pos).rgb * colorMul;
}

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float2 viewTexel      = 1.0f / gPushConstants.screenDim;
    float2 halfScreenSize = gPushConstants.screenDim * 0.5f;
    float2 screenCoord    = pin.v_uv * gPushConstants.screenDim;

    float2 vDir = halfScreenSize - screenCoord;
    float  fDist = length(vDir) / halfScreenSize.x;
    vDir = normalize(vDir);
    fDist = max(0.0f, fDist - gPushConstants.blurStartDist);

    vDir *= fDist * gPushConstants.size * gPushConstants.screenDim.x;
    float3 vDiffuseColor = float3(0.0f);
    vDiffuseColor += SampleTextureBlur(viewTexel, screenCoord, vDir, -1.0f, 0.1f);
    vDiffuseColor += SampleTextureBlur(viewTexel, screenCoord, vDir, -0.5f, 0.2f);
    vDiffuseColor += SampleTextureBlur(viewTexel, screenCoord, vDir,  0.0f, 0.5f);
    vDiffuseColor += SampleTextureBlur(viewTexel, screenCoord, vDir,  0.5f, 0.2f);
    vDiffuseColor += SampleTextureBlur(viewTexel, screenCoord, vDir,  1.0f, 0.1f);
    vDiffuseColor /= 1.1f;
    return float4(vDiffuseColor, 1.0f);
}
