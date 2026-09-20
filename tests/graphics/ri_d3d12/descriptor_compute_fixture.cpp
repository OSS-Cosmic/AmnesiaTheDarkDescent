// Focused native D3D12 descriptor/compute fixture.
//
// RIProgram currently keeps DXIL opaque and its descriptor/pipeline bind paths
// are Vulkan-only.  This test therefore drives the same shader contract with
// the native D3D12 command API, while keeping the register/name assertions in
// the fixture itself.  It is intentionally opt-in and GPU-only.
#include "graphics/RIDevice.h"
#include "graphics/RID3D12.h"
#include "graphics/RICommand.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIRenderer.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int hplMain(const std::string &) { return 0; }

namespace {

constexpr uint32_t kUniformValue = 23;
constexpr uint32_t kTextureValue = 17;
constexpr uint32_t kExpectedValue = kUniformValue + kTextureValue;
constexpr uint32_t kCbvOffset = 256;

[[noreturn]] void Fail(const char *what) {
  std::fprintf(stderr, "FAIL: %s\n", what);
  std::exit(1);
}

void Require(bool ok, const char *what) {
  if (!ok) Fail(what);
  std::printf("PASS: %s\n", what);
}

void RequireHr(HRESULT hr, const char *what) { Require(SUCCEEDED(hr), what); }

std::vector<uint8_t> ReadBytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Require(file.good(), "DXIL fixture artifact opens");
  const auto size = file.tellg();
  Require(size > 0, "DXIL fixture artifact is nonempty");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(bytes.data()), size);
  Require(file.good(), "DXIL fixture artifact reads");
  return bytes;
}

std::string ReadText(const std::filesystem::path &path) {
  std::ifstream file(path);
  Require(file.good(), "DXIL fixture sidecar opens");
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

void CheckArtifactContract(const std::filesystem::path &root) {
  const auto dxil = root / "descriptor_compute.comp.dxil";
  const auto meta = ReadText(dxil.string() + ".meta");
  Require(meta.find("HPL2_SHADER_ARTIFACT\n") == 0,
          "DXIL metadata has the fixture artifact envelope");
  Require(meta.find("version=1\n") != std::string::npos &&
              meta.find("format=dxil\n") != std::string::npos,
          "DXIL metadata version and format agree");
  Require(meta.find("stage=compute\n") != std::string::npos &&
              meta.find("entry=CSMain\n") != std::string::npos &&
              meta.find("reflection=descriptor_compute.comp.dxil.reflection-v1.json\n") !=
                  std::string::npos,
          "DXIL metadata links the compute entry point and reflection sidecar");

  const auto reflection = ReadText(dxil.string() + ".reflection-v1.json");
  for (const char *name : {"gUniforms", "gTexture", "gOptionalTextures",
                           "gSampler", "gOutput"}) {
    Require(reflection.find(name) != std::string::npos,
            "DXIL reflection contains every named fixture binding");
  }
  for (const char *binding : {
           "\"kind\": \"constantBuffer\", \"index\": 0",
           "\"kind\": \"shaderResource\", \"index\": 0",
           "\"kind\": \"samplerState\", \"index\": 0",
           "\"kind\": \"unorderedAccess\", \"index\": 0"}) {
    Require(reflection.find(binding) != std::string::npos,
            "DXIL reflection contains every numeric fixture register");
  }
  Require(reflection.find("\"name\": \"gOptionalTextures\"") !=
              std::string::npos &&
              reflection.find("\"count\": 2") != std::string::npos,
          "DXIL reflection preserves the optional texture array slots");
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device *device, D3D12_HEAP_TYPE heapType,
                                  UINT64 size, D3D12_RESOURCE_FLAGS flags,
                                  D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heapType;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> result;
  RequireHr(device->CreateCommittedResource(
                 &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                 IID_PPV_ARGS(&result)),
             "D3D12 fixture buffer creates");
  (void)flags;
  return result;
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device *device) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 1;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> result;
  RequireHr(device->CreateCommittedResource(
                 &heap, D3D12_HEAP_FLAG_NONE, &desc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)),
             "D3D12 fixture texture creates");
  return result;
}

void WaitForFence(ID3D12Fence *fence, uint64_t value, HANDLE event) {
  if (fence->GetCompletedValue() < value) {
    RequireHr(fence->SetEventOnCompletion(value, event),
              "fixture fence event arms");
    Require(WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0,
            "fixture fence reaches completion");
  }
}

