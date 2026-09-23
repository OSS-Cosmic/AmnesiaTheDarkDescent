#include "graphics/RIGpuProfiler.h"

#include "graphics/RITypes.h"

#include <cfloat>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <iterator>
#include <vector>

namespace hpl {

#if (DEVICE_IMPL_VULKAN)
static constexpr VkPipelineStageFlags2 kTimestampStage =
    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
#endif

void RIGpuProfiler::init(struct RIDevice *device) {
  // dispose() is tolerant of a null or partially initialized device.
  dispose(device);
  m_enabled = false;
  if (!device)
    return;

#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && device->vk.device &&
      device->physicalAdapter.vk.physicalDevice) {
    const uint64_t freq = device->physicalAdapter.timestampFrequencyHz;
    if (freq == 0)
      return;
    VkPhysicalDevice phys = device->physicalAdapter.vk.physicalDevice;
    const uint32_t qfIdx = device->queues[RI_QUEUE_GRAPHICS].vk.queueFamilyIdx;
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfProps.data());
    const uint32_t validBits =
        (qfIdx < qfCount) ? qfProps[qfIdx].timestampValidBits : 0;
    if (validBits == 0)
      return;
    m_validBitsMask = (validBits >= 64) ? ~0ull : ((1ull << validBits) - 1ull);
    m_ticksToMs = 1000.0 / (double)freq;
    for (auto &slot : m_slots) {
      VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      ci.queryCount = kMaxQueries;
      if (!VK_WrapResult(vkCreateQueryPool(device->vk.device, &ci, nullptr,
                                           &slot.pool))) {
        for (auto &s : m_slots) {
          if (s.pool) {
            vkDestroyQueryPool(device->vk.device, s.pool, nullptr);
            s.pool = VK_NULL_HANDLE;
          }
        }
        return;
      }
      slot.timelineValue = 0;
      slot.resolved = true;
      slot.queryCount = 0;
      slot.scopes.clear();
    }
    m_enabled = true;
    return;
  }
#endif

#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12) &&
      device->d3d12.device && device->d3d12.queues[RI_QUEUE_GRAPHICS]) {
    UINT64 freq = 0;
    if (!D3D12_WrapResult(device->d3d12.queues[RI_QUEUE_GRAPHICS]
                              ->GetTimestampFrequency(&freq)) ||
        freq == 0)
      return;
    m_ticksToMs = 1000.0 / (double)freq;
    auto failInit = [&]() {
      for (auto &s : m_slots) {
        if (!s.readback.isEmpty()) {
          s.readback.dispose(device);
          s.readback = {};
        }
        s.mapped = nullptr;
        if (s.queryHeap) {
          s.queryHeap->Release();
          s.queryHeap = nullptr;
        }
        s.timelineValue = 0;
        s.resolved = true;
        s.queryCount = 0;
        s.scopes.clear();
      }
    };
    const UINT64 readbackSize = sizeof(uint64_t) * kMaxQueries;
    for (auto &slot : m_slots) {
      const D3D12_QUERY_HEAP_DESC queryDesc = {
          D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMaxQueries, 0};
      if (!D3D12_WrapResult(device->d3d12.device->CreateQueryHeap(
              &queryDesc, IID_PPV_ARGS(&slot.queryHeap)))) {
        failInit();
        return;
      }
      slot.readback = RIBuffer::create(
          device, {readbackSize, RI_BUFFER_USAGE_TRANSFER_DST,
                   RI_MEMORY_HOST_READBACK, 0});
      if (slot.readback.isEmpty() || !slot.readback.mappedAddress) {
        failInit();
        return;
      }
      slot.mapped = static_cast<uint64_t *>(slot.readback.mappedAddress);
      slot.timelineValue = 0;
      slot.resolved = true;
      slot.queryCount = 0;
      slot.scopes.clear();
    }
    m_enabled = true;
  }
#endif
}

void RIGpuProfiler::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    for (auto &slot : m_slots) {
      if (slot.pool && device && device->vk.device) {
        vkDestroyQueryPool(device->vk.device, slot.pool, nullptr);
        slot.pool = VK_NULL_HANDLE;
      }
      slot.scopes.clear();
    }
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    for (auto &slot : m_slots) {
      if (!slot.readback.isEmpty() && device) {
        slot.readback.dispose(device);
        slot.readback = {};
        slot.mapped = nullptr;
      }
      if (slot.queryHeap) {
        slot.queryHeap->Release();
        slot.queryHeap = nullptr;
      }
      slot.scopes.clear();
      slot.timelineValue = 0;
      slot.resolved = true;
      slot.queryCount = 0;
    }
  }
