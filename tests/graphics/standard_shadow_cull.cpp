// Headless coverage for the Standard renderer's GPU shadow cull.
//
// The point of these tests is that the GPU kernel and this host code call the
// same scalar predicates out of amnesia/slang/StandardCull.h, so the arithmetic
// exercised here is the arithmetic the shader runs. The machine running them
// has no GPU, so this is where frustum-extraction and survivor-selection bugs
// have to be caught.

// MathLib's emulation.h macro-defines the AVX intrinsic names it emulates
// (_mm256_loadu_si256 -> emu_mm256_loadu_si256), and with ML_NAMESPACE the
// emu_* functions land in namespace ml while the macros stay global. Any
// system header pulled in after ml.h that touches those intrinsics -- MSVC's
// <wchar.h> does -- then fails to resolve the replacement. So: system and
// third-party headers first, ml.h last.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "utest.h"

#define ML_NAMESPACE
#include "ml.h"

#include "../../amnesia/slang/StandardCull.h"
#include "graphics/StandardShadowCull.h"

namespace {

using hpl::StandardCullCandidate;
using hpl::StandardCullTile;
using hpl::StandardDrawIndirect;

// Rebuild the ml matrix the way cMath::ToFloatTranspose4x4 does, so MathLib
// sees exactly the matrix cFrustum would have handed it.
ml::float4x4 ToMathLib(const float rowMajor[16]) {
    ml::float4x4 value(rowMajor[0], rowMajor[4], rowMajor[8], rowMajor[12],
                       rowMajor[1], rowMajor[5], rowMajor[9], rowMajor[13],
                       rowMajor[2], rowMajor[6], rowMajor[10], rowMajor[14],
                       rowMajor[3], rowMajor[7], rowMajor[11], rowMajor[15]);
    value.Transpose();
    return value;
}

void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
}

// Row-major Vulkan-style perspective: z maps to [0, 1], which is the
// convention the near-plane extraction (row2 alone) depends on.
void Perspective(float out[16], float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    for (int i = 0; i < 16; ++i) {
        out[i] = 0.0f;
    }
    out[0] = f / aspect;
    out[5] = f;
    out[10] = zFar / (zFar - zNear);
    out[11] = -zNear * zFar / (zFar - zNear);
    out[14] = 1.0f;
}

void LookAt(float out[16], float eyeX, float eyeY, float eyeZ, float yaw, float pitch) {
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
    const float forward[3] = {cosPitch * sinYaw, sinPitch, cosPitch * cosYaw};
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};
    float right[3] = {worldUp[1] * forward[2] - worldUp[2] * forward[1],
                      worldUp[2] * forward[0] - worldUp[0] * forward[2],
                      worldUp[0] * forward[1] - worldUp[1] * forward[0]};
    const float length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    right[0] /= length;
    right[1] /= length;
    right[2] /= length;
    const float up[3] = {forward[1] * right[2] - forward[2] * right[1],
                         forward[2] * right[0] - forward[0] * right[2],
                         forward[0] * right[1] - forward[1] * right[0]};
    const float eye[3] = {eyeX, eyeY, eyeZ};
    const float* basis[3] = {right, up, forward};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = basis[row][0];
        out[row * 4 + 1] = basis[row][1];
        out[row * 4 + 2] = basis[row][2];
        out[row * 4 + 3] = -(basis[row][0] * eye[0] + basis[row][1] * eye[1] + basis[row][2] * eye[2]);
    }
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

void ViewProjection(float out[16], float fovY, float aspect, float zNear, float zFar,
                    float eyeX, float eyeY, float eyeZ, float yaw, float pitch) {
    float projection[16];
    float view[16];
    Perspective(projection, fovY, aspect, zNear, zFar);
    LookAt(view, eyeX, eyeY, eyeZ, yaw, pitch);
    Multiply(projection, view, out);
}

StandardCullTile MakeTile(const float viewProjection[16], uint32_t variabilityMask) {
    StandardCullTile tile{};
    hpl::StandardExtractFrustumPlanes(viewProjection, tile.planes);
    tile.planeCount = 6u;
    tile.variabilityMask = variabilityMask;
    return tile;
}

StandardCullCandidate MakeCandidate(float centerX, float centerY, float centerZ, float halfSize,
                                    uint32_t slot, uint32_t vertexCount, uint32_t renderFlags) {
    StandardCullCandidate candidate{};
    candidate.aabbMinX = centerX - halfSize;
    candidate.aabbMinY = centerY - halfSize;
    candidate.aabbMinZ = centerZ - halfSize;
    candidate.aabbMaxX = centerX + halfSize;
    candidate.aabbMaxY = centerY + halfSize;
    candidate.aabbMaxZ = centerZ + halfSize;
    candidate.objectSlot = slot;
    candidate.vertexCount = vertexCount;
    candidate.renderFlags = renderFlags;
    return candidate;
}

constexpr uint32_t kCaster = kStandardCullShadowCasterBit;
constexpr uint32_t kStaticCaster = kStandardCullShadowCasterBit | kStandardCullStaticBit;
constexpr uint32_t kBothVariability =
    kStandardCullVariabilityStatic | kStandardCullVariabilityDynamic;

} // namespace

// ---------------------------------------------------------------------------
// Record layout. The GPU reads these bytes through a scalar-layout storage
// buffer; a silent offset change here corrupts every draw.
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, RecordLayoutMatchesTheGpuContract) {
    ASSERT_EQ(sizeof(StandardCullCandidate), (size_t)48);
    ASSERT_EQ(offsetof(StandardCullCandidate, aabbMinX), (size_t)0);
    ASSERT_EQ(offsetof(StandardCullCandidate, objectSlot), (size_t)12);
    ASSERT_EQ(offsetof(StandardCullCandidate, aabbMaxX), (size_t)16);
    ASSERT_EQ(offsetof(StandardCullCandidate, vertexCount), (size_t)28);
    ASSERT_EQ(offsetof(StandardCullCandidate, renderFlags), (size_t)32);
    ASSERT_EQ(offsetof(StandardCullCandidate, visibilityKey), (size_t)36);
    ASSERT_EQ(offsetof(StandardCullCandidate, cullFlags), (size_t)40);
    ASSERT_EQ(offsetof(StandardCullCandidate, commandWordOffset), (size_t)44);

    ASSERT_EQ(sizeof(StandardCullTile), (size_t)128);
    ASSERT_EQ(offsetof(StandardCullTile, planes), (size_t)0);
    ASSERT_EQ(offsetof(StandardCullTile, candidateBase), (size_t)96);
    ASSERT_EQ(offsetof(StandardCullTile, candidateCount), (size_t)100);
    ASSERT_EQ(offsetof(StandardCullTile, indirectBase), (size_t)104);
    ASSERT_EQ(offsetof(StandardCullTile, countIndex), (size_t)108);
    ASSERT_EQ(offsetof(StandardCullTile, variabilityMask), (size_t)112);
    ASSERT_EQ(offsetof(StandardCullTile, planeCount), (size_t)116);
    ASSERT_EQ(offsetof(StandardCullTile, cameraIndex), (size_t)120);

    // The camera record the occlusion test reads. Separate from the tile so a
    // shadow tile does not carry a matrix it never uses.
    ASSERT_EQ(sizeof(hpl::StandardCullCamera), (size_t)80);
    ASSERT_EQ(offsetof(hpl::StandardCullCamera, viewProjection), (size_t)0);
    ASSERT_EQ(offsetof(hpl::StandardCullCamera, hiZWidth), (size_t)64);
    ASSERT_EQ(offsetof(hpl::StandardCullCamera, hiZHeight), (size_t)68);
    ASSERT_EQ(offsetof(hpl::StandardCullCamera, hiZMipCount), (size_t)72);

    ASSERT_EQ(sizeof(hpl::StandardCullGroup), (size_t)8);
    ASSERT_EQ(sizeof(StandardDrawIndirect), (size_t)16);
}

