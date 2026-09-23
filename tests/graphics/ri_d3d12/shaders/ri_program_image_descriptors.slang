Texture2D<float4> gSampled[2] : register(t3, space2);
RWTexture2D<float4> gStorage : register(u4, space1);
RWStructuredBuffer<uint> gOutput : register(u7, space1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float4 value = gSampled[1].Load(int3(0, 0, 0));
    gStorage[uint2(0, 0)] = value;
    gOutput[0] = (uint)(value.r * 255.0 + 0.5);
}
