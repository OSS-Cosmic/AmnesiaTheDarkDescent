#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RIDevice.h"
#include "graphics/RICommand.h"
#include "graphics/RIBuffer.h"
#include "graphics/RIFormat.h"
#include "graphics/RITexture.h"
#include "system/LowLevelSystem.h"

struct RID3D12BarrierSubresourceRange {
  uint32_t baseMip;
  uint32_t mipCount;
  uint32_t baseLayer;
  uint32_t layerCount;
  UINT firstPlane;
  UINT planeCount;
  UINT resourcePlaneCount;
  bool wholeResource;
};

static bool ri_d3d12_GetBarrierSubresourceRange(
    const RITextureBarrier &src, RID3D12BarrierSubresourceRange &out) {
  const RIFormatProps *props = GetRIFormatProps(src.texture->format);
  const uint32_t mipNum = src.texture->d3d12.mipNum;
  const uint32_t layerNum = src.texture->d3d12.layerNum;
  if (!mipNum || !layerNum)
    return false;

  const bool hasDepth = props->isDepth != 0;
  const bool hasStencil = props->isStencil != 0;
  out.resourcePlaneCount = (hasDepth && hasStencil) ? 2 : 1;
  switch (src.aspect) {
  case RI_BARRIER_ASPECT_COLOR:
    if (hasDepth || hasStencil)
      return false;
    out.firstPlane = 0;
    out.planeCount = 1;
    break;
  case RI_BARRIER_ASPECT_DEPTH:
    if (!hasDepth)
      return false;
    // A combined depth/stencil resource is two D3D12 planes, and D3D12 requires
    // every plane of a DSV to be in the depth state when it is bound. The
    // shared call sites all pass ASPECT_DEPTH because Vulkan (without
    // separateDepthStencilLayouts) transitions both aspects for a depth-only
    // aspectMask; covering both planes here keeps that contract. Leaving plane
    // 1 out left the stencil plane in COMMON for the resource's lifetime.
    out.firstPlane = 0;
    out.planeCount = hasStencil ? 2 : 1;
    break;
  case RI_BARRIER_ASPECT_STENCIL:
    if (!hasStencil)
      return false;
    out.firstPlane = hasDepth ? 1 : 0;
    out.planeCount = 1;
    break;
  case RI_BARRIER_ASPECT_DEPTH_STENCIL:
    if (!hasDepth || !hasStencil)
      return false;
    out.firstPlane = 0;
    out.planeCount = 2;
    break;
  default:
    assert(false);
    return false;
  }

  if (src.baseMip >= mipNum || src.baseLayer >= layerNum)
    return false;
  out.baseMip = src.baseMip;
  out.baseLayer = src.baseLayer;
  out.mipCount = src.mipCount ? src.mipCount : mipNum - out.baseMip;
  out.layerCount = src.layerCount ? src.layerCount : layerNum - out.baseLayer;
  if (out.mipCount > mipNum - out.baseMip ||
      out.layerCount > layerNum - out.baseLayer)
    return false;
  out.wholeResource = out.baseMip == 0 && out.mipCount == mipNum &&
                      out.baseLayer == 0 && out.layerCount == layerNum;
  return true;
}

// A rejected range means the barrier is dropped outright, which on D3D12 shows
// up only as corrupt output much later. RITextureBarrier defaults its aspect to
// COLOR, so a call site that forgets the aspect on a depth resource lands here
// and would otherwise be invisible. Reported once — these repeat every frame.
static void ri_d3d12_WarnBarrierDropped(const RITextureBarrier &src) {
  static bool sbWarned = false;
  if (sbWarned)
    return;
  sbWarned = true;
  hpl::Warning("RI D3D12: dropped a texture barrier (aspect %u, format %u, "
               "baseMip %u mipCount %u baseLayer %u layerCount %u); the "
               "resource will not be transitioned\n",
               unsigned(src.aspect), unsigned(src.texture->format),
               src.baseMip, src.mipCount, src.baseLayer, src.layerCount);
}