// ---------------------------------------------------------------------------
// Frustum extraction. This is the load-bearing claim of the whole change: the
// GPU must run the test WalkAndPrepareRenderList used to run, not a lookalike.
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, PlanesMatchMathLibForRandomCameras) {
    std::mt19937 rng(20260915u);
    std::uniform_real_distribution<float> position(-40.0f, 40.0f);
    std::uniform_real_distribution<float> angle(-3.14159f, 3.14159f);

    for (int iteration = 0; iteration < 200; ++iteration) {
        float viewProjection[16];
        ViewProjection(viewProjection, 1.2f, 16.0f / 9.0f, 0.05f, 100.0f,
                       position(rng), position(rng), position(rng),
                       angle(rng), angle(rng) * 0.3f);

        float mine[24];
        const bool reversed = hpl::StandardExtractFrustumPlanes(viewProjection, mine);

        ml::float4 theirs[ml::PLANES_NUM];
        const bool theirsReversed =
            ml::MvpToPlanes(ml::STYLE_D3D, ToMathLib(viewProjection), theirs);
        // MathLib infers reversed-Z from |near.w| > |far.w|, which also fires
        // for an ordinary standard-Z camera sitting far from the origin and
        // looking away from it. Matching that quirk exactly is the contract --
        // diverging would swap which plane planeCount = 5 drops.
        ASSERT_EQ(reversed, theirsReversed);

        for (int plane = 0; plane < 6; ++plane) {
            ASSERT_NEAR(mine[plane * 4 + 0], theirs[plane].x, 1e-3);
            ASSERT_NEAR(mine[plane * 4 + 1], theirs[plane].y, 1e-3);
            ASSERT_NEAR(mine[plane * 4 + 2], theirs[plane].z, 1e-3);
            ASSERT_NEAR(mine[plane * 4 + 3], theirs[plane].w, 1e-3);
        }
    }
}

// A point light's cube face is a 90 degree frustum widened by a couple of
// texels so neighbouring faces overlap; that widening rides through the
// projection into the planes, so it must not perturb the match.
UTEST(StandardShadowCull, PlanesMatchMathLibForWidenedCubeFaces) {
    const float border = 2.0f;
    const float size = 512.0f;
    const float fov = 2.0f * std::atan((size * 0.5f + border) / (size * 0.5f));

    for (int face = 0; face < 6; ++face) {
        const float yaw = static_cast<float>(face) * 1.0471975f;
        float viewProjection[16];
        ViewProjection(viewProjection, fov, 1.0f, 0.05f, 40.0f, 1.0f, 2.0f, -3.0f, yaw, 0.0f);

        float mine[24];
        hpl::StandardExtractFrustumPlanes(viewProjection, mine);

        ml::float4 theirs[ml::PLANES_NUM];
        ml::MvpToPlanes(ml::STYLE_D3D, ToMathLib(viewProjection), theirs);

        for (int plane = 0; plane < 6; ++plane) {
            ASSERT_NEAR(mine[plane * 4 + 0], theirs[plane].x, 1e-3);
            ASSERT_NEAR(mine[plane * 4 + 3], theirs[plane].w, 1e-3);
        }
    }
}

// StandardExtractFrustumPlanes takes a ROW-MAJOR matrix (cMatrixf::v, which
// aliases m[row][col]). The renderer also keeps a transposed copy of the same
// matrix for the raster push constants, and feeding that one here produces
// planes that look plausible -- normalized, six of them -- but cull the wrong
// half of the world. Nothing downstream would catch it without a GPU, so pin
// the convention: the two orientations must disagree about a box the camera is
// plainly looking at.
UTEST(StandardShadowCull, ExtractionIsRowMajorNotTransposed) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f,
                   0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    float transposed[16];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            transposed[row * 4 + column] = viewProjection[column * 4 + row];
        }
    }

    StandardCullTile rowMajor = MakeTile(viewProjection, kBothVariability);
    StandardCullTile columnMajor = MakeTile(transposed, kBothVariability);

    // Dead ahead, well inside the frustum.
    const float minimum[3] = {-1.0f, -1.0f, 9.0f};
    const float maximum[3] = {1.0f, 1.0f, 11.0f};

    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(
        rowMajor, minimum[0], minimum[1], minimum[2],
        maximum[0], maximum[1], maximum[2]));
    ASSERT_FALSE(hpl::standardCullTileAcceptsAabb(
        columnMajor, minimum[0], minimum[1], minimum[2],
        maximum[0], maximum[1], maximum[2]));
}

UTEST(StandardShadowCull, AcceptanceMatchesMathLibCheckAabb) {
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> position(-40.0f, 40.0f);
    std::uniform_real_distribution<float> angle(-3.14159f, 3.14159f);
    std::uniform_real_distribution<float> extent(0.2f, 4.0f);

    int accepted = 0;
    int total = 0;
    for (int iteration = 0; iteration < 120; ++iteration) {
        float viewProjection[16];
        ViewProjection(viewProjection, 1.2f, 16.0f / 9.0f, 0.05f, 100.0f,
                       position(rng), position(rng), position(rng),
                       angle(rng), angle(rng) * 0.3f);

        const StandardCullTile tile = MakeTile(viewProjection, kBothVariability);
        ml::cFrustum frustum;
        frustum.Setup(ml::STYLE_D3D, ToMathLib(viewProjection));

        for (int box = 0; box < 50; ++box) {
            const float centerX = position(rng), centerY = position(rng), centerZ = position(rng);
            const float half = extent(rng);
            const float minimum[3] = {centerX - half, centerY - half, centerZ - half};
            const float maximum[3] = {centerX + half, centerY + half, centerZ + half};

            const bool mathLibKeeps = frustum.CheckAabb(
                ml::float3(minimum[0], minimum[1], minimum[2]),
                ml::float3(maximum[0], maximum[1], maximum[2]), ml::PLANES_NUM);
            const bool cullKeeps = hpl::standardCullTileAcceptsAabb(
                tile, minimum[0], minimum[1], minimum[2], maximum[0], maximum[1], maximum[2]);

            ASSERT_EQ(mathLibKeeps, cullKeeps);
            accepted += mathLibKeeps ? 1 : 0;
            ++total;
        }
    }
    // Guard against a vacuous pass: a test where nothing (or everything) is
    // accepted would agree with any implementation.
    ASSERT_GT(accepted, total / 20);
    ASSERT_LT(accepted, total - total / 20);
}

