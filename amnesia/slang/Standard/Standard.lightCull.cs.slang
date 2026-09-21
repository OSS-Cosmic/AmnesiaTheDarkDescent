// Per-tile light culling for the Standard opaque resolve.
//
// One 16x16 thread group per screen tile. The group reduces its pixels'
// world-space positions (standardPositionInput, .w <= 0 means sky) to an AABB of
// the surfaces actually VISIBLE in that tile, then keeps the lights whose reach
// sphere touches that box. Standard.light.3d then loops that list instead of
// every light in the level.
//
// Why the position G-buffer and not the frustum or the Hi-Z pyramid: the
// question the resolve actually asks is "does this light reach a surface I can
// see?", not "is this light in view". A frustum is a cone, so in an interior map
// whole rooms of lights sit inside it behind walls; the Hi-Z stores only a far
// bound (and, at light time, phase-1 depth). The visible-surface AABB answers
// the real question directly, and handles depth discontinuities inside a tile
// without a near/far split.
//
// EXACT, never top-K. A light is dropped from a tile only when its sphere cannot
// reach any pixel in it, which is precisely when standardLightReaches would have
// returned false for every one of those pixels. A tile that cannot fit its
// lights writes kStandardLightTileOverflow and the resolve loops everything for
// that tile. The Hybrid renderer's LightGridBuildPass top-K is deliberately NOT
// copied here: Standard sums lights deterministically, so dropping one pops.
//
// Indices are emitted in ascending order so the resolve accumulates in the same
// order it does today (float addition is not associative -- unordered emit would
// change low bits).
//
// Box lights are not binned. Their blend is order-dependent (Replace discards
// earlier boxes, StandardLighting.slang) and there are only a handful, so the
// resolve keeps looping all of them.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;          // the shared kStandardLightTile* constants are emitted here
// The light records live in StandardLighting; importing it also pulls that
// module's ramp/gobo samplers into scope, but they are unreferenced here and
// drop out of reflection as used:0, so the host never has to bind them.
import StandardLighting;

[vk::binding(0, 2)] Texture2D<float4> standardCullPositionInput;
[vk::binding(1, 2)] StructuredBuffer<StandardPointLight> standardCullPointLights;
[vk::binding(2, 2)] StructuredBuffer<StandardSpotLight>  standardCullSpotLights;
[vk::binding(3, 2)] RWStructuredBuffer<uint>             standardCullTileLights;

struct StandardLightCullPC
{
    uint2 extent;      // render extent in pixels
    uint  pointCount;  // visible prefix counts, as the resolve sees them
    uint  spotCount;
    uint  tilesX;
    uint  pad0;
    uint  pad1;
    uint  pad2;
};

[vk::push_constant]
StandardLightCullPC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

static const uint kThreads = kStandardLightTileSize * kStandardLightTileSize;

groupshared float3 sMin[kThreads];
groupshared float3 sMax[kThreads];
groupshared uint   sValid[kThreads];
// One bit per light, set by whichever thread tested it; walked in order by
// thread 0 so the emitted list stays ascending without a prefix sum.
groupshared uint   sPointMask[kStandardLightCullMaskWords];
groupshared uint   sSpotMask[kStandardLightCullMaskWords];

// Sphere vs AABB: squared distance to the closest point on the box. Same test
// as Grid.slang's sphereIntersectsGridCell, on an arbitrary box.
bool standardCullSphereHitsBox(float3 center, float radius, float3 lo, float3 hi)
{
    float3 closest = clamp(center, lo, hi);
    float3 d = closest - center;
    return dot(d, d) <= radius * radius;
}

// A light only contributes where standardLightReaches passes, which needs the
// enabled bit, a finite positive radius and distance < radius. Testing the same
// conditions here keeps the list a strict superset of what the resolve shades:
// binning a light that turns out not to reach costs a loop iteration, never a
// different pixel value.
bool standardCullReachesTile(uint config, float radius, float3 position,
                             float3 lo, float3 hi)
{
    if ((config & kStandardLightEnabled) == 0u) return false;
    if (!isfinite(radius) || radius <= 0.0f) return false;
    if (!(isfinite(position.x) && isfinite(position.y) && isfinite(position.z)))
        return false;
    return standardCullSphereHitsBox(position, radius, lo, hi);
}

