#pragma once

// Tangent-space parallax occlusion mapping (relief mapping) shared helper.
//
// Ported from the legacy relief search in
//   build/amnesia/core/shaders/deferred_gbuffer_solid_frag.glsl
// (linear search to the first intersection, then a binary refine). The legacy
// height map stores depth-below-the-top-surface; the search marches the eye
// vector through the [0,1] height volume and stops where the ray crosses the
// stored height field.
//
// The result is a warped UV. Callers decide how to apply it — the POM
// barycentric pass converts the UV delta into a barycentric perturbation so
// the whole pipeline reconstructs the occluded point
// (see VBuffer/VBufferPomBary.cs.slang).
//
// Derivative note: the callers are non-pixel stages (compute) with no
// automatic texture derivatives, so every height sample uses an explicit,
// texture-size-independent lodBase. The POM march intentionally samples the
// height map at ONE fixed lod for all steps of a march, so the march stays
// self-consistent rather than changing mip mid-march.

import SceneTypes;          // kInvalidTextureIndex (shared consts live here)
import bindless;            // gTextures2D, gMaterialSampler

// Sample the height field. Legacy convention: depth is in the alpha channel,
// except dedicated grayscale height maps (kMaterialFlagIsHeightMapSingleChannel)
// carry it in .r.
public float sampleHeight(uint heightTexIdx, float2 uv, bool singleChannel,
                          float texLodBase)
{
    float4 s = sampleBindless2DCone(heightTexIdx, uv, texLodBase);
    return singleChannel ? s.r : s.a;
}

// Returns the parallax-corrected UV. `worldViewDir` is the camera->surface ray
// direction (== payload.direction); N/T/B are the orthonormal world-space TBN.
// Returns `baseUv` unchanged when there is no height map or the view is too
// grazing to march safely.
public float2 parallaxOcclusionUV(
    uint   heightTexIdx,
    float  heightMapScale,
    bool   singleChannel,
    float3 worldViewDir,
    float3 N, float3 T, float3 B,
    float2 baseUv,
    float texLodBase)
{
    if (heightTexIdx == kInvalidTextureIndex)
        return baseUv;
    // Legacy GetNormalizedDepth yields 0 for a collapsed or inverted depth
    // range, so a zero or negative HeightMapScale never parallaxes (Standard
    // skips it the same way in standardParallaxUV). Running it reversed slides
    // the shading point off the rasterized sample, which the velocity buffer
    // never sees, and smears the denoised lighting. NaN also bails here.
    if (!(heightMapScale > 0.0001f))
        return baseUv;

    // Tangent-space eye vector, matching legacy gvTangentEyePos
    // (deferred_base_vtx.glsl): the lateral (xy) march follows the
    // camera->surface ray so the UV shifts the correct way under view motion,
    // while z is the surface->eye direction (positive on front faces) so the
    // height volume is spanned top->bottom. Building all three from -worldViewDir
    // negates xy and flips the parallax.
    float3 eye = float3(dot(worldViewDir, T),
                        dot(worldViewDir, B),
                        dot(-worldViewDir, N));

    // Grazing / back-facing / NaN guard. Written as !(eye.z > k) so a NaN eye.z
    // (degenerate normal -> normalize() = NaN) also bails: with a plain
    // `eye.z <= k` test NaN compares false, slips through, and poisons the step
    // count below — and an out-of-range float->uint loop bound spins forever and
    // hangs the GPU (RADV_DEBUG=hang).
    if (!(eye.z > 1.0e-3f))
        return baseUv;

    // Give the eye vector unit length along the surface normal so it spans the
    // full [0,1] height volume, then scale the lateral march by the material's
    // height scale.
    eye *= (1.0f / eye.z);
    eye.xy *= heightMapScale;

    // More steps at grazing angles (tangent-space surface normal is (0,0,1), so
    // the legacy dot(eye, N) collapses to eye.z / length(eye)). Cast to int and
    // integer-clamp BEFORE the loop: a NaN/Inf length (degenerate tangent) must
    // never reach the loop bound — float->int of a non-finite value is undefined
    // but finite, and the integer clamp then pins it into [2,20]; a float bound
    // could stay NaN and hang.
    int nSteps = (int)floor((1.0f - eye.z / max(length(eye), 1.0e-6f)) * 18.0f) + 2;
    nSteps = clamp(nSteps, 2, 20);
    float fSteps = (float)nSteps;

    float3 pos  = float3(baseUv, 0.0f);
    float3 step = eye / fSteps;

    // Linear search to the first intersection.
    [loop]
    for (int i = 0; i < nSteps - 1; ++i)
    {
        float h = sampleHeight(heightTexIdx, pos.xy, singleChannel, texLodBase);
        if (pos.z < h)
            pos += step;
    }

    // Binary refine around the crossing.
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        float h = sampleHeight(heightTexIdx, pos.xy, singleChannel, texLodBase);
        if (pos.z < h)
            pos += step;
        step *= 0.5f;
        pos  -= step;
    }

    return pos.xy;
}
