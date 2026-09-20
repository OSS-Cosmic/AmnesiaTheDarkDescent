#pragma once

#include "HostDefinitions.h"

// Shared host/shader constants: bindless descriptor-set binding numbers, pool
// capacities, material/blend flag bits, and the renderer's tuning knobs
// (light grid, direct-lighting denoiser, path tracer, water).
// (Moved out of SceneTypes.slang.)

// -----------------------------------------------------------------------------
// Standard-BSDF diffuse lobe (Falcor BSDFConfig.slangh analogue). Selects the
// demodulated diffuse weight evaluated by Brdf.slang's diffuseLobeWeight():
//   Lambert   — flat 1 (the old look, modulo the now-explicit 1/π)
//   Disney    — Burley 2012 retro-reflection (fd90 = 0.5 + 2·LdotH²·rough)
//   Frostbite — Disney + Lagarde energy renormalization (Falcor's default)
// Pure preprocessor defines, so they live OUTSIDE the shared-const guard
// below: Brdf.slang's raw #include must see them (an undefined kDiffuseBrdf
// would silently #if-select Lambert).
// -----------------------------------------------------------------------------
#define kDiffuseBrdfLambert   0
#define kDiffuseBrdfDisney    1
#define kDiffuseBrdfFrostbite 2
#ifndef kDiffuseBrdf
#define kDiffuseBrdf kDiffuseBrdfFrostbite 
#endif

// The typed constants below are emitted in:
//   - every C++ TU (`static const` at namespace scope = internal linkage, no ODR worry), and
//   - exactly one Slang module: SceneTypes, gated by HPL_DEFINE_SHARED_CONSTS.
// Other Slang modules access them via `import SceneTypes` rather than
// re-emitting copies, which would collide at import sites that pull in
// both SceneTypes and (e.g.) Bindless.resource.
#if defined(__cplusplus) || defined(HPL_DEFINE_SHARED_CONSTS)

