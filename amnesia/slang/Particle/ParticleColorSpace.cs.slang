// Temporarily encode the raytraced scene for legacy particle blending.
// One lane loads/stores its own texel; alpha and unclipped HDR headroom survive.
#include "../HostDefinitions.h"
#include "../RtForwardColor.h"
import SceneTypes;
import Utils.Color.ColorHelpers;

[vk::binding(0, 2)]
[format("rgba16f")]
RWTexture2D<float4> gParticleScene;
// Display-space scene as the bracket opened; written on encode, read on
// restore to emulate the legacy 8-bit clip (rtForwardColorClip).
[vk::binding(1, 2)]
[format("rgba16f")]
RWTexture2D<float4> gParticleSceneBase;

struct ParticleColorSpacePush
{
    uint toLinear;
    uint linearBlend;
    float gammaRatio; // userGamma / (userGamma + backendBias), or 1 without tone map
};
[vk::push_constant]
ConstantBuffer<ParticleColorSpacePush> gPushConstants HPL_PUSH_CONSTANT_REGISTER;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    gParticleScene.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    float4 color = gParticleScene[id.xy];
    bool restore = gPushConstants.toLinear != 0u;
    bool linearBlend = gPushConstants.linearBlend != 0u;
    float3 display = (!restore || linearBlend)
        ? linearToSRGB(color.rgb * kSceneExposure) : color.rgb;
    // Clip only what the forward passes added, in the same lifted space they
    // blended in, before the lift is undone.
    if (restore)
        display = rtForwardColorClip(display, gParticleSceneBase[id.xy].rgb);
    // Pre-apply the backend-only lift to the background before blending.
    // Undo it on the composite afterward: the final global peak gamma then
    // leaves forward sources with user peak gamma alone. Match its RGB-ratio
    // preserving operator in BOTH directions, including the linear-blend path.
    // Alpha/coverage is never powered.
    // Preserve negative values and HDR headroom through the reversible pair.
    float exponent = restore ? 1.0f / gPushConstants.gammaRatio
                             : gPushConstants.gammaRatio;
    display = rtForwardColorGamma(display, exponent);
    if (!restore)
        gParticleSceneBase[id.xy] = float4(display, 0.0f);
    color.rgb = (restore || linearBlend)
        ? sRGBToLinear(display) / kSceneExposure : display;
    gParticleScene[id.xy] = color;
}
