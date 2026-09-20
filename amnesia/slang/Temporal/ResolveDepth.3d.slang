// Reconstruct display-resolution, unjittered device depth from the smaller,
// jittered render-resolution depth buffer. The fragment stage writes depth
// only; stencil is NOT produced by this shader (the C++ side clears stencil in
// the attachment).
//
// Sample convention:
//   inputUV = displayUV - jitterPixels / renderExtent
//
// The output is display-sized unjittered device depth. This uses a 3x3 point
// footprint rather than the cheaper 2x2 footprint because Amnesia has many
// thin foreground silhouettes (wires, railings, and chain links), for which a
// 3x3 neighborhood is more likely to retain the foreground. The rejected 2x2
// option costs fewer samples, but misses more thin silhouettes. Taking the
// conservative minimum over this footprint dilates foreground silhouettes by
// one source-texel radius; that tradeoff is intentional and should be known by
// the C++ side.
//
// Depth convention verified in HPL2/core/sources/graphics/HybridRenderer.cpp:
// the G-buffer depth is cleared to 1.0f, uses depthMin=0.0f/depthMax=1.0f,
// and its GBufferMRTPipelineDesc uses VK_COMPARE_OP_LESS_OR_EQUAL. Therefore
// this is non-reversed depth (near = 0, far = 1), and conservative nearest is
// the minimum depth value.
//
// Bindings (set 0):
//   binding 0 : SamplerState depthSampler (point, clamp)
//   binding 1 : Texture2D<float> renderDepth (depth aspect only)

[vk::binding(0, 0)]
SamplerState depthSampler;

[vk::binding(1, 0)]
Texture2D<float> renderDepth;

struct ResolveDepthPC
{
    uint2  renderExtent;
    uint2  displayExtent;
    float2 jitterPixels;
};

[vk::push_constant]
ResolveDepthPC gPushConstants;

struct VSOut
{
    [vk::location(0)] float2 displayUV : TEXCOORD0;
    float4 posH : SV_POSITION;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;

    // Same three vertices as the shared post-effect fullscreen triangle. The
    // flipped viewport maps this UV directly to top-left-origin framebuffer
    // coordinates: ndc = float2(2, -2) * uv + float2(-1, 1).
    o.displayUV = float2(float((vertexId << 1) & 2),
                         float(vertexId & 2));
    o.posH = float4(float2(2.0f, -2.0f) * o.displayUV
                  + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// Return the conservative nearest (minimum) device depth in a bounded 3x3
// source footprint. SampleLevel is used at explicit texel centers, and the
// bound sampler must be point filtered so no bilinear depth is invented.
[shader("fragment")]
float psMain(VSOut pin) : SV_Depth
{
    float2 renderExtentF = float2(float(gPushConstants.renderExtent.x),
                                  float(gPushConstants.renderExtent.y));
    float2 displayExtentF = float2(float(gPushConstants.displayExtent.x),
                                   float(gPushConstants.displayExtent.y));

    // Re-form the exact display pixel center so displayExtent is part of the
    // binding contract and the 1:1, zero-jitter case maps pixel-for-pixel.
    float2 displayPixel = floor(pin.displayUV * displayExtentF);
    float2 displayUV = (displayPixel + 0.5f) / displayExtentF;
    float2 inputUV = displayUV - gPushConstants.jitterPixels / renderExtentF;

    int2 sourceCenter = int2(floor(inputUV * renderExtentF));
    int2 sourceMax = int2(int(gPushConstants.renderExtent.x) - 1,
                          int(gPushConstants.renderExtent.y) - 1);
    float resolvedDepth = 1.0f;

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 tap = clamp(sourceCenter + int2(x, y), int2(0, 0),
                             sourceMax);
            float2 tapUV = (float2(float(tap.x), float(tap.y)) + 0.5f)
                         / renderExtentF;
            float tapDepth = renderDepth.SampleLevel(depthSampler, tapUV, 0.0f);
            resolvedDepth = min(resolvedDepth, tapDepth);
        }
    }

    return resolvedDepth;
}
