#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RICommand.h"
#include "graphics/RIDescriptorSetAllocator.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cstdio>

void RID3D12_PoolInit(RIDevice &device, RIPool &pool, RIQueue &queue) {
  memset(&pool.d3d12, 0, sizeof(pool.d3d12));
  pool.d3d12.queue = queue.d3d12.queue;
  pool.d3d12.type = queue.d3d12.type;
  HRESULT hr = device.d3d12.device->CreateCommandAllocator(
      (D3D12_COMMAND_LIST_TYPE)pool.d3d12.type,
      IID_PPV_ARGS(&pool.d3d12.allocator));
  if (!D3D12_WrapResult(hr)) {
    memset(&pool.d3d12, 0, sizeof(pool.d3d12));
    return;
  }
}

void RID3D12_PoolReset(RIDevice &device, RIPool &pool) {
  (void)device;
  (void)pool;
  // Each D3D12 command list owns its allocator and resets it in CmdBegin.
  // A Vulkan-style pool can supply several simultaneously recording lists;
  // one shared D3D12 allocator cannot.
}

void RID3D12_PoolDispose(RIDevice &device, RIPool &pool) {
  (void)device;
  if (pool.d3d12.allocator) {
    pool.d3d12.allocator->Release();
    pool.d3d12.allocator = nullptr;
  }
  memset(&pool.d3d12, 0, sizeof(pool.d3d12));
}

void RID3D12_CmdInit(RIDevice &device, RICmd &cmd, RIPool &pool) {
  memset(&cmd.d3d12, 0, sizeof(cmd.d3d12));
  cmd.d3d12.device = &device;
  if (!pool.d3d12.allocator)
    return;
  HRESULT hr = device.d3d12.device->CreateCommandAllocator(
      (D3D12_COMMAND_LIST_TYPE)pool.d3d12.type,
      IID_PPV_ARGS(&cmd.d3d12.allocator));
  if (!D3D12_WrapResult(hr)) {
    memset(&cmd.d3d12, 0, sizeof(cmd.d3d12));
    return;
  }
  hr = device.d3d12.device->CreateCommandList(
      0, (D3D12_COMMAND_LIST_TYPE)pool.d3d12.type, cmd.d3d12.allocator, nullptr,
      IID_PPV_ARGS(&cmd.d3d12.cmdList));
  if (!D3D12_WrapResult(hr)) {
    cmd.d3d12.allocator->Release();
    cmd.d3d12.allocator = nullptr;
    cmd.d3d12.cmdList = nullptr;
    return;
  }
  if (!D3D12_WrapResult(
          cmd.d3d12.cmdList->QueryInterface(IID_PPV_ARGS(&cmd.d3d12.cmdList7))))
    cmd.d3d12.cmdList7 = nullptr;
  cmd.d3d12.enhancedBarriersSupported =
      device.physicalAdapter.isEnchancedBarrierSupported;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = RI_D3D12_RTV_DESCRIPTOR_CAPACITY;
  if (!D3D12_WrapResult(device.d3d12.device->CreateDescriptorHeap(
          &heapDesc, IID_PPV_ARGS(&cmd.d3d12.rtvHeap)))) {
    if (cmd.d3d12.cmdList7)
      cmd.d3d12.cmdList7->Release();
    cmd.d3d12.cmdList->Release();
    cmd.d3d12.cmdList = nullptr;
    cmd.d3d12.allocator->Release();
    cmd.d3d12.allocator = nullptr;
    memset(&cmd.d3d12, 0, sizeof(cmd.d3d12));
    return;
  }
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heapDesc.NumDescriptors = RI_D3D12_DSV_DESCRIPTOR_CAPACITY;
  if (!D3D12_WrapResult(device.d3d12.device->CreateDescriptorHeap(
          &heapDesc, IID_PPV_ARGS(&cmd.d3d12.dsvHeap)))) {
    if (cmd.d3d12.cmdList7)
      cmd.d3d12.cmdList7->Release();
    cmd.d3d12.rtvHeap->Release();
    cmd.d3d12.rtvHeap = nullptr;
    cmd.d3d12.cmdList->Release();
    cmd.d3d12.cmdList = nullptr;
    cmd.d3d12.allocator->Release();
    cmd.d3d12.allocator = nullptr;
    memset(&cmd.d3d12, 0, sizeof(cmd.d3d12));
    return;
  }
  // UAV-clear scratch: a CPU-only heap for ClearUnorderedAccessView*'s
  // ViewCPUHandle and a shader-visible one for its ViewGPUHandleInCurrentHeap.
  // Failure here is not fatal — clearStorageImage checks for the heaps and
  // reports a clear it cannot perform rather than taking the whole device down.
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = RI_D3D12_UAV_CLEAR_DESCRIPTOR_CAPACITY;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (D3D12_WrapResult(device.d3d12.device->CreateDescriptorHeap(
          &heapDesc, IID_PPV_ARGS(&cmd.d3d12.uavClearCpuHeap)))) {
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (D3D12_WrapResult(device.d3d12.device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&cmd.d3d12.uavClearGpuHeap)))) {
      cmd.d3d12.uavClearCpuStart =
          cmd.d3d12.uavClearCpuHeap->GetCPUDescriptorHandleForHeapStart();
      cmd.d3d12.uavClearGpuHeapCpuStart =
          cmd.d3d12.uavClearGpuHeap->GetCPUDescriptorHandleForHeapStart();
      cmd.d3d12.uavClearGpuStart =
          cmd.d3d12.uavClearGpuHeap->GetGPUDescriptorHandleForHeapStart();
      cmd.d3d12.uavClearDescriptorSize =
          device.d3d12.device->GetDescriptorHandleIncrementSize(
              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
      cmd.d3d12.uavClearCpuHeap->Release();
      cmd.d3d12.uavClearCpuHeap = nullptr;
    }
  }
  cmd.d3d12.rtvStart = cmd.d3d12.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  cmd.d3d12.dsvStart = cmd.d3d12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
  cmd.d3d12.rtvDescriptorSize =
      device.d3d12.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  cmd.d3d12.dsvDescriptorSize =
      device.d3d12.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  // Command lists are created open; close so begin() Reset works.
  cmd.d3d12.cmdList->Close();
}

