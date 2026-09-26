// LightGridBuildPass — builds the coarse, camera-centered world-space light
// grid used by the path tracer's NEE, the direct-lighting passes and the
// MainCompositePass direct cull.
//
//   binLights : ONE THREAD PER CELL. Each cell walks every light type over its
//               stable SLOT CAPACITY (gPerFrame.{point,spot,area}LightCount are
//               host high-water slot counts, sparse — zeroed holes have radius 0
//               and are skipped) and keeps the TOP-kLightsPerCellMax lights by
//               estimated maximum contribution within the cell, writing both the
//               packed light id (packLightId(type, slot) — the same value
//               getLight() decodes, so a reservoir's persisted id stays valid
//               across frames) and a sampling CDF. The cell owns its count +
//               list slots, so there are no atomics, no inter-thread contention,
//               and the per-cell count is written unconditionally (no host
//               zero-clear).
//
// Per-cell gather (vs the old per-light scatter): work is uniform across threads
// — a single large light can't stall a whole wave. The light reach is precomputed
// on the host (pl/sl.radius), and the sphere/cell test compares squared distance,
// so the inner loop has no sqrt. Point lights are omnidirectional so the reach
// sphere is exact; spotlights additionally get an angular cull matching
// SpotLight.sample: a rectangular projection for gobos, or a circular cone for
// analytic spots. Binning stays conservative, including wide gobos and
// projection corners.
//
// TOP-K, NOT FIRST-K. The cap used to drop whatever came last in index order,
// which is arbitrary with respect to how much a light actually matters here.
// Now each candidate gets a cheap contribution estimate over the cell and
// the list keeps the best K, sorted descending, with a running CDF in
// gLightGridWeight. That buys two things:
//   * the one-sample NEE in PathTraceCommon can importance-sample the cell
//     (pdf = w_i / wTotal) instead of picking uniformly (pdf = 1/count). With a
//     uniform pick the odds of drawing the light that matters are 1/count, and
//     every miss still pays a full BVH shadow-ray traversal — which is how light
//     density turned into frame time without adding a single ray;
//   * K can be small (8 instead of 128), so the enumerating walkers
//     (buildDirectReservoir, the probe) get shorter lists and the two grid
//     buffers drop from 16 MB to 1 MB each.
// The dropped tail is by construction the dimmest lights at this cell, so K is a
// quality knob rather than a correctness cliff.
//
// Cell positions are relative to gPerFrame.lightGridOriginW (the camera snapped
// to a whole cell), NOT raw posW — see the SceneConstants comment. getCellLights
// must use the same origin or it reads a cell this pass filled for another one.
//
// Dispatched each frame in cHybridRenderer::Draw just before the G-buffer
// raster pass, followed by a storage-write barrier into ray-tracing / compute /
// fragment — i.e. it runs ahead of every consumer of the grid (direct lighting,
// path trace, composite).

#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;                   // packLightId / kLightType*
import ILight;                  // lightReachWindow (same window ILight.sample shades with)
import PerFrame.resource;
import Grid;                    // gridUnflattenCellIndex / sphereIntersectsGridCell

// Bounded top-K list, kept sorted descending by weight. K is small and the
// arrays are indexed only by unrolled loop counters, so this stays in registers.
struct TopK
{
    float w[kLightsPerCellMax];
    uint  id[kLightsPerCellMax];
    uint  count;

    [mutating]
    void init() { count = 0u; [unroll] for (uint i = 0u; i < kLightsPerCellMax; ++i) { w[i] = 0.f; id[i] = 0u; } }

    // Insertion sort against the current tail. A candidate that can't beat the
    // weakest kept light when the list is full is rejected without touching it.
    // Fully unrolled, so every array index is a compile-time constant and the
    // whole thing stays in registers rather than spilling to scratch.
    [mutating]
    void insert(uint lightId, float weight)
    {
        if (weight <= 0.f) return;
        if (count == kLightsPerCellMax && weight <= w[kLightsPerCellMax - 1u]) return;

        uint last = min(count, kLightsPerCellMax - 1u);   // slot the new entry may land in
        bool placed = false;
        [unroll]
        for (uint i = kLightsPerCellMax - 1u; i > 0u; --i)
        {
            if (i > last || placed) continue;   // past the live tail, or already seated
            if (w[i - 1u] < weight) { w[i] = w[i - 1u]; id[i] = id[i - 1u]; }   // shift down
            else                    { w[i] = weight;    id[i] = lightId; placed = true; }
        }
        if (!placed) { w[0] = weight; id[0] = lightId; }   // new maximum
        count = min(count + 1u, kLightsPerCellMax);
    }
};

