#pragma once

#include "HostDefinitions.h"

// Shared host/device culling records and predicates for the Standard renderer.
//
// The GPU cull kernel (Standard.cull.cs.slang) and the headless C++ reference
// (HPL2/core/sources/graphics/StandardShadowCull.cpp) call the *same* scalar
// functions from this header, so a regression test on the host exercises the
// arithmetic the shader actually runs. Same arrangement as LightGridCulling.h.
//
// Everything here is scalar: the C++ vector types in HostDefinitions.h carry no
// arithmetic operators, and keeping the shader side scalar too means the two
// paths cannot drift through an operator overload that exists on only one side.

HOST_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// Records. Scalar layout (slangc runs with -fvk-use-scalar-layout), so these
// sizes and offsets are identical on both sides; the host asserts them.
// ---------------------------------------------------------------------------

// One cullable instance. 48 B.
//
// `vertexCount` and `objectSlot` are the two fields a surviving candidate needs
// to become a VkDrawIndirectCommand: {vertexCount, 1, 0, objectSlot}. The draw
// carries the object slot in firstInstance, which the vertex shader reads back
// as SV_StartInstanceLocation to index gSceneObjects -- see
// Standard.visibility.3d.slang. Bounds are WORLD space and already include the
// object's transform, so the kernel never touches a model matrix.
//
// renderFlags is copied in rather than read from gSceneObjects so the kernel is
// a pure function of its input buffers: no set-0 dependency, and no question
// about whether submitObject's staged uploads have landed before the dispatch.
SLANG_PUBLIC struct StandardCullCandidate {
    SLANG_PUBLIC float aabbMinX, aabbMinY, aabbMinZ;
    SLANG_PUBLIC uint  objectSlot;
    SLANG_PUBLIC float aabbMaxX, aabbMaxY, aabbMaxZ;
    SLANG_PUBLIC uint  vertexCount;
    SLANG_PUBLIC uint  renderFlags;     // eRenderableFlag_* | kStandardCullStaticBit
    SLANG_PUBLIC uint  visibilityKey;   // camera HiZ pass only; 0 for shadow tiles
    SLANG_PUBLIC uint  cullFlags;       // kStandardCullFlag*
    // INSTANCE_MASK mode only: element index of this candidate's instanceCount
    // word inside the indirect buffer, viewed as a uint array. Unused (0) for
    // the shadow tiles, which get whole commands written for them instead.
    SLANG_PUBLIC uint  commandWordOffset;
};

// One shadow-atlas tile: a frustum plus the ranges it owns. 128 B.
//
// planes are world space, normalized, inward-positive, in ml's plane order
// (left, right, bottom, top, near, far) so the GPU test and the CPU test that
// WalkAndPrepareRenderList used to run are the same test.
//
// candidateCount is both the number of candidates to test AND the number of
// indirect elements reserved for this tile. Reserving the worst case is what
// lets the CPU hand out ranges without knowing the survivor count.
SLANG_PUBLIC struct StandardCullTile {
    SLANG_PUBLIC float planes[24];      // 6 x (nx, ny, nz, d)
    SLANG_PUBLIC uint  candidateBase;
    SLANG_PUBLIC uint  candidateCount;
    SLANG_PUBLIC uint  indirectBase;
    SLANG_PUBLIC uint  countIndex;
    SLANG_PUBLIC uint  variabilityMask; // eObjectVariabilityFlag_*
    SLANG_PUBLIC uint  planeCount;      // 6; 5 drops the last plane BY INDEX
    // Index into the camera buffer, or kStandardCullNoCamera for a tile that
    // only frustum-tests (every shadow tile).
    SLANG_PUBLIC uint  cameraIndex;
    SLANG_PUBLIC uint  pad1;
};

