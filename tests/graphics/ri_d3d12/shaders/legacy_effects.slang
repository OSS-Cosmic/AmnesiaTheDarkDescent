// Calls production math; expected values live independently in the CPU runner.
import Standard.StandardLighting;
import LegacyEffectMath;
import Utils.Color.ColorHelpers;
import SceneTypes;
import Water.WaterSurface;
RWStructuredBuffer<float4> output : register(u0);
[shader("compute")]
[numthreads(1,1,1)]
void CSMain()
{
    StandardLightSurface s = {};
    s.color=float3(1); s.normal=float3(0,0,1);
    float3 a=standardLegacyLight(s,float3(0,0,1),float3(0,0,1),float3(.5),1,1,0);
    output[0]=float4(linearToSRGB(standardLightingToScene(a+a)*kSceneExposure),1);
    a=standardLegacyLight(s,float3(0,0,1),float3(0,0,1),float3(.2,.4,.1),1,1,0);
    float3 b=standardLegacyLight(s,float3(0,0,1),float3(0,0,1),float3(.1,.2,.3),1,1,0);
    output[1]=float4(linearToSRGB(standardLightingToScene(a+b)*kSceneExposure),1);
    StandardBoxLight box = {};
    box.config=kStandardLightEnabled;box.boxMin=float3(-1);box.boxMax=float3(1);
    box.color=float3(.1,.2,.3);
    a=standardBoxLight(float3(.9),float3(1),float3(0),box);
    box.blendFunc=kStandardBoxBlendAdd;box.color=float3(.1);
    output[2]=float4(standardBoxLight(a,float3(1),float3(0),box),1);
    float4x4 rotation=float4x4(0,0,1,0, 0,1,0,0, -1,0,0,0, 0,0,0,1);
    output[3]=float4(standardPointGoboDirection(rotation,float3(1,0,0)),1);
    rotation[0][3]=1000;rotation[1][3]=-500;rotation[2][3]=17;
    output[4]=float4(standardPointGoboDirection(rotation,float3(1,0,0)),1);
    output[5]=float4(legacyWaterColor(float3(.2,.4,.6),float3(1),float3(0),0),1);
    output[6]=float4(legacyWaterColor(float3(.2,.4,.6),float3(.5),float3(.1,.2,.3),.25),1);
    output[7]=float4(legacyWaterDistortion(float2(.2,.4),.1,-2,1),0,1);
    output[8]=float4(legacyWaterDistortion(float2(.2,.4),.1,-2,2),0,1);
    output[9]=float4(legacyWaterDistortion(float2(.2,.4),0,-2,2),0,1);
    output[10]=float4(legacyOverlay(float3(.25,.75,.5),float3(.75,.25,.2)),1);
    output[11]=float4(legacyInfectionMask(.2,1,.3,2),legacyInfectionMask(.2,1,.3,.5),legacyInfectionMask(0,2,0,2),1);
    output[12]=float4(linearToSRGB(standardLightingToScene(float3(1.4,.7,.4))*kSceneExposure),1);
    output[13]=float4(legacyInfectionMask(.2,1,0,2),legacyInfectionMask(.2,1,.8,2),legacyInfectionMask(.2,1,1,2),1);
    output[14]=float4(float3(.2,2,8)*kWaterRefractionExposure,1);
    output[15]=float4(legacyWaterFallback(float3(0,0,1)),0,1);
    output[16]=float4(legacyWaterFallback(float3(0,0,-1)),0,1);
    output[17]=float4(waterDepthTransmission(float3(.25,.5,1),0),1);
    output[18]=float4(waterDepthTransmission(float3(.25,.5,1),2),1);
    output[19]=float4(waterDepthTransmission(float3(1),20)*float3(.2,2,8),1);
    output[20]=float4(waterReflectionWeight(.8,.5,true),waterReflectionWeight(.8,.5,false),waterShoreDistortion(.075),1);
    WaterSurfaceFrame f=waterSurfaceFrame(float3(1,0,0),float3(0,0,1),float2(1,0),float2(0,1),float3(0,-1,0));
    output[21]=float4(f.T,1);
    f=waterSurfaceFrame(float3(0,0,1),float3(-1,0,0),float2(0,1),float2(-1,0),float3(0,-1,0));
    output[22]=float4(f.T,1);
    f=waterSurfaceFrame(float3(1,0,0),float3(0,0,1),float2(-1,0),float2(0,1),float3(0,-1,0));
    output[23]=float4(f.T,1);
    output[24]=float4(f.B,1);
    f=waterSurfaceFrame(float3(1,0,0),float3(0,0,1),float2(0),float2(0),float3(0,-1,0));
    output[25]=float4(length(f.T),length(f.B),dot(f.T,f.B),1);
    float4x4 invProjection=float4x4(1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1);
    output[26]=float4(waterBackgroundThickness(float2(.5),.5,-.25,invProjection),
        waterBackgroundThickness(float2(.5),1,-.25,invProjection),
        waterBackgroundThickness(float2(.5),.1,-.25,invProjection),1);
}
