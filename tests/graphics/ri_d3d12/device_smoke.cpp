// Opt-in GPU smoke test for the renderer/device bootstrap. This intentionally
// includes RIDevice directly before any other RI header; temporal_camera.cpp
// takes the other supported shape by including through RITypes.h. Both paths
// must agree on the RI layout.
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
#include "graphics/RIBuffer.h"
#include "graphics/RICommand.h"
#include "graphics/RIRenderer.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

// The premake project defines IGNORE_HPL_MAIN so this TU builds without pulling
// in HPL2's SDL WinMain wrapper. HPL2.lib is still linked and its own
// LowLevelSystemSDL.obj carries WinMain (compiled without IGNORE_HPL_MAIN), so
// provide a stub hplMain to satisfy the reference — the smoke test never
// invokes WinMain; the ConsoleApp entry point is main() below.
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

void RequireD3D12Allocator(RIDevice *device, bool vulkan, const char *check) {
  if (!vulkan)
    Require(device->d3d12.allocator != nullptr, check);
}

void RequireD3D12Disposed(RIDevice *device, bool vulkan, const char *check) {
  if (!vulkan)
    Require(device->d3d12.device == nullptr && device->d3d12.allocator == nullptr,
            check);
}

void RunDeviceCycle(bool vulkan, unsigned cycle) {
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName = vulkan ? "RIVulkanDeviceSmoke" : "RID3D12DeviceSmoke";

  Require(InitRIRenderer(&init) == RI_SUCCESS,
          vulkan ? (cycle == 1 ? "Vulkan InitRIRenderer cycle 1 succeeds"
                               : "Vulkan InitRIRenderer cycle 2 succeeds")
                 : (cycle == 1 ? "D3D12 InitRIRenderer cycle 1 succeeds"
                               : "D3D12 InitRIRenderer cycle 2 succeeds"));

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(NULL, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1,
          vulkan ? "Vulkan enumeration reports at least one adapter"
                 : "D3D12 enumeration reports at least one adapter");
  Require(numAdapters <= 8,
          vulkan ? "Vulkan adapter count fits the smoke-test array"
                 : "D3D12 adapter count fits the smoke-test array");

  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          vulkan ? "Vulkan enumeration populates the adapter array"
                 : "D3D12 enumeration populates the adapter array");

  if (!vulkan) {
    for (uint32_t i = 0; i < capacity; ++i)
      Require(adapters[i].colorAttachmentMaxNum ==
                  D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT,
              "D3D12 adapter publishes all output-merger render-target slots");
  }

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
    Require(foundHardware, "D3D12 enumeration finds a non-WARP adapter");
  }

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          vulkan ? "Vulkan hardware device initializes and is valid"
                 : "D3D12 hardware device initializes and is valid");
  RequireD3D12Allocator(&device, vulkan,
                        "successful D3D12 device owns a D3D12MA allocator");
  device.dispose();
  RequireD3D12Disposed(&device, vulkan,
                       "D3D12 device disposal clears device and allocator handles");
  device.dispose();
  std::printf("PASS: %s dispose is safe when repeated\n",
              vulkan ? "Vulkan device" : "D3D12 device");

  ShutdownRIRenderer();
}

void RunWarpCycle() {
  g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12DeviceSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          "D3D12 WARP InitRIRenderer succeeds");

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(NULL, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1,
          "D3D12 WARP enumeration reports at least one adapter");

  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          "D3D12 WARP enumeration populates the adapter array");

  uint32_t warp = 0;
  bool foundWarp = false;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (adapters[i].d3d12.isWarp) {
      warp = i;
      foundWarp = true;
      break;
    }
  }
  Require(foundWarp, "D3D12 enumeration finds the WARP adapter");

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[warp];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 WARP device initializes and is valid");
  RequireD3D12Allocator(&device, false,
                        "successful D3D12 WARP device owns a D3D12MA allocator");
  device.dispose();
  RequireD3D12Disposed(&device, false,
                       "D3D12 WARP disposal clears device and allocator handles");
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}

