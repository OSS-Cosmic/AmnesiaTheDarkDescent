// Opt-in D3D12 smoke test for RI resource-uploader mip generation.
#include "graphics/RID3D12.h"
#include "graphics/RIDevice.h"
#include "graphics/RICommand.h"
#include "graphics/RIFormat.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"

#include <D3D12MemAlloc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

int hplMain(const std::string &) { return 0; }

namespace {

[[noreturn]] void Fail(const char *check) {
  std::fprintf(stderr, "FAIL: %s\n", check);
  std::fflush(stderr);
  std::_Exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition) Fail(check);
  std::printf("PASS: %s\n", check);
  std::fflush(stdout);
}

uint32_t FullMipCount(uint32_t width, uint32_t height, uint32_t depth) {
  const uint32_t maxDimension = std::max(width, std::max(height, depth));
  uint32_t count = 1;
  for (uint32_t dimension = maxDimension; dimension > 1; dimension >>= 1)
    ++count;
  return count;
}

RITexture MakeTexture(RIDevice *device, uint32_t format, uint32_t width,
                      uint32_t height, uint32_t depth, uint32_t mips,
                      uint32_t layers) {
  Require(mips == FullMipCount(width, height, depth),
          "texture mip count is floor(log2(max dimension)) plus one");
  RITextureDesc desc = {};
  desc.type = depth > 1 ? RI_TEXTURE_3D : RI_TEXTURE_2D;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.depth = depth;
  desc.mipNum = mips;
  desc.layerNum = layers;
  desc.sampleCount = 1;
  desc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE |
               RI_USAGE_TRANSFER_SRC |
               RI_USAGE_TRANSFER_DST;
  RITexture texture = RITexture::create(device, desc);
  Require(texture.d3d12.resource != nullptr && texture.d3d12.allocation != nullptr,
          "RI mip texture owns a D3D12MA allocation");
  Require(texture.d3d12.allocation->GetResource() == texture.d3d12.resource,
          "RI mip texture allocation matches its D3D12 resource");
  return texture;
}

void RequireTextureDisposed(RITexture *texture, const char *check) {
  Require(texture->isEmpty() && texture->d3d12.resource == nullptr &&
              texture->d3d12.allocation == nullptr,
          check);
}

void RequireMipScratchAllocations(RIResourceUploader *uploader) {
  for (uint32_t set = 0; set < 2; ++set) {
    for (const RID3D12MipScratch &scratch :
         uploader->d3d12_mip_generation.scratch[set]) {
      Require(scratch.source.d3d12.resource != nullptr &&
                  scratch.source.d3d12.allocation != nullptr &&
                  scratch.source.d3d12.allocation->GetResource() ==
                      scratch.source.d3d12.resource &&
                  scratch.intermediate.d3d12.resource != nullptr &&
                  scratch.intermediate.d3d12.allocation != nullptr &&
                  scratch.intermediate.d3d12.allocation->GetResource() ==
                      scratch.intermediate.d3d12.resource,
              "internal mip textures own matching D3D12MA allocations");
    }
  }
}

uint32_t BytesPerTexel(uint32_t format) {
  if (format == RI_FORMAT_RGBA16_SFLOAT)
    return 8u;
  if (format == RI_FORMAT_RGBA32_SFLOAT)
    return 16u;
  return 4u;
}

std::vector<uint8_t> Rgba8Pattern(uint32_t width, uint32_t height,
                                  uint32_t depth, uint8_t seed,
                                  bool checker) {
  const uint32_t slices = depth > 1 ? depth : 1;
  std::vector<uint8_t> result(size_t(width) * height * slices * 4);
  for (uint32_t z = 0; z < slices; ++z)
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const size_t i = (size_t(z) * height * width + size_t(y) * width + x) * 4;
        const uint8_t value = checker ? (((x + y + z) & 1) ? 255 : 0)
                                      : uint8_t(seed + x * 13 + y * 7 + z * 3);
        result[i + 0] = value;
        result[i + 1] = uint8_t(255 - value);
        result[i + 2] = uint8_t(seed + z * 17);
        result[i + 3] = 255;
      }
  return result;
}