#else
  (void)device;
#endif
  m_enabled = false;
  m_openStack.clear();
  m_openLabelCount = 0;
}

void RIGpuProfiler::beginFrame(struct RICmd *cmd, uint32_t slot,
                               uint64_t timelineValue) {
  m_openStack.clear();
  if (!m_enabled || slot >= m_slots.size())
    return;
  m_activeSlot = slot;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    Slot &s = m_slots[slot];
    if (!cmd || !cmd->vk.cmd || !s.pool)
      return;
    vkCmdResetQueryPool(cmd->vk.cmd, s.pool, 0, kMaxQueries);
    s.queryCount = 0;
    s.scopes.clear();
    s.timelineValue = timelineValue;
    s.resolved = false;
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    Slot &s = m_slots[slot];
    s.queryCount = 0;
    s.scopes.clear();
    s.timelineValue = timelineValue;
    s.resolved = false;
  }
#endif
}

void RIGpuProfiler::endFrame(struct RICmd *cmd, bool willSubmit) {
  if (!willSubmit) {
    if (m_enabled) {
      Slot &s = m_slots[m_activeSlot];
      s.timelineValue = 0;
      s.resolved = true;
    }
    return;
  }
#if (DEVICE_IMPL_D3D12)
  if (m_enabled && RIIsTargetSelected(RI_DEVICE_API_D3D12) && cmd &&
      cmd->d3d12.cmdList) {
    Slot &s = m_slots[m_activeSlot];
    if (s.queryCount != 0)
      cmd->d3d12.cmdList->ResolveQueryData(
          s.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, s.queryCount,
          s.readback.d3d12.resource, 0);
  }
#else
  (void)cmd;
#endif
}

void RIGpuProfiler::beginScope(struct RICmd *cmd, const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && vkCmdBeginDebugUtilsLabelEXT &&
      cmd && cmd->vk.cmd) {
    VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name ? name : "";
    vkCmdBeginDebugUtilsLabelEXT(cmd->vk.cmd, &label);
    ++m_openLabelCount;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12) && cmd && cmd->d3d12.cmdList) {
    // Metadata 0 = WINPIX_EVENT_UNICODE_VERSION with a wchar_t payload. DRED
    // records breadcrumb context strings only for Unicode events, so this is
    // what lets a device-removal dump name the scope that hung.
    wchar_t label[128] = {};
    const int length = name ? MultiByteToWideChar(CP_UTF8, 0, name, -1, label,
                                                  int(std::size(label)))
                            : 0;
    if (length <= 0)
      label[0] = L'\0';
    label[std::size(label) - 1] = L'\0';
    constexpr UINT kPixEventUnicodeVersion = 0;
    cmd->d3d12.cmdList->BeginEvent(
        kPixEventUnicodeVersion, label,
        (UINT)((std::wcslen(label) + 1) * sizeof(wchar_t)));
    ++m_openLabelCount;
  }
#endif
  if (!m_enabled)
    return;
  Slot &s = m_slots[m_activeSlot];
  const uint32_t depth = (uint32_t)m_openStack.size();
  uint32_t beginIdx = UINT32_MAX;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && cmd && cmd->vk.cmd && s.pool &&
      s.queryCount < kMaxQueries) {
    beginIdx = s.queryCount++;
    vkCmdWriteTimestamp2(cmd->vk.cmd, kTimestampStage, s.pool, beginIdx);
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12) && cmd && cmd->d3d12.cmdList &&
      s.queryCount < kMaxQueries) {
    beginIdx = s.queryCount++;
    cmd->d3d12.cmdList->EndQuery(s.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                                 beginIdx);
  }
#endif
  s.scopes.push_back(
      Scope{std::string(name ? name : ""), depth, beginIdx, UINT32_MAX});
  m_openStack.push_back((uint32_t)(s.scopes.size() - 1));
}

