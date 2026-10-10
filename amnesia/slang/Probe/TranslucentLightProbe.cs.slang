// SPDX-License-Identifier: GPL-3.0
// GPU-only, one workgroup per AffectedByLightLevel object. The request/result
// ring belongs to the renderer and is independent of the gameplay query limit.
#include "../Constants.h"
import SceneTypes;
import Probe.LightProbeSampling;

[vk::binding(0, 2)] StructuredBuffer<LightProbeRequest> gProbeRequests;
[vk::binding(1, 2)] RWStructuredBuffer<LightProbeResult> gProbeResults;

struct TranslucentProbePC
{
    uint probeBase;
    uint probeCount;
};
[vk::push_constant]
TranslucentProbePC gPushConstants HPL_PUSH_CONSTANT_REGISTER;

[shader("compute")]
[numthreads(kLightProbeSampleCount, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (group.x >= gPushConstants.probeCount)
        return;
    uint probe = gPushConstants.probeBase + group.x;
    // Physical scene lighting includes the lantern and darkness assist light.
    ProbeLightFilter filter = {};
    float3 irradiance = sampleLightProbe(gProbeRequests[probe].posW, lane, filter);
    if (lane == 0u)
    {
        LightProbeResult result;
        result.irradiance = irradiance;
        result.padding = 0.0f;
        gProbeResults[probe] = result;
    }
}
