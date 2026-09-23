#include "graphics/RITypes.h"
#include "graphics/RID3D12.h"
#include <cassert>


void RIAccelStructure::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    RID3D12_DisposeAccelStructure(*device, *this);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.handle != VK_NULL_HANDLE) {
      vkDestroyAccelerationStructureKHR(device->vk.device, vk.handle, NULL);
      vk.handle = VK_NULL_HANDLE;
    }
  }
#endif
}

void RIAccelStructure::setDebugObjectName(struct RIDevice *device,
                                            const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vkSetDebugUtilsObjectNameEXT && vk.handle && name) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)vk.handle, name};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
    }
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    RID3D12_SetAccelStructureDebugName(*device, *this, name);
    return;
  }
#endif
  assert(false && "unhandled backend");
}