// One 64-candidate chunk of one tile. 8 B. The (tile, candidate) work list is
// flattened on the host at group granularity, so a single dispatch covers every
// tile with no prefix sum, no indirect dispatch, and a deterministic
// group -> tile mapping that the host reference can reproduce exactly.
SLANG_PUBLIC struct StandardCullGroup {
    SLANG_PUBLIC uint tileIndex;
    SLANG_PUBLIC uint candidateOffset;  // multiple of kStandardCullGroupSize
};

// Camera-pass occlusion parameters. 80 B.
//
// Kept out of StandardCullTile rather than folded into it: a shadow tile has no
// use for any of this, and widening the tile would grow every one of the ~1000
// of them by a matrix. Tiles that occlusion-test point at one of these by
// index; tiles that do not leave cameraIndex at kStandardCullNoCamera.
//
// viewProjection is ROW-MAJOR, the same cMatrixf order
// StandardExtractFrustumPlanes takes, so both come from the same `vp.v`.
SLANG_PUBLIC struct StandardCullCamera {
    SLANG_PUBLIC float viewProjection[16];
    SLANG_PUBLIC uint  hiZWidth;     // mip 0 extent of the depth pyramid
    SLANG_PUBLIC uint  hiZHeight;
    SLANG_PUBLIC uint  hiZMipCount;  // >= 1
    SLANG_PUBLIC uint  pad0;
};

// What standardCullProjectAabb computed: where the box lands on screen, how
// near it gets, and whether the result may be used at all.
SLANG_PUBLIC struct StandardCullScreenRect {
    SLANG_PUBLIC float minU, minV, maxU, maxV;  // [0,1], y already flipped
    SLANG_PUBLIC float nearestDepth;            // smallest NDC z over the corners
    SLANG_PUBLIC uint  mip;                     // pyramid level to sample
    SLANG_PUBLIC uint  testable;                // 0 = keep the object untested
    SLANG_PUBLIC uint  pad0;
};

HOST_NAMESPACE_END

// ---------------------------------------------------------------------------
// Constants. Plain #defines rather than SHARED_CONST: Constants.h has to gate
// its `public static const` block behind HPL_DEFINE_SHARED_CONSTS so exactly
// one Slang module emits it, and a macro sidesteps that coupling entirely.
// ---------------------------------------------------------------------------

#define kStandardCullGroupSize      64u

// Mirrors of engine flags. Kept as literals because the engine headers that
// define them (GraphicsTypes.h, SceneTypes.h) are C++-only and cannot be
// included from Slang. The host asserts these against the real macros.
#define kStandardCullShadowCasterBit 0x00000001u  // eRenderableFlag_ShadowCaster
#define kStandardCullStaticBit       0x80000000u  // set by the host for IsStatic()
#define kStandardCullVariabilityStatic  0x00000001u  // eObjectVariabilityFlag_Static
#define kStandardCullVariabilityDynamic 0x00000002u  // eObjectVariabilityFlag_Dynamic

// cullFlags
#define kStandardCullFlagNeverOcclude 0x00000001u  // camera pass: skip the HiZ test

// Kernel modes.
//   COMPACT  survivors only, packed by an atomic bump; needs drawIndirectCount.
//   IN_PLACE every candidate writes its own slot, culled ones with
//            instanceCount = 0; used when the device lacks drawIndirectCount.
//   INSTANCE_MASK writes only the instanceCount word of a command the host
//            already filled in, so one kernel serves both the 4-word
//            VkDrawIndirectCommand and the 5-word indexed form (instanceCount
//            is word 1 in both). Order is the host's, untouched -- which is
//            what the back-to-front translucent pass needs.
//   VISIBILITY_REPLAY phase 1 of the camera's two-phase cull: draw what was
//            visible LAST frame, so there is something in the depth buffer to
//            build a pyramid from. Writes instanceCount from the persistent
//            visibility buffer; runs no occlusion test (there is no pyramid
//            yet).
//   VISIBILITY_UPDATE phase 2: test everything against the pyramid phase 1
//            produced, draw what is visible now but was NOT drawn in phase 1,
//            and record the new answer for next frame. The phase-2 command
//            sits gCommandWordDelta words after the phase-1 one.
#define kStandardCullModeCompact          0u
#define kStandardCullModeInPlace          1u
#define kStandardCullModeInstanceMask     2u
#define kStandardCullModeVisibilityReplay 3u
#define kStandardCullModeVisibilityUpdate 4u

