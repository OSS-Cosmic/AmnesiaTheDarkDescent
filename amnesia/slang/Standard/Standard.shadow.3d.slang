// Authored spotlight shadow map raster.  Depth is Vulkan's explicit [0,1]
// convention, matching MatrixPerspectiveProjection(..., false).
import MaterialRaster;
import StandardRaster;
import SceneTypes;
import bindless;

struct ShadowConstants { float4x4 viewProjection; uint variabilityMask; };
[[vk::push_constant]] ShadowConstants shadowConstants;
struct V { float4 posH : SV_POSITION; float2 uv : TEXCOORD0;
            nointerpolation uint objectID : OBJECT_ID;
            nointerpolation uint materialID : MATERIAL_ID; };

[shader("vertex")] V vsMain(uint vertexID : SV_VertexID,
                             uint drawInst : SV_InstanceID,
                             uint baseInst : SV_StartInstanceLocation)
{
    uint id = drawInst + baseInst;
    UniformObject obj = gSceneObjects[id];
    StandardRasterVertex vertex = fetchStandardRasterVertex(obj, vertexID);
    float4 worldPosition = mul(obj.modelMat, float4(vertex.position, 1.0f));
    V v; v.posH = mul(shadowConstants.viewProjection, worldPosition);
    v.uv = standardTransformUV(obj, vertex.uv); v.objectID=id; v.materialID=obj.materialID;
    return v;
}

[shader("pixel")] void psMain(V v)
{
    uint flags = gSceneObjects[v.objectID].renderFlags;
    bool isStatic = (flags & 0x80000000u) != 0u;
    if ((isStatic && (shadowConstants.variabilityMask & 1u) == 0u) ||
        (!isStatic && (shadowConstants.variabilityMask & 2u) == 0u) ||
        (flags & kObjectFlagShadowCaster) == 0u)
        discard;
    VertexData data = {}; data.texC = v.uv;
    if (materialAlphaTest(data, v.materialID, gSceneObjects[v.objectID].dissolveAmount))
        discard;
}