// ---------------------------------------------------------------------------
// Plane predicate edge cases
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, PlanePredicateKeepsStraddlingAndRejectsOutside) {
    // Inward normal +X, plane through the origin: keep anything reaching x >= 0.
    const float nx = 1.0f, ny = 0.0f, nz = 0.0f, d = 0.0f;

    ASSERT_TRUE(hpl::standardCullPlaneAcceptsAabb(nx, ny, nz, d, 1.0f, -1.0f, -1.0f, 2.0f, 1.0f, 1.0f));
    // Straddling: the positive corner is still inside.
    ASSERT_TRUE(hpl::standardCullPlaneAcceptsAabb(nx, ny, nz, d, -1.0f, -1.0f, -1.0f, 0.5f, 1.0f, 1.0f));
    // Exactly touching counts as inside.
    ASSERT_TRUE(hpl::standardCullPlaneAcceptsAabb(nx, ny, nz, d, -2.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f));
    // Wholly behind.
    ASSERT_FALSE(hpl::standardCullPlaneAcceptsAabb(nx, ny, nz, d, -2.0f, -1.0f, -1.0f, -0.5f, 1.0f, 1.0f));
}

UTEST(StandardShadowCull, DegenerateBoundsAreKeptNotDropped) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const StandardCullTile tile = MakeTile(viewProjection, kBothVariability);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    // A broken bounding volume must draw a stray object, never silently delete
    // a visible one. MathLib's SIMD sign test behaves the same way on NaN.
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, nan, nan, nan, nan, nan, nan));

    // A zero-extent box on the view axis is a legitimate degenerate case.
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 10.0f));

    // A box swallowing the whole frustum is visible, not outside.
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, -500.0f, -500.0f, -500.0f, 500.0f, 500.0f, 500.0f));
}

UTEST(StandardShadowCull, FarPlaneCanBeSkipped) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    StandardCullTile tile = MakeTile(viewProjection, kBothVariability);

    // Straight ahead but past the far plane.
    const float minimum[3] = {-1.0f, -1.0f, 40.0f};
    const float maximum[3] = {1.0f, 1.0f, 42.0f};

    ASSERT_FALSE(hpl::standardCullTileAcceptsAabb(tile, minimum[0], minimum[1], minimum[2],
                                                  maximum[0], maximum[1], maximum[2]));
    // planeCount 5 is the ml::PLANES_NO_FAR path taken for infinite-far frusta.
    tile.planeCount = 5u;
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, minimum[0], minimum[1], minimum[2],
                                                 maximum[0], maximum[1], maximum[2]));
}

// The reversed-Z heuristic is |near.w| > |far.w|, which is not a property of
// the projection: it also fires for an ordinary standard-Z camera far enough
// from the world origin and looking toward it, and it swaps the near and far
// entries when it does. MathLib does the same thing, so the swap is reproduced
// on purpose -- but it makes planeCount = 5 drop the wrong plane, and this is
// the test that says so.
UTEST(StandardShadowCull, SkippingTheFarPlaneByIndexIsWrongOnASwappedSet) {
    float viewProjection[16];
    // Eye 60 units back, looking at the origin: the origin sits at camera-space
    // z = 60, well past (near + far) / 2, so the heuristic fires.
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 20.0f, 0.0f, 0.0f, -60.0f, 0.0f, 0.0f);
    StandardCullTile tile{};
    const bool reversed = hpl::StandardExtractFrustumPlanes(viewProjection, tile.planes);
    ASSERT_TRUE(reversed);
    tile.planeCount = 6u;
    tile.variabilityMask = kBothVariability;

    // Straight ahead but past the far plane (camera-space z 80, far is 20).
    const float pastFar[6] = {-1.0f, -1.0f, 20.0f, 1.0f, 1.0f, 22.0f};

    ASSERT_FALSE(hpl::standardCullTileAcceptsAabb(tile, pastFar[0], pastFar[1], pastFar[2],
                                                  pastFar[3], pastFar[4], pastFar[5]));

    // The swap put the far plane in the NEAR slot, so dropping index 5 leaves
    // the far plane enforced -- planeCount = 5 does not do what it says here.
    // The plane it drops instead is the near plane, which is the half of the
    // damage a box test cannot show: for a symmetric perspective frustum the
    // four side planes already exclude everything behind the apex.
    tile.planeCount = 5u;
    ASSERT_FALSE(hpl::standardCullTileAcceptsAabb(tile, pastFar[0], pastFar[1], pastFar[2],
                                                  pastFar[3], pastFar[4], pastFar[5]));

    // StandardDisableFarPlane picks the slot geometrically, so it zeroes the
    // plane that faces against the frustum's forward direction -- slot 4 on
    // this swapped set, not the slot 5 that planeCount = 5 would have taken.
    tile.planeCount = 6u;
    hpl::StandardDisableFarPlane(tile.planes);
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(tile.planes[16 + i], 0.0f);
    }
    ASSERT_NE(tile.planes[20] * tile.planes[20] + tile.planes[21] * tile.planes[21] +
                  tile.planes[22] * tile.planes[22],
              0.0f);
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, pastFar[0], pastFar[1], pastFar[2],
                                                 pastFar[3], pastFar[4], pastFar[5]));
}

UTEST(StandardShadowCull, DisablingTheFarPlaneKeepsTheNearPlaneUnswapped) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    StandardCullTile tile{};
    const bool reversed = hpl::StandardExtractFrustumPlanes(viewProjection, tile.planes);
    ASSERT_FALSE(reversed);
    tile.planeCount = 6u;
    tile.variabilityMask = kBothVariability;

    hpl::StandardDisableFarPlane(tile.planes);
    // Unswapped set: the far plane is where planeCount = 5 would have expected
    // it, and the near plane survives.
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(tile.planes[20 + i], 0.0f);
    }
    ASSERT_NE(tile.planes[16] * tile.planes[16] + tile.planes[17] * tile.planes[17] +
                  tile.planes[18] * tile.planes[18],
              0.0f);
    // Past the far plane: now accepted.
    ASSERT_TRUE(hpl::standardCullTileAcceptsAabb(tile, -1.0f, -1.0f, 40.0f, 1.0f, 1.0f, 42.0f));
}

// ---------------------------------------------------------------------------
// Camera occlusion
//
// The pyramid here is built by hand so the test owns the occluder set exactly:
// a max-reduced (farthest-depth) chain, which is the only reduction that makes
// "nearest point of the box is behind the farthest thing drawn here" a proof of
// invisibility under standard Z.
// ---------------------------------------------------------------------------

using hpl::StandardCullCamera;
using hpl::StandardHiZPyramid;

// Concatenated mips, mip 0 first, each one the max of its 2x2 parent block.
struct Pyramid {
    std::vector<float> texels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;

