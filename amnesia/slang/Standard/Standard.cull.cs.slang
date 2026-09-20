// GPU culling for the Standard renderer's shadow atlas tiles.
//
// Replaces the per-tile CPU walk the renderer used to do: StandardRenderer
// called rendering::WalkAndPrepareRenderList twice per tile, so a dozen shadow
// lights meant roughly a hundred full linear scans of both renderable sets
// every frame. Now the host gathers one candidate set per frame and this kernel
// does the per-tile frustum test.
//
// The predicates live in StandardCull.h and are shared verbatim with
// HPL2/core/sources/graphics/StandardShadowCull.cpp, which is covered by
// headless tests (tests/graphics/standard_shadow_cull.cpp). The frustum test
// there is checked against MathLib's MvpToPlanes / cFrustum::CheckAabb, so this
// kernel runs the same test the CPU used to run rather than a lookalike.
//
// Resources (set 2; names resolved by host reflection):
//   gShadowCullCandidates  in   one record per (light, caster) pair
//   gShadowCullTiles       in   one record per atlas tile: frustum + ranges
//   gShadowCullGroups      in   one record per 64-candidate chunk of a tile
//   gShadowIndirect        out  VkDrawIndirectCommand, written as uint4
//   gShadowDrawCounts      out  one survivor count per tile

#include "../StandardCull.h"

[vk::binding(0, 2)] StructuredBuffer<StandardCullCandidate> gShadowCullCandidates;
[vk::binding(1, 2)] StructuredBuffer<StandardCullTile>      gShadowCullTiles;
[vk::binding(2, 2)] StructuredBuffer<StandardCullGroup>     gShadowCullGroups;
[vk::binding(3, 2)] RWStructuredBuffer<uint4>               gShadowIndirect;
[vk::binding(4, 2)] RWStructuredBuffer<uint>                gShadowDrawCounts;
[vk::binding(5, 2)] StructuredBuffer<StandardCullCamera>    gCullCameras;
[vk::binding(6, 2)] Texture2D<float>                        gCullHiZ;
// The same buffer as gShadowIndirect, viewed as words. INSTANCE_MASK mode
// rewrites one word of a command the host already built, and instanceCount is
// word 1 of both VkDrawIndirectCommand and VkDrawIndexedIndirectCommand, so a
// word view covers the indexed and non-indexed translucent draws alike.
[vk::binding(7, 2)] RWStructuredBuffer<uint>                gCullIndirectWords;
// Persistent across frames, indexed by StandardCullCandidate::visibilityKey.
// Not a per-frame ring: phase 1 of frame N reads what phase 2 of frame N-1
// wrote. A key collision makes phase 1 draw something hidden (depth rejects it)
// or skip something visible (phase 2 draws it), so it costs efficiency and
// never correctness.
[vk::binding(8, 2)] RWStructuredBuffer<uint>                gCullVisibility;

// Bases are absolute element offsets, not zero: the host hands out tile and
// group ranges from per-frame ring segments, so this Draw's entries sit at an
// arbitrary offset and the neighbouring entries belong to frames still in
// flight. Iterating from 0 would clear another frame's counters.
struct StandardCullPC {
    uint gCullMode;     // kStandardCullModeCompact | kStandardCullModeInPlace
    uint gTileBase;     // first live entry in gShadowCullTiles
    uint gTileCount;    // number of live entries from gTileBase
    uint gGroupBase;    // first live entry in gShadowCullGroups
    uint gGroupCount;   // number of live entries from gGroupBase
    // VISIBILITY_UPDATE only: words from a candidate's phase-1 command to its
    // phase-2 one. Both phases share one candidate array, so the second
    // dispatch reaches its own command by a fixed stride rather than needing a
    // duplicate candidate per phase.
    uint gCommandWordDelta;
    uint gPad1;
    uint gPad2;
};

[vk::push_constant]
StandardCullPC gPushConstants;

