// RT-free Standard environment composite. Scene color is deliberately a
// separate input image from the output attachment; the host ping-pongs the
// color target so this pass never samples its active attachment.
import Utils.Color.ColorHelpers;   // sRGBToLinear / linearToSRGB
import PerFrame.resource;
import SceneTypes;
import Fog;
import StandardEnvironmentBindings;

[vk::binding(0,2)] Texture2D<float4> sceneColorInput;
[vk::binding(1,2)] Texture2D<float4> positionInput;

struct V { float4 posH : SV_POSITION; float2 uv : TEXCOORD0; };
struct O { float4 color : SV_TARGET0; };
struct EnvironmentRay { float3 origin; float3 direction; float3 farPoint; };

EnvironmentRay reconstructEnvironmentRay(float2 pixel, float2 viewTexel,
                                         float4x4 projMat, float4x4 invProjMat,
                                         float4x4 invViewMat, float3 cameraPosition,
                                         uint orthographicCamera)
{
    EnvironmentRay result;
    result.origin = cameraPosition;
    float2 uv = (pixel + 0.5f) * viewTexel;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    if (orthographicCamera != 0u) {
        float4 zeroClip = mul(projMat, float4(0.0f, 0.0f, 0.0f, 1.0f));
        float4 originView = mul(invProjMat,
                                float4(ndc, zeroClip.z / zeroClip.w, 1.0f));
        originView /= originView.w;
        result.origin = mul(invViewMat, float4(originView.xyz, 1.0f)).xyz;
    }
    float4 farView = mul(invProjMat, float4(ndc, 1.0f, 1.0f));
    farView /= farView.w;
    result.farPoint = mul(invViewMat, float4(farView.xyz, 1.0f)).xyz;
    result.direction = normalize(result.farPoint - result.origin);
    return result;
}

float environmentBackgroundAlpha(float clearAlpha, float skyAlpha,
                                 uint skyBoxEnabled)
{
    return skyBoxEnabled != 0u ? skyAlpha : clearAlpha;
}

[shader("vertex")] V vsMain(uint id : SV_VertexID)
{
    V v; v.uv = float2(float((id << 1) & 2), float(id & 2));
    v.posH = float4(v.uv * 2.0f - 1.0f, 0.0f, 1.0f); return v;
}

[shader("pixel")] O psMain(V v)
{
    uint2 pixel = min(uint2(v.posH.xy), uint2(gPerFrame.viewportSize) - 1u);
    float4 source = sceneColorInput.Load(int3(pixel, 0));
    float4 position = positionInput.Load(int3(pixel, 0));
    float3 color = source.rgb;
    float3 cameraOrigin = gPerFrame.posW;
    float3 ray = float3(0.0f, 0.0f, 1.0f);
    if (position.a > 0.5f) {
        if (gPerFrame.fogEnabled != 0u || gPerFrame.fogAreaCount != 0u) {
            if (gPerFrame.orthographicCamera != 0u)
                cameraOrigin = reconstructEnvironmentRay(
                    float2(pixel), gPerFrame.viewTexel, gPerFrame.projMat,
                    gPerFrame.invProjMat, gPerFrame.invViewMat, gPerFrame.posW,
                    gPerFrame.orthographicCamera).origin;
            color = applyFog(color, position.xyz, cameraOrigin);
        }
    } else {
        // The negative viewport used by this pass makes fragment coordinates
        // top-left origin. Reconstruct the far-plane point through the actual
        // inverse projection, so aspect/FOV and orthographic cameras agree.
        EnvironmentRay reconstructed = reconstructEnvironmentRay(
            float2(pixel), gPerFrame.viewTexel, gPerFrame.projMat,
            gPerFrame.invProjMat, gPerFrame.invViewMat, gPerFrame.posW,
            gPerFrame.orthographicCamera);
        cameraOrigin = reconstructed.origin;
        ray = reconstructed.direction;
        // Base game: clear colour and sky (cube texel x tint) are display
        // values written straight to the 8-bit buffer. Build the display term
        // and emit sRGBToLinear(D) / kSceneExposure so the tonemap returns D.
        float3 display = gPerFrame.clearColor.rgb;
        float alpha = environmentBackgroundAlpha(gPerFrame.clearColor.a,
                                                  gPerFrame.skyBoxColor.a,
                                                  gPerFrame.skyBoxEnabled);
        if (gPerFrame.skyBoxEnabled != 0u) {
            display = gPerFrame.skyBoxColor.rgb;
            if (gPerFrame.skyBoxTextureIndex != 0xffffffffu)
                display = sampleEnvironmentCube(gPerFrame.skyBoxTextureIndex, ray, 0.0f).rgb
                        * gPerFrame.skyBoxColor.rgb;
        }
        color = sRGBToLinear(saturate(display)) / kSceneExposure;
        // Background pixels still represent a ray ending at the finite far
        // plane. This keeps world and local fog visible over sky/clear color.
        if (gPerFrame.fogEnabled != 0u || gPerFrame.fogAreaCount != 0u)
            color = applyFog(color, reconstructed.farPoint, cameraOrigin);
        O o; o.color = float4(color, alpha); return o;
    }
    O o; o.color = float4(color, source.a); return o;
}