    StandardHiZPyramid View() const {
        StandardHiZPyramid view{};
        view.texels = texels.data();
        view.width = width;
        view.height = height;
        view.mipCount = mipCount;
        return view;
    }
};

Pyramid BuildPyramid(uint32_t width, uint32_t height, const std::vector<float>& mip0) {
    Pyramid pyramid;
    pyramid.width = width;
    pyramid.height = height;
    pyramid.texels = mip0;
    uint32_t w = width;
    uint32_t h = height;
    uint32_t count = 1;
    std::vector<float> previous = mip0;
    while (w > 1u || h > 1u) {
        const uint32_t nw = std::max<uint32_t>(1u, w >> 1);
        const uint32_t nh = std::max<uint32_t>(1u, h >> 1);
        std::vector<float> level(static_cast<size_t>(nw) * nh, 0.0f);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                float farthest = 0.0f;
                for (uint32_t dy = 0; dy < 2u; ++dy) {
                    for (uint32_t dx = 0; dx < 2u; ++dx) {
                        const uint32_t sx = std::min<uint32_t>(w - 1u, x * 2u + dx);
                        const uint32_t sy = std::min<uint32_t>(h - 1u, y * 2u + dy);
                        farthest = std::max(farthest, previous[static_cast<size_t>(sy) * w + sx]);
                    }
                }
                level[static_cast<size_t>(y) * nw + x] = farthest;
            }
        }
        pyramid.texels.insert(pyramid.texels.end(), level.begin(), level.end());
        previous.swap(level);
        w = nw;
        h = nh;
        ++count;
    }
    pyramid.mipCount = count;
    return pyramid;
}

// NDC depth of a point on the view axis. Standard Z is violently nonlinear --
// with near = 0.1 a stored depth of 0.5 is barely 0.2 world units out -- so
// occluder depths in these tests are derived from the projection rather than
// written as literals.
float NdcDepthAt(const float viewProjection[16], float worldZ) {
    const float clipZ = viewProjection[10] * worldZ + viewProjection[11];
    const float clipW = viewProjection[14] * worldZ + viewProjection[15];
    return clipZ / clipW;
}

StandardCullCamera MakeCamera(const float viewProjection[16], uint32_t width,
                              uint32_t height, uint32_t mipCount) {
    StandardCullCamera camera{};
    for (int i = 0; i < 16; ++i)
        camera.viewProjection[i] = viewProjection[i];
    camera.hiZWidth = width;
    camera.hiZHeight = height;
    camera.hiZMipCount = mipCount;
    return camera;
}

UTEST(StandardShadowCull, OcclusionRejectsABoxBehindTheOccluderAndKeepsOneInFront) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    // A wall filling the screen, 30 world units out.
    const uint32_t kSize = 64;
    const float wall = NdcDepthAt(viewProjection, 30.0f);
    Pyramid pyramid = BuildPyramid(kSize, kSize, std::vector<float>(kSize * kSize, wall));
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // Straight ahead, small, and far enough back that its whole extent sits
    // beyond the wall's stored depth.
    const hpl::StandardCullScreenRect behind =
        hpl::standardCullProjectAabb(camera, -0.5f, -0.5f, 60.0f, 0.5f, 0.5f, 61.0f);
    ASSERT_EQ(behind.testable, 1u);
    ASSERT_TRUE(behind.nearestDepth > wall);
    ASSERT_TRUE(hpl::StandardCullOcclusionReference(camera, pyramid.View(),
                                                    -0.5f, -0.5f, 60.0f, 0.5f, 0.5f, 61.0f));

    // The same box in front of the wall survives.
    ASSERT_FALSE(hpl::StandardCullOcclusionReference(camera, pyramid.View(),
                                                     -0.5f, -0.5f, 1.0f, 0.5f, 0.5f, 2.0f));
}

UTEST(StandardShadowCull, OcclusionKeepsABoxWhereTheOccluderHasAHole) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    // A wall everywhere except the top-left quadrant, which is open sky
    // (depth 1.0). A max reduction propagates the hole up the chain, so a box
    // overlapping it must survive at every mip.
    const uint32_t kSize = 64;
    const float wall = NdcDepthAt(viewProjection, 30.0f);
    std::vector<float> mip0(kSize * kSize, wall);
    for (uint32_t y = 0; y < kSize / 2; ++y)
        for (uint32_t x = 0; x < kSize / 2; ++x)
            mip0[static_cast<size_t>(y) * kSize + x] = 1.0f;
    Pyramid pyramid = BuildPyramid(kSize, kSize, mip0);
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // Up and to the left in world space. With the flipped V that lands in the
    // open top-left quadrant; without the flip it would land in the wall, so
    // this case is also what pins the Y orientation.
    ASSERT_FALSE(hpl::StandardCullOcclusionReference(camera, pyramid.View(),
                                                     -30.0f, 5.0f, 60.0f, -20.0f, 15.0f, 61.0f));
    // Mirrored below the axis, where the wall really is.
    ASSERT_TRUE(hpl::StandardCullOcclusionReference(camera, pyramid.View(),
                                                    -30.0f, -15.0f, 60.0f, -20.0f, -5.0f, 61.0f));
}

UTEST(StandardShadowCull, OcclusionKeepsBoxesItCannotProveHidden) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const uint32_t kSize = 64;
    Pyramid pyramid = BuildPyramid(kSize, kSize,
                                   std::vector<float>(kSize * kSize, NdcDepthAt(viewProjection, 30.0f)));
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // Straddling the near plane: no usable perspective divide.
    const hpl::StandardCullScreenRect straddling =
        hpl::standardCullProjectAabb(camera, -1.0f, -1.0f, -5.0f, 1.0f, 1.0f, 5.0f);
    ASSERT_EQ(straddling.testable, 0u);
    ASSERT_FALSE(hpl::standardCullOccluded(straddling, 0.0f));

    // Entirely behind the camera.
    const hpl::StandardCullScreenRect behindCamera =
        hpl::standardCullProjectAabb(camera, -1.0f, -1.0f, -20.0f, 1.0f, 1.0f, -10.0f);
    ASSERT_EQ(behindCamera.testable, 0u);

    // NaN bounds, the same rule standardCullPlaneAcceptsAabb follows.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const hpl::StandardCullScreenRect broken =
        hpl::standardCullProjectAabb(camera, nan, nan, nan, nan, nan, nan);
    ASSERT_EQ(broken.testable, 0u);
    ASSERT_FALSE(hpl::StandardCullOcclusionReference(camera, pyramid.View(),
                                                     nan, nan, nan, nan, nan, nan));

    // An empty pyramid can never cull.
    StandardHiZPyramid empty{};
    ASSERT_FALSE(hpl::StandardCullOcclusionReference(camera, empty,
                                                     -0.5f, -0.5f, 60.0f, 0.5f, 0.5f, 61.0f));
}