void RID3D12_ResourceBarrier(RICmd &cmd, uint32_t memoryBarrierNum,
                             const RIMemoryBarrier *memoryBarriers,
                             uint32_t bufferBarrierNum,
                             const RIBufferBarrier *bufferBarriers,
                             uint32_t textureBarrierNum,
                             const RITextureBarrier *textureBarriers) {
  if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
    return;
  if (cmd.d3d12.enhancedBarriersSupported && cmd.d3d12.cmdList7) {
    std::vector<D3D12_GLOBAL_BARRIER> globals(memoryBarrierNum);
    std::vector<D3D12_BUFFER_BARRIER> buffers(bufferBarrierNum);
    std::vector<D3D12_TEXTURE_BARRIER> textures(textureBarrierNum);
    uint32_t textureBarrierCount = 0;
    for (uint32_t i = 0; i < memoryBarrierNum; ++i) {
      const RIMemoryBarrier &src = memoryBarriers[i];
      globals[i] = {ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, src.before),
                    ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.before),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.after)};
    }
    for (uint32_t i = 0; i < bufferBarrierNum; ++i) {
      const RIBufferBarrier &src = bufferBarriers[i];
      buffers[i] = {ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, src.before),
                    ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.before),
                    ri_d3d12_RIResourceStateToBarrierAccess(src.after),
                     src.buffer->d3d12.resource, src.offset,
                     src.size ? src.size : UINT64_MAX};
    }
    for (uint32_t i = 0; i < textureBarrierNum; ++i) {
      const RITextureBarrier &src = textureBarriers[i];
      RID3D12BarrierSubresourceRange range = {};
      if (!ri_d3d12_GetBarrierSubresourceRange(src, range)) {
        ri_d3d12_WarnBarrierDropped(src);
        continue;
      }
      D3D12_TEXTURE_BARRIER &dst = textures[textureBarrierCount++] = {};
      dst.SyncBefore = ri_d3d12_RIStageBitsToBarrierSync(src.beforeStages, src.before);
      dst.SyncAfter = ri_d3d12_RIStageBitsToBarrierSync(src.afterStages, src.after);
      dst.AccessBefore = ri_d3d12_RIResourceStateToBarrierAccess(src.before);
      dst.AccessAfter = ri_d3d12_RIResourceStateToBarrierAccess(src.after);
      dst.LayoutBefore = ri_d3d12_RIResourceStateToBarrierLayout(src.before);
      dst.LayoutAfter = ri_d3d12_RIResourceStateToBarrierLayout(src.after);
      dst.pResource = src.texture->d3d12.resource;
      dst.Subresources.IndexOrFirstMipLevel = range.baseMip;
      dst.Subresources.NumMipLevels = range.mipCount;
      dst.Subresources.FirstArraySlice = range.baseLayer;
      dst.Subresources.NumArraySlices = range.layerCount;
      dst.Subresources.FirstPlane = range.firstPlane;
      dst.Subresources.NumPlanes = range.planeCount;
      dst.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    }
    textures.resize(textureBarrierCount);
    D3D12_BARRIER_GROUP groups[3] = {};
    UINT groupCount = 0;
    if (!globals.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_GLOBAL;
      groups[groupCount].NumBarriers = (UINT)globals.size();
      groups[groupCount].pGlobalBarriers = globals.data();
      ++groupCount;
    }
    if (!buffers.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_BUFFER;
      groups[groupCount].NumBarriers = (UINT)buffers.size();
      groups[groupCount].pBufferBarriers = buffers.data();
      ++groupCount;
    }
    if (!textures.empty()) {
      groups[groupCount].Type = D3D12_BARRIER_TYPE_TEXTURE;
      groups[groupCount].NumBarriers = (UINT)textures.size();
      groups[groupCount].pTextureBarriers = textures.data();
      ++groupCount;
    }
    if (groupCount) {
      cmd.d3d12.cmdList7->Barrier(groupCount, groups);
      ++g_riD3D12EnhancedBarrierCallCount;
    }
    return;
  }

  size_t barrierBudget = memoryBarrierNum + bufferBarrierNum;
  for (uint32_t i = 0; i < textureBarrierNum; ++i) {
    const RITextureBarrier &src = textureBarriers[i];
    RID3D12BarrierSubresourceRange range = {};
    if (!ri_d3d12_GetBarrierSubresourceRange(src, range))
      continue;
    barrierBudget += (range.wholeResource &&
                      range.firstPlane == 0 &&
                      range.planeCount == range.resourcePlaneCount) ? 1 :
                     static_cast<size_t>(range.mipCount) * range.layerCount *
                         range.planeCount;
  }
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  barriers.reserve(barrierBudget);
  for (uint32_t i = 0; i < memoryBarrierNum; ++i)
    barriers.push_back({D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE,
                        {nullptr}});
  for (uint32_t i = 0; i < bufferBarrierNum; ++i) {
    const RIBufferBarrier &src = bufferBarriers[i];
    const D3D12_RESOURCE_STATES before = ri_d3d12_RIResourceStateToStates(src.before);
    const D3D12_RESOURCE_STATES after = ri_d3d12_RIResourceStateToStates(src.after);
    D3D12_RESOURCE_BARRIER dst = {};
    if (before == after) { dst.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; dst.UAV.pResource = src.buffer->d3d12.resource; }
    else { dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; dst.Transition = {src.buffer->d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; }
    barriers.push_back(dst);
  }
  for (uint32_t i = 0; i < textureBarrierNum; ++i) {
    const RITextureBarrier &src = textureBarriers[i];
    RID3D12BarrierSubresourceRange range = {};
    if (!ri_d3d12_GetBarrierSubresourceRange(src, range)) {
      ri_d3d12_WarnBarrierDropped(src);
      continue;
    }
    const D3D12_RESOURCE_STATES before = ri_d3d12_RIResourceStateToStates(src.before);
    const D3D12_RESOURCE_STATES after = ri_d3d12_RIResourceStateToStates(src.after);
    if (before == after) { D3D12_RESOURCE_BARRIER dst = {}; dst.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; dst.UAV.pResource = src.texture->d3d12.resource; barriers.push_back(dst); continue; }
    const uint32_t mipNum = src.texture->d3d12.mipNum;
    const uint32_t layerNum = src.texture->d3d12.layerNum;
    if (range.wholeResource && range.firstPlane == 0 &&
        range.planeCount == range.resourcePlaneCount) {
      D3D12_RESOURCE_BARRIER dst = {};
      dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      dst.Transition = {src.texture->d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
      barriers.push_back(dst);
    } else {
      for (uint32_t plane = 0; plane < range.planeCount; ++plane)
        for (uint32_t layer = 0; layer < range.layerCount; ++layer)
          for (uint32_t mip = 0; mip < range.mipCount; ++mip) {
            D3D12_RESOURCE_BARRIER dst = {};
            dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            dst.Transition = {src.texture->d3d12.resource,
                              range.baseMip + mip + (range.baseLayer + layer) * mipNum +
                                  (range.firstPlane + plane) * mipNum * layerNum,
                              before, after};
            barriers.push_back(dst);
          }
    }
  }
  if (!barriers.empty()) {
    cmd.d3d12.cmdList->ResourceBarrier((UINT)barriers.size(), barriers.data());
    ++g_riD3D12LegacyBarrierCallCount;
  }
}

#endif
