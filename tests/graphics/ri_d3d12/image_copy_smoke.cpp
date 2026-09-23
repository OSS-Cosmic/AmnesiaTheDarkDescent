// Opt-in D3D12 smoke test for RI image-to-image region copies.
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIRenderer.h"

#include <algorithm>
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
  std::exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition) Fail(check);
  std::printf("PASS: %s\n", check);
}

std::vector<uint8_t> Pattern(uint32_t width, uint32_t height, uint32_t depth,
                             uint8_t seed) {
  std::vector<uint8_t> result(size_t(width) * height * (depth ? depth : 1) * 4);
  for (uint32_t z = 0; z < (depth ? depth : 1); ++z)
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        size_t i = (size_t(z) * height * width + size_t(y) * width + x) * 4;
        result[i + 0] = uint8_t(seed + x * 7 + y * 11 + z * 29);
        result[i + 1] = uint8_t(seed ^ (x * 13 + y * 3 + z * 17));
        result[i + 2] = uint8_t(seed + x + y * 5 + z * 9);
        result[i + 3] = 0xE1;
      }
  return result;
}

RITexture MakeTexture(RIDevice *device, uint32_t width, uint32_t height,
                      uint32_t depth, uint32_t mips, uint32_t layers) {
  RITextureDesc desc = {};
  desc.type = depth > 1 ? RI_TEXTURE_3D : RI_TEXTURE_2D;
  desc.format = RI_FORMAT_RGBA8_UNORM;
  desc.width = width; desc.height = height; desc.depth = depth;
  desc.mipNum = mips; desc.layerNum = layers; desc.sampleCount = 1;
  desc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC |
               RI_USAGE_TRANSFER_DST;
  return RITexture::create(device, desc);
}

void Upload(RIDevice *device, RIResourceUploader *uploader, RITexture *texture,
            uint32_t mip, uint32_t layer, uint32_t z, uint32_t width,
            uint32_t height, uint32_t depth, const std::vector<uint8_t> &data) {
  RIResourceTextureTransaction trans = {};
  trans.target = *texture;
  trans.format = RI_FORMAT_RGBA8_UNORM;
  trans.width = width; trans.height = height; trans.depth = depth;
  trans.sliceNum = height; trans.rowPitch = width * 4;
  trans.arrayOffset = layer; trans.mipOffset = mip; trans.z = uint16_t(z);
  // RITexture::create initializes ordinary D3D12 textures in COMMON, which is
  // represented by RI_RESOURCE_STATE_UNDEFINED.  Every subresource in this
  // newly-created fixture is therefore untouched before its first upload.
  trans.currentState = RI_RESOURCE_STATE_UNDEFINED;
  trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  trans.postStages = RI_STAGE_ALL_SHADER;
  RI_ResourceBeginCopyTexture(device, uploader, &trans);
  Require(trans.mapped.data != nullptr, "image-copy uploader maps texture data");
  for (uint32_t slice = 0; slice < (depth ? depth : 1); ++slice)
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(static_cast<uint8_t *>(trans.mapped.data) +
                      slice * trans.alignSlicePitch + y * trans.alignRowPitch,
                  data.data() + (size_t(slice) * height + y) * width * 4,
                  size_t(width) * 4);
  RI_ResourceEndCopyTexture(device, uploader, &trans);
  RIResourceUploaderD3D12Result result = RI_D3D12FlushResourceUpdate(device, uploader);
  Require(result.signaled && result.timeline != nullptr && result.value != 0,
          "image-copy texture upload submits a completion signal");
  result.timeline->wait(device, result.value);
}

void RecordCopy(RIDevice *device, RITexture *src, RITexture *dst,
                const RIImageCopyDesc &copy) {
  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  RITextureBarrier source(src, RI_RESOURCE_STATE_SHADER_RESOURCE,
                          RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_ALL_SHADER,
                          RI_STAGE_COPY);
  source.baseMip = uint16_t(copy.srcMipLevel); source.mipCount = 1;
  source.baseLayer = uint16_t(copy.srcArrayLayer); source.layerCount = 1;
  RITextureBarrier target(dst, RI_RESOURCE_STATE_SHADER_RESOURCE,
                          RI_RESOURCE_STATE_COPY_DST, RI_STAGE_ALL_SHADER,
                          RI_STAGE_COPY);
  target.baseMip = uint16_t(copy.dstMipLevel); target.mipCount = 1;
  target.baseLayer = uint16_t(copy.dstArrayLayer); target.layerCount = 1;
  cmd.vk_d3d12_textureBarrier(source);
  cmd.vk_d3d12_textureBarrier(target);
  cmd.copyImage(device, src, dst, copy);
  source.before = RI_RESOURCE_STATE_COPY_SRC;
  source.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
  source.beforeStages = RI_STAGE_COPY; source.afterStages = RI_STAGE_ALL_SHADER;
  target.before = RI_RESOURCE_STATE_COPY_DST;
  target.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
  target.beforeStages = RI_STAGE_COPY; target.afterStages = RI_STAGE_ALL_SHADER;
  cmd.vk_d3d12_textureBarrier(source);
  cmd.vk_d3d12_textureBarrier(target);
  cmd.end(device);
  ID3D12CommandList *lists[] = {cmd.d3d12.cmdList};
  device->queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1, lists);
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  cmd.dispose(device);
  pool.dispose(device);
}

