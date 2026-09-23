#ifndef RI_COMMAND_RING_BUFFER_H
#define RI_COMMAND_RING_BUFFER_H

// Depends on the command types (RICmd/RIPool/RIQueue) and the device (RIDevice +
// RIIsTargetSelected / VK_WrapResult) directly, not the umbrella — RITypes.h
// includes THIS header, so pulling the umbrella back in would be a cycle.
#include "graphics/RICommand.h"
#include "graphics/RIDevice.h"
#include <cassert>
#include <cstring>

// Compile-time maximums; runtime poolCount/cmdPerPool are passed to
// RICommandRingBuffer::init.
#define RI_COMMAND_RING_POOL_COUNT 8
#define RI_COMMAND_RING_CMD_PER_POOL 32

struct RICommandRingElement {
  RICommandRingElement() { memset(this, 0, sizeof(*this)); }
  // Blocks until the element's fence signals (no-op without sync primitives).
  void wait(struct RIDevice *device);
  struct RICmd *cmds;
  uint32_t numCmds;
  struct RIPool *pool;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSemaphore semaphore;
      VkFence fence;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      // Borrowed from RIQueue::d3d12.fence — captured on a successful submit
      // shared by every element in the pool. The monotonic value covers every
      // successful submit using that pool before it is reset. Value 0 means
      // the pool has not been submitted yet, so wait() no-ops.
      // The ring buffer never releases this pointer.
      ID3D12Fence *fence;
      uint64_t value;
      ID3D12Fence **backingFence;
      uint64_t *backingValue;
    } d3d12;
#endif
  };
};

template <uint32_t MaxPoolCount = RI_COMMAND_RING_POOL_COUNT,
          uint32_t CmdPerPool = RI_COMMAND_RING_CMD_PER_POOL>
struct RICommandRingBuffer {
  RICommandRingBuffer() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_POOL_COUNT = MaxPoolCount;
  static constexpr uint32_t CMD_PER_POOL = CmdPerPool;

  // Defined at the bottom of this header once RIDevice is complete
  // (same precedent as RISwapchain::dispose).
  void init(struct RIDevice *device, struct RIQueue *queue, uint32_t poolCount,
            uint32_t cmdPerPool, bool syncPrimitives);
  void dispose(struct RIDevice *device);
  // Rotates to the next pool and rewinds the command/Vulkan sync cursors.
  void advance();
  // Claims numCmds command buffers from the current pool and advances the
  // cursors. Vulkan also advances its per-acquisition synchronization slot.
  struct RICommandRingElement acquire(struct RIDevice *device,
                                      uint32_t numCmds);

  uint32_t poolIndex;
  uint32_t cmdIndex;
  uint32_t fenceIndex;

  uint32_t poolCount;
  uint32_t cmdPerPool;
  bool syncPrimitive;

  struct RIPool pools[MaxPoolCount];
  struct RICmd cmds[MaxPoolCount][CmdPerPool];

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkFence fences[MaxPoolCount][CmdPerPool];
      VkSemaphore semaphores[MaxPoolCount][CmdPerPool];
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      // One borrowed monotonic completion token per pool. Each acquired
      // element in a pool binds to the same token so its wait covers all
      // successful submissions made through that pool before reset.
      ID3D12Fence *fences[MaxPoolCount];
      uint64_t values[MaxPoolCount];
    } d3d12;
#endif
  };
};

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer<MaxPoolCount, CmdPerPool>::init(
    struct RIDevice *device, struct RIQueue *queue, uint32_t poolCount,
    uint32_t cmdPerPool, bool syncPrimitives) {
  assert(poolCount > 0 && poolCount <= MaxPoolCount);
  assert(cmdPerPool > 0 && cmdPerPool <= CmdPerPool);
  memset(this, 0, sizeof(*this));
  this->poolCount = poolCount;
  this->cmdPerPool = cmdPerPool;
  // DX12 borrows the queue's single monotonic fence; syncPrimitives is a no-op.
  this->syncPrimitive = syncPrimitives;

  poolIndex = 0;
  cmdIndex = 0;
  fenceIndex = 0;

  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    pools[poolIdx].init(device, queue);
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].init(device, &pools[poolIdx]);
#if (DEVICE_IMPL_VULKAN)
      if (RIIsTargetSelected(RI_DEVICE_API_VK) && syncPrimitives) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_WrapResult(vkCreateSemaphore(device->vk.device, &semaphoreCreateInfo,
                                        NULL, &vk.semaphores[poolIdx][cmdIdx]));

        VkFenceCreateInfo fenceCreateInfo = {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_WrapResult(vkCreateFence(device->vk.device, &fenceCreateInfo, NULL,
                                    &vk.fences[poolIdx][cmdIdx]));
      }
#endif
    }
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer<MaxPoolCount, CmdPerPool>::dispose(
    struct RIDevice *device) {
  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].dispose(device);
#if (DEVICE_IMPL_VULKAN)
      if (RIIsTargetSelected(RI_DEVICE_API_VK) &&
          syncPrimitive) {
        vkDestroySemaphore(device->vk.device, vk.semaphores[poolIdx][cmdIdx],
                           NULL);
        vkDestroyFence(device->vk.device, vk.fences[poolIdx][cmdIdx], NULL);
      }
#endif
    }
    pools[poolIdx].dispose(device);
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer<MaxPoolCount, CmdPerPool>::advance() {
  poolIndex = (poolIndex + 1) % poolCount;
  cmdIndex = 0;
  fenceIndex = 0;
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline struct RICommandRingElement
RICommandRingBuffer<MaxPoolCount, CmdPerPool>::acquire(struct RIDevice *device,
                                                       uint32_t numCmds) {
  struct RICommandRingElement result;
  memset(&result, 0, sizeof(struct RICommandRingElement));

  assert(numCmds <= cmdPerPool);
  assert(numCmds + cmdIndex <= cmdPerPool);

  result.cmds = &cmds[poolIndex][cmdIndex];
  result.numCmds = numCmds;
  result.pool = &pools[poolIndex];
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && syncPrimitive) {
    result.vk.semaphore = vk.semaphores[poolIndex][fenceIndex];
    result.vk.fence = vk.fences[poolIndex][fenceIndex];
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    result.d3d12.backingFence = &d3d12.fences[poolIndex];
    result.d3d12.backingValue = &d3d12.values[poolIndex];
    result.d3d12.fence = *result.d3d12.backingFence;
    result.d3d12.value = *result.d3d12.backingValue;
  }
#endif

  fenceIndex += 1;
  cmdIndex += numCmds;

  return result;
}

#endif // RI_COMMAND_RING_BUFFER_H
