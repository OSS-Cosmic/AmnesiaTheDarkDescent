#ifndef RI_RENDERER_H
#define RI_RENDERER_H

#include "graphics/RITypes.h"
#include <cassert>
#include <stdint.h>

struct RIDeviceDesc {
  struct RIPhysicalAdapter *physicalAdapter;
  // Requests only; what the device actually enabled is published on RIDevice
  // (rayTracingEnabled, accelerationStructureEnabled, ...).
  //
  // Only genuinely optional capabilities are requestable. Everything a given
  // renderer always needs -- swapchain, bindless descriptors, buffer device
  // addresses, scalar block layout, 64-bit integers, dynamic rendering -- has
  // no flag: RIDevice::init builds whatever the adapter can give, so checking
  // an adapter against a renderer's fixed requirements is the caller's job
  // (see cGraphics::Init).
  //
  // Acceleration structures and ray-tracing pipelines; DXR tier 1.
  uint32_t requestRayTracing : 1;
  // Inline ray query in any shader stage; DXR tier 2 / VK_KHR_ray_query.
  // Implies requestRayTracing, which provides the structures it traces.
  uint32_t requestRayQuery : 1;
};

#if DEVICE_IMPL_VULKAN
void VK_ConfigureBufferQueueFamilies( VkBufferCreateInfo *info, struct RIQueue *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_ConfigureImageQueueFamilies( VkImageCreateInfo *info, struct RIQueue *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_FillQueueFamilies( struct RIDevice *dev, uint32_t *queueFamilies, uint32_t *queueFamiliesIdx, size_t reservedLen );
#endif

#endif
