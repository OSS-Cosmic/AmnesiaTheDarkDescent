// Opt-in GPU smoke test for the RI resource uploader lifecycle and buffer transfers.
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIScratchAlloc.h"
#include "graphics/RITimeline.h"
#include "graphics/RIVK.h"
#include "system/stb_ds.h"

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
}

void Skip(const char *check) { std::printf("SKIP: %s\n", check); }

uint8_t RequestedApi(bool vulkan) {
  return vulkan ? RI_DEVICE_API_VK : RI_DEVICE_API_D3D12;
}

RIBuffer CreateDeviceBuffer(RIDevice *device, size_t size, uint32_t extraUsage = 0) {
  RIBufferDesc desc = {};
  desc.size = size;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST | extraUsage;
  desc.location = RI_MEMORY_DEVICE;
  return RIBuffer::create(device, desc);
}

RIBuffer CreateReadbackBuffer(RIDevice *device, size_t size) {
  RIBufferDesc desc = {};
  desc.size = size;
  desc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
  desc.location = RI_MEMORY_HOST_READBACK;
  return RIBuffer::create(device, desc);
}

void CopyDeviceToReadback(RIDevice *device, RIBuffer *src, RIBuffer *dst,
                          size_t size, size_t srcOffset = 0, size_t dstOffset = 0) {
  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(dst, RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_COPY_DST));
  cmd.copyBuffer(device, src, srcOffset, dst, dstOffset, size);
  cmd.end(device);
  RICmd *commands[] = {&cmd};
  RISubmitDesc submit = {};
  submit.cmds = commands;
  submit.cmdCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) == RI_SUCCESS,
          "readback copy submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  cmd.dispose(device);
  pool.dispose(device);
}

