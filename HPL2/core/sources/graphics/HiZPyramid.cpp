#include "graphics/HiZPyramid.h"

#include "graphics/Graphics.h"

#include <algorithm>

namespace hpl {

bool HiZPyramid::Create(cGraphics *graphics, uint32_t image,
                        uint32_t renderWidth, uint32_t renderHeight) {
  if (!graphics || image >= RI_MAX_SWAPCHAIN_IMAGES)
    return false;

  // Level 0 is half the render extent ROUNDED UP: rounding down would drop the
  // last row/column of depth, and a missing occluder texel makes the stored
  // depth too near, which is the direction that culls visible geometry.
  const uint32_t pyramidWidth = std::max<uint32_t>(1u, (renderWidth + 1u) / 2u);
  const uint32_t pyramidHeight =
      std::max<uint32_t>(1u, (renderHeight + 1u) / 2u);
  uint32_t levels = 1;
  uint32_t extent = std::max(pyramidWidth, pyramidHeight);
  while (extent > 1u && levels < kMaxMips) {
    extent >>= 1;
    ++levels;
  }

  RITextureDesc td{};
  td.type = RI_TEXTURE_2D;
  td.format = RI_FORMAT_R32_SFLOAT;
  td.width = static_cast<uint16_t>(pyramidWidth);
  td.height = static_cast<uint16_t>(pyramidHeight);
  td.depth = 1;
  td.layerNum = 1;
  td.mipNum = static_cast<uint8_t>(levels);
  td.sampleCount = 1;
  // Each dispatch samples the whole pyramid through one SRV while writing a
  // single mip through a UAV, so read and write access overlap on the same
  // resource with no transition between -- see cStandardHiZPass::Build, which
  // holds the texture in a combined state for the entire build. Vulkan allows
  // that with VK_IMAGE_LAYOUT_GENERAL; on D3D12 only a simultaneous-access
  // texture admits both, since its layout is pinned to COMMON. Never cleared,
  // so the UAV-clear restriction that comes with the flag does not bite.
  td.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE |
             RI_USAGE_SIMULTANEOUS_ACCESS;
  RITexture created = RITexture::create(&graphics->device, td);
  if (created.isEmpty())
    return false;
  texture[image] = RISharedPointer<RITexture>(&graphics->device, created);
  texture[image]->setDebugObjectName(&graphics->device, "HiZPyramid.depth");

  RITextureViewDesc sample{};
  sample.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
  sample.format = RI_FORMAT_R32_SFLOAT;
  sample.baseMip = 0;
  sample.mipNum = levels;
  sample.layerNum = 1;
  RITextureView whole =
      RITextureView::create(&graphics->device, texture[image].Get(), sample);
  if (whole.isEmpty())
    return false;
  sampleView[image] = RISharedPointer<RITextureView>(&graphics->device, whole);

  for (uint32_t mip = 0; mip < levels; ++mip) {
    RITextureViewDesc storage{};
    storage.viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D;
    storage.format = RI_FORMAT_R32_SFLOAT;
    storage.baseMip = mip;
    storage.mipNum = 1;
    storage.layerNum = 1;
    RITextureView view =
        RITextureView::create(&graphics->device, texture[image].Get(), storage);
    if (view.isEmpty())
      return false;
    mipView[image][mip] =
        RISharedPointer<RITextureView>(&graphics->device, view);
  }

  width = pyramidWidth;
  height = pyramidHeight;
  mipCount = levels;
  return true;
}

void HiZPyramid::Defer(cGraphics *graphics) {
  if (!graphics)
    return;
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    graphics->graphicsDefer.push(texture[i]);
    graphics->graphicsDefer.push(sampleView[i]);
    for (uint32_t mip = 0; mip < kMaxMips; ++mip)
      graphics->graphicsDefer.push(mipView[i][mip]);
  }
  mipCount = 0;
}

} // namespace hpl
