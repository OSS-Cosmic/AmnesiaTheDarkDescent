#pragma once

// Slang-side ILight interface over the host/device-shared light structs declared
// in SceneTypes.slang. Conformances are added via `extension` so the shared
// header stays a pure data-layout file (consumed by both C++ and shader).
//
// Light type is determined by which SSBO an entry lives in (point/spot/area) and
// the per-type count boundaries in gPerFrame — see Scene.slang getLight() — so
// the structs carry no explicit type discriminator.
//
// Usage example:
//   void shadeAny<L : ILight>(L light, float3 worldPos) {
//       LightSample ls;
//       if (light.sample(worldPos, surfaceFootprintW, ls)) { ... }
//   }

#include "HostDefinitions.h"
import SceneTypes;            // light struct fields + kInvalidTextureIndex sentinel
import bindless;             // gTextures2D, gTexturesCube, gMaterialSampler
import Random;               // concentricDiskSample (soft-shadow source sampling)
import RayCone;              // projected-light texture footprint helpers

// -----------------------------------------------------------------------------
// ILight — common shape for the point/spot/box bindless light SSBO entries.
// `getPosition()` returns the source for point/spot, the AABB
// center for box. `sample()` is the NEE entry point used by the
// direct-lighting and path-trace shading paths. `surfaceFootprintW` is the
// world-space footprint DIAMETER at the shading point. Lights with projected
// textures need it because their UVs are not the surface triangle's UV
// parametrisation. PointLight uses it for the angular footprint of its cube gobo.
// -----------------------------------------------------------------------------
public interface ILight
{
    public float3 getColor();
    public float3 getPosition();
    public float getRadius();         // light-grid bin reach (world units); 0 => skip
    public bool sample(float3 shadingPosW, float surfaceFootprintW, out LightSample ls);

    // Whether this light casts shadows (GetCastShadows). When false the NEE
    // shadow ray is skipped — the light fully lights its targets, no occlusion.
    public bool castsShadow();

    // Jittered point on the emitter for soft-shadow rays. `dirToLight` is the unit
    // direction from the shading point to the light (used to orient a perpendicular
    // disk for spherical point/spot sources; area lights ignore it and sample their
    // own plane). `shadingPosW` is the point being shaded — area lights need it to
    // re-derive the barn-door clipped source region, spherical sources ignore it.
    // `u` is a uniform float2 in [0,1). Returns the light center when the source is
    // degenerate (point/spot with sourceRadius 0) => hard shadow.
    public float3 sampleSourcePoint(float3 shadingPosW, float3 dirToLight, float2 u);
};

// Reach window: UE-style (1 - (d/r)^4)^2, reaching EXACTLY zero at `radius` and
// C1 there, so nothing pops when a light leaves a grid cell.
//
// This is what makes `radius` (the light-grid bin reach) a real quantity rather
// than a bookkeeping number. Shading and LightGridBuildPass now cut off at the
// same distance, which is the precondition for LightParameters.cpp clamping the
// derived reach to kLightReachMaxScale x the authored radius. Before the window,
// point/spot shading was a bare 1/d^2 with no cutoff at all, so the reach had to
// be enormous (~8x the authored radius) to keep the grid boundary invisible --
// and a single lamp then landed in a quarter of the grid's cells.
//
// radius <= 0 means "no reach authored"; the window is then a no-op, matching
// the old unwindowed behaviour.
public float lightReachWindow(float dist, float radius)
{
    if (radius <= 0.0) return 1.0;
    float x  = dist / radius;
    float x4 = (x * x) * (x * x);
    float w  = saturate(1.0 - x4);
    return w * w;
}

// Jittered point on a spherical source of `radius` at `center`, on a disk facing
// the shading point (perpendicular to `dirToLight`). radius <= 0 => the center
// (hard shadow). Shared by point and spot.
float3 diskSourcePoint(float3 center, float radius, float3 dirToLight, float2 u)
{
    if (radius <= 0.0) return center;
    float3 t = normalize(abs(dirToLight.y) < 0.99f
                         ? cross(float3(0.f, 1.f, 0.f), dirToLight)
                         : cross(float3(1.f, 0.f, 0.f), dirToLight));
    float3 b = cross(dirToLight, t);
    float2 d = concentricDiskSample(u) * radius;
    return center + d.x * t + d.y * b;
}

// Barn-door clipped view of the emitter rectangle, in rect-local tangential
// coordinates (x along `right`, y along `up`). `valid` is false when the flaps
// hide the source completely from the shading point.
struct BarnRect
{
    float2 lo;
    float2 hi;
    bool   valid;
};