HOST_NAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// Engine-owned bindless descriptor set (set 0). Slang accepts a `static const
// uint` reference inside `[vk::binding(N, 0)]` so these can live as typed
// constants rather than preprocessor macros — same name lookup on the C++
// side when populating VkDescriptorSetLayoutBinding.
// -----------------------------------------------------------------------------
SHARED_CONST uint kBindingTextures2D                 = 0u;
SHARED_CONST uint kBindingTexturesCube               = 1u;
SHARED_CONST uint kBindingTextures2DArray           = 2u;  // Texture2DArray[] — animated images (one slot, frame = layer)
// DXIL geometry pull table. Vulkan uses the same logical stream handles as
// buffer device addresses; D3D12 uses the registered raw-SRV descriptor index.
SHARED_CONST uint kBindingGeometryBuffers            = 4u;
SHARED_CONST uint kGeometryBufferCapacity            = 32768u;
// Bindings 3..8 were the gOpaque*Handles BDA arrays, folded into UniformObject.
// Slot 3 is reused for the animated-texture record table; slot 4 is the HLSL
// geometry ByteAddressBuffer[] stream boundary; 5..8 stay free.
SHARED_CONST uint kBindingAnimTex                    = 3u;  // StructuredBuffer<AnimTexRec> — per-2D-array-slot animation params
SHARED_CONST uint kBindingMaterialSampler            = 9u;
// Slots 10..19 held the retired surfel-cache SSBOs (counter, surfel, geometry,
// valid, dirty, free, recycle, rayResult, cellInfo, cellToSurfel) and are free.
// Numbering of the survivors is unchanged.
SHARED_CONST uint kBindingSceneObjects               = 20u;  // UniformObject[]
SHARED_CONST uint kBindingMaterials                  = 21u;  // MaterialDataBlob[] flat material table
// Slots 22/23 (formerly kBindingPointLights / kBindingAreaLights on set 0) and
// 29 (kBindingSpotLights) / 42 (kBindingFogAreas) are now free — the per-world
// light/fog SSBOs moved to the dedicated per-world set kWorldSet. See
// kBindingWorld* below.
// 24/25/26 were the translucent/water/decal typed material SSBOs — folded into
// the single MaterialDataBlob table, leaving these slots free.
// Slots 27/28 held the retired surfel ref-counter / per-cell reservation SSBOs
// and are free.
SHARED_CONST uint kBindingPackedHitInfo             = 31u;  // RGBA32UI storage image
// Slots 33/34/35 held the retired surfel depth atlas (storage image / sampled
// image / its sampler) and 37 its SurfelBounds cull record — all free.
SHARED_CONST uint kBindingTlas                        = 36u;  // RaytracingAccelerationStructure
SHARED_CONST uint kBindingBindlessSlotGeneration      = 38u;  // per object slot: reuse generation (kObjectSlotCapacity)
// Slot 39 held the retired per-surfel captured anchor generation and is free.
SHARED_CONST uint kBindingLightGridCount              = 40u;  // RWStructuredBuffer<uint> (kLightGridCellCount) — world-space light grid
SHARED_CONST uint kBindingLightGridList               = 41u;  // RWStructuredBuffer<uint> (kLightGridCellCount * kLightsPerCellMax)
// Slot 42 (formerly kBindingFogAreas on set 0) is now free — fog moved to kWorldSet.
// Slots 43/44 were the refracted / reflected per-bounce V-buffers; nothing
// binds or declares them any more (water refraction clobbers gPackedHitInfo
// like glass, water reflection moved to the raster water pass).
SHARED_CONST uint kBindingPackedRefractionHitInfo     = 43u;  // RGBA32UI storage image — unused
SHARED_CONST uint kBindingPackedReflectionHitInfo     = 44u;  // RGBA32UI storage image — unused
// Slot 45 (formerly kBindingAttenuationLut, the light falloff LUT) is now free —
// lighting is fully analytic (inverse-square radial + smoothstep spot cone).
// Slots 46/47 (formerly kBindingDecals / kBindingObjectDecalIndices on set 0)
// are now free — decal data moved to the per-world set kWorldDecalSet, baked
// static by cWorld::Compile. See kBindingWorldDecals below.
SHARED_CONST uint kBindingDissolveMap                 = 48u;  // immutable core_dissolve noise (128px), UV-sampled by the shared alphaTest dissolve fade
// set 1: depth-aspect SRV of the scene depth buffer (opaque geometry), bound
// per-frame by the particle pass for the soft-particle depth fade. Reflected
// per-program (like gPerFrame / the other set-1 images), not a fixed layout.
SHARED_CONST uint kBindingSceneDepth                  = 49u;  // Texture2D<float> scene depth (soft particles)

// Per-world decal data baked by cWorld::Compile, bound on the MainCompositePass.
// It rides set 2 — the conventional per-pass compute-I/O set — alongside the
// composite's own resources, so its bindings start at 4 to clear the composite's
// set-2 slots 0..3 (gIndirectLighting@0, slot 1 unused, gOutput@2,
// gDirectLighting@3) — decals stay at 4 even though slot 1 is now a gap, so the
// layout stays stable. gDecals is read only by MainCompositePass.
SHARED_CONST uint kWorldDecalSet                      = 2u;
SHARED_CONST uint kBindingWorldDecals                 = 4u;  // StructuredBuffer<GpuDecal>  — baked OOB decals
SHARED_CONST uint kBindingWorldObjectDecalIndices     = 5u;  // StructuredBuffer<uint>      — flat per-object decal-index pool

