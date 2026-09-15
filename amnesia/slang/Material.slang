#pragma once

// Material — inline RayQuery shadow / reflection traces over the engine's flat
// material table and compatibility exports for the raster material helpers.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;       // VertexData, TriangleHit
import bindless;         // gSceneObjects, gRtAccel
import Scene;            // getVertexData (vertex fetch for the RayQuery traces)
import PerFrame.resource;
__exported import MaterialRaster;

// Inline non-opaque shadow query. The BLAS is built with
// VK_GEOMETRY_OPAQUE_BIT cleared (VertexBuffer_RI.cpp:589 sets
// geom.flags = 0), so the driver reports every triangle hit as a
// *candidate* — the shader must explicitly commit. A plain
// `Proceed() + CommittedStatus()` (no commit call) silently reports
// every shadow ray as unoccluded, which is why direct-light shadows
// were missing from the GI composite before this helper landed.
//
// Loops while candidates remain, runs the diffuse alpha test on each
// non-opaque triangle, and commits when alpha passes. Returns true
// when [origin, origin + dir*tMax] is unoccluded — matches the GLSL
// twin in amnesia/glsl/traceray_rq.glsl::AnyHit.
public bool traceShadowRay(float3 origin, float3 dir, float tMax)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.f;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
           | RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> q;
    // The TLAS applies the global shadow policy and includes shadow-only hulls.
    q.TraceRayInline(gRtAccel, 0, kRayMaskShadow, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            TriangleHit hit;
            hit.instanceID     = q.CandidateInstanceID();
            hit.primitiveIndex = q.CandidatePrimitiveIndex();
            hit.barycentrics   = q.CandidateTriangleBarycentrics();

            uint materialID = gSceneObjects[hit.instanceID].materialID;
            VertexData v    = getVertexData(hit);

            if (!materialAlphaTest(v, materialID,
                                   gSceneObjects[hit.instanceID].dissolveAmount))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_NOTHING;
}

// Inline ray-traced reflection lookup returning the committed TriangleHit (not
// just its albedo) so the caller can fully lit-shade it — used by the raster
// water pass for sharp lit reflections. Walks the TLAS in `dir` from `origin`
// (already biased off the surface by the caller), commits the first non-opaque
// alpha-passing hit. Returns false (outHit zeroed) on miss.
public bool traceReflectionHit(float3 origin, float3 dir, float tMax, out TriangleHit outHit)
{
    outHit.instanceID = 0u; outHit.primitiveIndex = 0u; outHit.barycentrics = float2(0.f);

    RayDesc ray;
    ray.Origin = origin; ray.Direction = dir; ray.TMin = 0.f; ray.TMax = tMax;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
           | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(gRtAccel, 0, kRayMaskOpaque, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            TriangleHit c;
            c.instanceID     = q.CandidateInstanceID();
            c.primitiveIndex = q.CandidatePrimitiveIndex();
            c.barycentrics   = q.CandidateTriangleBarycentrics();
            uint matID  = gSceneObjects[c.instanceID].materialID;
            VertexData v = getVertexData(c);
            if (!materialAlphaTest(v, matID,
                                   gSceneObjects[c.instanceID].dissolveAmount))
                q.CommitNonOpaqueTriangleHit();
        }
    }

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;

    outHit.instanceID     = q.CommittedInstanceID();
    outHit.primitiveIndex = q.CommittedPrimitiveIndex();
    outHit.barycentrics   = q.CommittedTriangleBarycentrics();
    return true;
}
