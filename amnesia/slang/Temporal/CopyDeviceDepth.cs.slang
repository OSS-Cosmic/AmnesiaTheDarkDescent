// Copy the raw device depth value into a standalone R32_SFLOAT image.
// The input is a depth-aspect-only sampled view of the scene depth image.

#include "../HostDefinitions.h"

[vk::binding(0, 2)]
Texture2D<float> gDeviceDepth;

[vk::binding(1, 2)]
RWTexture2D<float> gCopiedDepth;

struct CopyDeviceDepthPC
{
    uint2 gExtent; // render extent
};

[vk::push_constant]
CopyDeviceDepthPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= gPushConstants.gExtent.x || p.y >= gPushConstants.gExtent.y)
        return;

    // Direct integer-coordinate load/store preserves the non-linear device
    // depth value without filtering, scaling, or linearization.
    gCopiedDepth[p] = gDeviceDepth.Load(int3(p, 0));
}