void RID3D12_InvalidateCachedBindings(RICmd &cmd) {
  cmd.d3d12.computePipelineBound = false;
  cmd.d3d12.boundGraphicsRootSignature = nullptr;
  cmd.d3d12.boundComputeRootSignature = nullptr;
  cmd.d3d12.boundResourceHeap = nullptr;
  cmd.d3d12.boundSamplerHeap = nullptr;
  cmd.d3d12.graphicsRootArgsSet = 0;
  cmd.d3d12.computeRootArgsSet = 0;
  cmd.d3d12.graphicsRootTableArgs = 0;
  cmd.d3d12.computeRootTableArgs = 0;
  cmd.d3d12.graphicsRootArgsRequired = 0;
  cmd.d3d12.computeRootArgsRequired = 0;
  cmd.d3d12.rootArgsReported = 0;
  cmd.d3d12.boundPipelineDebugName = nullptr;
}

void RID3D12_RestoreCachedBindings(RIDevice &device, RICmd &cmd) {
  if (!cmd.d3d12.cmdList)
    return;
  RID3D12_InvalidateCachedBindings(cmd);
  ID3D12DescriptorHeap *resourceHeap = nullptr;
  ID3D12DescriptorHeap *samplerHeap = nullptr;
  // Copy command lists cannot carry root tables and do not need shader-visible
  // heaps. Binding them only on direct/compute lists also avoids issuing a
  // graphics-only command on a copy queue.
  if (cmd.d3d12.cmdList->GetType() != D3D12_COMMAND_LIST_TYPE_COPY &&
      getDescriptorArenaHeaps(&device, &resourceHeap, &samplerHeap))
    RID3D12_SetDescriptorHeaps(cmd, resourceHeap, samplerHeap);
}

void RID3D12_CmdBegin(RIDevice &device, RICmd &cmd) {
  if (!cmd.d3d12.cmdList || !cmd.d3d12.allocator)
    return;
  if (!D3D12_WrapResult(cmd.d3d12.allocator->Reset()))
    return;
  if (!D3D12_WrapResult(cmd.d3d12.cmdList->Reset(cmd.d3d12.allocator, nullptr)))
    return;
  cmd.d3d12.rtvCount = 0;
  cmd.d3d12.dsvCount = 0;
  cmd.d3d12.uavClearCount = 0;
  cmd.d3d12.activeColorCount = 0;
  cmd.d3d12.activeDepth = false;
  cmd.d3d12.vertexBufferCacheValidMask = 0;
  // Reset the root-binding kind together with the command-list state.  A
  // command list can be reused after Reset(), and root parameters from the
  // previous recording are no longer valid until a pipeline is bound again.
  // Reset() also drops the bound root signature and descriptor heaps, so the
  // redundancy caches for both must start empty. That is the same state an SDK
  // recording behind our back leaves behind, so both go through one path.
  RID3D12_RestoreCachedBindings(device, cmd);
}