// Per-world light/fog SSBOs. These are cWorld-owned persistent per-world buffers,
// so they ride their OWN dedicated set (kWorldSet = 3, otherwise unused) rather
// than the engine-global bindless set 0. Every pass that reads lights/fog binds
// this set via RIProgram::bindDescriptors (program-managed + cached: the same
// world buffers hash to a cache hit, so the descriptor set is written once and
// reused until a re-bake swaps a buffer). Counts still come from SceneConstants
// on set 1. cWorld just uploads buffer CONTENTS each frame; it no longer writes
// any descriptor set.
SHARED_CONST uint kWorldSet                           = 3u;
SHARED_CONST uint kBindingWorldPointLights            = 0u;  // RWStructuredBuffer<PointLight>
SHARED_CONST uint kBindingWorldSpotLights             = 1u;  // RWStructuredBuffer<SpotLight>
SHARED_CONST uint kBindingWorldAreaLights             = 2u;  // RWStructuredBuffer<RectLight>
SHARED_CONST uint kBindingWorldFogAreas               = 3u;  // RWStructuredBuffer<FogAreaParams>

// -----------------------------------------------------------------------------
// Bindless pool capacities + sentinel.
//   kObjectSlotCapacity:  total scene-object slots the bindless allocator
//                         hands out (set 0, kBindingSceneObjects).
//   k{Texture,Material}SlotCapacity: upper bound on bindless textures /
//                         materials per frame; the host sizes
//                         m_diffuseBindless from kObjectSlotCapacity.
//   k{Point,Spot,Box}SlotLightCapacity: per-frame light SSBO sizing.
//   kInvalidTextureIndex: sentinel returned by the texture slot allocator
//                         when a slot is missing or the pool is exhausted.
// -----------------------------------------------------------------------------
SHARED_CONST uint kObjectSlotCapacity        = 32768u;  // 16384 * 2
SHARED_CONST uint kTextureSlotCapacity       = 16384u;
// Materials live in one flat table (gMaterials, kBindingMaterials) of fixed-size
// MaterialDataBlobs indexed directly by UniformObject.materialID — Falcor's
// MaterialSystem model. The blob's first 8 bytes are a MaterialHeader (type +
// materialConfig); shaders read the type to dispatch and reinterpret<T> the blob
// into the concrete material struct (DiffuseMaterial / TranslucentMaterial /
// WaterMaterial). Adding a material type only needs a new enum value + struct +
// IMaterial conformance — no new buffer/binding/range. kMaterialBlobUints must be
// >= the largest material struct in uints (WaterMaterial is currently 19 → 76B).
SHARED_CONST uint kMaterialCapacity          = 4096u;   // material table slot count
SHARED_CONST uint kMaterialBlobUints         = 24u;     // uint32s per blob (96B; >= largest material struct)
SHARED_CONST uint kPointSlotLightCapacity    = 256u;
SHARED_CONST uint kSpotSlotLightCapacity     = 256u;
SHARED_CONST uint kAreaSlotLightCapacity     = 64u;     // rectangular area lights (rarer than point/spot)
SHARED_CONST uint kFogAreaCapacity           = 32u;
SHARED_CONST uint kMaxDecals                  = 4096u;  // gDecals[] capacity (clustered OOB decals)
SHARED_CONST uint kInvalidTextureIndex       = 0xffffffffu;

// -----------------------------------------------------------------------------
// GPU-driven particles.
//   kGpuParticleCapacity: slots in the single persistent GpuParticle pool
//                         (gParticleState). Every live emitter owns a fixed
//                         contiguous slice of it sized by its authored
//                         MaxParticleNum. The shipped game's worst case is the
//                         sum of every .ps MaxParticleNum == 14,056, so this is
//                         ~4x oversized for custom stories.
//   kGpuEmitterCapacity:  live emitter INSTANCES in a frame (gParticleEmitters).
//   kGpuEmitterParamCapacity: distinct authored emitter DATAS
//                         (gParticleEmitterParams), one per <ParticleEmitter>
//                         element across every loaded .ps.
//   kGpuParticleGroupSize: sim/expand workgroup width. One thread per particle
//                         SLOT (not per live particle) so a thread can spawn
//                         into its own dead slot without an allocator.
// -----------------------------------------------------------------------------
SHARED_CONST uint kGpuParticleCapacity       = 65536u;  // 64 B each = 4 MB
SHARED_CONST uint kGpuEmitterCapacity        = 1024u;
SHARED_CONST uint kGpuEmitterParamCapacity   = 1024u;
SHARED_CONST uint kGpuParticleGroupSize      = 64u;