void FlushUploader(RIDevice *device, RIResourceUploader *uploader, bool vulkan) {
  if (vulkan) {
#if DEVICE_IMPL_VULKAN
    RIResourceUploaderVKResult result = RI_VKFlushResourceUpdate(device, uploader, 0, nullptr);
    if (result.signaled)
      Require(vkWaitForFences(device->vk.device, 1, &result.vk.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
              "Vulkan uploader fence wait succeeds");
#else
    (void)device; (void)uploader;
#endif
  } else {
#if DEVICE_IMPL_D3D12
    RIResourceUploaderD3D12Result result = RI_D3D12FlushResourceUpdate(device, uploader);
    if (result.signaled) {
      Require(result.timeline != nullptr && result.value != 0,
              "D3D12 uploader returns a timeline signal");
      result.timeline->wait(device, result.value);
    }
#else
    (void)device; (void)uploader;
#endif
  }
}

void RunScratchAllocatorSmoke(RIDevice *device, bool vulkan) {
  std::printf("INFO: %s scratch allocator coverage\n", vulkan ? "Vulkan" : "D3D12");
  const size_t uniformAlignment =
      device->physicalAdapter.constantBufferOffsetAlignment;
  Require(uniformAlignment != 0 && (uniformAlignment & (uniformAlignment - 1)) == 0,
          "uniform scratch alignment is a nonzero power of two");

  RIScratchAlloc uniform = {};
  RIScratchAllocDesc uniformDesc = {};
  // Reserve one aligned unit so the requests under test have nonzero offsets;
  // the two following aligned requests then fill the rest of this block.
  uniformDesc.blockSize = uniformAlignment * 3;
  uniformDesc.alignmentReq = uniformAlignment;
  uniformDesc.alloc = RIUniformScratchAllocHandler;
  InitRIScratchAlloc(device, &uniform, &uniformDesc);

  RIBufferScratchAllocReq prefix = RIAllocBufferFromScratchAlloc(device, &uniform, 1);
#if DEVICE_IMPL_D3D12
  if (!vulkan) {
    Require((uniform.current.buffer.d3d12.usage &
             RI_BUFFER_USAGE_DEVICE_ADDRESS) == 0 &&
                uniform.current.buffer.d3d12.shaderResourceIndex == UINT32_MAX,
            "D3D12 uniform scratch blocks do not consume geometry descriptors");
  }
#endif
  RIFinishScrachReq(device, &prefix);
  RIBufferScratchAllocReq first = RIAllocBufferFromScratchAlloc(device, &uniform, 1);
  RIBufferScratchAllocReq second = RIAllocBufferFromScratchAlloc(device, &uniform, 17);
  Require(first.pMappedAddress != nullptr && second.pMappedAddress != nullptr,
          "uniform scratch allocations are host mapped");
  Require(first.bufferOffset % uniformAlignment == 0 && second.bufferOffset > first.bufferOffset &&
              second.bufferOffset % uniformAlignment == 0,
          "uniform scratch allocations use aligned nonzero offsets");
  const size_t firstConsumed = ((size_t(1) + uniformAlignment - 1) / uniformAlignment) *
                               uniformAlignment;
  Require(first.bufferOffset + firstConsumed <= second.bufferOffset,
          "uniform scratch allocations do not overlap");
  auto *firstBytes = static_cast<uint8_t *>(first.pMappedAddress) +
                     first.bufferOffset;
  auto *secondBytes = static_cast<uint8_t *>(second.pMappedAddress) +
                      second.bufferOffset;
  std::memset(firstBytes, 0x31, first.bufferSize);
  std::memset(secondBytes, 0xA7, second.bufferSize);
  Require(firstBytes[0] == 0x31 && secondBytes[0] == 0xA7,
          "uniform scratch allocations preserve distinct patterns");
  RIFinishScrachReq(device, &first);
  RIFinishScrachReq(device, &second);

  RIBufferScratchAllocReq rollover =
      RIAllocBufferFromScratchAlloc(device, &uniform, uniformAlignment);
  Require(rollover.bufferOffset % uniformAlignment == 0 &&
              rollover.block.buffer.cookie != first.block.buffer.cookie &&
              RINumberOfUsedBlock(device, &uniform) == 2,
          "uniform scratch allocation rolls over to a new block");
  RIFinishScrachReq(device, &rollover);

  const hash_t recycledCookie = first.block.buffer.cookie;
  RIResetScratchAlloc(device, &uniform);
  Require(uniform.blockOffset == 0 && arrlen(uniform.recycle) == 0 &&
              arrlen(uniform.pool) == 1 && arrlen(uniform.oversized) == 0,
          "scratch reset moves completed blocks to the reuse pool");

  // Fill the current block, then force a rollover so the block saved by reset
  // is selected from the reuse pool.
  RIAllocBufferFromScratchAlloc(device, &uniform, uniformAlignment);
  RIAllocBufferFromScratchAlloc(device, &uniform, uniformAlignment);
  RIAllocBufferFromScratchAlloc(device, &uniform, uniformAlignment);
  RIBufferScratchAllocReq reused =
      RIAllocBufferFromScratchAlloc(device, &uniform, uniformAlignment);
  Require(reused.block.buffer.cookie == recycledCookie && arrlen(uniform.pool) == 0,
          "scratch reset reuses a pooled block");
  RIFinishScrachReq(device, &reused);

  const size_t oversizedSize = uniformDesc.blockSize + 1;
  RIBufferScratchAllocReq oversized =
      RIAllocBufferFromScratchAlloc(device, &uniform, oversizedSize);
  Require(oversized.bufferOffset == 0 && oversized.bufferSize == oversizedSize &&
              oversized.pMappedAddress != nullptr && arrlen(uniform.oversized) == 1,
          "oversized uniform allocation uses a mapped one-shot block");
  RIFinishScrachReq(device, &oversized);
  RIResetScratchAlloc(device, &uniform);
  Require(arrlen(uniform.oversized) == 0,
          "scratch reset releases oversized one-shot blocks");
  FreeRIScratchAlloc(device, &uniform);
  Require(uniform.current.buffer.isEmpty(),
          "uniform scratch disposal releases the current block");

  RIScratchAlloc accel = {};
  RIScratchAllocDesc accelDesc = {};
  accelDesc.blockSize = 4096;
  accelDesc.alignmentReq =
      device->physicalAdapter.accelerationStructureScratchOffsetAlignment;
  accelDesc.alloc = RIAccelScratchAllocHandler;
  Require(accelDesc.alignmentReq != 0 &&
              (accelDesc.alignmentReq & (accelDesc.alignmentReq - 1)) == 0,
          "AS scratch alignment is a nonzero power of two");
  InitRIScratchAlloc(device, &accel, &accelDesc);
  RIBufferScratchAllocReq asScratch = RIAllocBufferFromScratchAlloc(device, &accel, 1024);
  Require(asScratch.block.buffer.isEmpty() == false && asScratch.pMappedAddress == nullptr &&
              asScratch.deviceAddress != 0 &&
              asScratch.deviceAddress % accelDesc.alignmentReq == 0,
          "AS scratch allocation is device-only and device-address aligned");
  RIFinishScrachReq(device, &asScratch);
  FreeRIScratchAlloc(device, &accel);
  Require(accel.current.buffer.isEmpty() && arrlen(accel.recycle) == 0 &&
              arrlen(accel.pool) == 0 && arrlen(accel.oversized) == 0,
          "AS scratch disposal releases all blocks");
}

void Upload(RIDevice *device, RIResourceUploader *uploader, RIBuffer *target,
            size_t size, size_t offset, const void *data, bool vulkan) {
  RIResourceBufferTransaction trans = {};
  trans.target = *target;
  trans.size = size;
  trans.offset = offset;
  trans.currentState = RI_RESOURCE_STATE_UNDEFINED;
  trans.postState = RI_RESOURCE_STATE_COPY_SRC;
  trans.postStages = RI_STAGE_COPY;
  RI_ResourceBeginCopyBuffer(device, uploader, &trans);
  Require(trans.mapped.data != nullptr && trans.mapped.size >= size,
          "uploader maps the requested range");
  std::memcpy(trans.mapped.data, data, size);
  RI_ResourceEndCopyBuffer(device, uploader, &trans);
  FlushUploader(device, uploader, vulkan);
}

struct TextureReadback {
  uint32_t rowPitch;
  std::vector<uint8_t> bytes;
};

TextureReadback CopyTextureSubresourceToReadback(RIDevice *device, RITexture *texture,
                                                  uint32_t mip, uint32_t layer,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t depth, uint32_t format,
                                                  uint32_t z = 0) {
  const RIFormatProps *props = GetRIFormatProps(format);
  const uint32_t rows = RIFormatBlockCount(height, props->blockHeight);
  const uint32_t rowBytes = RIFormatBlockCount(width, props->blockWidth) * props->stride;
  uint32_t rowPitch = (uint32_t)RIFormatAlignRowPitch(
      rowBytes, device->physicalAdapter.uploadBufferTextureRowAlignment, props->stride);
  size_t size = size_t(rowPitch) * rows * (depth ? depth : 1);
#if DEVICE_IMPL_D3D12
  // Size the readback buffer from GetCopyableFootprints total so multi-slice
  // 3D subresources satisfy the CopyTextureRegion id=856 debug check.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowsOutD3D = 0; UINT64 rowSizeD3D = 0; UINT64 totalD3D = 0;
  uint32_t subresourceD3D = 0;
  bool fullSubresource = false;
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    subresourceD3D = mip + layer * texture->d3d12.mipNum;
    D3D12_RESOURCE_DESC resourceDesc = texture->d3d12.resource->GetDesc();
    device->d3d12.device->GetCopyableFootprints(&resourceDesc, subresourceD3D, 1, 0, &footprint,
                                                  &rowsOutD3D, &rowSizeD3D, &totalD3D);
    if (resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
      footprint.Footprint.Depth = (depth ? depth : 1);
    }
    // GetCopyableFootprints returns block-aligned Width/Height for BC formats
    // (e.g. 8x8 for a 6x6 BC1 mip), so compare against the mip's texel extent
    // from the resource description instead.
    const uint32_t mipWidth  = std::max(1u, (uint32_t)(resourceDesc.Width  >> mip));
    const uint32_t mipHeight = std::max(1u, (uint32_t)(resourceDesc.Height >> mip));
    const uint32_t mipDepth  = resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                   ? std::max(1u, ((uint32_t)resourceDesc.DepthOrArraySize) >> mip)
                                   : 1;
    fullSubresource = (z == 0) && (width == mipWidth) && (height == mipHeight) &&
                      ((depth ? depth : 1) == mipDepth);
    size = (size_t)totalD3D;
    rowPitch = footprint.Footprint.RowPitch;
  }
#endif
  RIBuffer readback = CreateReadbackBuffer(device, size);
  Require(!readback.isEmpty(), "texture readback buffer is created");

  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  RITextureBarrier toCopy(texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                          RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_ALL_SHADER, RI_STAGE_COPY);
  toCopy.baseMip = (uint16_t)mip; toCopy.mipCount = 1;
  toCopy.baseLayer = (uint16_t)layer; toCopy.layerCount = 1;
  cmd.vk_d3d12_textureBarrier(toCopy);
#if DEVICE_IMPL_D3D12
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.d3d12.resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = texture->d3d12.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresourceD3D;
    D3D12_BOX box = {0, 0, z, width, height, z + (depth ? depth : 1)};
    // BC sources require a null or block-aligned source box per CopyTextureRegion docs (https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion); sub-block mips like 6x6 BC1 otherwise crash.
    cmd.d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, fullSubresource ? nullptr : &box);
  }
