#pragma once
#include "HostDefinitions.h"
// SceneTypes is the canonical Slang home for the SHARED_CONST capacity /
// grid / counter-slot constants declared in Constants.h. Setting
// HPL_DEFINE_SHARED_CONSTS makes Constants.h emit them here; other Slang
// modules that need them `import SceneTypes` instead of re-#including.
#define HPL_DEFINE_SHARED_CONSTS
#include "Constants.h"
#undef HPL_DEFINE_SHARED_CONSTS

HOST_NAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// Type discriminators. Every Light/Material struct stores its discriminator
// in `type` at offset 0, so any consumer can read the first 4 bytes of a
// record to identify its concrete kind without knowing which buffer it came
// from. This is the contract the ILight interface, the Material.slang loaders,
// and the light-grid build / NEE sampling loops rely on.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Scalar-layout point light. `type` discriminator at offset 0 (ILight contract).
// `intensity` is the light's radiance (Falcor LightData semantics — the
// authored engine radius uploads verbatim in HybridRenderer's light loops, no
// scale knobs; color already carries brightness). The radiance falloff
// (interface.slang) is a plain WINDOWLESS inverse-square, so intensity only sets
// brightness there; the reach cutoff (reach² = maxChannel(color)·intensity /
// kLightRadianceFloor − kPointLightSourceRadiusSq) lives solely in
// LightGridBuildPass binning. `color` is the linear diffuse
// colour. `goboTextureIndex` points into
// textures_cube[] (set 0, binding 1); when set, sample the cube in light-local
// space using the worldToLight* rows — the rows of the light's world rotation (R),
// which reconstruct R^T via `float3x3(worldToLightX, worldToLightY, worldToLightZ)`.
// -----------------------------------------------------------------------------
SLANG_PUBLIC struct PointLight {
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float intensity;                    // luminous power (CPU-derived)
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC float3  worldToLightX;
    SLANG_PUBLIC float3  worldToLightY;
    SLANG_PUBLIC float3  worldToLightZ;
    SLANG_PUBLIC float radius;                       // CPU-precomputed light-grid bin reach (world units); 0 ⇒ skip
    SLANG_PUBLIC float sourceRadius;                 // physical source radius (world units); 0 ⇒ hard shadow. Penumbra.
    SLANG_PUBLIC uint  shadowEnabled;                // 0 or 1 — gate the NEE shadow ray (GetCastShadows)
};

// Scalar-layout spot light. `intensity` = luminous power (same convention as
// PointLight; bin reach derived from the radiance floor in-shader — see PointLight
// above). The shader uses the windowless inverse-square radial falloff plus an
// angular cone term. Carries
// the cone forward + pre-baked cos(angle/2), an optional projected gobo, a
// shadow bit, and the light's view-projection matrix (gobo / future shadow-map
// UVs).
SLANG_PUBLIC struct SpotLight {
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float intensity;                    // luminous power (CPU-derived)
    SLANG_PUBLIC float3  direction;                    // world-space outward forward
    SLANG_PUBLIC float cosOuterAngle;                // cos(GetFOV() * 0.5)
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC uint  shadowEnabled;                // 0 or 1
    SLANG_PUBLIC float4x4  viewProjection;               // light-space ViewProj for gobo UVs
    SLANG_PUBLIC float radius;                       // CPU-precomputed light-grid bin reach (world units); 0 ⇒ skip
    SLANG_PUBLIC float sourceRadius;                 // physical source radius (world units); 0 ⇒ hard shadow. Penumbra.
};