std::vector<uint8_t> Rgba8Constant(uint32_t width, uint32_t height,
                                   uint32_t depth, uint8_t value) {
  const uint32_t slices = depth > 1 ? depth : 1;
  std::vector<uint8_t> result(size_t(width) * height * slices * 4);
  for (size_t i = 0; i < result.size(); i += 4) {
    result[i + 0] = value;
    result[i + 1] = value;
    result[i + 2] = value;
    result[i + 3] = 255;
  }
  return result;
}

std::vector<uint8_t> Rgba16Constant(uint32_t width, uint32_t height,
                                    uint32_t depth, uint16_t r, uint16_t g,
                                    uint16_t b, uint16_t a) {
  const uint32_t slices = depth > 1 ? depth : 1;
  std::vector<uint8_t> result(size_t(width) * height * slices * 8);
  for (size_t i = 0; i < result.size(); i += 8) {
    std::memcpy(result.data() + i + 0, &r, 2);
    std::memcpy(result.data() + i + 2, &g, 2);
    std::memcpy(result.data() + i + 4, &b, 2);
    std::memcpy(result.data() + i + 6, &a, 2);
  }
  return result;
}

std::vector<uint8_t> Rgba32Constant(uint32_t width, uint32_t height,
                                    uint32_t depth, uint32_t r, uint32_t g,
                                    uint32_t b, uint32_t a) {
  const uint32_t slices = depth > 1 ? depth : 1;
  std::vector<uint8_t> result(size_t(width) * height * slices * 16);
  for (size_t i = 0; i < result.size(); i += 16) {
    std::memcpy(result.data() + i + 0, &r, 4);
    std::memcpy(result.data() + i + 4, &g, 4);
    std::memcpy(result.data() + i + 8, &b, 4);
    std::memcpy(result.data() + i + 12, &a, 4);
  }
  return result;
}

void Upload(RIDevice *device, RIResourceUploader *uploader, RITexture *texture,
            uint32_t format, uint32_t mip, uint32_t layer, uint32_t width,
            uint32_t height, uint32_t depth, const std::vector<uint8_t> &data) {
  const uint32_t bpp = BytesPerTexel(format);
  RIResourceTextureTransaction trans = {};
  trans.target = *texture;
  trans.format = format;
  trans.width = width;
  trans.height = height;
  trans.depth = depth;
  trans.sliceNum = height;
  trans.rowPitch = width * bpp;
  trans.arrayOffset = layer;
  trans.mipOffset = mip;
  trans.currentState = RI_RESOURCE_STATE_UNDEFINED;
  trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  trans.postStages = RI_STAGE_ALL_SHADER;
  RI_ResourceBeginCopyTexture(device, uploader, &trans);
  Require(trans.mapped.data != nullptr, "mip smoke upload maps source texture");
  const uint32_t slices = depth > 1 ? depth : 1;
  for (uint32_t z = 0; z < slices; ++z)
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(static_cast<uint8_t *>(trans.mapped.data) +
                      size_t(z) * trans.alignSlicePitch + size_t(y) * trans.alignRowPitch,
                  data.data() + (size_t(z) * height + y) * width * bpp,
                  size_t(width) * bpp);
  RI_ResourceEndCopyTexture(device, uploader, &trans);
}

void Flush(RIDevice *device, RIResourceUploader *uploader) {
  RIResourceUploaderD3D12Result result = RI_D3D12FlushResourceUpdate(device, uploader);
  if (result.signaled) {
    Require(result.timeline != nullptr && result.value != 0,
            "mip smoke flush returns a D3D12 timeline signal");
    result.timeline->wait(device, result.value);
  }
}

