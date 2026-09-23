// Opt-in D3D12 external/ordinary descriptor-table smoke shader.
// The external tables deliberately use two different register spaces: this
// mirrors the production geometry raw-SRV table and prevents accidental range
// flattening when an external layout is supplied by a caller.
struct SmokeConstants {
    uint ordinaryValue;
    uint textureIndex;
    uint rawIndex;
    uint indexed;
    uint64_t indexedHandle;
    uint64_t nonIndexedHandle;
};

cbuffer gConstants : register(b0, space0) { SmokeConstants gConstants; };
StructuredBuffer<uint> gOrdinary : register(t0, space0);
RWStructuredBuffer<uint> gOutput : register(u0, space0);

Texture2D<float4> gExternalTextures[] : register(t0, space2);
ByteAddressBuffer gExternalBuffers[] : register(t4, space3);
SamplerState gSampler : register(s0, space0);

// Keep the native execution shader independent of Slang's physical-pointer
// lowering. The companion geometry_stream.slang is compiled in the same
// fixture and is the RIProgram GeometryStream acceptance artifact.
uint smokeGeometryLoad(uint64_t handle, uint byteOffset)
{
    return gExternalBuffers[NonUniformResourceIndex((uint)(handle >> 32) - 1u)]
        .Load((uint)handle + byteOffset);
}

[numthreads(2, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint lane = dispatchThreadID.x;
    uint textureIndex = NonUniformResourceIndex(gConstants.textureIndex + lane);
    uint rawIndex = NonUniformResourceIndex(gConstants.rawIndex + lane);
    uint ordinary = gOrdinary[lane];
    uint raw = gExternalBuffers[rawIndex].Load(0);
    uint tex = uint(gExternalTextures[textureIndex].SampleLevel(
        gSampler, float2(0.5f, 0.5f), 0).r * 255.0f + 0.5f);

    // Exercise both indexed and non-indexed producer handles, including a
    // nonzero byte offset. A zero handle remains the public null-stream path.
    uint geometry = gConstants.indexed != 0
        ? smokeGeometryLoad(gConstants.indexedHandle, 4u)
        : smokeGeometryLoad(gConstants.nonIndexedHandle, 4u);
    gOutput[lane] = gConstants.ordinaryValue + ordinary + raw + tex + geometry;
}
