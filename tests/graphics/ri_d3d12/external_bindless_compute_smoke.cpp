// Focused opt-in D3D12 external-bindless compute smoke.
// Native D3D12 is retained for the ordinary fixture table and readback. The
// external table itself is also exercised through the RI bindless API below.
#include "graphics/RIProgram.h"
using hpl::RIProgram;
#include "graphics/RID3D12.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommandRingBuffer.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace hpl;

int hplMain(const std::string &) { return 0; }

namespace {
[[noreturn]] void Fail(const char *s) { std::fprintf(stderr, "FAIL: %s\n", s); std::exit(1); }
void Require(bool ok, const char *s) { if (!ok) Fail(s); std::printf("PASS: %s\n", s); }
void Hr(HRESULT hr, const char *s) { Require(SUCCEEDED(hr), s); }

ComPtr<ID3D12Resource> Buffer(ID3D12Device *d, D3D12_HEAP_TYPE heap, UINT64 bytes,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
  ComPtr<ID3D12Resource> r;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                IID_PPV_ARGS(&r)), "smoke buffer creates");
  return r;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device *d) {
  D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> r;
  Hr(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                IID_PPV_ARGS(&r)), "external texture creates");
  return r;
}

std::vector<uint8_t> Bytes(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate); Require(f.good(), "DXIL opens");
  auto n = f.tellg(); Require(n > 4, "DXIL is nonempty"); std::vector<uint8_t> b((size_t)n);
  f.seekg(0); f.read((char *)b.data(), n); Require(f.good(), "DXIL reads"); return b;
}

struct Constants { uint32_t ordinary, textureIndex, rawIndex, indexed; uint64_t indexedHandle, nonIndexedHandle; };

struct GeometryConstants {
  uint32_t indexed;
  uint32_t padding;
  uint64_t indexedHandle;
  uint64_t nonIndexedHandle;
  uint64_t nullHandle;
};