// The gather reads only the rect's four CORNER texels, so the chosen mip has to
// guarantee the rect touches at most two texels per axis. If it can touch three,
// the middle one is never read -- and because the pyramid stores the FARTHEST
// depth, a missed texel makes the sampled value too NEAR and culls geometry that
// is actually visible. On screen that is a wall flickering as the camera moves
// and its rect slides across texel boundaries.
UTEST(StandardShadowCull, OcclusionMipGuaranteesTheGatherCoversTheRect) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const uint32_t kSize = 256;
    Pyramid pyramid = BuildPyramid(kSize, kSize,
                                   std::vector<float>(kSize * kSize, NdcDepthAt(viewProjection, 30.0f)));
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // Sweep size AND sub-texel offset: whether a rect straddles two texels or
    // three depends on where it lands, not just how big it is, which is exactly
    // why the bug comes and goes instead of being always wrong.
    for (int sizeStep = 1; sizeStep <= 40; ++sizeStep) {
        const float half = 0.05f * static_cast<float>(sizeStep);
        for (int offsetStep = 0; offsetStep < 16; ++offsetStep) {
            const float offset = 0.013f * static_cast<float>(offsetStep);
            const hpl::StandardCullScreenRect rect = hpl::standardCullProjectAabb(
                camera, -half + offset, -half + offset, 5.0f,
                half + offset, half + offset, 6.0f);
            if (rect.testable == 0u)
                continue;
            const uint32_t mipWidth = std::max<uint32_t>(1u, kSize >> rect.mip);
            const uint32_t mipHeight = std::max<uint32_t>(1u, kSize >> rect.mip);
            // The same texel indices StandardHiZSampleFarthest computes.
            const auto texel = [](float uv, uint32_t extent) {
                const float clamped = uv < 0.0f ? 0.0f : (uv > 1.0f ? 1.0f : uv);
                uint32_t t = static_cast<uint32_t>(clamped * static_cast<float>(extent));
                return t >= extent ? extent - 1u : t;
            };
            ASSERT_TRUE(texel(rect.maxU, mipWidth) - texel(rect.minU, mipWidth) <= 1u);
            ASSERT_TRUE(texel(rect.maxV, mipHeight) - texel(rect.minV, mipHeight) <= 1u);
        }
    }
}

// End-to-end form of the same bug: an occluder wall with a single hole punched
// through it. Anything visible through the hole must survive the cull wherever
// the hole falls inside its rect. The max-reduce carries the hole up every mip,
// so a correct gather always finds it.
UTEST(StandardShadowCull, OcclusionKeepsGeometryVisibleThroughAHole) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const uint32_t kSize = 256;
    const float wall = NdcDepthAt(viewProjection, 10.0f);
    std::vector<float> mip0(kSize * kSize, wall);

    // One hole, far away (nothing drawn there).
    const uint32_t holeX = 128;
    const uint32_t holeY = 128;
    mip0[static_cast<size_t>(holeY) * kSize + holeX] = 1.0f;
    Pyramid pyramid = BuildPyramid(kSize, kSize, mip0);
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // Boxes well behind the wall, swept across sizes and offsets. Every one
    // whose rect contains the hole must be kept.
    for (int sizeStep = 1; sizeStep <= 24; ++sizeStep) {
        const float half = 0.08f * static_cast<float>(sizeStep);
        for (int offsetStep = -6; offsetStep <= 6; ++offsetStep) {
            const float offset = 0.05f * static_cast<float>(offsetStep);
            const float minX = -half + offset;
            const float maxX = half + offset;
            const hpl::StandardCullScreenRect rect = hpl::standardCullProjectAabb(
                camera, minX, -half, 40.0f, maxX, half, 41.0f);
            if (rect.testable == 0u)
                continue;
            // Does the hole's texel fall inside this rect at mip 0?
            const float holeU = (static_cast<float>(holeX) + 0.5f) / static_cast<float>(kSize);
            const float holeV = (static_cast<float>(holeY) + 0.5f) / static_cast<float>(kSize);
            const bool holeInside = holeU >= rect.minU && holeU <= rect.maxU &&
                                    holeV >= rect.minV && holeV <= rect.maxV;
            if (!holeInside)
                continue;
            ASSERT_FALSE(hpl::StandardCullOcclusionReference(
                camera, pyramid.View(), minX, -half, 40.0f, maxX, half, 41.0f));
        }
    }
}

UTEST(StandardShadowCull, OcclusionMipCoversTheWholeRect) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.0f, 0.1f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const uint32_t kSize = 64;
    Pyramid pyramid = BuildPyramid(kSize, kSize,
                                   std::vector<float>(kSize * kSize, NdcDepthAt(viewProjection, 30.0f)));
    const StandardCullCamera camera = MakeCamera(viewProjection, kSize, kSize, pyramid.mipCount);

    // A big near box spans most of the image, so the chosen mip has to be
    // coarse enough that four samples still cover it.
    const hpl::StandardCullScreenRect wide =
        hpl::standardCullProjectAabb(camera, -20.0f, -20.0f, 20.0f, 20.0f, 20.0f, 21.0f);
    ASSERT_EQ(wide.testable, 1u);
    const float widthInTexels = (wide.maxU - wide.minU) * static_cast<float>(kSize);
    const float heightInTexels = (wide.maxV - wide.minV) * static_cast<float>(kSize);
    const float mipTexels = static_cast<float>(1u << wide.mip);
    // One texel, not two: the four-corner gather only covers a rect that
    // straddles at most two adjacent texels per axis.
    ASSERT_TRUE(std::max(widthInTexels, heightInTexels) <= mipTexels);
    ASSERT_TRUE(wide.mip < pyramid.mipCount);

    // A tiny distant box stays at a fine mip.
    const hpl::StandardCullScreenRect tiny =
        hpl::standardCullProjectAabb(camera, -0.05f, -0.05f, 80.0f, 0.05f, 0.05f, 80.1f);
    ASSERT_EQ(tiny.testable, 1u);
    ASSERT_TRUE(tiny.mip < wide.mip);
}

// ---------------------------------------------------------------------------
// Two-phase camera cull
//
// The pair of phases has to hold two properties for every possible state: no
// object is ever rasterized twice, and no visible object is missed. That is
// four states of (was visible last frame) x (visible now), and the cost of
// getting one wrong is either double-drawing or geometry silently vanishing,
// neither of which a screenshot makes obvious.
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, TwoPhaseDrawsEveryVisibleObjectExactlyOnce) {
    for (uint32_t wasVisible = 0u; wasVisible <= 1u; ++wasVisible) {
        for (int frustum = 0; frustum < 2; ++frustum) {
            for (int occluded = 0; occluded < 2; ++occluded) {
                const bool frustumKeep = frustum != 0;
                // Phase 2's keep carries frustum AND occlusion; phase 1's is
                // the frustum alone, because no pyramid exists yet.
                const bool visibleNow = frustumKeep && occluded == 0;

                const uint32_t phaseOne =
                    hpl::standardCullReplayInstanceCount(frustumKeep, wasVisible);
                const uint32_t phaseTwo =
                    hpl::standardCullUpdateInstanceCount(visibleNow, wasVisible);

                // Never twice.
                ASSERT_TRUE(phaseOne + phaseTwo <= 1u);
                // Never missed: anything actually visible is drawn by one phase.
                if (visibleNow)
                    ASSERT_EQ(phaseOne + phaseTwo, 1u);
                // The stored answer is exactly this frame's visibility.
                ASSERT_EQ(hpl::standardCullNextVisibility(visibleNow),
                          visibleNow ? 1u : 0u);
            }
        }
    }
}

