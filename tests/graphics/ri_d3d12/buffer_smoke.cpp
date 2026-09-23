// Opt-in GPU smoke test for RIBuffer lifecycle and mapping behavior.
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
#include "graphics/RIRenderer.h"

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

void Skip(const char *check) { std::printf("SKIP: %s\n", check); }

uint8_t RequestedApi(bool vulkan) {
  return vulkan ? RI_DEVICE_API_VK : RI_DEVICE_API_D3D12;
}

void RequireDisposed(RIDevice *device, RIBuffer *buffer, bool vulkan,
                     const char *label) {
  Require(buffer->isEmpty(), label);
  Require(buffer->mappedAddress == nullptr, "disposed buffer mapping is null");
  Require(buffer->cookie == 0, "disposed buffer cookie is zero");
  Require(buffer->GetDeviceHandle(device) == 0,
          "disposed buffer device handle is zero");
  if (!vulkan) {
    Require(buffer->d3d12.resource == nullptr,
            "disposed D3D12 buffer resource handle is null");
    Require(buffer->d3d12.allocation == nullptr,
            "disposed D3D12 buffer allocation handle is null");
  }
}

void RequireBufferAllocation(RIBuffer *buffer, bool vulkan, const char *check) {
  if (vulkan) {
    Require(buffer->vk.buffer != VK_NULL_HANDLE &&
                buffer->vk.allocation != nullptr,
            check);
  } else {
    Require(buffer->d3d12.resource != nullptr &&
                buffer->d3d12.allocation != nullptr,
            check);
  }
}

