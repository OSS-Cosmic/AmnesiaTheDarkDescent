#include "graphics/RID3D12.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

#include <windows.h>
#include <d3d12.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

int hplMain(const std::string &) { return 0; }

namespace {

[[noreturn]] void Fail(const char *what) {
  std::fprintf(stderr, "FAIL: %s\n", what);
  std::exit(1);
}

void Require(bool ok, const char *what) {
  if (!ok) Fail(what);
  std::printf("PASS: %s\n", what);
}

void Release(IUnknown *object) {
  if (object) object->Release();
}

void SubmitAndWait(RIDevice *device, RICmd *cmd) {
  RICmd *commands[] = {cmd};
  RISubmitDesc submit = {};
  submit.cmds = commands;
  submit.cmdCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) == RI_SUCCESS,
          "graphics command list submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
}

void Transition(RICmd *cmd, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  cmd->d3d12.cmdList->ResourceBarrier(1, &barrier);
}

struct Readback {
  ID3D12Resource *buffer = nullptr;
  void *mapped = nullptr;
  UINT rowPitch = 0;
};

Readback ReadSubresource(RIDevice *device, RICmd *cmd, RITexture *texture,
                         UINT mip, UINT layer, D3D12_RESOURCE_STATES before,
                         UINT plane = 0) {
  ID3D12Resource *resource = texture->d3d12.resource;
  D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
  UINT subresource = mip + layer * texture->d3d12.mipNum +
                     plane * texture->d3d12.mipNum * texture->d3d12.layerNum;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 rowSize = 0, totalSize = 0;
  device->d3d12.device->GetCopyableFootprints(&resourceDesc, subresource, 1, 0,
                                               &footprint, &rows, &rowSize,
                                               &totalSize);

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = totalSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  Readback result;
  Require(SUCCEEDED(device->d3d12.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&result.buffer))),
          "readback buffer creates");

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = result.buffer;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;

  cmd->begin(device);
  Transition(cmd, resource, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(cmd, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, before);
  cmd->end(device);
  SubmitAndWait(device, cmd);

  D3D12_RANGE range = {0, SIZE_T(totalSize)};
  Require(SUCCEEDED(result.buffer->Map(0, &range, &result.mapped)) && result.mapped,
          "readback maps");
  result.rowPitch = footprint.Footprint.RowPitch;
  return result;
}

void CheckColor(const Readback &readback, UINT x, UINT y, BYTE r, BYTE g, BYTE b,
                BYTE a, const char *what) {
  const BYTE *pixel = static_cast<const BYTE *>(readback.mapped) +
                      SIZE_T(y) * readback.rowPitch + SIZE_T(x) * 4;
  Require(pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == a,
          what);
}

void CheckDepthStencil(const Readback &depthReadback,
                       const Readback &stencilReadback, UINT x, UINT y,
                       float depth, BYTE stencil, const char *what) {
  const BYTE *pixel = static_cast<const BYTE *>(depthReadback.mapped) +
                      SIZE_T(y) * depthReadback.rowPitch + SIZE_T(x) * 4;
  const BYTE actualStencil =
      *(static_cast<const BYTE *>(stencilReadback.mapped) +
        SIZE_T(y) * stencilReadback.rowPitch + SIZE_T(x));
  uint32_t packed = uint32_t(pixel[0]) | (uint32_t(pixel[1]) << 8) |
                    (uint32_t(pixel[2]) << 16) | (uint32_t(pixel[3]) << 24);
  const uint32_t depthBits = packed & 0x00ffffffu;
  const uint32_t expected = uint32_t(depth * 16777215.0f + 0.5f);
  if (actualStencil != stencil || std::abs(int64_t(depthBits) - expected) > 2)
    std::fprintf(stderr,
                 "depth/stencil mismatch: bytes=%02x %02x %02x %02x depth=%u expected=%u stencil=%u expected=%u\n",
                 pixel[0], pixel[1], pixel[2], pixel[3], depthBits, expected,
                 unsigned(actualStencil), unsigned(stencil));
  Require(actualStencil == stencil &&
              std::abs(int64_t(depthBits) - expected) <= 2,
          what);
}