UTEST(StandardShadowCull, TwoPhaseCaseByCase) {
    // Still visible: phase 1 draws it from last frame's answer, phase 2 must
    // not draw it again.
    ASSERT_EQ(hpl::standardCullReplayInstanceCount(true, 1u), 1u);
    ASSERT_EQ(hpl::standardCullUpdateInstanceCount(true, 1u), 0u);

    // Newly visible: phase 1 knows nothing about it, phase 2 picks it up. This
    // is the case a single-phase cull against last frame's pyramid gets wrong,
    // and the whole reason for the second phase.
    ASSERT_EQ(hpl::standardCullReplayInstanceCount(true, 0u), 0u);
    ASSERT_EQ(hpl::standardCullUpdateInstanceCount(true, 0u), 1u);

    // Newly occluded: phase 1 still draws it on last frame's answer -- wasted,
    // but depth rejects it -- and phase 2 correctly declines.
    ASSERT_EQ(hpl::standardCullReplayInstanceCount(true, 1u), 1u);
    ASSERT_EQ(hpl::standardCullUpdateInstanceCount(false, 1u), 0u);
    ASSERT_EQ(hpl::standardCullNextVisibility(false), 0u);

    // Outside the frustum: the frustum gate applies to phase 1 too, so a stale
    // visible bit cannot drag off-screen geometry into the draw.
    ASSERT_EQ(hpl::standardCullReplayInstanceCount(false, 1u), 0u);
    ASSERT_EQ(hpl::standardCullUpdateInstanceCount(false, 1u), 0u);
}

UTEST(StandardShadowCull, TwoPhaseFirstFrameDrawsEverythingInPhaseTwo) {
    // The visibility table starts zeroed, so the first frame after a create has
    // an empty phase 1 and a phase 2 that draws the whole visible set. A
    // non-zero initial table would instead skip geometry on frame one.
    const uint32_t cold = 0u;
    ASSERT_EQ(hpl::standardCullReplayInstanceCount(true, cold), 0u);
    ASSERT_EQ(hpl::standardCullUpdateInstanceCount(true, cold), 1u);
}

// ---------------------------------------------------------------------------
// Candidate gates
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, NonCastersAreRejected) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const StandardCullTile tile = MakeTile(viewProjection, kBothVariability);

    const StandardCullCandidate caster = MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 3u, 99u, kCaster);
    ASSERT_TRUE(hpl::standardCullKeepCandidate(tile, caster));

    const StandardCullCandidate notCaster = MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 3u, 99u, 0u);
    ASSERT_FALSE(hpl::standardCullKeepCandidate(tile, notCaster));
}

// The static/dynamic gate is per light (iLight::GetShadowCastersAffected), so
// it cannot be hoisted into the frame-global gather -- it has to stay in the
// kernel, evaluated per tile.
UTEST(StandardShadowCull, VariabilityMaskMatrix) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    const StandardCullCandidate staticCaster =
        MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 1u, 6u, kStaticCaster);
    const StandardCullCandidate dynamicCaster =
        MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 2u, 6u, kCaster);

    const StandardCullTile staticOnly = MakeTile(viewProjection, kStandardCullVariabilityStatic);
    ASSERT_TRUE(hpl::standardCullKeepCandidate(staticOnly, staticCaster));
    ASSERT_FALSE(hpl::standardCullKeepCandidate(staticOnly, dynamicCaster));

    const StandardCullTile dynamicOnly = MakeTile(viewProjection, kStandardCullVariabilityDynamic);
    ASSERT_FALSE(hpl::standardCullKeepCandidate(dynamicOnly, staticCaster));
    ASSERT_TRUE(hpl::standardCullKeepCandidate(dynamicOnly, dynamicCaster));

    const StandardCullTile both = MakeTile(viewProjection, kBothVariability);
    ASSERT_TRUE(hpl::standardCullKeepCandidate(both, staticCaster));
    ASSERT_TRUE(hpl::standardCullKeepCandidate(both, dynamicCaster));

    const StandardCullTile neither = MakeTile(viewProjection, 0u);
    ASSERT_FALSE(hpl::standardCullKeepCandidate(neither, staticCaster));
    ASSERT_FALSE(hpl::standardCullKeepCandidate(neither, dynamicCaster));
}

// ---------------------------------------------------------------------------
// Reference kernel
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
    std::vector<StandardCullCandidate> candidates;
    StandardCullTile tile{};
};

// Half the candidates are in front of the camera, half far behind it.
Fixture BuildMixedFixture(uint32_t count) {
    Fixture fixture;
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    fixture.tile = MakeTile(viewProjection, kBothVariability);
    fixture.tile.candidateBase = 0u;
    fixture.tile.candidateCount = count;

    for (uint32_t i = 0; i < count; ++i) {
        const bool visible = (i % 2u) == 0u;
        const float z = visible ? 10.0f : -50.0f;
        fixture.candidates.push_back(
            MakeCandidate(0.0f, 0.0f, z, 0.5f, 100u + i, 30u + i, kCaster));
    }
    return fixture;
}

} // namespace

UTEST(StandardShadowCull, CompactModePacksSurvivorsToTheFront) {
    Fixture fixture = BuildMixedFixture(16u);
    std::vector<StandardDrawIndirect> out(fixture.tile.candidateCount);

    const uint32_t written = hpl::StandardShadowCullReference(
        fixture.candidates.data(), fixture.candidates.size(), fixture.tile,
        kStandardCullModeCompact, out.data());

    ASSERT_EQ(written, 8u);
    for (uint32_t i = 0; i < written; ++i) {
        // Survivors are the even indices, in order.
        const StandardCullCandidate& source = fixture.candidates[i * 2u];
        ASSERT_EQ(out[i].vertexCount, source.vertexCount);
        ASSERT_EQ(out[i].instanceCount, 1u);
        ASSERT_EQ(out[i].firstVertex, 0u);
        // firstInstance carries the object slot; the vertex shader reads it
        // back as SV_StartInstanceLocation to index gSceneObjects.
        ASSERT_EQ(out[i].firstInstance, source.objectSlot);
    }
}

// The reserved range is the candidate count, so the survivor count can never
// reach it. This is what replaces the old "shadowDrawCount != casters.size()"
// hard fail: it becomes an invariant rather than a runtime check.
UTEST(StandardShadowCull, CompactModeNeverOverrunsTheReservation) {
    std::mt19937 rng(99u);
    std::uniform_int_distribution<int> countDistribution(1, 64);

    for (int iteration = 0; iteration < 200; ++iteration) {
        const uint32_t count = static_cast<uint32_t>(countDistribution(rng));
        Fixture fixture = BuildMixedFixture(count);
        std::vector<StandardDrawIndirect> out(count);

        const uint32_t written = hpl::StandardShadowCullReference(
            fixture.candidates.data(), fixture.candidates.size(), fixture.tile,
            kStandardCullModeCompact, out.data());

        ASSERT_LE(written, count);
    }
}

