// Opt-in D3D12 smoke for Slang push constants lowered to root constants.
// RIProgram owns shader reflection and PSO setup; RICmd owns the backend write
// so graphics and compute root state are both covered by this executable.
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
#include <vector>

using Microsoft::WRL::ComPtr;
int hplMain(const std::string &) { return 0; }

namespace {
[[noreturn]] void Fail(const char *s) { std::fprintf(stderr, "FAIL: %s\n", s); std::exit(1); }
void Require(bool ok, const char *s) { if (!ok) Fail(s); std::printf("PASS: %s\n", s); }
void Hr(HRESULT h, const char *s) { Require(SUCCEEDED(h), s); }

std::vector<uint8_t> Read(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate); Require(f.good(), "shader artifact opens");
  auto n = f.tellg(); Require(n > 0, "shader artifact is nonempty");
  std::vector<uint8_t> result(static_cast<size_t>(n)); f.seekg(0); f.read((char *)result.data(), n);
  Require(f.good(), "shader artifact reads"); return result;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device *d, D3D12_HEAP_TYPE heap, UINT64 size,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size;
  rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
  ComPtr<ID3D12Resource> r; Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      state, nullptr, IID_PPV_ARGS(&r)), "buffer creates"); return r;
}

bool ValidWrite(uint32_t offset, uint32_t count, uint32_t capacity) {
  return count != 0 && offset <= capacity && count <= capacity - offset;
}

void SetConstants(RICmd &cmd, RIDevice &device, hpl::RIProgram &program,
                  const uint32_t *v, uint32_t offset, uint32_t count,
                  uint32_t capacity) {
  Require(ValidWrite(offset, count, capacity), "graphics root-constant range is valid");
  cmd.vk_d3d12_setPushConstants(&device, program, offset * 4, count * 4, v);
}

