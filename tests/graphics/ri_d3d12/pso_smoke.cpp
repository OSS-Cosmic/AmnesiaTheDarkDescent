// Focused RIProgram D3D12 smoke coverage.  Pipeline/root-signature creation
// must go through RIProgram; native D3D12 objects below are only resources and
// readback plumbing used to prove the resulting commands execute.
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIProgram.h"
#include "graphics/RIBuffer.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RICommand.h"
#include "system/String.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
int hplMain(const std::string &) { return 0; }

namespace {
[[noreturn]] void Fail(const char *what) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
void Require(bool ok, const char *what) { if (!ok) Fail(what); std::printf("PASS: %s\n", what); }
void Hr(HRESULT hr, const char *what) { Require(SUCCEEDED(hr), what); }

std::vector<uint8_t> Bytes(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  Require(f.good(), "shader artifact opens");
  auto n = f.tellg(); Require(n > 0, "shader artifact is nonempty");
  std::vector<uint8_t> b(static_cast<size_t>(n)); f.seekg(0); f.read((char *)b.data(), n);
  Require(f.good(), "shader artifact reads"); return b;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device *d, D3D12_HEAP_TYPE heap, UINT64 size,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
  ComPtr<ID3D12Resource> r;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "buffer creates");
  return r;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device *d, DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = 8; rd.Height = 8; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.Format = format; rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> r;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r)), "render target creates");
  return r;
}

ComPtr<ID3D12Resource> DepthTexture(ID3D12Device *d) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = 8; rd.Height = 8; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_D32_FLOAT; rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear = {}; clear.Format = DXGI_FORMAT_D32_FLOAT;
  clear.DepthStencil = {1.0f, 0};
  ComPtr<ID3D12Resource> r;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                IID_PPV_ARGS(&r)), "depth target creates");
  return r;
}

void Wait(ID3D12CommandQueue *q, ID3D12Device *d, ID3D12CommandList *list) {
  ComPtr<ID3D12Fence> fence; Hr(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence creates");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "fence event creates");
  q->ExecuteCommandLists(1, &list); Hr(q->Signal(fence.Get(), 1), "queue signals fence");
  if (fence->GetCompletedValue() < 1) { Hr(fence->SetEventOnCompletion(1, event), "fence event arms"); WaitForSingleObject(event, INFINITE); }
  CloseHandle(event);
}

hpl::RIProgram::ShaderArtifact Artifact(hpl::cFileSearcher &searcher,
                                        const char *name) {
  auto artifact = hpl::RIProgram::loadShaderArtifact(&searcher, name);
  Require(!artifact.empty() && artifact.reflection,
          "RIProgram shader artifact and reflection load");
  return artifact;
}

// Plain value: the desc owns its whole state, so unlike the
// VkGraphicsPipelineCreateInfo this replaces there are no caller-owned locals
// to keep alive for the pointers inside it.
hpl::RIGraphicsPipelineDesc GraphicsDesc(RI_Format_e format, bool blend,
                                         bool depth, RICullMode_e cull) {
  hpl::RIGraphicsPipelineDesc desc = {};
  desc.blend[0].blendEnable = blend;
  desc.blend[0].srcColor = RI_BLEND_SRC_ALPHA;
  desc.blend[0].dstColor = blend ? RI_BLEND_ONE_MINUS_SRC_ALPHA : RI_BLEND_ZERO;
  desc.blend[0].colorOp = RI_BLEND_OP_ADD;
  desc.blend[0].srcAlpha = RI_BLEND_ONE;
  desc.blend[0].dstAlpha = blend ? RI_BLEND_ONE_MINUS_SRC_ALPHA : RI_BLEND_ZERO;
  desc.blend[0].alphaOp = RI_BLEND_OP_ADD;
  desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;
  desc.blendCount = 1; // must match renderTarget.colorCount
  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = cull;
  // Explicit: the desc defaults to CLOCKWISE where a zeroed Vulkan raster
  // state means COUNTER_CLOCKWISE, and the front-face culling case below
  // depends on the winding.
  desc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;
  desc.raster.lineWidth = 1.0f;
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.sampleCount = 1;
  desc.depthStencil.depthTest = depth;
  desc.depthStencil.depthWrite = depth;
  desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;
  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = format;
  desc.renderTarget.depthFormat =
      depth ? RI_FORMAT_D32_SFLOAT : RI_FORMAT_UNKNOWN;
  return desc;
}

// position + colour, 28-byte stride: the triangle.vert layout.
hpl::RIGraphicsPipelineDesc TriangleVertexInput(hpl::RIGraphicsPipelineDesc desc) {
  desc.vertexInput.bindingCount = 1;
  desc.vertexInput.bindings[0] = {0, 28, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.attributeCount = 2;
  desc.vertexInput.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};
  desc.vertexInput.attributes[1] = {1, 0, RI_FORMAT_RGBA32_SFLOAT, 12};
  return desc;
}

// position + uv + colour, 36-byte stride: the gui.vert layout.
hpl::RIGraphicsPipelineDesc GuiVertexInput(hpl::RIGraphicsPipelineDesc desc) {
  desc.vertexInput.bindingCount = 1;
  desc.vertexInput.bindings[0] = {0, 36, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.attributeCount = 3;
  desc.vertexInput.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};
  desc.vertexInput.attributes[1] = {1, 0, RI_FORMAT_RG32_SFLOAT, 12};
  desc.vertexInput.attributes[2] = {2, 0, RI_FORMAT_RGBA32_SFLOAT, 20};
  return desc;
}

size_t DebugLayerErrors(ID3D12Device *device) {
  ComPtr<ID3D12InfoQueue> info;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info)))) return 0;
  size_t errors = 0;
  const UINT64 count = info->GetNumStoredMessages();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
    std::vector<uint8_t> storage(bytes);
    auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
    if (SUCCEEDED(info->GetMessage(i, message, &bytes)) &&
        message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
      ++errors;
      std::fprintf(stderr, "D3D12 debug error: %.*s\n",
                   static_cast<int>(message->DescriptionByteLength),
                   message->pDescription);
    }
  }
  return errors;
}