void RunBufferCases(RIDevice *device, bool vulkan) {
  RIBufferDesc desc = {};
  desc.size = 4096;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  if (vulkan)
    desc.usage |= RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                  RI_BUFFER_USAGE_DEVICE_ADDRESS;
  desc.location = RI_MEMORY_DEVICE;
  RIBuffer buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "device buffer creation succeeds");
  RequireBufferAllocation(&buffer, vulkan,
                          vulkan ? "device Vulkan buffer owns allocation and native buffer"
                                 : "device buffer owns a D3D12MA allocation");
  Require(buffer.mappedAddress == nullptr, "device buffer is not mapped");
  const uint64_t deviceHandle = buffer.GetDeviceHandle(device);
  Require(deviceHandle > 0, "device buffer has a GPU handle");
  if (vulkan) {
    Require(buffer.GetShaderResourceHandle(device) == deviceHandle,
            "Vulkan shader handle is the buffer device address");
  } else {
    Require(buffer.GetShaderResourceHandle(device) == 0,
            "unregistered buffer shader handle is missing");
    RID3D12_SetBufferShaderResourceIndex(buffer, 7u);
    const uint64_t shaderHandle = buffer.GetShaderResourceHandle(device);
    Require(shaderHandle == ((static_cast<uint64_t>(7u) + 1ull) << 32) &&
                (shaderHandle & 0xffffffffull) == 0,
            "D3D12 shader handle encodes descriptor index in high word");
  }
  buffer.setDebugObjectName(device, "RID3D12BufferSmoke.Device");
  buffer.dispose(device);
  RequireDisposed(device, &buffer, vulkan, "device buffer is empty after dispose");
  buffer.dispose(device);

  desc = {};
  desc.size = 200;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_CONSTANT_BUFFER;
  desc.location = RI_MEMORY_HOST_UPLOAD;
  buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "upload buffer creation succeeds");
  RequireBufferAllocation(&buffer, vulkan,
                          vulkan ? "upload Vulkan buffer owns allocation and native buffer"
                                 : "upload buffer owns a D3D12MA allocation");
  Require(buffer.mappedAddress != nullptr, "upload buffer is mapped");
  if (!vulkan) {
    Require(buffer.GetDeviceHandle(device) > 0, "upload buffer has a GPU handle");
    Require(buffer.d3d12.allocationSize >= 256,
            "upload allocation has CBV padding");
    Require(buffer.d3d12.requestedSize == 200,
            "upload allocation preserves requested size");
  }
  std::memset(buffer.mappedAddress, 0x5A, 200);
  buffer.flushMappedRange(device, 0, 0);
  buffer.dispose(device);
  RequireDisposed(device, &buffer, vulkan, "upload buffer is empty after dispose");

  desc = {};
  desc.size = 1024;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  desc.location = RI_MEMORY_HOST_READBACK;
  buffer = RIBuffer::create(device, desc);
  Require(!buffer.isEmpty(), "readback buffer creation succeeds");
  RequireBufferAllocation(&buffer, vulkan,
                          vulkan ? "readback Vulkan buffer owns allocation and native buffer"
                                 : "readback buffer owns a D3D12MA allocation");
  Require(buffer.mappedAddress != nullptr, "readback buffer is mapped");
  if (!vulkan)
    Require(buffer.GetDeviceHandle(device) > 0,
            "readback buffer has a GPU handle");
  buffer.invalidateMappedRange(device, 0, 0);
  buffer.dispose(device);
  RequireDisposed(device, &buffer, vulkan, "readback buffer is empty after dispose");

  desc = {};
  desc.size = 0;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  desc.location = RI_MEMORY_DEVICE;
  buffer = RIBuffer::create(device, desc);
  if (vulkan && !buffer.isEmpty()) {
    Skip("Vulkan may allocate the size-zero buffer");
    buffer.dispose(device);
  } else {
    Require(buffer.isEmpty(), "size-zero buffer creation fails");
    Require(buffer.cookie == 0, "size-zero buffer cookie is zero");
    Require(buffer.GetDeviceHandle(device) == 0,
            "size-zero buffer device handle is zero");
    buffer.dispose(device);
  }

  if (!vulkan) {
    auto requireAlignedBuffer = [&](uint64_t size, uint32_t usage,
                                    uint64_t alignment, const char *label) {
      RIBufferDesc alignedDesc = {};
      alignedDesc.size = size;
      alignedDesc.usage = usage;
      alignedDesc.location = RI_MEMORY_DEVICE;
      alignedDesc.alignment = alignment;
      RIBuffer aligned = RIBuffer::create(device, alignedDesc);
      Require(!aligned.isEmpty(), label);
      RequireBufferAllocation(&aligned, false,
                              "aligned buffer owns a D3D12MA allocation");
      uint64_t gpuAddress = aligned.GetDeviceHandle(device);
      Require(gpuAddress != 0 && gpuAddress % alignment == 0,
              "aligned buffer GPU address has requested modulus");
      Require(aligned.d3d12.requestedSize == size,
              "aligned buffer preserves requested size");
      aligned.dispose(device);
      RequireDisposed(device, &aligned, false,
                      "aligned buffer is empty after dispose");
      aligned.dispose(device);
    };

    desc = {};
    desc.size = 200;
    desc.usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_CONSTANT_BUFFER;
    desc.location = RI_MEMORY_HOST_UPLOAD;
    desc.alignment = 256;
    buffer = RIBuffer::create(device, desc);
    Require(!buffer.isEmpty(), "aligned upload constant buffer creation succeeds");
    RequireBufferAllocation(&buffer, false,
                            "aligned upload constant buffer owns a D3D12MA allocation");
    Require(buffer.mappedAddress != nullptr,
            "aligned upload constant buffer is mapped");
    const uint64_t alignedUploadAddress = buffer.GetDeviceHandle(device);
    Require(alignedUploadAddress != 0 && alignedUploadAddress % 256 == 0,
            "aligned upload constant buffer GPU address has 256 modulus");
    Require(buffer.d3d12.requestedSize == 200,
            "aligned upload constant buffer preserves requested size");
    buffer.dispose(device);
    RequireDisposed(device, &buffer, false,
                     "aligned upload constant buffer is empty after dispose");
    buffer.dispose(device);

    requireAlignedBuffer(
        4096, RI_BUFFER_USAGE_TRANSFER_DST | RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        "maximum-guaranteed aligned device storage buffer creation succeeds");
    requireAlignedBuffer(
        4096, RI_BUFFER_USAGE_SCRATCH,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
        "maximum-guaranteed aligned device scratch buffer creation succeeds");

    auto requireRejectedAlignment = [&](uint64_t alignment, const char *label) {
      RIBufferDesc rejectedDesc = {};
      rejectedDesc.size = 4096;
      rejectedDesc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
      rejectedDesc.location = RI_MEMORY_DEVICE;
      rejectedDesc.alignment = alignment;
      RIBuffer rejected = RIBuffer::create(device, rejectedDesc);
      Require(rejected.isEmpty(), label);
      RequireDisposed(device, &rejected, false,
                      "failed aligned buffer output is cleared");
      rejected.dispose(device);
    };

    requireRejectedAlignment(3, "non-power-of-two alignment is rejected");
    requireRejectedAlignment(512,
                             "alignment just above D3D12 guarantee is rejected");

    desc = {};
    desc.size = 4096;
    desc.usage = RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE;
    desc.location = RI_MEMORY_HOST_UPLOAD;
    buffer = RIBuffer::create(device, desc);
    Require(buffer.isEmpty(), "upload storage buffer is rejected");
    Require(buffer.cookie == 0, "upload storage buffer cookie is zero");
    Require(buffer.GetDeviceHandle(device) == 0,
            "upload storage buffer device handle is zero");
    buffer.dispose(device);

    desc.location = RI_MEMORY_HOST_READBACK;
    buffer = RIBuffer::create(device, desc);
    Require(buffer.isEmpty(), "readback storage buffer is rejected");
    Require(buffer.cookie == 0, "readback storage buffer cookie is zero");
    Require(buffer.GetDeviceHandle(device) == 0,
            "readback storage buffer device handle is zero");
    buffer.dispose(device);
  }

  desc = {};
  desc.size = 4096;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  desc.location = RI_MEMORY_DEVICE;
  RIBuffer first = RIBuffer::create(device, desc);
  Require(!first.isEmpty(), "first recreation buffer creation succeeds");
  RequireBufferAllocation(&first, vulkan,
                          vulkan ? "first recreation Vulkan buffer owns allocation and native buffer"
                                 : "first recreation buffer owns a D3D12MA allocation");
  hash_t firstCookie = first.cookie;
  first.dispose(device);
  RIBuffer second = RIBuffer::create(device, desc);
  Require(!second.isEmpty(), "second recreation buffer creation succeeds");
  RequireBufferAllocation(&second, vulkan,
                          vulkan ? "second recreation Vulkan buffer owns allocation and native buffer"
                                 : "second recreation buffer owns a D3D12MA allocation");
  Require(second.cookie != firstCookie, "recreated buffers have distinct cookies");
  second.dispose(device);

  buffer = RIBuffer::create(device, desc, std::optional<hash_t>{0xdeadbeefull});
  Require(!buffer.isEmpty(), "explicit-cookie buffer creation succeeds");
  RequireBufferAllocation(&buffer, vulkan,
                          vulkan ? "explicit-cookie Vulkan buffer owns allocation and native buffer"
                                 : "explicit-cookie buffer owns a D3D12MA allocation");
  Require(buffer.cookie == 0xdeadbeefull, "explicit cookie is preserved");
  buffer.setDebugObjectName(device, "RID3D12BufferSmoke.ExplicitCookie");
  buffer.dispose(device);
  RequireDisposed(device, &buffer, vulkan,
                  "explicit-cookie buffer is empty after dispose");
  buffer.dispose(device);

  RIBuffer invalid = RIBuffer::create(
      device, {0, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_DEVICE, 0},
      std::optional<hash_t>{0xdeadbeefull});
  if (vulkan && !invalid.isEmpty()) {
    Skip("Vulkan may allocate the size-zero explicit-cookie buffer");
    invalid.dispose(device);
  } else {
    Require(invalid.isEmpty(), "failed explicit-cookie buffer is empty");
    Require(invalid.cookie == 0, "failed explicit-cookie buffer cookie is zero");
    invalid.dispose(device);
  }

  RIBuffer empty;
  Require(empty.GetDeviceHandle(device) == 0,
          "empty buffer device handle is zero");
  empty.setDebugObjectName(device, "RID3D12BufferSmoke.Empty");
}

void RunBufferCycle(bool vulkan) {
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName = vulkan ? "RIVulkanBufferSmoke" : "RID3D12BufferSmoke";
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
    for (uint32_t i = 0; i < capacity; ++i) {
      if (!adapters[i].d3d12.isWarp) {
        selected = i;
        break;
      }
    }
    bool foundHardware = !adapters[selected].d3d12.isWarp;
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
  if (!vulkan)
    Require(device.d3d12.allocator != nullptr,
            "successful D3D12 buffer device owns a D3D12MA allocator");
  RunBufferCases(&device, vulkan);
  device.dispose();
  if (!vulkan)
    Require(device.d3d12.device == nullptr && device.d3d12.allocator == nullptr,
            "D3D12 buffer device disposal clears device and allocator handles");
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
  RunBufferCycle(vulkan);
  return 0;
}