[shader("compute")]
[numthreads(kStandardLightTileSize, kStandardLightTileSize, 1)]
void cullLights(uint3 dispatchThreadID : SV_DispatchThreadID,
                uint3 groupID : SV_GroupID,
                uint  groupIndex : SV_GroupIndex)
{
    const uint tileIndex = groupID.x + groupID.y * gPushConstants.tilesX;
    const uint base = tileIndex * kStandardLightTileStride;

    // ---- tile AABB over visible surfaces ----
    float3 mn = float3( 1.0e30f);
    float3 mx = float3(-1.0e30f);
    uint   valid = 0u;

    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x < gPushConstants.extent.x && pixel.y < gPushConstants.extent.y)
    {
        float4 p = standardCullPositionInput.Load(int3(int2(pixel), 0));
        if (p.w > 0.0f && isfinite(p.x) && isfinite(p.y) && isfinite(p.z))
        {
            mn = p.xyz;
            mx = p.xyz;
            valid = 1u;
        }
    }

    sMin[groupIndex] = mn;
    sMax[groupIndex] = mx;
    sValid[groupIndex] = valid;
    if (groupIndex < kStandardLightCullMaskWords)
    {
        sPointMask[groupIndex] = 0u;
        sSpotMask[groupIndex] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = kThreads / 2u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            sMin[groupIndex] = min(sMin[groupIndex], sMin[groupIndex + stride]);
            sMax[groupIndex] = max(sMax[groupIndex], sMax[groupIndex + stride]);
            sValid[groupIndex] += sValid[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    const uint validCount = sValid[0];
    const float3 tileLo = sMin[0];
    const float3 tileHi = sMax[0];

    // Every pixel in the tile is sky: nothing to light, and the resolve returns
    // early for those pixels anyway.
    if (validCount == 0u)
    {
        if (groupIndex == 0u)
        {
            standardCullTileLights[base + 0u] = 0u;
            standardCullTileLights[base + 1u] = 0u;
        }
        return;
    }

    // More lights than the mask can address: fall back rather than drop any.
    const uint maskCapacity = kStandardLightCullMaskWords * 32u;
    if (gPushConstants.pointCount > maskCapacity ||
        gPushConstants.spotCount > maskCapacity)
    {
        if (groupIndex == 0u)
        {
            standardCullTileLights[base + 0u] = kStandardLightTileOverflow;
            standardCullTileLights[base + 1u] = 0u;
        }
        return;
    }

    // ---- test lights in parallel, one bit each ----
    for (uint i = groupIndex; i < gPushConstants.pointCount; i += kThreads)
    {
        StandardPointLight light = standardCullPointLights[i];
        if (standardCullReachesTile(light.config, light.radius, light.position,
                                    tileLo, tileHi))
            InterlockedOr(sPointMask[i >> 5u], 1u << (i & 31u));
    }
    // Spots get the same radial sphere test and no cone test: the shading cone
    // is an aspect-stretched projection (StandardLighting.slang), so the round
    // cone helper in Grid.slang is not conservative for them.
    for (uint i = groupIndex; i < gPushConstants.spotCount; i += kThreads)
    {
        StandardSpotLight light = standardCullSpotLights[i];
        if (standardCullReachesTile(light.config, light.radius, light.position,
                                    tileLo, tileHi))
            InterlockedOr(sSpotMask[i >> 5u], 1u << (i & 31u));
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- ordered emit ----
    // Serial, but it only walks the bitmask the group just built in parallel.
    if (groupIndex == 0u)
    {
        uint cursor = base + 2u;
        uint pointWritten = 0u;
        uint spotWritten = 0u;
        bool overflow = false;

        for (uint i = 0u; i < gPushConstants.pointCount; ++i)
        {
            if ((sPointMask[i >> 5u] & (1u << (i & 31u))) == 0u) continue;
            if (pointWritten + spotWritten >= kStandardLightTileMaxLights)
            {
                overflow = true;
                break;
            }
            standardCullTileLights[cursor + pointWritten] = i;
            ++pointWritten;
        }
        if (!overflow)
        {
            for (uint i = 0u; i < gPushConstants.spotCount; ++i)
            {
                if ((sSpotMask[i >> 5u] & (1u << (i & 31u))) == 0u) continue;
                if (pointWritten + spotWritten >= kStandardLightTileMaxLights)
                {
                    overflow = true;
                    break;
                }
                standardCullTileLights[cursor + pointWritten + spotWritten] = i;
                ++spotWritten;
            }
        }

        if (overflow)
        {
            standardCullTileLights[base + 0u] = kStandardLightTileOverflow;
            standardCullTileLights[base + 1u] = 0u;
        }
        else
        {
            standardCullTileLights[base + 0u] = pointWritten;
            standardCullTileLights[base + 1u] = spotWritten;
        }
    }
}
