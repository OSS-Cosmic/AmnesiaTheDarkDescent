/* CPU/GPU ABI for the legacy Standard direct-light resolve. */
#ifndef HPL_STANDARD_LIGHT_DATA_H
#define HPL_STANDARD_LIGHT_DATA_H

#include <cstdint>

namespace hpl {

// These records deliberately use only scalar/vector floats and uint32_t.  The
// matching Standard resolve shaders bind them as scalar-layout SSBOs.
enum : uint32_t {
  kStandardInvalidTexture = 0xffffffffu,
  kStandardLightPoint = 0u,
  kStandardLightSpot = 1u,
  kStandardLightHasGobo = 1u << 0,
  kStandardLightEnabled = 1u << 1,
  kStandardLightHasShadow = 1u << 2,
  kStandardInvalidShadow = 0xffffffffu
};

struct StandardPointLightData {
  float position[3];
  float radius;                 // authored legacy reach, <= 0 disables
  float color[3];               // raw authored diffuse RGB (display space; shaders decode)
  float specularScale;          // raw authored diffuse alpha
  float invViewRotation[16];
  uint32_t config;
  uint32_t falloffTexture;
  uint32_t goboTexture;
  // First of kStandardShadowCubeFaces shadow tiles (+X,-X,+Y,-Y,+Z,-Z), or
  // kStandardInvalidShadow.
  uint32_t shadowIndex;
};

struct StandardSpotLightData {
  float position[3];
  float radius;
  float direction[3];
  float oneMinusCosHalfFov;
  float color[3];
  float specularScale;
  float spotViewProjection[16];
  uint32_t config;
  uint32_t radialFalloffTexture;
  uint32_t coneFalloffTexture;
  uint32_t goboTexture;
  uint32_t shadowIndex;         // shadow tile index, or kStandardInvalidShadow
  float shadowBias;
  uint32_t shadowResolution;    // shadow tile size in texels
};

// One square shadow-map tile inside a shadow atlas page. Spot lights own one
// tile, point lights one per cube face. viewProjection is the exact matrix the
// tile was rasterized with.
struct StandardShadowTileData {
  float viewProjection[16];
  uint32_t atlasLayer;
  uint32_t originX;
  uint32_t originY;
  uint32_t size;
  float bias;
  uint32_t clampToTile;         // 1: taps outside clamp to the edge (cube faces)
  uint32_t reserved0;
  uint32_t reserved1;
};

inline constexpr uint32_t kStandardShadowCubeFaces = 6;

struct StandardBoxLightData {
  float boxMin[3];
  uint32_t config;               // kStandardLightEnabled, etc
  float boxMax[3];
  uint32_t blendFunc;            // 0 = Replace, 1 = Add
  float color[3];                // raw authored diffuse RGB
  float reserved;
};

struct StandardLightCounts {
  uint32_t pointCount;
  uint32_t spotCount;
  uint32_t boxCount;
  // eShadowMapQuality for the opaque resolve; 0 (Low) everywhere else.
  uint32_t shadowQuality;
};

using StandardPointLight = StandardPointLightData;
using StandardSpotLight = StandardSpotLightData;

static_assert(sizeof(StandardPointLightData) == 112,
              "Standard point-light ABI changed");
static_assert(sizeof(StandardSpotLightData) == 140,
              "Standard spot-light ABI changed");
static_assert(sizeof(StandardBoxLightData) == 48,
              "Standard box-light ABI changed");
static_assert(sizeof(StandardShadowTileData) == 96,
              "Standard shadow-tile ABI changed");
static_assert(sizeof(StandardLightCounts) == 16, "Standard light counts ABI changed");

// Set-2 bindings used by the Standard resolve.  Keeping these names and
// indices in the CPU header documents the shader interface; descriptor writes
// are resolved by shader name at runtime. Binding 10 holds the legacy shadow
// jitter offsets. Binding 12 is reserved for Standard AO.
enum : uint32_t {
  kStandardResolveColorBinding = 0,
  kStandardResolvePositionBinding = 1,
  kStandardResolveNormalBinding = 2,
  kStandardResolveShadingNormalBinding = 3,
  kStandardResolveSurfaceBinding = 4,
  kStandardPointLightsBinding = 5,
  kStandardSpotLightsBinding = 6,
  kStandardLightCountsBinding = 7,
  kStandardRampSamplerBinding = 8,
  kStandardGoboSamplerBinding = 9,
  kStandardShadowMapBinding = 11,
  kStandardAmbientOcclusionBinding = 12,
  kStandardBoxLightsBinding = 13,
  kStandardShadowTilesBinding = 16
};

class cWorld;
class iRenderable;

// Legacy RendererDeferred per-object light level for AffectedByLightLevel
// translucent meshes and particles: box lights add their peak colour,
// point/spot lights peak colour x linear radial falloff, capped at 1.
// Defined in StandardTranslucentPass.cpp.
float StandardTranslucentLightLevel(cWorld *world, iRenderable *object);

} // namespace hpl

#endif