// Animated textures: each animated Image is one Texture2DArray (N frames = N
// layers) at a single bindless slot in gTextures2DArray[] (kBindingTextures2DArray).
// The slot id is stored in a material/gobo texture index with kAnimatedTextureBit
// set; the shader strips the bit, reads gAnimTex[slot] for {frameCount, frameTime,
// mode}, and samples layer = animFrame(gPerFrame.afT). Mode values match the host
// eTextureAnimMode enum (1 = Loop, 2 = Oscillate).
SHARED_CONST uint kTexture2DArrayCapacity    = 1024u;       // animated images are few
SHARED_CONST uint kAnimatedTextureBit        = 0x80000000u; // texture-index high bit ⇒ 2D-array slot
SHARED_CONST uint kAnimModeLoop              = 1u;
SHARED_CONST uint kAnimModeOscillate         = 2u;

// -----------------------------------------------------------------------------
// TLAS instance-mask categories. Each TLAS instance picks one (or more) of
// these bits; each TraceRay / TraceRayInline passes a cull mask of the
// categories it wants to hit. instance.mask & ray.cullMask must be non-zero
// for a hit to be considered. Translucents joined the TLAS in the same
// commit that added the mesh translucent / refraction-bounce path, and the
// indirect-lighting path tracer must not bounce off them — sampling
// a translucent's shared DiffuseMaterial slot (no albedo texture, no solid
// scalars) feeds NaN into the bounce radiance, which surfaces as NaN in the
// indirect output.
SHARED_CONST uint kRayMaskOpaque             = 0x01u;
SHARED_CONST uint kRayMaskTranslucent        = 0x02u;
// Shadow queries always use this bit. The TLAS applies allLightsCastShadows
// or the authored caster flag, excludes meshes with replacement hulls, and
// includes shadow-only hulls without exposing them to primary/GI/reflection rays.
SHARED_CONST uint kRayMaskShadow             = 0x04u;
SHARED_CONST uint kRayMaskAll                = 0xffu;

// -----------------------------------------------------------------------------
// Material-config flag bits packed into DiffuseMaterial.materialConfig by the
// host (submitMaterial in GlobalManagedSets.cpp). Single source of
// truth shared with C++ via the SHARED_CONST macro — there is no separate
// enum on the host side; the host packs these directly.
// Shaders gate optional shading (e.g. the GGX specular lobe) on these; e.g.
// `(m.materialConfig & kMaterialFlagEnableSpecular) != 0u`.
// -----------------------------------------------------------------------------
SHARED_CONST uint kMaterialFlagEnableDiffuse            = 1u << 0;
SHARED_CONST uint kMaterialFlagEnableNormal             = 1u << 1;
SHARED_CONST uint kMaterialFlagEnableSpecular           = 1u << 2;
SHARED_CONST uint kMaterialFlagEnableAlpha              = 1u << 3;
SHARED_CONST uint kMaterialFlagEnableHeight             = 1u << 4;
SHARED_CONST uint kMaterialFlagEnableIllumination       = 1u << 5;
SHARED_CONST uint kMaterialFlagEnableCubeMap            = 1u << 6;
SHARED_CONST uint kMaterialFlagEnableDissolveAlpha      = 1u << 7;
SHARED_CONST uint kMaterialFlagEnableCubeMapAlpha       = 1u << 8;
SHARED_CONST uint kMaterialFlagIsHeightMapSingleChannel = 1u << 9;   // parallax samples .r vs .a
SHARED_CONST uint kMaterialFlagIsAlphaSingleChannel     = 1u << 10;
// Water surface — drives the V-buffer RT wave-animated refraction +
// reflection bounce and the GIRenderPass refraction swap (water also sets
// HasRefraction). Not a texture-presence flag; set host-side per MaterialID.
// Bit 11 is free (12/13 unused, 14/15/16 are the refraction/dissolve bits).
SHARED_CONST uint kMaterialFlagIsWater                  = 1u << 11;
// Bit 14 is solid-diffuse UseDissolveFilter aliased with translucent
// UseRefractionNormals — safe because the two material variants never share
// a draw call. Keep both names so the host populates either flag against the
// same bit per material kind.
SHARED_CONST uint kMaterialFlagUseDissolveFilter        = 1u << 14;
SHARED_CONST uint kMaterialFlagUseRefractionNormals     = 1u << 14;
SHARED_CONST uint kMaterialFlagUseRefractionEdgeCheck   = 1u << 15;
SHARED_CONST uint kMaterialFlagHasRefraction            = 1u << 16;
// Translucent "AffectedByLightLevel" material var: the shader dims the
// translucent/particle color by the surrounding analytic-light level
// (gScene.lightLevelAt) instead of rendering it full-bright.
SHARED_CONST uint kMaterialFlagAffectedByLightLevel     = 1u << 17;
SHARED_CONST uint kMaterialFlagDiffuseIsMask            = 1u << 18;
SHARED_CONST uint kMaterialFlagSmoothHalo               = 1u << 19;
SHARED_CONST uint kMaterialFlagLitDiffuse               = 1u << 20;

