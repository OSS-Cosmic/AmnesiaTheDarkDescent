import shared;

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

static const float4 kPositions[3] =
{
    float4(0.0f, 0.6f, 0.0f, 1.0f),
    float4(0.6f, -0.6f, 0.0f, 1.0f),
    float4(-0.6f, -0.6f, 0.0f, 1.0f),
};

static const float3 kColors[3] =
{
    float3(1.0f, 0.0f, 0.0f),
    float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f),
};

VSOutput VSMain(float3 position : POSITION, float4 color : COLOR0)
{
    VSOutput output;
    output.position = float4(position, 1.0f);
    output.color = float4(applyFallbackColor(color.xyz), color.w);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
