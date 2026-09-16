// Reinterleave quarter-resolution ambient occlusion to full resolution.
// Input is a 16-slice 2D array texture (each quarter-res pixel has 16 samples
// representing a 4x4 sub-region of full-resolution). Outputs full-resolution AO.

#include "../HostDefinitions.h"
#include "../StandardAo.h"
import StandardAmbientOcclusion;
import PerFrame.resource;

// Positive linear view depth, same layout as the AO array. Used as the
// bilateral guide so the resolve averages within a surface and not across a
// silhouette.
[vk::binding(12, 2)]
RWTexture2DArray<float> aoPreparedDepthTexture;

// Read as a storage image: it stays in GENERAL from the coarse pass.
[vk::binding(13, 2)]
RWTexture2DArray<float> aoQuarterTexture;

[vk::binding(14, 2)]
RWTexture2D<float> aoTexture;

static const float POWER_EXP = 1.0f;

// Resolve footprint, in full-resolution pixels either side of the centre. The
// deinterleave gives each of the 16 pixels in a 4x4 block its own sample
// rotation, so the noise has a period of one block; a radius of 2 covers a full
// period on both axes and averages the rotations out. A smaller radius leaves
// visible speckle, a larger one costs taps for little extra smoothing.
static const int RESOLVE_RADIUS = 2;

// How far apart two view depths may be, as a fraction of the centre depth,
// before they stop being treated as the same surface. Relative rather than
// absolute because view depth is linear: a fixed epsilon that behaves at one
// metre bleeds across silhouettes at twenty.
static const float DEPTH_TOLERANCE = 0.02f;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 dtid : SV_DispatchThreadID)
{
    // dtid.z is the slice index (0..15)
    // One shared mapping for all three passes; see StandardAo.h.
    uint2 fullResPix = standardAoSlicePixel(dtid.xy, dtid.z);

    // Guard: if full-res pixel >= viewportSize, return
    // All pixels should be covered because of ceil extents (unlike enhanced floor)
    if (fullResPix.x >= gPerFrame.viewportSize.x || 
        fullResPix.y >= gPerFrame.viewportSize.y)
        return;

    // Cross-bilateral resolve of the deinterleave.
    //
    // Each full-resolution neighbour lives in its own (quarter texel, slice)
    // pair, which the shared mapping resolves exactly -- that is what makes this
    // a real neighbourhood rather than a walk within one slice. Averaging over a
    // block's worth of pixels is what turns the per-slice rotations from visible
    // structure into a smooth term; without it the jitter goes into lighting raw.
    const float centerDepth = aoPreparedDepthTexture[uint3(dtid.xy, dtid.z)];
    float aoSum = 0.0f;
    float weightSum = 0.0f;
    for (int dy = -RESOLVE_RADIUS; dy <= RESOLVE_RADIUS; ++dy)
    {
        for (int dx = -RESOLVE_RADIUS; dx <= RESOLVE_RADIUS; ++dx)
        {
            int2 tap = int2(fullResPix) + int2(dx, dy);
            tap = clamp(tap, int2(0, 0),
                        int2(gPerFrame.viewportSize.xy) - int2(1, 1));
            uint2 tapPos = uint2(tap);
            uint2 tapQuarter = standardAoPixelQuarter(tapPos);
            uint tapSlice = standardAoPixelSlice(tapPos);

            float tapDepth = aoPreparedDepthTexture[uint3(tapQuarter, tapSlice)];
            // Same surface? Scale the tolerance by depth so it holds at any
            // distance, and drop the tap entirely once it is clearly elsewhere.
            float allowed = max(centerDepth, 1e-4f) * DEPTH_TOLERANCE;
            float delta = abs(tapDepth - centerDepth);
            float weight = saturate(1.0f - delta / max(allowed, 1e-6f));
            if (weight <= 0.0f)
                continue;

            aoSum += aoQuarterTexture[uint3(tapQuarter, tapSlice)] * weight;
            weightSum += weight;
        }
    }

    // weightSum always includes the centre tap, but guard anyway: a NaN depth
    // would otherwise turn the pixel black instead of leaving it unoccluded.
    float ao = weightSum > 0.0f ? (aoSum / weightSum)
                                : aoQuarterTexture[uint3(dtid.xy, dtid.z)];

    // Compute final: pow(saturate(ao), POWER_EXP)
    float final = pow(saturate(ao), POWER_EXP);

    // Write to aoTexture
    aoTexture[fullResPix] = final;
}
