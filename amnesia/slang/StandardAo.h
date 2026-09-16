#pragma once

#include "HostDefinitions.h"

// Shared host/device layout math for the Standard renderer's deinterleaved AO.
//
// The pass splits the screen into 4x4 blocks and computes AO in 16 quarter
// resolution slices, slice s owning pixel phase (s%4, s/4) of every block. Three
// compute passes have to agree on that mapping exactly -- prepare writes it,
// coarse reads and writes it, reinterleave scatters it back -- and slice index
// maps straight onto pixel phase, so ANY quantity that varies per slice when it
// should not becomes a visible 4x4 grid.
//
// The mapping lives here rather than being written out three times so the
// headless tests exercise the same arithmetic the shaders run, the same
// arrangement as StandardCull.h and StandardShadowFilter.h.

#define kStandardAoBlockSize 4u
#define kStandardAoSlices    16u
// Entries in the fixed rotation table the coarse pass indexes.
#define kStandardAoJitterCount 32u

HOST_NAMESPACE_BEGIN

// Quarter-resolution extent for a full-resolution one.
//
// CEIL, not floor: flooring drops the rightmost columns and bottom rows of an
// extent that is not a multiple of four, leaving those pixels with no slice and
// therefore no AO -- a seam down the edge of the viewport.
SLANG_PUBLIC inline uint standardAoQuarterExtent(uint fullExtent)
{
    return (fullExtent + kStandardAoBlockSize - 1u) / kStandardAoBlockSize;
}

// Pixel phase within a 4x4 block that this slice owns.
SLANG_PUBLIC inline uint2 standardAoSliceOffset(uint slice)
{
    return uint2(slice % kStandardAoBlockSize, slice / kStandardAoBlockSize);
}

// The full-resolution pixel a (quarter texel, slice) pair owns.
SLANG_PUBLIC inline uint2 standardAoSlicePixel(uint2 quarterPos, uint slice)
{
    uint2 offset = standardAoSliceOffset(slice);
    return uint2(quarterPos.x * kStandardAoBlockSize + offset.x,
                 quarterPos.y * kStandardAoBlockSize + offset.y);
}

// Which slice owns a given full-resolution pixel, and which quarter texel.
SLANG_PUBLIC inline uint standardAoPixelSlice(uint2 fullPos)
{
    return (fullPos.y % kStandardAoBlockSize) * kStandardAoBlockSize +
           (fullPos.x % kStandardAoBlockSize);
}

SLANG_PUBLIC inline uint2 standardAoPixelQuarter(uint2 fullPos)
{
    return uint2(fullPos.x / kStandardAoBlockSize, fullPos.y / kStandardAoBlockSize);
}

// Snap a ray sample to the phase its slice actually stores.
//
// A ray marches in full-resolution pixels, but the depth for the sample can only
// be fetched from the marching slice, which holds one phase per block. Returning
// the phased pixel lets the caller build its uv from the SAME pixel the depth
// came from: pairing an exact xy with a depth up to three pixels away puts a
// per-slice bias into the reconstructed position, and per-slice bias is a grid.
SLANG_PUBLIC inline uint2 standardAoPhasedSamplePixel(uint2 samplePos, uint slice)
{
    uint2 block = standardAoPixelQuarter(samplePos);
    return standardAoSlicePixel(block, slice);
}

// Rotation-table index for one (quarter texel, slice).
//
// Varies per SLICE first. That is what decorrelates the 16 pixels inside a
// single 4x4 block; indexing on the quarter coordinate alone gives all 16 of
// them the same rotation and paints the block-sized grid this function exists to
// avoid. The block is folded in as well so the 16-rotation pattern does not
// repeat identically in every block across the screen.
SLANG_PUBLIC inline uint standardAoJitterIndex(uint2 quarterPos, uint slice)
{
    uint blockHash = (quarterPos.x * 73856093u) ^ (quarterPos.y * 19349663u);
    return (slice * 2u + blockHash) % kStandardAoJitterCount;
}

HOST_NAMESPACE_END
