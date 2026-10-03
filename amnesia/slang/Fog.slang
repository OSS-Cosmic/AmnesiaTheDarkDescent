/// Screen-space fog helper shared by the main composite pass and the
/// translucent / particle / water fragment shaders. Replaces the legacy per-object fog-area
/// visibility (RendererDeferred.cpp:365-478) with a per-pixel iteration
/// that walks the gFogAreas SSBO and accumulates both a visibility scalar
/// and an additive fog colour.
///
/// Returned visibility is multiplicative: 1 = no fog, 0 = fully fogged.
/// Opaque consumers do `finalColor = visibility * sceneColor + fogColor`.
/// Translucent consumers multiply visibility into their per-pixel alpha so
/// translucent surfaces fade behind fog without painting fog colour.
///
/// World fog (gPerFrame.worldFogStart / worldFogLength / fogFalloffExp /
/// worldFogColor) is folded into the same accumulator so both kinds of fog
/// composite consistently.

#include "Constants.h"
import SceneTypes;
import FogBindings;
import PerFrame.resource;
import Utils.Color.ColorHelpers;  // linearToSRGB / sRGBToLinear — display-space fog blend

struct FogResult {
    float  visibility;  // 1 = no fog, 0 = fully fogged
    float3 color;       // accumulated fog colour, premultiplied by its contribution weight
};

// Accumulates every fog area crossed by the segment originW + rayDir * [0, segLen]
// into `visibility` / `color` (over-composite). `viewDepthScale` converts the
// segment's world distance onto the view-Z metric the authored Start/End use.
// Shared by computeFog (eye -> surface) and computeFogSegment (any path segment).
void accumulateFogAreas(float3 originW, float3 rayDir, float segLen, float viewDepthScale,
                        inout float visibility, inout float3 color)
{
    uint count = gPerFrame.fogAreaCount;
    for (uint i = 0u; i < count; ++i) {
        FogAreaParams fa = gFogAreas[gPerFrame.fogAreaIndices[i >> 2u][i & 3u]];

        // Move the ray into the box's unit-cube local space. Point uses w=1
        // (translation applied), direction uses w=0 (rotation+scale only).
        float3 boxCamOrigin = mul(fa.invModelMat, float4(originW, 1.0f)).xyz;
        float3 boxRayDir    = mul(fa.invModelMat, float4(rayDir,     0.0f)).xyz;

        // Slab method against [-0.5, 0.5]^3. Guard divide-by-zero on
        // axis-aligned rays by replacing tiny denominators with a large
        // value — the slab will then trivially pass on that axis.
        float3 invDir = float3(
            abs(boxRayDir.x) > 1.0e-6f ? 1.0f / boxRayDir.x : 1.0e30f,
            abs(boxRayDir.y) > 1.0e-6f ? 1.0f / boxRayDir.y : 1.0e30f,
            abs(boxRayDir.z) > 1.0e-6f ? 1.0f / boxRayDir.z : 1.0e30f);
        float3 t1 = (float3(-0.5f) - boxCamOrigin) * invDir;
        float3 t2 = (float3( 0.5f) - boxCamOrigin) * invDir;
        float3 tmin = min(t1, t2);
        float3 tmax = max(t1, t2);
        float  tEntry = max(max(tmin.x, tmin.y), tmin.z);
        float  tExit  = min(min(tmax.x, tmax.y), tmax.z);

        // No intersection with the slab volume.
        if (tExit < 0.0f || tExit < tEntry) continue;

        bool originInside = all(abs(boxCamOrigin) <= float3(0.5f));

        // Outside-box but ray ends before reaching the box -> contributes
        // nothing (the surface is in front of the fog volume).
        if (!originInside && segLen < tEntry) continue;

        bool showBackInside  = (fa.flags & 1u) != 0u;
        bool showBackOutside = (fa.flags & 2u) != 0u;

        // Distance the eye→surface ray spends inside the box. Mirrors the
        // legacy GetFogAreaVisibilityForObject branch table.
        float fogDist;
        if (originInside) {
            fogDist = showBackInside ? min(tExit, segLen) : segLen;
        } else {
            fogDist = showBackOutside
                          ? min(tExit - tEntry, segLen - tEntry)
                          : (segLen - tEntry);
        }

        fogDist *= viewDepthScale;
        if (fogDist <= 0.0f || fogDist < fa.start) continue;

        float alpha;
        if (fogDist > fa.end) {
            alpha = 1.0f;
        } else {
            alpha = saturate((fogDist - fa.start) / max(fa.end - fa.start, 1.0e-6f));
            if (fa.falloffExp != 1.0f) alpha = pow(alpha, fa.falloffExp);
        }

        float contribution = saturate(alpha * fa.colorA);
        visibility *= (1.0f - contribution);
        // Over-composite fog layers, just as the original alpha-blended
        // passes did. Unattenuated addition made overlapping volumes brighter
        // than either fog color and let bright fog leak through dark fog.
        color = color * (1.0f - contribution) + contribution * fa.color;
    }
}

