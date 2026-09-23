#include "graphics/RITimeline.h"
#include "graphics/RIDevice.h"
#if (DEVICE_IMPL_D3D12)
#include "graphics/RID3D12.h"
#endif
#include "system/LowLevelSystem.h"

namespace hpl {

void RITimeline::init(RIDevice *device) {
  signalValue = 0;
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    d3d12.fence = nullptr;
    d3d12.event = nullptr;
    if (!D3D12_WrapResult(device->d3d12.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d12.fence)))) {
      d3d12.fence = nullptr;
      return;
    }
    d3d12.event = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET,
                                EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!d3d12.event) {
      hpl::Log("RI: failed to create D3D12 timeline event\n");
      d3d12.fence->Release();
      d3d12.fence = nullptr;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    vk.semaphore = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo timelineInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &timelineInfo;
    VK_WrapResult(
        vkCreateSemaphore(device->vk.device, &createInfo, NULL, &vk.semaphore));
    return;
  }
#endif
}

void RITimeline::dispose(RIDevice *device) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (d3d12.fence) {
      d3d12.fence->Release();
      d3d12.fence = nullptr;
    }
    if (d3d12.event) {
      CloseHandle(d3d12.event);
      d3d12.event = nullptr;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    if (vk.semaphore) {
      vkDestroySemaphore(device->vk.device, vk.semaphore, NULL);
      vk.semaphore = VK_NULL_HANDLE;
    }
  }
#else
  (void)device;
#endif
}

uint64_t RITimeline::completed(RIDevice *device) const {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    uint64_t value = d3d12.fence ? d3d12.fence->GetCompletedValue() : 0;
    if (value == UINT64_MAX)
      hpl::Warning("RI: D3D12 timeline detected device removal\n");
    return value;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    uint64_t value = 0;
    if (!VK_WrapResult(vkGetSemaphoreCounterValue(device->vk.device,
                                                  vk.semaphore, &value)))
      return 0;
    return value;
  }
#endif
  (void)device;
  return 0;
}

void RITimeline::wait(RIDevice *device, uint64_t value) const {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!d3d12.fence || d3d12.fence->GetCompletedValue() >= value)
      return;
    if (!D3D12_WrapResult(
            d3d12.fence->SetEventOnCompletion(value, d3d12.event))) {
      hpl::Warning("RI: failed to set D3D12 timeline wait event\n");
      return;
    }
    WaitForSingleObject(d3d12.event, INFINITE);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkSemaphoreWaitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &vk.semaphore;
    waitInfo.pValues = &value;
    VK_WrapResult(vkWaitSemaphores(device->vk.device, &waitInfo, UINT64_MAX));
    return;
  }
#endif
  (void)device;
  (void)value;
}

} // namespace hpl