// One tangential axis of the barn-door clip. The flap hinges on the rect edge at
// +halfExt, tilts from the emission normal by θ (sinA/cosA) and runs L world units,
// so its tip sits at (halfExt + L·sinθ, L·cosθ) in (lateral, depth). From a shading
// point at (coord, lz) the tip occludes the far part of the emitter: the ray through
// the tip, extended back to the emitter plane, is the clip boundary. Returns the
// visible (lo, hi) span on that axis; lo >= hi means nothing is visible.
float2 barnClipAxis(float coord, float halfExt, float lz, float sinA, float cosA, float L)
{
    float h   = L * cosA;              // flap tip depth
    float tip = halfExt + L * sinA;    // flap tip lateral offset

    if (lz > h)
    {
        float inv = 1.0 / (lz - h);
        float hi = min( halfExt, ( tip * lz - coord * h) * inv);
        float lo = max(-halfExt, (-tip * lz - coord * h) * inv);
        return float2(lo, hi);
    }

    // Level with the flaps (between the emitter plane and the tip depth): the point
    // is either inside the channel and sees the whole emitter, or outside a flap,
    // which blocks that side entirely.
    float channel = halfExt + lz * (sinA / cosA);
    if (abs(coord) > channel) return float2(1.0, -1.0);   // empty
    return float2(-halfExt, halfExt);
}

// Both axes of the clip. `dLocal` is the shading point in the rect frame
// (x = right, y = up, z = emission normal, z > 0). L = 0 or θ → 90° (cosA → 0)
// means the flaps are open, so the full rect stays visible.
BarnRect barnClipRect(float3 dLocal, float hw, float hh, float barnDoorAngle, float barnDoorLength)
{
    BarnRect br;
    br.lo    = float2(-hw, -hh);
    br.hi    = float2( hw,  hh);
    br.valid = true;

    float sinA, cosA;
    sincos(barnDoorAngle, sinA, cosA);
    if (barnDoorLength <= 0.0 || cosA <= 1.0e-4) return br;   // open / disabled

    float2 cx = barnClipAxis(dLocal.x, hw, dLocal.z, sinA, cosA, barnDoorLength);
    float2 cy = barnClipAxis(dLocal.y, hh, dLocal.z, sinA, cosA, barnDoorLength);
    br.lo    = float2(cx.x, cy.x);
    br.hi    = float2(cx.y, cy.y);
    br.valid = (br.hi.x > br.lo.x) && (br.hi.y > br.lo.y);
    return br;
}

public extension PointLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    public float3 sampleSourcePoint(float3 shadingPosW, float3 dirToLight, float2 u)
    {
        return diskSourcePoint(position, sourceRadius, dirToLight, u);
    }

    // Radiance = color · intensity · windowed softened inverse-square. Still HDR
    // (>1) near the source so lights cross the bloom white point, but now cut off
    // at exactly `radius` — the same distance LightGridBuildPass bins to, so
    // shading and culling agree (see lightReachWindow).
    public bool sample(float3 shadingPosW, float surfaceFootprintW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;
        float3 toLight = position - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L      = toLight / dist;
        float  d2     = dist * dist;
        float  window = lightReachWindow(dist, radius);
        if (window <= 0.0) return false;                                   // past the reach — nothing to shade or shadow
        float  atten  = window / (d2 + kPointLightSourceRadiusSq);         // softened inverse-square (HDR near source)

        float3 gobo = float3(1.0);
        if (goboTextureIndex != kInvalidTextureIndex)
        {
            float3x3 wToL = float3x3(worldToLightX, worldToLightY, worldToLightZ);
            float3 lightLocalDir = mul(wToL, -L);
            // The surface triangle's cone LOD base describes its own UVs, not
            // this cube direction; apply the angular cube-map footprint.
            float lodBase = cubeGoboTexLodBase(surfaceFootprintW, dist, lightLocalDir);
            gobo = sampleBindlessCubeCone(goboTextureIndex, lightLocalDir, lodBase).rgb;
        }

        ls.Li       = color * intensity * gobo * atten;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};

public extension SpotLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    public float3 sampleSourcePoint(float3 shadingPosW, float3 dirToLight, float2 u)
    {
        return diskSourcePoint(position, sourceRadius, dirToLight, u);
    }

    // Radiance = color · intensity · windowed softened inverse-square (same radial
    // term as PointLight, cut off at exactly `radius` to match the grid binning —
    // see lightReachWindow), times an angular cone term (analytic smoothstep from
    // the FOV edge to the center, or a projected gobo via viewProjection when bound).
    public bool sample(float3 shadingPosW, float surfaceFootprintW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;
        float3 toLight = position - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L      = toLight / dist;
        float  d2     = dist * dist;
        float  window = lightReachWindow(dist, radius);
        if (window <= 0.0) return false;                                   // past the reach — nothing to shade or shadow
        float  atten  = window / (d2 + kPointLightSourceRadiusSq);         // softened inverse-square (HDR near source)

        float3 gobo = float3(1.0);
        float  cone = 1.0;
        if (goboTextureIndex != kInvalidTextureIndex)
        {
            float4 lc = mul(viewProjection, float4(shadingPosW, 1.0));
            if (lc.w <= 0.0) return false;
            float2 uv = lc.xy / lc.w;
            if (any(uv < float2(0.0)) || any(uv > float2(1.0))) return false;
            // The surface triangle's cone LOD base describes its own UVs, not
            // these projected light coordinates; apply the projective footprint.
            float lodBase = projectedTexLodBase(viewProjection, lc, surfaceFootprintW);
            gobo = sampleBindless2DCone(goboTextureIndex, uv, lodBase).rgb;
        }
        else
        {
            float cosTheta = dot(-L, direction);
            if (cosTheta < cosOuterAngle) return false;
            cone = smoothstep(cosOuterAngle, 1.0, cosTheta);
        }

        ls.Li       = color * intensity * gobo * cone * atten;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};