// VkDrawIndirectCommand is four consecutive uints under scalar layout, so a
// uint4 is the command. firstInstance carries the object slot, which the vertex
// shader reads back as SV_StartInstanceLocation to index gSceneObjects -- the
// same convention the CPU-filled path used.
// Farthest depth stored under the rect: a 2x2 gather at its corners on the mip
// the projection chose. Mirrors StandardHiZSampleFarthest in
// HPL2/core/sources/graphics/StandardShadowCull.cpp, which the headless tests
// run -- keep the two in step.
float sampleHiZFarthest(StandardCullCamera camera, StandardCullScreenRect rect)
{
    uint mip = min(rect.mip, camera.hiZMipCount - 1u);
    uint mipWidth = max(1u, camera.hiZWidth >> mip);
    uint mipHeight = max(1u, camera.hiZHeight >> mip);

    float us[2] = {rect.minU, rect.maxU};
    float vs[2] = {rect.minV, rect.maxV};
    float farthest = 0.0f;
    for (uint iv = 0u; iv < 2u; ++iv) {
        for (uint iu = 0u; iu < 2u; ++iu) {
            uint x = min(mipWidth - 1u, (uint)(saturate(us[iu]) * (float)mipWidth));
            uint y = min(mipHeight - 1u, (uint)(saturate(vs[iv]) * (float)mipHeight));
            farthest = max(farthest, gCullHiZ.Load(int3(int(x), int(y), int(mip))));
        }
    }
    return farthest;
}

// Occlusion gate. Separate from standardCullKeepCandidate because only the
// camera pass has a pyramid to test against; every shadow tile leaves
// cameraIndex at kStandardCullNoCamera and skips this entirely.
bool cullOccludes(StandardCullTile tile, StandardCullCandidate candidate)
{
    if (tile.cameraIndex == kStandardCullNoCamera)
        return false;
    if ((candidate.cullFlags & kStandardCullFlagNeverOcclude) != 0u)
        return false;

    StandardCullCamera camera = gCullCameras[tile.cameraIndex];
    if (camera.hiZMipCount == 0u)
        return false;

    StandardCullScreenRect rect = standardCullProjectAabb(
        camera,
        candidate.aabbMinX, candidate.aabbMinY, candidate.aabbMinZ,
        candidate.aabbMaxX, candidate.aabbMaxY, candidate.aabbMaxZ);
    return standardCullOccluded(rect, sampleHiZFarthest(camera, rect));
}

uint4 makeDrawCommand(StandardCullCandidate candidate, uint instanceCount)
{
    return uint4(candidate.vertexCount, instanceCount, 0u, candidate.objectSlot);
}

// Zero every live tile's survivor counter before the cull bumps it.
//
// Separate dispatch rather than a "thread 0 clears it" trick inside the cull:
// tiles span multiple groups, so no single group owns a tile's counter, and
// there is no cross-group ordering that would make an in-kernel clear safe.
[shader("compute")]
[numthreads(64, 1, 1)]
void cullShadowResetCounts(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint local = dispatchThreadID.x;
    if (local >= gPushConstants.gTileCount) {
        return;
    }
    StandardCullTile tile = gShadowCullTiles[gPushConstants.gTileBase + local];
    gShadowDrawCounts[tile.countIndex] = 0u;
}