// Soft particles: world-space view-depth band (meters) over which a particle
// fades to zero alpha as it approaches the opaque geometry behind it. Larger =
// softer/wider fade; smaller = closer to the old hard intersection edge.
// Amnesia is meter-scale.
SHARED_CONST float kSoftParticleFadeDistance = 0.35f;

// Particle alpha rejection, ported from the original engine's
// translucency_particle.frag (`if(finalColor.a < 0.01) { discard; }`).
// NOT redundant with the blend state: applyBlendModulation scales .rgb only
// and leaves .a untouched for Add/Mul/MulX2, so for those three modes this
// test is the ONLY consumer of texture alpha. Without it, a texture whose
// cutout lives in alpha (retail ps_glass_piece on Add, ps_glass_shards on
// MulX2) emits its transparent-region RGB across the whole quad and reads as
// a solid square. 0.01 rather than 0: DXT5 alpha interpolates to small
// nonzero values in the empty region, which a `<= 0` test would let through.
SHARED_CONST float kParticleAlphaDiscard = 0.01f;

// Blend-mode value families (see BlendModes.slang for the shared math).
// Two DIFFERENT encodings of the same modes — do not mix them up:
//   1. kBlendMode*: the push-constant / pipeline scheme. Mirrors the
//      BlendMode enums in Decal/Particle/TranslucentMeshPipelineDesc.h and
//      the remapBlend writes at the HybridRenderer.cpp draw sites.
//   2. kMaterialBlendMode*: raw hpl::eMaterialBlendMode (GraphicsTypes.h,
//      None first), stored verbatim into GpuDecal.blendMode and consumed by
//      the composite's exact decal blend.
SHARED_CONST uint kBlendModeAdd         = 0u;
SHARED_CONST uint kBlendModeMul         = 1u;
SHARED_CONST uint kBlendModeMulX2       = 2u;
SHARED_CONST uint kBlendModeAlpha       = 3u;
SHARED_CONST uint kBlendModePremulAlpha = 4u;

SHARED_CONST uint kMaterialBlendModeNone        = 0u;
SHARED_CONST uint kMaterialBlendModeAdd         = 1u;
SHARED_CONST uint kMaterialBlendModeMul         = 2u;
SHARED_CONST uint kMaterialBlendModeMulX2       = 3u;
SHARED_CONST uint kMaterialBlendModeAlpha       = 4u;
SHARED_CONST uint kMaterialBlendModePremulAlpha = 5u;

