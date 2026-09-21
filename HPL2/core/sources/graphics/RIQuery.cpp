#include "graphics/RIQuery.h"

#include "graphics/RITypes.h"

#include <cassert>
#include <cstring>

bool RIQueryPool::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.pool == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return d3d12.heap == nullptr;
#endif
  return true;
}

bool RIQueryPool::init(struct RIDevice *device,
                       const struct RIQueryPoolDesc &desc) {
  // dispose() is tolerant of a null or partially initialized device.
  dispose(device);
  if (!device || desc.queryCount == 0)
    return false;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (!device->vk.device)
      return false;
    // Precise counting is a device feature on Vulkan. Downgrade rather than
    // fail, and republish what was granted so callers read it back from type.
    const bool precise = desc.type == RI_QUERY_TYPE_OCCLUSION &&
                         device->occlusionQueryPreciseEnabled;
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_OCCLUSION;
    ci.queryCount = desc.queryCount;
    if (!VK_WrapResult(
            vkCreateQueryPool(device->vk.device, &ci, nullptr, &vk.pool))) {
      vk.pool = VK_NULL_HANDLE;
      return false;
    }
    queryCount = desc.queryCount;
    // The pool itself is always host-readable on Vulkan.
    resolvedCount = desc.queryCount;
    type = precise ? RI_QUERY_TYPE_OCCLUSION : RI_QUERY_TYPE_OCCLUSION_BINARY;
    return true;
  }
#endif

#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!device->d3d12.device)
      return false;
    // One heap type serves both occlusion variants; the variant is chosen per
    // BeginQuery/EndQuery/ResolveQueryData call instead.
    const D3D12_QUERY_HEAP_DESC heapDesc = {D3D12_QUERY_HEAP_TYPE_OCCLUSION,
                                            desc.queryCount, 0};
    if (!D3D12_WrapResult(device->d3d12.device->CreateQueryHeap(
            &heapDesc, IID_PPV_ARGS(&d3d12.heap)))) {
      d3d12.heap = nullptr;
      return false;
    }
    // A query heap cannot be read from the CPU; results must be resolved into
    // a buffer first. 8 bytes (UINT64) per occlusion query.
    d3d12.readback = RIBuffer::create(
        device, {sizeof(uint64_t) * (uint64_t)desc.queryCount,
                 RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_HOST_READBACK, 0});
    if (d3d12.readback.isEmpty() || !d3d12.readback.mappedAddress) {
      d3d12.readback.dispose(device);
      d3d12.readback = {};
      d3d12.heap->Release();
      d3d12.heap = nullptr;
      return false;
    }
    d3d12.mapped = static_cast<uint64_t *>(d3d12.readback.mappedAddress);
    queryCount = desc.queryCount;
    // Nothing has been resolved yet; getResults must not hand back stale memory.
    resolvedCount = 0;
    // D3D12_QUERY_TYPE_OCCLUSION always returns exact counts, no feature gate.
    type = (uint8_t)desc.type;
    return true;
  }
#endif

  // A backend with no occlusion-query support: the caller degrades.
  return false;
}

void RIQueryPool::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.pool && device && device->vk.device)
      vkDestroyQueryPool(device->vk.device, vk.pool, nullptr);
    vk.pool = VK_NULL_HANDLE;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!d3d12.readback.isEmpty() && device)
      d3d12.readback.dispose(device);
    d3d12.readback = {};
    d3d12.mapped = nullptr;
    if (d3d12.heap)
      d3d12.heap->Release();
    d3d12.heap = nullptr;
  }
#endif
  (void)device;
  queryCount = 0;
  resolvedCount = 0;
  type = RI_QUERY_TYPE_OCCLUSION_BINARY;
}

void RIQueryPool::setDebugObjectName(struct RIDevice *device,
                                     const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vkSetDebugUtilsObjectNameEXT && vk.pool && name && device) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_QUERY_POOL, (uint64_t)vk.pool, name};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
    }
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (d3d12.heap && name) {
      wchar_t wide[128] = {};
      const int written = MultiByteToWideChar(
          CP_UTF8, 0, name, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
      if (written > 0)
        d3d12.heap->SetName(wide);
    }
    if (!d3d12.readback.isEmpty() && device)
      d3d12.readback.setDebugObjectName(device, name);
    return;
  }
#endif
  (void)device;
  (void)name;
}

bool RIQueryPool::getResults(struct RIDevice *device, uint32_t first,
                             uint32_t count, uint64_t *results) {
  if (!device || !results || count == 0 || isEmpty())
    return false;
  // Guards the D3D12 memcpy and the Vulkan read alike; also catches a
  // first + count that wrapped.
  if (first > queryCount || count > queryCount - first)
    return false;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    // No WAIT bit: VK_NOT_READY is a valid "try again next frame", not an error.
    const VkResult r = vkGetQueryPoolResults(
        device->vk.device, vk.pool, first, count, sizeof(uint64_t) * count,
        results, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    return r == VK_SUCCESS;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    // Reject a range no resolve has filled -- the readback buffer would
    // otherwise hand back the previous frame's counts as if they were current.
    if (!d3d12.mapped || first + count > resolvedCount)
      return false;
    d3d12.readback.invalidateMappedRange(device, sizeof(uint64_t) * first,
                                         sizeof(uint64_t) * count);
    std::memcpy(results, d3d12.mapped + first, sizeof(uint64_t) * count);
    return true;
  }
#endif
  return false;
}