RITexture MakeTexture(RIDevice *device, uint32_t format, uint32_t usage,
                      uint32_t width, uint32_t height, uint32_t mips = 1,
                      uint32_t layers = 1) {
  RITextureDesc desc = {};
  desc.type = RI_TEXTURE_2D;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.mipNum = mips;
  desc.layerNum = layers;
  desc.sampleCount = RI_SAMPLE_COUNT_1;
  desc.usage = usage;
  RITexture texture = RITexture::create(device, desc);
  Require(!texture.isEmpty(), "RI attachment texture creates");
  return texture;
}

RITextureView MakeView(RIDevice *device, RITexture *texture, uint32_t type,
                       uint32_t format, uint32_t mip = 0, uint32_t layer = 0) {
  RITextureViewDesc desc = {static_cast<RITextureViewType_e>(type), format,
                            mip, 1, layer, 1};
  RITextureView view = RITextureView::create(device, texture, desc);
  Require(!view.isEmpty(), "RI attachment view creates");
  return view;
}

RIRenderingAttachment ColorAttachment(const RITextureView &view, uint8_t load,
                                      uint8_t store, float r, float g, float b,
                                      float a) {
  RIRenderingAttachment attachment = {};
  attachment.view = view;
  attachment.loadOp = load;
  attachment.storeOp = store;
  attachment.clearValue.color[0] = r;
  attachment.clearValue.color[1] = g;
  attachment.clearValue.color[2] = b;
  attachment.clearValue.color[3] = a;
  return attachment;
}

RIRenderingAttachment DepthAttachment(const RITextureView &view, uint8_t load,
                                       uint8_t store, bool readOnly,
                                       uint8_t stencilLoad, uint8_t stencilStore,
                                       float depth, uint32_t stencil) {
  RIRenderingAttachment attachment = {};
  attachment.view = view;
  attachment.loadOp = load;
  attachment.storeOp = store;
  attachment.readOnly = readOnly;
  attachment.hasStencil = true;
  attachment.stencilLoadOp = stencilLoad;
  attachment.stencilStoreOp = stencilStore;
  attachment.clearValue.depth = depth;
  attachment.clearValue.stencil = stencil;
  return attachment;
}

void RenderScope(RIDevice *device, RICmd *cmd, RIRenderingAttachment *colors,
                 uint32_t colorCount, RIRenderingAttachment *depth,
                 int16_t x, int16_t y, int16_t width, int16_t height) {
  RIBeginRenderingDesc desc = {};
  desc.renderArea.x = x;
  desc.renderArea.y = y;
  desc.renderArea.width = width;
  desc.renderArea.height = height;
  desc.colorCount = colorCount;
  desc.colors = colors;
  desc.depthStencil = depth;
  cmd->begin(device);
  cmd->vk_d3d12_beginRendering(device, desc);
  cmd->vk_d3d12_endRendering(device);
  cmd->end(device);
  SubmitAndWait(device, cmd);
}

