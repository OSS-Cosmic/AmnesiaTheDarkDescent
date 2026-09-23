import shared;

RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    gOutput[dispatchThreadID.x] = sharedThreadValue(dispatchThreadID.x);
}