void Generate(RIDevice *device, RIResourceUploader *uploader, RITexture *texture,
              uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
              uint32_t mips, uint32_t arrayOffset, uint32_t layerNum) {
  RIGenerateMipsDesc desc = {};
  desc.target = *texture;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.depth = depth;
  desc.mipNum = mips;
  desc.arrayOffset = arrayOffset;
  desc.layerNum = layerNum;
  desc.currentState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  desc.currentStages = RI_STAGE_ALL_SHADER;
  desc.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  desc.postStages = RI_STAGE_ALL_SHADER;
  RI_ResourceGenerateMips(device, uploader, &desc);
  RequireMipScratchAllocations(uploader);
  Flush(device, uploader);
}

std::vector<uint8_t> Readback(RIDevice *device, RITexture *texture, uint32_t mip,
                              uint32_t layer) {
  D3D12_RESOURCE_DESC resource = texture->d3d12.resource->GetDesc();
  const uint32_t subresource = texture->d3d12.depth > 1
                                   ? mip
                                   : mip + layer * texture->d3d12.mipNum;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 rowSize = 0;
  UINT64 total = 0;
  device->d3d12.device->GetCopyableFootprints(&resource, subresource, 1, 0,
                                               &footprint, &rows, &rowSize, &total);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc = {1, 0};
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *readback = nullptr;
  Require(SUCCEEDED(device->d3d12.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&readback))), "mip smoke readback buffer creates");
  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  RITextureBarrier barrier(texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                           RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_ALL_SHADER, RI_STAGE_COPY);
  barrier.baseMip = uint16_t(mip);
  barrier.mipCount = 1;
  barrier.baseLayer = uint16_t(layer);
  barrier.layerCount = 1;
  cmd.vk_d3d12_textureBarrier(barrier);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = texture->d3d12.resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  cmd.d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier.before = RI_RESOURCE_STATE_COPY_SRC;
  barrier.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
  barrier.beforeStages = RI_STAGE_COPY;
  barrier.afterStages = RI_STAGE_ALL_SHADER;
  cmd.vk_d3d12_textureBarrier(barrier);
  cmd.end(device);
  ID3D12CommandList *lists[] = {cmd.d3d12.cmdList};
  device->queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1, lists);
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  void *mapped = nullptr;
  D3D12_RANGE range = {0, SIZE_T(total)};
  Require(SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped,
          "mip smoke readback maps");
  std::vector<uint8_t> result(static_cast<uint8_t *>(mapped),
                              static_cast<uint8_t *>(mapped) + total);
  readback->Unmap(0, nullptr);
  cmd.dispose(device);
  pool.dispose(device);
  readback->Release();
  return result;
}

void CheckLevelHasTexel(RIDevice *device, RITexture *texture, uint32_t mip,
                        uint32_t layer, uint8_t expected, const char *name) {
  const auto bytes = Readback(device, texture, mip, layer);
  Require(!bytes.empty(), name);
  Require(std::abs(int(bytes[0]) - int(expected)) <= 2, name);
}

void CheckAllLevelsReadable(RIDevice *device, RITexture *texture,
                            uint32_t mipNum, uint32_t layer,
                            const char *name) {
  for (uint32_t mip = 0; mip < mipNum; ++mip)
    Require(!Readback(device, texture, mip, layer).empty(), name);
}