void RunWarpCopyCycle() {
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12DeviceSmokeCopy";
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          "D3D12 WARP copy InitRIRenderer succeeds");

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(NULL, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1,
          "D3D12 WARP copy enumeration reports at least one adapter");

  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          "D3D12 WARP copy enumeration populates the adapter array");

  uint32_t warp = 0;
  bool foundWarp = false;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (adapters[i].d3d12.isWarp) {
      warp = i;
      foundWarp = true;
      break;
    }
  }
  Require(foundWarp, "D3D12 WARP copy enumeration finds the WARP adapter");

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[warp];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 WARP copy device initializes and is valid");
  RequireD3D12Allocator(&device, false,
                        "successful D3D12 WARP copy device owns a D3D12MA allocator");

  const UINT kCopyBytes = 256;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Alignment = 0;
  bufferDesc.Width = kCopyBytes;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufferDesc.SampleDesc = {1, 0};
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  ID3D12Resource *uploadBuffer = nullptr;
  Require(SUCCEEDED(device.d3d12.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
              IID_PPV_ARGS(&uploadBuffer))),
          "D3D12 WARP copy upload buffer creation succeeds");
  Require(uploadBuffer != nullptr, "D3D12 WARP copy upload buffer is allocated");

  D3D12_RANGE readRange = {0, 0};
  void *uploadPtr = nullptr;
  Require(SUCCEEDED(uploadBuffer->Map(0, &readRange, &uploadPtr)),
          "D3D12 WARP copy upload buffer maps");
  Require(uploadPtr != nullptr, "D3D12 WARP copy upload mapping is allocated");
  for (UINT i = 0; i < kCopyBytes; ++i)
    static_cast<uint8_t *>(uploadPtr)[i] = (uint8_t)(i * 7 + 3);
  uploadBuffer->Unmap(0, nullptr);

  heap.Type = D3D12_HEAP_TYPE_READBACK;
  ID3D12Resource *readbackBuffer = nullptr;
  Require(SUCCEEDED(device.d3d12.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&readbackBuffer))),
          "D3D12 WARP copy readback buffer creation succeeds");
  Require(readbackBuffer != nullptr,
          "D3D12 WARP copy readback buffer is allocated");

  RIPool pool;
  pool.init(&device, &device.queues[RI_QUEUE_COPY]);
  Require(pool.d3d12.allocator != nullptr,
          "D3D12 WARP copy command allocator is allocated");
  RICmd cmd;
  cmd.init(&device, &pool);
  Require(cmd.d3d12.cmdList != nullptr,
          "D3D12 WARP copy command list is allocated");
  cmd.begin(&device);
  cmd.d3d12.cmdList->CopyBufferRegion(readbackBuffer, 0, uploadBuffer, 0,
                                     kCopyBytes);
  cmd.end(&device);

  ID3D12CommandList *lists[] = {cmd.d3d12.cmdList};
  device.queues[RI_QUEUE_COPY].d3d12.queue->ExecuteCommandLists(1, lists);
  device.queues[RI_QUEUE_COPY].waitIdle(&device);

  readRange = {0, kCopyBytes};
  void *readbackPtr = nullptr;
  Require(SUCCEEDED(readbackBuffer->Map(0, &readRange, &readbackPtr)),
          "D3D12 WARP copy readback buffer maps");
  Require(readbackPtr != nullptr,
          "D3D12 WARP copy readback mapping is allocated");
  for (UINT i = 0; i < kCopyBytes; ++i) {
    Require(static_cast<uint8_t *>(readbackPtr)[i] == (uint8_t)(i * 7 + 3),
            "D3D12 WARP copy round-trip matches");
  }

  readbackBuffer->Unmap(0, nullptr);
  uploadBuffer->Release();
  readbackBuffer->Release();
  cmd.dispose(&device);
  pool.dispose(&device);

  // Second round-trip through the RI abstraction: RIBuffer::create + RICmd::copyBuffer.
  const uint64_t kRICopyBytes = 256;
  RIBufferDesc srcDesc = {};
  srcDesc.size = kRICopyBytes;
  srcDesc.usage = RI_BUFFER_USAGE_TRANSFER_SRC;
  srcDesc.location = RI_MEMORY_HOST_UPLOAD;
  RIBuffer riUpload = RIBuffer::create(&device, srcDesc);
  Require(!riUpload.isEmpty(),
          "D3D12 WARP RI copyBuffer upload buffer creates");
  Require(riUpload.mappedAddress != nullptr,
          "D3D12 WARP RI copyBuffer upload buffer is mapped");
  Require(riUpload.d3d12.resource != nullptr && riUpload.d3d12.allocation != nullptr,
          "D3D12 WARP RI upload buffer owns a D3D12MA allocation");
  for (uint64_t i = 0; i < kRICopyBytes; ++i)
    static_cast<uint8_t *>(riUpload.mappedAddress)[i] = (uint8_t)(i * 11 + 5);

  RIBufferDesc dstDesc = {};
  dstDesc.size = kRICopyBytes;
  dstDesc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  dstDesc.location = RI_MEMORY_HOST_READBACK;
  RIBuffer riReadback = RIBuffer::create(&device, dstDesc);
  Require(!riReadback.isEmpty(),
          "D3D12 WARP RI copyBuffer readback buffer creates");
  Require(riReadback.mappedAddress != nullptr,
          "D3D12 WARP RI copyBuffer readback buffer is mapped");
  Require(riReadback.d3d12.resource != nullptr &&
              riReadback.d3d12.allocation != nullptr,
          "D3D12 WARP RI readback buffer owns a D3D12MA allocation");

  RIPool riPool;
  riPool.init(&device, &device.queues[RI_QUEUE_COPY]);
  Require(riPool.d3d12.allocator != nullptr,
          "D3D12 WARP RI copyBuffer command allocator is allocated");
  RICmd riCmd;
  riCmd.init(&device, &riPool);
  Require(riCmd.d3d12.cmdList != nullptr,
          "D3D12 WARP RI copyBuffer command list is allocated");
  riCmd.begin(&device);
  riCmd.copyBuffer(&device, &riUpload, 0, &riReadback, 0, kRICopyBytes);
  riCmd.end(&device);

  ID3D12CommandList *riLists[] = {riCmd.d3d12.cmdList};
  device.queues[RI_QUEUE_COPY].d3d12.queue->ExecuteCommandLists(1, riLists);
  device.queues[RI_QUEUE_COPY].waitIdle(&device);

  for (uint64_t i = 0; i < kRICopyBytes; ++i) {
    Require(static_cast<const uint8_t *>(riReadback.mappedAddress)[i] ==
                (uint8_t)(i * 11 + 5),
            "D3D12 WARP RI copyBuffer round-trip matches");
  }

  riCmd.dispose(&device);
  riPool.dispose(&device);
  riReadback.dispose(&device);
  riUpload.dispose(&device);
  Require(riReadback.d3d12.resource == nullptr &&
              riReadback.d3d12.allocation == nullptr &&
              riUpload.d3d12.resource == nullptr &&
              riUpload.d3d12.allocation == nullptr,
          "D3D12 RI buffer disposal clears resource and allocation handles");
  device.dispose();
  RequireD3D12Disposed(&device, false,
                       "D3D12 WARP copy disposal clears device and allocator handles");
  ShutdownRIRenderer();
}

