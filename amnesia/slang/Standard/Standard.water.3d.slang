#include "Constants.h"
import PerFrame.resource;
import SceneTypes;
import MaterialRaster;
import bindless;
import Fog;
import Translucent.WaterCommon;
import Utils.Color.ColorHelpers;
import StandardEnvironmentBindings;

[vk::binding(0,2)] Texture2D<float4> sceneColorInput;
[vk::binding(1,2)] Texture2D<float>  sceneDepthInput;
[vk::binding(2,2)] Texture2D<float4> waterReflectionInput;
[vk::binding(3,2)] SamplerState waterSampler;

struct WaterPush
{
    uint reflectionAvailable;
    uint refractionEnabled;
    uint reflectionEnabled;
    uint cubeMapTextureIndex;
    float4x4 reflectionViewProjection;
};
[vk::push_constant] WaterPush gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct I { float3 p:POSITION; float3 n:NORMAL; float4 t:TANGENT; float4 c:COLOR; float2 uv:TEXCOORD0; };
struct V { float3 p:POSITION; float2 uv:TEXCRD; float3 n:NORMAL; float3 t:TANGENT; float3 b:BITANGENT; float4 c:COLOR; nointerpolation uint id:OBJECTID; float4 h:SV_POSITION; };

public bool waterFinite(float x) { return x == x && abs(x) < 3.402823466e+37f; }
public float waterFiniteOr(float x, float fallback) { return waterFinite(x) ? x : fallback; }
public float3 waterSafeNormalize(float3 x, float3 fallback)
{
    float d=dot(x,x);
    return (waterFinite(d) && d>1.0e-12f) ? x*rsqrt(d) : fallback;
}
public float waterFresnelFinite(float edotN, float bias, float power)
{
    edotN=saturate(waterFiniteOr(edotN,0.0f)); bias=saturate(waterFiniteOr(bias,0.0f));
    power=max(waterFiniteOr(power,1.0f),0.0f);
    return saturate(bias+(1.0f-bias)*pow(max(1.0f-edotN,1.0e-6f),power));
}
public float waterReflectionFadeFinite(float viewZ, float start, float end)
{
    viewZ=waterFiniteOr(viewZ,0.0f); start=waterFiniteOr(start,0.0f); end=waterFiniteOr(end,0.0f);
    return end>0.0f && end>start ? 1.0f-saturate((viewZ-start)/(end-start)) : 1.0f;
}
public bool waterScreenUV(float2 uv)
{ return all(isfinite(uv)) && all(uv>=float2(0.0f)) && all(uv<=float2(1.0f)); }
public bool waterProjectReflectionUV(float3 p, float4x4 viewProjection, out float2 uv)
{
    float4 q=mul(viewProjection,float4(p,1.0f)); uv=float2(0.0f);
    if (!all(isfinite(q)) || q.w<=1.0e-6f) return false;
    uv=q.xy/q.w*float2(0.5f,-0.5f)+float2(0.5f); return waterScreenUV(uv);
}
public float3 waterCompose(float3 sampledScene, float3 tint, float3 reflected,
                           float fresnel, float visibility, float3 fogColor)
{
    return (tint*sampledScene+reflected*saturate(fresnel))*saturate(visibility)+fogColor/kSceneExposure;
}

[shader("vertex")]
V vsMain(I x,uint di:SV_InstanceID,uint bi:SV_StartInstanceLocation)
{
    uint id=di+bi; UniformObject o=gSceneObjects[id]; float3 p=mul(o.modelMat,float4(x.p,1)).xyz;
    float3 n=waterSafeNormalize(mul(transpose((float3x3)o.invModelMat),x.n),float3(0,1,0));
    float3 t0=mul((float3x3)o.modelMat,x.t.xyz); float3 b0=mul((float3x3)o.modelMat,cross(x.t.xyz,x.n)*x.t.w);
    V v; v.p=p; v.uv=mul(o.uvMat,float4(x.uv,0,1)).xy; v.id=id; v.c=x.c; v.n=n;
    v.t=waterSafeNormalize(t0-n*dot(n,t0),float3(1,0,0));
    float hand=dot(cross(v.t,n),b0)<0.0f?-1.0f:1.0f;
    v.b=waterSafeNormalize(cross(v.t,n)*hand,float3(0,0,1));
    v.h=mul(gPerFrame.projMat,mul(gPerFrame.viewMat,float4(p,1))); return v;
}