void Run(RIDevice *device, RIResourceUploader *uploader) {
  Require(RI_FormatSupportsMipGeneration(device, RI_FORMAT_RGBA8_UNORM),
          "RGBA8 linear format advertises mip generation");
  Require(RI_FormatSupportsMipGeneration(device, RI_FORMAT_RGBA8_SRGB),
          "RGBA8 sRGB format advertises mip generation");
  Require(RI_FormatSupportsMipGeneration(device, RI_FORMAT_RGBA16_SFLOAT),
          "RGBA16 float format advertises mip generation");
  Require(RI_FormatSupportsMipGeneration(device, RI_FORMAT_RGBA32_SFLOAT),
          "RGBA32 float format advertises mip generation");
  Require(!RI_FormatSupportsMipGeneration(device, RI_FORMAT_RGBA8_UINT),
          "integer formats do not advertise mip generation");

  RITexture linear = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 7, 5, 1, 3, 1);
  Require(!linear.isEmpty(), "linear odd-size texture creates");
  Upload(device, uploader, &linear, RI_FORMAT_RGBA8_UNORM, 0, 0, 7, 5, 1,
         Rgba8Constant(7, 5, 1, 64));
  Generate(device, uploader, &linear, RI_FORMAT_RGBA8_UNORM, 7, 5, 1, 3, 0, 1);
  for (uint32_t mip = 0; mip < 3; ++mip)
    CheckLevelHasTexel(device, &linear, mip, 0, 64,
                       "linear odd-size mip readback matches reference");
  linear.dispose(device);
  RequireTextureDisposed(&linear, "linear texture disposal clears allocation handles");

  RITexture srgb = MakeTexture(device, RI_FORMAT_RGBA8_SRGB, 2, 2, 1, 2, 1);
  Require(!srgb.isEmpty(), "sRGB checkerboard texture creates");
  Upload(device, uploader, &srgb, RI_FORMAT_RGBA8_SRGB, 0, 0, 2, 2, 1,
         Rgba8Pattern(2, 2, 1, 41, true));
  Generate(device, uploader, &srgb, RI_FORMAT_RGBA8_SRGB, 2, 2, 1, 2, 0, 1);
  CheckLevelHasTexel(device, &srgb, 1, 0, 188,
                     "sRGB mip decodes, filters in linear light, and re-encodes");
  srgb.dispose(device);
  RequireTextureDisposed(&srgb, "sRGB texture disposal clears allocation handles");

  const uint32_t hdrMips = FullMipCount(5, 3, 1);
  RITexture hdr = MakeTexture(device, RI_FORMAT_RGBA16_SFLOAT, 5, 3, 1,
                              hdrMips, 1);
  Require(!hdr.isEmpty(), "HDR texture creates");
  // Half values 1.5, 0.25, 4.0, 1.0; constants must survive every reduction.
  Upload(device, uploader, &hdr, RI_FORMAT_RGBA16_SFLOAT, 0, 0, 5, 3, 1,
         Rgba16Constant(5, 3, 1, 0x3E00, 0x3400, 0x4400, 0x3C00));
  Generate(device, uploader, &hdr, RI_FORMAT_RGBA16_SFLOAT, 5, 3, 1,
           hdrMips, 0, 1);
  for (uint32_t mip = 0; mip < hdrMips; ++mip) {
    const auto bytes = Readback(device, &hdr, mip, 0);
    Require(bytes.size() >= 8 && bytes[0] == 0x00 && bytes[1] == 0x3E &&
                bytes[2] == 0x00 && bytes[3] == 0x34 && bytes[4] == 0x00 &&
                bytes[5] == 0x44,
            "HDR mip preserves linear floating-point values");
  }
  hdr.dispose(device);
  RequireTextureDisposed(&hdr, "HDR texture disposal clears allocation handles");

  const uint32_t hdr32Mips = FullMipCount(5, 3, 1);
  RITexture hdr32 = MakeTexture(device, RI_FORMAT_RGBA32_SFLOAT, 5, 3, 1,
                                hdr32Mips, 1);
  Require(!hdr32.isEmpty(), "RGBA32F HDR texture creates");
  // Float values 1.5, 0.25, 4.0, 1.0; constants must survive every reduction.
  Upload(device, uploader, &hdr32, RI_FORMAT_RGBA32_SFLOAT, 0, 0, 5, 3, 1,
         Rgba32Constant(5, 3, 1, 0x3FC00000, 0x3E800000, 0x40800000,
                       0x3F800000));
  Generate(device, uploader, &hdr32, RI_FORMAT_RGBA32_SFLOAT, 5, 3, 1,
           hdr32Mips, 0, 1);
  for (uint32_t mip = 0; mip < hdr32Mips; ++mip) {
    const auto bytes = Readback(device, &hdr32, mip, 0);
    Require(bytes.size() >= 16 && bytes[0] == 0x00 && bytes[1] == 0x00 &&
                bytes[2] == 0xC0 && bytes[3] == 0x3F &&
                bytes[4] == 0x00 && bytes[5] == 0x00 &&
                bytes[6] == 0x80 && bytes[7] == 0x3E &&
                bytes[8] == 0x00 && bytes[9] == 0x00 &&
                bytes[10] == 0x80 && bytes[11] == 0x40 &&
                bytes[12] == 0x00 && bytes[13] == 0x00 &&
                bytes[14] == 0x80 && bytes[15] == 0x3F,
            "RGBA32F HDR mip preserves linear floating-point values");
  }
  hdr32.dispose(device);
  RequireTextureDisposed(&hdr32, "RGBA32F texture disposal clears allocation handles");

  RITexture degenerate = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 1, 7, 1, 3, 1);
  Require(!degenerate.isEmpty(), "1xN texture creates");
  Upload(device, uploader, &degenerate, RI_FORMAT_RGBA8_UNORM, 0, 0, 1, 7, 1,
         Rgba8Constant(1, 7, 1, 77));
  Generate(device, uploader, &degenerate, RI_FORMAT_RGBA8_UNORM, 1, 7, 1, 3, 0, 1);
  CheckAllLevelsReadable(device, &degenerate, 3, 0,
                         "1xN every mip level reads back");
  CheckLevelHasTexel(device, &degenerate, 2, 0, 77, "1xN mip chain clamps dimensions to one");
  degenerate.dispose(device);
  RequireTextureDisposed(&degenerate,
                        "1xN texture disposal clears allocation handles");

  RITexture row = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 7, 1, 1, 3, 1);
  Require(!row.isEmpty(), "Nx1 texture creates");
  Upload(device, uploader, &row, RI_FORMAT_RGBA8_UNORM, 0, 0, 7, 1, 1,
         Rgba8Constant(7, 1, 1, 93));
  Generate(device, uploader, &row, RI_FORMAT_RGBA8_UNORM, 7, 1, 1, 3, 0, 1);
  CheckAllLevelsReadable(device, &row, 3, 0,
                         "Nx1 every mip level reads back");
  CheckLevelHasTexel(device, &row, 2, 0, 93, "Nx1 mip chain clamps dimensions to one");
  row.dispose(device);
  RequireTextureDisposed(&row, "Nx1 texture disposal clears allocation handles");

  RITexture array = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 4, 4, 1, 3, 3);
  Require(!array.isEmpty(), "array texture creates");
  for (uint32_t layer = 0; layer < 3; ++layer)
    Upload(device, uploader, &array, RI_FORMAT_RGBA8_UNORM, 0, layer, 4, 4, 1,
           Rgba8Constant(4, 4, 1, uint8_t(20 + layer * 60)));
  Generate(device, uploader, &array, RI_FORMAT_RGBA8_UNORM, 4, 4, 1, 3, 1, 1);
  CheckAllLevelsReadable(device, &array, 3, 1,
                         "selected array layer every mip level reads back");
  CheckLevelHasTexel(device, &array, 0, 0, 20, "array subrange keeps untouched base layer");
  CheckLevelHasTexel(device, &array, 1, 1, 80, "array subrange generates selected layer");
  CheckLevelHasTexel(device, &array, 0, 2, 140, "array subrange keeps untouched layer base level");
  array.dispose(device);
  RequireTextureDisposed(&array, "array texture disposal clears allocation handles");

  RITexture volume = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 5, 3, 3, 3, 1);
  Require(!volume.isEmpty(), "3D texture creates");
  Upload(device, uploader, &volume, RI_FORMAT_RGBA8_UNORM, 0, 0, 5, 3, 3,
         Rgba8Constant(5, 3, 3, 91));
  Generate(device, uploader, &volume, RI_FORMAT_RGBA8_UNORM, 5, 3, 3, 3, 0, 1);
  CheckAllLevelsReadable(device, &volume, 3, 0,
                         "3D every mip level reads back");
  CheckLevelHasTexel(device, &volume, 2, 0, 91, "3D mip chain addresses volume slices");
  volume.dispose(device);
  RequireTextureDisposed(&volume, "3D texture disposal clears allocation handles");

  // Repeated batches force uploader command-set reuse and timeline waits.
  for (uint32_t batch = 0; batch < 5; ++batch) {
    RITexture texture = MakeTexture(device, RI_FORMAT_RGBA8_UNORM, 4, 4, 1, 3, 1);
    Require(!texture.isEmpty(), "batched mip texture creates");
    Upload(device, uploader, &texture, RI_FORMAT_RGBA8_UNORM, 0, 0, 4, 4, 1,
           Rgba8Constant(4, 4, 1, uint8_t(10 + batch * 20)));
    Generate(device, uploader, &texture, RI_FORMAT_RGBA8_UNORM, 4, 4, 1, 3, 0, 1);
    CheckLevelHasTexel(device, &texture, 2, 0, uint8_t(10 + batch * 20),
                       "mip generation survives batches spanning command sets");
    texture.dispose(device);
    RequireTextureDisposed(&texture,
                          "batched texture disposal clears allocation handles");
  }
}

} // namespace

