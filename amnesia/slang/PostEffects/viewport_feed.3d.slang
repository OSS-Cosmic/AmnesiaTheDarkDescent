/// Copyright 2026 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0

// Copy the authored viewport rectangle from a source image into a fullscreen
// destination. The source image may be larger than the authored rectangle
// (for example, when a guard band is present), so the rectangle is kept in
// source-pixel space rather than inferred from the image extent.
//
// Project convention: UV (0, 0) is the top-left source/destination pixel and
// Y increases downward. The host pairs this shader with the engine's negative-
// height viewport, as it does for the other .3d presentation passes.
//
// Bindings (set 0):
//   binding 0 : SamplerState sourceSampler (nearest or linear, supplied by host)
//   binding 1 : Texture2D<float4> sourceColor (RGBA8, RGBA16F, or other RGBA/HDR)
//
// pixelRectMin/max are inclusive source-pixel bounds. Both bounds are passed
// in source-image pixel coordinates; the host must provide a non-empty,
// in-bounds rectangle and a non-zero sourceExtent.

[vk::binding(0, 0)]
SamplerState sourceSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceColor;

struct ViewportFeedPC
{
    uint2 sourceExtent;
    uint2 pixelRectMin;
    uint2 pixelRectMax;
};

[vk::push_constant]
ViewportFeedPC gPushConstants;

struct VSOut
{
    [vk::location(0)] float2 v_uv : TEXCOORD0;
    float4 posH : SV_POSITION;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;

    // Fullscreen triangle. With the host's negative-height viewport this is
    // top-left-origin UV space in both Vulkan and D3D12 paths.
    o.v_uv = float2(float((vertexId << 1) & 2),
                    float(vertexId & 2));
    o.posH = float4(float2(2.0f, -2.0f) * o.v_uv
                  + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

[shader("fragment")]
float4 psMain(VSOut pin) : SV_Target
{
    float2 sourceExtent = float2(float(gPushConstants.sourceExtent.x),
                                 float(gPushConstants.sourceExtent.y));
    float2 rectMin = float2(float(gPushConstants.pixelRectMin.x),
                            float(gPushConstants.pixelRectMin.y));
    float2 rectMax = float2(float(gPushConstants.pixelRectMax.x),
                            float(gPushConstants.pixelRectMax.y));

    // Interpolate in pixel space, then address the center of the selected
    // rectangle. Clamping the normalized coordinate to the first/last texel
    // centers prevents bilinear filtering from pulling guard-band or other
    // unrelated pixels across either crop edge. SampleLevel intentionally
    // leaves nearest/linear filtering to the sampler supplied by the host.
    // The bounds are inclusive, so the span contains max-min+1 texels.
	// UVs address output pixel centers; subtract half a pixel before the
	// explicit center offset below so an inclusive rect maps exactly.
	float2 sourcePixel = rectMin + pin.v_uv * (rectMax - rectMin + 1.0f) - 0.5f;
    float2 minCenterUV = (rectMin + 0.5f) / sourceExtent;
    float2 maxCenterUV = (rectMax + 0.5f) / sourceExtent;
    float2 sourceUV = clamp((sourcePixel + 0.5f) / sourceExtent,
                            minCenterUV, maxCenterUV);

    // Do not force alpha to one or apply any color transform: the feed carries
    // the complete RGBA value, including linear HDR values.
    return sourceColor.SampleLevel(sourceSampler, sourceUV, 0.0f);
}
