// Reinterleave quarter-resolution ambient occlusion to full resolution.
// Input is a 16-slice 2D array texture (each quarter-res pixel has 16 samples
// representing a 4x4 sub-region of full-resolution). Outputs full-resolution AO.

#include "../HostDefinitions.h"
import StandardAmbientOcclusion;
import PerFrame.resource;

// Read as a storage image: it stays in GENERAL from the coarse pass.
[vk::binding(13, 2)]
RWTexture2DArray<float> aoQuarterTexture;

[vk::binding(14, 2)]
RWTexture2D<float> aoTexture;

static const float POWER_EXP = 1.0f;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 dtid : SV_DispatchThreadID)
{
    // dtid.z is the slice index (0..15)
    // Map slice to full-resolution pixel: dtid.xy*4 + (dtid.z%4, dtid.z/4)
    uint2 fullResPix = dtid.xy * 4u + uint2(dtid.z % 4u, dtid.z / 4u);

    // Guard: if full-res pixel >= viewportSize, return
    // All pixels should be covered because of ceil extents (unlike enhanced floor)
    if (fullResPix.x >= gPerFrame.viewportSize.x || 
        fullResPix.y >= gPerFrame.viewportSize.y)
        return;

    // Load ao value from aoQuarterTexture
    float ao = aoQuarterTexture[uint3(dtid.xy, dtid.z)];

    // Compute final: pow(saturate(ao), POWER_EXP)
    float final = pow(saturate(ao), POWER_EXP);

    // Write to aoTexture
    aoTexture[fullResPix] = final;
}
