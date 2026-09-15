// Standard opaque decal resolve.  This is a separate fullscreen graphics pass
// before lighting: it modifies only the reconstructed albedo and preserves its
// alpha for the later lighting/output attachment.
//
// The packed surface contract is Standard.reconstruct/fallback's uint4:
//   x = gSceneObjects object slot, y = material ID, z/w = UV bits.
// The object slot's decalList is the prepared World receiver list, packed as
// (offset << 8) | count.  Thus overlap order is the stable order in that list.
// No spatial grid, Scene, Material, or ray-tracing module is used here.
import PerFrame.resource;
import SceneTypes;
import MaterialRaster;
import bindless;
import StandardDecals;

[vk::binding(0,2)] Texture2D<float4> standardColorInput;
[vk::binding(1,2)] Texture2D<float4> standardPositionInput;
[vk::binding(2,2)] Texture2D<float4> standardNormalInput;
[vk::binding(3,2)] Texture2D<uint4>  standardSurfaceInput;
[vk::binding(kBindingWorldDecals, kWorldDecalSet)]
StructuredBuffer<GpuDecal> gDecals;
[vk::binding(kBindingWorldObjectDecalIndices, kWorldDecalSet)]
StructuredBuffer<uint> gObjectDecalIndices;

struct V { float4 posH : SV_POSITION; float2 uv : TEXCOORD0; };

[shader("vertex")]
V vsMain(uint id : SV_VertexID)
{
    V v;
    v.uv = float2(float((id << 1) & 2), float(id & 2));
    v.posH = float4(v.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return v;
}

[shader("pixel")]
float4 psMain(V v) : SV_TARGET0
{
    uint2 pixel = uint2(v.posH.xy);
    float4 color = standardColorInput.Load(int3(pixel, 0));
    float4 position = standardPositionInput.Load(int3(pixel, 0));
    float4 normalView = standardNormalInput.Load(int3(pixel, 0));
    uint4 packed = standardSurfaceInput.Load(int3(pixel, 0));
    // Derivatives are captured before any divergent return or decal-loop hit
    // test.  They remain valid while each receiver-associated decal chooses a
    // different box/atlas transform below.
    float3 positionDx = ddx(position.xyz);
    float3 positionDy = ddy(position.xyz);
    if (position.w <= 0.0f)
        return color;

    UniformObject object = gSceneObjects[packed.x];
    float3 normalW = mul((float3x3)gPerFrame.invViewRotationMat,
                         normalView.xyz);
    uint count = object.decalList & 0xffu;
    uint offset = object.decalList >> 8;
    for (uint k = 0u; k < count; ++k)
    {
        GpuDecal decal = gDecals[gObjectDecalIndices[offset + k]];
        float3 localPosition;
        if (!standardDecalBoxHit(decal, position.xyz, normalW, localPosition))
            continue;
        // World::Compile already filtered this list by receiver category. The
        // receiverMask remains in the record for CPU/diagnostic parity but is
        // not reinterpreted against object flags that are not in the packed
        // Standard surface contract.
        if (decal.diffuseTexIndex == kInvalidTextureIndex)
            continue; // neutral fallback for every blend mode
        float3 localPositionDx = mul((float3x3)decal.invModelMat, positionDx);
        float3 localPositionDy = mul((float3x3)decal.invModelMat, positionDy);
        float2 uv, uvDx, uvDy;
        standardDecalAtlasUVGrad(decal, localPosition, localPositionDx,
                                 localPositionDy, uv, uvDx, uvDy);
        float4 sample = sampleBindless2DGrad(decal.diffuseTexIndex, uv,
                                             uvDx, uvDy);
        color.rgb = standardDecalBlend(decal.blendMode, color.rgb, sample,
                                       decal.color);
    }
    return float4(color.rgb, color.a);
}