// A tile with no camera does no occlusion test.
#define kStandardCullNoCamera 0xFFFFFFFFu

// Clip w at or below this counts as on/behind the near plane: the projection is
// not usable there, so the box is kept rather than tested.
#define kStandardCullNearEpsilon 1.0e-6f

HOST_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// Predicates
// ---------------------------------------------------------------------------

// Is the box on the inward side of one plane?
//
// Picks the AABB corner farthest along the plane normal ("positive vertex") and
// keeps the box unless even that corner is behind the plane. This is exactly
// what ml::cFrustum::CheckAabb does -- it blends min/max by the sign mask of
// the plane and rejects on a negative dot -- so a box accepted here is accepted
// there. Boxes that merely straddle a plane are kept: the test rejects only
// what is wholly outside.
SLANG_PUBLIC inline bool standardCullPlaneAcceptsAabb(
    float nx, float ny, float nz, float d,
    float minX, float minY, float minZ,
    float maxX, float maxY, float maxZ)
{
    float vx = nx > 0.0f ? maxX : minX;
    float vy = ny > 0.0f ? maxY : minY;
    float vz = nz > 0.0f ? maxZ : minZ;
    // Reject only on a provably negative dot. Written as !(x < 0) rather than
    // (x >= 0) so a NaN bound is KEPT, matching ml::cFrustum::CheckAabb, whose
    // SIMD sign test also fails to fire on NaN. A broken bounding volume should
    // draw a stray object, never silently delete a visible one.
    return !(nx * vx + ny * vy + nz * vz + d < 0.0f);
}

