// GPU particle expansion: turn live particles into camera-facing quads for ONE
// viewport, and write that emitter's VkDrawIndexedIndirectCommand.
//
// Runs once per viewport, after Standard.particleSim.cs.slang has advanced the
// shared particle state. This is the GPU port of
// iParticleEmitter::BuildViewportVertices (ParticleEmitter.cpp:326-698): same
// four draw types, same reflection-frustum corner flip, same distance fade and
// parent-system colour, same sub-division UV table.
//
// Output layout matches the host scratch contract the particle vertex shader
// already pulls through BDA (Standard.particle.3d.slang:61-70), stream-of-arrays
// within one slice:
//     [ pos float4 * V ][ col float4 * V ][ uv float3 * V ],  V = 4 * particles
// Positions are VIEW SPACE for the point/line types and WORLD SPACE for Axis,
// which is why the host hands those emitters different model matrices
// (iParticleEmitter::GetModelMatrix, ParticleEmitter.cpp:799).

#include "../HostDefinitions.h"
import SceneTypes;
import StandardParticles;
import PerFrame.resource;

[vk::binding(0, 2)] StructuredBuffer<GpuParticle>        gParticleState;
[vk::binding(1, 2)] StructuredBuffer<GpuEmitterInstance> gParticleEmitters;
[vk::binding(2, 2)] StructuredBuffer<GpuEmitterParams>   gParticleEmitterParams;
[vk::binding(3, 2)] StructuredBuffer<uint2>              gParticleGroups;
// Per emitter slot, PER VIEWPORT: first float of that emitter's quad slice in
// gParticleQuadVerts. The host allocates the slices from the per-frame ring.
[vk::binding(4, 2)] StructuredBuffer<uint>               gParticleSliceOffset;
// float-addressed view of the per-viewport quad ring.
[vk::binding(5, 2)] RWStructuredBuffer<float>            gParticleQuadVerts;

struct ParticleExpandPC
{
    uint  groupCount;
    uint  invertCull;     // reflection frustum: flip the quad corners in y
    float cameraOriginX;  // frustum origin, for the distance fade
    float cameraOriginY;
    float cameraOriginZ;
    uint  pad0;
    uint  pad1;
    uint  pad2;
};

[vk::push_constant]
ParticleExpandPC gPushConstants;

// iParticleEmitter::BuildViewportVertices' fade prologue (ParticleEmitter.cpp:337).
// Distances are compared squared, exactly as the host does.
float particleFadeAlpha(GpuEmitterInstance inst)
{
    float3 camera = float3(gPushConstants.cameraOriginX, gPushConstants.cameraOriginY, gPushConstants.cameraOriginZ);
    float3 delta  = inst.worldPos - camera;
    float  distSq = dot(delta, delta);
    float  alpha  = 1.0f;

    if (inst.fadeMinStart > 0.0f && distSq < inst.fadeMinStart * inst.fadeMinStart)
    {
        if (distSq <= inst.fadeMinEnd * inst.fadeMinEnd)
            alpha = 0.0f;
        else
            alpha *= (sqrt(distSq) - inst.fadeMinEnd) / (inst.fadeMinStart - inst.fadeMinEnd);
    }
    if (inst.fadeMaxStart > 0.0f && distSq > inst.fadeMaxStart * inst.fadeMaxStart)
    {
        if (distSq >= inst.fadeMaxEnd * inst.fadeMaxEnd)
            alpha = 0.0f;
        else
            alpha *= 1.0f - (sqrt(distSq) - inst.fadeMaxStart) / (inst.fadeMaxEnd - inst.fadeMaxStart);
    }
    return alpha;
}