struct TextureReadback {
  std::vector<uint8_t> data;
  uint32_t rowPitch = 0;
  uint32_t slicePitch = 0;
  uint32_t depth = 0;
};

TextureReadback Readback(RIDevice *device, RITexture *texture,
                         uint32_t mip, uint32_t layer, uint32_t width,
                         uint32_t height, uint32_t depth) {
  D3D12_RESOURCE_DESC resource = texture->d3d12.resource->GetDesc();
  uint32_t subresource = mip + layer * texture->d3d12.mipNum;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0; UINT64 rowSize = 0; UINT64 total = 0;
  device->d3d12.device->GetCopyableFootprints(&resource, subresource, 1, 0,
                                               &footprint, &rows, &rowSize,
                                               &total);
  // Keep the complete footprint returned for the selected subresource. A 3D
  // subresource contains every depth slice; shrinking Depth here would make
  // the copy layout disagree with the allocation for partial readbacks.
  Require(footprint.Footprint.Width >= width && footprint.Footprint.Height >= height &&
              footprint.Footprint.Depth >= depth,
          "image-copy readback footprint covers the requested extent");
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1; buffer.SampleDesc = {1, 0};
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *readback = nullptr;
  Require(SUCCEEDED(device->d3d12.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &buffer,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&readback))), "image-copy readback buffer creates");
  RIPool pool; pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd; cmd.init(device, &pool); cmd.begin(device);
  RITextureBarrier barrier(texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
                           RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_ALL_SHADER,
                           RI_STAGE_COPY);
  barrier.baseMip = uint16_t(mip); barrier.mipCount = 1;
  barrier.baseLayer = uint16_t(layer); barrier.layerCount = 1;
  cmd.vk_d3d12_textureBarrier(barrier);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = texture->d3d12.resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  cmd.d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier.before = RI_RESOURCE_STATE_COPY_SRC;
  barrier.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
  barrier.beforeStages = RI_STAGE_COPY; barrier.afterStages = RI_STAGE_ALL_SHADER;
  cmd.vk_d3d12_textureBarrier(barrier); cmd.end(device);
  ID3D12CommandList *lists[] = {cmd.d3d12.cmdList};
  device->queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1, lists);
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  D3D12_RANGE range = {0, SIZE_T(total)}; void *mapped = nullptr;
  Require(SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped != nullptr,
          "image-copy readback maps");
  TextureReadback result;
  result.data.assign(static_cast<uint8_t *>(mapped),
                     static_cast<uint8_t *>(mapped) + total);
  result.rowPitch = footprint.Footprint.RowPitch;
  result.slicePitch = footprint.Footprint.RowPitch * footprint.Footprint.Height;
  result.depth = footprint.Footprint.Depth;
  readback->Unmap(0, nullptr); cmd.dispose(device); pool.dispose(device);
  readback->Release();
  return result;
}

bool SameUsefulTexels(const TextureReadback &actual,
                      const TextureReadback &expected, uint32_t width,
                      uint32_t height, uint32_t depth) {
  if (actual.rowPitch != expected.rowPitch || actual.slicePitch != expected.slicePitch ||
      actual.depth < depth || expected.depth < depth)
    return false;
  for (uint32_t z = 0; z < depth; ++z)
    for (uint32_t y = 0; y < height; ++y)
      if (std::memcmp(actual.data.data() + size_t(z) * actual.slicePitch +
                          size_t(y) * actual.rowPitch,
                      expected.data.data() + size_t(z) * expected.slicePitch +
                          size_t(y) * expected.rowPitch,
                      size_t(width) * 4) != 0)
        return false;
  return true;
}

