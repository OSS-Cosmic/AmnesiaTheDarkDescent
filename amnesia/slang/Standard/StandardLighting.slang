#pragma once

// RT-free lighting for the Standard renderer.  The record declarations below
// are deliberately kept in this module: StandardLightData.h is their CPU
// scalar-layout twin.  Do not replace these with SceneTypes PointLight/SpotLight
// -- Standard preserves the legacy radius/falloff contract, while SceneTypes
// lights use the newer windowless inverse-square contract.
//
// Record layout (scalar layout, 16-byte rows):
//   Point: position.xyz, radius; color.rgb, specularScale;
//          invViewRotation (4 rows); config, falloff, gobo, shadowIndex
//   Spot:  position.xyz, radius; direction.xyz, oneMinusCosHalfFov;
//          color.rgb, specularScale; spotViewProj (4 rows);
//          config, radialFalloff, coneFalloff, gobo, shadowIndex, bias, resolution
//   Shadow tile: viewProj (4 rows); atlasLayer, originX, originY, size;
//          bias, clampToTile, reserved0, reserved1
//
// Texture indices are bindless 2D slots for the two 1D-authored ramps (the
// ramps are uploaded as width-N 2D views by the Standard light uploader), and
// bindless cube/2D slots for point/projected gobos.  kInvalidTextureIndex means
// "use the identity ramp" or "no gobo".

#include "../HostDefinitions.h"
#include "../Constants.h"
import SceneTypes;
import MaterialRaster;
import PerFrame.resource;
import bindless;
import Utils.Color.ColorHelpers;

[vk::binding(8,2)] SamplerState standardRampSampler;
[vk::binding(9,2)] SamplerState standardGoboSampler;

public static const uint kStandardLightHasGobo = 1u << 0;
public static const uint kStandardLightEnabled = 1u << 1;
public static const uint kStandardLightHasShadow = 1u << 2;
public static const uint kStandardInvalidShadow = 0xffffffffu;
public static const uint kStandardBoxBlendReplace = 0u;
public static const uint kStandardBoxBlendAdd = 1u;

public struct StandardPointLight
{
    public float3 position;
    public float radius;
    public float3 color;
    public float specularScale;
    public float4x4 invViewRotation;
    public uint config;
    public uint falloffTexture;
    public uint goboTexture;
    public uint shadowIndex; // first of six cube-face shadow tiles (+X,-X,+Y,-Y,+Z,-Z)
};

public struct StandardSpotLight
{
    public float3 position;
    public float radius;
    public float3 direction;
    public float oneMinusCosHalfFov;
    public float3 color;
    public float specularScale;
    public float4x4 spotViewProj;
    public uint config;
    public uint radialFalloffTexture;
    public uint coneFalloffTexture;
    public uint goboTexture;
    public uint shadowIndex;      // shadow tile index
    public float shadowBias;
    public uint shadowResolution; // shadow tile size in texels
};

// StandardShadowTileData twin: one square tile of a shadow atlas page.
public struct StandardShadowTile
{
    public float4x4 viewProj;
    public uint atlasLayer;
    public uint originX;
    public uint originY;
    public uint size;
    public float bias;
    public uint clampToTile;
    // PCSS inputs; see StandardShadowTileData. lightSize 0 disables softening.
    public float nearPlane;
    public float farPlane;
    public float tanHalfFov;
    public float lightSize;
    public uint reserved0;
    public uint reserved1;
};

public struct StandardBoxLight
{
    public float3 boxMin;
    public uint config;
    public float3 boxMax;
    public uint blendFunc;
    public float3 color;
    public float reserved;
};

public struct StandardLightCounts
{
    public uint pointCount;
    public uint spotCount;
    public uint boxCount;
    public uint shadowQuality; // eShadowMapQuality; Low outside the opaque resolve
};

public struct StandardLightSurface
{
    public float3 color;       // material color before lighting (AO identity lives here)
    public float3 normal;
    public float3 position;
    public float2 uv;
    public float2 specular;    // legacy spec map: intensity, gloss
    public float specularScale;
};

public float3 standardSafeNormalize(float3 value, float3 fallback)
{
    float squaredLength = dot(value, value);
    return isfinite(squaredLength) && squaredLength > 1.0e-12f
        ? value * rsqrt(squaredLength) : fallback;
}