UTEST(StandardShadowCull, InPlaceModeWritesEverySlotAndZeroesCulledOnes) {
    Fixture fixture = BuildMixedFixture(16u);
    std::vector<StandardDrawIndirect> compact(fixture.tile.candidateCount);
    std::vector<StandardDrawIndirect> inPlace(fixture.tile.candidateCount);

    const uint32_t compactWritten = hpl::StandardShadowCullReference(
        fixture.candidates.data(), fixture.candidates.size(), fixture.tile,
        kStandardCullModeCompact, compact.data());
    const uint32_t inPlaceWritten = hpl::StandardShadowCullReference(
        fixture.candidates.data(), fixture.candidates.size(), fixture.tile,
        kStandardCullModeInPlace, inPlace.data());

    ASSERT_EQ(inPlaceWritten, fixture.tile.candidateCount);

    uint32_t survivors = 0;
    for (uint32_t i = 0; i < fixture.tile.candidateCount; ++i) {
        ASSERT_EQ(inPlace[i].firstInstance, fixture.candidates[i].objectSlot);
        ASSERT_EQ(inPlace[i].vertexCount, fixture.candidates[i].vertexCount);
        if (inPlace[i].instanceCount == 1u) {
            // The two modes must agree on WHICH candidates survive; only the
            // packing differs.
            ASSERT_EQ(compact[survivors].firstInstance, inPlace[i].firstInstance);
            ++survivors;
        } else {
            ASSERT_EQ(inPlace[i].instanceCount, 0u);
        }
    }
    ASSERT_EQ(survivors, compactWritten);
}

UTEST(StandardShadowCull, TileReadsOnlyItsOwnCandidateSlice) {
    Fixture fixture = BuildMixedFixture(16u);
    // Point the tile at the second half of the array.
    fixture.tile.candidateBase = 8u;
    fixture.tile.candidateCount = 8u;

    std::vector<StandardDrawIndirect> out(8);
    const uint32_t written = hpl::StandardShadowCullReference(
        fixture.candidates.data(), fixture.candidates.size(), fixture.tile,
        kStandardCullModeCompact, out.data());

    ASSERT_EQ(written, 4u);
    for (uint32_t i = 0; i < written; ++i) {
        ASSERT_EQ(out[i].firstInstance, fixture.candidates[8u + i * 2u].objectSlot);
    }
}

// ---------------------------------------------------------------------------
// Sphere/AABB overlap, the per-frame gather's per-light predicate
// ---------------------------------------------------------------------------

UTEST(StandardShadowCull, SphereOverlapsAabbCases) {
    const float minimum[3] = {-1.0f, -1.0f, -1.0f};
    const float maximum[3] = {1.0f, 1.0f, 1.0f};

    // Centre inside.
    ASSERT_TRUE(hpl::StandardSphereOverlapsAabb(0.0f, 0.0f, 0.0f, 0.1f,
                                                minimum[0], minimum[1], minimum[2],
                                                maximum[0], maximum[1], maximum[2]));
    // Outside but reaching in.
    ASSERT_TRUE(hpl::StandardSphereOverlapsAabb(3.0f, 0.0f, 0.0f, 2.5f,
                                                minimum[0], minimum[1], minimum[2],
                                                maximum[0], maximum[1], maximum[2]));
    // Exactly touching a face.
    ASSERT_TRUE(hpl::StandardSphereOverlapsAabb(3.0f, 0.0f, 0.0f, 2.0f,
                                                minimum[0], minimum[1], minimum[2],
                                                maximum[0], maximum[1], maximum[2]));
    // Short of the face.
    ASSERT_FALSE(hpl::StandardSphereOverlapsAabb(3.0f, 0.0f, 0.0f, 1.9f,
                                                 minimum[0], minimum[1], minimum[2],
                                                 maximum[0], maximum[1], maximum[2]));
    // Corner distance is the diagonal, not the axis distance: a radius that
    // would reach a face must not reach the corner.
    ASSERT_FALSE(hpl::StandardSphereOverlapsAabb(3.0f, 3.0f, 3.0f, 2.1f,
                                                 minimum[0], minimum[1], minimum[2],
                                                 maximum[0], maximum[1], maximum[2]));
    // Zero radius degenerates to point-in-box.
    ASSERT_TRUE(hpl::StandardSphereOverlapsAabb(0.5f, 0.5f, 0.5f, 0.0f,
                                                minimum[0], minimum[1], minimum[2],
                                                maximum[0], maximum[1], maximum[2]));
    ASSERT_FALSE(hpl::StandardSphereOverlapsAabb(1.5f, 0.0f, 0.0f, 0.0f,
                                                 minimum[0], minimum[1], minimum[2],
                                                 maximum[0], maximum[1], maximum[2]));
}

// ---------------------------------------------------------------------------
// Instance-mask mode.
//
// This is the mode both translucent paths run in -- Standard's translucent pass
// and every one of the Hybrid renderer's three translucent families. Unlike
// COMPACT it must not move anything: blended geometry is drawn back-to-front,
// so the host's slot order IS the correctness condition. The kernel owns
// exactly one word per candidate, the instanceCount at commandWordOffset.
// ---------------------------------------------------------------------------

namespace {
// The command ring as the kernel sees it: a flat uint[] of 5-word slots.
constexpr uint32_t kMaskCommandWords = 5u;

// A 5-word indexed command with a recognisable filler in every word the kernel
// must not touch.
void WriteMaskCommand(std::vector<uint32_t>& words, uint32_t slot,
                      uint32_t indexCount, uint32_t objectSlot) {
    uint32_t* w = words.data() + static_cast<size_t>(slot) * kMaskCommandWords;
    w[0] = indexCount;
    w[1] = 1u;          // instanceCount -- the only word the kernel owns
    w[2] = 0xAAAAAAAAu; // firstIndex, sentinel
    w[3] = 0xBBBBBBBBu; // vertexOffset, sentinel
    w[4] = objectSlot;
}
} // namespace