void RunSmoke() {
  g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12RenderScopeSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");

  uint32_t count = 0;
  Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count > 0 && count <= 8,
          "D3D12 adapter enumeration succeeds");
  RIPhysicalAdapter adapters[8] = {};
  uint32_t capacity = count;
  Require(EnumerateRIAdapters(adapters, &capacity) == RI_SUCCESS && capacity > 0,
          "D3D12 adapter enumeration populates adapters");
  uint32_t selected = 0;
  for (uint32_t i = 0; i < capacity; ++i)
    if (!adapters[i].d3d12.isWarp) { selected = i; break; }

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 device initializes");
  if (device.d3d12.infoQueue) device.d3d12.infoQueue->ClearStoredMessages();

  RITexture color = MakeTexture(&device, RI_FORMAT_RGBA8_UNORM,
                                RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_TRANSFER_SRC,
                                8, 8, 2, 2);
  RITexture second = MakeTexture(&device, RI_FORMAT_RGBA8_UNORM,
                                 RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_TRANSFER_SRC,
                                 8, 8);
  RITexture depth = MakeTexture(&device, RI_FORMAT_D24_UNORM_S8_UINT,
                                RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_TRANSFER_SRC,
                                8, 8);

  RITextureView colorView = MakeView(&device, &color, RI_VIEWTYPE_COLOR_ATTACHMENT,
                                     RI_FORMAT_RGBA8_UNORM);
  RITextureView mipLayerView = MakeView(&device, &color, RI_VIEWTYPE_COLOR_ATTACHMENT,
                                        RI_FORMAT_RGBA8_UNORM, 1, 1);
  RITextureView secondView = MakeView(&device, &second, RI_VIEWTYPE_COLOR_ATTACHMENT,
                                      RI_FORMAT_RGBA8_UNORM);
  RITextureView depthView = MakeView(&device, &depth, RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
                                     RI_FORMAT_D24_UNORM_S8_UINT);

  RIPool pool;
  pool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(&device, &pool);

  // CLEAR followed by LOAD proves that RI load operations preserve the area
  // outside a later subrectangle. The second scope also exercises store=STORE.
  RIRenderingAttachment first = ColorAttachment(
      colorView, RI_ATTACHMENT_LOAD_OP_CLEAR, RI_ATTACHMENT_STORE_OP_STORE,
      1, 0, 0, 1);
  RenderScope(&device, &cmd, &first, 1, nullptr, 0, 0, 8, 8);
  first = ColorAttachment(colorView, RI_ATTACHMENT_LOAD_OP_CLEAR,
                          RI_ATTACHMENT_STORE_OP_STORE, 0, 0, 1, 1);
  RenderScope(&device, &cmd, &first, 1, nullptr, 2, 2, 4, 4);
  Readback colorReadback = ReadSubresource(&device, &cmd, &color, 0, 0,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
  CheckColor(colorReadback, 0, 0, 255, 0, 0, 255,
             "RI LOAD preserves outside subrectangle");
  CheckColor(colorReadback, 3, 3, 0, 0, 255, 255,
             "RI render area clears only the subrectangle");
  colorReadback.buffer->Unmap(0, nullptr);
  Release(colorReadback.buffer);

  // A clear on a non-zero mip/layer must select exactly that RI view.
  RIRenderingAttachment mipLayer = ColorAttachment(
      mipLayerView, RI_ATTACHMENT_LOAD_OP_CLEAR, RI_ATTACHMENT_STORE_OP_STORE,
      1, 1, 0, 1);
  RenderScope(&device, &cmd, &mipLayer, 1, nullptr, 0, 0, 4, 4);
  Readback mipReadback = ReadSubresource(&device, &cmd, &color, 1, 1,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
  CheckColor(mipReadback, 0, 0, 255, 255, 0, 255,
             "RI mip/layer attachment clear is readable");
  mipReadback.buffer->Unmap(0, nullptr);
  Release(mipReadback.buffer);

  // MRT is one RI rendering scope with two RI color attachments.
  RIRenderingAttachment mrt[2] = {
      ColorAttachment(colorView, RI_ATTACHMENT_LOAD_OP_CLEAR,
                      RI_ATTACHMENT_STORE_OP_STORE, 0, 1, 1, 1),
      ColorAttachment(secondView, RI_ATTACHMENT_LOAD_OP_CLEAR,
                      RI_ATTACHMENT_STORE_OP_STORE, 1, 0, 1, 1)};
  RenderScope(&device, &cmd, mrt, 2, nullptr, 0, 0, 8, 8);
  Readback mrtColor = ReadSubresource(&device, &cmd, &color, 0, 0,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  Readback mrtSecond = ReadSubresource(&device, &cmd, &second, 0, 0,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
  CheckColor(mrtColor, 0, 0, 0, 255, 255, 255, "RI MRT first target is cleared");
  CheckColor(mrtSecond, 0, 0, 255, 0, 255, 255, "RI MRT second target is cleared");
  mrtColor.buffer->Unmap(0, nullptr);
  mrtSecond.buffer->Unmap(0, nullptr);
  Release(mrtColor.buffer);
  Release(mrtSecond.buffer);

  // DONT_CARE is used only where its discarded contents are not observed. A
  // following deterministic CLEAR+STORE scope proves the attachment remains
  // usable without asserting undefined DONT_CARE contents.
  RIRenderingAttachment discard = ColorAttachment(
      secondView, RI_ATTACHMENT_LOAD_OP_DONT_CARE,
      RI_ATTACHMENT_STORE_OP_DONT_CARE, 0, 0, 0, 1);
  RenderScope(&device, &cmd, &discard, 1, nullptr, 0, 0, 8, 8);
  discard = ColorAttachment(secondView, RI_ATTACHMENT_LOAD_OP_CLEAR,
                            RI_ATTACHMENT_STORE_OP_STORE, 0, 0, 0, 1);
  RenderScope(&device, &cmd, &discard, 1, nullptr, 0, 0, 8, 8);
  Readback discardReadback = ReadSubresource(&device, &cmd, &second, 0, 0,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
  CheckColor(discardReadback, 0, 0, 0, 0, 0, 255,
             "RI DONT_CARE/store discard is followed by deterministic clear");
  discardReadback.buffer->Unmap(0, nullptr);
  Release(discardReadback.buffer);

  // Clear depth and stencil independently, then load both through a read-only
  // depth/stencil attachment. D24S8 readback verifies both aspects.
  RIRenderingAttachment ds = DepthAttachment(
      depthView, RI_ATTACHMENT_LOAD_OP_CLEAR, RI_ATTACHMENT_STORE_OP_STORE,
      false, RI_ATTACHMENT_LOAD_OP_CLEAR, RI_ATTACHMENT_STORE_OP_STORE,
      0.25f, 3);
  RenderScope(&device, &cmd, nullptr, 0, &ds, 0, 0, 8, 8);
  ds = DepthAttachment(depthView, RI_ATTACHMENT_LOAD_OP_LOAD,
                       RI_ATTACHMENT_STORE_OP_STORE, false,
                       RI_ATTACHMENT_LOAD_OP_CLEAR, RI_ATTACHMENT_STORE_OP_STORE,
                       0.0f, 7);
  RenderScope(&device, &cmd, nullptr, 0, &ds, 0, 0, 8, 8);
  ds = DepthAttachment(depthView, RI_ATTACHMENT_LOAD_OP_LOAD,
                       RI_ATTACHMENT_STORE_OP_STORE, true,
                       RI_ATTACHMENT_LOAD_OP_LOAD, RI_ATTACHMENT_STORE_OP_STORE,
                       0.0f, 0);
  RenderScope(&device, &cmd, nullptr, 0, &ds, 0, 0, 8, 8);
  Readback depthReadback = ReadSubresource(&device, &cmd, &depth, 0, 0,
                                           D3D12_RESOURCE_STATE_DEPTH_WRITE);
  Readback stencilReadback = ReadSubresource(
      &device, &cmd, &depth, 0, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, 1);
  CheckDepthStencil(depthReadback, stencilReadback, 0, 0, 0.25f, 7,
                    "RI depth LOAD/read-only depth preserves depth and stencil");
  depthReadback.buffer->Unmap(0, nullptr);
  stencilReadback.buffer->Unmap(0, nullptr);
  Release(depthReadback.buffer);
  Release(stencilReadback.buffer);

  UINT64 debugErrors = 0;
  if (device.d3d12.infoQueue) {
    const UINT64 messageCount = device.d3d12.infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < messageCount; ++i) {
      SIZE_T bytes = 0;
      device.d3d12.infoQueue->GetMessage(i, nullptr, &bytes);
      std::vector<uint8_t> storage(bytes);
      auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
      if (SUCCEEDED(device.d3d12.infoQueue->GetMessage(i, message, &bytes)) &&
          message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        ++debugErrors;
    }
  }
  Require(debugErrors == 0,
          "valid RI rendering scopes produce zero debug errors");

  cmd.dispose(&device);
  pool.dispose(&device);
  depthView.dispose(&device);
  secondView.dispose(&device);
  mipLayerView.dispose(&device);
  colorView.dispose(&device);
  depth.dispose(&device);
  second.dispose(&device);
  color.dispose(&device);
  device.dispose();
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main() {
#if DEVICE_IMPL_D3D12
  RunSmoke();
  return 0;
#else
  std::printf("SKIP: DX12 not compiled in\n");
  return 0;
#endif
}
