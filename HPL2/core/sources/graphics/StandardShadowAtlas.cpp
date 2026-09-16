#include "graphics/StandardShadowAtlas.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hpl {

bool StandardShadowIsPow2(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

uint32_t StandardShadowFloorPow2(uint32_t value) {
  if (value == 0)
    return 0;
  uint32_t result = 1;
  while (value >>= 1)
    result <<= 1;
  return result;
}

uint32_t StandardShadowCeilPow2(uint32_t value) {
  if (value <= 1)
    return 1;
  if (value > 0x80000000u)
    return 0x80000000u;
  const uint32_t floor = StandardShadowFloorPow2(value);
  return floor == value ? value : floor << 1;
}

uint32_t StandardShadowTileSize(uint32_t legacySize, float projectedDiameterPx,
                                uint32_t minTileSize) {
  uint32_t size = StandardShadowFloorPow2(legacySize);
  if (size == 0)
    return 0;
  if (std::isfinite(projectedDiameterPx) && projectedDiameterPx > 0.0f) {
    const float clamped =
        std::min(projectedDiameterPx, static_cast<float>(size));
    const uint32_t screen =
        StandardShadowCeilPow2(static_cast<uint32_t>(std::ceil(clamped)));
    size = std::min(size, screen);
  }
  const uint32_t minimum = std::min(StandardShadowFloorPow2(minTileSize),
                                    StandardShadowFloorPow2(legacySize));
  return std::max(size, minimum);
}

static bool ValidConfig(const StandardShadowAtlasConfig &config) {
  return StandardShadowIsPow2(config.atlasSize) && config.maxAtlases > 0 &&
         StandardShadowIsPow2(config.minTileSize) &&
         config.minTileSize <= config.atlasSize;
}

void StandardShadowFitBudget(const StandardShadowAtlasConfig &config,
                             std::vector<StandardShadowTileRequest> &requests) {
  if (!ValidConfig(config)) {
    requests.clear();
    return;
  }
  for (StandardShadowTileRequest &request : requests)
    request.size =
        std::min(StandardShadowFloorPow2(request.size), config.atlasSize);

  const uint64_t atlasArea = uint64_t(config.atlasSize) * config.atlasSize;
  uint64_t remaining = atlasArea * config.maxAtlases;

  std::vector<uint32_t> owners;
  for (const StandardShadowTileRequest &request : requests)
    if (std::find(owners.begin(), owners.end(), request.owner) == owners.end())
      owners.push_back(request.owner);

  std::vector<bool> keep(requests.size(), false);
  std::vector<size_t> tiles;
  for (uint32_t owner : owners) {
    tiles.clear();
    bool invalid = false;
    for (size_t i = 0; i < requests.size(); ++i) {
      if (requests[i].owner != owner)
        continue;
      tiles.push_back(i);
      invalid |= requests[i].size < config.minTileSize;
    }
    if (invalid)
      continue;
    auto ownerArea = [&]() {
      uint64_t area = 0;
      for (size_t i : tiles)
        area += uint64_t(requests[i].size) * requests[i].size;
      return area;
    };
    uint64_t area = ownerArea();
    while (area > remaining) {
      bool canHalve = true;
      for (size_t i : tiles)
        canHalve &= requests[i].size / 2 >= config.minTileSize;
      if (!canHalve)
        break;
      for (size_t i : tiles)
        requests[i].size /= 2;
      area = ownerArea();
    }
    if (area > remaining)
      continue;
    remaining -= area;
    for (size_t i : tiles)
      keep[i] = true;
  }

  size_t out = 0;
  for (size_t i = 0; i < requests.size(); ++i)
    if (keep[i])
      requests[out++] = requests[i];
  requests.resize(out);
}

std::vector<StandardShadowTilePlacement>
StandardShadowPackAtlases(const StandardShadowAtlasConfig &config,
                          std::span<const StandardShadowTileRequest> requests,
                          uint32_t &atlasCount) {
  std::vector<StandardShadowTilePlacement> placements(requests.size());
  atlasCount = 0;
  if (!ValidConfig(config))
    return placements;

  std::vector<size_t> sorted(requests.size());
  std::iota(sorted.begin(), sorted.end(), size_t(0));
  std::stable_sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
    const StandardShadowTileRequest &ra = requests[a], &rb = requests[b];
    if (ra.size != rb.size)
      return ra.size > rb.size;
    if (ra.owner != rb.owner)
      return ra.owner < rb.owner;
    return ra.face < rb.face;
  });

  struct Corner {
    uint32_t x, y;
  };
  const uint32_t atlasSize = config.atlasSize;
  const uint64_t atlasArea = uint64_t(atlasSize) * atlasSize;
  std::vector<Corner> ladder;
  Corner pen{0, 0};
  uint64_t used = 0;
  uint32_t atlas = 0;
  bool open = false;

  for (size_t index : sorted) {
    const uint32_t size = requests[index].size;
    if (!StandardShadowIsPow2(size) || size > atlasSize)
      continue;
    const uint64_t area = uint64_t(size) * size;
    // Guard the geometric invariant as well as the area one: a tile that would
    // leave the page also starts the next atlas instead of overlapping.
    if (!open || used + area > atlasArea || pen.y + size > atlasSize ||
        pen.x + size > atlasSize) {
      const uint32_t next = open ? atlas + 1 : 0;
      if (next >= config.maxAtlases)
        break;
      atlas = next;
      open = true;
      ladder.clear();
      pen = {0, 0};
      used = 0;
    }

    placements[index] = {atlas, pen.x, pen.y, size};
    used += area;
    pen.x += size;
    if (!ladder.empty() && ladder.back().y == pen.y + size)
      ladder.back().x = pen.x;
    else
      ladder.push_back({pen.x, pen.y + size});

    if (pen.x == atlasSize) {
      // The row is complete: drop down to the next step of the ladder. Steps
      // that no longer rise above the new pen row stop constraining it.
      ladder.pop_back();
      pen.y += size;
      while (!ladder.empty() && ladder.back().y <= pen.y)
        ladder.pop_back();
      pen.x = ladder.empty() ? 0 : ladder.back().x;
    }
  }
  atlasCount = open ? atlas + 1 : 0;
  return placements;
}

} // namespace hpl
