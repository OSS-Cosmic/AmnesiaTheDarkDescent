// Raster-only Standard billboard/beam/particle pass.  This intentionally
// shares the particle material, blend, fog, and light-level contracts with
// Particle/Particle.frag.slang, but has no water or ray-tracing binding.

import bindless;
import SceneTypes;
import PerFrame.resource;
import MaterialRaster;
import Fog;
import BlendModes;

[vk::binding(0, 2)] Texture2D<float> standardSceneDepth;

struct StandardParticleVertex
{
    float3 viewPos : VIEWPOS;
    float2 uv : TEXCRD;
    float4 color : COLOR;
    nointerpolation uint objectId : OBJECTID;
    float4 positionH : SV_POSITION;
};

struct StandardParticlePushConstants
{
    uint blendMode;
    float sceneAlpha;
    // Legacy per-object CPU level (StandardTranslucentLightLevel).
    float lightLevel;
    // Material DepthTest; zero skips the soft-depth fade (legacy nodepth sprites).
    uint depthTest;
};

[vk::push_constant]
StandardParticlePushConstants pc;

// Standard/Vulkan projections use a [0, 1] device-depth range: zero is the
// near plane and one is the far plane. Perspective depth needs the usual
// reciprocal reconstruction; orthographic depth is already affine in view
// distance and must not go through the perspective reciprocal.
public float standardParticleSceneLinearDepth(float deviceDepth)
{
    if (gPerFrame.orthographicCamera != 0u)
        return lerp(gPerFrame.zNear, gPerFrame.zFar, deviceDepth);

    return gPerFrame.zNear * gPerFrame.zFar /
        (gPerFrame.zFar - deviceDepth * (gPerFrame.zFar - gPerFrame.zNear));
}

public float standardParticleSoftIntersection(float sceneLinearDepth,
                                              float particleLinearDepth)
{
    return saturate((sceneLinearDepth - particleLinearDepth) /
                    kSoftParticleFadeDistance);
}

[shader("vertex")]
StandardParticleVertex vsMain(uint vertexId : SV_VertexID,
                             uint drawInst : SV_InstanceID,
                             uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst + baseInst;
    UniformObject object = gSceneObjects[instanceId];
    uint index = object.indexHandle != 0ull
        ? ((uint*)object.indexHandle)[vertexId] : vertexId;
    float3 position = object.posHandle != 0ull
        ? ((float4*)object.posHandle)[index].xyz : float3(0.0f);
    float2 uv = object.uv0Handle != 0ull
        ? ((float3*)object.uv0Handle)[index].xy : float2(0.0f);
    float4 color = object.colorHandle != 0ull
        ? ((float4*)object.colorHandle)[index] : float4(1.0f);

    float4 viewPosition = mul(gPerFrame.viewMat,
                              mul(object.modelMat, float4(position, 1.0f)));
    StandardParticleVertex result;
    result.viewPos = viewPosition.xyz;
    result.uv = mul(object.uvMat, float4(uv, 0.0f, 1.0f)).xy;
    result.color = color;
    result.objectId = instanceId;
    result.positionH = mul(gPerFrame.projMat, viewPosition);
    return result;
}

[shader("fragment")]
float4 psMain(StandardParticleVertex input) : SV_TARGET
{
    TranslucentMaterial material =
        loadTranslucentMaterial(gSceneObjects[input.objectId].materialID);
    uint diffuseTexture = material.tex[0];
    bool smoothHalo = pc.blendMode == kBlendModeAdd &&
        (material.materialConfig & kMaterialFlagSmoothHalo) != 0u;

    float4 textureSample = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (!smoothHalo && diffuseTexture != kInvalidTextureIndex)
        textureSample = sampleBindless2D(diffuseTexture, input.uv);
    float4 source = smoothHalo
        ? smoothHaloLinearSample(input.uv, input.color)
        : translucentLinearSample(textureSample, input.color, pc.blendMode,
            (material.materialConfig & kMaterialFlagDiffuseIsMask) != 0u);

    float3 worldPosition = mul(gPerFrame.invViewMat,
                               float4(input.viewPos, 1.0f)).xyz;
    FogResult fog;
    fog.visibility = 1.0f;
    fog.color = float3(0.0f);
    if (gPerFrame.fogEnabled != 0u || gPerFrame.fogAreaCount != 0u)
        fog = computeFog(worldPosition, gPerFrame.posW);

    // DepthTest=false materials (torch halos, nodepth fog) ignore scene depth
    // entirely, as the legacy renderer did; only depth-tested sprites soften
    // where they meet geometry.
    float softFade = 1.0f;
    if (pc.depthTest != 0u)
    {
        float sceneDepth = standardSceneDepth.Load(
            int3(int2(input.positionH.xy), 0)).r;
        float sceneLinearDepth = standardParticleSceneLinearDepth(sceneDepth);
        float particleLinearDepth = -input.viewPos.z;
        softFade = standardParticleSoftIntersection(sceneLinearDepth,
                                                    particleLinearDepth);
    }
    float opacity = pc.sceneAlpha * fog.visibility * softFade *
                    saturate(material.particleOpacityScale);

    float lightLevel = 1.0f;
    if ((material.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
        lightLevel = saturate(pc.lightLevel);
    lightLevel *= max(material.particleBrightnessScale, 0.0f);

    float4 modulated = applyBlendModulation(pc.blendMode, source, opacity,
                                            lightLevel);

    // The legacy alpha test (translucency_particle.frag: `finalColor.a < 0.01
    // -> discard`). Tested here because neither encodeStandardBlendSource nor
    // encodeBlendOutputLinear touches .a, so this sees the same value the
    // original did. It is NOT redundant with the blend state:
    // applyBlendModulation never folds texture alpha into RGB for
    // Add/Mul/MulX2, so for those modes the discard is the only thing that
    // honours a cutout stored in alpha. Drop it and a texture whose shape
    // lives in alpha (ps_glass_piece on Add, ps_glass_shards on MulX2) emits
    // its transparent-region RGB across the whole quad and reads as a solid
    // square. The soft-depth and lifetime fades ride in `opacity`, not in .a,
    // so no sprite gains a contour here.
    if (modulated.a < kParticleAlphaDiscard)
        discard;

    return encodeBlendOutputLinear(pc.blendMode,
        encodeStandardBlendSource(pc.blendMode, modulated));
}
