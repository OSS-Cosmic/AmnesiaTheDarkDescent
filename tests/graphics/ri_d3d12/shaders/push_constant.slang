struct PushConstants
{
    uint4 value;
};

[vk::push_constant]
ConstantBuffer<PushConstants> gPushConstants;

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VSOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(gPushConstants.value) / 255.0;
}

RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    gOutput[0] = gPushConstants.value.x;
    gOutput[1] = gPushConstants.value.y;
    gOutput[2] = gPushConstants.value.z;
    gOutput[3] = gPushConstants.value.w;
}