int main(int argc, char **argv) {
  bool debugLayer = true;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--no-debug-layer") == 0) debugLayer = false;
  if (debugLayer) g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12MipGenerationSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 mip renderer initializes");
  uint32_t count = 0;
  Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count,
          "mip smoke adapter enumeration succeeds");
  RIPhysicalAdapter adapters[8] = {};
  uint32_t capacity = std::min(count, 8u);
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS && capacity,
          "mip smoke adapter enumeration populates adapters");
  uint32_t selected = 0;
  for (uint32_t i = 0; i < capacity; ++i)
    if (!adapters[i].d3d12.isWarp) { selected = i; break; }
  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 mip device initializes");
  Require(device.d3d12.allocator != nullptr,
          "successful D3D12 mip device owns a D3D12MA allocator");
  RIResourceUploader uploader = {};
  RI_InitResourceUploader(&device, &uploader);
  Require(uploader.upload_resource.queue != nullptr, "mip uploader initializes");
  for (uint32_t i = 0; i < 2; ++i) {
    Require(uploader.d3d12_mip_generation.constant_buffer[i] != nullptr &&
                uploader.d3d12_mip_generation.constant_allocation[i] != nullptr,
            "mip constant buffer owns a D3D12MA allocation");
    Require(uploader.d3d12_mip_generation.constant_allocation[i]->GetResource() ==
                uploader.d3d12_mip_generation.constant_buffer[i],
            "mip constant buffer allocation matches its D3D12 resource");
  }
  Run(&device, &uploader);
  RI_FreeResourceUploader(&device, &uploader);
  Require(!uploader.d3d12_mip_generation.isReady() &&
              uploader.d3d12_mip_generation.constant_buffer[0] == nullptr &&
              uploader.d3d12_mip_generation.constant_buffer[1] == nullptr &&
              uploader.d3d12_mip_generation.constant_allocation[0] == nullptr &&
              uploader.d3d12_mip_generation.constant_allocation[1] == nullptr &&
              uploader.d3d12_mip_generation.scratch[0].empty() &&
              uploader.d3d12_mip_generation.scratch[1].empty(),
          "mip disposal clears constant and internal allocation handles");
  device.dispose();
  Require(device.d3d12.device == nullptr && device.d3d12.allocator == nullptr,
          "D3D12 mip device disposal clears device and allocator handles");
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
  return 0;
}
