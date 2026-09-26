#pragma once

// HitInfo / TriangleHit (un)packing — matches the Falcor source-compat surface
// used by the V-buffer / direct-lighting / path-trace passes:
// `HitInfo h = HitInfo(packedUVec4)`, `h.isValid()`,
// `h.getTriangleHit()`, and `TriangleHit::pack()` for writes. The underlying
// structs (data layout) live in SceneTypes.slang; these extensions add the
// pack/unpack behaviour.

import SceneTypes;

public extension HitInfo
{
    public bool isValid()
    {
        return any(packed != uint4(0u, 0u, 0u, 0u));
    }

    // Barycentrics are stored as full fp32 (b1 in .z, b2 in .w). Half floats
    // quantise b1/b2 to ~1/2048 of the triangle's extent, which snaps the
    // reconstructed UV to multi-texel blocks on large or heavily tiled
    // triangles (visible as stair-stepped texels in every RT pass that
    // rebuilds UVs from the V-buffer).
    //
    // b2 is in [0, 1], so its sign bit is free: it is always set in the
    // packed form so that .w is never zero and the all-zero "no hit" clear
    // value stays distinguishable via isValid().
    public TriangleHit getTriangleHit()
    {
        TriangleHit h;
        h.instanceID     = packed.x;
        h.primitiveIndex = packed.y;
        h.barycentrics   = float2(
            asfloat(packed.z),
            asfloat(packed.w & 0x7FFFFFFFu));
        return h;
    }
};

public extension TriangleHit
{
    public uint4 pack()
    {
        uint4 r;
        r.x = instanceID;
        r.y = primitiveIndex;
        r.z = asuint(barycentrics.x);
        r.w = asuint(barycentrics.y) | 0x80000000u;  // sign bit = "valid" flag
        return r;
    }
};
