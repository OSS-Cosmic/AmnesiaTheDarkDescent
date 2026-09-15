// Prepare depth values for ambient occlusion by sampling position texture
// and converting to view-space depth. Outputs a 2D array where each layer
// represents a 4x4 region of the full-resolution image, packed into 16 slices.

#include "../HostDefinitions.h"
import StandardAmbientOcclusion;
import PerFrame.resource;

[vk::binding(1, 2)]
Texture2D<float4> positionTexture;

[vk::binding(12, 2)]
RWTexture2DArray<float> aoPreparedDepthTexture;

struct AOPrepareDepthsPC {
    uint2 quarterDim;
};

[vk::push_constant]
AOPrepareDepthsPC pc;

// NOTE: Device depth linearization is NOT applied here, unlike enhanced OpenGL.
// The output is raw view-space linear depth.

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 dtid : SV_DispatchThreadID)
{
    // Guard against out-of-bounds threads
    if (dtid.x >= pc.quarterDim.x || dtid.y >= pc.quarterDim.y)
        return;

    // Process each of the 16 slices in the 4x4 block
    for (uint s = 0u; s < 16u; ++s)
    {
        // Map slice index to 4x4 offset: s%4 is column, s/4 is row
        uint2 offset = uint2(s % 4u, s / 4u);
        uint2 fullResPix = dtid.xy * 4u + offset;

        // Load position from texture
        float4 pos = positionTexture.Load(int3(fullResPix, 0));

        // Compute view-space depth: -(viewMat * pos).z
        // pos.w <= 0 indicates invalid/out-of-bounds pixel
        float viewDepth;
        if (pos.w <= 0.0f)
        {
            // Invalid position or out-of-bounds; use far plane (large positive depth)
            viewDepth = 1e6f;
        }
        else
        {
            // Transform world position to view space and extract Z
            float4 viewPos = mul(gPerFrame.viewMat, pos);
            viewDepth = -viewPos.z;
        }

        // Store in the corresponding slice of the output texture array
        aoPreparedDepthTexture[uint3(dtid.xy, s)] = viewDepth;
    }
}