// -----------------------------------------------------------------------------
// World-space cell-grid sizing. This grid was owned by the retired surfel cache;
// kCellDimension / kCellUnit survive because the light grid derives its world
// coverage (kLightGridExtent) from them.
//   kCellDimension: linear cell count along each axis of the uniform grid.
//   kCellUnit:      world-space side length of one cell.
// -----------------------------------------------------------------------------
SHARED_CONST uint  kCellDimension       = 250u;
SHARED_CONST float kCellUnit            = 0.25f;
SHARED_CONST uint2 kTileSize            = uint2(16, 16);

// World-space light grid (coarse, camera-centered) for NEE importance
// sampling. kLightGridExtent sets the per-axis world coverage; kLightGridUnit
// is derived from it. Built each frame by LightGridBuildPass; read by the
// direct pass and the path tracer's getCellLights.
SHARED_CONST uint  kLightGridDim       = 32u;
SHARED_CONST float kLightGridExtent    = float(kCellDimension) * kCellUnit;
SHARED_CONST float kLightGridUnit      = kLightGridExtent / float(kLightGridDim);
SHARED_CONST uint  kLightGridCellCount = kLightGridDim * kLightGridDim * kLightGridDim;  // 32768
SHARED_CONST uint  kLightsPerCellMax   = 128u;                                           // per-cell light-list cap (list buffer = kLightGridCellCount·this·4B); lights past it are dropped by atomic order

// Gameplay light probe (LightProbePass.cs) — an off-screen, world-space
// illumination sensor read back on the CPU. One thread per probe in a single
// workgroup, so this doubles as the dispatch's thread count. Gameplay asks for
// a handful of points at a time (the player sensor uses five), so a small fixed
// cap keeps the whole thing one wave and the readback one cache line per probe.
SHARED_CONST uint kMaxLightProbes        = 16u;
SHARED_CONST uint kMaxLightProbeExcludes = 4u;   // lights the sensor must not see (the player's own ambient/lantern)

// Decals use a precomputed per-object decal list (UniformObject.decalList →
// gObjectDecalIndices), not a spatial grid — see SceneTypes.UniformObject and
// World::Compile. kMaxObjectDecalIndices caps the flat association pool.
SHARED_CONST uint  kMaxObjectDecalIndices = 1u << 20;   // 1M (offset is 24-bit; cap well under that)

// -----------------------------------------------------------------------------
// GI bounce-albedo boost — art-directed, applied in the GI RAY PATH only
// (path-tracer NEE + bounce throughput); the direct pass and the composite's
// albedo multiply stay physically correct. Amnesia's diffuse sets are dark
// (measured mean GI-hit albedo ≈ 0.04 linear), so a physically-correct bounce
// keeps ~4% of the light's energy and indirect reads as black. Bounce albedo
// becomes pow(albedo, exp): 1.0 = physically correct, 0.5 (sqrt) lifts 0.04 →
// 0.2 (~5× indirect on this content) — the same knob as Lumen's Diffuse Color
// Boost. Per-channel pow slightly desaturates dark hues; expected.
// -----------------------------------------------------------------------------
SHARED_CONST float kGIAlbedoBoostExp = 1.0f;

// Reference per-pixel path tracer (PathTracePass.rt.slang).
SHARED_CONST uint  kPathTraceMaxBounces        = 3u;   // path vertices after the primary hit
SHARED_CONST uint  kPathTraceSamplesPerPixel   = 1u;   // rays per pixel per frame; temporal accumulation does the rest

// determins the strenght of the wave intensity
SHARED_CONST float kWaterRefractionIntensity = 0.6f;

// Brightness multiplier for the water refraction/reflection bounce radiance.
// The RT bounces are re-shaded (NEE direct + indirect + emission), which
// reads dimmer than the base game's fully-lit framebuffer/cubemap sample; this
// lifts them back toward that brightness. 1.0 = raw re-shade.
SHARED_CONST float kWaterReflectionExposure = 1.0f;
SHARED_CONST float kWaterRefractionExposure = 2.0f; //0.5f;

