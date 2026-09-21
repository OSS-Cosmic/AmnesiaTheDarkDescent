#include "Constants.h"
import PerFrame.resource;
import SceneTypes;
import MaterialRaster;
import StandardLighting;
import Fog;
import bindless;
import BlendModes;
import Utils.Color.ColorHelpers;

// Set 2 is owned by StandardTranslucentPass. The copy is made immediately
// before each refractive mesh, so it contains all preceding surfaces.
[vk::binding(0,2)] Texture2D<float4> sceneColorInput;
[vk::binding(4,2)] Texture2D<float> sceneDepthInput;
[vk::binding(5,2)] SamplerState sceneColorSampler;
[vk::binding(1,2)] StructuredBuffer<StandardPointLight> standardPointLights;
[vk::binding(2,2)] StructuredBuffer<StandardSpotLight> standardSpotLights;
[vk::binding(3,2)] ConstantBuffer<StandardLightCounts> standardLightCounts;

struct I { float3 p:POSITION; float3 n:NORMAL; float4 t:TANGENT; float4 c:COLOR; float2 uv:TEXCOORD0; };
struct V { float3 viewPos:VIEWPOS; float2 uv:TEXCRD; float3 n:NORMAL; float3 t:TANGENT; float3 b:BITANGENT; float4 c:COLOR; nointerpolation uint objectId:OBJECTID; float4 posH:SV_POSITION; };
// Kept in lockstep with StandardTranslucentPass.cpp.
// reflectionOnly selects the legacy UseIlluminationTrans additive pass.
// clipPlane is the half-space the planar water reflection keeps. A zero normal
// disables it, which is what the main viewport passes.
struct Push { uint blendMode; float sceneAlpha; uint refractive; float lightLevel; uint reflectionOnly; float4 clipPlane; };
[vk::push_constant] Push gPushConstants HPL_PUSH_CONSTANT_REGISTER;

float3 safeN(float3 x, float3 fallback)
{
    float d=dot(x,x);
    return isfinite(d)&&d>1.0e-12f&&d<1.0e30f ? x*rsqrt(d) : fallback;
}

[shader("vertex")]
V vsMain(I x, uint drawInst:SV_InstanceID, uint baseInst:SV_StartInstanceLocation)
{
    uint id=drawInst+baseInst; UniformObject o=gSceneObjects[id];
    float3 pW=mul(o.modelMat,float4(x.p,1)).xyz;
    float3x3 modelLinear=(float3x3)o.modelMat;
    float3x3 normalW=transpose((float3x3)o.invModelMat);
    V v; v.viewPos=mul(gPerFrame.viewMat,float4(pW,1)).xyz;
    v.posH=mul(gPerFrame.projMat,float4(v.viewPos,1));
    v.uv=mul(o.uvMat,float4(x.uv,0,1)).xy; v.c=x.c; v.objectId=id;
    v.n=safeN(mul(normalW,x.n),float3(0,0,1));
    float3 authoredTangentW=mul(modelLinear,x.t.xyz);
    float3 authoredBitangentW=mul(modelLinear,cross(x.t.xyz,x.n)*x.t.w);
    v.t=safeN(authoredTangentW-v.n*dot(v.n,authoredTangentW),standardOrthoTangent(v.n));
    v.b=safeN(authoredBitangentW-v.n*dot(v.n,authoredBitangentW),standardOrthoTangent(v.n));
    return v;
}

float3 materialNormalW(V v,TranslucentMaterial m)
{
    float3 n=safeN(v.n,float3(0,0,1));
    if ((m.materialConfig&kMaterialFlagEnableNormal)!=0u&&m.tex[1]!=kInvalidTextureIndex)
    {
        float3 t=safeN(v.t-n*dot(n,v.t),standardOrthoTangent(n));
        float handedness=dot(cross(t,n),v.b)<0.0f?-1.0f:1.0f;
        float3 b=safeN(cross(t,n)*handedness,standardOrthoTangent(n));
        float3 mn=sampleBindless2D(m.tex[1],v.uv).xyz*2.0f-1.0f;
        n=safeN(mn.x*t+mn.y*b+mn.z*n,n);
    }
    return n;
}

float3 transLighting(float3 p,float3 n,float3 cameraToSurfaceW,TranslucentMaterial m,float3 dx,float3 dy)
{
    if ((m.materialConfig&kMaterialFlagLitDiffuse)==0u)return float3(1);
    StandardLightSurface s; s.color=float3(1);s.normal=n;s.position=p;s.uv=float2(0);
    s.specular=float2(0);s.specularScale=0; float3 r=float3(0);
    for(uint i=0u;i<standardLightCounts.pointCount;i++)r+=standardPointLight(s,cameraToSurfaceW,standardPointLights[i]);
    for(uint i=0u;i<standardLightCounts.spotCount;i++)r+=standardSpotLight(s,cameraToSurfaceW,standardSpotLights[i],1,dx,dy);
    return r*max(m.litDiffuseScale,0.0f);
}