void Run(bool enableDebugLayer) {
  if (enableDebugLayer)
    g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {}; init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12ImageCopySmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 image-copy renderer initializes");
  uint32_t count = 0; Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count,
                                "image-copy adapter enumeration succeeds");
  RIPhysicalAdapter adapters[8] = {}; uint32_t capacity = std::min(count, 8u);
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS && capacity,
          "image-copy adapter enumeration populates adapters");
  uint32_t selected = 0;
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
          "image-copy selects hardware or WARP adapter");
  RIDevice device; RIDeviceDesc deviceDesc = {}; deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 image-copy device initializes");
  RIResourceUploader uploader = {}; RI_InitResourceUploader(&device, &uploader);
  Require(uploader.upload_resource.queue != nullptr,
          "image-copy uploader initializes its transfer queue");

  RITexture src = MakeTexture(&device, 16, 16, 1, 3, 2);
  RITexture dst = MakeTexture(&device, 16, 16, 1, 3, 2);
  Require(!src.isEmpty() && !dst.isEmpty(), "image-copy array textures create");
  const auto source = Pattern(8, 8, 1, 0x31), untouched = Pattern(4, 4, 1, 0xA2);
  Upload(&device, &uploader, &src, 1, 1, 0, 8, 8, 1, source);
  Upload(&device, &uploader, &dst, 2, 1, 0, 4, 4, 1, untouched);
  RIImageCopyDesc copy = {};
  copy.srcMipLevel = 1; copy.srcArrayLayer = 1;
  copy.srcX = 2; copy.srcY = 1;
  copy.dstMipLevel = 2; copy.dstArrayLayer = 1;
  copy.dstX = 1; copy.dstY = 1;
  copy.width = 3; copy.height = 2; copy.depth = 1;
  RecordCopy(&device, &src, &dst, copy);
  auto result = Readback(&device, &dst, 2, 1, 4, 4, 1);
  bool copied = true, preserved = true;
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 3; ++x)
    copied &= std::memcmp(result.data.data() + size_t(y + 1) * result.rowPitch + (x + 1) * 4,
                           source.data() + (size_t(y + 1) * 8 + x + 2) * 4, 4) == 0;
  for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x)
    if (!(x >= 1 && x < 4 && y >= 1 && y < 3))
      preserved &= std::memcmp(result.data.data() + size_t(y) * result.rowPitch + x * 4,
                               untouched.data() + (size_t(y) * 4 + x) * 4, 4) == 0;
  Require(copied, "nonzero-offset mip/layer copy transfers every selected texel");
  Require(preserved, "nonzero-offset mip/layer copy preserves every untouched texel");
  RIImageCopyDesc invalid = copy; invalid.dstX = 3; invalid.width = 2;
  RecordCopy(&device, &src, &dst, invalid);
  auto afterInvalid = Readback(&device, &dst, 2, 1, 4, 4, 1);
  Require(SameUsefulTexels(afterInvalid, result, 4, 4, 1),
          "invalid 2D region is rejected without modifying destination");
  invalid = copy; invalid.width = 0;
  RecordCopy(&device, &src, &dst, invalid);
  auto afterEmpty = Readback(&device, &dst, 2, 1, 4, 4, 1);
  Require(SameUsefulTexels(afterEmpty, result, 4, 4, 1),
          "empty image region is rejected without modifying destination");
  src.dispose(&device); dst.dispose(&device);

  RITexture volumeSrc = MakeTexture(&device, 8, 8, 4, 1, 1);
  RITexture volumeDst = MakeTexture(&device, 8, 8, 4, 1, 1);
  Require(!volumeSrc.isEmpty() && !volumeDst.isEmpty(), "image-copy 3D textures create");
  const auto volume = Pattern(8, 8, 4, 0x54), volumeFill = Pattern(8, 8, 4, 0xC3);
  Upload(&device, &uploader, &volumeSrc, 0, 0, 0, 8, 8, 4, volume);
  Upload(&device, &uploader, &volumeDst, 0, 0, 0, 8, 8, 4, volumeFill);
  RIImageCopyDesc volumeCopy = {};
  volumeCopy.srcX = 1; volumeCopy.srcY = 1; volumeCopy.srcZ = 1;
  volumeCopy.dstX = 2;
  volumeCopy.width = 3; volumeCopy.height = 2; volumeCopy.depth = 2;
  RecordCopy(&device, &volumeSrc, &volumeDst, volumeCopy);
  auto volumeResult = Readback(&device, &volumeDst, 0, 0, 8, 8, 4);
  copied = preserved = true;
  for (uint32_t z = 0; z < 2; ++z) for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 3; ++x)
      copied &= std::memcmp(volumeResult.data.data() + size_t(z) * volumeResult.slicePitch +
                                 size_t(y) * volumeResult.rowPitch + (x + 2) * 4,
                             volume.data() + (size_t(z + 1) * 8 * 8 + size_t(y + 1) * 8 + x + 1) * 4, 4) == 0;
  for (uint32_t z = 0; z < 4; ++z) for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      if (!(z < 2 && x >= 2 && x < 5 && y < 2))
        preserved &= std::memcmp(volumeResult.data.data() + size_t(z) * volumeResult.slicePitch +
                                      size_t(y) * volumeResult.rowPitch + x * 4,
                                  volumeFill.data() + (size_t(z) * 8 * 8 + size_t(y) * 8 + x) * 4, 4) == 0;
  Require(copied, "3D volume copy transfers every selected voxel");
  Require(preserved, "3D volume copy preserves every untouched voxel");
  volumeSrc.dispose(&device); volumeDst.dispose(&device);
  RI_FreeResourceUploader(&device, &uploader); device.dispose(); ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  bool enableDebugLayer = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-debug-layer") == 0)
      enableDebugLayer = false;
  }
  Run(enableDebugLayer);
  return 0;
}