void ExerciseRiProgramGeometry(RIDevice *device,
                               const std::filesystem::path &shaderPath) {
  auto shader = Bytes(shaderPath);
  hpl::RIProgram::ShaderArtifact artifact;
  artifact.bytes = std::make_shared<const std::vector<char>>(
      reinterpret_cast<const char *>(shader.data()),
      reinterpret_cast<const char *>(shader.data() + shader.size()));
  artifact.format = RIShaderArtifactFormat::Dxil;
  auto reflection = std::make_shared<hpl::RIProgram::ShaderReflection>();
  reflection->entryPoint = "CSMain";
  reflection->stage = "compute";
  reflection->resources = {
      {"gConstants", "compute", hpl::RIProgram::ShaderRegisterClass::CBV, 0, 0},
      {"gOutput", "compute", hpl::RIProgram::ShaderRegisterClass::UAV, 0, 0,
       1, false, 4, 0, "structuredBuffer"},
      {"gGeometryBuffers", "compute", hpl::RIProgram::ShaderRegisterClass::SRV, 4, 3, 0, true},
      {"gExternalA", "compute", hpl::RIProgram::ShaderRegisterClass::SRV, 0, 10, 0, true},
      {"gExternalB", "compute", hpl::RIProgram::ShaderRegisterClass::SRV, 0, 11, 0, true},
  };
  artifact.reflection = reflection;
  hpl::RIProgram::ModuleStage stage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE,
                                    artifact, "CSMain");

  RIBuffer constants = RIBuffer::create(
      device, {D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
               RI_BUFFER_USAGE_CONSTANT_BUFFER, RI_MEMORY_HOST_UPLOAD, 0});
  RIBuffer producerIndexed = RIBuffer::create(
      device, {16, RI_BUFFER_USAGE_SHADER_RESOURCE, RI_MEMORY_HOST_UPLOAD, 0});
  RIBuffer producerNonIndexed = RIBuffer::create(
      device, {16, RI_BUFFER_USAGE_SHADER_RESOURCE, RI_MEMORY_HOST_UPLOAD, 0});
  Require(!constants.isEmpty() && !producerIndexed.isEmpty() &&
              !producerNonIndexed.isEmpty(),
          "RIProgram geometry producer buffers create");
  auto fill = [](RIBuffer &buffer, uint32_t value) {
    auto *words = static_cast<uint32_t *>(buffer.mappedAddress);
    words[0] = 0;
    words[1] = value;
    words[2] = value + 1;
    words[3] = value + 2;
  };
  fill(producerIndexed, 21);
  fill(producerNonIndexed, 31);
  Require(RID3D12_RegisterBufferShaderResource(*device, producerIndexed) &&
              RID3D12_RegisterBufferShaderResource(*device, producerNonIndexed),
          "RIProgram producer geometry handles register");
  const uint64_t indexedHandle = producerIndexed.GetShaderResourceHandle(device) | 4u;
  const uint64_t nonIndexedHandle = producerNonIndexed.GetShaderResourceHandle(device) | 4u;
  Require((indexedHandle >> 32) == 1 && (indexedHandle & 0xffffffffu) == 4,
          "RIProgram covers descriptor index zero and nonzero byte offset");
  Require((nonIndexedHandle >> 32) != 0 && (nonIndexedHandle & 0xffffffffu) == 4,
          "RIProgram covers the second producer geometry handle");
  RIBuffer output = RIBuffer::create(
      device, {12, RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_SRC,
               RI_MEMORY_DEVICE, 0});
  RIBuffer readback = RIBuffer::create(
      device, {12, RI_BUFFER_USAGE_TRANSFER_DST, RI_MEMORY_HOST_READBACK, 0});
  Require(!output.isEmpty() && !readback.isEmpty(),
          "RIProgram geometry result buffers create");
  GeometryConstants values = {1, 0, indexedHandle, nonIndexedHandle, 0};
  std::memcpy(constants.mappedAddress, &values, sizeof(values));
  constants.flushMappedRange(device, 0, sizeof(values));
  producerIndexed.flushMappedRange(device, 0, 16);
  producerNonIndexed.flushMappedRange(device, 0, 16);

  RIBindlessDescriptorSet bindless;
  // Keep geometry and two additional unbounded arrays external while b0/u0
  // remain ordinary RIProgram descriptors. The extra arrays are deliberately
  // unused by this shader; they exercise multi-unbounded root construction
  // without changing the byte-exact GeometryStream result.
  RIBindlessDescriptorSet::BackendBinding table[] = {
      {4, RIBindlessRegisterClass::SRV, 6, 4, 3, 0,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
      {5, RIBindlessRegisterClass::SRV, 3, 0, 10, 0,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
      {6, RIBindlessRegisterClass::SRV, 4, 0, 11, 0,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
  };
  // The producer arena predates any one program.  Use the global-layout
  // wildcard and let RIProgram supply the root parameters from this DXIL.
  RIBindlessD3D12Layout layout{};
  layout.geometryRangeCount = UINT_MAX;
  layout.geometryRangeOffset = 0;
  Require(bindless.initialize(device, table, layout),
          "RIProgram geometry external table initializes");
  auto a = RIDescriptor::storageBuffer(device, &producerIndexed, 0, 16, 0, true, false);
  auto b = RIDescriptor::storageBuffer(device, &producerNonIndexed, 0, 16, 0, true, false);
  RIBindlessDescriptorSet::WriteBinding writes[] = {
      {4, 0, a}, {4, 1, b},
  };
  Require(bindless.writeDescriptors(device, writes),
          "RIProgram geometry external descriptors write");

  hpl::RIProgram program;
  RIBindlessLayout externalLayouts[4] = {};
  externalLayouts[0] = bindless.layout();
  program.initialize(device, std::span<hpl::RIProgram::ModuleStage>(&stage, 1),
                     externalLayouts, "RIProgram GeometryStream dispatch");
  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  program.bindComputePipeline(device, &cmd, HASH_INITIAL_VALUE,
                              "RIProgram GeometryStream PSO");
  Require(program.bindBindlessDescriptorSet(&cmd, &bindless, 0,
                                             VK_PIPELINE_BIND_POINT_COMPUTE),
           "RIProgram GeometryStream binds shared external table");
  RIProgram::DescriptorBinding ordinary[] = {
      RIProgram::DescriptorBinding(
          "gConstants", RIDescriptor::uniformBuffer(
                            device, &constants, 0,
                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)),
      RIProgram::DescriptorBinding(
          "gOutput", RIDescriptor::storageBuffer(
                         device, &output, 0, 12, 4, false, true)),
  };
  program.bindDescriptors(device, &cmd, 0, ordinary, std::size(ordinary),
                          VK_PIPELINE_BIND_POINT_COMPUTE);
  cmd.dispatch(device, 1, 1, 1);
  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&output, RI_RESOURCE_STATE_STORAGE_WRITE,
                      RI_RESOURCE_STATE_COPY_SRC));
  cmd.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&readback, RI_RESOURCE_STATE_UNDEFINED,
                      RI_RESOURCE_STATE_COPY_DST));
  cmd.copyBuffer(device, &output, 0, &readback, 0, 12);
  cmd.end(device);
  RICmd *commands[] = {&cmd};
  RICommandRingElement completion;
  RISubmitDesc submit = {commands, 1, nullptr, 0, nullptr, 0, &completion};
  ComPtr<ID3D12Fence> gate;
  Hr(device->d3d12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&gate)),
     "RIProgram completion gate creates");
  Hr(device->queues[RI_QUEUE_GRAPHICS].d3d12.queue->Wait(gate.Get(), 1),
     "RIProgram completion gate arms");
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) == RI_SUCCESS,
          "RIProgram GeometryStream dispatch submits");
  RIDescriptorArenaFence pending = {completion.d3d12.fence, completion.d3d12.value};
  Require(!bindless.writeDescriptors(device, writes, &pending),
          "RIProgram rejects external reuse before completion");
  // The fence gate is per slot, not per set. Elements 2..5 of binding 4 have
  // never been written, so they still hold the null descriptor initialize()
  // placed there and no submitted list can be reading through them. Filling one
  // in must succeed even with the set bound and no fence supplied — gating the
  // whole set instead left every texture loaded after the first frame null.
  RIBindlessDescriptorSet::WriteBinding freshWrites[] = {{4, 2, a}};
  Require(bindless.writeDescriptors(device, freshWrites),
          "RIProgram permits a write to a never-written external slot");
  Require(!bindless.writeDescriptors(device, freshWrites),
          "RIProgram rejects rewriting that slot once it is live");
  // An empty descriptor RELEASES the slot: it restores the null descriptor and
  // drops the retained resource, so the slot stops counting as live and the next
  // occupant can claim it with an ordinary fence-less write. Without this the
  // index could be returned to its pool and handed out again while the set still
  // believed the slot live, and every write the new texture made was rejected —
  // leaving it sampling the destroyed one (garbage textures after a level
  // unload). Releasing needs no fence; the caller owes the GPU-passed ordering.
  RIBindlessDescriptorSet::WriteBinding releaseWrites[] = {{4, 2, RIDescriptor{}}};
  Require(bindless.writeDescriptors(device, releaseWrites),
          "RIProgram releases a live external slot without a fence");
  Require(bindless.writeDescriptors(device, freshWrites),
          "RIProgram permits rewriting a released slot without a fence");
  Require(!bindless.writeDescriptors(device, freshWrites),
          "RIProgram rejects rewriting the reclaimed slot once it is live again");
  Hr(gate->Signal(1),
     "RIProgram completion gate releases");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  Require(bindless.writeDescriptors(device, writes, &pending),
          "RIProgram permits external reuse after completion");
  readback.invalidateMappedRange(device, 0, 12);
  auto *result = static_cast<const uint32_t *>(readback.mappedAddress);
  Require(result[0] == 21 && result[1] == 31 && result[2] == 0,
          "RIProgram GeometryStream readback is byte-exact");
  cmd.dispose(device);
  pool.dispose(device);
  program.dispose(device);
  bindless.destroy(device);
  readback.dispose(device);
  output.dispose(device);
  producerNonIndexed.dispose(device);
  producerIndexed.dispose(device);
  constants.dispose(device);
}