// Setting a root signature invalidates every root argument on the command
// list, even when the signature is the one already bound. Passes that bind
// descriptors once and then call bindPipeline per draw depend on the
// redundant set being elided here; without it their descriptor tables are
// silently dropped and the draw reads an uninitialized root argument.
void RID3D12_SetGraphicsRootSignature(RICmd &cmd, ID3D12RootSignature *rs) {
  if (!cmd.d3d12.cmdList || !rs || cmd.d3d12.boundGraphicsRootSignature == rs)
    return;
  cmd.d3d12.cmdList->SetGraphicsRootSignature(rs);
  cmd.d3d12.boundGraphicsRootSignature = rs;
  cmd.d3d12.graphicsRootArgsSet = 0;
  cmd.d3d12.graphicsRootTableArgs = 0;
  cmd.d3d12.rootArgsReported = 0;
}

void RID3D12_SetComputeRootSignature(RICmd &cmd, ID3D12RootSignature *rs) {
  if (!cmd.d3d12.cmdList || !rs || cmd.d3d12.boundComputeRootSignature == rs)
    return;
  cmd.d3d12.cmdList->SetComputeRootSignature(rs);
  cmd.d3d12.boundComputeRootSignature = rs;
  cmd.d3d12.computeRootArgsSet = 0;
  cmd.d3d12.computeRootTableArgs = 0;
  cmd.d3d12.rootArgsReported = 0;
}

// Root parameters past 63 fall outside the tracking masks. Reflected
// signatures here have a handful of parameters, so this only means the
// debug-time check ignores them -- never that a bind is skipped.
static void ri_d3d12_note_root_arg(RICmd &cmd, bool compute, uint32_t parameter,
                                   bool table) {
  if (parameter >= 64)
    return;
  const uint64_t bit = 1ull << parameter;
  if (compute) {
    cmd.d3d12.computeRootArgsSet |= bit;
    if (table)
      cmd.d3d12.computeRootTableArgs |= bit;
  } else {
    cmd.d3d12.graphicsRootArgsSet |= bit;
    if (table)
      cmd.d3d12.graphicsRootTableArgs |= bit;
  }
}

void RID3D12_SetGraphicsRootDescriptorTable(
    RICmd &cmd, uint32_t parameter, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
  if (!cmd.d3d12.cmdList || parameter == UINT32_MAX)
    return;
  cmd.d3d12.cmdList->SetGraphicsRootDescriptorTable(parameter, handle);
  ri_d3d12_note_root_arg(cmd, false, parameter, true);
}

void RID3D12_SetComputeRootDescriptorTable(RICmd &cmd, uint32_t parameter,
                                           D3D12_GPU_DESCRIPTOR_HANDLE handle) {
  if (!cmd.d3d12.cmdList || parameter == UINT32_MAX)
    return;
  cmd.d3d12.cmdList->SetComputeRootDescriptorTable(parameter, handle);
  ri_d3d12_note_root_arg(cmd, true, parameter, true);
}

void RID3D12_SetGraphicsRoot32BitConstants(RICmd &cmd, uint32_t parameter,
                                           uint32_t count, const void *data,
                                           uint32_t offset) {
  if (!cmd.d3d12.cmdList || parameter == UINT32_MAX)
    return;
  cmd.d3d12.cmdList->SetGraphicsRoot32BitConstants(parameter, count, data,
                                                   offset);
  ri_d3d12_note_root_arg(cmd, false, parameter, false);
}

void RID3D12_SetComputeRoot32BitConstants(RICmd &cmd, uint32_t parameter,
                                          uint32_t count, const void *data,
                                          uint32_t offset) {
  if (!cmd.d3d12.cmdList || parameter == UINT32_MAX)
    return;
  cmd.d3d12.cmdList->SetComputeRoot32BitConstants(parameter, count, data,
                                                  offset);
  ri_d3d12_note_root_arg(cmd, true, parameter, false);
}

void RID3D12_NoteBoundProgramRootArgs(RICmd &cmd, bool compute,
                                      uint64_t required,
                                      const char *debugName) {
  if (compute)
    cmd.d3d12.computeRootArgsRequired = required;
  else
    cmd.d3d12.graphicsRootArgsRequired = required;
  cmd.d3d12.boundPipelineDebugName = debugName;
}

