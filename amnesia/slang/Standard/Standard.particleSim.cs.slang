// GPU particle simulation: integrate + depth-buffer collision.
//
// One thread per particle SLOT. Spawning is a separate earlier dispatch
// (Standard.particleSpawn.cs.slang) driven by the CPU shadow scheduler, so this
// shader owns no allocation and needs no atomics. Runs ONCE PER FRAME, before any
// viewport is rendered, because the result is view-independent -- the
// per-viewport billboarding lives in Standard.particleExpand.cs.slang.
//
// Thread -> (emitter, slot) mapping comes from gParticleGroups, one entry per
// workgroup, built on the host from a prefix sum over the emitters' slice
// capacities. That keeps the dispatch exactly as wide as the live particle
// pool instead of maxCapacity * emitterCount.

#include "../HostDefinitions.h"
import SceneTypes;
import StandardParticles;
import PerFrame.resource;

[vk::binding(0, 2)] RWStructuredBuffer<GpuParticle>        gParticleState;
[vk::binding(1, 2)] StructuredBuffer<GpuEmitterInstance>   gParticleEmitters;
[vk::binding(2, 2)] StructuredBuffer<GpuEmitterParams>     gParticleEmitterParams;
// .x = emitter slot, .y = first particle-slice index this workgroup covers.
[vk::binding(3, 2)] StructuredBuffer<uint2>                gParticleGroups;
// Previous frame's scene depth, for the collision sweep.
[vk::binding(4, 2)] Texture2D<float>                       gParticleCollisionDepth;

struct ParticleSimPC
{
    uint  groupCount;
    uint  collisionEnabled;   // zero when no usable depth target is bound
    float collisionThickness; // view-space depth slab treated as solid
    uint  pad0;
};

[vk::push_constant]
ParticleSimPC gPushConstants;

// Screen-space collision. Projects the particle with the PREVIOUS frame's
// matrices (this pass runs before the current frame's depth exists) and treats
// a sample that is in front of the particle, but within collisionThickness, as
// a surface. Approximate by construction: off-screen geometry cannot be hit.
// Exactly one shipped emitter authors Collides="true".
bool particleCollide(inout GpuParticle part, float bounceAmount)
{
    if (gPushConstants.collisionEnabled == 0u)
        return false;

    float4 view = mul(gPerFrame.prevViewMat, float4(part.pos, 1.0f));
    float4 clip = mul(gPerFrame.prevProjMat, view);
    if (clip.w <= 0.0f)
        return false;

    float2 ndc = clip.xy / clip.w;
    float2 uv  = ndc * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return false;

    int2 pixel = int2(uv * gPerFrame.viewportSize);
    float deviceDepth = gParticleCollisionDepth.Load(int3(pixel, 0));
    if (deviceDepth <= 0.0f || deviceDepth >= 1.0f)
        return false;

    float sceneZ = (gPerFrame.orthographicCamera != 0u)
        ? lerp(gPerFrame.zNear, gPerFrame.zFar, deviceDepth)
        : gPerFrame.zNear * gPerFrame.zFar /
              (gPerFrame.zFar - deviceDepth * (gPerFrame.zFar - gPerFrame.zNear));

    float partZ = -view.z; // view-space distance along the camera axis (right-handed, -z forward)

    // Behind the surface, but not so far behind that the particle has clearly
    // passed through a thin wall into a different room.
    if (partZ < sceneZ || partZ > sceneZ + gPushConstants.collisionThickness)
        return false;

    // Reconstruct a surface normal from the depth neighbourhood. Cheap central
    // difference; good enough for a bounce direction.
    float dR = gParticleCollisionDepth.Load(int3(pixel + int2(1, 0), 0));
    float dU = gParticleCollisionDepth.Load(int3(pixel + int2(0, 1), 0));
    float3 normal = normalize(float3(sceneZ - dR, sceneZ - dU, 1e-3f));

    // Put the particle back on the surface and reflect, scaled by bounce.
    part.pos = part.lastPos;
    float speed = length(part.vel);
    if (speed > 0.0f)
    {
        float3 reflected = normalize(part.vel - normal * 2.0f * dot(part.vel, normal));
        part.vel = reflected * (speed * bounceAmount);
    }
    return true;
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

    uint slot = inst.sliceBase + localIndex;
    GpuEmitterParams params = gParticleEmitterParams[inst.paramId];
    GpuParticle part = gParticleState[slot];

    // Dead slots are inert until the scheduler's next spawn command claims them.
    if (!particleAlive(part))
        return;

    part = particleIntegrate(part, params, inst, inst.dt);

    // ---- collision ------------------------------------------------------
    if ((params.flags & kGpuEmitterFlagCollides) != 0u)
    {
        ParticleDefaults d = particleDefaults(params, part.seed);
        if (particleCollide(part, d.bounceAmount))
        {
            uint bounces = particleBounce(part);
            bounces = (bounces > 0u) ? (bounces - 1u) : 0u;
            part.packed = particlePack(true, bounces, particleSubDiv(part));
            if (bounces == 0u)
                part.life = 0.0f;
        }
    }

    // ---- death ----------------------------------------------------------
    // Just clear the alive bit. Respawn is the scheduler's call, not this
    // shader's: the CPU heap already knows this slot expires now and will issue
    // a spawn command for it on the same tick when the emitter respawns
    // (ParticleEmitter_UserData.cpp:1066-1086). Keeping the decision in one
    // place is what lets mlNumOfParticles stay exact without a readback.
    if (part.life <= 0.0f)
        part.packed = particlePack(false, 0u, 0u);

    gParticleState[slot] = part;
}
