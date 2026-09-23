// Deliberately explicit register names: the native fixture builds the matching
// root signature by numeric slot, while the sidecar reflection is checked by
// name.  The two paths must describe the same contract.
struct FixtureUniforms
{
    uint value;
};

cbuffer gUniforms : register(b0, space0)
{
    FixtureUniforms gConstants;
};

Texture2D<float4> gTexture : register(t0, space0);
Texture2D<float4> gOptionalTextures[2] : register(t1, space0);
SamplerState gSampler : register(s0, space0);
RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Sample rather than Load so the sampler binding is part of the fixture.
    // The optional array is intentionally not read; null descriptors at t1/t2
    // therefore exercise partially populated array slots without undefined
    // shader access.
    float sampleValue = gTexture.SampleLevel(gSampler, float2(0.5f, 0.5f), 0).r;
    gOutput[0] = gConstants.value + uint(sampleValue * 255.0f + 0.5f);
}
