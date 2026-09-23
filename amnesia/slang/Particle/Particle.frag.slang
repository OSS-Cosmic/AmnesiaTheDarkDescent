/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/particle.frag. Like its GLSL twin, this is the
// bindless port of AmnesiaTheDarkDescent/HPL2/resource/translucency_particle.frag.fsl.
// Hardware blend state is selected on the C++ side (one pipeline per blend
// mode); this shader applies the matching per-pixel color-space transform
// the legacy shader did so the output going into the blender is in the
// right space (e.g. mul/mulx2 pre-bias before SRC*DST blend).

#include "../Constants.h"   // kMaterialFlagAffectedByLightLevel

import bindless;
import SceneTypes;
import PerFrame.resource;
import Scene;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import Fog;          // computeFog — shared world + fog-area visibility
import BlendModes;   // authored modulation and premultiplication
import Utils.Color.ColorHelpers;

// Mirrors the `PushBlock` written from cHybridRenderer::Draw at the per-
// particle-draw site (uint blendMode, float sceneAlpha). Fragment-only push
// range — matches VK_SHADER_STAGE_FRAGMENT_BIT on the vkCmdPushConstants
// call.
struct ParticlePushConstants
{
    uint  blendMode;
    float sceneAlpha;
};

[vk::push_constant]
ParticlePushConstants gPushConstants HPL_PUSH_CONSTANT_REGISTER;

struct ParticleVSOut
{
    [vk::location(0)] float3 viewPos                 : TEXCOORD0;
    [vk::location(1)] float2 uv                      : TEXCOORD1;
    [vk::location(2)] float4 color                   : TEXCOORD2;
    [vk::location(3)] nointerpolation uint objectId  : TEXCOORD3;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(ParticleVSOut psIn) : SV_TARGET
{
    UniformObject       obj = gSceneObjects[psIn.objectId];
    // Particle materials are all Type="translucent", so the material blob is
    // tagged MATERIAL_TYPE_TRANSLUCENT. Read them the same way Translucent.frag does.
    TranslucentMaterial mat = loadTranslucentMaterial(obj.materialID);

    float4 sample = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint diffuseTex = mat.tex[0];
    bool smoothHalo = gPushConstants.blendMode == kBlendModeAdd &&
        (mat.materialConfig & kMaterialFlagSmoothHalo) != 0u;
    if (!smoothHalo && diffuseTex != kInvalidTextureIndex)
        sample = sampleBindless2D(diffuseTex, psIn.uv);
    // The attachment is display-encoded during this pass. Recover the
    // authored texture RGB before tinting; alpha is always linear data.
    float4 diffuseColor;
    if (smoothHalo)
    {
        diffuseColor = smoothHaloLinearSample(psIn.uv, float4(1.0f));
        diffuseColor.rgb = linearToSRGB(diffuseColor.rgb) * psIn.color.rgb;
        diffuseColor.a = psIn.color.a;
    }
    else
    {
        float3 authored = linearToSRGB(sample.rgb);
        if ((mat.materialConfig & kMaterialFlagDiffuseIsMask) != 0u)
            authored = authored.rrr;
        diffuseColor = float4(authored * psIn.color.rgb, sample.a * psIn.color.a);
        if (gPushConstants.blendMode == kBlendModePremulAlpha)
            diffuseColor.rgb *= psIn.color.a *
                ((mat.materialConfig & kMaterialFlagDiffuseIsMask) != 0u ? sample.a : 1.0f);
    }

    // Per-pixel fog via the shared helper (world + fog areas), matching
    // Translucent.frag: the visibility scalar folds into the per-pixel alpha.
    // The world position is reconstructed from the interpolated view-space
    // position — one reconstruction shared with the lightLevel path below.
    // (Replaces an inline view-depth world-fog variant that ignored
    // gFogAreas; world-distance fog is slightly radial vs the old planar
    // falloff, and particles now fade inside fog areas like mesh
    // translucents — restoring legacy parity.)
    float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, gPerFrame.posW);
    // Opacity remains linear coverage, independent of the RGB blend space.
    // Opacity still controls both source coverage and destination transmission.
    float finalAlpha = gPushConstants.sceneAlpha * fog.visibility * saturate(mat.particleOpacityScale);

    // Soft particles: fade alpha as this camera-facing fragment approaches the
    // opaque geometry behind it, replacing the hard intersection edge with a
    // smooth gradient. Sample the scene depth (opaque) at this pixel, linearize
    // both to positive view-space distance (VK z[0,1]: 0 = near, 1 = far), and
    // fade across kSoftParticleFadeDistance meters. finalAlpha is the single
    // operand applyBlendModulation folds in for every blend mode (ADD/MUL/MULX2
    // toward their neutral, ALPHA/PREMUL via .a), so this fades all modes.
    // Depth is geometry, not filtered material color. Interpolating device
    // depth across a silhouette creates a fictitious surface and an incorrect
    // opacity band. The particle pass and opaque depth share the render extent.
    float sceneZdev = gSceneDepth.Load(int3(int2(psIn.posH.xy), 0)).r;
    float  sceneLinear =
        gPerFrame.zNear * gPerFrame.zFar /
        (gPerFrame.zFar - sceneZdev * (gPerFrame.zFar - gPerFrame.zNear));
    float  particleLinear = -psIn.viewPos.z;   // view forward = -Z
    // Fade against opaque depth only. Fading against water would erase
    // splashes born at or just below the waterline inside this 0.35 m band.
    float  soft = saturate((sceneLinear - particleLinear) / kSoftParticleFadeDistance);
    finalAlpha *= soft;

    // AffectedByLightLevel materials dim with the surrounding analytic light
    // (base-game afLightLevel behavior — see lightLevelAt). Per-pixel,
    // so large billboards get an exact gradient across the quad.
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
    {
        lightLevel = lightLevelAt(surfacePosW);
    }
    // Retain the existing multiplier-mode brightness control. Radiance
    // brightness remains a linear multiplier, applied separately below.
    bool multiplier = gPushConstants.blendMode == kBlendModeMul ||
                      gPushConstants.blendMode == kBlendModeMulX2;
    if (multiplier)
        lightLevel *= max(mat.particleBrightnessScale, 0.0f);

    // Legacy lighting and opacity modulation in authored space.
    float4 modulated    = applyBlendModulation(gPushConstants.blendMode, diffuseColor,
                                               finalAlpha, lightLevel);

    // Alpha and premultiplied-alpha blending already fade coverage to zero.
    // Cutting them off at 0.01 creates a visible contour over dark backgrounds
    // after display gamma, particularly with a boost. Keep the legacy cutout
    // only for modes whose RGB blending does not consume texture alpha.
    if (gPushConstants.blendMode != kBlendModeAlpha &&
        gPushConstants.blendMode != kBlendModePremulAlpha &&
        modulated.a < kParticleAlphaDiscard)
        discard;

    if (!multiplier)
        modulated.rgb = linearToSRGB(sRGBToLinear(modulated.rgb) *
                                     max(mat.particleBrightnessScale, 0.0f));

    // ONE / ONE_MINUS_SRC_ALPHA still requires straight-alpha RGB to be
    // premultiplied once, now in the same authored space as the destination.
    // Exposure encoding belongs to the scene conversion after this pass.
    return encodeBlendOutputLinear(gPushConstants.blendMode, modulated);
}