// Global scene exposure, applied by the tonemap post effect before the display
// encode (posteffect_tonemap.frag.slang). The maps carry legacy light
// intensities (3-5) authored for the old LDR renderer, while the PBR model
// here is radiance = color * intensity / (d^2 + sourceRadiusSq) -- so the
// composited scene lands far below display range. This is the single global
// compensation for that; per-map fixes belong in the light authoring.
//
// A linear multiply applied after HDR composition. Particle opacity and
// brightness are authored independently of the final display-gamma curve;
// exposure still scales their linear contribution along with the scene.
// Read host-side by the tonemap's callers (LuxMapHandler,
// LuxMainMenu, iEditorViewport, cLevelEditorCameraCapture).
SHARED_CONST float kSceneExposure = 2.5f;

// How much wave turbulence the REFLECTION bounce normal keeps (the refraction
// bounce always uses the full wave normal). The RT reflection is a sharp mirror
// trace; blending its normal back toward the flat surface normal calms the
// ripples so it reads more like a mirror and less turbulent. 0 = perfect
// mirror, 1 = full wave distortion.
SHARED_CONST float kWaterReflectionTurbulence = 0.3f;

// Continuation gain for the reduced-resolution water reflection producer.
// WaterReflection.rt.slang keeps the first reflected vertex at full strength and
// scales only the second-vertex continuation radiance with this art-direction
// knob: firstVertexRadiance + continuation * kWaterReflectionIndirectIntensity.
SHARED_CONST float kWaterReflectionIndirectIntensity = 1.0f;

// Evaluation / composite overlay modes. Mode 0 is indirect lighting (the
// default), and mode 6 is the shadow-flag visualisation. Values 1..5 are
// historical reserved slots from the retired Surfel diagnostics and have no
// shader implementation.
// The numbering is retained for compatibility with the host overlay API and
// the editor's lower toolbar (HPL2/tools/editors/common/EditorWindowLowerToolbar.cpp,
// which still selects between kOverlayModeShadowFlag and
// kOverlayModeIndirectLighting), not with any in-game debug combo box.
// kDefaultOverlayMode is the initial value for the runtime overlay-mode push
// constant. The host may change the active mode at runtime without
// recompiling the shader.
SHARED_CONST uint  kOverlayModeIndirectLighting = 0u;
SHARED_CONST uint  kOverlayModeShadowFlag       = 6u;
SHARED_CONST uint  kDefaultOverlayMode          = kOverlayModeIndirectLighting;

// Per-object render flag bits mirrored from GraphicsTypes.h for shader-side
// diagnostics. UniformObject.renderFlags stores the raw iRenderable flags.
SHARED_CONST uint  kObjectFlagShadowCaster      = 0x00000001u;

// Per-component lighting toggles. 0 = skip that term, 1 = add it. The host
// used to drive these via a per-pass CB; they were always set to 1 there, so
// they live here as static defaults. Flip in source + recompile.
//
// Direct and indirect remain separate until the composite, so these toggles
// can isolate either contribution there.
SHARED_CONST uint  kDefaultRenderDirectLighting    = 1u;
SHARED_CONST uint  kDefaultRenderIndirectLighting  = 1u;
// Geometry rejection for ReSTIR DI reuse: reproject/compare the surface key
// (view depth + normal) and reject a neighbour or the previous frame's
// reservoir when the surface differs. Used by DirectLightingPass (temporal
// reservoir reprojection) and DirectSpatialReusePass (spatial neighbour
// rejection). NRD uses its own disocclusion tests.
SHARED_CONST float kReprojZRelTol                  = 0.05f;   // ≤5% view-depth difference accepted
SHARED_CONST float kReprojNormalCos                = 0.9f;    // ≈25° normal tolerance

