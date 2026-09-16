#pragma once

#include <cstddef>
#include <cstdint>

#include "StandardCull.h"

namespace hpl {

// Host-side half of the Standard renderer's GPU cull.
//
// This translation unit deliberately includes no engine headers beyond the
// shared record definitions, so it compiles standalone against a test harness
// with no RI, no Vulkan, and no world. The per-candidate predicates it relies
// on live in amnesia/slang/StandardCull.h and are the same ones the compute
// kernel calls, which is what makes a headless test meaningful on a machine
// with no GPU.

// VkDrawIndirectCommand, redeclared so this file needs no Vulkan headers. The
// host asserts the layout against the real struct where Vulkan is available.
struct StandardDrawIndirect {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};

// Extract the six world-space frustum planes of a view-projection matrix.
//
// `viewProjRowMajor` is an hpl cMatrixf in its native row-major order (what
// cFrustum stores before cMath::ToFloatTranspose4x4 hands it to MathLib). The
// output is normalized, inward-positive, and in ml's plane order -- left,
// right, bottom, top, near, far -- so it is interchangeable with what
// ml::MvpToPlanes(ml::STYLE_D3D, ...) produces for the same matrix. That
// equivalence is the whole claim of this change: the GPU runs the test the CPU
// used to run, relocated, not a lookalike.
//
// Writes 24 floats: 6 planes x (nx, ny, nz, d), the layout of
// StandardCullTile::planes.
//
// Returns MathLib's reversed-Z verdict, which is the heuristic |near.w| >
// |far.w| rather than a real property of the projection: it also fires for an
// ordinary standard-Z camera far from the origin looking away from it, and it
// swaps the near and far entries when it does. That is reproduced deliberately,
// because ml::PLANES_NO_FAR drops the last plane by index, so diverging here
// would drop a different plane than the CPU path used to.
bool StandardExtractFrustumPlanes(const float viewProjRowMajor[16], float outPlanes[24]);

// Neutralize the far plane of an extracted plane set.
//
// StandardCullTile::planeCount = 5 drops the LAST plane by index, which is the
// far plane only when the reversed-Z swap above did not fire. Because that
// heuristic misfires for an ordinary standard-Z camera far from the origin, a
// caller that wants an infinite-far frustum must not simply set planeCount = 5:
// on a swapped set that drops the NEAR plane and keeps the far one, which draws
// everything behind the camera.
//
// This zeroes the far plane in whichever slot it actually landed in and leaves
// planeCount at 6. A zeroed plane has dot + d == 0, and the predicate rejects
// only on a provably negative dot, so it accepts unconditionally -- the same
// effect as not testing it.
//
// The slot is decided geometrically, not from the reversed flag: that flag
// cannot say whether the swap was right (a genuine reversed-Z projection) or
// wrong (the misfire), and the two put the far plane in opposite slots. The
// four side planes all lean toward the frustum's forward direction, so their
// summed normal gives a forward vector, and the far plane is the depth plane
// facing against it.
void StandardDisableFarPlane(float planes[24]);

// Does a sphere overlap an AABB? Used by the per-frame gather to pick each
// light's candidate sublist out of the frame-global caster set.
bool StandardSphereOverlapsAabb(float centerX, float centerY, float centerZ, float radius,
                                float minX, float minY, float minZ,
                                float maxX, float maxY, float maxZ);

// A depth pyramid as the host reference sees it: every mip concatenated, mip 0
// first, each mip max(1, width >> m) x max(1, height >> m) and stored row-major.
// The GPU holds the same data as a real mip chain; this shape is what lets a
// headless test build one by hand.
struct StandardHiZPyramid {
    const float* texels;
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
};

// First texel index of `mip`, or the total texel count when mip == mipCount.
size_t StandardHiZMipOffset(const StandardHiZPyramid& pyramid, uint32_t mip);

// The farthest depth stored under the rect: a 2x2 gather at the rect's corners
// on the mip the projection chose, reduced with max. The mip is picked so its
// texels are at least as large as the rect, which is what makes four samples
// enough to cover it.
//
// Returns 1.0 (nothing can be behind it, so nothing is culled) when the rect is
// not testable or the pyramid is malformed.
float StandardHiZSampleFarthest(const StandardHiZPyramid& pyramid,
                                const StandardCullScreenRect& rect);

// The whole camera occlusion test, for tests and for parity with the kernel:
// project, sample, compare. True means the box is provably hidden.
bool StandardCullOcclusionReference(const StandardCullCamera& camera,
                                    const StandardHiZPyramid& pyramid,
                                    float minX, float minY, float minZ,
                                    float maxX, float maxY, float maxZ);

// Reference implementation of the cull kernel, byte-for-byte in intent with
// Standard.cull.cs.slang's cullShadowTiles.
//
// `candidates` is the whole candidate array; the tile selects its own slice via
// candidateBase/candidateCount. `outIndirect` must have room for
// tile.candidateCount commands -- reserving the worst case is what lets the
// host hand out indirect ranges without knowing the survivor count.
//
// Returns the number of commands written: the survivor count in COMPACT mode,
// and tile.candidateCount in IN_PLACE mode (where culled entries are present
// but carry instanceCount = 0).
uint32_t StandardShadowCullReference(const StandardCullCandidate* candidates,
                                     size_t candidateArrayLength,
                                     const StandardCullTile& tile,
                                     uint32_t mode,
                                     StandardDrawIndirect* outIndirect);

// Reference for kStandardCullModeInstanceMask, the mode the translucent passes
// run in. It is separate from StandardShadowCullReference because it does not
// write commands at all: the host wrote every one of them, and the kernel owns
// exactly one word per candidate -- the instanceCount at commandWordOffset.
// Slot order is therefore untouched, which is what a back-to-front blended pass
// requires.
//
// `pyramid` may be null, which runs the frustum test alone (what the kernel
// does for a tile whose cameraIndex is kStandardCullNoCamera). `commandWords`
// is the whole command ring viewed as uint[]; a candidate whose word offset
// falls outside it is skipped rather than allowed to corrupt memory.
//
// Returns the number of candidates kept.
uint32_t StandardCullInstanceMaskReference(const StandardCullCandidate* candidates,
                                           size_t candidateArrayLength,
                                           const StandardCullTile& tile,
                                           const StandardCullCamera* camera,
                                           const StandardHiZPyramid* pyramid,
                                           uint32_t* commandWords,
                                           size_t commandWordCount);

} // namespace hpl