UTEST(StandardShadowCull, InstanceMaskWritesOnlyTheInstanceCountWord) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    StandardCullTile tile = MakeTile(viewProjection, kBothVariability);

    // Three candidates: dead ahead (kept), far behind the camera (frustum
    // rejects it), and a non-caster (the render-flag gate rejects it).
    std::vector<StandardCullCandidate> candidates;
    candidates.push_back(MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 11u, 33u, kCaster));
    candidates.push_back(MakeCandidate(0.0f, 0.0f, -40.0f, 1.0f, 22u, 44u, kCaster));
    candidates.push_back(MakeCandidate(0.0f, 0.0f, 10.0f, 1.0f, 33u, 55u, /*renderFlags=*/0u));

    // Put the slice at a non-zero base, as a ring segment would.
    const uint32_t commandBase = 7u;
    std::vector<uint32_t> words((commandBase + candidates.size()) * kMaskCommandWords, 0u);
    for (uint32_t i = 0; i < candidates.size(); ++i) {
        const uint32_t slot = commandBase + i;
        WriteMaskCommand(words, slot, candidates[i].vertexCount, candidates[i].objectSlot);
        candidates[i].commandWordOffset = slot * kMaskCommandWords + 1u;
    }
    const std::vector<uint32_t> before = words;

    tile.candidateBase = 0u;
    tile.candidateCount = static_cast<uint32_t>(candidates.size());
    const uint32_t kept = hpl::StandardCullInstanceMaskReference(
        candidates.data(), candidates.size(), tile, /*camera=*/nullptr,
        /*pyramid=*/nullptr, words.data(), words.size());

    ASSERT_EQ(1u, kept);
    ASSERT_EQ(1u, words[(commandBase + 0u) * kMaskCommandWords + 1u]);
    ASSERT_EQ(0u, words[(commandBase + 1u) * kMaskCommandWords + 1u]);
    ASSERT_EQ(0u, words[(commandBase + 2u) * kMaskCommandWords + 1u]);

    // Every other word -- including the words of slots 0..commandBase-1, which
    // belong to a frame still in flight -- is untouched.
    for (size_t i = 0; i < words.size(); ++i) {
        if (i % kMaskCommandWords == 1u &&
            i >= static_cast<size_t>(commandBase) * kMaskCommandWords) {
            continue;
        }
        ASSERT_EQ(before[i], words[i]);
    }
}

UTEST(StandardShadowCull, InstanceMaskKeepsHostSlotOrder) {
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    StandardCullTile tile = MakeTile(viewProjection, kBothVariability);

    // Alternating keep / reject. A compacting mode would pack the survivors to
    // the front and destroy the back-to-front order; this mode must leave each
    // survivor in the slot the host sorted it into.
    std::vector<StandardCullCandidate> candidates;
    for (uint32_t i = 0; i < 8u; ++i) {
        const bool visible = (i % 2u) == 0u;
        candidates.push_back(MakeCandidate(0.0f, 0.0f, visible ? 10.0f : -40.0f, 1.0f,
                                           100u + i, 3u * (i + 1u), kCaster));
    }
    std::vector<uint32_t> words(candidates.size() * kMaskCommandWords, 0u);
    for (uint32_t i = 0; i < candidates.size(); ++i) {
        WriteMaskCommand(words, i, candidates[i].vertexCount, candidates[i].objectSlot);
        candidates[i].commandWordOffset = i * kMaskCommandWords + 1u;
    }

    tile.candidateBase = 0u;
    tile.candidateCount = static_cast<uint32_t>(candidates.size());
    const uint32_t kept = hpl::StandardCullInstanceMaskReference(
        candidates.data(), candidates.size(), tile, nullptr, nullptr, words.data(),
        words.size());
    ASSERT_EQ(4u, kept);

    for (uint32_t i = 0; i < candidates.size(); ++i) {
        const uint32_t* w = words.data() + static_cast<size_t>(i) * kMaskCommandWords;
        // The command still describes ITS OWN candidate: nothing moved.
        ASSERT_EQ(candidates[i].vertexCount, w[0]);
        ASSERT_EQ(candidates[i].objectSlot, w[4]);
        ASSERT_EQ((i % 2u) == 0u ? 1u : 0u, w[1]);
    }
}

UTEST(StandardShadowCull, InstanceMaskHidesAnOccludedDrawAndHonoursNeverOcclude) {
    // One flat occluder filling the pyramid at mid depth; the camera looks
    // down +Z from the origin.
    float viewProjection[16];
    ViewProjection(viewProjection, 1.2f, 1.7778f, 0.05f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    StandardCullTile tile = MakeTile(viewProjection, kBothVariability);
    tile.cameraIndex = 0u;

    StandardCullCamera camera{};
    std::memcpy(camera.viewProjection, viewProjection, sizeof(camera.viewProjection));
    camera.hiZWidth = 8u;
    camera.hiZHeight = 8u;
    camera.hiZMipCount = 4u;

    // Depth of a point 20 units ahead: everything stored at that depth hides
    // anything farther away.
    const float occluderZ = 20.0f;
    const float zNear = 0.05f, zFar = 100.0f;
    const float occluderDepth = (zFar / (zFar - zNear)) * (occluderZ - zNear) / occluderZ;

    std::vector<float> texels;
    for (uint32_t mip = 0; mip < camera.hiZMipCount; ++mip) {
        const uint32_t w = std::max<uint32_t>(1u, camera.hiZWidth >> mip);
        const uint32_t h = std::max<uint32_t>(1u, camera.hiZHeight >> mip);
        texels.insert(texels.end(), static_cast<size_t>(w) * h, occluderDepth);
    }
    hpl::StandardHiZPyramid pyramid{texels.data(), camera.hiZWidth, camera.hiZHeight,
                                    camera.hiZMipCount};

    // Both boxes sit well behind the occluder, dead ahead. One is marked
    // never-occlude, as a refractive draw would be.
    std::vector<StandardCullCandidate> candidates;
    candidates.push_back(MakeCandidate(0.0f, 0.0f, 60.0f, 1.0f, 1u, 3u, kCaster));
    candidates.push_back(MakeCandidate(0.0f, 0.0f, 60.0f, 1.0f, 2u, 3u, kCaster));
    candidates[1].cullFlags = kStandardCullFlagNeverOcclude;

    std::vector<uint32_t> words(candidates.size() * kMaskCommandWords, 0u);
    for (uint32_t i = 0; i < candidates.size(); ++i) {
        WriteMaskCommand(words, i, candidates[i].vertexCount, candidates[i].objectSlot);
        candidates[i].commandWordOffset = i * kMaskCommandWords + 1u;
    }

    tile.candidateBase = 0u;
    tile.candidateCount = static_cast<uint32_t>(candidates.size());
    hpl::StandardCullInstanceMaskReference(candidates.data(), candidates.size(), tile,
                                           &camera, &pyramid, words.data(), words.size());

    // Hidden behind the occluder.
    ASSERT_EQ(0u, words[0 * kMaskCommandWords + 1u]);
    // Same geometry, but opted out of the depth test.
    ASSERT_EQ(1u, words[1 * kMaskCommandWords + 1u]);

    // Guard against a vacuous pass: with no pyramid, the frustum test alone
    // keeps both.
    std::vector<uint32_t> frustumOnly(words.size(), 0u);
    for (uint32_t i = 0; i < candidates.size(); ++i)
        WriteMaskCommand(frustumOnly, i, candidates[i].vertexCount, candidates[i].objectSlot);
    hpl::StandardCullInstanceMaskReference(candidates.data(), candidates.size(), tile,
                                           &camera, /*pyramid=*/nullptr,
                                           frustumOnly.data(), frustumOnly.size());
    ASSERT_EQ(1u, frustumOnly[0 * kMaskCommandWords + 1u]);
    ASSERT_EQ(1u, frustumOnly[1 * kMaskCommandWords + 1u]);
}