void RunWarpTextureCycle() {
  g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12DeviceSmokeTexture";
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          "D3D12 WARP texture InitRIRenderer succeeds");

  uint32_t numAdapters = 0;
  Require(EnumerateRIAdapters(NULL, &numAdapters) == RI_SUCCESS &&
              numAdapters >= 1,
          "D3D12 WARP texture enumeration reports at least one adapter");

  RIPhysicalAdapter adapters[8];
  uint32_t capacity = numAdapters;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS &&
              capacity >= 1 && capacity <= 8,
          "D3D12 WARP texture enumeration populates the adapter array");

  uint32_t warp = 0;
  bool foundWarp = false;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (adapters[i].d3d12.isWarp) {
      warp = i;
      foundWarp = true;
      break;
    }
  }
  Require(foundWarp,
          "D3D12 WARP texture enumeration finds the WARP adapter");

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[warp];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 WARP texture device initializes and is valid");
  RequireD3D12Allocator(&device, false,
                        "successful D3D12 WARP texture device owns a D3D12MA allocator");

  RITextureDesc textureDesc = {
      .type = RI_TEXTURE_2D,
      .format = RI_FORMAT_RGBA8_UNORM,
      .width = 128,
      .height = 128,
      .depth = 1,
      .mipNum = 1,
      .layerNum = 1,
      .sampleCount = 1,
      .usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST,
      .flags = 0};
  RITexture tex = RITexture::create(&device, textureDesc);
  Require(!tex.isEmpty(), "D3D12 WARP texture color texture is not empty");
  Require(tex.d3d12.resource != nullptr,
          "D3D12 WARP texture color resource is allocated");
  Require(tex.d3d12.allocation != nullptr,
          "D3D12 WARP texture color owns a D3D12MA allocation");
  Require(tex.cookie != 0, "D3D12 WARP texture color cookie is populated");

  RITextureViewDesc viewDesc = {
      .viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D,
      .format = RI_FORMAT_RGBA8_UNORM,
      .baseMip = 0,
      .mipNum = 1,
      .baseLayer = 0,
      .layerNum = 1};
  RITextureView view = RITextureView::create(&device, &tex, viewDesc);
  Require(!view.isEmpty(), "D3D12 WARP texture sampled view is not empty");
  Require(view.d3d12.resource == tex.d3d12.resource,
          "D3D12 WARP texture sampled view borrows the texture resource");
  Require(view.cookie != 0, "D3D12 WARP texture sampled view cookie is populated");

  view.dispose(&device);
  Require(view.isEmpty(), "D3D12 WARP texture sampled view disposes empty");
  tex.dispose(&device);
  Require(tex.isEmpty() && tex.d3d12.resource == nullptr &&
              tex.d3d12.allocation == nullptr,
          "D3D12 WARP texture disposal clears resource and allocation handles");
  view.dispose(&device);
  tex.dispose(&device);
  Require(view.isEmpty() && tex.isEmpty(),
          "D3D12 WARP texture double-dispose is safe");

  RITextureDesc renderTargetDesc = textureDesc;
  renderTargetDesc.usage = RI_USAGE_COLOR_ATTACHMENT;
  RITexture renderTarget = RITexture::create(&device, renderTargetDesc);
  Require(!renderTarget.isEmpty(),
          "D3D12 WARP texture render-target texture is not empty");
  Require(renderTarget.d3d12.resource != nullptr &&
              renderTarget.d3d12.allocation != nullptr,
          "D3D12 WARP render-target texture owns a D3D12MA allocation");
  renderTarget.dispose(&device);
  Require(renderTarget.isEmpty() && renderTarget.d3d12.resource == nullptr &&
              renderTarget.d3d12.allocation == nullptr,
          "D3D12 WARP render-target disposal clears resource and allocation handles");

  RITextureDesc depthDesc = textureDesc;
  depthDesc.format = RI_FORMAT_D32_SFLOAT;
  depthDesc.usage = RI_USAGE_DEPTH_STENCIL_ATTACHMENT;
  RITexture depth = RITexture::create(&device, depthDesc);
  Require(!depth.isEmpty(),
          "D3D12 WARP texture depth-stencil texture is not empty");
  Require(depth.d3d12.resource != nullptr && depth.d3d12.allocation != nullptr,
          "D3D12 WARP depth texture owns a D3D12MA allocation");
  depth.dispose(&device);
  Require(depth.isEmpty() && depth.d3d12.resource == nullptr &&
              depth.d3d12.allocation == nullptr,
          "D3D12 WARP depth texture disposal clears resource and allocation handles");

  RISampler sampler;
  Require(sampler.isEmpty(), "D3D12 WARP texture sampler starts empty");
  sampler.dispose(&device);
  Require(sampler.isEmpty(), "D3D12 WARP texture sampler dispose is a no-op");

  // A desc with no storage buffer and a zero storageSize is malformed
  // regardless of whether the adapter supports DXR, so this stays a hard
  // requirement on every adapter. The success path (real geometry, real
  // storage) is covered by accel_structure_smoke.cpp, which skips itself when
  // rayTracingTier == 0.
  RIAccelStructure as;
  RIAccelStructureDesc accelDesc = {};
  accelDesc.type = RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL;
  Require(as.init(&device, &accelDesc) == RI_FAIL,
          "D3D12 acceleration-structure init rejects a desc with no storage");
  Require(as.isEmpty(),
          "D3D12 failed acceleration-structure init leaves the structure empty");
  as.dispose(&device);
  Require(as.isEmpty(),
          "D3D12 acceleration-structure dispose is safe after a failed init");

  device.dispose();
  RequireD3D12Disposed(&device, false,
                       "D3D12 WARP texture disposal clears device and allocator handles");
  ShutdownRIRenderer();
}

