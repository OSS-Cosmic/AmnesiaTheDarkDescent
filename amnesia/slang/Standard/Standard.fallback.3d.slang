// Interpolated fallback. It emits the same material inputs as reconstruction
// (albedo, world position, view normal, UV/material/object IDs) without using
// barycentrics or any ray-tracing module.
import PerFrame.resource;
import MaterialRaster;
import StandardRaster;
import SceneTypes;
import bindless;
struct V { float4 posH:SV_POSITION; float3 posW:POSW; float3 geometricNormalV:GEOMETRIC_NORMAL; float3 normalV:NORMAL; float3 tangentV:TANGENT; float3 bitangentV:BITANGENT; float2 uv:TEXCOORD0; nointerpolation uint objectID:OBJECT_ID; nointerpolation uint materialID:MATERIAL_ID; float4 curClip:CLIP; float4 prevClip:PREVCLIP; };
struct O { float4 albedo:SV_TARGET0; float4 position:SV_TARGET1; float4 normal:SV_TARGET2; float4 shadingNormal:SV_TARGET3; uint4 surface:SV_TARGET4; float2 velocity:SV_TARGET5; };
[shader("vertex")]
V vsMain(uint vertexID : SV_VertexID, uint drawInst : SV_InstanceID,
         uint baseInst : SV_StartInstanceLocation)
{
    uint id = drawInst + baseInst;
    UniformObject obj = gSceneObjects[id];
    StandardRasterVertex vertex = fetchStandardRasterVertex(obj, vertexID);
    float4 worldPosition = mul(obj.modelMat, float4(vertex.position, 1.0f));
    V v;
    v.posH = mul(mul(gPerFrame.projMat, gPerFrame.viewMat), worldPosition);
    v.curClip = mul(mul(gPerFrame.projMat, gPerFrame.viewMat), worldPosition);
    v.prevClip = mul(mul(gPerFrame.prevProjMat, gPerFrame.prevViewMat), mul(obj.prevModelMat, float4(vertex.position, 1.0f)));
    v.posW = worldPosition.xyz;
    // Preserve the reference geometric-normal contract: this is the raw
    // inverse-transpose normal carried to the fragment and normalized there.
    v.geometricNormalV = standardViewNormal(obj, vertex.normal);
    standardViewBasis(obj, vertex.normal, vertex.tangent,
                      v.normalV, v.tangentV, v.bitangentV);
    v.uv = standardTransformUV(obj, vertex.uv);
    v.objectID = id;
    v.materialID = obj.materialID;
    return v;
}

[shader("pixel")]
O psMain(V v)
{
    DiffuseMaterial material = loadDiffuseMaterial(v.materialID);
    // Parallax shifts every material lookup, the alpha test included, as the
    // legacy solid_z and solid_diffuse passes both did.
    float2 uv = standardParallaxUV(material, v.uv,
                                   mul(gPerFrame.viewMat, float4(v.posW, 1.0f)).xyz,
                                   v.normalV, v.tangentV, v.bitangentV);
    VertexData vertexData = {};
    vertexData.posW = v.posW;
    vertexData.normalW = v.normalV;
    vertexData.texC = uv;
    if (materialAlphaTest(vertexData, v.materialID,
                          gSceneObjects[v.objectID].dissolveAmount))
        discard;

    O o;
    o.albedo = float4(sampleBindless2D(material.tex[0], uv).rgb, 1.0f);
    o.position = float4(v.posW, 1.0f);
    o.normal = float4(standardSafeNormalize(v.geometricNormalV, float3(0, 0, 1)), 1.0f);
    float3 normalizedBasis = standardSafeNormalize(v.normalV, float3(0, 0, 1));
    float3 shadingNormal = normalizedBasis;
    if ((material.materialConfig & kMaterialFlagEnableNormal) != 0u &&
        material.tex[1] != kInvalidTextureIndex)
    {
        float3 mapNormal = sampleBindless2D(material.tex[1], uv).xyz * 2.0f - 1.0f;
        shadingNormal = standardSafeNormalize(mapNormal.x * v.tangentV +
                                               mapNormal.y * v.bitangentV +
                                               mapNormal.z * v.normalV, normalizedBasis);
    }
    o.albedo.rgb = standardSolidCubeReflection(material, o.albedo.rgb, v.posW,
                                               shadingNormal, uv);
    o.shadingNormal = float4(shadingNormal, 1.0f);
    o.albedo.rgb = standardCubeMapFold(material, o.albedo.rgb,
                                       mul(gPerFrame.viewMat, float4(v.posW, 1.0f)).xyz,
                                       shadingNormal, v.uv);
    o.surface = uint4(v.objectID, v.materialID, asuint(uv.x), asuint(uv.y));
    float2 curUV = v.curClip.xy / v.curClip.w * float2(0.5, -0.5) + float2(0.5, 0.5);
    float2 prevUV = v.prevClip.xy / v.prevClip.w * float2(0.5, -0.5) + float2(0.5, 0.5);
    o.velocity = curUV - prevUV;
    return o;
}