// The sub-division atlas cell, rebuilt from the authored grid rather than
// uploaded -- iParticleEmitter::SetSubDivUV (ParticleEmitter.cpp:162) is a pure
// function of SubDiv and the cell index. Corner order is (1,1) (0,1) (0,0) (1,0).
void particleSubDivUV(GpuEmitterParams p, uint cell, out float2 uv[4])
{
    if (p.subDivCount <= 1u || p.subDiv.x == 0u || p.subDiv.y == 0u)
    {
        uv[0] = float2(1.0f, 1.0f);
        uv[1] = float2(0.0f, 1.0f);
        uv[2] = float2(0.0f, 0.0f);
        uv[3] = float2(1.0f, 0.0f);
        return;
    }

    uint x = cell % p.subDiv.x;
    uint y = cell / p.subDiv.x;
    float invW = 1.0f / float(p.subDiv.x);
    float invH = 1.0f / float(p.subDiv.y);
    float fx = float(x), fy = float(y);

    uv[0] = float2((fx + 1.0f) * invW, (fy + 1.0f) * invH);
    uv[1] = float2( fx         * invW, (fy + 1.0f) * invH);
    uv[2] = float2( fx         * invW,  fy         * invH);
    uv[3] = float2((fx + 1.0f) * invW,  fy         * invH);
}

