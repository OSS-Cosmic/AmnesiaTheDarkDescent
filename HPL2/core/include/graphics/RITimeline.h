#ifndef HPL_RI_TIMELINE_H
#define HPL_RI_TIMELINE_H

#include "graphics/RIPreamble.h"

#include <cstdint>

struct RIDevice;

namespace hpl {

struct RITimeline {
  void init(struct RIDevice *device);
  void dispose(struct RIDevice *device);
  uint64_t next() { return ++signalValue; }
  uint64_t pending() const { return signalValue; }
  uint64_t completed(struct RIDevice *device) const;
  void wait(struct RIDevice *device, uint64_t value) const;

  uint64_t signalValue = 0;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSemaphore semaphore;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12Fence *fence; // owned; init creates, dispose releases
      HANDLE event;       // owned; manual-reset Win32 event for wait()
    } d3d12;
#endif
  };
};

} // namespace hpl

#endif // HPL_RI_TIMELINE_H
