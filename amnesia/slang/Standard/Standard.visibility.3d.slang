// Standard packed visibility raster pass. This module intentionally has no
// Scene/Material ray-tracing imports: it is a raster-only TriangleHit writer.
import PerFrame.resource;
import MaterialRaster;
import StandardRaster;
import SceneTypes;
import bindless;

struct V { float4 posH:SV_POSITION; float2 uv:TEXCOORD0; nointerpolation uint objectID:OBJECT_ID; nointerpolation uint materialID:MATERIAL_ID; float4 curClip:CLIP; float4 prevClip:PREVCLIP; };
struct O { uint4 hit:SV_TARGET0; float4 diagnostic:SV_TARGET1; float2 velocity:SV_TARGET2; };

[shader("vertex")] V vsMain(uint vertexID:SV_VertexID,uint drawInst:SV_InstanceID,uint baseInst:SV_StartInstanceLocation)
{
    uint id = drawInst + baseInst;
    UniformObject obj = gSceneObjects[id];
    StandardRasterVertex vertex = fetchStandardRasterVertex(obj, vertexID);
    float4 worldPosition = mul(obj.modelMat, float4(vertex.position, 1.0f));
    V v;
    v.posH = mul(mul(gPerFrame.projMat, gPerFrame.viewMat), worldPosition);
    v.curClip = mul(mul(gPerFrame.projMat, gPerFrame.viewMat), worldPosition);
    v.prevClip = mul(mul(gPerFrame.prevProjMat, gPerFrame.prevViewMat), mul(obj.prevModelMat, float4(vertex.position, 1.0f)));
    v.uv = standardTransformUV(obj, vertex.uv);
    v.objectID = id;
    v.materialID = obj.materialID;
    return v;
}
[shader("pixel")] O psMain(V v,uint primitiveID:SV_PrimitiveID,float3 bary:SV_Barycentrics)
{
    VertexData vd={}; vd.texC=v.uv;
    if(materialAlphaTest(vd,v.materialID,gSceneObjects[v.objectID].dissolveAmount)) discard;
    DiffuseMaterial material = loadDiffuseMaterial(v.materialID);
    O o;
    o.hit = uint4(v.objectID, primitiveID,
        (f32tof16(bary.y) & 0xffffu) | ((f32tof16(bary.z) & 0xffffu) << 16),
        1u);
    o.diagnostic = float4(sampleBindless2D(material.tex[0], v.uv).rgb, 1.0f);
    float2 curUV = v.curClip.xy / v.curClip.w * float2(0.5, -0.5) + float2(0.5, 0.5);
    float2 prevUV = v.prevClip.xy / v.prevClip.w * float2(0.5, -0.5) + float2(0.5, 0.5);
    o.velocity = curUV - prevUV;
    return o;
}
