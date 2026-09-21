// Standard opaque lighting resolve.  This pass intentionally has no Scene,
// Material, RayQuery, or re_* imports: it is usable by the later translucent,
// particle, and water paths with a caller-supplied shadow visibility.
import PerFrame.resource;
import StandardLighting;
import SceneTypes;
import MaterialRaster;
import bindless;
import Utils.Color.ColorHelpers; // linearToSRGB / sRGBToLinear for the decal fold

// PCSS arithmetic, shared verbatim with the headless tests in
// tests/graphics/standard_shadow_filter.cpp.
#include "../StandardShadowFilter.h"

// Set 2 binds the five reconstructed surfaces, two legacy light-record buffers,
// counts and samplers. The HDR output is a separate render attachment.
[vk::binding(0,2)] Texture2D<float4> standardColorInput;
[vk::binding(1,2)] Texture2D<float4> standardPositionInput;
[vk::binding(2,2)] Texture2D<float4> standardNormalInput;
[vk::binding(3,2)] Texture2D<float4> standardShadingNormalInput;
[vk::binding(4,2)] Texture2D<uint4>  standardSurfaceInput;
[vk::binding(5,2)] StructuredBuffer<StandardPointLight> standardPointLights;
[vk::binding(6,2)] StructuredBuffer<StandardSpotLight> standardSpotLights;
[vk::binding(13,2)] StructuredBuffer<StandardBoxLight> standardBoxLights;
[vk::binding(7,2)] ConstantBuffer<StandardLightCounts> standardLightCounts;
// Shadow atlas pages (one array layer per page) and the tiles packed in them.
[vk::binding(11,2)] Texture2DArray<float> standardShadowMap;
[vk::binding(16,2)] StructuredBuffer<StandardShadowTile> standardShadowTiles;
[vk::binding(12,2)] Texture2D<float> standardAmbientOcclusionInput;
// Type="Decal" mesh accumulators: multiply (cleared white), add (cleared black).
[vk::binding(14,2)] Texture2D<float4> standardDecalMulInput;
[vk::binding(15,2)] Texture2D<float4> standardDecalAddInput;

// Legacy scatter-disk offsets (32 px tiles for Medium, 64 px for High), two
// offsets per texel stacked as jitterSamples/2 tile blocks.
[vk::binding(10,2)] Texture2D<float4> standardShadowJitterInput;

// Per-tile light list from Standard.lightCull.cs. Layout and the overflow
// sentinel are documented beside kStandardLightTile* in Constants.h. When
// standardLightCounts.lightTilesX is 0 this is unbound-but-valid and every
// light is looped, which is the path the translucent and reflection passes
// take (they share this module's light buffers but not the camera's tiles).
[vk::binding(17,2)] StructuredBuffer<uint> standardTileLights;

// Emulates the legacy LINEAR comparison sampler with four bilinearly weighted
// texel compares inside one atlas tile. Spot tiles treat taps outside the tile
// as the fully-lit border; cube-face tiles clamp to the edge texel (their
// frustum is widened so the kernel stays inside the rendered area).
float standardShadowCompare(StandardShadowTile tile, float2 uv, float depth)
{
    int extent = (int)tile.size;
    float2 texelPos = uv * (float)tile.size - 0.5f;
    int2 base = (int2)floor(texelPos);
    float2 f = texelPos - (float2)base;
    int2 origin = int2((int)tile.originX, (int)tile.originY);
    float lit[4];
    for (int i = 0; i < 4; ++i)
    {
        int2 t = base + int2(i & 1, i >> 1);
        if (tile.clampToTile != 0u)
            t = clamp(t, int2(0, 0), int2(extent - 1, extent - 1));
        else if (t.x < 0 || t.y < 0 || t.x >= extent || t.y >= extent)
        {
            lit[i] = 1.0f;
            continue;
        }
        float mapDepth = standardShadowMap.Load(int4(origin + t, (int)tile.atlasLayer, 0));
        lit[i] = depth <= mapDepth ? 1.0f : 0.0f;
    }
    return lerp(lerp(lit[0], lit[1], f.x), lerp(lit[2], lit[3], f.x), f.y);
}

// Nearest-texel depth fetch, for the blocker search. Deliberately NOT the
// bilinear compare above: the search wants the depths that are actually stored,
// not a filtered visibility. Outside a spot tile reads as fully unoccluded,
// matching standardShadowCompare's border rule.
float standardShadowLoadDepth(StandardShadowTile tile, float2 uv)
{
    int extent = (int)tile.size;
    int2 t = (int2)floor(uv * (float)tile.size);
    if (tile.clampToTile != 0u)
        t = clamp(t, int2(0, 0), int2(extent - 1, extent - 1));
    else if (t.x < 0 || t.y < 0 || t.x >= extent || t.y >= extent)
        return 1.0f;
    int2 origin = int2((int)tile.originX, (int)tile.originY);
    return standardShadowMap.Load(int4(origin + t, (int)tile.atlasLayer, 0));
}

