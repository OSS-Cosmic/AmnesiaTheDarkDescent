// Fragment half of the split-file push-constant regression fixture. Pairs with
// split_push_constant.vert.slang; see that file for what this reproduces.
//
// Deliberately declares NO constant buffer, so without the pin slangc would
// give this stage's push block b0 while the vertex stage's lands on b1.

#include "HostDefinitions.h"

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

[shader("fragment")]
float4 PSMain(VSOutput input) : SV_Target
{
    return float4(gPushConstants.value) / 255.0;
}