// Scalar-layout rectangular area light, modeled on Unreal's Rect Light. The
// emitter is a width×height rectangle centered at `position`, lying in the local
// Y–Z plane: `right` (local +Y, source-width axis), `up` (local +Z, source-height
// axis), and `normal` (local +X, one-sided emission direction). `intensity` is the
// TOTAL energy averaged over the surface — radiance = intensity / (width·height),
// so enlarging the rect dims it (UE semantics). The shader (RectLight.sample in
// ILight.slang) shades the closest point on the rect as a softened inverse-square
// source windowed to 0 at `radius` (the spherical attenuation radius), tinted by an
// optional `sourceTextureIndex` (textures_2d[], rect-local UV). Soft shadows sample
// the rectangle itself (RectLight.sampleSourcePoint) — the area IS the source.
// `barnDoorAngle`/`barnDoorLength` shape the beam: four flaps hinged on the rect
// edges, tilted from the normal by the angle and extended by the length, occlude
// part of the emitter per shading point. barnClipRect (ILight.slang) turns that into
// a clipped source rectangle used for BOTH the closest-point shade (scaled by the
// visible area fraction) and the shadow-ray source sampling, so the beam narrows,
// dims at its edge, and its penumbra matches. Angle → 90° or length 0 leaves the
// flaps open (no-op).
SLANG_PUBLIC struct RectLight {
    SLANG_PUBLIC uint  sourceTextureIndex;           // textures_2d[]; kInvalidTextureIndex when absent
    SLANG_PUBLIC float3 position;                   // rect center, world space
    SLANG_PUBLIC float intensity;                    // total energy (radiance = intensity / area)
    SLANG_PUBLIC float3  color;                      // linear diffuse
    SLANG_PUBLIC float3  right;                      // local +Y (unit), source-width axis
    SLANG_PUBLIC float3  up;                         // local +Z (unit), source-height axis
    SLANG_PUBLIC float3  normal;                     // local +X (unit), emission direction
    SLANG_PUBLIC float width;                        // source width  (world units, along right)
    SLANG_PUBLIC float height;                       // source height (world units, along up)
    SLANG_PUBLIC float radius;                       // attenuation radius (world units); falloff windows to 0 here; 0 ⇒ skip
    SLANG_PUBLIC float barnDoorAngle;                // flap tilt from normal (radians); 90° ⇒ open
    SLANG_PUBLIC float barnDoorLength;               // flap length (world units); 0 ⇒ disabled
    SLANG_PUBLIC uint  shadowEnabled;                // 0 or 1 — gate the NEE shadow ray (GetCastShadows)
};

// Per-fog-area params for the screen-space fog iteration. One entry per
// active cFogArea each frame, capped at kFogAreaCapacity. The shader walks
// the SSBO per-pixel, intersects the eye→surface ray with each box, and
// accumulates a visibility scalar + fog colour contribution.
//
// `invModelMat` is the inverse of cFogArea::GetModelMatrix — it sends a
// world-space point into the area's unit-box local space ([-0.5, 0.5]^3).
// `flags`: bit 0 = ShowBacksideWhenInside, bit 1 = ShowBacksideWhenOutside.
// `color.rgb` is the linear fog colour; `colorA` is the legacy
// GetColor().a (fog alpha — 1.0 means fully opaque fog at full distance).
SLANG_PUBLIC struct FogAreaParams {
    SLANG_PUBLIC float4x4 invModelMat;     // 64 B
    SLANG_PUBLIC float3   color;           // 12 B (linear)
    SLANG_PUBLIC float    colorA;          //  4 B
    SLANG_PUBLIC float    start;           //  4
    SLANG_PUBLIC float    end;             //  4
    SLANG_PUBLIC float    falloffExp;      //  4
    SLANG_PUBLIC uint     flags;           //  4
};

// Per-2D-array-slot animation record (gAnimTex, kBindingAnimTex). Written once by
// cTextureManager when an animated Image is assigned its array slot; read by the
// bindless sample helper to pick the current layer from gPerFrame.afT. mode uses
// kAnimModeLoop / kAnimModeOscillate.
SLANG_PUBLIC struct AnimTexRec {
    SLANG_PUBLIC uint  frameCount;  // array layer count
    SLANG_PUBLIC float frameTime;   // seconds per frame
    SLANG_PUBLIC uint  mode;        // kAnimModeLoop / kAnimModeOscillate
    SLANG_PUBLIC uint  _pad;
};