void RunFixture(const char *executable) {
  const std::filesystem::path shaderRoot =
      std::filesystem::path(executable).parent_path() / "compiled_shaders" / "d3d12";
  CheckArtifactContract(shaderRoot);
  const std::vector<uint8_t> shader =
      ReadBytes(shaderRoot / "descriptor_compute.comp.dxil");
  Require(shader.size() >= 4 && shader[0] == 'D' && shader[1] == 'X' &&
              shader[2] == 'B' && shader[3] == 'C',
          "fixture shader is DXIL");

  g_riD3D12EnableDebugLayer = true;
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12DescriptorComputeFixture";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");

  uint32_t count = 0;
  Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count > 0,
          "D3D12 adapter enumeration succeeds");
  std::vector<RIPhysicalAdapter> adapters(count);
  Require(EnumerateRIAdapters(adapters.data(), &count) == RI_SUCCESS,
          "D3D12 adapter enumeration populates adapters");
  uint32_t selected = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (!adapters[i].d3d12.isWarp) { selected = i; break; }

  RIDevice device;
  RIDeviceDesc deviceDesc = {};
  deviceDesc.physicalAdapter = &adapters[selected];
  Require(device.init(&deviceDesc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 device initializes and is valid");

  ID3D12Device *d3d = device.d3d12.device;
  D3D12_DESCRIPTOR_HEAP_DESC resourceHeapDesc = {
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> resourceHeap;
  RequireHr(d3d->CreateDescriptorHeap(&resourceHeapDesc,
                                      IID_PPV_ARGS(&resourceHeap)),
             "resource descriptor heap creates");
  D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1,
      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> samplerHeap;
  RequireHr(d3d->CreateDescriptorHeap(&samplerHeapDesc,
                                      IID_PPV_ARGS(&samplerHeap)),
             "sampler descriptor heap creates");
  const UINT resourceStride =
      d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT samplerStride =
      d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

  D3D12_DESCRIPTOR_RANGE ranges[4] = {};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 1};
  ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4};
  ranges[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 3;
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &ranges[3];
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {2, params, 0, nullptr,
                                        D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rootBlob, rootError;
  RequireHr(D3D12SerializeRootSignature(&rootDesc,
                                        D3D_ROOT_SIGNATURE_VERSION_1,
                                        &rootBlob, &rootError),
             "root signature serializes");
  ComPtr<ID3D12RootSignature> root;
  RequireHr(d3d->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                     rootBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&root)),
             "root signature creates");

  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = root.Get();
  psoDesc.CS = {shader.data(), shader.size()};
  ComPtr<ID3D12PipelineState> pso;
  RequireHr(d3d->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)),
             "compute pipeline creates from fixture DXIL");

  auto constants = MakeBuffer(d3d, D3D12_HEAP_TYPE_UPLOAD, 512,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mappedConstants = nullptr;
  RequireHr(constants->Map(0, nullptr, reinterpret_cast<void **>(&mappedConstants)),
            "uniform buffer maps");
  *reinterpret_cast<uint32_t *>(mappedConstants + kCbvOffset) = kUniformValue;
  constants->Unmap(0, nullptr);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {
      constants->GetGPUVirtualAddress() + kCbvOffset, 256};
  auto resourceCpu = resourceHeap->GetCPUDescriptorHandleForHeapStart();
  d3d->CreateConstantBufferView(&cbv, resourceCpu);

  auto texture = MakeTexture(d3d);
  auto textureUpload = MakeBuffer(d3d, D3D12_HEAP_TYPE_UPLOAD, 256,
                                  D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mappedTexture = nullptr;
  RequireHr(textureUpload->Map(0, nullptr, reinterpret_cast<void **>(&mappedTexture)),
            "texture upload maps");
  mappedTexture[0] = kTextureValue; mappedTexture[1] = 0;
  mappedTexture[2] = 0; mappedTexture[3] = 255;
  textureUpload->Unmap(0, nullptr);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  auto srvCpu = resourceCpu; srvCpu.ptr += resourceStride;
  d3d->CreateShaderResourceView(texture.Get(), &srv, srvCpu);
  auto nullCpu = srvCpu; nullCpu.ptr += resourceStride;
  d3d->CreateShaderResourceView(nullptr, &srv, nullCpu);
  nullCpu.ptr += resourceStride;
  d3d->CreateShaderResourceView(nullptr, &srv, nullCpu);

  D3D12_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MinLOD = 0; sampler.MaxLOD = D3D12_FLOAT32_MAX;
  d3d->CreateSampler(&sampler, samplerHeap->GetCPUDescriptorHandleForHeapStart());

  const uint32_t submissionCount = RI_NUMBER_FRAMES_FLIGHT + 2;
  std::vector<ComPtr<ID3D12Resource>> outputs(submissionCount);
  std::vector<ComPtr<ID3D12Resource>> readbacks(submissionCount);
  std::vector<ComPtr<ID3D12CommandAllocator>> allocators(submissionCount);
  std::vector<ComPtr<ID3D12GraphicsCommandList>> lists(submissionCount);
  std::vector<ComPtr<ID3D12DescriptorHeap>> resourceHeaps(submissionCount);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = 1;
  uav.Buffer.StructureByteStride = sizeof(uint32_t);

  RIPool uploadPool;
  uploadPool.init(&device, &device.queues[RI_QUEUE_GRAPHICS]);
  RICmd uploadCmd;
  uploadCmd.init(&device, &uploadPool);
  uploadCmd.begin(&device);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = textureUpload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src.PlacedFootprint.Footprint.Width = 1; src.PlacedFootprint.Footprint.Height = 1;
  src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = 256;
  uploadCmd.d3d12.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER textureBarrier = {};
  textureBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  textureBarrier.Transition.pResource = texture.Get();
  textureBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  textureBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  textureBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  uploadCmd.d3d12.cmdList->ResourceBarrier(1, &textureBarrier);
  uploadCmd.end(&device);
  ID3D12CommandList *uploadLists[] = {uploadCmd.d3d12.cmdList};
  device.queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1, uploadLists);
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
  uploadCmd.dispose(&device); uploadPool.dispose(&device);

  for (uint32_t i = 0; i < submissionCount; ++i) {
    outputs[i] = MakeBuffer(d3d, D3D12_HEAP_TYPE_DEFAULT, 4,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readbacks[i] = MakeBuffer(d3d, D3D12_HEAP_TYPE_READBACK, 4,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    RequireHr(d3d->CreateDescriptorHeap(&resourceHeapDesc,
                                        IID_PPV_ARGS(&resourceHeaps[i])),
              "delayed fixture resource heap creates");
    auto submissionCpu = resourceHeaps[i]->GetCPUDescriptorHandleForHeapStart();
    d3d->CopyDescriptorsSimple(4, submissionCpu, resourceCpu,
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto outputCpu = submissionCpu; outputCpu.ptr += 4 * resourceStride;
    d3d->CreateUnorderedAccessView(outputs[i].Get(), nullptr, &uav, outputCpu);
    RequireHr(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&allocators[i])),
               "delayed fixture command allocator creates");
    RequireHr(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocators[i].Get(), pso.Get(),
                                      IID_PPV_ARGS(&lists[i])),
               "delayed fixture command list creates");
    ID3D12DescriptorHeap *heaps[] = {resourceHeaps[i].Get(), samplerHeap.Get()};
    lists[i]->SetDescriptorHeaps(2, heaps);
    lists[i]->SetComputeRootSignature(root.Get());
    lists[i]->SetPipelineState(pso.Get());
    lists[i]->SetComputeRootDescriptorTable(
        0, resourceHeaps[i]->GetGPUDescriptorHandleForHeapStart());
    lists[i]->SetComputeRootDescriptorTable(1, samplerHeap->GetGPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER outputToUav = {};
    outputToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outputToUav.Transition.pResource = outputs[i].Get();
    outputToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    outputToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    lists[i]->ResourceBarrier(1, &outputToUav);
    lists[i]->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER outputBarrier = {};
    outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outputBarrier.Transition.pResource = outputs[i].Get();
    outputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    outputBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    lists[i]->ResourceBarrier(1, &outputBarrier);
    lists[i]->CopyBufferRegion(readbacks[i].Get(), 0, outputs[i].Get(), 0, 4);
    RequireHr(lists[i]->Close(), "delayed fixture command list closes");
  }

  ComPtr<ID3D12Fence> fence;
  RequireHr(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
             "delayed fixture fence creates");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Require(event != nullptr, "delayed fixture fence event creates");
  uint64_t lastFence = 0;
  for (uint32_t i = 0; i < submissionCount; ++i) {
    ID3D12CommandList *commandLists[] = {lists[i].Get()};
    device.queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1, commandLists);
    RequireHr(device.queues[RI_QUEUE_GRAPHICS].d3d12.queue->Signal(fence.Get(), ++lastFence),
              "delayed fixture submission signals");
  }
  Require(submissionCount > RI_NUMBER_FRAMES_FLIGHT,
          "delayed submissions exceed frames in flight");
  WaitForFence(fence.Get(), lastFence, event);
  CloseHandle(event);
  for (const auto &readback : readbacks) {
    void *mapped = nullptr;
    D3D12_RANGE range = {0, 4};
    RequireHr(readback->Map(0, &range, &mapped), "compute readback maps");
    Require(*static_cast<uint32_t *>(mapped) == kExpectedValue,
            "compute descriptor fixture writes the sampled uniform result");
    readback->Unmap(0, nullptr);
  }

  device.dispose();
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}

} // namespace

int main(int argc, char **argv) {
  RunFixture(argc > 0 ? argv[0] : "RID3D12DescriptorComputeFixture.exe");
  return 0;
}