// Fetches a published tile. Out-of-range indices and malformed tiles are
// treated as "no shadow" rather than reading past the buffer.
// `tileCount` is passed in rather than queried here: this sits in the innermost
// per-light loop, and GetDimensions is a resource query that cannot be hoisted
// out of it by the compiler. psMain reads it once per pixel instead.
bool standardShadowTileAt(uint index, uint tileCount, out StandardShadowTile tile)
{
    if (index >= tileCount)
    {
        tile = (StandardShadowTile)0;
        return false;
    }
    tile = standardShadowTiles[index];
    return tile.size != 0u && tile.size <= 0x7fffu && isfinite(tile.bias);
}

// Projects a world position into a tile. Validates the unbiased receiver first:
// bias must not pull an outside-frustum receiver into the comparison.
bool standardShadowProject(StandardShadowTile tile, float3 position,
                           out float2 shadowUV, out float biasedDepth)
{
    shadowUV = float2(0.0f);
    biasedDepth = 0.0f;
    float4 shadowP = mul(tile.viewProj, float4(position, 1.0f));
    if (!isfinite(shadowP.x) || !isfinite(shadowP.y) ||
        !isfinite(shadowP.z) || !isfinite(shadowP.w) || shadowP.w <= 0.0f)
        return false;
    float3 ndc = shadowP.xyz / shadowP.w;
    if (!standardFinite3(ndc) || ndc.x < -1.0f || ndc.x >= 1.0f ||
        ndc.y < -1.0f || ndc.y >= 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        return false;
    biasedDepth = (shadowP.z - tile.bias) / shadowP.w;
    if (!isfinite(biasedDepth)) return false;
    // The negative-height Vulkan raster viewport maps NDC +Y to texel Y=0.
    shadowUV = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
    return true;
}

// Legacy deferred_light_spotlight.frag.fsl filter. Low is one bilinear
// compare; Medium/High take four jittered presamples and only run the full
// kernel inside the penumbra. Offsets are in texels of the tile.
float standardShadowFilter(StandardShadowTile tile, float2 shadowUV, float biasedDepth,
                           uint2 pixel)
{
    uint quality = standardLightCounts.shadowQuality;
    float texelSize = 1.0f / (float)tile.size;
    // Low quality, or a tile with no emitter size, keeps the plain one-texel
    // bilinear compare this pass has always done.
    if (quality == 0u || !(tile.lightSize > 0.0f))
        return standardShadowCompare(tile, shadowUV, biasedDepth);

    uint jitterSize = quality >= 2u ? 64u : 32u;
    uint jitterSamples = quality >= 2u ? 32u : 16u;
    uint2 jitterTile = pixel % jitterSize;

    // ---- blocker search ----
    // How far in front of the receiver an occluder could be and still shadow
    // it. The jitter offsets are sorted outermost-first, so the first rows
    // already span the search region.
    float receiverLinear =
        standardShadowLinearDepth(biasedDepth, tile.nearPlane, tile.farPlane);
    float searchUV = max(
        standardShadowSearchUV(receiverLinear, tile.nearPlane, tile.lightSize,
                               tile.tanHalfFov),
        texelSize);

    float blockerSum = 0.0f;
    float blockerCount = 0.0f;
    for (uint s = 0u; s < 4u; ++s)
    {
        float4 o = standardShadowJitterInput.Load(
            int3((int)jitterTile.x, (int)(jitterTile.y + s * jitterSize), 0)) * 2.0f - 1.0f;
        float2 taps[2] = { o.xy, o.zw };
        for (uint t = 0u; t < 2u; ++t)
        {
            float depth = standardShadowLoadDepth(tile, shadowUV + taps[t] * searchUV);
            if (depth < biasedDepth)
            {
                blockerSum += standardShadowLinearDepth(depth, tile.nearPlane,
                                                        tile.farPlane);
                blockerCount += 1.0f;
            }
        }
    }

    // No blocker found does NOT mean lit: the search radius can be smaller than
    // the occluder's offset. Falling back to the plain compare keeps this from
    // ever being brighter than the filter it replaced, which is the failure
    // mode that shows up as light leaking through walls.
    if (blockerCount < 0.5f)
        return standardShadowCompare(tile, shadowUV, biasedDepth);

    // ---- penumbra ----
    float blockerLinear = blockerSum / blockerCount;
    float penumbraUV = standardShadowPenumbraUV(receiverLinear, blockerLinear,
                                                tile.lightSize, tile.tanHalfFov);
    float radiusUV = standardShadowFilterRadiusUV(penumbraUV, texelSize,
                                                  kStandardShadowMaxFilterTexels);

    // Contact-hard: the penumbra collapsed to the one-texel floor, so the wide
    // kernel would only resample what the bilinear compare already answers.
    // This is what keeps hard-edged regions as cheap as they were before PCSS,
    // where the old code leaned on its (sum-1)*sum penumbra test.
    if (!(radiusUV > texelSize))
        return standardShadowCompare(tile, shadowUV, biasedDepth);

    // ---- PCF at that radius ----
    float sum = 0.0f;
    for (uint k = 0u; k < jitterSamples / 2u; ++k)
    {
        float4 o = standardShadowJitterInput.Load(
            int3((int)jitterTile.x, (int)(jitterTile.y + k * jitterSize), 0)) * 2.0f - 1.0f;
        sum += standardShadowCompare(tile, shadowUV + o.xy * radiusUV, biasedDepth);
        sum += standardShadowCompare(tile, shadowUV + o.zw * radiusUV, biasedDepth);
    }
    return sum / (float)jitterSamples;
}

float standardSpotShadowVisibility(StandardSpotLight light, float3 position,
                                   uint2 pixel, uint tileCount)
{
    if ((light.config & kStandardLightHasShadow) == 0u ||
        light.shadowIndex == kStandardInvalidShadow || !standardFinite3(position))
        return 1.0f;
    StandardShadowTile tile;
    if (!standardShadowTileAt(light.shadowIndex, tileCount, tile))
        return 1.0f;
    float2 shadowUV;
    float biasedDepth;
    if (!standardShadowProject(tile, position, shadowUV, biasedDepth))
        return 1.0f;
    return standardShadowFilter(tile, shadowUV, biasedDepth, pixel);
}

// Point lights own six cube-face tiles (+X,-X,+Y,-Y,+Z,-Z). The face is the
// major axis of the light-to-receiver vector, matching the face frusta the
// CPU rasterized.
float standardPointShadowVisibility(StandardPointLight light, float3 position,
                                    uint2 pixel, uint tileCount)
{
    if ((light.config & kStandardLightHasShadow) == 0u ||
        light.shadowIndex == kStandardInvalidShadow || light.shadowIndex > 0xfffffff0u ||
        !standardFinite3(position))
        return 1.0f;
    float3 d = position - light.position;
    float3 a = abs(d);
    uint face;
    if (a.x >= a.y && a.x >= a.z) face = d.x >= 0.0f ? 0u : 1u;
    else if (a.y >= a.z)          face = d.y >= 0.0f ? 2u : 3u;
    else                          face = d.z >= 0.0f ? 4u : 5u;
    StandardShadowTile tile;
    if (!standardShadowTileAt(light.shadowIndex + face, tileCount, tile))
        return 1.0f;
    float2 shadowUV;
    float biasedDepth;
    if (!standardShadowProject(tile, position, shadowUV, biasedDepth))
        return 1.0f;
    return standardShadowFilter(tile, shadowUV, biasedDepth, pixel);
}

struct V { float4 posH : SV_POSITION; float2 uv : TEXCOORD0; };

[shader("vertex")]
V vsMain(uint id : SV_VertexID)
{
    V v;
    v.uv = float2(float((id << 1) & 2), float(id & 2));
    v.posH = float4(v.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return v;
}

[shader("pixel")]
float4 psMain(V v) : SV_TARGET0
{
    uint2 pixel = uint2(v.posH.xy);
    float4 color = standardColorInput.Load(int3(pixel, 0));
    float4 position = standardPositionInput.Load(int3(pixel, 0));
    float4 normal = standardNormalInput.Load(int3(pixel, 0));
    uint4 packed = standardSurfaceInput.Load(int3(pixel, 0));
    if (position.w <= 0.0f) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint materialID = packed.y;
    float2 uv = asfloat(packed.zw);
    DiffuseMaterial material = loadDiffuseMaterial(materialID);
    float2 specular = float2(0.0f);
    if (material.tex[3] != kInvalidTextureIndex &&
        (material.materialConfig & kMaterialFlagEnableSpecular) != 0u)
        specular = sampleBindless2D(material.tex[3], uv).xy;

    StandardLightSurface surface;
    // Base game order on its gamma G-buffer colour: Type="Decal" meshes blend
    // in, then SSAO multiplies it (eDeferredSSAO_OnColorBuffer), then lights
    // read it. Do both in display space. Untouched pixels skip the round trip
    // so plain surfaces keep exact albedo.
    float3 decalMul = standardDecalMulInput.Load(int3(pixel, 0)).rgb;
    float3 decalAdd = standardDecalAddInput.Load(int3(pixel, 0)).rgb;
    // Clamp so the 1x1 "unoccluded" fallback reads its only texel.
    uint aoWidth, aoHeight;
    standardAmbientOcclusionInput.GetDimensions(aoWidth, aoHeight);
    uint2 aoPixel = min(pixel, uint2(max(aoWidth, 1u) - 1u, max(aoHeight, 1u) - 1u));
    float ao = standardAmbientOcclusionInput.Load(int3(aoPixel, 0)).x;
    float3 albedo = color.rgb;
    bool decalTouched = any(decalMul != float3(1.0f)) || any(decalAdd != float3(0.0f));
    if (decalTouched || ao != 1.0f)
    {
        float3 albedoDisplay = linearToSRGB(max(albedo, 0.0f));
        if (decalTouched)
            albedoDisplay = saturate(albedoDisplay * decalMul + decalAdd);
        albedo = sRGBToLinear(albedoDisplay * ao);
    }
    surface.color = albedo; // pre-light material color, decals and AO applied
    surface.position = position.xyz;
    surface.uv = uv;
    surface.specular = specular;
    surface.specularScale =
        ((material.materialConfig & kMaterialFlagEnableSpecular) != 0u &&
         material.tex[3] != kInvalidTextureIndex) ? 1.0f : 0.0f;
    surface.normal = mul((float3x3)gPerFrame.invViewRotationMat,
                         standardShadingNormalInput.Load(int3(pixel, 0)).xyz);

    float3 V = gPerFrame.posW - surface.position;
    float3 result = float3(0.0f);
    uint pointCount = standardLightCounts.pointCount;
    uint spotCount = standardLightCounts.spotCount;
    // Queried once here, not per light: see standardShadowTileAt.
    uint shadowTileCount, shadowTileStride;
    standardShadowTiles.GetDimensions(shadowTileCount, shadowTileStride);

    // Box lights applied first, then point and spot lights additively, finally emission.
    uint boxCount = standardLightCounts.boxCount;
    for (uint i = 0u; i < boxCount; ++i)
        result = standardBoxLight(result, surface.color, surface.position, standardBoxLights[i]);

    // Per-tile light list. Standard.lightCull.cs binned this tile's lights
    // against the world-space AABB of the surfaces visible in it, so a light in
    // the next room that cannot reach any of them is simply absent. The list is
    // exact -- a tile that could not fit its lights stores the overflow
    // sentinel and falls back to the full loop below -- and the indices are
    // ascending, so the accumulation order is unchanged.
    uint tileBase = 0u;
    uint tilePointCount = pointCount;   // fall back to every light
    uint tileSpotCount  = spotCount;
    bool tiled = false;
    if (standardLightCounts.lightTilesX != 0u)
    {
        uint2 tile = pixel / kStandardLightTileSize;
        uint  flat = tile.x + tile.y * standardLightCounts.lightTilesX;
        uint  base = flat * kStandardLightTileStride;
        uint  stored = standardTileLights[base];
        if (stored != kStandardLightTileOverflow)
        {
            tiled = true;
            tileBase = base + 2u;
            tilePointCount = stored;
            tileSpotCount  = standardTileLights[base + 1u];
        }
    }

    // The reach test gates the shadow filter. standardPointLight /
    // standardSpotLight repeat it internally and return zero for exactly the
    // same lights, so skipping here changes only what a rejected light costs,
    // never what it contributes. It stays even when tiled: the tile AABB is a
    // conservative bound over the whole tile, not this pixel.
    for (uint k = 0u; k < tilePointCount; ++k)
    {
        uint i = tiled ? standardTileLights[tileBase + k] : k;
        StandardPointLight light = standardPointLights[i];
        if (!standardLightReaches(light.config, light.radius, light.position,
                                  surface.position))
            continue;
        float visibility = standardPointShadowVisibility(light, surface.position,
                                                         pixel, shadowTileCount);
        result += standardPointLight(surface, V, light, visibility);
    }
    // Hoisted out of the loop: these are loop-invariant, and the reach test
    // below makes the loop body divergent, where derivatives are undefined.
    float3 positionDx = ddx(surface.position);
    float3 positionDy = ddy(surface.position);
    for (uint k = 0u; k < tileSpotCount; ++k)
    {
        // Spot indices follow the point indices in the same tile run.
        uint i = tiled ? standardTileLights[tileBase + tilePointCount + k] : k;
        StandardSpotLight light = standardSpotLights[i];
        if (!standardLightReaches(light.config, light.radius, light.position,
                                  surface.position))
            continue;
        float visibility = standardSpotShadowVisibility(light, surface.position,
                                                        pixel, shadowTileCount);
        result += standardSpotLight(surface, V, light, visibility,
                                    positionDx, positionDy);
    }

    UniformObject object = gSceneObjects[packed.x];
    result += standardIllumination(material, object.illuminationAmount, uv);

    // Emission is added here after direct lighting, before environment and fog composite.
    return float4(result, 1.0f);
}
