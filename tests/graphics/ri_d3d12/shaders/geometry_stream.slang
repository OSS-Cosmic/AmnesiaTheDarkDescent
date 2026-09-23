import GeometryStream;

struct GeometryConstants
{
    uint indexed;
    uint padding;
    uint64_t indexedHandle;
    uint64_t nonIndexedHandle;
    uint64_t nullHandle;
};

cbuffer gConstants : register(b0, space0) { GeometryConstants constants; }
RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(3, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint lane = dispatchThreadID.x;
    const uint64_t handle = lane == 0 && constants.indexed != 0
                              ? constants.indexedHandle
                              : lane == 1 ? constants.nonIndexedHandle
                                          : constants.nullHandle;
    // The null stream is a valid producer value and must not be decoded as a
    // descriptor index. The other two handles carry a nonzero byte offset.
    gOutput[lane] = handle == 0ull ? 0u : geometryLoad(handle, 0u);
}
