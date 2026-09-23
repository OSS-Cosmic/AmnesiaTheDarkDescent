// Opt-in GPU smoke test for RI queue submission, timelines, and command rings.
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"
#include "graphics/RICommandRingBuffer.h"
#include "graphics/RITimeline.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int hplMain(const std::string &) { return 0; }

namespace {

[[noreturn]] void Fail(const char *check) {
  std::fprintf(stderr, "FAIL: %s\n", check);
  std::exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition)
    Fail(check);
  std::printf("PASS: %s\n", check);
}

uint8_t RequestedApi(bool vulkan) {
  return vulkan ? RI_DEVICE_API_VK : RI_DEVICE_API_D3D12;
}

void RunEmptySubmit(RIDevice *device, bool vulkan) {
  RISubmitDesc desc = {};
  enum RIResult_e result =
      device->queues[RI_QUEUE_GRAPHICS].submit(device, desc);
  Require(vulkan ? result == RI_SUCCESS : result == RI_FAIL,
          vulkan ? "Vulkan empty submit succeeds"
                 : "D3D12 empty submit is rejected");
}

void RunQueueListSubmits(RIDevice *device) {
  const RIQueueType_e queues[] = {RI_QUEUE_GRAPHICS, RI_QUEUE_COMPUTE,
                                  RI_QUEUE_COPY};
  for (RIQueueType_e queueType : queues) {
    RIPool pool;
    pool.init(device, &device->queues[queueType]);
    RICmd cmd;
    cmd.init(device, &pool);
    cmd.begin(device);
    cmd.end(device);
    RICmd *commands[] = {&cmd};
    RISubmitDesc desc = {};
    desc.cmds = commands;
    desc.cmdCount = 1;
    Require(device->queues[queueType].submit(device, desc) == RI_SUCCESS,
            "empty graphics/compute/copy command list submit succeeds");
    device->queues[queueType].waitIdle(device);
    cmd.dispose(device);
    pool.dispose(device);
  }
}

void RunCrossQueueTimeline(RIDevice *device) {
  hpl::RITimeline srcTimeline;
  hpl::RITimeline dstTimeline;
  srcTimeline.init(device);
  dstTimeline.init(device);

  RIPool copyPool;
  copyPool.init(device, &device->queues[RI_QUEUE_COPY]);
  RICmd copyCmd;
  copyCmd.init(device, &copyPool);
  copyCmd.begin(device);
  copyCmd.end(device);
  RICmd *copyCommands[] = {&copyCmd};
  RITimelineOp copySignals[] = {
      {&srcTimeline, srcTimeline.next(), RI_STAGE_COPY},
  };
  RISubmitDesc copySubmit = {};
  copySubmit.cmds = copyCommands;
  copySubmit.cmdCount = 1;
  copySubmit.signals = copySignals;
  copySubmit.signalCount = 1;
  Require(device->queues[RI_QUEUE_COPY].submit(device, copySubmit) == RI_SUCCESS,
          "copy timeline signal submit succeeds");

  RIPool graphicsPool;
  graphicsPool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd graphicsCmd;
  graphicsCmd.init(device, &graphicsPool);
  graphicsCmd.begin(device);
  graphicsCmd.end(device);
  RICmd *graphicsCommands[] = {&graphicsCmd};
  RITimelineOp graphicsWaits[] = {
      {&srcTimeline, srcTimeline.pending(), RI_STAGE_ALL_GRAPHICS},
  };
  RITimelineOp graphicsSignals[] = {
      {&dstTimeline, dstTimeline.next(), RI_STAGE_ALL_GRAPHICS},
  };
  RISubmitDesc graphicsSubmit = {};
  graphicsSubmit.cmds = graphicsCommands;
  graphicsSubmit.cmdCount = 1;
  graphicsSubmit.waits = graphicsWaits;
  graphicsSubmit.waitCount = 1;
  graphicsSubmit.signals = graphicsSignals;
  graphicsSubmit.signalCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, graphicsSubmit) ==
              RI_SUCCESS,
          "graphics cross-queue wait/signal submit succeeds");

  dstTimeline.wait(device, dstTimeline.pending());
  Require(srcTimeline.completed(device) >= 1,
          "source timeline reports completed GPU progress");
  Require(dstTimeline.completed(device) >= 1,
          "destination timeline reports completed GPU progress");

  graphicsCmd.dispose(device);
  graphicsPool.dispose(device);
  copyCmd.dispose(device);
  copyPool.dispose(device);
  dstTimeline.dispose(device);
  srcTimeline.dispose(device);
}