// Texture-slot indices into the bindless `textures_2d[]` array (set=0,
// binding=0). One uint32 per slot — see per_frame.resource.glsl for the
// `DiffuseMaterial_*Texture_ID` accessors that index this array.
// Slot layout: 0=Diffuse, 1=NMap, 2=Alpha, 3=Specular,
//              4=Height, 5=Illumination, 6=DissolveAlpha, 7=CubeMapAlpha.
// `cubeMapTextureIndex` is the parallel reflection-cubemap slot (textures_cube[]).
// Shared material header — overlays the first 8 bytes of every material struct
// below (each starts with `type` then `materialConfig`). gMaterials stores one
// fixed-size MaterialDataBlob per materialID; a shader reinterprets the blob's
// head as a MaterialHeader to read the type/config cheaply (for dispatch), then
// reinterprets the whole blob into the concrete struct. Falcor MaterialHeader analogue.
SLANG_PUBLIC struct MaterialHeader {
    SLANG_PUBLIC uint  type;                          // MATERIAL_TYPE_* tag
    SLANG_PUBLIC uint  materialConfig;                // shared kMaterialFlag* bits
};

SLANG_PUBLIC struct DiffuseMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_DIFFUSE
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  cubeMapTextureIndex;          // kInvalidTextureIndex when absent
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float heightMapScale;               // SolidDiffuse-only
    SLANG_PUBLIC float heightMapBias;                // SolidDiffuse-only
    SLANG_PUBLIC float frenselBias;                  // SolidDiffuse
    SLANG_PUBLIC float frenselPow;                   // SolidDiffuse
};

// GPU twin of MaterialTranslucent (Material.h). All bool flags
// (isAffectedByLightLevel, hasRefraction, refractionEdgeCheck,
// refractionNormals) and the eMaterialBlendMode pack into materialConfig.
SLANG_PUBLIC struct TranslucentMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_TRANSLUCENT
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  cubeMapTextureIndex;          // reflection cubemap; kInvalidTextureIndex when absent
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float refractionScale;
    SLANG_PUBLIC float frenselBias;
    SLANG_PUBLIC float frenselPow;
    SLANG_PUBLIC float rimLightMul;
    SLANG_PUBLIC float rimLightPow;
    SLANG_PUBLIC float particleOpacityScale;
    SLANG_PUBLIC float particleBrightnessScale;
    SLANG_PUBLIC float litDiffuseScale;
};

// GPU twin of MaterialWater (Material.h). Bool flags (hasReflection,
// isLargeSurface, worldReflectionOcclusionTest) pack into materialConfig.
SLANG_PUBLIC struct WaterMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_WATER
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float refractionScale;
    SLANG_PUBLIC float frenselBias;
    SLANG_PUBLIC float frenselPow;
    SLANG_PUBLIC float reflectionFadeStart;
    SLANG_PUBLIC float reflectionFadeEnd;
    SLANG_PUBLIC float waveSpeed;
    SLANG_PUBLIC float waveAmplitude;
    SLANG_PUBLIC float waveFreq;
};

// Fixed-size, type-erased material record stored in gMaterials[materialID]
// (Falcor MaterialDataBlob analogue). The host memcpys one concrete material
// struct (DiffuseMaterial / TranslucentMaterial / WaterMaterial) into `data`;
// shaders read data[0] (== type) to dispatch and reinterpret<T>(blob) to recover
// the typed struct. kMaterialBlobUints must be >= the largest material struct.
SLANG_PUBLIC struct MaterialDataBlob {
    SLANG_PUBLIC uint data[kMaterialBlobUints];
};

