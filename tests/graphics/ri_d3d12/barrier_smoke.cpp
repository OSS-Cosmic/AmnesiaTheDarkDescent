// Opt-in GPU smoke test for RI resource-state barrier translation.
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int hplMain(const std::string &) { return 0; }

namespace {

constexpr std::array<uint8_t, 4096> MakePattern() {
  std::array<uint8_t, 4096> pattern = {};
  for (size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = uint8_t(0x9F ^ (i * 7));
  return pattern;
}
constexpr std::array<uint8_t, 4096> pattern = MakePattern();

[[noreturn]] void Fail(const char *check) {
  std::fprintf(stderr, "FAIL: %s\n", check);
  std::fflush(stderr);
  // Skip atexit/static destructors: D3D12 resources acquired earlier in the
  // test are still live, and running the debug-layer teardown against them
  // aborts the process before this Fail message is seen.
  std::_Exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition)
    Fail(check);
  std::printf("PASS: %s\n", check);
}

uint8_t RequestedApi(bool vulkan) {
  return vulkan ? RI_DEVICE_API_VK : RI_DEVICE_API_D3D12;
}

void RunBarrierCycle(bool vulkan, bool enableDebugLayer) {
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName = vulkan ? "RIVulkanBarrierSmoke"
                                : "RID3D12BarrierSmoke";
  if (!vulkan && enableDebugLayer)
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
  RIBufferDesc bufferDesc = {};
  bufferDesc.size = 4096;
  bufferDesc.usage = RI_BUFFER_USAGE_TRANSFER_SRC |
                     RI_BUFFER_USAGE_TRANSFER_DST |
                     RI_BUFFER_USAGE_SHADER_RESOURCE;
  bufferDesc.location = RI_MEMORY_DEVICE;
  RIBuffer bufA = RIBuffer::create(&device, bufferDesc);
  RIBuffer bufB = RIBuffer::create(&device, bufferDesc);
  Require(!bufA.isEmpty() && !bufB.isEmpty(),
          "two device buffers for barrier smoke are created");

  RIBufferDesc uploadDesc = {};
  uploadDesc.size = 4096;
  uploadDesc.usage = RI_BUFFER_USAGE_TRANSFER_SRC;
  uploadDesc.location = RI_MEMORY_HOST_UPLOAD;
  RIBuffer uploadBuf = RIBuffer::create(&device, uploadDesc);
  Require(!uploadBuf.isEmpty() && uploadBuf.mappedAddress != nullptr,
          "upload staging buffer is created and mapped");

  RIBufferDesc readbackDesc = {};
  readbackDesc.size = 4096;
  readbackDesc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  readbackDesc.location = RI_MEMORY_HOST_READBACK;
  RIBuffer readbackBuf = RIBuffer::create(&device, readbackDesc);
  Require(!readbackBuf.isEmpty() && readbackBuf.mappedAddress != nullptr,
          "readback buffer is created and mapped");

  std::memcpy(uploadBuf.mappedAddress, pattern.data(), 4096);
  uploadBuf.flushMappedRange(&device, 0, 0);

  {
    RIPool seedPool;
    seedPool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
    RICmd seedCmd;
    seedCmd.init(&device, &seedPool);
    seedCmd.begin(&device);
    seedCmd.copyBuffer(&device, &uploadBuf, 0, &bufA, 0, 4096);
    seedCmd.end(&device);
    RICmd *seedCmds[] = {&seedCmd};
    RISubmitDesc seedSubmit = {};
    seedSubmit.cmds = seedCmds;
    seedSubmit.cmdCount = 1;
    Require(device.queues[RI_QUEUE_GRAPHICS].submit(&device, seedSubmit) == RI_SUCCESS,
            "seed bufA from upload buffer submits");
    device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
    Require(true, "seed bufA from upload buffer completes");
    seedCmd.dispose(&device);
    seedPool.dispose(&device);
  }

  RIPool pool;
  pool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
  Require(vulkan ? pool.vk.pool != VK_NULL_HANDLE
                 : pool.d3d12.allocator != nullptr,
          "graphics command pool initializes");
  RICmd cmd;
  cmd.init(&device, &pool);
  Require(vulkan ? cmd.vk.cmd != VK_NULL_HANDLE : cmd.d3d12.cmdList != nullptr,
          "barrier command initializes");
  cmd.begin(&device);
  Require(vulkan ? cmd.vk.cmd != VK_NULL_HANDLE : cmd.d3d12.cmdList != nullptr,
          "barrier command begins");
  const bool expectEnhanced = !vulkan && adapters[selected].isEnchancedBarrierSupported &&
                              cmd.d3d12.cmdList7 != nullptr;
  if (!vulkan) {
    g_riD3D12EnhancedBarrierCallCount = 0;
    g_riD3D12LegacyBarrierCallCount = 0;
  }

  RIBufferBarrier first[] = {
      // Seed fence handles cross-submit sync; stale resources need NO_ACCESS activation.
      RIBufferBarrier(&bufA, RI_RESOURCE_STATE_UNDEFINED,
                      RI_RESOURCE_STATE_COPY_SRC),
      RIBufferBarrier(&bufB, RI_RESOURCE_STATE_UNDEFINED,
                      RI_RESOURCE_STATE_COPY_DST),
  };
  cmd.vk_d3d12_resourceBarrier<0, 2, 0>(0, nullptr, 2, first, 0, nullptr);
  Require(true, "batched UNDEFINED/UNDEFINED to COPY_SRC/COPY_DST barrier records");

  cmd.copyBuffer(&device, &bufA, 0, &bufB, 0, 4096);
  Require(true, "copyBuffer bufA to bufB records between barriers");

  RIBufferBarrier second[] = {
      RIBufferBarrier(&bufA, RI_RESOURCE_STATE_COPY_SRC,
                      RI_RESOURCE_STATE_SHADER_RESOURCE),
      // An index-buffer destination has no RI stage hint: the backend must
      // derive D3D12_BARRIER_SYNC_INDEX_INPUT rather than VERTEX_SHADING.
      RIBufferBarrier(&bufB, RI_RESOURCE_STATE_COPY_DST,
                      RI_RESOURCE_STATE_INDEX_BUFFER, RI_STAGE_COPY,
                      RI_STAGE_NONE),
  };
  cmd.vk_d3d12_resourceBarrier<0, 2, 0>(0, nullptr, 2, second, 0, nullptr);
  Require(true, "batched COPY_SRC/COPY_DST to shader/index-input barrier records");

  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&bufB, RI_RESOURCE_STATE_INDEX_BUFFER,
                      RI_RESOURCE_STATE_SHADER_RESOURCE));

  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&bufB, RI_RESOURCE_STATE_SHADER_RESOURCE,
                      RI_RESOURCE_STATE_COPY_SRC));
  cmd.copyBuffer(&device, &bufB, 0, &readbackBuf, 0, 4096);
  Require(true, "readback bufB into host-visible buffer records");

  cmd.vk_d3d12_memoryBarrier(
      RIMemoryBarrier(RI_RESOURCE_STATE_STORAGE_WRITE,
                      RI_RESOURCE_STATE_STORAGE_READ));
  Require(true, "memory barrier records");
  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&bufA, RI_RESOURCE_STATE_SHADER_RESOURCE,
                      RI_RESOURCE_STATE_UNDEFINED));
  Require(true, "single buffer barrier records");

  cmd.end(&device);
  RICmd *commands[] = {&cmd};
  RISubmitDesc submit = {};
  submit.cmds = commands;
  submit.cmdCount = 1;
  Require(device.queues[RI_QUEUE_GRAPHICS].submit(&device, submit) == RI_SUCCESS,
          "barrier command list submits");
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
  Require(true, "barrier submission reaches idle");
  if (!vulkan)
    Require(expectEnhanced ? g_riD3D12EnhancedBarrierCallCount > 0 && g_riD3D12LegacyBarrierCallCount == 0
                           : g_riD3D12LegacyBarrierCallCount > 0 && g_riD3D12EnhancedBarrierCallCount == 0,
            "barrier path matches adapter enhanced-barrier support");
  readbackBuf.invalidateMappedRange(&device, 0, 0);
  Require(readbackBuf.mappedAddress != nullptr,
          "readback mapping remains valid after invalidateMappedRange");
  Require(std::memcmp(readbackBuf.mappedAddress, pattern.data(), 4096) == 0,
          "readback payload matches source pattern");

  cmd.dispose(&device);
  pool.dispose(&device);
  readbackBuf.dispose(&device);
  uploadBuf.dispose(&device);
  bufB.dispose(&device);
  bufA.dispose(&device);
  device.dispose();
  ShutdownRIRenderer();
  if (!vulkan)
    g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool vulkan = false;
  bool enableDebugLayer = true;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--vulkan") == 0)
      vulkan = true;
    else if (std::strcmp(argv[i], "--no-debug-layer") == 0)
      enableDebugLayer = false;
  RunBarrierCycle(vulkan, enableDebugLayer);
  return 0;
}
