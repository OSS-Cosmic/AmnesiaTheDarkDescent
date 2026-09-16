#pragma once

#include "graphics/RIDefines.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

#include <cstdint>

namespace hpl {
class cGraphics;

// The camera depth pyramid the GPU occlusion cull tests against.
//
// Level 0 is half the render extent; every further level is a 2x2 MAX reduce of
// the one before, so each texel holds the FARTHEST depth in its footprint --
// the operator that makes the cull a proof rather than a guess. cStandardHiZPass
// fills it; the cull kernel samples it through gCullHiZ.
//
// One per swapchain image, because the build discards and rewrites the whole
// chain each frame and a frame still in flight must not have its pyramid
// overwritten.
//
// This lives outside either viewport state so both the Standard and the Hybrid
// renderer can own one without duplicating the allocation and teardown.
struct HiZPyramid {
  static constexpr uint32_t kMaxMips = 13;

  RISharedPointer<RITexture> texture[RI_MAX_SWAPCHAIN_IMAGES];
  // Spans the whole chain: the cull kernel selects its level with Load(), so it
  // needs every mip reachable through a single descriptor.
  RISharedPointer<RITextureView> sampleView[RI_MAX_SWAPCHAIN_IMAGES];
  // One single-level storage view per mip: a compute write targets exactly one
  // level, and a storage view cannot span a chain.
  RISharedPointer<RITextureView> mipView[RI_MAX_SWAPCHAIN_IMAGES][kMaxMips];

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;

  HiZPyramid() = default;
  HiZPyramid(HiZPyramid &&) noexcept = default;
  HiZPyramid &operator=(HiZPyramid &&) noexcept = default;
  HiZPyramid(const HiZPyramid &) = delete;
  HiZPyramid &operator=(const HiZPyramid &) = delete;

  // Allocates the chain for one swapchain image from the render extent. Safe to
  // call per image; width/height/mipCount are the same for all of them.
  bool Create(cGraphics *graphics, uint32_t image, uint32_t renderWidth,
              uint32_t renderHeight);

  // Parks every owned handle in the graphics deferral queue. The caller still
  // owns the struct; the dimensions are left alone so a partially torn-down
  // pyramid still reports itself unusable through IsUsable.
  void Defer(cGraphics *graphics);

  // Whether this image has a pyramid worth culling against.
  bool IsUsable(uint32_t image) const {
    return mipCount > 0 && image < RI_MAX_SWAPCHAIN_IMAGES &&
           !texture[image].isEmpty() && !sampleView[image].isEmpty();
  }
};
} // namespace hpl
