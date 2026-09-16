// Headless coverage for the Standard renderer's deinterleaved AO layout.
//
// The AO pass had no tests at all, which is how a 4x4 grid artifact survived: it
// is only visible on a GPU, on a wall, at the right angle. The layout arithmetic
// is pure integer math, so it can be pinned here — and slice index maps directly
// onto pixel phase, which makes "varies per slice when it should not" the single
// failure mode worth testing for.
//
// The shaders include the same header, so this is the arithmetic the GPU runs.

#include "../../amnesia/slang/StandardAo.h"
#include "utest.h"

#include <set>
#include <vector>

namespace {

// Every full-resolution pixel a dispatch would write, for one viewport.
std::vector<int> CoverageCount(uint32_t width, uint32_t height) {
    const uint32_t quarterW = hpl::standardAoQuarterExtent(width);
    const uint32_t quarterH = hpl::standardAoQuarterExtent(height);
    std::vector<int> hits(static_cast<size_t>(width) * height, 0);
    for (uint32_t qy = 0; qy < quarterH; ++qy) {
        for (uint32_t qx = 0; qx < quarterW; ++qx) {
            for (uint32_t slice = 0; slice < kStandardAoSlices; ++slice) {
                const hpl::uint2 pixel =
                    hpl::standardAoSlicePixel(hpl::uint2(qx, qy), slice);
                // The shaders bounds-check before writing; mirror that.
                if (pixel.x >= width || pixel.y >= height)
                    continue;
                ++hits[static_cast<size_t>(pixel.y) * width + pixel.x];
            }
        }
    }
    return hits;
}

} // namespace

UTEST(StandardAoLayout, EveryPixelIsCoveredExactlyOnce) {
    // Odd extents are the interesting ones: a floored quarter extent drops the
    // right and bottom edges, which is a seam down the side of the viewport.
    const uint32_t sizes[][2] = {
        {1, 1}, {4, 4}, {7, 5}, {255, 129}, {1920, 1080}, {13, 4}, {4, 13},
    };
    for (const auto &size : sizes) {
        const std::vector<int> hits = CoverageCount(size[0], size[1]);
        for (size_t i = 0; i < hits.size(); ++i) {
            ASSERT_EQ(hits[i], 1);
        }
    }
}

UTEST(StandardAoLayout, QuarterExtentRoundsUp) {
    ASSERT_EQ(hpl::standardAoQuarterExtent(1u), 1u);
    ASSERT_EQ(hpl::standardAoQuarterExtent(4u), 1u);
    ASSERT_EQ(hpl::standardAoQuarterExtent(5u), 2u);
    ASSERT_EQ(hpl::standardAoQuarterExtent(7u), 2u);
    ASSERT_EQ(hpl::standardAoQuarterExtent(8u), 2u);
    // Flooring here would leave the last three columns with no slice at all.
    ASSERT_EQ(hpl::standardAoQuarterExtent(1920u), 480u);
    ASSERT_EQ(hpl::standardAoQuarterExtent(1921u), 481u);
}

UTEST(StandardAoLayout, SliceAndPixelMappingsAreInverses) {
    for (uint32_t qy = 0; qy < 8; ++qy) {
        for (uint32_t qx = 0; qx < 8; ++qx) {
            for (uint32_t slice = 0; slice < kStandardAoSlices; ++slice) {
                const hpl::uint2 pixel =
                    hpl::standardAoSlicePixel(hpl::uint2(qx, qy), slice);
                ASSERT_EQ(hpl::standardAoPixelSlice(pixel), slice);
                const hpl::uint2 quarter = hpl::standardAoPixelQuarter(pixel);
                ASSERT_EQ(quarter.x, qx);
                ASSERT_EQ(quarter.y, qy);
            }
        }
    }
}

UTEST(StandardAoLayout, PhasedSampleMatchesTheSliceItsDepthComesFrom) {
    // A ray marches in full-resolution pixels, but its depth can only come from
    // the marching slice. The phased pixel must be the one that slice actually
    // stores for that block, or the reconstructed position pairs an xy with a
    // depth from a different pixel — a per-slice bias, i.e. a grid.
    for (uint32_t slice = 0; slice < kStandardAoSlices; ++slice) {
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                const hpl::uint2 sample(x, y);
                const hpl::uint2 phased =
                    hpl::standardAoPhasedSamplePixel(sample, slice);
                // Same 4x4 block as the raw sample...
                const hpl::uint2 sampleBlock = hpl::standardAoPixelQuarter(sample);
                const hpl::uint2 phasedBlock = hpl::standardAoPixelQuarter(phased);
                ASSERT_EQ(phasedBlock.x, sampleBlock.x);
                ASSERT_EQ(phasedBlock.y, sampleBlock.y);
                // ...and owned by the slice whose depth will be fetched.
                ASSERT_EQ(hpl::standardAoPixelSlice(phased), slice);
            }
        }
    }
}

UTEST(StandardAoLayout, JitterVariesPerSliceWithinABlock) {
    // The grid artifact: indexing the rotation table on the quarter coordinate
    // alone gave all 16 pixels of a block the same rotation, so the pattern
    // changed only between blocks. Every slice must draw a different rotation.
    for (uint32_t qy = 0; qy < 4; ++qy) {
        for (uint32_t qx = 0; qx < 4; ++qx) {
            std::set<uint32_t> indices;
            for (uint32_t slice = 0; slice < kStandardAoSlices; ++slice) {
                indices.insert(hpl::standardAoJitterIndex(hpl::uint2(qx, qy), slice));
            }
            ASSERT_EQ(indices.size(), (size_t)kStandardAoSlices);
        }
    }
}

UTEST(StandardAoLayout, JitterAlsoVariesBetweenBlocks) {
    // Per-slice alone would repeat the same 16 rotations in every block, which
    // is a fainter version of the same structure. Folding the block in breaks it.
    std::set<uint32_t> firstSliceIndices;
    for (uint32_t qy = 0; qy < 8; ++qy)
        for (uint32_t qx = 0; qx < 8; ++qx)
            firstSliceIndices.insert(hpl::standardAoJitterIndex(hpl::uint2(qx, qy), 0u));
    ASSERT_TRUE(firstSliceIndices.size() > 1);

    // And it must stay deterministic: same input, same rotation, every frame.
    ASSERT_EQ(hpl::standardAoJitterIndex(hpl::uint2(3u, 7u), 5u),
              hpl::standardAoJitterIndex(hpl::uint2(3u, 7u), 5u));
    ASSERT_TRUE(hpl::standardAoJitterIndex(hpl::uint2(0u, 0u), 0u) < kStandardAoJitterCount);
}