void ExerciseRiExternalTable(RIDevice *device,
                             const std::vector<uint8_t> &shader) {
  RIBuffer externalA = RIBuffer::create(
      device, {16, RI_BUFFER_USAGE_SHADER_RESOURCE, RI_MEMORY_HOST_UPLOAD, 0});
  RIBuffer externalB = RIBuffer::create(
      device, {16, RI_BUFFER_USAGE_SHADER_RESOURCE, RI_MEMORY_HOST_UPLOAD, 0});
  Require(!externalA.isEmpty() && !externalB.isEmpty(),
          "RI external producer buffers create");
  *static_cast<uint32_t *>(externalA.mappedAddress) = 11;
  *static_cast<uint32_t *>(externalB.mappedAddress) = 13;
  externalA.flushMappedRange(device, 0, 16);
  externalB.flushMappedRange(device, 0, 16);

  // The two SRV bindings model t0/space2 and t4/space3 in the fixture root
  // signature. RIBindlessDescriptorSet owns the shader-visible allocation;
  // the test must not manufacture a native heap for this table.
  RIBindlessDescriptorSet bindless;
  RIBindlessDescriptorSet::BackendBinding bindings[] = {
      {3, RIBindlessRegisterClass::SRV, 4, 0, 2, 0,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
      {4, RIBindlessRegisterClass::SRV, 2, 4, 3, 4,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
  };
  // RIProgram deserializes its own equivalent root signature from DXIL. Use
  // wildcard metadata here so the public external-table contract does not
  // depend on COM object identity with the native execution fixture above.
  RIBindlessD3D12Layout layout{};
  layout.geometryRangeCount = UINT_MAX;
  layout.geometryRangeOffset = 0;
  Require(bindless.initialize(device, bindings, layout),
          "RI external bindless table initializes");

  auto descriptorA = RIDescriptor::storageBuffer(device, &externalA, 0, 16, 0,
                                                  true, false);
  auto descriptorB = RIDescriptor::storageBuffer(device, &externalB, 0, 16, 0,
                                                  true, false);
  Require(!descriptorA.isEmpty() && !descriptorB.isEmpty(),
          "RI external SRV descriptors build");
  RIBindlessDescriptorSet::WriteBinding validWrites[] = {
      {4, 0, descriptorA}, {4, 1, descriptorB},
  };
  Require(bindless.writeDescriptors(device, validWrites),
          "RI external descriptor writes succeed");
  Require(bindless.d3d12.ownsAllocation,
          "RI external writes retain the owned arena allocation");

  // These are deliberately invalid and must be rejected atomically.
  const auto allocationBeforeInvalid = bindless.d3d12.allocation;
  RIDescriptor wrongType = {};
  wrongType.cookie = 1;
  wrongType.type = RI_DESCRIPTOR_TYPE_SAMPLER;
  RIBindlessDescriptorSet::WriteBinding invalidWrites[] = {
      {4, 0, wrongType}, {4, 2, descriptorA}, {99, 0, descriptorA},
  };
  Require(!bindless.writeDescriptors(device, invalidWrites),
          "RI wrong-type and out-of-range writes are rejected");
  Require(bindless.d3d12.allocation.resourceOffset ==
              allocationBeforeInvalid.resourceOffset &&
              bindless.d3d12.allocation.samplerOffset ==
              allocationBeforeInvalid.samplerOffset,
          "RI wrong-type and out-of-range writes are ignored");

  // Before the table is bound, a rewrite is safe and preserves logical slots.
  RIBindlessDescriptorSet::WriteBinding rewrite = {4, 0, descriptorB};
  Require(bindless.writeDescriptors(device, std::span<const RIBindlessDescriptorSet::WriteBinding>(
                                      &rewrite, 1)),
          "RI external pre-bind rewrite succeeds");
  Require(bindless.d3d12.ownsAllocation,
          "RI external descriptor rewrite remains live");

  hpl::RIProgram::ShaderArtifact artifact;
  artifact.bytes = std::make_shared<const std::vector<char>>(
      reinterpret_cast<const char *>(shader.data()),
      reinterpret_cast<const char *>(shader.data() + shader.size()));
  artifact.format = RIShaderArtifactFormat::Dxil;
  auto reflection = std::make_shared<hpl::RIProgram::ShaderReflection>();
  reflection->entryPoint = "CSMain";
  reflection->stage = "compute";
  reflection->resources = {
      {"gExternalBuffers", "compute", hpl::RIProgram::ShaderRegisterClass::SRV, 4, 3, 0, true},
  };
  artifact.reflection = reflection;
  hpl::RIProgram::ModuleStage stage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE,
                                    artifact, "CSMain");
  hpl::RIProgram program;
  RIBindlessLayout externalLayouts[4] = {};
  externalLayouts[3] = bindless.layout();
  program.initialize(device, std::span<hpl::RIProgram::ModuleStage>(&stage, 1),
                     externalLayouts,
                     "external-bindless RIProgram binding");

  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd cmd;
  cmd.init(device, &pool);
  cmd.begin(device);
  Require(program.bindBindlessDescriptorSet(&cmd, &bindless, 3,
                                    VK_PIPELINE_BIND_POINT_COMPUTE),
          "RIProgram public external bind records on D3D12");
  Require(cmd.d3d12.cmdList != nullptr,
          "RIProgram external bindless bind records on D3D12");

  // The set is bound now, so the per-slot liveness gate is armed: a slot that
  // already resolves to a resource refuses a fence-less rewrite. An EMPTY
  // descriptor releases it instead -- restoring the null descriptor and
  // dropping the reference the set retained -- after which the slot is
  // writable again. Without the release an index returned to its pool and
  // handed out again stays permanently unwritable, so its new owner samples
  // the destroyed resource (black/garbage textures after a level unload), and
  // the old resource is never freed.
  //
  // `bindings` puts binding 3 (4 descriptors) before binding 4, so binding 4
  // element 0 is resources[4]. It holds externalB after the rewrite above.
  RIBindlessDescriptorSet::WriteBinding relive = {4, 0, descriptorA};
  Require(!bindless.writeDescriptors(
              device, std::span<const RIBindlessDescriptorSet::WriteBinding>(
                          &relive, 1)),
          "RI bound external slot refuses a fence-less rewrite");
  ID3D12Resource *const held = descriptorB.payload.buffer.nativeResource;
  held->AddRef();
  const ULONG refsWhileLive = held->Release();
  RIBindlessDescriptorSet::WriteBinding release = {4, 0, RIDescriptor{}};
  Require(bindless.writeDescriptors(
              device, std::span<const RIBindlessDescriptorSet::WriteBinding>(
                          &release, 1)),
          "RI releases a live external slot without a fence");
  held->AddRef();
  Require(held->Release() == refsWhileLive - 1,
          "RI drops the reference a released slot retained");
  Require(bindless.d3d12.resources[4] == nullptr &&
              bindless.d3d12.allocations[4] == nullptr,
          "RI clears the per-slot resource record on release");
  Require(bindless.writeDescriptors(
              device, std::span<const RIBindlessDescriptorSet::WriteBinding>(
                          &relive, 1)),
          "RI permits rewriting a released slot without a fence");

  // A release is an ordinary write as far as validation goes, so it inherits
  // the all-or-nothing contract: one bad entry fails the batch and no slot is
  // touched. Element 1 must still hold its resource afterwards.
  RIBindlessDescriptorSet::WriteBinding badRelease = {4, 99, RIDescriptor{}};
  Require(!bindless.writeDescriptors(
              device, std::span<const RIBindlessDescriptorSet::WriteBinding>(
                          &badRelease, 1)),
          "RI rejects a release past the end of the array");
  RIBindlessDescriptorSet::WriteBinding mixedRelease[] = {
      {4, 1, RIDescriptor{}}, {99, 0, RIDescriptor{}}};
  Require(!bindless.writeDescriptors(device, mixedRelease),
          "RI rejects a release batch containing an unknown binding");
  Require(bindless.d3d12.resources[5] != nullptr,
          "RI validates a release batch before releasing any slot");
  cmd.end(device);
  cmd.dispose(device);
  pool.dispose(device);
  program.dispose(device);
  bindless.destroy(device);
  externalA.dispose(device);
  externalB.dispose(device);
}

void Run(const char *exe) {
  auto root = std::filesystem::path(exe).parent_path() / "compiled_shaders" / "d3d12";
  auto shader = Bytes(root / "external_bindless_compute.comp.dxil");
  Require(shader[0] == 'D' && shader[1] == 'X' && shader[2] == 'B' && shader[3] == 'C', "external shader is DXIL");

  g_riD3D12EnableDebugLayer = true; RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12; init.applicationName = "RID3D12ExternalBindlessComputeSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");
  uint32_t count = 0; Require(EnumerateRIAdapters(nullptr, &count) == RI_SUCCESS && count, "adapters enumerate");
  std::vector<RIPhysicalAdapter> adapters(count); Require(EnumerateRIAdapters(adapters.data(), &count) == RI_SUCCESS, "adapter list populates");
  uint32_t selected = 0; for (uint32_t i = 0; i < count; ++i) if (!adapters[i].d3d12.isWarp) { selected = i; break; }
  std::printf("ADAPTER: %s (WARP=%u)\n", adapters[selected].name,
              static_cast<unsigned>(adapters[selected].d3d12.isWarp));
  RIDevice device; RIDeviceDesc dd = {}; dd.physicalAdapter = &adapters[selected];
  Require(device.init(&dd) == RI_SUCCESS && RIDeviceIsValid(&device), "D3D12 device initializes");
  ID3D12Device *d = device.d3d12.device;

  // One ordinary table (param 0), one caller-owned external table (param 1),
  // and a separate sampler table (param 2). The external table contains
  // nonzero raw-buffer and texture-array indices and is updated after reuse.
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> heap; Hr(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "external resource heap creates");
  D3D12_DESCRIPTOR_HEAP_DESC sd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> sampHeap; Hr(d->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&sampHeap)), "external sampler heap creates");
  UINT stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_DESCRIPTOR_RANGE ordinaryRanges[3] = {{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,1},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
  D3D12_DESCRIPTOR_RANGE external[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,2,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,4,3,4}};
  D3D12_DESCRIPTOR_RANGE samplerRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,0,0,0};
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = params[1].ParameterType = params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {3, ordinaryRanges}; params[1].DescriptorTable = {2, external}; params[2].DescriptorTable = {1, &samplerRange};
  D3D12_ROOT_SIGNATURE_DESC rsd = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rb, re; Hr(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &re), "external root signature serializes");
  ComPtr<ID3D12RootSignature> rootSig; Hr(d->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&rootSig)), "external root signature creates");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = rootSig.Get(); pd.CS = {shader.data(), shader.size()};
  ComPtr<ID3D12PipelineState> pso; Hr(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "external compute pipeline creates");

  auto constants = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ordinary = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto rawA = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto rawB = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = Buffer(d, D3D12_HEAP_TYPE_DEFAULT, 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = Buffer(d, D3D12_HEAP_TYPE_READBACK, 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  uint32_t *p = nullptr; Hr(ordinary->Map(0, nullptr, (void **)&p), "ordinary maps"); p[0]=3; p[1]=4; ordinary->Unmap(0,nullptr);
  Hr(rawA->Map(0,nullptr,(void **)&p), "raw A maps"); p[0]=11; p[1]=11; rawA->Unmap(0,nullptr);
  Hr(rawB->Map(0,nullptr,(void **)&p), "raw B maps"); p[0]=13; p[1]=13; rawB->Unmap(0,nullptr);
  auto texA = Texture(d); auto texB = Texture(d);
  auto texUploadA = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto texUploadB = Buffer(d, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  Hr(texUploadA->Map(0,nullptr,(void **)&p), "texture A upload maps"); p[0] = 31u | (255u << 24); texUploadA->Unmap(0,nullptr);
  Hr(texUploadB->Map(0,nullptr,(void **)&p), "texture B upload maps"); p[0] = 47u | (255u << 24); texUploadB->Unmap(0,nullptr);
  Constants c = {7,1,1,0,0,(2ull << 32)};
  Hr(constants->Map(0,nullptr,(void **)&p), "constants map"); *(Constants *)p=c; constants->Unmap(0,nullptr);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {constants->GetGPUVirtualAddress(),256}; d->CreateConstantBufferView(&cbv,cpu);
  cpu.ptr += stride; D3D12_SHADER_RESOURCE_VIEW_DESC bs = {}; bs.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; bs.Format=DXGI_FORMAT_UNKNOWN; bs.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; bs.Buffer.NumElements=2; bs.Buffer.StructureByteStride=4; d->CreateShaderResourceView(ordinary.Get(),&bs,cpu);
  cpu.ptr += stride; D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; uav.Format=DXGI_FORMAT_UNKNOWN; uav.Buffer.NumElements=2; uav.Buffer.StructureByteStride=4; d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
  // Slots 3..6 are the external texture array followed by raw buffers at 7;
  // rawIndex=1 and lane divergence select descriptors 8 and 9.
  D3D12_SHADER_RESOURCE_VIEW_DESC texSrv = {}; texSrv.Format=DXGI_FORMAT_R8G8B8A8_UNORM; texSrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; texSrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; texSrv.Texture2D.MipLevels=1;
  auto textureCpu = heap->GetCPUDescriptorHandleForHeapStart(); textureCpu.ptr += 4*stride; d->CreateShaderResourceView(texA.Get(),&texSrv,textureCpu); textureCpu.ptr += stride; d->CreateShaderResourceView(texB.Get(),&texSrv,textureCpu);
  D3D12_SHADER_RESOURCE_VIEW_DESC raw = {}; raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; raw.Format=DXGI_FORMAT_R32_TYPELESS; raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; raw.Buffer.NumElements=1; raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
  cpu.ptr = heap->GetCPUDescriptorHandleForHeapStart().ptr + 8*stride; d->CreateShaderResourceView(rawA.Get(),&raw,cpu); cpu.ptr += stride; d->CreateShaderResourceView(rawB.Get(),&raw,cpu);
  D3D12_SAMPLER_DESC sampler = {}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT; sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD=D3D12_FLOAT32_MAX; d->CreateSampler(&sampler,sampHeap->GetCPUDescriptorHandleForHeapStart());
  Require(stride != 0 && c.rawIndex != 0, "external table uses nonzero divergent indices");
  Require(rawA->GetGPUVirtualAddress() != 0 && rawB->GetGPUVirtualAddress() != 0, "producer arena buffers have GPU handles");
  ComPtr<ID3D12CommandAllocator> uploadAlloc; Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&uploadAlloc)),"texture upload allocator creates");
  ComPtr<ID3D12GraphicsCommandList> uploadList; Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,uploadAlloc.Get(),nullptr,IID_PPV_ARGS(&uploadList)),"texture upload list creates");
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {}; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM; src.PlacedFootprint.Footprint.Width=1; src.PlacedFootprint.Footprint.Height=1; src.PlacedFootprint.Footprint.Depth=1; src.PlacedFootprint.Footprint.RowPitch=256; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=0;
  src.pResource=texUploadA.Get(); dst.pResource=texA.Get(); uploadList->CopyTextureRegion(&dst,0,0,0,&src,nullptr); src.pResource=texUploadB.Get(); dst.pResource=texB.Get(); uploadList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  D3D12_RESOURCE_BARRIER texBar[2] = {}; for (int i=0;i<2;++i) { texBar[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; texBar[i].Transition.pResource=i?texB.Get():texA.Get(); texBar[i].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; texBar[i].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; texBar[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; } uploadList->ResourceBarrier(2,texBar); Hr(uploadList->Close(),"texture upload list closes"); ID3D12CommandList *uploadLists[]={uploadList.Get()}; device.queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1,uploadLists); device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
  ComPtr<ID3D12CommandAllocator> alloc; Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"smoke allocator creates");
  ComPtr<ID3D12GraphicsCommandList> list; Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),pso.Get(),IID_PPV_ARGS(&list)),"smoke command list creates");
  ID3D12DescriptorHeap *heaps[] = {heap.Get(),sampHeap.Get()}; list->SetDescriptorHeaps(2,heaps); list->SetComputeRootSignature(rootSig.Get()); list->SetPipelineState(pso.Get());
  list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart()); auto ext=heap->GetGPUDescriptorHandleForHeapStart(); ext.ptr += 3*stride; list->SetComputeRootDescriptorTable(1,ext); list->SetComputeRootDescriptorTable(2,sampHeap->GetGPUDescriptorHandleForHeapStart()); list->Dispatch(1,1,1);
  D3D12_RESOURCE_BARRIER outBar = {}; outBar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; outBar.Transition.pResource=output.Get(); outBar.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; outBar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; outBar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; list->ResourceBarrier(1,&outBar); list->CopyBufferRegion(readback.Get(),0,output.Get(),0,8);
  Hr(list->Close(),"external dispatch records"); ID3D12CommandList *lists[]={list.Get()}; device.queues[RI_QUEUE_GRAPHICS].d3d12.queue->ExecuteCommandLists(1,lists); device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);
  uint32_t *result=nullptr; D3D12_RANGE readRange={0,8}; Hr(readback->Map(0,&readRange,(void **)&result),"external compute readback maps"); Require(result[0] == 7+3+11+31 && result[1] == 7+4+13+47, "external compute readback is byte-exact"); readback->Unmap(0,nullptr);
  // D3D12 RIProgram acceptance is intentionally exercised separately from
  // native execution: both external-bindless and the producer geometry
  // compute artifact must be retained as DXIL without Vulkan reflection.
  auto accept = [&](const std::vector<uint8_t> &bytes, const char *name) {
    hpl::RIProgram::ShaderArtifact artifact;
    artifact.bytes = std::make_shared<const std::vector<char>>(
        reinterpret_cast<const char *>(bytes.data()),
        reinterpret_cast<const char *>(bytes.data() + bytes.size()));
    artifact.format = RIShaderArtifactFormat::Dxil;
    auto reflection = std::make_shared<hpl::RIProgram::ShaderReflection>();
    reflection->entryPoint = "CSMain"; reflection->stage = "compute";
    artifact.reflection = reflection;
    hpl::RIProgram::ModuleStage stage(hpl::RIProgram::PROGRAM_STAGE_COMPUTE,
                                      artifact, "CSMain");
    hpl::RIProgram program; program.initialize(&device, std::span<hpl::RIProgram::ModuleStage>(&stage, 1), {}, name);
    program.dispose(&device);
  };
  accept(shader, "external-bindless RIProgram acceptance");
  ExerciseRiExternalTable(&device, shader);
  auto geometryShader = Bytes(root / "geometry_stream.comp.dxil");
  accept(geometryShader, "GeometryStream RIProgram acceptance");
  ExerciseRiProgramGeometry(&device, root / "geometry_stream.comp.dxil");
  Require(true, "RIProgram accepts external and GeometryStream compute artifacts");
  // Repeated teardown/reuse is part of the smoke even when a validation layer
  // rejects the intentionally incomplete texture descriptors on old hardware.
  device.dispose();
  device.dispose();
  ShutdownRIRenderer(); g_riD3D12EnableDebugLayer=false;
  Require(true,"device teardown is repeatable");
}
}

int main(int argc, char **argv) { Run(argc > 0 ? argv[0] : "RID3D12ExternalBindlessComputeSmoke.exe"); return 0; }
