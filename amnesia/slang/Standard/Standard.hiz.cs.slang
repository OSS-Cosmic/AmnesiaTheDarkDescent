// Depth pyramid for the Standard renderer's camera occlusion cull.
//
// Level 0 is half the render target; each further level is a 2x2 reduce of the
// one before. The reduction operator is MAX -- the FARTHEST depth in the block,
// under the standard-Z convention this renderer rasterizes with (depth cleared
// to 1.0, near at 0; see the depth clear in cStandardRenderer::Draw).
//
// Max is the only operator that makes the occlusion test a proof. The consumer
// asks "is the nearest point of this box behind everything drawn in its screen
// rect?", so the pyramid has to report the farthest thing drawn, not the
// nearest. Reducing with min instead yields a pyramid that culls geometry which
// is actually visible -- silently, and only where an occluder has a hole.
//
// Odd source extents are handled by widening the gather to 3x3 rather than
// dropping the tail: a missed texel can only make the stored depth too NEAR,
// which is exactly the direction that over-culls.
//
// Resources (set 2; names resolved by host reflection):
//   gHiZDepth   in   full-resolution scene depth, used only for level 0
//   gHiZSource  in   the whole pyramid; the level being read is selected by
//                    sourceMip on Load, so no per-level SRV is needed
//   gHiZTarget  out  a single-level storage view of the level being written

#include "../HostDefinitions.h"

[vk::binding(0, 2)] Texture2D<float>   gHiZDepth;
[vk::binding(1, 2)] Texture2D<float>   gHiZSource;
[vk::binding(2, 2)] RWTexture2D<float> gHiZTarget;

struct StandardHiZPC {
    uint2 targetExtent;
    uint2 sourceExtent;
    uint  sourceMip;
    uint  sourceIsDepth;  // 1 for level 0, which reads gHiZDepth instead
    uint  pad0;
    uint  pad1;
};

[vk::push_constant]
StandardHiZPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

float loadSource(uint2 coordinate)
{
    uint2 clamped = min(coordinate, gPushConstants.sourceExtent - uint2(1u, 1u));
    if (gPushConstants.sourceIsDepth != 0u) {
        return gHiZDepth.Load(int3(int2(clamped), 0));
    }
    return gHiZSource.Load(int3(int2(clamped), int(gPushConstants.sourceMip)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hizReduce(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 target = dispatchThreadID.xy;
    if (target.x >= gPushConstants.targetExtent.x || target.y >= gPushConstants.targetExtent.y) {
        return;
    }

    uint2 base = target * 2u;
    float farthest = loadSource(base);
    farthest = max(farthest, loadSource(base + uint2(1u, 0u)));
    farthest = max(farthest, loadSource(base + uint2(0u, 1u)));
    farthest = max(farthest, loadSource(base + uint2(1u, 1u)));

    // An odd source leaves one row/column unowned by any 2x2 block. Fold it
    // into the last block instead of losing it.
    if ((gPushConstants.sourceExtent.x & 1u) != 0u) {
        farthest = max(farthest, loadSource(base + uint2(2u, 0u)));
        farthest = max(farthest, loadSource(base + uint2(2u, 1u)));
    }
    if ((gPushConstants.sourceExtent.y & 1u) != 0u) {
        farthest = max(farthest, loadSource(base + uint2(0u, 2u)));
        farthest = max(farthest, loadSource(base + uint2(1u, 2u)));
    }
    if ((gPushConstants.sourceExtent.x & 1u) != 0u && (gPushConstants.sourceExtent.y & 1u) != 0u) {
        farthest = max(farthest, loadSource(base + uint2(2u, 2u)));
    }

    gHiZTarget[target] = farthest;
}
