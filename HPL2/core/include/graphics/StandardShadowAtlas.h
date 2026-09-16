/* CPU shadow-atlas packing for the Standard renderer. No RI/Vulkan dependency. */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace hpl {

inline constexpr uint32_t kStandardShadowNoAtlas = 0xffffffffu;

// One square shadow tile. Spot lights request one tile, point lights six
// (one per cube face). Tiles of one light share an owner; owners appear in
// priority order (nearest light first).
struct StandardShadowTileRequest {
  uint32_t size = 0;
  uint32_t owner = 0;
  uint32_t face = 0;
};

struct StandardShadowTilePlacement {
  uint32_t atlas = kStandardShadowNoAtlas;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t size = 0;
  bool IsValid() const { return atlas != kStandardShadowNoAtlas; }
};

struct StandardShadowAtlasConfig {
  uint32_t atlasSize = 0;   // power of two, per atlas page
  uint32_t maxAtlases = 0;  // pages available in the atlas array
  uint32_t minTileSize = 0; // power of two; smaller owners are dropped
};

bool StandardShadowIsPow2(uint32_t value);
uint32_t StandardShadowFloorPow2(uint32_t value);
uint32_t StandardShadowCeilPow2(uint32_t value);

// Tile size policy: the legacy step (authored resolution, already clamped by
// the quality cap and the distance LOD) is lowered to the power of two that
// covers the light's projected screen diameter, but never below minTileSize.
// A non-finite or non-positive diameter (camera inside the light) keeps the
// legacy step.
uint32_t StandardShadowTileSize(uint32_t legacySize, float projectedDiameterPx,
                                uint32_t minTileSize);

// Fits the requests into maxAtlases * atlasSize^2 texels. Owners are visited in
// order of first appearance; an owner that does not fit the remaining budget
// has all its tiles halved together until it fits, and is removed entirely if
// it would drop below minTileSize. Sizes are also rounded down to powers of two
// and clamped to atlasSize. Kept requests preserve their input order.
void StandardShadowFitBudget(const StandardShadowAtlasConfig &config,
                             std::vector<StandardShadowTileRequest> &requests);

// Packs square power-of-two tiles with the descending-size "ladder" packer
// (lisyarus.github.io/blog/posts/texture-packing.html). When the current atlas
// has no area left for the next tile a new atlas is started. Because tiles are
// visited largest first, an atlas's used area is always a multiple of the
// current tile area, so pages fill exactly and any request set within the
// FitBudget budget is fully placed. placements[i] belongs to requests[i];
// invalid or unplaceable requests get an invalid placement.
std::vector<StandardShadowTilePlacement> StandardShadowPackAtlases(
    const StandardShadowAtlasConfig &config,
    std::span<const StandardShadowTileRequest> requests, uint32_t &atlasCount);

} // namespace hpl