SLANG_PUBLIC struct UniformObject {
    SLANG_PUBLIC float dissolveAmount;
    SLANG_PUBLIC uint  materialID;
    SLANG_PUBLIC float illuminationAmount;
    SLANG_PUBLIC float4x4  modelMat;
    SLANG_PUBLIC float4x4  invModelMat;
    SLANG_PUBLIC float4x4  prevModelMat;   // last frame's modelMat (== modelMat for statics); motion vectors
    SLANG_PUBLIC float4x4  uvMat;
    // Precomputed per-object decal list, packed (offset << 8) | count, indexing
    // gObjectDecalIndices[] (stable gDecals[] indices). Built once at load by
    // World::Compile from the static decal↔object intersection (the editor
    // baked decals as static meshes clipped to specific geometry). count==0 for
    // dynamic/movable objects, so they never receive decals. compositeDecals
    // iterates only this list — no spatial grid, no receiver-category bits.
    SLANG_PUBLIC uint  decalList;
    // Raw iRenderable render flags. Used by diagnostics (for example the
    // Shadow Flag overlay) without affecting ray masks or material data.
    SLANG_PUBLIC uint  renderFlags;

    // Per-object vertex/index stream device addresses (BDAs), folded in from the
    // former gOpaque*Handles bindless buffers — they were always indexed by this
    // same object slot. submitObject writes them from the renderable's vertex
    // buffer (or an explicit override for particle scratch streams); 0 = stream
    // absent. fetchBindlessTriangle() and the gbuffer / particle vertex shaders
    // read these instead of six parallel StructuredBuffer<uint64_t>. Placed last;
    // the uint64 block stays 8-byte aligned under scalar layout.
    SLANG_PUBLIC uint64_t posHandle;
    SLANG_PUBLIC uint64_t normalHandle;
    SLANG_PUBLIC uint64_t tangentHandle;
    SLANG_PUBLIC uint64_t uv0Handle;
    SLANG_PUBLIC uint64_t colorHandle;
    SLANG_PUBLIC uint64_t indexHandle;
};

// Clustered OOB decal. No geometry: invModelMat maps a world position into the
// decal's unit-cube box [-0.5,0.5]^3; the projection pass clips to the box and
// samples the decal texture directly via diffuseTexIndex. receiverMask is the
// eDecalReceiver filter; blendMode is eMaterialBlendMode. The geometry is baked
// once per world by cWorld::Compile into the world-owned set-2 gDecals[];
// diffuseTexIndex is refreshed each frame from the normal texture LRU
// (cWorld::RefreshDecalTextures), which keeps the texture MRU so its slot is stable.
SLANG_PUBLIC struct GpuDecal {
    SLANG_PUBLIC float4x4 invModelMat;
    SLANG_PUBLIC float4   color;
    SLANG_PUBLIC uint     diffuseTexIndex; // gTextures2D[] slot, refreshed each frame via the LRU
    SLANG_PUBLIC uint     receiverMask;
    SLANG_PUBLIC uint     blendMode;
    SLANG_PUBLIC uint     subDivX;        // texture-atlas grid (editor SubDiv)
    SLANG_PUBLIC uint     subDivY;
    SLANG_PUBLIC uint     subDivIndex;    // selected atlas cell (editor CurrentSubDiv)
    // World-space projection axis (decal local +Y). projectDecal rejects surfaces
    // facing away from it (matches DecalCreator's per-triangle backface cull).
    SLANG_PUBLIC float3   projAxisWS;
    SLANG_PUBLIC float    _pad;
};

// -----------------------------------------------------------------------------
// Gameplay light probe (LightProbePass.cs). Gameplay asks "how lit is this
// world point?" and reads the answer back on the CPU a few frames later; see
// cLightProbeQuery. Both structs are 16 bytes under scalar layout, so the host
// mirrors them with plain float arrays.
// -----------------------------------------------------------------------------

// One world-space probe request; padding keeps the CPU/GPU stride explicit.
SLANG_PUBLIC struct LightProbeRequest {
    SLANG_PUBLIC float3 posW;
    SLANG_PUBLIC float  padding;
};

