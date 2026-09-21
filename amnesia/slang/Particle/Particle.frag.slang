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
import BlendModes;   // linear source preparation and opacity modulation

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
    float4 diffuseColor = smoothHalo ? smoothHaloLinearSample(psIn.uv, psIn.color)
        : translucentLinearSample(sample, psIn.color,
        gPushConstants.blendMode, (mat.materialConfig & kMaterialFlagDiffuseIsMask) != 0u);

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
    // Authoring controls operate in linear space, before display processing.
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
    // Soft-faded against OPAQUE depth only.
    //
    // Water used to fold into this through a separate nearest-water-depth
    // layer, so that submerged smoke could not draw over a water surface. It
    // also erased every splash: splashes spawn at or just under the waterline
    // (ps_water_dump_splash spawns at the emitter origin, waterlurker_*_splash
    // a little below it), the fade band is kSoftParticleFadeDistance = 0.35 m,
    // and these materials are BlendMode="Alpha", so a faded fragment does not
    // dim -- it trips the alpha-discard below and vanishes outright.
    //
    // Fading a splash against the very surface it is splashing off is the
    // wrong trade, so the water term is gone. Submerged particles now draw over
    // water; if that needs solving again it wants a test that can tell "behind
    // the surface" from "touching it", not a shared 0.35 m ramp.
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
    lightLevel *= max(mat.particleBrightnessScale, 0.0f);

    // Opacity and lighting modulate linear RGB; gamma never changes alpha.
    float4 modulated    = applyBlendModulation(gPushConstants.blendMode, diffuseColor,
                                               finalAlpha, lightLevel);

    // The legacy alpha test (translucency_particle.frag: `finalColor.a < 0.01
    // -> discard`), tested on the post-modulation alpha exactly as the original
    // did. This is NOT redundant with the blend state: applyBlendModulation
    // never folds texture alpha into RGB for Add/Mul/MulX2, so for those modes
    // the discard is the only thing that honours a cutout stored in alpha.
    // Drop it and a texture whose shape lives in alpha (ps_glass_piece,
    // ps_glass_shards) emits its transparent-region RGB across the whole quad
    // and reads as a solid square. The soft-intersection and lifetime fades
    // ride in `finalAlpha`, not in `.a`, so no sprite gains a contour here.
    if (modulated.a < kParticleAlphaDiscard)
        discard;

    return encodeBlendOutputLinear(gPushConstants.blendMode, modulated);
}
