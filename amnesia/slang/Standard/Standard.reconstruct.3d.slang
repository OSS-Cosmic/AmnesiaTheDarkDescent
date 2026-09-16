// Fullscreen graphics reconstruction from the packed raster TriangleHit.
// BDA triangle fetch makes this path equivalent to interpolated raster data,
// including inverse-transpose normals and material UV transforms.
import PerFrame.resource;
import MaterialRaster;
import StandardRaster;
import SceneTypes;
import bindless;
import HitInfo;
// Set 0 is the externally-owned bindless layout; set 1 is PerFrame. These
// pass-local inputs live in set 2 so they cannot alias either layout.
[vk::binding(0,2)] Texture2D<uint4> visibilityInput;
[vk::binding(1,2)] Texture2D<float> depthInput;
struct V { float4 posH:SV_POSITION; float2 uv:TEXCOORD0; };
struct O { float4 albedo:SV_TARGET0; float4 position:SV_TARGET1; float4 normal:SV_TARGET2; float4 shadingNormal:SV_TARGET3; uint4 surface:SV_TARGET4; };
[shader("vertex")] V vsMain(uint id:SV_VertexID) { V v; v.uv=float2(float((id<<1)&2),float(id&2)); v.posH=float4(v.uv*2-1,0,1); return v; }
[shader("pixel")] O psMain(V v)
{
    uint2 pixel = uint2(v.posH.xy);
    float depth = depthInput.Load(int3(pixel, 0));
    HitInfo hitInfo = HitInfo(visibilityInput.Load(int3(pixel, 0)));
    if (!hitInfo.isValid())
        discard;
    // Keep the depth binding live for the pass ABI. Valid normalized depth is
    // [0, 1]; in particular, depth == 1 is a valid LEQUAL far-plane hit.
    if (depth < 0.0f)
        discard;

    TriangleHit hit = hitInfo.getTriangleHit();
    UniformObject obj = gSceneObjects[hit.instanceID];
    // Visibility is already coverage-tested. Do not re-run material alpha
    // after half-precision barycentric reconstruction, and do not reject the
    // valid far-plane hit accepted by the LEQUAL visibility depth test.
    uint baseVertex = hit.primitiveIndex * 3u;
    StandardRasterVertex v0 = fetchStandardRasterVertex(obj, baseVertex + 0u);
    StandardRasterVertex v1 = fetchStandardRasterVertex(obj, baseVertex + 1u);
    StandardRasterVertex v2 = fetchStandardRasterVertex(obj, baseVertex + 2u);
    float3 barycentrics = float3(1.0f - hit.barycentrics.x - hit.barycentrics.y,
                                hit.barycentrics.x, hit.barycentrics.y);
    float3 localPosition = v0.position * barycentrics.x
                         + v1.position * barycentrics.y
                         + v2.position * barycentrics.z;
    float4 worldPosition = mul(obj.modelMat, float4(localPosition, 1.0f));
    // Match enhanced solid_diffuse.vert.fsl/raster interpolation: transform
    // each vertex basis first, then interpolate the three normalized outputs.
    float3 geometricNormal0 = standardViewNormal(obj, v0.normal);
    float3 geometricNormal1 = standardViewNormal(obj, v1.normal);
    float3 geometricNormal2 = standardViewNormal(obj, v2.normal);
    float3 shadingNormalV0, shadingTangentV0, shadingBitangentV0;
    float3 shadingNormalV1, shadingTangentV1, shadingBitangentV1;
    float3 shadingNormalV2, shadingTangentV2, shadingBitangentV2;
    standardViewBasis(obj, v0.normal, v0.tangent,
                      shadingNormalV0, shadingTangentV0, shadingBitangentV0);
    standardViewBasis(obj, v1.normal, v1.tangent,
                      shadingNormalV1, shadingTangentV1, shadingBitangentV1);
    standardViewBasis(obj, v2.normal, v2.tangent,
                      shadingNormalV2, shadingTangentV2, shadingBitangentV2);
    float3 viewNormal = geometricNormal0 * barycentrics.x
                      + geometricNormal1 * barycentrics.y
                      + geometricNormal2 * barycentrics.z;
    float3 shadingNormalView = shadingNormalV0 * barycentrics.x
                             + shadingNormalV1 * barycentrics.y
                             + shadingNormalV2 * barycentrics.z;
    float3 viewTangent = shadingTangentV0 * barycentrics.x
                       + shadingTangentV1 * barycentrics.y
                       + shadingTangentV2 * barycentrics.z;
    float3 viewBitangent = shadingBitangentV0 * barycentrics.x
                         + shadingBitangentV1 * barycentrics.y
                         + shadingBitangentV2 * barycentrics.z;
    float2 localUV = v0.uv * barycentrics.x + v1.uv * barycentrics.y
                   + v2.uv * barycentrics.z;
    float2 uv = standardTransformUV(obj, localUV);

    DiffuseMaterial material = loadDiffuseMaterial(obj.materialID);
    // Legacy solid_diffuse parallax shifts every material lookup and the
    // stored surface UV that the light resolve samples specular from.
    uv = standardParallaxUV(material, uv, mul(gPerFrame.viewMat, worldPosition).xyz,
                            standardSafeNormalize(shadingNormalView, float3(0, 0, 1)),
                            viewTangent, viewBitangent);
    O o;
    o.albedo = float4(sampleBindless2D(material.tex[0], uv).rgb, 1.0f);
    o.position = float4(worldPosition.xyz, 1.0f);
    o.normal = float4(standardSafeNormalize(viewNormal, float3(0, 0, 1)), 1.0f);
    float3 normalizedBasis = standardSafeNormalize(shadingNormalView, float3(0, 0, 1));
    float3 shadingNormal = normalizedBasis;
    if ((material.materialConfig & kMaterialFlagEnableNormal) != 0u &&
        material.tex[1] != kInvalidTextureIndex)
    {
        float3 mapNormal = sampleBindless2D(material.tex[1], uv).xyz * 2.0f - 1.0f;
        shadingNormal = standardSafeNormalize(mapNormal.x * viewTangent +
                                               mapNormal.y * viewBitangent +
                                               mapNormal.z * shadingNormalView, normalizedBasis);
    }
    o.albedo.rgb = standardSolidCubeReflection(material, o.albedo.rgb, worldPosition.xyz,
                                               shadingNormal, uv);
    o.shadingNormal = float4(shadingNormal, 1.0f);
    o.albedo.rgb = standardCubeMapFold(material, o.albedo.rgb,
                                       mul(gPerFrame.viewMat, worldPosition).xyz,
                                       shadingNormal, uv);
    o.surface = uint4(hit.instanceID, obj.materialID, asuint(uv.x), asuint(uv.y));
    return o;
}