// Sum of shadow-tested direct LightSample.Li contributions. The probe has no
// surface normal and applies no N-dot-L factor. Gameplay derives luminance.
SLANG_PUBLIC struct LightProbeResult {
    SLANG_PUBLIC float3 irradiance;
    SLANG_PUBLIC float  padding;
};

// -----------------------------------------------------------------------------
// Shared hit / sampling types (HitInfo / TriangleHit / VertexData / LightSample).
// Used by the V-buffer, direct-lighting and path-trace passes for hit packing
// and NEE sampling.
// -----------------------------------------------------------------------------

// Result of sampling any ILight from a shading point. `dir` points from the
// shading point toward the light; `distance` is finite for analytic lights
// and used to terminate the shadow ray exactly at the source.
SLANG_PUBLIC struct LightSample {
    SLANG_PUBLIC float3  Li;              // incident radiance (color * intensity * falloff)
    SLANG_PUBLIC float pdf;             // sampling pdf (1.0 for analytic)
    SLANG_PUBLIC float3  dir;             // unit dir from shading point to light
    SLANG_PUBLIC float distance;        // shadow-ray tMax
};

// Packed primary hit. Mirrors the layout of `gPackedHitInfo` (a uint4
// V-buffer image written by VBufferPomBary.cs from the raster VBufferRaster.3d
// hit) used across the V-buffer / G-buffer port:
//   .x : geometry instance id
//   .y : primitive (triangle) index inside the instance
//   .z : barycentrics packed as two float16s (low=b1, high=b2; b0 = 1-b1-b2)
//   .w : 0 (reserved). All-zero packed = "no hit", which `isValid()` tests.
// Wraps a single uint4 so shader code can do `HitInfo h = HitInfo(packed)`,
// matching Falcor's source-compatible constructor pattern.
SLANG_PUBLIC struct HitInfo {
    SLANG_PUBLIC uint4 packed;
};

// Closer-to-Falcor TriangleHit alias; same logical content as HitInfo but
// unpacked. Kept distinct so future hit kinds (procedural, displaced) can
// grow into HitInfo without disturbing the existing triangle-only paths.
SLANG_PUBLIC struct TriangleHit {
    SLANG_PUBLIC uint  instanceID;
    SLANG_PUBLIC uint  primitiveIndex;
    SLANG_PUBLIC float2  barycentrics;    // (b1, b2); b0 = 1 - b1 - b2
};

// Per-hit interpolated vertex data. World-space because every consumer of it
// (shading, light-grid cell lookup, NEE shadow origin) wants world-space.
SLANG_PUBLIC struct VertexData {
    SLANG_PUBLIC float3  posW;
    SLANG_PUBLIC float3  normalW;
    SLANG_PUBLIC float3  tangentW;
    SLANG_PUBLIC float2  texC;
};