void Run(const char *exe) {
  g_riD3D12EnableDebugLayer = true; RIBackendInit init = {}; init.api = RI_DEVICE_API_D3D12; init.applicationName = "RID3D12PSOSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");

  auto dir = std::filesystem::path(exe).parent_path() / "compiled_shaders" / "d3d12";
  hpl::cFileSearcher searcher; searcher.AddDirectory(hpl::cString::To16Char(dir.string()), "*", false);
  auto vs = Artifact(searcher, "triangle.vert.dxil");
  auto ps = Artifact(searcher, "triangle.frag.dxil");
  auto cs = Artifact(searcher, "compute.comp.dxil");
  auto guiVs = Artifact(searcher, "gui.vert.dxil");
  auto guiPs = Artifact(searcher, "gui.frag.dxil");

  uint32_t count = 0; Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count > 0, "D3D12 adapters enumerate");
  std::vector<RIPhysicalAdapter> adapters(count); Require(EnumerateRIAdapters(adapters.data(), &count) == RI_SUCCESS, "D3D12 adapter data populates");
  uint32_t selected = 0; for (uint32_t i = 0; i < count; ++i) if (!adapters[i].d3d12.isWarp) { selected = i; break; }
  RIDevice device; RIDeviceDesc dd = {}; dd.physicalAdapter = &adapters[selected]; Require(device.init(&dd) == RI_SUCCESS && RIDeviceIsValid(&device), "D3D12 device initializes");
  ID3D12Device *d = device.d3d12.device; ID3D12CommandQueue *q = device.queues[RI_QUEUE_GRAPHICS].d3d12.queue;
  ComPtr<ID3D12InfoQueue> infoQueue;
  if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&infoQueue)))) infoQueue->ClearStoredMessages();
  hpl::RIProgram graphics, compute, gui;
  std::array<hpl::RIProgram::ModuleStage, 2> graphicsStages = {
      hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_VERTEX, vs, "VSMain"),
      hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_FRAGMENT, ps, "PSMain")};
  graphics.initialize(&device, graphicsStages, {}, "RIProgram graphics smoke");
  hpl::RIProgram::ModuleStage computeStage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE, cs, "CSMain");
  compute.initialize(&device, std::span<hpl::RIProgram::ModuleStage>(&computeStage, 1), {}, "RIProgram compute smoke");
  std::array<hpl::RIProgram::ModuleStage, 2> guiStages = {
      hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_VERTEX, guiVs, "vsMain"),
      hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_FRAGMENT, guiPs, "psMain")};
  gui.initialize(&device, guiStages, {}, "RIProgram GUI smoke");

  RIPool pool; pool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]); RICmd cmd; cmd.init(&device, &pool); cmd.begin(&device);
  auto graphicsDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_RGBA8_UNORM, false, false, RI_CULL_MODE_NONE));
  graphics.bindPipeline(&device, &cmd, 0x100, "graphics miss", graphicsDesc);
  Require(graphics.getD3D12PipelineCacheSize() == 1, "RIProgram graphics PSO cache creates one entry on miss");
  graphics.bindPipeline(&device, &cmd, 0x100, "graphics hit", graphicsDesc);
  Require(graphics.getD3D12PipelineCacheSize() == 1, "RIProgram graphics PSO cache reuses the matching entry");
  graphicsDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_BGRA8_UNORM, true, true, RI_CULL_MODE_BACK));
  graphics.bindPipeline(&device, &cmd, 0x101, "graphics blend/depth", graphicsDesc);
  graphicsDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_RGBA16_SFLOAT, false, true, RI_CULL_MODE_FRONT));
  graphics.bindPipeline(&device, &cmd, 0x102, "graphics alternate format", graphicsDesc);
  Require(graphics.getD3D12PipelineCacheSize() == 3, "RIProgram graphics PSO cache keeps distinct state variants");

  // Two descs that differ only in colour attachment format, bound under one
  // shared variant hash. RIHashGraphicsPipelineDesc folds the state into the
  // cache key, so they must land in separate slots and the repeat of the
  // second must hit -- a caller-supplied hash alone cannot tell them apart.
  auto regressionDescA = TriangleVertexInput(GraphicsDesc(RI_FORMAT_RGBA8_UNORM, false, false, RI_CULL_MODE_NONE));
  auto regressionDescB = TriangleVertexInput(GraphicsDesc(RI_FORMAT_BGRA8_UNORM, false, false, RI_CULL_MODE_NONE));
  constexpr hash_t regressionHash = 0x400;
  graphics.bindPipeline(&device, &cmd, regressionHash, "same hash R8 cache miss", regressionDescA);
  Require(graphics.getD3D12PipelineCacheSize() == 4, "RIProgram PSO cache creates a same-hash R8 variant");
  graphics.bindPipeline(&device, &cmd, regressionHash, "same hash BGRA cache miss", regressionDescB);
  Require(graphics.getD3D12PipelineCacheSize() == 5, "RIProgram PSO cache separates same-hash attachment formats");
  graphics.bindPipeline(&device, &cmd, regressionHash, "same hash BGRA cache hit", regressionDescB);
  Require(graphics.getD3D12PipelineCacheSize() == 5, "RIProgram PSO cache hits the identical normalized state");

  graphicsDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_RGBA8_UNORM, false, false, RI_CULL_MODE_NONE));
  graphics.bindPipeline(&device, &cmd, 0x100, "graphics draw cache hit", graphicsDesc);
  Require(graphics.getD3D12PipelineCacheSize() == 5, "RIProgram graphics PSO cache reuses the draw entry");

  auto target = Texture(d, DXGI_FORMAT_R8G8B8A8_UNORM); auto blendTarget = Texture(d, DXGI_FORMAT_B8G8R8A8_UNORM); auto floatTarget = Texture(d, DXGI_FORMAT_R16G16B16A16_FLOAT); auto depthTarget = DepthTexture(d); auto floatDepthTarget = DepthTexture(d); auto upload = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 6 * 28, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ); auto indices = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 6, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ); auto frontIndices = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 6, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  const float vertices[6][7] = {
      {0.0f, 0.75f, 0.25f, 1.0f, 0.0f, 0.0f, 0.5f},
      {0.75f, -0.75f, 0.25f, 1.0f, 0.0f, 0.0f, 0.5f},
      {-0.75f, -0.75f, 0.25f, 1.0f, 0.0f, 0.0f, 0.5f},
      {0.0f, 0.75f, 0.75f, 0.0f, 1.0f, 0.0f, 1.0f},
      {0.75f, -0.75f, 0.75f, 0.0f, 1.0f, 0.0f, 1.0f},
      {-0.75f, -0.75f, 0.75f, 0.0f, 1.0f, 0.0f, 1.0f}};
  Hr(upload->Map(0, nullptr, &mapped), "triangle vertex buffer maps"); std::memcpy(mapped, vertices, sizeof(vertices)); upload->Unmap(0, nullptr);
  Hr(indices->Map(0, nullptr, &mapped), "triangle index buffer maps"); uint16_t idx[3] = {3,4,5}; std::memcpy(mapped, idx, sizeof(idx)); indices->Unmap(0, nullptr);
  Hr(frontIndices->Map(0, nullptr, &mapped), "front-face index buffer maps"); uint16_t frontIdx[3] = {0,2,1}; std::memcpy(mapped, frontIdx, sizeof(frontIdx)); frontIndices->Unmap(0, nullptr);
  D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}; ComPtr<ID3D12DescriptorHeap> rtvHeap; Hr(d->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap)), "RTV heap creates"); UINT rtvStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); auto rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart(); d->CreateRenderTargetView(target.Get(), nullptr, rtvStart); auto blendRtv = rtvStart; blendRtv.ptr += rtvStride; d->CreateRenderTargetView(blendTarget.Get(), nullptr, blendRtv); auto floatRtv = blendRtv; floatRtv.ptr += rtvStride; d->CreateRenderTargetView(floatTarget.Get(), nullptr, floatRtv);
  D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}; ComPtr<ID3D12DescriptorHeap> dsvHeap; Hr(d->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsvHeap)), "DSV heap creates"); auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart(); d->CreateDepthStencilView(depthTarget.Get(), nullptr, dsv); auto floatDsv = dsv; floatDsv.ptr += d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); d->CreateDepthStencilView(floatDepthTarget.Get(), nullptr, floatDsv);
  ID3D12GraphicsCommandList *list = cmd.d3d12.cmdList;
  D3D12_VIEWPORT d3dViewport = {0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f};
  D3D12_RECT d3dScissor = {1, 1, 7, 7};
  list->RSSetViewports(1, &d3dViewport); list->RSSetScissorRects(1, &d3dScissor);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VERTEX_BUFFER_VIEW vb = {upload->GetGPUVirtualAddress(), 6 * 28, 28};
  list->IASetVertexBuffers(0, 1, &vb);
  D3D12_INDEX_BUFFER_VIEW ib = {indices->GetGPUVirtualAddress(), 6, DXGI_FORMAT_R16_UINT};
  list->IASetIndexBuffer(&ib); list->DrawInstanced(3, 1, 0, 0); list->DrawIndexedInstanced(3, 1, 0, 0, 0);

  D3D12_RESOURCE_DESC td = target->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowSize = 0, total = 0; d->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total); auto rb = Buffer(d, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST); D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &barrier); D3D12_TEXTURE_COPY_LOCATION src = {target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_TEXTURE_COPY_LOCATION dst = {rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint = fp; list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  float blendClear[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  list->OMSetRenderTargets(1, &blendRtv, FALSE, &dsv); list->ClearRenderTargetView(blendRtv, blendClear, 0, nullptr); list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  auto blendDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_BGRA8_UNORM, true, true, RI_CULL_MODE_NONE)); graphics.bindPipeline(&device, &cmd, 0x103, "blend and depth draw", blendDesc); list->DrawInstanced(3, 1, 0, 0); list->IASetIndexBuffer(&ib); list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  list->OMSetRenderTargets(1, &floatRtv, FALSE, &floatDsv); list->ClearRenderTargetView(floatRtv, clear, 0, nullptr); list->ClearDepthStencilView(floatDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  auto floatDesc = TriangleVertexInput(GraphicsDesc(RI_FORMAT_RGBA16_SFLOAT, false, true, RI_CULL_MODE_FRONT)); graphics.bindPipeline(&device, &cmd, 0x102, "alternate format cull draw", floatDesc); D3D12_INDEX_BUFFER_VIEW frontIb = {frontIndices->GetGPUVirtualAddress(), 6, DXGI_FORMAT_R16_UINT}; list->IASetIndexBuffer(&frontIb); list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  auto copyTexture = [&](ComPtr<ID3D12Resource> source) { D3D12_RESOURCE_DESC desc = source->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; UINT copyRows = 0; UINT64 copyRowSize = 0, copyTotal = 0; d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &copyRows, &copyRowSize, &copyTotal); auto readback = Buffer(d, D3D12_HEAP_TYPE_READBACK, copyTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST); D3D12_RESOURCE_BARRIER transition = {}; transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; transition.Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &transition); D3D12_TEXTURE_COPY_LOCATION from = {source.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_TEXTURE_COPY_LOCATION to = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; to.PlacedFootprint = footprint; list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr); return std::pair<ComPtr<ID3D12Resource>, D3D12_PLACED_SUBRESOURCE_FOOTPRINT>(readback, footprint); };
  auto blendCopy = copyTexture(blendTarget); auto floatCopy = copyTexture(floatTarget);
  D3D12_RESOURCE_DESC depthDesc = depthTarget->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFp = {}; UINT depthRows = 0; UINT64 depthRowSize = 0, depthTotal = 0; d->GetCopyableFootprints(&depthDesc, 0, 1, 0, &depthFp, &depthRows, &depthRowSize, &depthTotal); auto depthCopy = Buffer(d, D3D12_HEAP_TYPE_READBACK, depthTotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST); D3D12_RESOURCE_BARRIER depthBarrier = {}; depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; depthBarrier.Transition = {depthTarget.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &depthBarrier); D3D12_TEXTURE_COPY_LOCATION depthSrc = {depthTarget.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_TEXTURE_COPY_LOCATION depthDst = {depthCopy.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; depthDst.PlacedFootprint = depthFp; list->CopyTextureRegion(&depthDst, 0, 0, 0, &depthSrc, nullptr);

  RIBufferDesc outputDesc = {64 * 4, RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_SRC, RI_MEMORY_DEVICE, 0};
  RIBuffer output = RIBuffer::create(&device, outputDesc);
  Require(!output.isEmpty(), "compute output RI buffer creates");
  auto outputRb = Buffer(d, D3D12_HEAP_TYPE_READBACK, 64 * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  hpl::RIProgram::DescriptorBinding outputBinding("gOutput", RIDescriptor::storageBuffer(&device, &output, 0, 64 * 4, 4, false, true));
  compute.bindComputePipeline(&device, &cmd, 0x200, "compute miss");
  Require(compute.getD3D12PipelineCacheSize() == 1, "RIProgram compute PSO cache creates one entry on miss");
  compute.bindComputePipeline(&device, &cmd, 0x200, "compute hit");
  Require(compute.getD3D12PipelineCacheSize() == 1, "RIProgram compute PSO cache reuses the matching entry");
  compute.bindDescriptors(&device, &cmd, 0, &outputBinding, 1, VK_PIPELINE_BIND_POINT_COMPUTE);
  list->Dispatch(1, 1, 1); barrier.Transition = {output.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(outputRb.Get(), 0, output.d3d12.resource, 0, 64 * 4);
  // Root arguments must survive a pipeline bind that does not actually change
  // the root signature, and a descriptor bind that comes before the pipeline.
  // Both orders occur in the renderer (StandardTranslucentPass binds
  // descriptors once and then binds a pipeline per draw); before the shadowed
  // root-signature bind, the redundant SetComputeRootSignature wiped the
  // descriptor table and the dispatch read an uninitialized root argument.
  auto runOrderCase = [&](const char *label, bool descriptorsFirst,
                          uint32_t pipelineBinds) {
    RIBuffer caseOutput = RIBuffer::create(&device, outputDesc);
    Require(!caseOutput.isEmpty(), "root-argument case output buffer creates");
    auto caseRb = Buffer(d, D3D12_HEAP_TYPE_READBACK, 64 * 4,
                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    hpl::RIProgram::DescriptorBinding caseBinding(
        "gOutput", RIDescriptor::storageBuffer(&device, &caseOutput, 0, 64 * 4, 4,
                                                false, true));
    if (descriptorsFirst)
      compute.bindDescriptors(&device, &cmd, 0, &caseBinding, 1,
                              VK_PIPELINE_BIND_POINT_COMPUTE);
    for (uint32_t bind = 0; bind < pipelineBinds; ++bind)
      compute.bindComputePipeline(&device, &cmd, 0x200, label);
    if (!descriptorsFirst)
      compute.bindDescriptors(&device, &cmd, 0, &caseBinding, 1,
                              VK_PIPELINE_BIND_POINT_COMPUTE);
    // A second pipeline bind after the descriptors is the shape that broke:
    // it re-sets the same root signature, which invalidates the table.
    compute.bindComputePipeline(&device, &cmd, 0x200, label);
    cmd.dispatch(&device, 1, 1, 1);
    D3D12_RESOURCE_BARRIER caseBarrier = {};
    caseBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    caseBarrier.Transition = {caseOutput.d3d12.resource,
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &caseBarrier);
    list->CopyBufferRegion(caseRb.Get(), 0, caseOutput.d3d12.resource, 0, 64 * 4);
    return std::pair<RIBuffer, ComPtr<ID3D12Resource>>(caseOutput, caseRb);
  };
  const uint32_t missingRootArgsBefore = g_riD3D12MissingRootArgumentCount;
  auto pipelineFirstCase = runOrderCase("root args, pipeline first", false, 1);
  auto descriptorsFirstCase = runOrderCase("root args, descriptors first", true, 0);
  Require(g_riD3D12MissingRootArgumentCount == missingRootArgsBefore,
          "no dispatch reports an unbound root parameter");

  hpl::RIBindlessDescriptorSet uninitializedBindless;
  // The public bindless call currently rejects this uninitialized set on D3D12;
  // the backend-specific entry point is checked as well for the same reason.
  Require(!compute.bindBindlessDescriptorSet(&cmd, &uninitializedBindless, 0, VK_PIPELINE_BIND_POINT_COMPUTE), "RIProgram public bindless path validates ownership");
  Require(!compute.bindD3D12BindlessDescriptorSet(&cmd, &uninitializedBindless, true), "RIProgram bindless path validates ownership");
  auto guiDesc = GuiVertexInput(GraphicsDesc(RI_FORMAT_RGBA8_UNORM, true, false, RI_CULL_MODE_NONE));
  gui.bindPipeline(&device, &cmd, 0x300, "GUI shader", guiDesc);
  Require(gui.getD3D12PipelineCacheSize() == 1, "GUI shader RIProgram PSO creates");
  cmd.end(&device); Wait(q, d, list); D3D12_RANGE range = {0, (SIZE_T)total}; Hr(rb->Map(0, &range, &mapped), "triangle readback maps"); auto *pixels = (uint8_t *)mapped; auto pixel = [&](UINT x, UINT y) { return pixels + y * fp.Footprint.RowPitch + x * 4; }; bool colored = false; for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) { auto *px = pixel(x, y); colored |= px[0] || px[1] || px[2]; } Require(colored, "nonindexed and indexed vertex attributes reach render target"); Require(pixel(0, 0)[0] == 0 && pixel(0, 0)[1] == 0 && pixel(0, 0)[2] == 0, "scissor rejects pixels outside viewport intersection"); Require(pixel(4, 4)[0] || pixel(4, 4)[1] || pixel(4, 4)[2], "viewport maps triangle into target"); rb->Unmap(0, nullptr); range = {0, 64 * 4}; Hr(outputRb->Map(0, &range, &mapped), "compute readback maps"); bool exact = true; const auto *computeBytes = static_cast<const uint8_t *>(mapped); for (uint32_t i = 0; i < 64; ++i) { uint32_t expected = i; exact &= std::memcmp(computeBytes + i * sizeof(expected), &expected, sizeof(expected)) == 0; } outputRb->Unmap(0, nullptr); Require(exact, "compute storage buffer readback is byte-exact reference");

  auto requireSequence = [&](std::pair<RIBuffer, ComPtr<ID3D12Resource>> &c,
                             const char *what) {
    D3D12_RANGE caseRange = {0, 64 * 4};
    void *caseMapped = nullptr;
    Hr(c.second->Map(0, &caseRange, &caseMapped), "root-argument readback maps");
    bool byteExact = true;
    const auto *bytes = static_cast<const uint8_t *>(caseMapped);
    for (uint32_t i = 0; i < 64; ++i)
      byteExact &= std::memcmp(bytes + i * sizeof(i), &i, sizeof(i)) == 0;
    c.second->Unmap(0, nullptr);
    Require(byteExact, what);
    c.first.dispose(&device);
  };
  requireSequence(pipelineFirstCase,
                  "descriptor table survives a redundant pipeline bind after it");
  requireSequence(descriptorsFirstCase,
                  "descriptor table survives being bound before the pipeline");

  range = {0, static_cast<SIZE_T>(blendCopy.first->GetDesc().Width)}; Hr(blendCopy.first->Map(0, &range, &mapped), "blend readback maps"); const auto *blendPixel = static_cast<const uint8_t *>(mapped) + 4 * blendCopy.second.Footprint.RowPitch + 4 * 4; Require(blendPixel[2] > 120 && blendPixel[2] < 190 && blendPixel[1] < 100, "blend readback observes alpha compositing in BGRA8 attachment"); Require(blendPixel[2] > blendPixel[1] + 40, "blend readback observes depth rejection of the farther indexed triangle"); blendCopy.first->Unmap(0, nullptr);
  range = {0, static_cast<SIZE_T>(floatCopy.first->GetDesc().Width)}; Hr(floatCopy.first->Map(0, &range, &mapped), "float attachment readback maps"); const auto *floatPixel = static_cast<const uint8_t *>(mapped) + 4 * floatCopy.second.Footprint.RowPitch + 4 * 8; bool floatCleared = true; for (size_t i = 0; i < 8; ++i) floatCleared &= floatPixel[i] == 0; Require(floatCleared, "R16G16B16A16 readback observes front-face culling of the indexed triangle"); floatCopy.first->Unmap(0, nullptr);
  range = {0, static_cast<SIZE_T>(depthTotal)}; Hr(depthCopy->Map(0, &range, &mapped), "depth readback maps"); const auto *depthBytes = static_cast<const uint8_t *>(mapped); float depthValue = *reinterpret_cast<const float *>(depthBytes + 4 * depthFp.Footprint.RowPitch + 4 * sizeof(float)); Require(depthValue > 0.20f && depthValue < 0.30f, "depth readback observes nearer depth write after farther indexed draw was rejected"); depthCopy->Unmap(0, nullptr);
  const size_t debugErrors = DebugLayerErrors(d);
  Require(debugErrors == 0, "debug layer captures no D3D12 validation failures");

  output.dispose(&device); compute.dispose(&device); graphics.dispose(&device); gui.dispose(&device); cmd.dispose(&device); pool.dispose(&device); device.dispose(); ShutdownRIRenderer(); g_riD3D12EnableDebugLayer = false;
}
} // namespace

int main(int argc, char **argv) {
#if DEVICE_IMPL_D3D12
  Run(argc > 0 ? argv[0] : "RID3D12PSOSmoke.exe"); return 0;
#else
  std::printf("SKIP: DX12 not compiled in\n"); return 0;
#endif
}