public extension RectLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    // Rect-local frame of a shading point: (x along right, y along up, z along the
    // emission normal). Shared by sample() and sampleSourcePoint() so both see the
    // same barn-door clip.
    public float3 toLightLocal(float3 shadingPosW)
    {
        float3 d = shadingPosW - position;
        return float3(dot(d, right), dot(d, up), dot(d, normal));
    }

    // Soft shadows sample the actual rectangle: a uniform point across the visible
    // (barn-door clipped) part of the width×height plane. The area itself IS the
    // source, so there is no disk radius — a larger source rectangle casts a softer
    // penumbra — and clipping here is what keeps the penumbra inside the shaped beam.
    public float3 sampleSourcePoint(float3 shadingPosW, float3 dirToLight, float2 u)
    {
        float3 l = toLightLocal(shadingPosW);
        if (l.z <= 0.0) return position;
        BarnRect br = barnClipRect(l, width * 0.5, height * 0.5, barnDoorAngle, barnDoorLength);
        if (!br.valid) return position;                    // fully occluded: aim at the centre
        float2 p = lerp(br.lo, br.hi, u);
        return position + p.x * right + p.y * up;
    }

    // Analytic closest-point-on-rectangle shading (UE Rect Light model). The
    // emitter is a width×height rect at `position`, framed by (right, up, normal).
    // We shade the closest point on the rect as a softened inverse-square source,
    // windowed to 0 at the attenuation `radius`. Intensity is the TOTAL energy
    // averaged over the surface (radiance = intensity / area), so a bigger rect is
    // dimmer per unit area. An optional `sourceTextureIndex` tints the emission
    // (rect-local UV; does NOT affect shadows — those sample the area in
    // sampleSourcePoint). One-sided: only the +normal hemisphere is lit. pdf = 1.
    // Barn doors (barnDoorAngle θ, barnDoorLength L) confine the beam: four flaps hinged on the
    // rect edges, tilted from the normal by θ and L units long, occlude part of the emitter as
    // seen from the shading point. barnClipRect turns that into a clipped source rectangle, which
    // both moves the closest point and scales the contribution by the visible area fraction, so
    // the beam narrows AND dims at its edge. θ → 90° or L = 0 ⇒ flaps open ⇒ no-op.
    public bool sample(float3 shadingPosW, float surfaceFootprintW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;

        // Shading point in the rect's local frame.
        float3 l  = toLightLocal(shadingPosW);
        float  lx = l.x;
        float  ly = l.y;
        float  lz = l.z;
        if (lz <= 0.0) return false;                       // behind the emitter

        float hw = width  * 0.5;
        float hh = height * 0.5;

        // Barn-door beam shaping: clip the emitter to the part this point can still
        // see past the flaps. Open/disabled flaps leave the full rect.
        BarnRect br = barnClipRect(l, hw, hh, barnDoorAngle, barnDoorLength);
        if (!br.valid) return false;                       // flaps hide the source

        // Closest point on the VISIBLE rect: clamp to the clipped span.
        float  cx = clamp(lx, br.lo.x, br.hi.x);
        float  cy = clamp(ly, br.lo.y, br.hi.y);
        float3 Q  = position + right * cx + up * cy;

        float3 toLight = Q - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L     = toLight / dist;
        float  d2    = dist * dist;

        // Softened inverse-square, windowed to 0 at the attenuation radius.
        // Point and spot now share this window (lightReachWindow) — this light
        // had it first, which is why the shape is the house style.
        float window = lightReachWindow(dist, radius);
        if (window <= 0.0) return false;                   // past the reach
        float atten = window / (d2 + kPointLightSourceRadiusSq);

        // Intensity is averaged over the emitter surface (UE semantics).
        float area = max(width * height, 1.0e-4);

        float3 srcTex = float3(1.0);
        if (sourceTextureIndex != kInvalidTextureIndex)
        {
            float2 uv = float2(cx / width + 0.5, 0.5 - cy / height);
            // The surface triangle's cone LOD base describes its own UVs, not
            // this rect-local emitter UV; average the closest-point source
            // texture over the competing surface/emitter footprint.
            float lodBase = areaLightSourceTexLodBase(surfaceFootprintW, dist,
                                                      width, height);
            srcTex = sampleBindless2DCone(sourceTextureIndex, uv, lodBase).rgb;
        }

        // Visible fraction of the emitter after clipping. The closest-point model has
        // no notion of a partly hidden source, so scale the contribution by the area
        // that survives the flaps — this is what dims the beam edge.
        float coverage = saturate(((br.hi.x - br.lo.x) * (br.hi.y - br.lo.y)) / area);

        ls.Li       = color * (intensity / area) * srcTex * atten * coverage;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};
