// Bounded planar water capture. This is an ordinary raster pass; it never
// invokes the main renderer and therefore cannot recurse through water.
#include "Constants.h"
import Utils.Color.ColorHelpers;   // sRGBToLinear / linearToSRGB
import PerFrame.resource;
import SceneTypes;
import MaterialRaster;
import StandardLighting;
import bindless;

[vk::binding(5,2)] StructuredBuffer<StandardPointLight> standardPointLights;
[vk::binding(6,2)] StructuredBuffer<StandardSpotLight> standardSpotLights;
[vk::binding(7,2)] ConstantBuffer<StandardLightCounts> standardLightCounts;
[vk::binding(11,2)] Texture2DArray<float> standardShadowMap;
[vk::binding(13,2)] StructuredBuffer<StandardBoxLight> standardBoxLights;

struct ReflectionPush { float4 plane; };
[vk::push_constant] ReflectionPush pc;
struct I { float3 p:POSITION; float3 n:NORMAL; float4 t:TANGENT; float4 c:COLOR; float2 uv:TEXCOORD0; };
struct V { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; nointerpolation uint id:OBJECTID; float4 h:SV_POSITION; };
struct ReflectionOutputs { float4 color:SV_TARGET0; float4 position:SV_TARGET1; };

[shader("vertex")]
V vsMain(I x, uint di:SV_InstanceID, uint bi:SV_StartInstanceLocation)
{
    V v; v.id=di+bi; UniformObject o=gSceneObjects[v.id];
    v.p=mul(o.modelMat,float4(x.p,1)).xyz;
    v.n=standardSafeNormalize(mul(transpose((float3x3)o.invModelMat),x.n),float3(0,1,0));
    v.uv=mul(o.uvMat,float4(x.uv,0,1)).xy;
    v.h=mul(gPerFrame.projMat,mul(gPerFrame.viewMat,float4(v.p,1)));
    return v;
}

[shader("fragment")]
ReflectionOutputs psMain(V v)
{
    if (dot(pc.plane.xyz,v.p)+pc.plane.w < 0.0f) discard;
    UniformObject o=gSceneObjects[v.id]; DiffuseMaterial m=loadDiffuseMaterial(o.materialID);
    float3 c=float3(0.18f,0.22f,0.28f);
    if (m.tex[0]!=kInvalidTextureIndex) c=sampleBindless2D(m.tex[0],v.uv).rgb;
    StandardLightSurface s; s.color=c; s.normal=v.n; s.position=v.p;
    s.uv=v.uv; s.specular=float2(0,0); s.specularScale=0;
    float3 viewDirection=gPerFrame.posW-v.p;
    float3 lit=float3(0,0,0);
    bool boxLit=false;
    for(uint i=0u;i<standardLightCounts.boxCount;i++) {
        float3 boxContrib=standardBoxLight(float3(0,0,0),c,s.position,standardBoxLights[i]);
        if(any(boxContrib!=float3(0,0,0))) {
            lit+=boxContrib;
            boxLit=true;
        }
    }
    for(uint i=0u;i<standardLightCounts.pointCount;i++)
        lit+=standardPointLight(s,viewDirection,standardPointLights[i]);
    for(uint i=0u;i<standardLightCounts.spotCount;i++)
        lit+=standardSpotLight(s,viewDirection,standardSpotLights[i],1.0f,float3(0),float3(0));
    // Fallbacks share the lights' pre-exposure units (see Standard.environment.3d.slang).
    // skyBoxColor is a display-space tint, so tint the display albedo and decode.
    if (all(lit==float3(0,0,0))) lit=sRGBToLinear(saturate(linearToSRGB(c)*gPerFrame.skyBoxColor.rgb))/kSceneExposure;
    else if (!boxLit) lit+=c*0.08f/kSceneExposure;
    lit += standardIllumination(m, o.illuminationAmount, v.uv);
    ReflectionOutputs output;
    output.color = float4(lit,1.0f);
    output.position = float4(v.p,1.0f);
    return output;
}