// Is the box inside every plane of the tile's frustum?
//
// planeCount = 5 drops the last plane BY INDEX, matching the ml::PLANES_NO_FAR
// that WalkAndPrepareRenderList passes for GetInfFarPlane() frusta. Index 5 is
// the far plane only when MvpToPlanes' reversed-Z swap did not fire, and that
// heuristic misfires for a standard-Z camera far from the origin -- so an
// infinite-far caller should keep planeCount at 6 and call
// StandardDisableFarPlane (StandardShadowCull.h) instead, which zeroes the far
// plane in whichever slot it really landed in.
SLANG_PUBLIC inline bool standardCullTileAcceptsAabb(
    StandardCullTile tile,
    float minX, float minY, float minZ,
    float maxX, float maxY, float maxZ)
{
    for (uint i = 0u; i < tile.planeCount; ++i) {
        uint b = i * 4u;
        if (!standardCullPlaneAcceptsAabb(tile.planes[b + 0u], tile.planes[b + 1u],
                                          tile.planes[b + 2u], tile.planes[b + 3u],
                                          minX, minY, minZ, maxX, maxY, maxZ)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Occlusion (camera pass)
// ---------------------------------------------------------------------------

// Project a world-space AABB to the depth pyramid's screen rect.
//
// Scalar throughout, like everything else here: the C++ float4x4 is a plain
// float[16] with no operators, so the matrix-vector product is written out.
// Row-major, M * v -- the same convention StandardExtractFrustumPlanes reads.
//
// Y IS FLIPPED. Both raster passes run under a negative-height viewport
// (viewport.y = height, viewport.height = -height; see
// cStandardShadowPass::RenderAtlas and cStandardTranslucentPass::Draw), so a
// framebuffer row is (0.5 - 0.5 * ndc.y) * height, not (0.5 + 0.5 * ndc.y).
// Sampling the pyramid with the unflipped V mirrors the occluder set
// vertically, which reads as geometry popping in the wrong half of the screen.
//
// `testable` is 0 when the box reaches the near plane, where the perspective
// divide is meaningless. The caller keeps the object: an occlusion test must
// only ever remove something it can prove is hidden.
SLANG_PUBLIC inline StandardCullScreenRect standardCullProjectAabb(
    StandardCullCamera camera,
    float minX, float minY, float minZ,
    float maxX, float maxY, float maxZ)
{
    StandardCullScreenRect rect;
    rect.minU = 0.0f;
    rect.minV = 0.0f;
    rect.maxU = 1.0f;
    rect.maxV = 1.0f;
    rect.nearestDepth = 0.0f;
    rect.mip = 0u;
    rect.testable = 0u;
    rect.pad0 = 0u;

    float lowU = 1.0f;
    float lowV = 1.0f;
    float highU = 0.0f;
    float highV = 0.0f;
    float nearest = 1.0f;

    for (uint corner = 0u; corner < 8u; ++corner) {
        float x = (corner & 1u) != 0u ? maxX : minX;
        float y = (corner & 2u) != 0u ? maxY : minY;
        float z = (corner & 4u) != 0u ? maxZ : minZ;

        float clipX = camera.viewProjection[0] * x + camera.viewProjection[1] * y +
                      camera.viewProjection[2] * z + camera.viewProjection[3];
        float clipY = camera.viewProjection[4] * x + camera.viewProjection[5] * y +
                      camera.viewProjection[6] * z + camera.viewProjection[7];
        float clipZ = camera.viewProjection[8] * x + camera.viewProjection[9] * y +
                      camera.viewProjection[10] * z + camera.viewProjection[11];
        float clipW = camera.viewProjection[12] * x + camera.viewProjection[13] * y +
                      camera.viewProjection[14] * z + camera.viewProjection[15];

        // Reaches the near plane: no usable projection, keep the box.
        if (!(clipW > kStandardCullNearEpsilon)) {
            return rect;
        }

        float inverseW = 1.0f / clipW;
        float ndcX = clipX * inverseW;
        float ndcY = clipY * inverseW;
        float ndcZ = clipZ * inverseW;

        float u = 0.5f + 0.5f * ndcX;
        float v = 0.5f - 0.5f * ndcY;

        lowU = u < lowU ? u : lowU;
        lowV = v < lowV ? v : lowV;
        highU = u > highU ? u : highU;
        highV = v > highV ? v : highV;
        nearest = ndcZ < nearest ? ndcZ : nearest;
    }

    // A box behind the camera projects to an inverted rect; one crossing the
    // frustum sides projects outside [0,1]. Clamp to the image and bail if
    // nothing is left -- the frustum test owns that rejection, not this one.
    lowU = lowU < 0.0f ? 0.0f : lowU;
    lowV = lowV < 0.0f ? 0.0f : lowV;
    highU = highU > 1.0f ? 1.0f : highU;
    highV = highV > 1.0f ? 1.0f : highV;
    if (!(highU > lowU) || !(highV > lowV)) {
        return rect;
    }

    // NaN bounds fall through every comparison above and land here; keeping
    // them matches standardCullPlaneAcceptsAabb, which also keeps NaN.
    if (!(nearest >= 0.0f)) {
        return rect;
    }

    // A mip whose texels are at least as large as the rect. Level 0 is the
    // pyramid's own resolution, already half the render target.
    //
    // ONE texel, not two. The gather reads only the rect's four CORNER texels,
    // so it covers the rect only when the rect straddles at most two adjacent
    // texels per axis -- which a one-texel-wide rect always does and a
    // two-texel-wide one does not: spanning 0.9 to 2.9 touches texels 0, 1 and
    // 2, and the corners read only 0 and 2.
    //
    // Missing a texel is not symmetric. The pyramid stores the FARTHEST depth,
    // so a skipped texel can only make the sampled value too NEAR, which makes
    // the caller cull something it cannot actually prove is hidden. On screen
    // that is a wall blinking out as the camera moves and its rect slides
    // across a texel boundary.
    float pixelWidth = (highU - lowU) * (float)camera.hiZWidth;
    float pixelHeight = (highV - lowV) * (float)camera.hiZHeight;
    float extent = pixelWidth > pixelHeight ? pixelWidth : pixelHeight;
    uint mip = 0u;
    while (mip + 1u < camera.hiZMipCount && extent > 1.0f) {
        extent = extent * 0.5f;
        mip = mip + 1u;
    }

    rect.minU = lowU;
    rect.minV = lowV;
    rect.maxU = highU;
    rect.maxV = highV;
    rect.nearestDepth = nearest;
    rect.mip = mip;
    rect.testable = 1u;
    return rect;
}

// Is the box hidden behind `sampledDepth`?
//
// sampledDepth is the FARTHEST depth in the box's screen rect, which is what a
// max-reduced pyramid stores (standard Z: larger is farther). If even the
// nearest point of the box is behind that, nothing of it can be visible.
//
// Strict `>`: a box exactly at the stored depth is the surface that wrote it.
SLANG_PUBLIC inline bool standardCullOccluded(StandardCullScreenRect rect,
                                              float sampledDepth)
{
    if (rect.testable == 0u)
        return false;
    return rect.nearestDepth > sampledDepth;
}

// ---------------------------------------------------------------------------
// Two-phase camera cull
//
// Phase 1 draws what was visible LAST frame, so the depth buffer has something
// to build a pyramid from. Phase 2 tests everything against that pyramid and
// draws what is visible now MINUS what phase 1 already drew.
//
// Together the two must satisfy: nothing is rasterized twice, and nothing
// visible is missed. The pair below is the whole of that logic, kept here so
// the headless tests check the same arithmetic the kernel runs.
// ---------------------------------------------------------------------------

// Phase 1. `frustumKeep` is the frustum test alone -- no pyramid exists yet
// this frame, so there is no occlusion term.
SLANG_PUBLIC inline uint standardCullReplayInstanceCount(bool frustumKeep,
                                                         uint wasVisible)
{
    return (frustumKeep && wasVisible != 0u) ? 1u : 0u;
}

// Phase 2. Read phaseOneDrawn from this candidate's phase-one command, not
// shared history: another candidate may have overwritten a colliding key.
SLANG_PUBLIC inline uint standardCullUpdateInstanceCount(bool visibleNow,
                                                         uint phaseOneDrawn)
{
    return (visibleNow && phaseOneDrawn == 0u) ? 1u : 0u;
}

// What phase 2 stores for next frame's phase 1.
SLANG_PUBLIC inline uint standardCullNextVisibility(bool visibleNow)
{
    return visibleNow ? 1u : 0u;
}

// Does this candidate belong in this tile's shadow map?
//
// Three gates, in the order the CPU applied them before the cull moved to the
// GPU: the object must be a shadow caster at all, its static/dynamic class must
// be one the light accepts (iLight::GetShadowCastersAffected), and its bounds
// must survive the tile frustum. The variability test cannot be hoisted into
// the per-frame gather because the mask is per light, not per frame.
SLANG_PUBLIC inline bool standardCullKeepCandidate(
    StandardCullTile tile, StandardCullCandidate c)
{
    if ((c.renderFlags & kStandardCullShadowCasterBit) == 0u)
        return false;

    bool isStatic = (c.renderFlags & kStandardCullStaticBit) != 0u;
    uint needed = isStatic ? kStandardCullVariabilityStatic
                           : kStandardCullVariabilityDynamic;
    if ((tile.variabilityMask & needed) == 0u)
        return false;

    return standardCullTileAcceptsAabb(tile,
                                       c.aabbMinX, c.aabbMinY, c.aabbMinZ,
                                       c.aabbMaxX, c.aabbMaxY, c.aabbMaxZ);
}

HOST_NAMESPACE_END