uint32_t g_riD3D12MissingRootArgumentCount = 0;

void RID3D12_CheckRootArguments(RICmd &cmd, const char *what) {
  const bool compute = cmd.d3d12.computePipelineBound;
  const uint64_t required = compute ? cmd.d3d12.computeRootArgsRequired
                                    : cmd.d3d12.graphicsRootArgsRequired;
  const uint64_t bound =
      compute ? cmd.d3d12.computeRootArgsSet : cmd.d3d12.graphicsRootArgsSet;
  const uint64_t missing = required & ~bound & ~cmd.d3d12.rootArgsReported;
  if (!missing)
    return;
  cmd.d3d12.rootArgsReported |= missing;
  // Collect every missing parameter into one message and fail once afterwards.
  // Failing inside the loop would report only the lowest-numbered parameter
  // before the process stopped, hiding the rest of the picture.
  // All 64 parameters spell out to ~244 characters, so size for the worst case
  // and clamp regardless: snprintf reports the length it would have written,
  // which would otherwise walk the cursor past the end.
  char parameters[320] = {};
  size_t cursor = 0;
  for (uint32_t parameter = 0; parameter < 64; ++parameter) {
    if (!(missing & (1ull << parameter)))
      continue;
    ++g_riD3D12MissingRootArgumentCount;
    if (cursor >= sizeof(parameters))
      continue;
    const int written =
        snprintf(parameters + cursor, sizeof(parameters) - cursor, "%s%u",
                 cursor ? ", " : "", parameter);
    if (written > 0)
      cursor = std::min(cursor + (size_t)written, sizeof(parameters) - 1);
  }
  hpl::ValidationFailed(
      "RI D3D12: %s needs root parameter%s %s but nothing has bound %s since "
      "the last root-signature change (pipeline '%s'). Bind the pipeline "
      "before the descriptors, or rebind the descriptors after every pipeline "
      "bind.\n",
      what, (missing & (missing - 1)) ? "s" : "", parameters,
      (missing & (missing - 1)) ? "them" : "it",
      cmd.d3d12.boundPipelineDebugName ? cmd.d3d12.boundPipelineDebugName
                                       : "<unnamed>");
}

// Changing descriptor heaps invalidates descriptor-table root arguments the
// same way a root-signature change does, so the redundant bind is elided too.
// A null samplerHeap binds the resource heap alone, for callers such as mip
// generation that drive their own single-heap table.
void RID3D12_SetDescriptorHeaps(RICmd &cmd, ID3D12DescriptorHeap *resourceHeap,
                                ID3D12DescriptorHeap *samplerHeap) {
  if (!cmd.d3d12.cmdList)
    return;
  if (cmd.d3d12.boundResourceHeap == resourceHeap &&
      cmd.d3d12.boundSamplerHeap == samplerHeap)
    return;
  ID3D12DescriptorHeap *heaps[2] = {};
  UINT count = 0;
  if (resourceHeap)
    heaps[count++] = resourceHeap;
  if (samplerHeap)
    heaps[count++] = samplerHeap;
  if (!count)
    return;
  cmd.d3d12.cmdList->SetDescriptorHeaps(count, heaps);
  cmd.d3d12.boundResourceHeap = resourceHeap;
  cmd.d3d12.boundSamplerHeap = samplerHeap;
  // Descriptor tables are heap-relative, so a heap change strands them. Root
  // constants are unaffected and stay marked as bound.
  cmd.d3d12.graphicsRootArgsSet &= ~cmd.d3d12.graphicsRootTableArgs;
  cmd.d3d12.computeRootArgsSet &= ~cmd.d3d12.computeRootTableArgs;
  cmd.d3d12.graphicsRootTableArgs = 0;
  cmd.d3d12.computeRootTableArgs = 0;
}

void RID3D12_CmdEnd(RIDevice &device, RICmd &cmd) {
  (void)device;
  if (!cmd.d3d12.cmdList)
    return;
  D3D12_WrapResult(cmd.d3d12.cmdList->Close());
}

