// Reconstruct a display-sized linear HDR image from the current scene color
// when a temporal provider failed after rendering a jittered frame.
//
// Sample convention:
//   inputUV = displayUV - jitterPixels / renderExtent
//
// The scene color is already linear HDR. This pass deliberately applies no
// tone map, exposure, gamma, or other display transform; the normal post
// chain remains responsible for those operations. The valid rectangle is in
// input/render space and is clamped at texel centers so bilinear filtering
// cannot read outside the authored scene pixels.
//
// Bindings (set 0):
//   binding 0 : SamplerState colorSampler (bilinear, clamp)
//   binding 1 : Texture2D<float4> sceneHdrColor

[vk::binding(0, 0)]
SamplerState colorSampler;

[vk::binding(1, 0)]
Texture2D<float4> sceneHdrColor;

struct SpatialFallbackPC
{
    uint2  renderExtent;
    uint2  displayExtent;
    uint2  validRectMin;
    uint2  validRectMax;
    float2 jitterPixels;
};

[vk::push_constant]
SpatialFallbackPC gPushConstants;

struct VSOut
{
    [vk::location(0)] float2 displayUV : TEXCOORD0;
    float4 posH : SV_POSITION;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;

    // Same three vertices as the depth presentation pass. The flipped
    // viewport maps this UV directly to top-left-origin framebuffer pixels.
    o.displayUV = float2(float((vertexId << 1) & 2),
                         float(vertexId & 2));
    o.posH = float4(float2(2.0f, -2.0f) * o.displayUV
                  + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

[shader("fragment")]
float4 psMain(VSOut pin) : SV_Target
{
    float2 renderExtentF = float2(float(gPushConstants.renderExtent.x),
                                  float(gPushConstants.renderExtent.y));
    float2 displayExtentF = float2(float(gPushConstants.displayExtent.x),
                                   float(gPushConstants.displayExtent.y));

    // Re-form the exact display pixel center before removing the raster
    // jitter. This keeps odd-sized display extents and the 1:1 case stable.
    float2 displayPixel = floor(pin.displayUV * displayExtentF);
    float2 displayUV = (displayPixel + 0.5f) / displayExtentF;
    float2 inputUV = displayUV - gPushConstants.jitterPixels / renderExtentF;

    float2 validMinUV = (float2(float(gPushConstants.validRectMin.x),
                                float(gPushConstants.validRectMin.y)) + 0.5f)
                      / renderExtentF;
    float2 validMaxUV = (float2(float(gPushConstants.validRectMax.x),
                                float(gPushConstants.validRectMax.y)) + 0.5f)
                      / renderExtentF;
    inputUV = clamp(inputUV, validMinUV, validMaxUV);

    return sceneHdrColor.SampleLevel(colorSampler, inputUV, 0.0f);
}