#endif
#if DEVICE_IMPL_VULKAN
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    VkBufferImageCopy region = {};
    region.bufferRowLength = rowPitch / props->stride * props->blockWidth;
    region.bufferImageHeight = rows * props->blockHeight;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mip;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, (int32_t)z};
    region.imageExtent = {width, height, depth ? depth : 1};
    vkCmdCopyImageToBuffer(cmd.vk.cmd, texture->vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.vk.buffer, 1, &region);
  }
#endif
  RITextureBarrier back(texture, RI_RESOURCE_STATE_COPY_SRC,
                        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY, RI_STAGE_ALL_SHADER);
  back.baseMip = (uint16_t)mip; back.mipCount = 1;
  back.baseLayer = (uint16_t)layer; back.layerCount = 1;
  cmd.vk_d3d12_textureBarrier(back);
  cmd.end(device);
  RICmd *commands[] = {&cmd};
  RISubmitDesc submit = {}; submit.cmds = commands; submit.cmdCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) == RI_SUCCESS,
          "texture readback copy submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  readback.invalidateMappedRange(device, 0, 0);
  TextureReadback result = {rowPitch, std::vector<uint8_t>((uint8_t *)readback.mappedAddress,
                                                            (uint8_t *)readback.mappedAddress + size)};
  cmd.dispose(device); pool.dispose(device); readback.dispose(device);
  return result;
}

RITexture CreateTestTexture(RIDevice *device, uint32_t type, uint32_t format,
                            uint32_t width, uint32_t height, uint32_t depth,
                            uint32_t mips, uint32_t layers, uint32_t flags = 0) {
  RITextureDesc desc = {};
  desc.type = (RITextureType_e)type; desc.format = format;
  desc.width = width; desc.height = height; desc.depth = depth;
  desc.mipNum = mips; desc.layerNum = layers; desc.sampleCount = RI_SAMPLE_COUNT_1;
  desc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC | RI_USAGE_TRANSFER_DST;
  desc.flags = flags;
  return RITexture::create(device, desc);
}

std::vector<uint8_t> MakeTexturePayload(uint32_t width, uint32_t height, uint32_t depth,
                                        uint32_t format, uint8_t seed) {
  const RIFormatProps *p = GetRIFormatProps(format);
  size_t row = RIFormatBlockCount(width, p->blockWidth) * p->stride;
  size_t rows = RIFormatBlockCount(height, p->blockHeight);
  std::vector<uint8_t> data(row * rows * (depth ? depth : 1));
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(seed + i * 17u + i / (row ? row : 1));
  return data;
}