// One group per 64-candidate chunk of one tile.
//
// The (tile, candidate) work list is flattened on the host at group
// granularity, so one dispatch covers every tile of every light with no prefix
// sum and no indirect dispatch, and the group -> tile mapping stays
// deterministic enough for the host reference to reproduce it exactly. A
// dispatch per tile would be ~100 launches of ~60 threads; a 2D grid sized to
// the largest tile would waste the difference on every smaller one.
[shader("compute")]
[numthreads(64, 1, 1)]
void cullShadowTiles(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    if (groupID.x >= gPushConstants.gGroupCount) {
        return;
    }

    StandardCullGroup group = gShadowCullGroups[gPushConstants.gGroupBase + groupID.x];
    StandardCullTile tile = gShadowCullTiles[group.tileIndex];

    uint local = group.candidateOffset + groupThreadID.x;
    if (local >= tile.candidateCount) {
        return;
    }

    StandardCullCandidate candidate = gShadowCullCandidates[tile.candidateBase + local];
    bool frustumKeep = standardCullKeepCandidate(tile, candidate);

    if (gPushConstants.gCullMode == kStandardCullModeVisibilityReplay) {
        // FRUSTUM ONLY, deliberately. The pyramid still holds the last frame
        // that used this image, and phase 2 subtracts `wasVisible` -- the
        // STORED bit, not what phase 1 actually drew. If an occlusion test
        // could suppress a draw here, an object with wasVisible = 1 that this
        // phase skipped would be subtracted out of phase 2 as well and never
        // drawn at all. Keeping phase 1 purely frustum-gated is what makes
        // "phase 1 drew exactly frustumKeep && wasVisible" true, and that
        // identity is the whole basis of the subtraction.
        uint wasVisible = gCullVisibility[candidate.visibilityKey];
        gCullIndirectWords[candidate.commandWordOffset] =
            standardCullReplayInstanceCount(frustumKeep, wasVisible);
        return;
    }

    bool keep = frustumKeep && !cullOccludes(tile, candidate);

    if (gPushConstants.gCullMode == kStandardCullModeVisibilityUpdate) {
        // `keep` already carries the occlusion test against the pyramid phase 1
        // filled. Phase 2 draws what is visible now MINUS what phase 1 already
        // drew, so nothing is rasterized twice.
        uint wasVisible = gCullVisibility[candidate.visibilityKey];
        gCullIndirectWords[candidate.commandWordOffset + gPushConstants.gCommandWordDelta] =
            standardCullUpdateInstanceCount(keep, wasVisible);
        // The answer for next frame's phase 1. Written after the read above,
        // and each candidate owns its own key, so there is no ordering hazard
        // beyond a collision -- which is already benign.
        gCullVisibility[candidate.visibilityKey] =
            standardCullNextVisibility(keep);
        return;
    }

    if (gPushConstants.gCullMode == kStandardCullModeInstanceMask) {
        // The host already wrote the whole command; only its instanceCount is
        // ours. Slot order is the host's sort order, untouched, which is what
        // the back-to-front translucent pass needs.
        gCullIndirectWords[candidate.commandWordOffset] = keep ? 1u : 0u;
        return;
    }

    if (gPushConstants.gCullMode == kStandardCullModeInPlace) {
        // Every candidate owns a fixed slot and culled ones become a no-op
        // draw, which keeps the command count known to the host. Used when the
        // device has no vkCmdDrawIndirectCount.
        gShadowIndirect[tile.indirectBase + local] =
            makeDrawCommand(candidate, keep ? 1u : 0u);
        return;
    }

    if (!keep) {
        return;
    }

    // Compact: survivors pack to the front of the tile's range and the host
    // draws exactly that many. Draw ORDER within a tile is not deterministic,
    // which is safe here and only here: the shadow pass is depth-only with
    // LESS_OR_EQUAL, no colour attachment and no blending, so the resulting
    // depth image is order-independent and ties write the same value.
    uint slot;
    InterlockedAdd(gShadowDrawCounts[tile.countIndex], 1u, slot);

    // Cannot fire: the host reserves candidateCount elements for this tile, so
    // the counter cannot reach it. Kept because overrunning the reservation
    // would corrupt a neighbouring tile's commands rather than fail visibly,
    // and the host reference asserts this branch is unreachable.
    if (slot >= tile.candidateCount) {
        return;
    }

    gShadowIndirect[tile.indirectBase + slot] = makeDrawCommand(candidate, 1u);
}
