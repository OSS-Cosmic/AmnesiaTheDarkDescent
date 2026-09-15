#ifndef RI_RENDERER_H
#define RI_RENDERER_H

#include "graphics/RITypes.h"
#include <cassert>
#include <stdint.h>

struct RIDeviceDesc {
  struct RIPhysicalAdapter *physicalAdapter;
  // Requests only; what the device actually enabled is published on RIDevice
  // (rayTracingEnabled, accelerationStructureEnabled, ...).
  uint32_t requestRayTracing : 1;
};

#if DEVICE_IMPL_VULKAN
void VK_ConfigureBufferQueueFamilies( VkBufferCreateInfo *info, struct RIQueue *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_ConfigureImageQueueFamilies( VkImageCreateInfo *info, struct RIQueue *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_FillQueueFamilies( struct RIDevice *dev, uint32_t *queueFamilies, uint32_t *queueFamiliesIdx, size_t reservedLen );
#endif

#endif