void UploadTextureSubresource(RIDevice *device, RIResourceUploader *uploader, RITexture *texture,
                              uint32_t mip, uint32_t layer, uint32_t z, uint32_t width,
                              uint32_t height, uint32_t depth, uint32_t format,
                              const std::vector<uint8_t> &payload, bool vulkan,
                              bool requireOffset = false, bool alreadyShader = false) {
  const RIFormatProps *p = GetRIFormatProps(format);
  RIResourceTextureTransaction trans = {};
  trans.target = *texture; trans.format = format; trans.width = width; trans.height = height;
  trans.depth = depth; trans.sliceNum = RIFormatBlockCount(height, p->blockHeight);
  trans.rowPitch = RIFormatBlockCount(width, p->blockWidth) * p->stride;
  trans.arrayOffset = layer; trans.mipOffset = mip; trans.z = (uint16_t)z;
  trans.currentState = (z > 0 || alreadyShader) ? RI_RESOURCE_STATE_SHADER_RESOURCE
                                                  : RI_RESOURCE_STATE_UNDEFINED;
  trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  trans.postStages = RI_STAGE_ALL_SHADER;
  RI_ResourceBeginCopyTexture(device, uploader, &trans);
  Require(trans.mapped.data != nullptr, "texture uploader maps staging memory");
  if (requireOffset) Require(trans.mapped.offset > 0 &&
      trans.mapped.offset % device->physicalAdapter.uploadBufferOffsetAlignment == 0,
      "texture staging offset is nonzero and aligned");
  const size_t rowBytes = trans.rowPitch;
  const size_t rows = trans.sliceNum;
  for (uint32_t zSlice = 0; zSlice < (depth ? depth : 1); ++zSlice)
    for (size_t y = 0; y < rows; ++y)
      std::memcpy((uint8_t *)trans.mapped.data + zSlice * trans.alignSlicePitch + y * trans.alignRowPitch,
                  payload.data() + zSlice * rows * rowBytes + y * rowBytes, rowBytes);
  RI_ResourceEndCopyTexture(device, uploader, &trans);
  FlushUploader(device, uploader, vulkan);
}

bool CompareTexture(const TextureReadback &actual, const std::vector<uint8_t> &expected,
                    uint32_t width, uint32_t height, uint32_t depth, uint32_t format) {
  const RIFormatProps *p = GetRIFormatProps(format);
  size_t row = RIFormatBlockCount(width, p->blockWidth) * p->stride;
  size_t rows = RIFormatBlockCount(height, p->blockHeight);
  size_t slices = depth ? depth : 1;
  for (size_t z = 0; z < slices; ++z) for (size_t y = 0; y < rows; ++y)
    if (std::memcmp(actual.bytes.data() + z * actual.rowPitch * rows + y * actual.rowPitch,
                    expected.data() + z * row * rows + y * row, row) != 0) {
      const auto *got = actual.bytes.data() + z * actual.rowPitch * rows + y * actual.rowPitch;
      const auto *want = expected.data() + z * row * rows + y * row;
      std::fprintf(stderr,
                   "texture mismatch at z=%zu row=%zu: got=%02x expected=%02x\n",
                   z, y, unsigned(got[0]), unsigned(want[0]));
      return false;
    }
  return true;
}

void CheckAgilityExports() {
  HMODULE self = GetModuleHandleW(nullptr);
  Require(self != nullptr, "agility exports: process module handle is available");
  FARPROC versionSym = GetProcAddress(self, "D3D12SDKVersion");
  Require(versionSym != nullptr, "agility exports: D3D12SDKVersion is exported");
  Require(*reinterpret_cast<const UINT *>(versionSym) == 619u,
          "agility exports: D3D12SDKVersion equals 619");
  FARPROC pathSym = GetProcAddress(self, "D3D12SDKPath");
  Require(pathSym != nullptr, "agility exports: D3D12SDKPath is exported");
  const char *sdkPath = *reinterpret_cast<const char *const *>(pathSym);
  Require(sdkPath != nullptr && std::strcmp(sdkPath, ".\\D3D12\\") == 0,
          "agility exports: D3D12SDKPath equals .\\D3D12\\");
  HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
  if (!core) {
    std::printf("INFO: D3D12Core.dll not currently loaded (device create not yet performed or Windows selected system runtime)\n");
  } else {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(core, path, MAX_PATH);
    std::printf("INFO: D3D12Core loaded from %ls\n", path);
  }
}