public bool standardFinite3(float3 value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

public float3 standardOrthoTangent(float3 normal)
{
    float3 axis = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    return standardSafeNormalize(cross(axis, normal), float3(1.0f, 0.0f, 0.0f));
}

public float3 standardBoxLight(float3 accumulated, float3 materialColor, float3 position, StandardBoxLight light)
{
    if ((light.config & kStandardLightEnabled) == 0 || !standardFinite3(position)) return accumulated;
    bool inside = position.x >= light.boxMin.x && position.x <= light.boxMax.x &&
                  position.y >= light.boxMin.y && position.y <= light.boxMax.y &&
                  position.z >= light.boxMin.z && position.z <= light.boxMax.z;
    if (!inside) return accumulated;
    // Base game: albedo * colour in display space (deferred_light_box_frag.glsl).
    float3 lightContrib = sRGBToLinear(max(linearToSRGB(max(materialColor, 0.0f)) * light.color, 0.0f))
                        / kSceneExposure;
    return light.blendFunc == 0u ? lightContrib : accumulated + lightContrib;
}

// The reference samples a normalized distance squared.  Sampling at an edge
// must be black even when a clamp sampler or a malformed authored ramp would
// otherwise repeat the last texel.
public float standardRadialFalloff(uint textureIndex, float distanceSquared)
{
    if (!isfinite(distanceSquared) || distanceSquared >= 1.0f || distanceSquared < 0.0f)
        return 0.0f;
    if (textureIndex == kInvalidTextureIndex) return 1.0f;
    return gTextures2D[NonUniformResourceIndex(textureIndex)].SampleLevel(
        standardRampSampler, float2(saturate(distanceSquared), 0.5f), 0.0f).x;
}

public float standardConeFalloff(uint textureIndex, float oneMinusCos,
                                 float oneMinusCosHalfFov)
{
    if (!isfinite(oneMinusCos) || !isfinite(oneMinusCosHalfFov) ||
        oneMinusCos < 0.0f || oneMinusCosHalfFov <= 0.0f ||
        oneMinusCos >= oneMinusCosHalfFov)
        return 0.0f;
    if (textureIndex == kInvalidTextureIndex)
        return 1.0f;
    return gTextures2D[NonUniformResourceIndex(textureIndex)].SampleLevel(
        standardRampSampler,
        float2(saturate(oneMinusCos / oneMinusCosHalfFov), 0.5f), 0.0f).x;
}

public float3 standardNormalMapped(float3 geometricNormal, float2 uv,
                                  uint materialID, float3 dpdx, float3 dpdy,
                                  float2 duvdx, float2 duvdy)
{
    DiffuseMaterial m = loadDiffuseMaterial(materialID);
    if ((m.materialConfig & kMaterialFlagEnableNormal) == 0u ||
        m.tex[1] == kInvalidTextureIndex)
        return standardSafeNormalize(geometricNormal, float3(0.0f, 0.0f, 1.0f));

    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (!isfinite(det) || abs(det) < 1.0e-7f || !standardFinite3(dpdx) ||
        !standardFinite3(dpdy) || !isfinite(duvdx.x) || !isfinite(duvdx.y) ||
        !isfinite(duvdy.x) || !isfinite(duvdy.y))
        return standardSafeNormalize(geometricNormal, float3(0.0f, 0.0f, 1.0f));
    float3 N = standardSafeNormalize(geometricNormal, float3(0.0f, 0.0f, 1.0f));
    float3 rawT = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
    float3 T = standardSafeNormalize(rawT - N * dot(N, rawT),
                                     standardOrthoTangent(N));
    float3 B = standardSafeNormalize(cross(N, T), standardOrthoTangent(N));
    // The UV Jacobian sign is the tangent-space handedness.  Deriving B from
    // N and T first keeps it orthogonal even for a nearly degenerate basis.
    B *= det < 0.0f ? -1.0f : 1.0f;
    float3 mapN = sampleBindless2D(m.tex[1], uv).xyz * 2.0f - 1.0f;
    return standardSafeNormalize(mapN.x * T + mapN.y * B + mapN.z * N, N);
}

public float3 standardSampleGobo(uint textureIndex, float2 uv,
                                 float2 uvDx, float2 uvDy)
{
    if (textureIndex == kInvalidTextureIndex) return float3(1.0f);
    if ((textureIndex & kAnimatedTextureBit) != 0u)
    {
        uint a = textureIndex & ~kAnimatedTextureBit;
        float frame = (float)animFrame(gPerFrame.afT,
                                       gAnimTex[NonUniformResourceIndex(a)]);
        return gTextures2DArray[NonUniformResourceIndex(a)].SampleGrad(
            standardGoboSampler, float3(uv, frame), uvDx, uvDy).rgb;
    }
    return gTextures2D[NonUniformResourceIndex(textureIndex)].SampleGrad(
        standardGoboSampler, uv, uvDx, uvDy).rgb;
}

public float3 standardLegacyLight(StandardLightSurface s, float3 V,
                                  float3 lightDir, float3 lightColor,
                                  float attenuation, float visibility,
                                  float lightSpecularScale)
{
    float3 N = standardSafeNormalize(s.normal, float3(0.0f, 0.0f, 1.0f));
    float3 L = standardSafeNormalize(lightDir, float3(0.0f, 0.0f, 1.0f));
    float NoL = max(dot(N, L), 0.0f);
    // Base game evaluates the whole term in display space
    // (deferred_light_frag.glsl): albedo, light colour, gobo, ramps, N.L and
    // shadow are used raw. Re-encode the linear albedo, build that value, then
    // decode once so the tonemap's exposure + encode reproduce it.
    float3 diffuse = linearToSRGB(max(s.color, 0.0f)) * lightColor * NoL;

    float3 specular = float3(0.0f);
    if (lightSpecularScale > 0.0f && s.specularScale > 0.0f &&
        s.specular.x > 0.0f)
    {
        float3 viewDirection = standardSafeNormalize(V, N);
        float3 H = standardSafeNormalize(L + viewDirection, N);
        float exponent = exp2(s.specular.y * 10.0f) + 1.0f;
        float value = lightSpecularScale * s.specularScale * s.specular.x *
                      pow(saturate(dot(H, N)), exponent);
        specular = lightColor * value;
    }
    float3 display = (diffuse + specular) * attenuation * visibility;
    return sRGBToLinear(max(display, 0.0f)) / kSceneExposure;
}

public float3 standardPointLight(StandardLightSurface s, float3 V,
                                 StandardPointLight light, float visibility)
{
    if ((light.config & kStandardLightEnabled) == 0u ||
        !isfinite(light.radius) || light.radius <= 0.0f ||
        !standardFinite3(light.position) || !standardFinite3(s.position))
        return float3(0.0f);
    float3 toLight = light.position - s.position;
    float distanceScale = max(abs(toLight.x), max(abs(toLight.y), abs(toLight.z)));
    if (!isfinite(distanceScale) || distanceScale <= 0.0f) return float3(0.0f);
    float3 scaledToLight = toLight / distanceScale;
    float scaledD2 = dot(scaledToLight, scaledToLight);
    float distanceOverRadius = (distanceScale / light.radius) * sqrt(scaledD2);
    if (!isfinite(scaledD2) || !isfinite(distanceOverRadius) ||
        distanceOverRadius >= 1.0f || distanceOverRadius <= 0.0f)
        return float3(0.0f);
    float3 L = scaledToLight * rsqrt(scaledD2);
    float attenuation = standardRadialFalloff(light.falloffTexture,
                                               distanceOverRadius * distanceOverRadius);

    float3 gobo = float3(1.0f);
    if ((light.config & kStandardLightHasGobo) != 0u &&
        light.goboTexture != kInvalidTextureIndex)
    {
        // Legacy ABI name: invViewRotation contains the complete lightWorld
        // matrix. The enhanced deferred reference uses the homogeneous point
        // position L + cameraWorldPosition, where V is cameraWorldPosition -
        // surfaceWorldPosition in this shared helper contract.
        float3 cameraWorldPosition = V + s.position;
        if (!standardFinite3(cameraWorldPosition)) return float3(0.0f);
        float4 localDirH = mul(light.invViewRotation,
                               float4(L + cameraWorldPosition, 1.0f));
        float3 localDir = float3(localDirH.x, localDirH.y, localDirH.z);
        if (!standardFinite3(localDir) ||
            (localDir.x == 0.0f && localDir.y == 0.0f && localDir.z == 0.0f))
            return float3(0.0f);
        gobo = gTexturesCube[NonUniformResourceIndex(light.goboTexture)]
             .Sample(standardGoboSampler, localDir).rgb;
    }
    return standardLegacyLight(s, V, L, light.color * gobo, attenuation, visibility,
                               light.specularScale);
}

// Shared callers that do not have a shadow pass can use the fully-lit default.
public float3 standardPointLight(StandardLightSurface s, float3 V,
                                 StandardPointLight light)
{
    return standardPointLight(s, V, light, 1.0f);
}

public float3 standardSpotLight(StandardLightSurface s, float3 V,
                                StandardSpotLight light, float visibility,
                                float3 positionDx, float3 positionDy)
{
    if ((light.config & kStandardLightEnabled) == 0u ||
        !isfinite(light.radius) || light.radius <= 0.0f ||
        !standardFinite3(light.position) || !standardFinite3(s.position))
        return float3(0.0f);
    float3 toLight = light.position - s.position;
    float distanceScale = max(abs(toLight.x), max(abs(toLight.y), abs(toLight.z)));
    if (!isfinite(distanceScale) || distanceScale <= 0.0f) return float3(0.0f);
    float3 scaledToLight = toLight / distanceScale;
    float scaledD2 = dot(scaledToLight, scaledToLight);
    float distanceOverRadius = (distanceScale / light.radius) * sqrt(scaledD2);
    if (!isfinite(scaledD2) || !isfinite(distanceOverRadius) ||
        distanceOverRadius >= 1.0f || distanceOverRadius <= 0.0f)
        return float3(0.0f);
    float3 L = scaledToLight * rsqrt(scaledD2);
    float attenuation = standardRadialFalloff(light.radialFalloffTexture,
                                               distanceOverRadius * distanceOverRadius);
    float3 gobo = float3(1.0f);
    if ((light.config & kStandardLightHasGobo) != 0u &&
        light.goboTexture != kInvalidTextureIndex)
    {
        float4 p = mul(light.spotViewProj, float4(s.position, 1.0f));
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
            !isfinite(p.w) || p.w <= 0.0f) return float3(0.0f);
        float2 uv = p.xy / p.w * 0.5f + 0.5f;
        float2 uvDx = 0.5f * (mul(light.spotViewProj, float4(positionDx, 0.0f)).xy * p.w -
                              p.xy * mul(light.spotViewProj, float4(positionDx, 0.0f)).w) / (p.w * p.w);
        float2 uvDy = 0.5f * (mul(light.spotViewProj, float4(positionDy, 0.0f)).xy * p.w -
                              p.xy * mul(light.spotViewProj, float4(positionDy, 0.0f)).w) / (p.w * p.w);
        if (!isfinite(uv.x) || !isfinite(uv.y) || !standardFinite3(float3(uvDx, 0.0f)) ||
            !standardFinite3(float3(uvDy, 0.0f))) return float3(0.0f);
        gobo = standardSampleGobo(light.goboTexture, uv, uvDx, uvDy);
    }
    else
    {
        // Legacy measured the cone by angle and ignored Aspect without a gobo.
        // Shape it by the spot projection instead: the NDC radius traces an
        // ellipse Aspect times wider than tall, converted back to the angle it
        // is for a round cone so the falloff ramp and edge stay as authored
        // (Aspect 1 gives exactly 1 - dot(L, direction)).
        float4 p = mul(light.spotViewProj, float4(s.position, 1.0f));
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.w) || p.w <= 0.0f)
            return float3(0.0f);
        float ndcRadius = length(p.xy / p.w);
        float cosHalfFov = 1.0f - light.oneMinusCosHalfFov;
        if (!isfinite(ndcRadius) || cosHalfFov <= 0.0f) return float3(0.0f);
        float tanHalfFov = sqrt(max(1.0f - cosHalfFov * cosHalfFov, 0.0f)) / cosHalfFov;
        float tanAngle = ndcRadius * tanHalfFov;
        float oneMinusCos = 1.0f - rsqrt(1.0f + tanAngle * tanAngle);
        attenuation *= standardConeFalloff(light.coneFalloffTexture,
                                            oneMinusCos,
                                            light.oneMinusCosHalfFov);
    }
    return standardLegacyLight(s, V, L, light.color * gobo,
                               attenuation, visibility, light.specularScale);
}

public float3 standardSpotLight(StandardLightSurface s, float3 V,
                                StandardSpotLight light)
{
    return standardSpotLight(s, V, light, 1.0f, float3(0.0f), float3(0.0f));
}

public float3 standardIllumination(DiffuseMaterial material, float illuminationAmount, float2 uv)
{
    if (!isfinite(illuminationAmount) || illuminationAmount <= 0.0f)
        return float3(0.0f);
    if ((material.materialConfig & kMaterialFlagEnableIllumination) == 0u ||
        material.tex[5] == kInvalidTextureIndex)
        return float3(0.0f);
    // Base game adds texture * amount in display space (deferred_illumination_frag.glsl).
    return sRGBToLinear(linearToSRGB(max(sampleBindless2D(material.tex[5], uv).rgb, 0.0f)) * illuminationAmount)
         / kSceneExposure;
}
