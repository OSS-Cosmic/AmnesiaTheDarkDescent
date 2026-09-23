#pragma once

// Slang mirror of amnesia/glsl/bindless_triangle.glsl::FetchBindlessTriangle.
// Each call resolves the per-instance VkDeviceAddress for every vertex
// stream we care about, indexes the (optional) index buffer, then loads
// position / normal / tangent / uv for the three triangle vertices.
//
// Buffer-device-address access uses Slang's vk::buffer_reference attribute
// — same memory model as the GLSL `layout(buffer_reference, scalar) buffer`
// declarations on the GLSL side.

#include "HostDefinitions.h"

import SceneTypes;
import bindless;
import GeometryStream;

// BDA accesses use direct primitive pointer casts (`float4*`, `float3*`,
// `uint*`). The wrapper-struct-with-runtime-array pattern from earlier
// Slang releases silently miscompiles in slangc 2026+: `((Wrapper*)addr).v[i]`
// collapses to `Ptr<PhysicalStorageBuffer, uint>` at the SPIR-V level and
// the load yields zero. `[vk::buffer_reference]` is also unrecognized in
// 2026+. Primitive pointer types translate correctly per the Slang docs.
//
// Stride assumptions (must match the host upload format):
//   position : tightly packed vec4 (16 B)  — float4*
//   tangent  : tightly packed vec4 (16 B)  — float4*
//   normal   : tightly packed vec3 (12 B)  — float3*
//   uv0      : tightly packed vec3 (12 B)  — float3*
//   index    : uint (4 B)                  — uint*

public struct BindlessTriangleVtx
{
    public float3 posL[3];      // local-space positions
    public float3 normalL[3];   // local-space normals
    // xyz: local-space tangent. w: bitangent handedness (+/-1) — see the
    // GLSL twin for the rationale (composite derives bitangent via
    // cross(tangent, normal) * w to preserve export handedness).
    public float4 tangentL[3];
    public float2 uv[3];        // UV0 per vertex
};

// primId is per-instance, zero-based (matches gl_PrimitiveID at the GBuffer
// fragment site). Falls back to zero / +Z normal / Gram-Schmidt tangent /
// zero UV on missing streams, mirroring the GLSL behavior so the V-buffer
// reconstructor stays branch-identical across the two paths.
public void fetchBindlessTriangle(uint instanceId, uint primId, out BindlessTriangleVtx vtx)
{
    // Stream BDAs now live in the per-object UniformObject (folded in from the
    // former gOpaque*Handles buffers — same per-slot index).
    UniformObject obj  = gSceneObjects[instanceId];
    uint64_t indexAddr = obj.indexHandle;
    uint64_t posAddr   = obj.posHandle;
    uint64_t nrmAddr   = obj.normalHandle;
    uint64_t tanAddr   = obj.tangentHandle;
    uint64_t uvAddr    = obj.uv0Handle;

    uint idxBase = primId * 3u;
    uint3 tri = uint3(idxBase, idxBase + 1u, idxBase + 2u);
    if (indexAddr != 0ull)
    {
        tri = uint3(geometryLoad(indexAddr, (idxBase + 0u) * 4u),
                    geometryLoad(indexAddr, (idxBase + 1u) * 4u),
                    geometryLoad(indexAddr, (idxBase + 2u) * 4u));
    }

    if (posAddr != 0ull)
    {
        vtx.posL[0] = asfloat(geometryLoad4(posAddr, tri.x * 16u)).xyz;
        vtx.posL[1] = asfloat(geometryLoad4(posAddr, tri.y * 16u)).xyz;
        vtx.posL[2] = asfloat(geometryLoad4(posAddr, tri.z * 16u)).xyz;
    }
    else
    {
        vtx.posL[0] = float3(0.0f);
        vtx.posL[1] = float3(0.0f);
        vtx.posL[2] = float3(0.0f);
    }

    if (nrmAddr != 0ull)
    {
        vtx.normalL[0] = asfloat(geometryLoad3(nrmAddr, tri.x * 12u));
        vtx.normalL[1] = asfloat(geometryLoad3(nrmAddr, tri.y * 12u));
        vtx.normalL[2] = asfloat(geometryLoad3(nrmAddr, tri.z * 12u));
    }
    else
    {
        vtx.normalL[0] = float3(0.0f, 0.0f, 1.0f);
        vtx.normalL[1] = float3(0.0f, 0.0f, 1.0f);
        vtx.normalL[2] = float3(0.0f, 0.0f, 1.0f);
    }

    if (tanAddr != 0ull)
    {
        // Tangent.w is the authored bitangent handedness and must survive the
        // raw load; it is not a fourth position-like attribute.
        vtx.tangentL[0] = asfloat(geometryLoad4(tanAddr, tri.x * 16u));
        vtx.tangentL[1] = asfloat(geometryLoad4(tanAddr, tri.y * 16u));
        vtx.tangentL[2] = asfloat(geometryLoad4(tanAddr, tri.z * 16u));
    }
    else
    {
        // No tangent stream — synthesize one orthogonal to N using a helper
        // axis that isn't collinear (constant fallbacks collapse on +/-X
        // walls; see the GLSL twin's note for the symptom).
        [unroll]
        for (uint k = 0u; k < 3u; ++k)
        {
            float3 N = vtx.normalL[k];
            float3 helper = (abs(N.x) < 0.9f) ? float3(1.0f, 0.0f, 0.0f)
                                              : float3(0.0f, 1.0f, 0.0f);
            float3 T = normalize(cross(helper, N));
            vtx.tangentL[k] = float4(T, 1.0f);
        }
    }

    if (uvAddr != 0ull)
    {
        vtx.uv[0] = asfloat(geometryLoad3(uvAddr, tri.x * 12u)).xy;
        vtx.uv[1] = asfloat(geometryLoad3(uvAddr, tri.y * 12u)).xy;
        vtx.uv[2] = asfloat(geometryLoad3(uvAddr, tri.z * 12u)).xy;
    }
    else
    {
        vtx.uv[0] = float2(0.0f);
        vtx.uv[1] = float2(0.0f);
        vtx.uv[2] = float2(0.0f);
    }
}