void RecordAndSubmit(RIDevice *device, RICommandRingElement *element,
                     bool resetPool) {
  if (resetPool)
    element->pool->reset(device);
  element->cmds[0].begin(device);
  element->cmds[0].end(device);
  RICmd *commands[] = {&element->cmds[0]};
  RISubmitDesc desc = {};
  desc.cmds = commands;
  desc.cmdCount = 1;
  desc.completion = element;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, desc) == RI_SUCCESS,
          "ring command submit succeeds");
}

void RunRingWraparound(RIDevice *device, bool vulkan) {
  RICommandRingBuffer<> ring;
  ring.init(device, &device->queues[RI_QUEUE_GRAPHICS],
            RI_COMMAND_RING_POOL_COUNT, 1, true);
  for (uint32_t iter = 0; iter < RI_COMMAND_RING_POOL_COUNT * 3 + 1; ++iter) {
    ring.advance();
    RICommandRingElement element = ring.acquire(device, 1);
    element.wait(device);
#if DEVICE_IMPL_D3D12
    if (!vulkan)
      Require(element.d3d12.value == 0,
              "unsubmitted DX12 ring element has no pending value");
#endif
    RecordAndSubmit(device, &element, true);
#if DEVICE_IMPL_D3D12
    if (!vulkan)
      Require(element.d3d12.fence != nullptr && element.d3d12.value != 0,
              "submitted DX12 ring element captures completion fence");
#endif
    if ((iter + 1) % 4 == 0)
      element.wait(device);
  }
  ring.dispose(device);
}

void RunMultipleAcquisitions(RIDevice *device, bool vulkan) {
  RICommandRingBuffer<2, 4> ring;
  ring.init(device, &device->queues[RI_QUEUE_GRAPHICS], 2, 4, true);
  ring.advance();
  RICommandRingElement first = ring.acquire(device, 1);
  RICommandRingElement second = ring.acquire(device, 1);
  RecordAndSubmit(device, &first, true);
#if DEVICE_IMPL_D3D12
  const uint64_t firstValue = first.d3d12.value;
#endif
  RecordAndSubmit(device, &second, false);
#if DEVICE_IMPL_D3D12
  if (!vulkan)
    Require(second.d3d12.value > firstValue,
            "DX12 shared queue fence values are monotonic");
#endif
  second.wait(device);
  first.wait(device);
  first.pool->reset(device);
  ring.pools[0].reset(device);
  ring.dispose(device);
}

void RunQueueCycle(bool vulkan) {
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName = vulkan ? "RIVulkanQueueSmoke" : "RID3D12QueueSmoke";
  if (!vulkan)
    g_riD3D12EnableDebugLayer = true;

  Require(InitRIRenderer(&init) == RI_SUCCESS,
          vulkan ? "Vulkan InitRIRenderer succeeds"
                 : "D3D12 InitRIRenderer succeeds");
  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(nullptr, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1 && numAdapters <= 8,
          vulkan ? "Vulkan enumeration reports adapters"
                 : "D3D12 enumeration reports adapters");
  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          vulkan ? "Vulkan enumeration populates adapters"
                 : "D3D12 enumeration populates adapters");

  uint32_t selected = 0;
  if (!vulkan) {
    bool foundHardware = false;
    for (uint32_t i = 0; i < capacity; ++i) {
      if (!adapters[i].d3d12.isWarp) {
        selected = i;
        foundHardware = true;
        break;
      }
    }
    if (!foundHardware) {
      for (uint32_t i = 0; i < capacity; ++i) {
        if (adapters[i].d3d12.isWarp) {
          selected = i;
          break;
        }
      }
    }
    Require(foundHardware || adapters[selected].d3d12.isWarp,
            "D3D12 selects hardware or WARP adapter");
  }

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          vulkan ? "Vulkan device initializes and is valid"
                 : "D3D12 device initializes and is valid");

  RunEmptySubmit(&device, vulkan);
  RunQueueListSubmits(&device);
  RunCrossQueueTimeline(&device);
  RunRingWraparound(&device, vulkan);
  RunMultipleAcquisitions(&device, vulkan);

  device.dispose();
  ShutdownRIRenderer();
  if (!vulkan)
    g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  bool vulkan = false;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--vulkan") == 0)
      vulkan = true;
  RunQueueCycle(vulkan);
  return 0;
}