// Cheap estimate of how much this light contributes anywhere in the cell, used
// only to rank and to build the sampling pdf — it never scales radiance, so it
// can be crude. Mirrors the shape of ILight.sample (windowed softened
// inverse-square on the brightest channel) at the nearest point of the cell.
// The source-radius term already prevents singularities. Using the centre or
// flooring distance to the cell radius can zero lights that illuminate part
// of the cell, even when there is room for them in the list.
float cellLightWeight(float3 lightPos, float3 color, float intensity,
                      float radius, float3 cellCenter)
{
    float3 delta = max(abs(lightPos - cellCenter) - kLightGridUnit * 0.5f,
                       float3(0.0f));
    float d    = length(delta);
    float maxC = max(color.x, max(color.y, color.z));
    return maxC * intensity * lightReachWindow(d, radius)
         / (d * d + kPointLightSourceRadiusSq);
}

[numthreads(64, 1, 1)]
void binLights(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    // This coarse dispatch names world-space light-grid cells, not screen
    // pixels. It never indexes gPackedHitInfo, reconstructs a camera ray, or
    // produces a UV for NRD, so no primary-raster jitter or own-grid offset
    // applies here.
    uint flat = dispatchThreadId.x;
    if (flat >= kLightGridCellCount)
        return;

    int3   cellPos = gridUnflattenCellIndex(flat, kLightGridDim);
    // Snapped origin, shared with getCellLights — never raw gPerFrame.posW.
    float3 gridOrigin = gPerFrame.lightGridOriginW;
    float3 cellCenter = cellPos * kLightGridUnit + gridOrigin;
    uint   nPoint  = gPerFrame.pointLightCount;   // per-type slot capacities (sparse, zeroed holes)
    uint   nSpot   = gPerFrame.spotLightCount;
    uint   nArea   = gPerFrame.areaLightCount;

    TopK top;
    top.init();

    // Point lights — omnidirectional, reach sphere is exact.
    for (uint p = 0u; p < nPoint; ++p)
    {
        PointLight pl = gPointLights[p];
        if (pl.radius <= 0.f)   // 0 ⇒ invisible / dim-to-skip / unused slot hole
            continue;
        if (!sphereIntersectsGridCell(pl.position, pl.radius, cellPos, gridOrigin, kLightGridDim, kLightGridUnit))
            continue;
        top.insert(packLightId(kLightTypePoint, p),
                   cellLightWeight(pl.position, pl.color, pl.intensity, pl.radius, cellCenter));
    }

    // Spot lights — reach sphere + the shading path's angular footprint.
    for (uint s = 0u; s < nSpot; ++s)
    {
        SpotLight sl = gSpotLights[s];
        if (sl.radius <= 0.f)
            continue;
        if (!sphereIntersectsGridCell(sl.position, sl.radius, cellPos, gridOrigin, kLightGridDim, kLightGridUnit))
            continue;
        if (sl.goboTextureIndex != kInvalidTextureIndex)
        {
            if (!projectionIntersectsGridCell(sl.viewProjection, cellPos, gridOrigin,
                                              kLightGridDim, kLightGridUnit))
                continue;
        }
        else if (!coneIntersectsGridCell(sl.position, sl.direction, sl.cosOuterAngle,
                                         cellPos, gridOrigin, kLightGridDim, kLightGridUnit))
        {
            continue;
        }
        // The conservative footprint test above owns angular rejection. A
        // centre-only cone weight would drop partially illuminated cells.
        top.insert(packLightId(kLightTypeSpot, s),
                   cellLightWeight(sl.position, sl.color, sl.intensity, sl.radius, cellCenter));
    }

    // Area lights — planar emitters; the reach sphere is a conservative bound
    // (the exact rect + barn-door cull stays in RectLight.sample).
    for (uint a = 0u; a < nArea; ++a)
    {
        RectLight al = gAreaLights[a];
        if (al.radius <= 0.f)
            continue;
        if (!sphereIntersectsGridCell(al.position, al.radius, cellPos, gridOrigin, kLightGridDim, kLightGridUnit))
            continue;
        // RectLight.sample divides intensity by the emitter area (UE semantics),
        // so rank on the same quantity or a large panel outranks everything.
        float area = max(al.width * al.height, 1.0e-4f);
        top.insert(packLightId(kLightTypeArea, a),
                   cellLightWeight(al.position, al.color, al.intensity / area, al.radius, cellCenter));
    }

    // Publish the list plus its running CDF. Entry k holds the sum over 0..k, so
    // the last valid entry is the total and a pick is a scan for the first entry
    // >= u * total. Writing the CDF rather than raw weights keeps the sampler
    // branch-free and saves it a second pass to find the total.
    float running = 0.f;
    for (uint k = 0u; k < top.count; ++k)
    {
        running += top.w[k];
        gLightGridList[flat * kLightsPerCellMax + k]   = top.id[k];
        gLightGridWeight[flat * kLightsPerCellMax + k] = running;
    }

    gLightGridCount[flat] = top.count;          // every cell written → no host clear
}