float fresnel(float e,float bias,float power)
{ return saturate(bias+(1.0f-bias)*pow(max(1.0f-e,1.0e-6f),power)); }

// Refractive draws use a blend-disabled pipeline, so the prepared, encoded
// source is composed here against the per-draw scene copy with the legacy
// fixed-function rule of its blend mode.
float3 standardTranslucentRefractionCompose(uint blendMode,float3 source,float alpha,float3 refracted)
{
    if (blendMode==kBlendModeAdd)
        return source+refracted;
    if (blendMode==kBlendModeMul)
        return source*refracted;
    if (blendMode==kBlendModeMulX2)
        return 2.0f*source*refracted;
    if (blendMode==kBlendModeAlpha||blendMode==kBlendModePremulAlpha)
        return source+refracted*(1.0f-alpha);
    return refracted;
}

// Legacy refraction direction: RefractionNormals (forced on without a normal
// map) bends by the view-space surface normal; otherwise by the raw
// tangent-space normal-map sample.
float2 standardTranslucentRefractionDirection(V v,TranslucentMaterial m,uint cfg,float3 n)
{
    bool hasNormalMap=(cfg&kMaterialFlagEnableNormal)!=0u&&m.tex[1]!=kInvalidTextureIndex;
    if((cfg&kMaterialFlagUseRefractionNormals)==0u&&hasNormalMap)
        return sampleBindless2D(m.tex[1],v.uv).xy*2.0f-1.0f;
    return safeN(mul((float3x3)gPerFrame.viewMat,n),float3(0,0,1)).xy;
}

// Legacy translucency cube reflection: environment colour (cube alpha
// applied), Fresnel term and optional rim term for the surface normal.
bool standardTranslucentEnvironment(V v,TranslucentMaterial m,uint cfg,float3 n,
    out float3 env,out float fresnelTerm,out float rimTerm)
{
    env=float3(0.0f);fresnelTerm=0.0f;rimTerm=0.0f;
    if((cfg&kMaterialFlagEnableCubeMap)==0u||m.cubeMapTextureIndex==kInvalidTextureIndex)
        return false;
    float3 nV=safeN(mul((float3x3)gPerFrame.viewMat,n),float3(0,0,1));float3 eye=safeN(v.viewPos,nV);
    fresnelTerm=fresnel(saturate(dot(-eye,nV)),m.frenselBias,m.frenselPow);
    float3 dirW=mul((float3x3)gPerFrame.invViewRotationMat,reflect(eye,nV));
    float4 sampled=gTexturesCube[NonUniformResourceIndex(m.cubeMapTextureIndex)].Sample(gMaterialSampler,dirW);
    env=sRGBToLinear(sampled.rgb);
    if((cfg&kMaterialFlagEnableCubeMapAlpha)!=0u&&m.tex[7]!=kInvalidTextureIndex)env*=sampleBindless2D(m.tex[7],v.uv).a;
    if(m.rimLightMul>0.0f)rimTerm=pow(max(1.0f-abs(dot(nV,float3(0,0,-1))),1.0e-6f),m.rimLightPow)*m.rimLightMul;
    return true;
}

// Legacy env-map term: cube reflection and rim, faded by opacity and light
// level, encoded as an additive radiance source. Legacy adds it after the
// refraction compose, or in the separate additive pass when not refractive;
// it never enters a Mul/MulX2 multiplier.
float3 standardTranslucentReflection(float3 env,float fresnelTerm,float rimTerm,float opacity,float ll)
{
    return encodeBlendOutputLinear(kBlendModeAdd,encodeStandardBlendSource(kBlendModeAdd,
        applyBlendModulation(kBlendModeAdd,float4(env*(fresnelTerm+rimTerm),1.0f),opacity,ll))).rgb;
}

