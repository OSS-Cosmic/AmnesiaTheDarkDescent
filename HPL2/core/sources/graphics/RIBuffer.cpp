#include "graphics/RITypes.h"
#include "graphics/RIRenderer.h" // VK_ConfigureBufferQueueFamilies, RI_QUEUE_LEN
#include "graphics/RIVK.h"       // ri_vk_RIBufferUsageToVK
#include "graphics/RID3D12.h"
#include <cassert>

void RIBuffer::setDebugObjectName(struct RIDevice *device,
                                  const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vkSetDebugUtilsObjectNameEXT && vk.buffer) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_BUFFER, (uint64_t)vk.buffer, name};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
    }
    // Name the allocation too, not just the VkBuffer. VMA copies the string.
    if (vk.allocation && name)
      vmaSetAllocationName(device->vk.vmaAllocator, vk.allocation, name);
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    RID3D12_SetBufferDebugName(*device, *this, name);
    return;
  }
#endif
  assert(false && "unhandled backend");
}
void RIBuffer::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.buffer)
      vkDestroyBuffer(device->vk.device, vk.buffer, NULL);
    if (vk.allocation)
      vmaFreeMemory(device->vk.vmaAllocator, vk.allocation);
    vk.buffer = VK_NULL_HANDLE;
    vk.allocation = nullptr;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    RID3D12_DisposeBuffer(*device, *this);
#endif
  mappedAddress = nullptr;
  cookie = 0;
}

struct RIBuffer RIBuffer::create(struct RIDevice *device,
                                 const struct RIBufferDesc &desc,
                                 std::optional<hash_t> hash) {
  RIBuffer buf = {};
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    // Stamp the cookie before backend registration so the D3D12 registry can
    // deduplicate by the caller's stable identity plus native resource.
    buf.cookie = hash.value_or(hash_random());
    if (RID3D12_CreateBuffer(*device, desc, buf) != RI_SUCCESS)
      return RIBuffer{};
    return buf;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VK_ConfigureBufferQueueFamilies(&bci, device->queues, RI_QUEUE_LEN,
                                    queueFamilies, RI_QUEUE_LEN);
    bci.size = desc.size;
    bci.usage = ri_vk_RIBufferUsageToVK(desc.usage);

    VmaAllocationCreateInfo aci = {};
    if (desc.location == RI_MEMORY_HOST_UPLOAD) {
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else if (desc.location == RI_MEMORY_HOST_READBACK) {
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    } else {
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo allocationInfo = {};
    bool created = false;
    if (desc.alignment > 0) {
      created = VK_WrapResult(vmaCreateBufferWithAlignment(
          device->vk.vmaAllocator, &bci, &aci, desc.alignment, &buf.vk.buffer,
          &buf.vk.allocation, &allocationInfo));
    } else {
      created = VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci,
                                              &aci, &buf.vk.buffer,
                                              &buf.vk.allocation,
                                              &allocationInfo));
    }
    if (!created)
      return RIBuffer{};
    buf.mappedAddress = (desc.location == RI_MEMORY_HOST_UPLOAD ||
                         desc.location == RI_MEMORY_HOST_READBACK)
                            ? allocationInfo.pMappedData
                            : nullptr;
    buf.cookie = hash.value_or(hash_random());
    return buf;
  }
#endif
  (void)device;
  (void)desc;
  assert(false && "unhandled backend");
  return RIBuffer{};
}

uint64_t RIBuffer::GetDeviceHandle(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (!vk.buffer)
      return 0;
    VkBufferDeviceAddressInfo info = {
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = vk.buffer;
    return vkGetBufferDeviceAddress(device->vk.device, &info);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return RID3D12_BufferGpuAddress(*device, *this);
#endif
  assert(false && "unhandled backend");
  return 0;
}

uint64_t RIBuffer::GetShaderResourceHandle(struct RIDevice *device) const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    // Preserve the existing Vulkan BDA geometry path exactly. The const
    // façade is safe because vkGetBufferDeviceAddress does not mutate the
    // RIBuffer; GetDeviceHandle owns the backend implementation.
    return const_cast<RIBuffer *>(this)->GetDeviceHandle(device);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!d3d12.resource || d3d12.shaderResourceIndex == UINT32_MAX)
      return 0;
    // Zero is the missing-stream sentinel; the shader subtracts one from the
    // upper word when decoding the raw SRV index.
    return (uint64_t(d3d12.shaderResourceIndex) + 1ull) << 32;
  }
#endif
  assert(false && "unhandled backend");
  return 0;
}

void RIBuffer::flushMappedRange(struct RIDevice *device, uint64_t offset,
                                uint64_t size) {
  if (isEmpty() || mappedAddress == nullptr)
    return;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vmaFlushAllocation(device->vk.vmaAllocator, vk.allocation, offset,
                       size == 0 ? VK_WHOLE_SIZE : size);
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    RID3D12_FlushBufferRange(*device, *this, offset, size);
#endif
}

void RIBuffer::invalidateMappedRange(struct RIDevice *device, uint64_t offset,
                                     uint64_t size) {
  if (isEmpty() || mappedAddress == nullptr)
    return;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vmaInvalidateAllocation(device->vk.vmaAllocator, vk.allocation, offset,
                            size == 0 ? VK_WHOLE_SIZE : size);
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    RID3D12_InvalidateBufferRange(*device, *this, offset, size);
#endif
}