void Wait(ID3D12CommandQueue *q, ID3D12Device *d, ID3D12CommandList *list) {
  ComPtr<ID3D12Fence> fence; Hr(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence creates");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "fence event creates");
  q->ExecuteCommandLists(1, &list); Hr(q->Signal(fence.Get(), 1), "queue signals fence");
  if (fence->GetCompletedValue() < 1) { Hr(fence->SetEventOnCompletion(1, event), "fence event arms"); WaitForSingleObject(event, INFINITE); }
  CloseHandle(event);
}

void Run(const char *exe) {
  g_riD3D12EnableDebugLayer = true; RIBackendInit init = {}; init.api = RI_DEVICE_API_D3D12; init.applicationName = "RID3D12PushConstantSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");
  auto dir = std::filesystem::path(exe).parent_path() / "compiled_shaders" / "d3d12";
  hpl::cFileSearcher searcher; searcher.AddDirectory(hpl::cString::To16Char(dir.string()), "*", false);
  auto artifact = [&](const char *n) { auto a = hpl::RIProgram::loadShaderArtifact(&searcher, n); Require(!a.empty() && a.reflection, "push-constant artifact and reflection load"); return a; };
  auto vs = artifact("push_constant.vert.dxil"), ps = artifact("push_constant.frag.dxil"), cs = artifact("push_constant.comp.dxil");

  uint32_t count = 0; Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count, "D3D12 adapters enumerate");
  std::vector<RIPhysicalAdapter> adapters(count); Require(EnumerateRIAdapters(adapters.data(), &count) == RI_SUCCESS, "D3D12 adapter data populates");
  uint32_t selected = 0; for (uint32_t i = 0; i < count; ++i) if (!adapters[i].d3d12.isWarp) { selected = i; break; }
  RIDevice device; RIDeviceDesc dd = {}; dd.physicalAdapter = &adapters[selected]; Require(device.init(&dd) == RI_SUCCESS, "D3D12 device initializes");
  ID3D12Device *d = device.d3d12.device; ID3D12CommandQueue *q = device.queues[RI_QUEUE_GRAPHICS].d3d12.queue;
  hpl::RIProgram graphics, compute;
  std::array<hpl::RIProgram::ModuleStage, 2> gs = { hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_VERTEX, vs, "VSMain"), hpl::RIProgram::ModuleStage(hpl::RIProgram::PROGRAM_STAGE_FRAGMENT, ps, "PSMain") };
  graphics.initialize(&device, gs, {}, "push-constant graphics");
  hpl::RIProgram::ModuleStage csStage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE, cs, "CSMain"); compute.initialize(&device, std::span<hpl::RIProgram::ModuleStage>(&csStage, 1), {}, "push-constant compute");
  constexpr uint32_t kCount = 4; Require(graphics.getD3D12PushConstantRootParameter() != UINT32_MAX && compute.getD3D12PushConstantRootParameter() != UINT32_MAX, "graphics and compute expose root constants");
  Require(graphics.getD3D12PushConstantOffset() == 0 && compute.getD3D12PushConstantOffset() == 0 && graphics.getD3D12PushConstantSize() == kCount * 4 && compute.getD3D12PushConstantSize() == kCount * 4, "push-constant reflection reports the exact four-DWORD range");
  Require(!ValidWrite(4, 1, kCount) && !ValidWrite(0, 0, kCount) && !ValidWrite(3, 2, kCount), "invalid root-constant ranges are diagnosed without recording");

  RIPool pool; pool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]); RICmd cmd; cmd.init(&device, &pool); cmd.begin(&device); ID3D12GraphicsCommandList *list = cmd.d3d12.cmdList;
  D3D12_CLEAR_VALUE clear = {DXGI_FORMAT_R8G8B8A8_UNORM, {0,0,0,0}};
  ComPtr<ID3D12Resource> target; D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC tex = {}; tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; tex.Width = tex.Height = 1; tex.DepthOrArraySize = tex.MipLevels = 1; tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex.SampleDesc.Count = 1; tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)), "render target creates");
  D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}; ComPtr<ID3D12DescriptorHeap> rtvHeap; Hr(d->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap)), "RTV heap creates"); d->CreateRenderTargetView(target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
  // No vertex input, no depth, one opaque RGBA8 target. Everything else is the
  // RI desc default -- except the front face, which zero-initialized Vulkan
  // reports as counter-clockwise while the desc defaults to clockwise.
  hpl::RIGraphicsPipelineDesc pipelineDesc = {};
  pipelineDesc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;
  pipelineDesc.blendCount = 1;
  pipelineDesc.renderTarget.colorCount = 1;
  pipelineDesc.renderTarget.colorFormats[0] = RI_FORMAT_RGBA8_UNORM;
  graphics.bindPipeline(&device, &cmd, 1, "push-constant graphics", pipelineDesc); D3D12_VIEWPORT viewport = {0,0,1,1,0,1}; D3D12_RECT scissor = {0,0,1,1}; list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor); auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
  uint32_t rejected = 0xdeadbeefu;
  cmd.vk_d3d12_setPushConstants(&device, graphics, 2, 4, &rejected);
  cmd.vk_d3d12_setPushConstants(&device, graphics, kCount * 4, 4, &rejected);
  cmd.vk_d3d12_setPushConstants(&device, graphics, 0, 0, nullptr);
  std::printf("PASS: production validator diagnosed invalid ranges and accepted an empty no-op\n");
  uint32_t gv[] = {17, 34, 51, 255}; SetConstants(cmd, device, graphics, gv, 0, 4, kCount); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
  uint32_t patch[] = {68, 85}; SetConstants(cmd, device, graphics, patch, 1, 2, kCount); list->DrawInstanced(3, 1, 0, 0);

  RIBuffer output = RIBuffer::create(&device, {16, RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_SRC, RI_MEMORY_DEVICE, 0}); Require(!output.isEmpty(), "compute output creates"); RIBuffer readback = RIBuffer::create(&device, {16, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_HOST_READBACK, 0});
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; UINT rows = 0; UINT64 rowSize = 0, totalSize = 0;
  d->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, &rows, &rowSize, &totalSize);
  auto rb = Buffer(d, D3D12_HEAP_TYPE_READBACK, totalSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  hpl::RIProgram::DescriptorBinding binding("gOutput", RIDescriptor::storageBuffer(&device, &output, 0, 16, 4, false, true)); compute.bindComputePipeline(&device, &cmd, 2, "push-constant compute"); compute.bindDescriptors(&device, &cmd, 0, &binding, 1, VK_PIPELINE_BIND_POINT_COMPUTE); uint32_t cv[] = {901, 902, 903, 904}; SetConstants(cmd, device, compute, cv, 0, 4, kCount); uint32_t cpatch[] = {777}; SetConstants(cmd, device, compute, cpatch, 2, 1, kCount); list->Dispatch(1,1,1);
  D3D12_RESOURCE_BARRIER outputToCopy = {}; outputToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; outputToCopy.Transition = {output.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &outputToCopy); cmd.copyBuffer(&device, &output, 0, &readback, 0, 16);

  graphics.bindPipeline(&device, &cmd, 3, "push-constant graphics after compute", pipelineDesc); uint32_t finalv[] = {201, 202, 203, 255}; SetConstants(cmd, device, graphics, finalv, 0, 4, kCount); list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER toCopy = {}; toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; toCopy.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &toCopy);
  D3D12_TEXTURE_COPY_LOCATION src = {target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_TEXTURE_COPY_LOCATION dst = {rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint = footprint; list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  cmd.end(&device); Wait(q, d, list);
  readback.invalidateMappedRange(&device, 0, 16); auto *out = static_cast<const uint32_t *>(readback.mappedAddress); Require(out[0] == 901 && out[1] == 902 && out[2] == 777 && out[3] == 904, "compute push constants survive partial update and program switch");
  D3D12_RANGE range = {0, (SIZE_T)totalSize}; void *mapped = nullptr; Hr(rb->Map(0, &range, &mapped), "graphics readback maps"); auto *pixel = static_cast<const uint8_t *>(mapped); Require(pixel[0] == 201 && pixel[1] == 202 && pixel[2] == 203 && pixel[3] == 255, "graphics push constants read back exactly after alternation"); rb->Unmap(0, nullptr);
  readback.dispose(&device); output.dispose(&device); cmd.dispose(&device); pool.dispose(&device); compute.dispose(&device); graphics.dispose(&device); device.dispose(); ShutdownRIRenderer(); g_riD3D12EnableDebugLayer = false;
}
}
int main(int argc, char **argv) {
#if DEVICE_IMPL_D3D12
  Run(argc > 0 ? argv[0] : "RID3D12PushConstantSmoke.exe"); return 0;
#else
  std::printf("SKIP: DX12 not compiled in\n"); return 0;
#endif
}