[shader("pixel")]
float4 psMain(V v):SV_TARGET0
{
    UniformObject o=gSceneObjects[v.objectId];TranslucentMaterial m=loadTranslucentMaterial(o.materialID);
    if(o.dissolveAmount<=0.0f)discard;
    float3 worldPos=mul(gPerFrame.invViewMat,float4(v.viewPos,1)).xyz;
    if(any(gPushConstants.clipPlane.xyz!=0.0f)&&dot(gPushConstants.clipPlane.xyz,worldPos)+gPushConstants.clipPlane.w<0.0f)discard;
    uint cfg=m.materialConfig;float4 source=float4(0,0,0,1);
    if ((cfg&kMaterialFlagEnableDiffuse)!=0u&&m.tex[0]!=kInvalidTextureIndex)
    {
        if(gPushConstants.blendMode==kBlendModeAdd&&(cfg&kMaterialFlagSmoothHalo)!=0u)source=smoothHaloLinearSample(v.uv,v.c);
        else source=translucentLinearSample(sampleBindless2D(m.tex[0],v.uv),v.c,gPushConstants.blendMode,(cfg&kMaterialFlagDiffuseIsMask)!=0u);
    }
    bool dissolve=o.dissolveAmount<1.0f||(cfg&(kMaterialFlagEnableDissolveAlpha|kMaterialFlagUseDissolveFilter))!=0u;
    if(dissolve)
    {
        float d=gDissolveMap.SampleLevel(gMaterialSampler,v.uv,0).r;
        if((cfg&kMaterialFlagEnableDissolveAlpha)!=0u)
        {
            d=d*0.25f+0.75f;
            float da=1.0f;
            if(m.tex[6]!=kInvalidTextureIndex)da=sampleBindless2DLevel(m.tex[6],v.uv,0).r;
            d-=0.25f-da*0.25f;
        }
        else d=d*0.5f+0.5f;
        if(d-(1.0f-o.dissolveAmount*source.a)*0.5f<0.5f)discard;
    }
    if(source.a<=0.0f||!isfinite(source.a))discard;
    float3 p=worldPos;
    float3 n=materialNormalW(v,m);
    float3 cameraToSurfaceW=gPerFrame.posW-p;
    float3 dx=ddx(p),dy=ddy(p);
    // Legacy folds fog into opacity before blend preparation: the destination
    // is already fogged, so a fogged surface fades instead of being painted
    // with fog colour.
    FogResult fog=computeFog(p,gPerFrame.posW);
    float opacity=gPushConstants.sceneAlpha*fog.visibility;
    source.rgb*=transLighting(p,n,cameraToSurfaceW,m,dx,dy);
    // Lit radiance is already exposure-scaled; encodeStandardBlendSource divides once.
    if((cfg&kMaterialFlagLitDiffuse)!=0u&&gPushConstants.blendMode!=kBlendModeMul&&gPushConstants.blendMode!=kBlendModeMulX2)
        source.rgb*=kSceneExposure;
    float ll=1.0f;
    if ((cfg&kMaterialFlagAffectedByLightLevel)!=0u&&
        (cfg&kMaterialFlagLitDiffuse)==0u)
        ll=saturate(gPushConstants.lightLevel); // legacy per-object CPU level (StandardTranslucentPass)
    float3 env;float envFresnel;float envRim;
    const bool hasEnvironment=standardTranslucentEnvironment(v,m,cfg,n,env,envFresnel,envRim);
    if(gPushConstants.reflectionOnly!=0u)
    {
        // Legacy UseIlluminationTrans pass: the object is drawn again with
        // additive blending and only the cube reflection and rim terms.
        if(!hasEnvironment)discard;
        float3 reflection=standardTranslucentReflection(env,envFresnel,envRim,opacity,ll);
        if(!all(isfinite(reflection)))discard;
        return float4(reflection,1.0f);
    }
    float4 out=encodeBlendOutputLinear(gPushConstants.blendMode,encodeStandardBlendSource(gPushConstants.blendMode,
        applyBlendModulation(gPushConstants.blendMode,source,opacity,ll)));
    if(out.a<=0.0f||!isfinite(out.a))discard;
    if(!all(isfinite(out.rgb)))discard;
    if(gPushConstants.refractive!=0u)
    {
        // The copy was taken immediately before this draw, so the undistorted
        // sample is exactly the destination the hardware blend would have read.
        float2 undistortedUV=v.posH.xy*gPerFrame.viewTexel;
        float2 uv=undistortedUV;
        if((cfg&kMaterialFlagHasRefraction)!=0u)
        {
            float2 dir=standardTranslucentRefractionDirection(v,m,cfg,n);
            // Legacy distance falloff; view-space +Y is up while texture V
            // grows down under the flipped viewport.
            float invDist=min(1.0f/max(-v.viewPos.z,1.0e-3f),10.0f);
            uv+=float2(dir.x,-dir.y)*m.refractionScale*invDist;
            if(!all(isfinite(uv))||any(uv<float2(0))||any(uv>float2(1)))
                uv=undistortedUV;
            else if((cfg&kMaterialFlagUseRefractionEdgeCheck)!=0u)
            {
                // Both values are device-depth values. A nearer occluder at the
                // distorted sample must not bleed into the surface.
                float sceneZ=sceneDepthInput.SampleLevel(sceneColorSampler,uv,0);
                if(!isfinite(sceneZ)||sceneZ<v.posH.z-1.0e-3f)
                    uv=undistortedUV;
            }
        }
        float3 refracted=sceneColorInput.SampleLevel(sceneColorSampler,uv,0).rgb;
        if(!all(isfinite(refracted)))discard;
        float3 composed=standardTranslucentRefractionCompose(gPushConstants.blendMode,out.rgb,out.a,refracted);
        if(hasEnvironment)
            composed+=standardTranslucentReflection(env,envFresnel,envRim,opacity,ll);
        if(!all(isfinite(composed)))discard;
        return float4(composed,1.0f);
    }
    return out;
}