void RIGpuProfiler::endScope(struct RICmd *cmd) {
  if (m_enabled && !m_openStack.empty()) {
    Slot &s = m_slots[m_activeSlot];
    const uint32_t idx = m_openStack.back();
    m_openStack.pop_back();
    Scope &sc = s.scopes[idx];
#if (DEVICE_IMPL_VULKAN)
    if (RIIsTargetSelected(RI_DEVICE_API_VK) && cmd && cmd->vk.cmd && s.pool &&
        sc.beginIdx != UINT32_MAX && s.queryCount < kMaxQueries) {
      sc.endIdx = s.queryCount++;
      vkCmdWriteTimestamp2(cmd->vk.cmd, kTimestampStage, s.pool, sc.endIdx);
    }
#endif
#if (DEVICE_IMPL_D3D12)
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12) && cmd && cmd->d3d12.cmdList &&
        sc.beginIdx != UINT32_MAX && s.queryCount < kMaxQueries) {
      sc.endIdx = s.queryCount++;
      cmd->d3d12.cmdList->EndQuery(s.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                                   sc.endIdx);
    }
#endif
  }
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && vkCmdEndDebugUtilsLabelEXT &&
      cmd && cmd->vk.cmd && m_openLabelCount != 0) {
    vkCmdEndDebugUtilsLabelEXT(cmd->vk.cmd);
    --m_openLabelCount;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12) && cmd && cmd->d3d12.cmdList &&
      m_openLabelCount != 0) {
    cmd->d3d12.cmdList->EndEvent();
    --m_openLabelCount;
  }
#endif
}

void RIGpuProfiler::resolve(struct RIDevice *device,
                            uint64_t completedTimeline) {
  if (!m_enabled)
    return;
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK) && device && device->vk.device) {
    uint64_t bestValue = 0;
    for (auto &slot : m_slots) {
      if (slot.resolved || slot.timelineValue == 0 ||
          slot.timelineValue > completedTimeline)
        continue;
      slot.resolved = true;
      if (slot.queryCount == 0)
        continue;
      uint64_t data[kMaxQueries] = {};
      const VkResult r = vkGetQueryPoolResults(
          device->vk.device, slot.pool, 0, slot.queryCount,
          sizeof(uint64_t) * slot.queryCount, data, sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT);
      if (r != VK_SUCCESS || slot.timelineValue < bestValue)
        continue;
      bestValue = slot.timelineValue;
      m_lastResults.clear();
      m_lastResults.reserve(slot.scopes.size());
      float total = 0.0f;
      for (const Scope &sc : slot.scopes) {
        float ms = 0.0f;
        if (sc.beginIdx != UINT32_MAX && sc.endIdx != UINT32_MAX) {
          const uint64_t b = data[sc.beginIdx] & m_validBitsMask;
          const uint64_t e = data[sc.endIdx] & m_validBitsMask;
          if (e >= b) {
            const double elapsed = (double)(e - b) * m_ticksToMs;
            if (std::isfinite(elapsed) && elapsed <= (double)FLT_MAX)
              ms = (float)elapsed;
          }
        }
        m_lastResults.push_back(GpuPassTiming{sc.name, ms, sc.depth});
        if (sc.depth == 0)
          total += ms;
      }
      m_lastTotalMs = total;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    uint64_t bestValue = 0;
    for (auto &slot : m_slots) {
      if (slot.resolved || slot.timelineValue == 0 ||
          slot.timelineValue > completedTimeline)
        continue;
      slot.resolved = true;
      if (slot.queryCount == 0 || !slot.mapped || slot.timelineValue < bestValue)
        continue;
      bestValue = slot.timelineValue;
      m_lastResults.clear();
      m_lastResults.reserve(slot.scopes.size());
      float total = 0.0f;
      for (const Scope &sc : slot.scopes) {
        float ms = 0.0f;
        if (sc.beginIdx != UINT32_MAX && sc.endIdx != UINT32_MAX) {
          const uint64_t b = slot.mapped[sc.beginIdx];
          const uint64_t e = slot.mapped[sc.endIdx];
          if (e >= b) {
            const double elapsed = (double)(e - b) * m_ticksToMs;
            if (std::isfinite(elapsed) && elapsed <= (double)FLT_MAX)
              ms = (float)elapsed;
          }
        }
        m_lastResults.push_back(GpuPassTiming{sc.name, ms, sc.depth});
        if (sc.depth == 0)
          total += ms;
      }
      m_lastTotalMs = total;
    }
  }
#else
  (void)device;
  (void)completedTimeline;
#endif
}

} // namespace hpl