FogResult computeFog(float3 surfacePosW, float3 cameraPosW)
{
    FogResult r;
    r.visibility = 1.0f;
    r.color      = float3(0.0f);

    // Category (b): `ray` is formed from the caller's world-space surface and
    // camera positions. In the opaque composite the surface comes from the
    // V-buffer, so this is not a pixel-derived camera ray; rayDir is jitter-
    // independent by construction and must not receive a screen-space term.
    float3 ray = surfacePosW - cameraPosW;
    float cameraDist = length(ray);
    if (cameraDist <= 0.0f) return r;
    float3 rayDir = ray / cameraDist;
    // Fog Start/End were authored against view-space Z in the original deferred
    // fog pass: deferred_fog_fullscreen.frag.fsl:15 reads -position.z straight out
    // of a view-space G-buffer, and the fog-area box pass does the same. Radial
    // distance overstates density off-axis — at the 70 deg vertical FOV default
    // (Camera.cpp:42) it exceeds view Z by 1.60x at the horizontal frame edge and
    // 1.74x at the corners, so fog arrived far too early away from screen centre.
    // Projecting the unit ray onto the view axis recovers the reference's metric.
    float viewDepthScale = abs(mul(gPerFrame.viewMat, float4(rayDir, 0.0f)).z);
    float viewDist       = cameraDist * viewDepthScale;

    // World fog — view-space depth through the worldFogStart / worldFogLength
    // band, raised to fogFalloffExp. Mirrors deferred_fog_fullscreen.frag.fsl:15-18
    // and the translucency.frag.fsl world-fog block, both of which ramp on -viewZ.
    if (gPerFrame.worldFogLength > 0.0f) {
        float fogAmount = pow(
            saturate((viewDist - gPerFrame.worldFogStart) / gPerFrame.worldFogLength),
            gPerFrame.fogFalloffExp);
        // gPerFrame.oneMinusFogAlpha = (1 - worldFogColor.a) — recover .a
        float worldFogAlpha = 1.0f - gPerFrame.oneMinusFogAlpha;
        float contribution  = fogAmount * worldFogAlpha;
        r.visibility *= (1.0f - contribution);
        r.color      += contribution * gPerFrame.worldFogColor.rgb;
    }

    // Per-fog-area iteration. The work per pixel is bounded by
    // gPerFrame.fogAreaCount, which is typically a handful (capped at
    // kFogAreaCapacity host-side).
    accumulateFogAreas(cameraPosW, rayDir, cameraDist, viewDepthScale, r.visibility, r.color);

    // Premultiplied authored linear fog color. Consumers that add it directly
    // to scene radiance must divide by kSceneExposure.
    return r;
}

// Fog along an arbitrary path segment a -> b, for the path tracer's secondary
// rays. Light travelling b -> a is `L * visibility + color`; a caller walking a
// path from the camera does `radiance += thp * color / kSceneExposure;
// thp *= visibility;` before shading the hit at b.
//
// World fog is camera-distance based, not a medium, so a segment takes the
// fraction of the remaining visibility the fog band removes between the two
// endpoints' view depths (identical to computeFog when a is the camera).
FogResult computeFogSegment(float3 a, float3 b)
{
    FogResult r;
    r.visibility = 1.0f;
    r.color      = float3(0.0f);
    if (gPerFrame.fogEnabled == 0u && gPerFrame.fogAreaCount == 0u) return r;

    float3 ray = b - a;
    float segLen = length(ray);
    if (segLen <= 0.0f) return r;
    float3 rayDir = ray / segLen;

    if (gPerFrame.worldFogLength > 0.0f) {
        float worldFogAlpha = 1.0f - gPerFrame.oneMinusFogAlpha;
        float zA = abs(mul(gPerFrame.viewMat, float4(a, 1.0f)).z);
        float zB = abs(mul(gPerFrame.viewMat, float4(b, 1.0f)).z);
        float cA = pow(saturate((zA - gPerFrame.worldFogStart) / gPerFrame.worldFogLength),
                       gPerFrame.fogFalloffExp) * worldFogAlpha;
        float cB = pow(saturate((zB - gPerFrame.worldFogStart) / gPerFrame.worldFogLength),
                       gPerFrame.fogFalloffExp) * worldFogAlpha;
        // Fraction of the light still unfogged at a that is fogged by b.
        float seg = saturate((cB - cA) / max(1.0f - cA, 1.0e-6f));
        r.visibility *= (1.0f - seg);
        r.color      += seg * gPerFrame.worldFogColor.rgb;
    }

    float viewDepthScale = abs(mul(gPerFrame.viewMat, float4(rayDir, 0.0f)).z);
    accumulateFogAreas(a, rayDir, segLen, viewDepthScale, r.visibility, r.color);
    return r;
}

// Blend fog in display space to preserve authored appearance, then return to
// linear HDR before storing the scene or drawing any translucent geometry.
float3 applyFog(float3 sceneColor, float3 surfacePosW, float3 cameraPosW)
{
    FogResult fog = computeFog(surfacePosW, cameraPosW);
    float coverage = 1.0f - fog.visibility;
    if (coverage <= 0.0f)
        return max(sceneColor, 0.0f);
    float3 sceneDisplay = linearToSRGB(max(sceneColor, 0.0f) * kSceneExposure);
    float3 fogColor = fog.color / coverage;
    float3 result = lerp(sceneDisplay, linearToSRGB(fogColor), coverage);
    return sRGBToLinear(result) / kSceneExposure;
}
