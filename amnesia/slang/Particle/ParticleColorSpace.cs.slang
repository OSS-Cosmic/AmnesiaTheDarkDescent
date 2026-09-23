// Temporarily encode the raytraced scene for legacy particle blending.
// One lane loads/stores its own texel; alpha and unclipped HDR headroom survive.
#include "../HostDefinitions.h"
import SceneTypes;
import Utils.Color.ColorHelpers;

[vk::binding(0, 2)]
[format("rgba16f")]
RWTexture2D<float4> gParticleScene;

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
    // Pre-apply the backend-only lift to the background before blending.
    // Undo it on the composite afterward: the final global gamma then leaves
    // forward sources with user gamma alone. Alpha/coverage is never powered.
    // Preserve negative values and HDR headroom through the reversible pair.
    float exponent = restore ? 1.0f / gPushConstants.gammaRatio
                             : gPushConstants.gammaRatio;
    if (exponent != 1.0f)
        display = sign(display) * pow(abs(display), float3(exponent));
    color.rgb = (restore || linearBlend)
        ? sRGBToLinear(display) / kSceneExposure : display;
    gParticleScene[id.xy] = color;
}
