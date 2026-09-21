// Regression fixture for "conflicting reflected D3D12 push-constant ranges".
//
// push_constant.slang keeps every entry point in ONE file, so slangc numbers
// the whole unit consistently and that fixture can never reproduce the bug.
// The production renderer splits vertex and fragment into separate files, which
// are compiled by separate slangc invocations that each number registers from
// scratch.
//
// This is the outline_alpha / glow_object shape: the vertex stage declares a
// constant buffer AHEAD of the push block, the fragment stage (in the paired
// file) declares none. Left to auto-assignment the vertex push block lands on
// b1 and the fragment's on b0, the two stages disagree, and RIProgram aborts
// while building the shared root signature. HPL_PUSH_CONSTANT_REGISTER pins
// both to the same reserved register.

#include "HostDefinitions.h"

struct SplitPass
{
    float4 scale;
};

// Takes b0 in this compilation unit, displacing an unpinned push block to b1.
[vk::binding(0, 0)]
ConstantBuffer<SplitPass> pass;

struct SplitPushConstants
{
    uint4 value;
};

[vk::push_constant]
ConstantBuffer<SplitPushConstants> gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput VSMain(uint vertexID : SV_VertexID)
{
    const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VSOutput output;
    output.position = float4(positions[vertexID] * pass.scale.xy, 0.0, 1.0);
    // Keep the push block live in this stage so reflection reports it as used.
    output.position.z = float(gPushConstants.value.w) * 0.0;
    return output;
}