[shader("fragment")]
float4 psMain(V v):SV_TARGET0
{
    UniformObject o=gSceneObjects[v.id]; WaterMaterial w=loadWaterMaterial(o.materialID);
    if(o.dissolveAmount<=0.0f) discard;
    float2 uv=v.h.xy*gPerFrame.viewTexel; float2 distorted=uv;
    float3 eye=waterSafeNormalize(gPerFrame.posW-v.p,float3(0,1,0));
    float3 n=waterSafeNormalize(v.n,float3(0,1,0)); float3 t=waterSafeNormalize(v.t-n*dot(n,v.t),float3(1,0,0));
    float hand=dot(cross(t,n),v.b)<0.0f?-1.0f:1.0f; float3 b=waterSafeNormalize(cross(t,n)*hand,float3(0,0,1));
    float3 tangentNormal=float3(0,0,1);
    if (w.tex[1]!=kInvalidTextureIndex) {
        float3 wave=waterSafeNormalize(waterWaveNormalTangent(w,v.uv,ddx(v.uv),ddy(v.uv),gPerFrame.afT),float3(0,0,1));
        n=waterSafeNormalize(wave.x*t+wave.y*b+wave.z*n,n);
        tangentNormal=wave;
    }
    if (gPushConstants.refractionEnabled!=0u && (w.materialConfig&kMaterialFlagHasRefraction)!=0u) {
        float viewZ=max(abs(mul(gPerFrame.viewMat,float4(v.p,1)).z),1.0e-3f);
        distorted=uv+mul((float3x3)gPerFrame.viewMat,n).xy*w.refractionScale/viewZ;
        if (!waterScreenUV(distorted) || sceneDepthInput.SampleLevel(waterSampler,distorted,0)<v.h.z-1.0e-3f) distorted=uv;
    }
    float3 scene=sceneColorInput.SampleLevel(waterSampler,distorted,0).rgb; float3 tint=float3(1.0f);
    if ((w.materialConfig&kMaterialFlagEnableDiffuse)!=0u && w.tex[0]!=kInvalidTextureIndex)
        tint=sampleBindless2D(w.tex[0],v.uv).rgb*kWaterRefractionExposure;
    else
        tint*=kWaterRefractionExposure;
    // reflectionEnabled is the host's authored HasReflection setting; the
    // separate availability bit only says that a planar image was produced.
    float3 reflected=float3(0.0f); bool reflectionAuthored=gPushConstants.reflectionEnabled!=0u;
    if (gPushConstants.reflectionEnabled!=0u && reflectionAuthored) {
        float f=waterFresnelFinite(dot(eye,n),w.frenselBias,w.frenselPow)*waterReflectionFadeFinite(-mul(gPerFrame.viewMat,float4(v.p,1)).z,w.reflectionFadeStart,w.reflectionFadeEnd);
        if (gPushConstants.cubeMapTextureIndex!=kInvalidTextureIndex)
            reflected=sRGBToLinear(sampleEnvironmentCube(gPushConstants.cubeMapTextureIndex,reflect(-eye,n),0.0f).rgb);
        else {
            float2 ruv; bool planar=gPushConstants.reflectionAvailable!=0u && waterProjectReflectionUV(v.p,gPushConstants.reflectionViewProjection,ruv);
            if (planar) reflected=waterReflectionInput.SampleLevel(waterSampler,ruv,0).rgb;
        }
        reflected*=f;
    }
    FogResult fog=computeFog(v.p,gPerFrame.posW);
    if (gPushConstants.reflectionEnabled==0u) {
        // Legacy translucency_water.frag.fsl without reflection: a fixed fake
        // light over the wave normal, half-Lambert diffuse plus a pow-16 lobe.
        float lDotN=max(dot(normalize(float3(0.5f,0.5f,0.5f)),tangentNormal),0.0f);
        float fakeSpecular=pow(lDotN,16.0f);
        return float4(waterCompose(scene,tint*(lDotN*0.5f+0.5f),float3(fakeSpecular/kSceneExposure),
                                   1.0f,fog.visibility,fog.color),1.0f);
    }
    return float4(waterCompose(scene,tint,reflected,1.0f,fog.visibility,fog.color),1.0f);
}