void RID3D12_BindVertexBuffers(RICmd &cmd, uint32_t firstBinding,
                               uint32_t count, RIBuffer *const *buffers,
                               const uint64_t *offsets,
                               const uint32_t *strides) {
  if (!RIIsTargetSelected(RI_DEVICE_API_D3D12) || !cmd.d3d12.cmdList ||
      count == 0)
    return;
  if (firstBinding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
      count > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - firstBinding)
    return;
  assert(firstBinding <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT &&
         count <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - firstBinding);
  if (!buffers || !offsets || !strides)
    return;
  assert(buffers && offsets && strides);
  D3D12_VERTEX_BUFFER_VIEW views[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] =
      {};
  uint32_t changedMask = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t binding = firstBinding + i;
    const uint64_t offset = offsets[i];
    views[i].StrideInBytes = binding < cmd.d3d12.vertexBindingCount &&
                                     binding < RI_D3D12_VERTEX_STRIDE_COUNT
                                 ? strides[binding]
                                 : 0;
    if (buffers[i]) {
      if (buffers[i]->d3d12.resource &&
          offset <= buffers[i]->d3d12.requestedSize) {
        views[i].BufferLocation =
            buffers[i]->GetDeviceHandle(cmd.d3d12.device) + offset;
        const uint64_t size = buffers[i]->d3d12.requestedSize - offset;
        views[i].SizeInBytes =
            static_cast<UINT>(size > UINT_MAX ? UINT_MAX : size);
      }
    }
    const uint32_t bit = 1u << binding;
    if (!(cmd.d3d12.vertexBufferCacheValidMask & bit) ||
        memcmp(&cmd.d3d12.vertexBufferViews[binding], &views[i],
               sizeof(views[i])) != 0)
      changedMask |= bit;
  }
  if (!changedMask)
    return;
  cmd.d3d12.cmdList->IASetVertexBuffers(firstBinding, count, views);
  for (uint32_t i = 0; i < count; ++i)
    cmd.d3d12.vertexBufferViews[firstBinding + i] = views[i];
  const uint32_t rangeMask = count == 32 ? UINT_MAX : ((1u << count) - 1u);
  cmd.d3d12.vertexBufferCacheValidMask |= rangeMask << firstBinding;
}

void RID3D12_RebindCachedVertexBuffers(RICmd &cmd) {
  if (!RIIsTargetSelected(RI_DEVICE_API_D3D12) || !cmd.d3d12.cmdList)
    return;

  constexpr uint32_t kD3D12SlotCount =
      D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t kStrideCount =
      static_cast<uint32_t>(sizeof(cmd.d3d12.vertexBindingStrides) /
                            sizeof(cmd.d3d12.vertexBindingStrides[0]));
  static_assert(kD3D12SlotCount <= 32,
                "D3D12 vertex cache mask must fit in uint32_t");
  static_assert(kStrideCount <= kD3D12SlotCount,
                "vertex stride storage cannot exceed D3D12 vertex slots");

  const uint32_t validMask = cmd.d3d12.vertexBufferCacheValidMask;
  if (!validMask)
    return;

  uint32_t changedMask = 0;
  for (uint32_t slot = 0; slot < kD3D12SlotCount; ++slot) {
    const uint32_t bit = 1u << slot;
    if (!(validMask & bit))
      continue;
    const uint32_t stride =
        slot < cmd.d3d12.vertexBindingCount && slot < kStrideCount
            ? cmd.d3d12.vertexBindingStrides[slot]
            : 0;
    auto &view = cmd.d3d12.vertexBufferViews[slot];
    if (view.StrideInBytes != stride) {
      view.StrideInBytes = stride;
      changedMask |= bit;
    }
  }
  if (!changedMask)
    return;

  // Rebind contiguous changed runs.  The cached BufferLocation and
  // SizeInBytes are left untouched, which preserves offsets and null slots.
  for (uint32_t slot = 0; slot < kD3D12SlotCount;) {
    if (!(changedMask & (1u << slot))) {
      ++slot;
      continue;
    }
    const uint32_t first = slot;
    do {
      ++slot;
    } while (slot < kD3D12SlotCount && (changedMask & (1u << slot)));
    cmd.d3d12.cmdList->IASetVertexBuffers(first, slot - first,
                                          &cmd.d3d12.vertexBufferViews[first]);
  }
}

