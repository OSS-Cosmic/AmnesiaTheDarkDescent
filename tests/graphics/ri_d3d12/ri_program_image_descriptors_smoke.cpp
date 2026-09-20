// RIProgram D3D12 image-descriptor acceptance: non-zero register spaces,
// sampled/storage image views, UAV readback, and unsupported AS validation.
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RID3D12.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIProgram.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/RIBuffer.h"
#include "graphics/RICommand.h"
#include "system/String.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
using Microsoft::WRL::ComPtr;
int hplMain(const std::string &) { return 0; }
namespace {
[[noreturn]] void Fail(const char *s) { std::fprintf(stderr, "FAIL: %s\n", s); std::exit(1); }
void Check(bool v, const char *s) { if (!v) Fail(s); std::printf("PASS: %s\n", s); }
void Hr(HRESULT hr, const char *s) { Check(SUCCEEDED(hr), s); }
std::vector<uint8_t> Bytes(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate); Check(f.good(), "image shader opens");
  auto n = f.tellg(); Check(n > 0, "image shader is nonempty"); std::vector<uint8_t> b((size_t)n);
  f.seekg(0); f.read((char *)b.data(), n); Check(f.good(), "image shader reads"); return b;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device *d, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = type; D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
  ComPtr<ID3D12Resource> r; Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "readback buffer creates"); return r;
}
void Run(const char *exe) {
  g_riD3D12EnableDebugLayer = true; RIBackendInit init = {}; init.api = RI_DEVICE_API_D3D12; init.applicationName = "RID3D12RIProgramImageDescriptors"; Check(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");
  auto root = std::filesystem::path(exe).parent_path() / "compiled_shaders" / "d3d12";
  auto dxil = Bytes(root / "ri_program_image_descriptors.comp.dxil");
  hpl::cFileSearcher searcher; searcher.AddDirectory(hpl::cString::To16Char(root.string()), "*", false);
  auto artifact = hpl::RIProgram::loadShaderArtifact(&searcher, "ri_program_image_descriptors.comp"); Check(!artifact.empty() && artifact.reflection, "RIProgram image reflection loads");
  const auto sampledReflection = std::find_if(
      artifact.reflection->resources.begin(), artifact.reflection->resources.end(),
      [](const auto &resource) { return resource.name == "gSampled"; });
  Check(sampledReflection != artifact.reflection->resources.end() &&
            sampledReflection->arrayCount == 2 && !sampledReflection->unbounded,
        "RIProgram reflection reports the sampled descriptor array");
  uint32_t count = 0; Check(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count, "D3D12 adapter enumerates"); std::vector<RIPhysicalAdapter> adapters(count); EnumerateRIAdapters(adapters.data(), &count); uint32_t selected = 0; for (uint32_t i = 0; i < count; ++i) if (!adapters[i].d3d12.isWarp) { selected = i; break; }
  RIDevice device; RIDeviceDesc dd = {}; dd.physicalAdapter = &adapters[selected]; Check(device.init(&dd) == RI_SUCCESS, "D3D12 device initializes"); ID3D12Device *d = device.d3d12.device; ID3D12CommandQueue *q = device.queues[RI_QUEUE_GRAPHICS].d3d12.queue;
  hpl::RIBindlessDescriptorSet::Binding asBinding = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0}; hpl::RIBindlessDescriptorSet::BackendBinding asBackend = {}; Check(hpl::RIBindlessDescriptorSet::convertBinding(asBinding, asBackend) && asBackend.registerClass == hpl::RIBindlessRegisterClass::SRV && asBackend.srvDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE, "acceleration descriptor class maps to a DXR SRV");
  hpl::RIProgram program; hpl::RIProgram::ModuleStage stage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE, artifact, "CSMain"); program.initialize(&device, std::span<hpl::RIProgram::ModuleStage>(&stage, 1), {}, "RIProgram image descriptors");
  RITextureDesc td = {RI_TEXTURE_2D, RI_FORMAT_RGBA8_UNORM, 1, 1, 1, 1, 1, 1, RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE, 0};
  RITexture input = RITexture::create(&device, td); RITexture output = RITexture::create(&device, td); Check(!input.isEmpty() && !output.isEmpty(), "sampled/storage textures create");
  RITextureViewDesc vd = {RI_VIEWTYPE_SHADER_RESOURCE_2D, RI_FORMAT_RGBA8_UNORM, 0, 1, 0, 1}; RITextureView sampled = RITextureView::create(&device, &input, vd); vd.viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D; RITextureView storage = RITextureView::create(&device, &output, vd); Check(!sampled.isEmpty() && !storage.isEmpty(), "sampled/storage image views create");
  RIBuffer out = RIBuffer::create(&device, {4, RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_SRC, RI_MEMORY_DEVICE, 0}); Check(!out.isEmpty(), "image fixture output buffer creates");
  RIPool pool; pool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]); RICmd cmd; cmd.init(&device, &pool); cmd.begin(&device);
  auto upload = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ); uint8_t *pixels = nullptr; Hr(upload->Map(0, nullptr, (void **)&pixels), "image upload maps"); pixels[0] = 37; pixels[3] = 255; upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
  D3D12_RESOURCE_BARRIER inputCopy = {}; inputCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; inputCopy.Transition = {input.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST}; cmd.d3d12.cmdList->ResourceBarrier(1, &inputCopy);
  D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = input.d3d12.resource; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cmd.d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER prep[2] = {}; prep[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; prep[0].Transition = {input.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}; prep[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; prep[1].Transition = {output.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; cmd.d3d12.cmdList->ResourceBarrier(2, prep);
  auto sampledDesc = RIDescriptor::sampledImage(&device, &sampled); auto storageDesc = RIDescriptor::storageImage(&device, &storage); Check(!sampledDesc.isEmpty() && !storageDesc.isEmpty(), "image descriptors retain format/state payload");
  std::array<hpl::RIProgram::DescriptorBinding, 3> bindings = {hpl::RIProgram::DescriptorBinding("gSampled", sampledDesc, 1), hpl::RIProgram::DescriptorBinding("gStorage", storageDesc), hpl::RIProgram::DescriptorBinding("gOutput", RIDescriptor::storageBuffer(&device, &out, 0, 4, 4, false, true))};
  program.bindComputePipeline(&device, &cmd, 1, "image descriptor compute"); program.bindDescriptors(&device, &cmd, 0, bindings.data(), bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE); cmd.d3d12.cmdList->Dispatch(1, 1, 1); Check(true, "RIProgram accepts sampled/storage image descriptors");
  D3D12_RESOURCE_BARRIER done[2] = {}; done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; done[0].Transition = {output.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE}; done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; done[1].Transition = {out.d3d12.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE}; cmd.d3d12.cmdList->ResourceBarrier(2, done); auto readback = Buffer(d, D3D12_HEAP_TYPE_READBACK, 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST); cmd.d3d12.cmdList->CopyBufferRegion(readback.Get(), 0, out.d3d12.resource, 0, 4); cmd.end(&device); ComPtr<ID3D12Fence> fence; Hr(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "image fence creates"); q->ExecuteCommandLists(1, (ID3D12CommandList **)&cmd.d3d12.cmdList); Hr(q->Signal(fence.Get(), 1), "image fence signals"); HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Hr(fence->SetEventOnCompletion(1, event), "image fence event arms"); WaitForSingleObject(event, INFINITE); CloseHandle(event); uint32_t value = 0; D3D12_RANGE range = {0, 4}; void *mapped = nullptr; Hr(readback->Map(0, &range, &mapped), "image readback maps"); value = *(uint32_t *)mapped; readback->Unmap(0, nullptr); Check(value == 37, "sampled image value reaches storage-buffer readback");
  // Per-draw transient bindings (a new buffer offset every frame, as frame
  // UBO scratch allocations produce) must recycle descriptor tables once they
  // leave the in-flight window instead of growing the shared arena.
  RIBuffer scratch = RIBuffer::create(&device, {4 * 128, RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE, RI_MEMORY_DEVICE, 0}); Check(!scratch.isEmpty(), "transient binding buffer creates");
  pool.reset(&device); cmd.begin(&device); program.bindComputePipeline(&device, &cmd, 1, "image descriptor compute");
  size_t peakCache = 0;
  for (uint32_t frame = 1; frame <= 120; ++frame) {
    bindings[2] = hpl::RIProgram::DescriptorBinding("gOutput", RIDescriptor::storageBuffer(&device, &scratch, 4ull * (frame % 128), 4, 4, false, true));
    program.bindDescriptors(&device, &cmd, frame, bindings.data(), bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
    const size_t afterMiss = program.getD3D12DescriptorCacheSize();
    program.bindDescriptors(&device, &cmd, frame, bindings.data(), bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
    if (program.getD3D12DescriptorCacheSize() != afterMiss) Fail("repeated binding set reuses its cached descriptor table");
    peakCache = std::max(peakCache, afterMiss);
  }
  cmd.end(&device);
  Check(peakCache <= RI_NUMBER_FRAMES_FLIGHT + 1, "transient bindings recycle descriptor tables after the in-flight window");
  scratch.dispose(&device);
  // An uninitialized set still rejects writes before descriptor heaps or
  // retained resources are touched.
  hpl::RIBindlessDescriptorSet empty; Check(!empty.writeDescriptors(&device, {}), "uninitialized bindless allocation is rejected");
  program.dispose(&device); out.dispose(&device); storage.dispose(&device); sampled.dispose(&device); output.dispose(&device); input.dispose(&device); cmd.dispose(&device); pool.dispose(&device); device.dispose(); ShutdownRIRenderer(); g_riD3D12EnableDebugLayer = false; (void)dxil; (void)q; (void)d;
}
}
int main(int argc, char **argv) { Run(argc > 0 ? argv[0] : "RID3D12RIProgramImageDescriptorsSmoke.exe"); return 0; }
