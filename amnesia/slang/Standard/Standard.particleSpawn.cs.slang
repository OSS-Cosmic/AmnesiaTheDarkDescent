// GPU particle spawn: apply the CPU shadow scheduler's spawn commands.
//
// One thread per command. The scheduler (the successor to
// cParticleEmitter_UserData::UpdateMotion's creation block,
// ParticleEmitter_UserData.cpp:910-921) owns slot allocation and expiry, so this
// shader never races for a slot and never needs an atomic: each command names
// the exact pool slot it initialises.
//
// Runs immediately before Standard.particleSim.cs.slang, so a particle created
// this step is also integrated this step -- matching UpdateMotion's
// create-then-loop ordering.

#include "../HostDefinitions.h"
import SceneTypes;
import StandardParticles;

[vk::binding(0, 2)] RWStructuredBuffer<GpuParticle>       gParticleState;
[vk::binding(1, 2)] StructuredBuffer<GpuEmitterInstance>  gParticleEmitters;
[vk::binding(2, 2)] StructuredBuffer<GpuEmitterParams>    gParticleEmitterParams;
[vk::binding(3, 2)] StructuredBuffer<GpuParticleSpawn>    gParticleSpawnCmds;

struct ParticleSpawnPC
{
    uint spawnCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

[vk::push_constant]
ParticleSpawnPC gPushConstants;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gPushConstants.spawnCount)
        return;

    GpuParticleSpawn cmd = gParticleSpawnCmds[dtid.x];
    GpuEmitterInstance inst = gParticleEmitters[cmd.emitterSlot];
    GpuEmitterParams params = gParticleEmitterParams[inst.paramId];

    gParticleState[cmd.slot] =
        particleSpawn(params, inst, cmd.emitterSlot, cmd.seed, cmd.startLife);
}