void RID3D12_CmdDispose(RIDevice &device, RICmd &cmd) {
  (void)device;
  if (cmd.d3d12.cmdList7) {
    cmd.d3d12.cmdList7->Release();
    cmd.d3d12.cmdList7 = nullptr;
  }
  if (cmd.d3d12.cmdList) {
    cmd.d3d12.cmdList->Release();
    cmd.d3d12.cmdList = nullptr;
  }
  if (cmd.d3d12.rtvHeap) {
    cmd.d3d12.rtvHeap->Release();
    cmd.d3d12.rtvHeap = nullptr;
  }
  if (cmd.d3d12.dsvHeap) {
    cmd.d3d12.dsvHeap->Release();
    cmd.d3d12.dsvHeap = nullptr;
  }
  if (cmd.d3d12.uavClearCpuHeap) {
    cmd.d3d12.uavClearCpuHeap->Release();
    cmd.d3d12.uavClearCpuHeap = nullptr;
  }
  if (cmd.d3d12.uavClearGpuHeap) {
    cmd.d3d12.uavClearGpuHeap->Release();
    cmd.d3d12.uavClearGpuHeap = nullptr;
  }
  if (cmd.d3d12.allocator) {
    cmd.d3d12.allocator->Release();
    cmd.d3d12.allocator = nullptr;
  }
  memset(&cmd.d3d12, 0, sizeof(cmd.d3d12));
}

void RID3D12_QueueWaitIdle(RIDevice &device, RIQueue &queue) {
  if (!queue.d3d12.queue || !queue.d3d12.fence || !queue.d3d12.fenceEvent)
    return;
  const uint64_t signalValue = ++queue.d3d12.nextFenceValue;
  if (!D3D12_WrapResult(
          queue.d3d12.queue->Signal(queue.d3d12.fence, signalValue))) {
    RID3D12_CheckDeviceRemoved(device, "QueueWaitIdle");
    return;
  }
  const uint64_t completed = queue.d3d12.fence->GetCompletedValue();
  // A removed device reports UINT64_MAX for every fence.
  if (completed == UINT64_MAX)
    RID3D12_CheckDeviceRemoved(device, "QueueWaitIdle");
  if (completed < signalValue) {
    HRESULT hr = queue.d3d12.fence->SetEventOnCompletion(
        signalValue, queue.d3d12.fenceEvent);
    if (!D3D12_WrapResult(hr))
      return;
    WaitForSingleObject(queue.d3d12.fenceEvent, INFINITE);
  }
}

enum RIResult_e RID3D12_QueueSubmit(struct RIDevice &device,
                                    struct RIQueue &queue,
                                    const RID3D12SubmitDesc &desc) {
  if (!device.d3d12.device || !queue.d3d12.queue)
    return RI_FAIL;
  if (desc.listCount > 0 && !desc.lists)
    return RI_FAIL;
  if (desc.waitCount > 0 && !desc.waits)
    return RI_FAIL;
  if (desc.signalCount > 0 && !desc.signals)
    return RI_FAIL;
  if (desc.listCount == 0 && desc.waitCount == 0 && desc.signalCount == 0 &&
      !desc.completionFence)
    return RI_FAIL;

  for (uint32_t i = 0; i < desc.listCount; ++i) {
    const int listType = desc.lists[i] ? int(desc.lists[i]->GetType()) : -1;
    const int queueType = int(queue.d3d12.type);
    if (!desc.lists[i] || listType != queueType) {
      hpl::Warning("RI D3D12: queue submit command list %u has mismatched type "
                   "(list=%d queue=%d)\n",
                   i, listType, queueType);
      return RI_FAIL;
    }
  }

  for (uint32_t i = 0; i < desc.waitCount; ++i) {
    if (!desc.waits[i].fence || !D3D12_WrapResult(queue.d3d12.queue->Wait(
                                    desc.waits[i].fence, desc.waits[i].value)))
      return RI_FAIL;
  }
  if (desc.listCount > 0)
    queue.d3d12.queue->ExecuteCommandLists(
        desc.listCount, const_cast<ID3D12CommandList *const *>(desc.lists));

  bool success = true;
  for (uint32_t i = 0; i < desc.signalCount; ++i) {
    if (!desc.signals[i].fence ||
        !D3D12_WrapResult(queue.d3d12.queue->Signal(desc.signals[i].fence,
                                                    desc.signals[i].value)))
      success = false;
  }
  if (desc.completionFence && !D3D12_WrapResult(queue.d3d12.queue->Signal(
                                  desc.completionFence, desc.completionValue)))
    success = false;
  RID3D12_DrainDeviceMessages(device);
  RID3D12_CheckDeviceRemoved(device, "QueueSubmit");
  return success ? RI_SUCCESS : RI_FAIL;
}

#endif // DEVICE_IMPL_D3D12