void CheckUnavailableApi() {
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D11;
  init.applicationName = "RID3D12DeviceSmokeUnavailable";
  Require(InitRIRenderer(&init) == RI_FAIL,
          "unavailable API selection fails");

  uint32_t numAdapters = 1234;
  int enumerateResult = EnumerateRIAdapters(NULL, &numAdapters);
  Require(enumerateResult == RI_FAIL && numAdapters == 1234,
          "enumeration leaves failed renderer state untouched");
#if DEVICE_MULTI_BACKEND
  Require(RIActiveBackendApi() == RI_DEVICE_API_UNKNOWN,
          "failed API selection leaves renderer API unknown");
#endif
  ShutdownRIRenderer();
}

void CheckAgilityExports() {
  HMODULE self = GetModuleHandleW(nullptr);
  Require(self != nullptr,
          "agility exports: process module handle is available");

  FARPROC versionSym = GetProcAddress(self, "D3D12SDKVersion");
  Require(versionSym != nullptr,
          "agility exports: D3D12SDKVersion is exported");
  const UINT sdkVersion = *reinterpret_cast<const UINT *>(versionSym);
  Require(sdkVersion == 619u,
          "agility exports: D3D12SDKVersion equals 619");

  FARPROC pathSym = GetProcAddress(self, "D3D12SDKPath");
  Require(pathSym != nullptr, "agility exports: D3D12SDKPath is exported");
  const char *sdkPath = *reinterpret_cast<const char *const *>(pathSym);
  Require(sdkPath != nullptr && std::strcmp(sdkPath, ".\\D3D12\\") == 0,
          "agility exports: D3D12SDKPath equals .\\D3D12\\");

  HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
  if (core == nullptr) {
    std::printf("INFO: D3D12Core.dll not currently loaded (device create not yet performed or Windows selected system runtime)\n");
  } else {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(core, buf, MAX_PATH);
    std::printf("INFO: D3D12Core loaded from %ls\n", buf);
  }
}

} // namespace

int main(int argc, char **argv) {
  bool vulkan = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vulkan") == 0)
      vulkan = true;
  }

  if (vulkan) {
    RunDeviceCycle(true, 1);
    RunDeviceCycle(true, 2);
    CheckAgilityExports();
  } else {
    g_riD3D12EnableDebugLayer = false;
    RunDeviceCycle(false, 1);
    RunDeviceCycle(false, 2);
    RunWarpCycle();
    g_riD3D12EnableDebugLayer = true;
    RunWarpCopyCycle();
    RunWarpTextureCycle();
    g_riD3D12EnableDebugLayer = false;
    CheckAgilityExports();
  }
  CheckUnavailableApi();
  return 0;
}