// NRD v4.17 Reblur hit-distance normalization parameters (A, B, C). The
// vendored NRD removed the historical D member in v4.17; keep this vector in
// the shared header so the shader and C++ nrd::ReblurSettings setup cannot
// drift. The C++ integration MUST read hpl::kNrdHitDistanceParameters
// and copy its components into ReblurSettings::hitDistanceParameters.{A,B,C}.
SHARED_CONST float3 kNrdHitDistanceParameters    = float3(3.0f, 0.1f, 20.0f);

// ReSTIR DI (Bitterli 2020, biased reuse). The direct pass keeps a per-pixel
// light-selection reservoir instead of a radiance running-mean: a streaming RIS
// build over the cell lights, then temporal reuse (reproject + merge previous
// frame's reservoir) and spatial reuse (merge a few same-surface neighbours).
// Effective candidate count grows into the hundreds at one shadow ray/pixel/
// frame, which is the root-cause fix for many-light noise.
//   kReservoirMClamp — cap the reprojected previous M at N× the current build M
//     so stale lighting can't dominate (responsiveness vs. variance).
//   kSpatialSamples / kSpatialRadius — neighbour count + pixel search radius for
//     the spatial-reuse pass; neighbours are geometry-rejected on the depth/
//     normal key (kReproj* tolerances) to stop light leaking across surfaces.
SHARED_CONST float kReservoirMClamp                = 30.0f;   // temporal staleness cap (× current M)
SHARED_CONST int   kSpatialSamples                 = 6;       // spatial neighbours resampled
SHARED_CONST float kSpatialRadius                  = 16.0f;   // spatial search radius (px)

// PBR point/spot-light falloff — Falcor LightData semantics, no scale knobs:
// `intensity` IS the light's radiance (the host uploads the authored engine
// radius verbatim for both point and spot — one unit for every analytic light,
// so direct and the GI NEE agree by construction). The shader emits
// radiance = color · intensity · 1/(d² + sourceRadius²) — a softened
// inverse-square that goes HDR (>1) near the source so lights cross the bloom
// white point. Brightness tuning is an AUTHORING concern (light radius /
// color), not a constant.
//
// LightGridBuildPass bins each light out to the distance where its brightest
// channel's radiance dims to kLightRadianceFloor:
//   reach² = maxChannel(color) · intensity / kLightRadianceFloor − sourceRadius²
// reach² ≤ 0 (peak below the floor) drops the light entirely. NOTE: this bin reach
// also governs indirect/GI spread — the GI NEE only samples lights binned into a
// hit's cell, so lowering the floor widens and brightens GI (and crowds the
// per-cell light cap); it is not purely a grid-cost knob.
SHARED_CONST float kLightRadianceFloor         = 0.005f;    // min per-channel radiance (linear) worth binning; reach² = maxC(color)·authoredRadius/floor − sourceRadiusSq (scale-independent)
SHARED_CONST float kPointLightSourceRadiusSq   = 0.25f;  // soft source radius² (0.5m) — near-field softening + on-source peak cap (caps on-lamp radiance at color·intensity/this instead of 1/d²→∞; raise to soften lamp hotspots further)
// Default soft-shadow source radius when a light authors none (GetSourceRadius()==0):
// a fraction of the authored reach radius, so the penumbra scales with the lamp and
// lights are soft by default instead of hard. Host-only fallback in the point-light
// upload; an explicitly authored sourceRadius always wins.
SHARED_CONST float kPointLightDefaultSourceRadiusFrac = 0.05f;  // 5% of authored radius (e.g. 5m reach ⇒ 0.25m penumbra disk)

SHARED_CONST float kParalaxScale = 0.4f;


// Diffuse-lobe roughness for materials without a specular map (no gloss
// channel to decode): fully rough = matte with the strongest grazing
// retro-reflection. Materials with a spec map decode their roughness from
// tex[3].y (Brdf.slang decodeMaterialRoughness).
SHARED_CONST float kDefaultDiffuseRoughness = 1.0f;


HOST_NAMESPACE_END

#endif // __cplusplus || HPL_DEFINE_SHARED_CONSTS