void RunUploaderCycle(bool vulkan, bool enableDebugLayer) {
  if (!vulkan) g_riD3D12EnableDebugLayer = enableDebugLayer;
  RIBackendInit init = {};
  init.api = RequestedApi(vulkan);
  init.applicationName = vulkan ? "RIVulkanUploaderSmoke" : "RID3D12UploaderSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS,
          vulkan ? "Vulkan InitRIRenderer succeeds" : "D3D12 InitRIRenderer succeeds");

  uint32_t count = 0;
  Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count >= 1 && count <= 8,
          "adapter enumeration reports adapters");
  RIPhysicalAdapter adapters[8];
  uint32_t capacity = count;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS && capacity >= 1 && capacity <= 8,
          "adapter enumeration populates adapters");
  uint32_t selected = 0;
  if (!vulkan) {
    bool hardware = false;
    for (uint32_t i = 0; i < capacity; ++i) if (!adapters[i].d3d12.isWarp) { selected = i; hardware = true; break; }
    if (!hardware) for (uint32_t i = 0; i < capacity; ++i) if (adapters[i].d3d12.isWarp) { selected = i; break; }
    Require(hardware || adapters[selected].d3d12.isWarp, "D3D12 selects hardware or WARP adapter");
  }
  std::printf("INFO: %s adapter: %s\n", vulkan ? "RIVulkan" : "RID3D12", adapters[selected].name);

  RIDevice device;
  RIDeviceDesc desc = {};
  desc.physicalAdapter = &adapters[selected];
  Require(device.init(&desc) == RI_SUCCESS && RIDeviceIsValid(&device), "device initializes and is valid");
  if (!vulkan) CheckAgilityExports();
  RunScratchAllocatorSmoke(&device, vulkan);

  RIResourceUploader uploader = {};
  RI_InitResourceUploader(&device, &uploader);
  Require(uploader.upload_resource.queue != nullptr, "uploader graphics queue is populated");
  size_t initialSet = uploader.upload_resource.active_set;
  if (vulkan) {
    RIResourceUploaderVKResult r = RI_VKFlushResourceUpdate(&device, &uploader, 0, nullptr);
    Require(!r.signaled && uploader.upload_resource.active_set == initialSet,
            "empty Vulkan flush does not advance the active set");
  } else {
    RIResourceUploaderD3D12Result r = RI_D3D12FlushResourceUpdate(&device, &uploader);
    Require(!r.signaled && r.timeline == nullptr && r.value == 0 &&
                uploader.upload_resource.active_set == initialSet,
            "empty D3D12 flush returns no signal and does not advance the active set");
  }

  RI_FreeResourceUploader(&device, &uploader);
  RI_InitResourceUploader(&device, &uploader);
  Require(uploader.upload_resource.queue != nullptr, "uploader init/dispose/re-init preserves its queue");

  RIBuffer offsetBuffer = CreateDeviceBuffer(&device, 4096);
  Require(!offsetBuffer.isEmpty(), "offset-upload device buffer is created");
  std::vector<uint8_t> payload(256);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t((i * 13) ^ 0xA5);
  Upload(&device, &uploader, &offsetBuffer, payload.size(), 512, payload.data(), vulkan);
  RIBuffer smallReadback = CreateReadbackBuffer(&device, 4096);
  CopyDeviceToReadback(&device, &offsetBuffer, &smallReadback, 4096);
  smallReadback.invalidateMappedRange(&device, 0, 0);
  Require(std::memcmp((uint8_t *)smallReadback.mappedAddress + 512, payload.data(), payload.size()) == 0,
          "offset upload reads back byte-exactly");
  bool outsideZero = true;
  for (size_t i = 0; i < 512; ++i) outsideZero &= ((uint8_t *)smallReadback.mappedAddress)[i] == 0;
  for (size_t i = 768; i < 4096; ++i) outsideZero &= ((uint8_t *)smallReadback.mappedAddress)[i] == 0;
  Require(outsideZero, "offset upload leaves bytes outside the written window unchanged");

  const size_t bigSize = 10 * 1024 * 1024;
  RIBuffer big = CreateDeviceBuffer(&device, bigSize);
  Require(!big.isEmpty(), "overflow device buffer is created");
  std::vector<uint8_t> bigData(bigSize, 0);
  for (size_t i = 0; i < bigSize; i += 4096) {
    uint32_t block = uint32_t(i / 4096);
    std::memcpy(bigData.data() + i, &block, sizeof(block));
  }
  RIResourceBufferTransaction overflow = {};
  overflow.target = big;
  overflow.size = bigSize;
  overflow.currentState = RI_RESOURCE_STATE_UNDEFINED;
  overflow.postState = RI_RESOURCE_STATE_COPY_SRC;
  overflow.postStages = RI_STAGE_COPY;
  size_t active = uploader.upload_resource.active_set;
  RI_ResourceBeginCopyBuffer(&device, &uploader, &overflow);
  Require(overflow.mapped.data != nullptr && overflow.mapped.buffer.cookie != uploader.upload_resource.staging_buffer[active].cookie,
          "overflow upload spills into a temporary staging buffer");
  std::memcpy(overflow.mapped.data, bigData.data(), bigSize);
  RI_ResourceEndCopyBuffer(&device, &uploader, &overflow);
  FlushUploader(&device, &uploader, vulkan);
  RIBuffer bigReadback = CreateReadbackBuffer(&device, bigSize);
  CopyDeviceToReadback(&device, &big, &bigReadback, bigSize);
  bigReadback.invalidateMappedRange(&device, 0, 0);
  bool sparseOk = true;
  for (size_t window : {size_t(0), bigSize / 2, bigSize - 4096})
    sparseOk &= std::memcmp((uint8_t *)bigReadback.mappedAddress + window, bigData.data() + window, 4096) == 0;
  Require(sparseOk, "overflow upload sparse windows read back byte-exactly");

  std::vector<RIBuffer> cycleBuffers;
  std::vector<std::vector<uint8_t>> cyclePayloads;
  uint64_t previousSignal = 0;
  bool signalsMonotonic = true;
  for (int cycle = 0; cycle < 3; ++cycle) {
    cycleBuffers.push_back(CreateDeviceBuffer(&device, 64));
    cyclePayloads.emplace_back(64);
    for (size_t i = 0; i < 64; ++i) cyclePayloads.back()[i] = uint8_t(cycle * 67 + i);
    size_t set = uploader.upload_resource.active_set;
    Upload(&device, &uploader, &cycleBuffers.back(), 64, 0, cyclePayloads.back().data(), vulkan);
    if (!vulkan) {
#if DEVICE_IMPL_D3D12
      uint64_t signal = uploader.upload_resource.d3d12_set_signal_values[set];
      signalsMonotonic &= signal > previousSignal;
      previousSignal = signal;
#endif
    }
  }
  if (vulkan) Skip("Vulkan has no D3D12 set signal values to inspect");
  else Require(signalsMonotonic, "D3D12 set signal values increase across rotating sets");
  for (int i = 0; i < 3; ++i) {
    RIBuffer rb = CreateReadbackBuffer(&device, 64);
    CopyDeviceToReadback(&device, &cycleBuffers[i], &rb, 64);
    rb.invalidateMappedRange(&device, 0, 0);
    Require(std::memcmp(rb.mappedAddress, cyclePayloads[i].data(), 64) == 0,
            "rotating-set upload reads back byte-exactly");
    rb.dispose(&device);
  }

  RIBuffer pending = CreateDeviceBuffer(&device, 64);
  std::vector<uint8_t> pendingData(64, 0xD3);
  RIResourceBufferTransaction pendingTrans = {};
  pendingTrans.target = pending; pendingTrans.size = 64; pendingTrans.currentState = RI_RESOURCE_STATE_UNDEFINED;
  pendingTrans.postState = RI_RESOURCE_STATE_COPY_SRC; pendingTrans.postStages = RI_STAGE_COPY;
  RI_ResourceBeginCopyBuffer(&device, &uploader, &pendingTrans);
  std::memcpy(pendingTrans.mapped.data, pendingData.data(), pendingData.size());
  RI_ResourceEndCopyBuffer(&device, &uploader, &pendingTrans);
  RI_FreeResourceUploader(&device, &uploader);
  Require(true, "free with pending upload completes without hanging");
  RI_InitResourceUploader(&device, &uploader);
  Require(uploader.upload_resource.queue != nullptr, "uploader re-init works after pending cleanup");

  // Odd dimensions make the destination's row pitch visibly different from
  // the authored payload pitch, so comparison must skip row padding.
  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM,
                                          37, 17, 1, 1, 1);
    Require(!texture.isEmpty(), "odd-width RGBA8 texture is created");
    std::vector<uint8_t> data = MakeTexturePayload(37, 17, 1, RI_FORMAT_RGBA8_UNORM, 0x11);
    UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 37, 17, 1,
                             RI_FORMAT_RGBA8_UNORM, data, vulkan);
    TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                              37, 17, 1, RI_FORMAT_RGBA8_UNORM);
    Require(CompareTexture(read, data, 37, 17, 1, RI_FORMAT_RGBA8_UNORM),
            "odd-width RGBA8 texture reads back row-by-row");
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_BC1_RGBA_UNORM,
                                          16, 16, 1, 2, 1);
    Require(!texture.isEmpty(), "BC1 texture is created");
    for (uint32_t mip = 0; mip < 2; ++mip) {
      uint32_t extent = 16u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM,
                                                      uint8_t(0x90 + mip));
      UploadTextureSubresource(&device, &uploader, &texture, mip, 0, 0, extent, extent, 1,
                               RI_FORMAT_BC1_RGBA_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, 0,
                                                                extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM),
              "BC1 mip reads back block bytes exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_BC1_RGBA_UNORM,
                                          12, 12, 1, 3, 1);
    Require(!texture.isEmpty(), "BC1 12x12 texture is created");
    for (uint32_t mip = 0; mip < 3; ++mip) {
      uint32_t extent = 12u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM,
                                                      uint8_t(0x50 + mip));
      UploadTextureSubresource(&device, &uploader, &texture, mip, 0, 0, extent, extent, 1,
                               RI_FORMAT_BC1_RGBA_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, 0,
                                                                extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_BC1_RGBA_UNORM),
              "BC1 sub-block-aligned mip reads back block bytes exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_BC3_RGBA_UNORM,
                                          16, 16, 1, 2, 1);
    Require(!texture.isEmpty(), "BC3 texture is created");
    for (uint32_t mip = 0; mip < 2; ++mip) {
      uint32_t extent = 16u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_BC3_RGBA_UNORM,
                                                      uint8_t(0xB8 + mip));
      UploadTextureSubresource(&device, &uploader, &texture, mip, 0, 0, extent, extent, 1,
                               RI_FORMAT_BC3_RGBA_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, 0,
                                                                extent, extent, 1, RI_FORMAT_BC3_RGBA_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_BC3_RGBA_UNORM),
              "BC3 mip reads back block bytes exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_BC7_RGBA_UNORM,
                                          16, 16, 1, 2, 1);
    Require(!texture.isEmpty(), "BC7 texture is created");
    for (uint32_t mip = 0; mip < 2; ++mip) {
      uint32_t extent = 16u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM,
                                                      uint8_t(0xA0 + mip));
      UploadTextureSubresource(&device, &uploader, &texture, mip, 0, 0, extent, extent, 1,
                               RI_FORMAT_BC7_RGBA_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, 0,
                                                                extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM),
              "BC7 mip reads back block bytes exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_BC7_RGBA_UNORM,
                                          12, 12, 1, 3, 1);
    Require(!texture.isEmpty(), "BC7 12x12 texture is created");
    for (uint32_t mip = 0; mip < 3; ++mip) {
      uint32_t extent = 12u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM,
                                                      uint8_t(0x60 + mip));
      UploadTextureSubresource(&device, &uploader, &texture, mip, 0, 0, extent, extent, 1,
                               RI_FORMAT_BC7_RGBA_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, 0,
                                                                extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_BC7_RGBA_UNORM),
              "BC7 sub-block-aligned mip reads back block bytes exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGB32_SFLOAT,
                                          8, 8, 1, 1, 1);
    if (texture.isEmpty()) {
      Skip(vulkan ? "Vulkan RGB32_SFLOAT sampled-image support not present; upload coverage unavailable"
                  : "D3D12 RGB32_SFLOAT texture creation rejected; upload coverage unavailable");
    } else {
      Require(!texture.isEmpty(), "RGB32_SFLOAT texture is created");
      std::vector<uint8_t> data = MakeTexturePayload(8, 8, 1, RI_FORMAT_RGB32_SFLOAT, 0xE0);
      UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 8, 8, 1,
                               RI_FORMAT_RGB32_SFLOAT, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                                8, 8, 1, RI_FORMAT_RGB32_SFLOAT);
      Require(CompareTexture(read, data, 8, 8, 1, RI_FORMAT_RGB32_SFLOAT),
              "RGB32_SFLOAT texture reads back byte-exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA16_SFLOAT,
                                          12, 8, 1, 1, 1);
    Require(!texture.isEmpty(), "RGBA16_SFLOAT texture is created");
    std::vector<uint8_t> data = MakeTexturePayload(12, 8, 1, RI_FORMAT_RGBA16_SFLOAT, 0xC0);
    UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 12, 8, 1,
                             RI_FORMAT_RGBA16_SFLOAT, data, vulkan);
    TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                              12, 8, 1, RI_FORMAT_RGBA16_SFLOAT);
    Require(CompareTexture(read, data, 12, 8, 1, RI_FORMAT_RGBA16_SFLOAT),
            "RGBA16_SFLOAT texture reads back byte-exactly");
    texture.dispose(&device);
  }

  // Depth formats bypass CreateTestTexture because D3D12 requires ALLOW_DEPTH_STENCIL.
  {
    RITextureDesc desc = {};
    desc.type = RI_TEXTURE_2D; desc.format = RI_FORMAT_D16_UNORM;
    desc.width = 16; desc.height = 16; desc.depth = 0;
    desc.mipNum = 1; desc.layerNum = 1; desc.sampleCount = RI_SAMPLE_COUNT_1;
    desc.usage = RI_USAGE_DEPTH_STENCIL_ATTACHMENT; desc.flags = 0;
    RITexture texture = RITexture::create(&device, desc);
    Require(!texture.isEmpty(), "D16_UNORM depth texture is created");
    texture.dispose(&device);
  }

  {
    RITextureDesc desc = {};
    desc.type = RI_TEXTURE_2D; desc.format = RI_FORMAT_D32_SFLOAT;
    desc.width = 16; desc.height = 16; desc.depth = 0;
    desc.mipNum = 1; desc.layerNum = 1; desc.sampleCount = RI_SAMPLE_COUNT_1;
    desc.usage = RI_USAGE_DEPTH_STENCIL_ATTACHMENT; desc.flags = 0;
    RITexture texture = RITexture::create(&device, desc);
    Require(!texture.isEmpty(), "D32_SFLOAT depth texture is created");
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM,
                                          16, 8, 1, 1, 1);
    Require(!texture.isEmpty(), "offset RGBA8 texture is created");

    // Stage a small non-flushing buffer copy into the current set so the
    // uploader's staging offset is nonzero when the texture BeginCopyTexture runs.
    RIBuffer textureAdvance = CreateDeviceBuffer(&device, 256);
    std::vector<uint8_t> advanceData(137, 0x6B);
    size_t offsetActiveSet = uploader.upload_resource.active_set;
    RIResourceBufferTransaction advanceTrans = {};
    advanceTrans.target = textureAdvance;
    advanceTrans.size = advanceData.size();
    advanceTrans.currentState = RI_RESOURCE_STATE_UNDEFINED;
    advanceTrans.postState = RI_RESOURCE_STATE_COPY_SRC;
    advanceTrans.postStages = RI_STAGE_COPY;
    RI_ResourceBeginCopyBuffer(&device, &uploader, &advanceTrans);
    Require(advanceTrans.mapped.data != nullptr && advanceTrans.mapped.size >= advanceData.size(),
            "advance buffer maps staging range");
    std::memcpy(advanceTrans.mapped.data, advanceData.data(), advanceData.size());
    RI_ResourceEndCopyBuffer(&device, &uploader, &advanceTrans);
    Require(uploader.upload_resource.active_set == offsetActiveSet,
            "staging buffer and texture upload share the same active set");

    std::vector<uint8_t> data = MakeTexturePayload(16, 8, 1, RI_FORMAT_RGBA8_UNORM, 0x22);
    UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 16, 8, 1,
                             RI_FORMAT_RGBA8_UNORM, data, vulkan, true);
    TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                              16, 8, 1, RI_FORMAT_RGBA8_UNORM);
    Require(CompareTexture(read, data, 16, 8, 1, RI_FORMAT_RGBA8_UNORM),
            "nonzero-offset texture reads back byte-exactly");
    texture.dispose(&device);
    textureAdvance.dispose(&device);
  }

  // Each iteration submits a texture upload and flushes. The third iteration
  // is beyond RI_RESOURCE_MAX_SETS and therefore exercises a second lap.
  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM,
                                          8, 8, 1, 1, 1);
    Require(!texture.isEmpty(), "rotating texture is created");
    std::vector<uint8_t> last;
    for (int i = 0; i < RI_RESOURCE_MAX_SETS + 1; ++i) {
      last = MakeTexturePayload(8, 8, 1, RI_FORMAT_RGBA8_UNORM, uint8_t(0x30 + i));
      UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 8, 8, 1,
                               RI_FORMAT_RGBA8_UNORM, last, vulkan);
    }
    TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                              8, 8, 1, RI_FORMAT_RGBA8_UNORM);
    Require(CompareTexture(read, last, 8, 8, 1, RI_FORMAT_RGBA8_UNORM),
            "second-lap texture upload reads back byte-exactly");
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM,
                                          32, 32, 1, 3, 4);
    Require(!texture.isEmpty(), "multi-mip multi-layer texture is created");
    for (uint32_t mip = 0; mip < 3; ++mip) for (uint32_t layer = 0; layer < 4; ++layer) {
      uint32_t extent = 32u >> mip;
      std::vector<uint8_t> data = MakeTexturePayload(extent, extent, 1, RI_FORMAT_RGBA8_UNORM,
                                                      uint8_t(0x40 + mip * 13 + layer));
      UploadTextureSubresource(&device, &uploader, &texture, mip, layer, 0, extent, extent, 1,
                               RI_FORMAT_RGBA8_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, mip, layer,
                                                                extent, extent, 1, RI_FORMAT_RGBA8_UNORM);
      Require(CompareTexture(read, data, extent, extent, 1, RI_FORMAT_RGBA8_UNORM),
              "multi-mip multi-layer subresource reads back byte-exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM,
                                          16, 16, 1, 1, 6, RI_TEXTURE_FLAG_CUBE_COMPATIBLE);
    Require(!texture.isEmpty(), "cube-compatible texture is created");
    for (uint32_t face = 0; face < 6; ++face) {
      std::vector<uint8_t> data = MakeTexturePayload(16, 16, 1, RI_FORMAT_RGBA8_UNORM,
                                                      uint8_t(0x70 + face));
      UploadTextureSubresource(&device, &uploader, &texture, 0, face, 0, 16, 16, 1,
                               RI_FORMAT_RGBA8_UNORM, data, vulkan);
      TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, face,
                                                                16, 16, 1, RI_FORMAT_RGBA8_UNORM);
      Require(CompareTexture(read, data, 16, 16, 1, RI_FORMAT_RGBA8_UNORM),
              "cube face reads back byte-exactly");
    }
    texture.dispose(&device);
  }

  {
    RITexture texture = CreateTestTexture(&device, RI_TEXTURE_3D, RI_FORMAT_RGBA8_UNORM,
                                          16, 16, 4, 1, 1);
    if (texture.isEmpty()) {
      Skip(vulkan ? "Vulkan 3D texture creation rejected this smoke configuration; 3D upload coverage is unavailable"
                  : "D3D12 3D texture creation rejected this smoke configuration; 3D upload coverage is unavailable");
    } else {
      std::vector<std::vector<uint8_t>> slices;
      for (uint32_t z = 0; z < 4; ++z) {
        slices.push_back(MakeTexturePayload(16, 16, 1, RI_FORMAT_RGBA8_UNORM, uint8_t(0xB0 + z)));
        UploadTextureSubresource(&device, &uploader, &texture, 0, 0, z, 16, 16, 1,
                                 RI_FORMAT_RGBA8_UNORM, slices.back(), vulkan);
      }
      for (uint32_t z = 0; z < 4; ++z) {
        TextureReadback read = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                                  16, 16, 1, RI_FORMAT_RGBA8_UNORM, z);
        Require(CompareTexture(read, slices[z], 16, 16, 1, RI_FORMAT_RGBA8_UNORM),
                "3D single depth slice reads back byte-exactly");
      }
      std::vector<uint8_t> all = MakeTexturePayload(16, 16, 4, RI_FORMAT_RGBA8_UNORM, 0xD0);
      UploadTextureSubresource(&device, &uploader, &texture, 0, 0, 0, 16, 16, 4,
                               RI_FORMAT_RGBA8_UNORM, all, vulkan, false, true);
      TextureReadback readAll = CopyTextureSubresourceToReadback(&device, &texture, 0, 0,
                                                                  16, 16, 4, RI_FORMAT_RGBA8_UNORM);
      Require(CompareTexture(readAll, all, 16, 16, 4, RI_FORMAT_RGBA8_UNORM),
              "3D multi-slice upload reads back byte-exactly across all slices");
      texture.dispose(&device);
    }
  }

  pending.dispose(&device); for (RIBuffer &b : cycleBuffers) b.dispose(&device);
  bigReadback.dispose(&device); big.dispose(&device);
  smallReadback.dispose(&device); offsetBuffer.dispose(&device);
  RI_FreeResourceUploader(&device, &uploader); device.dispose(); ShutdownRIRenderer();
  if (!vulkan) g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool vulkan = false, enableDebugLayer = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vulkan") == 0) vulkan = true;
    else if (std::strcmp(argv[i], "--no-debug-layer") == 0) enableDebugLayer = false;
  }
  RunUploaderCycle(vulkan, enableDebugLayer);
  return 0;
}
