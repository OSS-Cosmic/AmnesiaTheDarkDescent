#pragma once

// Standard's fullscreen decal projection math.  The object decal list is the
// receiver association prepared by World::Compile; this module deliberately
// does not discover decals spatially or reinterpret the packed surface ID.
import SceneTypes;
import BlendModes;
import Utils.Color.ColorHelpers;

static const float kStandardDecalFacingCos = 0.25f;

public bool standardDecalBoxHit(GpuDecal decal, float3 positionW,
                                float3 normalW, out float3 localPosition)
{
    localPosition = mul(decal.invModelMat, float4(positionW, 1.0f)).xyz;
    if (any(abs(localPosition) > 0.5f))
        return false;

    // Match MainComposite: transform the receiver normal by the inverse box
    // matrix and require a meaningful component along decal-local +Y.  Both
    // sides are accepted; the threshold rejects box side-wall bleed without
    // depending on a possibly flipped authored axis.
    float3 localNormal = mul((float3x3)decal.invModelMat, normalW);
    float normalLength = length(localNormal);
    return normalLength >= 1.0e-6f &&
           abs(localNormal.y) > kStandardDecalFacingCos * normalLength;
}

public float2 standardDecalAtlasUV(GpuDecal decal, float3 localPosition)
{
    uint sx = max(decal.subDivX, 1u);
    uint sy = max(decal.subDivY, 1u);
    float cellX = float(decal.subDivIndex % sx);
    float cellY = float(decal.subDivIndex / sx);
    return float2(
        saturate((cellX + localPosition.x + 0.5f) / float(sx)),
        1.0f - saturate((cellY + localPosition.z + 0.5f) / float(sy)));
}

// Propagate the receiver footprint captured before divergent control flow
// through this decal's box projection. Once an atlas coordinate
// is saturated, retaining the old derivative would make an edge sample pull
// in unrelated atlas cells, so the saturated component has no footprint.
public void standardDecalAtlasUVGrad(GpuDecal decal, float3 localPosition,
                                     float3 localPositionDx,
                                     float3 localPositionDy,
                                     out float2 uv, out float2 uvDx,
                                     out float2 uvDy)
{
    uint sx = max(decal.subDivX, 1u);
    uint sy = max(decal.subDivY, 1u);
    float cellX = float(decal.subDivIndex % sx);
    float cellY = float(decal.subDivIndex / sx);
    float2 raw = float2(
        (cellX + localPosition.x + 0.5f) / float(sx),
        1.0f - (cellY + localPosition.z + 0.5f) / float(sy));
    uv = saturate(raw);
    uvDx = float2(localPositionDx.x / float(sx),
                  -localPositionDx.z / float(sy));
    uvDy = float2(localPositionDy.x / float(sx),
                  -localPositionDy.z / float(sy));
    if (raw.x <= 0.0f || raw.x >= 1.0f)
    {
        uvDx.x = 0.0f;
        uvDy.x = 0.0f;
    }
    if (raw.y <= 0.0f || raw.y >= 1.0f)
    {
        uvDx.y = 0.0f;
        uvDy.y = 0.0f;
    }
}

public float3 standardDecalBlend(uint blendMode, float3 albedoLinear,
                                 float4 sampleLinear, float4 decalColor)
{
    // This is intentionally the same authored/display-space sequence as the
    // composite pass.  In particular, 0.5 remains neutral for MulX2.
    float4 sourceDisplay = float4(linearToSRGB(sampleLinear.rgb), sampleLinear.a)
                         * decalColor;
    // decal.frag.fsl rejects the sampled texture alpha before multiplying the
    // tint.  Keep that legacy cutoff: a transparent texture must not become
    // active merely because a tint has alpha greater than one.
    if (sampleLinear.a < kDecalAlphaReject)
        return albedoLinear;
    return sRGBToLinear(blendDisplaySpaceExact(
        blendMode, linearToSRGB(albedoLinear), sourceDisplay));
}