// -----------------------------------------------------------------------------
// Per-frame scene UBO contents (scalar block layout). Bound on the GPU at
// set=1, binding=0 — see PerFrame.resource.slang. Populated by
// cHybridRenderer::Draw() each frame. Scalar layout means float3 packs at
// 12 B with 4-byte alignment, matching the host C++ struct exactly.
// -----------------------------------------------------------------------------
SLANG_PUBLIC struct SceneConstants {
    SLANG_PUBLIC float4x4  invViewRotationMat;
    SLANG_PUBLIC float4x4  viewMat;
    SLANG_PUBLIC float4x4  invViewMat;
    SLANG_PUBLIC float4x4  projMat;                    // current frame's actual jittered raster projection
    SLANG_PUBLIC float4x4  invProjMat;                 // inverse of the actual jittered projMat; exact depth-based world-position reconstruction
    SLANG_PUBLIC float4x4  prevViewMat;                // previous frame's UNJITTERED view matrix; motion vectors
    SLANG_PUBLIC float4x4  prevProjMat;                // previous frame's UNJITTERED projection; motion vectors
    SLANG_PUBLIC float4x4  unjitteredProjMat;          // current frame's unjittered projection; motion vectors and NRD

    SLANG_PUBLIC float worldFogStart;
    SLANG_PUBLIC float worldFogLength;
    SLANG_PUBLIC float oneMinusFogAlpha;
    SLANG_PUBLIC float fogFalloffExp;
    SLANG_PUBLIC float4  worldFogColor;

    SLANG_PUBLIC float2  viewTexel;
    SLANG_PUBLIC float2  viewportSize;
    SLANG_PUBLIC float afT;
    SLANG_PUBLIC uint  totalFrames;                // monotonic frame counter
    SLANG_PUBLIC float cameraFov;                  // main camera vertical FOV (radians)
    SLANG_PUBLIC uint  pointLightCount;            // gPointLights[] SLOT CAPACITY (high-water of stable slots; sparse, zeroed holes)
    SLANG_PUBLIC float fireflyClampThreshold;      // firefly luminance cap on per-sample path radiance (PathTracePass.rt.slang rayGen); <= 0 disables
    SLANG_PUBLIC float zNear;                      // view-space, positive
    SLANG_PUBLIC float zFar;                       // view-space, positive
    SLANG_PUBLIC uint  spotLightCount;             // gSpotLights[] SLOT CAPACITY (high-water of stable slots; sparse, zeroed holes)
    SLANG_PUBLIC uint  fogAreaCount;               // active entries in gFogAreas[]
    SLANG_PUBLIC uint  decalCount;                 // active entries in gDecals[]
    SLANG_PUBLIC uint  areaLightCount;             // gAreaLights[] SLOT CAPACITY (high-water of stable slots; sparse, zeroed holes)
    SLANG_PUBLIC uint  allLightsCastShadows;       // non-zero: treat every light as shadow-casting and trace shadow rays against all opaque instances

    // Camera sampling contract: top-left pixel coordinates (y down),
    // pixelUV = (pixel + 0.5) / renderExtent. jitterUV is the sample offset
    // from the pixel center in normalized render-extent units.
    SLANG_PUBLIC float3 posW;
    SLANG_PUBLIC float jitterX;                       // current jitterUV.x: top-left normalized sample offset from pixel center (jitterPixels / renderExtent, |jitterPixels| <= 0.5 render pixels), +x right
    SLANG_PUBLIC float3 cameraU;
    SLANG_PUBLIC float jitterY;                       // current jitterUV.y: top-left normalized sample offset from pixel center (jitterPixels / renderExtent, |jitterPixels| <= 0.5 render pixels), +y down
    SLANG_PUBLIC float3 cameraV;
    SLANG_PUBLIC float3 cameraW;
    SLANG_PUBLIC float prevJitterX;                   // previous frame's jitterUV.x, same units and sign convention
    SLANG_PUBLIC float prevJitterY;                   // previous frame's jitterUV.y, same units and sign convention
    SLANG_PUBLIC float materialMipBias;               // mip bias for gradient-based MATERIAL sampling when a temporal provider shades at a reduced render extent; 0 for Off/NativeAA/native-extent frames
    // Per-camera visible fog areas in legacy back-to-front draw order. The
    // parameter buffer is shared by all viewports and stays in world order.
    SLANG_PUBLIC uint fogAreaIndices[kFogAreaCapacity];
    SLANG_PUBLIC uint skyBoxTextureIndex;
    SLANG_PUBLIC uint skyBoxEnabled;
    SLANG_PUBLIC uint fogEnabled;
    SLANG_PUBLIC uint orthographicCamera;
    SLANG_PUBLIC float4 skyBoxColor;
    // Renderer clear color, in the target's configured color space.  The
    // Standard environment pass uses this only when skyBoxEnabled is zero;
    // skyBoxColor remains the active sky tint/cubemap multiplier.
    SLANG_PUBLIC float4 clearColor;
};
HOST_NAMESPACE_END