// Writes one quad at the particle's OWN slot position -- the output is never
// compacted. A dead or fully faded slot gets four identical corners, i.e. two
// zero-area triangles, which are discarded at primitive assembly at no fragment
// cost. That is what lets every emitter draw a constant capacity*6 indices with
// no atomics, no compaction pass and no indirect-argument buffer, so
// RendererSimple / RendererWireFrame keep their plain drawIndexed. The whole
// shipped game sums to 14,056 particle slots, so the wasted vertices are noise.
void writeQuad(uint vertexBase, uint quadIndex, uint vertexCapacity,
               float3 corner[4], float4 color, float2 uv[4])
{
    uint v0 = quadIndex * 4u;
    if (v0 + 4u > vertexCapacity)
        return;

    uint posBase = vertexBase;
    uint colBase = vertexBase + vertexCapacity * 4u;
    uint uvBase  = vertexBase + vertexCapacity * 8u;

    for (uint i = 0u; i < 4u; ++i)
    {
        uint p = posBase + (v0 + i) * 4u;
        gParticleQuadVerts[p + 0u] = corner[i].x;
        gParticleQuadVerts[p + 1u] = corner[i].y;
        gParticleQuadVerts[p + 2u] = corner[i].z;
        gParticleQuadVerts[p + 3u] = 1.0f;

        uint c = colBase + (v0 + i) * 4u;
        gParticleQuadVerts[c + 0u] = color.r;
        gParticleQuadVerts[c + 1u] = color.g;
        gParticleQuadVerts[c + 2u] = color.b;
        gParticleQuadVerts[c + 3u] = color.a;

        uint t = uvBase + (v0 + i) * 3u;
        gParticleQuadVerts[t + 0u] = uv[i].x;
        gParticleQuadVerts[t + 1u] = uv[i].y;
        gParticleQuadVerts[t + 2u] = 0.0f;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    if (gid.x >= gPushConstants.groupCount)
        return;

    uint2 group       = gParticleGroups[gid.x];
    uint  emitterSlot = group.x;
    uint  localIndex  = group.y + gtid.x;

    GpuEmitterInstance inst = gParticleEmitters[emitterSlot];
    if (localIndex >= inst.sliceCapacity)
        return;

    // Slot position IS quad position -- see writeQuad. Every thread writes,
    // alive or not, so stale geometry from an earlier frame can never show.
    uint vertexCapacity = inst.sliceCapacity * 4u;
    uint vertexBase     = gParticleSliceOffset[emitterSlot];

    float3 corner[4];
    float2 uv[4];
    float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);

    GpuParticle part = gParticleState[inst.sliceBase + localIndex];
    GpuEmitterParams p = gParticleEmitterParams[inst.paramId];

    float fade = particleFadeAlpha(inst);

    if (!particleAlive(part) || fade <= 0.0f)
    {
        // Degenerate quad: four coincident corners, discarded at assembly.
        for (uint i = 0u; i < 4u; ++i)
            corner[i] = float3(0.0f, 0.0f, 0.0f);
        uv[0] = uv[1] = uv[2] = uv[3] = float2(0.0f, 0.0f);
        writeQuad(vertexBase, localIndex, vertexCapacity, corner, color, uv);
        return;
    }

    ParticleDefaults d = particleDefaults(p, part.seed);
    color = particleEvalColor(part, p, d);
    float2 size = particleEvalSize(part, p, d);

    // cParticleSystem::mColor and the distance fade, matching the colorMul
    // prologue at ParticleEmitter.cpp:337-390. MultiplyRGBWithAlpha is applied
    // to the particle colour inside particleEvalColor and again to colorMul
    // here, exactly as the host applies it in both places.
    float4 colorMul = inst.parentColor;
    colorMul.a *= fade;
    if ((p.flags & kGpuEmitterFlagMulRGBWithAlpha) != 0u)
        colorMul = float4(colorMul.rgb * colorMul.a, colorMul.a);
    color *= colorMul;

    bool localSpace = (p.flags & kGpuEmitterFlagCoordSystemLocal) != 0u;
    float3 worldPos     = localSpace ? mul(inst.worldMat, float4(part.pos, 1.0f)).xyz     : part.pos;
    float3 worldLastPos = localSpace ? mul(inst.worldMat, float4(part.lastPos, 1.0f)).xyz : part.lastPos;

    float flipY = (gPushConstants.invertCull != 0u) ? -1.0f : 1.0f;

    particleSubDivUV(p, particleSubDiv(part), uv);

    if (p.drawType == kGpuDrawTypeAxis)
    {
        // World space, oriented by the emitter's own axes -- no view transform,
        // which is why the host hands Axis emitters an identity model matrix
        // (ParticleEmitter.cpp:803).
        float3 right   = inst.right   * size.x;
        float3 forward = inst.forward * size.y;
        corner[0] = worldPos + right + forward;
        corner[1] = worldPos - right + forward;
        corner[2] = worldPos - right - forward;
        corner[3] = worldPos + right - forward;
    }
    else if (p.drawType == kGpuDrawTypeLine)
    {
        float3 v1 = mul(gPerFrame.viewMat, float4(worldPos, 1.0f)).xyz;
        float3 v2 = mul(gPerFrame.viewMat, float4(worldLastPos, 1.0f)).xyz;

        float3 dirY;
        float2 dirX;
        float3 delta = v1 - v2;
        if (dot(delta, delta) <= 0.0f)
        {
            dirY = float3(0.0f, 1.0f, 0.0f);
            dirX = float2(1.0f, 0.0f);
        }
        else
        {
            dirY = normalize(delta);
            dirX = normalize(float2(dirY.y, -dirY.x));
        }

        // drawSize is always (1,1) -- cParticleEmitter_UserData sets it once at
        // ParticleEmitter_UserData.cpp:452 and nothing ever reassigns it. Kept
        // for faithfulness to BuildViewportVertices:615.
        float3 axisX = float3(dirX * p.drawSize.x * size.x, 0.0f);
        float3 axisY = dirY * p.drawSize.y * size.y * flipY;

        corner[0] = v2 - axisY + axisX;
        corner[1] = v2 - axisY - axisX;
        corner[2] = v1 + axisY - axisX;
        corner[3] = v1 + axisY + axisX;
    }
    else
    {
        // kGpuDrawTypeDynamicPoint. kGpuDrawTypeFixedPoint is unreachable from
        // assets -- GetDrawType (ParticleEmitter_UserData.cpp:130) maps "point"
        // to DynamicPoint and defaults to it -- but it is handled here anyway so
        // the enum has no hole.
        float3 viewPos = mul(gPerFrame.viewMat, float4(worldPos, 1.0f)).xyz;

        float2 extent = (p.drawType == kGpuDrawTypeFixedPoint)
                      ? p.drawSize
                      : p.drawSize * size;

        float3 add[4] = {
            float3( extent.x, -extent.y * flipY, 0.0f),
            float3(-extent.x, -extent.y * flipY, 0.0f),
            float3(-extent.x,  extent.y * flipY, 0.0f),
            float3( extent.x,  extent.y * flipY, 0.0f),
        };

        if ((p.flags & kGpuEmitterFlagUsePartSpin) != 0u)
        {
            float sn = sin(part.spin), cs = cos(part.spin);
            for (uint i = 0u; i < 4u; ++i)
                add[i] = float3(cs * add[i].x - sn * add[i].y,
                                sn * add[i].x + cs * add[i].y,
                                0.0f);
        }

        for (uint i = 0u; i < 4u; ++i)
            corner[i] = viewPos + add[i];
    }

    writeQuad(vertexBase, localIndex, vertexCapacity, corner, color, uv);
}
