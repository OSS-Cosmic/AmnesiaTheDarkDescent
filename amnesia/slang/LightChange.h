#pragma once

#include "HostDefinitions.h"

// Light-change tracking for the ray-traced direct-lighting history.
//
// A flickering light changes radiance without disoccluding anything, so the
// ReSTIR temporal merge (depth/normal-keyed) keeps a reservoir weighted up to
// kReservoirMClamp× the fresh one and a light that just switched on has to
// outvote it for several frames. The host stamps each GPU light with last
// frame's power and reach (prevPower / prevRadius), LightGridBuildPass weighs
// the light into each cell as it was and as it is, folds the difference into
// the fraction of each cell's lighting that changed, and packs it into the
// spare high bits of gLightGridCount. DirectLightingPass shrinks the reservoir
// history cap by it and writes it out as NRD history confidence.
//
// Absolute previous values rather than a prev/cur ratio: a light that switches
// off or fades to zero has no current power to scale, yet it is exactly the
// change the denoisers ghost worst.
//
// Scalar arithmetic only, shared with headless C++ tests (tests/graphics).

HOST_NAMESPACE_BEGIN

// Host-side power proxy: brightest linear channel × intensity, 0 for a light
// the grid skips (radius 0). Same factor cellLightWeight ranks on, so the grid
// can rebuild last frame's cell weight from it.
SLANG_PUBLIC inline float lightChangePower(float maxChannel, float intensity, float radius)
{
    return radius > 0.0f ? maxChannel * intensity : 0.0f;
}

// Fraction of a cell's lighting that changed: sum |w - wPrev| over the larger
// of the two totals, in [0, 1]. Accumulate sumDelta / sumCur / sumPrev per
// light from its current and previous cell weight.
SLANG_PUBLIC inline float lightChangeFraction(float sumDelta, float sumCur, float sumPrev)
{
    float total = sumCur > sumPrev ? sumCur : sumPrev;
    if (total <= 0.0f)
        return 0.0f;
    float f = sumDelta / total;
    return f > 1.0f ? 1.0f : f;
}

// gLightGridCount layout: low 16 bits = light count (<= kLightsPerCellMax),
// bits 16..23 = change fraction quantized to 8 bits.
SLANG_PUBLIC inline uint lightGridPackCount(uint count, float changeFraction)
{
    float f = changeFraction < 0.0f ? 0.0f : (changeFraction > 1.0f ? 1.0f : changeFraction);
    uint q = uint(f * 255.0f + 0.5f);
    return (count & 0xFFFFu) | (q << 16u);
}

SLANG_PUBLIC inline uint lightGridUnpackCount(uint packed)
{
    return packed & 0xFFFFu;
}

SLANG_PUBLIC inline float lightGridUnpackChange(uint packed)
{
    return float((packed >> 16u) & 0xFFu) * (1.0f / 255.0f);
}

// Temporal history cap (× current M) for a cell whose lighting changed by
// `changeFraction`: mClamp when nothing changed, falling linearly to 1 (history
// no heavier than the fresh reservoir) once `fullResetFraction` of the cell's
// lighting changed.
SLANG_PUBLIC inline float lightChangeHistoryClamp(float changeFraction, float mClamp,
                                                  float fullResetFraction)
{
    float t = fullResetFraction > 0.0f ? changeFraction / fullResetFraction : 1.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return mClamp + (1.0f - mClamp) * t;
}

// NRD history confidence (IN_DIFF/SPEC_CONFIDENCE, 0 = drop history, 1 = keep)
// for a cell whose lighting changed by `changeFraction`: 1 when nothing
// changed, falling linearly to `minConfidence` once `fullResetFraction` of the
// cell's lighting changed.
SLANG_PUBLIC inline float lightChangeHistoryConfidence(float changeFraction,
                                                       float fullResetFraction,
                                                       float minConfidence)
{
    float t = fullResetFraction > 0.0f ? changeFraction / fullResetFraction : 1.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 1.0f + (minConfidence - 1.0f) * t;
}

HOST_NAMESPACE_END
