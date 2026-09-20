// Small, deliberately self-contained RIProgram fixture.  Unlike the native
// table fixture, this shader is dispatched through RIProgram and consumes the
// producer-owned t4/space3 geometry table through the shared GeometryStream
// helper.
import bindless;
import GeometryStream;

struct GeometryConstants
{
    uint indexed;
    uint64_t indexedHandle;
    uint64_t nonIndexedHandle;
    uint64_t nullHandle;
};

cbuffer gConstants : register(b0, space0) { GeometryConstants constants; }
RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(3, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint lane = dispatchThreadID.x;
    uint64_t handle = lane == 0 ? constants.indexedHandle
                      : lane == 1 ? constants.nonIndexedHandle
                                  : constants.nullHandle;
    // The null stream is intentionally handled before the helper call: a
    // missing stream is a valid GeometryStream input and must not manufacture
    // a descriptor-table index from handle zero.
    gOutput[lane] = handle == 0 ? 0u : geometryLoad(handle, 0u);
}
