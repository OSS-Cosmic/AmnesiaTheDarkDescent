// Opt-in D3D12 smoke coverage for the global managed-set resource contract.
#include "graphics/GlobalManagedSets.h"
#include "graphics/RID3D12.h"
#include "graphics/RIDescriptorSetAllocator.h"
#include "graphics/RICommand.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIDevice.h"
#include "graphics/RIRenderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>

int hplMain(const std::string &) { return 0; }

using namespace hpl;

namespace {

[[noreturn]] void Fail(const char *check) {
  std::fprintf(stderr, "FAIL: %s\n", check);
  std::_Exit(1);
}

void Require(bool condition, const char *check) {
  if (!condition) Fail(check);
  std::printf("PASS: %s\n", check);
}

RIBuffer MakeBuffer(RIDevice *device, uint64_t size, uint32_t usage,
                    RIMemoryLocation_e location = RI_MEMORY_DEVICE) {
  return RIBuffer::create(device, {size, usage, location, 0});
}

void Flush(RIDevice *device, RIResourceUploader *uploader) {
  RIResourceUploaderD3D12Result result = RI_D3D12FlushResourceUpdate(device, uploader);
  if (result.signaled) {
    Require(result.timeline != nullptr && result.value != 0,
            "D3D12 upload signals a timeline");
    result.timeline->wait(device, result.value);
  }
}

void Upload(RIDevice *device, RIResourceUploader *uploader, RIBuffer *target,
            const void *data, size_t size, size_t offset = 0) {
  RIResourceBufferTransaction transaction = {};
  transaction.target = *target;
  transaction.size = size;
  transaction.offset = offset;
  transaction.currentState = RI_RESOURCE_STATE_UNDEFINED;
  transaction.postState = RI_RESOURCE_STATE_COPY_SRC;
  transaction.postStages = RI_STAGE_COPY;
  RI_ResourceBeginCopyBuffer(device, uploader, &transaction);
  Require(transaction.mapped.data != nullptr && transaction.mapped.size >= size,
          "upload staging allocation is mapped and large enough");
  std::memcpy(transaction.mapped.data, data, size);
  RI_ResourceEndCopyBuffer(device, uploader, &transaction);
  Flush(device, uploader);
}

void Readback(RIDevice *device, RIBuffer *source, void *out, size_t size) {
  RIBuffer readback = MakeBuffer(device, size, RI_BUFFER_USAGE_TRANSFER_DST,
                                 RI_MEMORY_HOST_READBACK);
  Require(!readback.isEmpty() && readback.mappedAddress != nullptr,
          "readback buffer is created and mapped");

  RIPool pool;
  pool.init(device, &device->queues[RI_QUEUE_GRAPHICS]);
  RICmd command;
  command.init(device, &pool);
  command.begin(device);
  command.vk_d3d12_bufferBarrier(
      RIBufferBarrier(&readback, RI_RESOURCE_STATE_UNDEFINED,
                      RI_RESOURCE_STATE_COPY_DST));
  command.copyBuffer(device, source, 0, &readback, 0, size);
  command.end(device);
  RICmd *commands[] = {&command};
  RISubmitDesc submit = {};
  submit.cmds = commands;
  submit.cmdCount = 1;
  Require(device->queues[RI_QUEUE_GRAPHICS].submit(device, submit) == RI_SUCCESS,
          "readback copy submits");
  device->queues[RI_QUEUE_GRAPHICS].waitIdle(device);
  readback.invalidateMappedRange(device, 0, 0);
  std::memcpy(out, readback.mappedAddress, size);
  command.dispose(device);
  pool.dispose(device);
  readback.dispose(device);
}

void CheckTypedDescriptorTables(RIDevice *device, RIBuffer *buffer) {
  Require(initDescriptorArena(device), "D3D12 descriptor arena initializes");
  RIDescriptorArenaAllocation allocation = {};
  Require(allocateDescriptorArena(device, 2, 1, &allocation),
          "typed resource and sampler table allocation succeeds");

  ID3D12DescriptorHeap *resourceHeap = nullptr;
  ID3D12DescriptorHeap *samplerHeap = nullptr;
  Require(getDescriptorArenaHeaps(device, &resourceHeap, &samplerHeap) &&
              resourceHeap != nullptr && samplerHeap != nullptr,
          "descriptor arena exposes shader-visible heaps");

  const UINT resourceStride = device->d3d12.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE resource = resourceHeap->GetCPUDescriptorHandleForHeapStart();
  resource.ptr += SIZE_T(allocation.resourceOffset) * resourceStride;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_UNKNOWN;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Buffer.NumElements = 64;
  srv.Buffer.StructureByteStride = sizeof(uint32_t);
  device->d3d12.device->CreateShaderResourceView(buffer->d3d12.resource, &srv,
                                                  resource);

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = 64;
  uav.Buffer.StructureByteStride = sizeof(uint32_t);
  D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = resource;
  uavHandle.ptr += resourceStride;
  device->d3d12.device->CreateUnorderedAccessView(buffer->d3d12.resource, nullptr,
                                                   &uav, uavHandle);

  const UINT samplerStride = device->d3d12.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  D3D12_CPU_DESCRIPTOR_HANDLE sampler = samplerHeap->GetCPUDescriptorHandleForHeapStart();
  sampler.ptr += SIZE_T(allocation.samplerOffset) * samplerStride;
  D3D12_SAMPLER_DESC samplerDesc = {};
  samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplerDesc.MinLOD = 0.0f;
  samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
  device->d3d12.device->CreateSampler(&samplerDesc, sampler);
  Require(resource.ptr != 0 && uavHandle.ptr != 0 && sampler.ptr != 0,
          "typed SRV/UAV and sampler descriptors are written");
  releaseDescriptorArena(device, &allocation, nullptr);
}

void RunCycle() {
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12GlobalManagedSetsSmoke";
  g_riD3D12EnableDebugLayer = true;
  Require(InitRIRenderer(&init) == RI_SUCCESS, "D3D12 renderer initializes");

  RIPhysicalAdapter adapters[8] = {};
  uint32_t count = 8;
  Require(EnumerateRIAdapters(adapters, &count) == RI_SUCCESS && count > 0,
          "D3D12 adapter enumeration succeeds");
  uint32_t selected = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (!adapters[i].d3d12.isWarp) { selected = i; break; }

  RIDevice device;
  RIDeviceDesc desc = {};
  desc.physicalAdapter = &adapters[selected];
  Require(device.init(&desc) == RI_SUCCESS && RIDeviceIsValid(&device),
          "D3D12 device initializes");

  const uint32_t usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST |
                         RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE;
  RIBuffer objectBuffer = MakeBuffer(&device, sizeof(UniformObject) * 4, usage);
  RIBuffer materialBuffer = MakeBuffer(&device, sizeof(MaterialDataBlob) * 4, usage);
  RIBuffer generationBuffer = MakeBuffer(&device, sizeof(uint32_t) * 8, usage);
  Require(!objectBuffer.isEmpty() && !materialBuffer.isEmpty() &&
              !generationBuffer.isEmpty() && objectBuffer.mappedAddress == nullptr,
          "global object/material/generation buffers are valid device-local RI buffers");
  RID3D12_RegisterBufferShaderResource(device, objectBuffer);
  CheckTypedDescriptorTables(&device, &objectBuffer);

  RIResourceUploader uploader = {};
  RI_InitResourceUploader(&device, &uploader);
  BindlessShadowMirror mirror;
  mirror.init(8, sizeof(uint32_t));
  mirror.markAllDirty();
  Require(mirror.hasDirty() && std::all_of(mirror.shadow.begin(), mirror.shadow.end(),
                                            [](uint8_t byte) { return byte == 0; }),
          "generation mirror starts zeroed and dirty");
  Upload(&device, &uploader, &generationBuffer, mirror.shadow.data(), mirror.shadow.size());
  std::vector<uint8_t> zeros(mirror.shadow.size(), 0xFF);
  Readback(&device, &generationBuffer, zeros.data(), zeros.size());
  Require(std::all_of(zeros.begin(), zeros.end(), [](uint8_t byte) { return byte == 0; }),
          "zero generation mirror is uploaded before consumption");
  mirror.clearDirty();

  UniformObject object = {};
  object.materialID = 3;
  object.dissolveAmount = 0.25f;
  MaterialDataBlob material = {};
  material.data[0] = 0x4D41544Cu;
  material.data[7] = 0x12345678u;
  Upload(&device, &uploader, &objectBuffer, &object, sizeof(object));
  Upload(&device, &uploader, &materialBuffer, &material, sizeof(material));
  UniformObject objectRead = {};
  MaterialDataBlob materialRead = {};
  Readback(&device, &objectBuffer, &objectRead, sizeof(objectRead));
  Readback(&device, &materialBuffer, &materialRead, sizeof(materialRead));
  Require(std::memcmp(&object, &objectRead, sizeof(object)) == 0,
          "object payload roundtrips byte-exactly");
  Require(std::memcmp(&material, &materialRead, sizeof(material)) == 0,
          "material payload roundtrips byte-exactly");
  RI_FreeResourceUploader(&device, &uploader);

  objectBuffer.dispose(&device);
  materialBuffer.dispose(&device);
  generationBuffer.dispose(&device);
  freeDescriptorArena(&device);
  device.dispose();
  ShutdownRIRenderer();
  g_riD3D12EnableDebugLayer = false;
}

void RunCreationFailureCleanup() {
  RIBackendInit init = {};
  init.api = RI_DEVICE_API_D3D12;
  init.applicationName = "RID3D12GlobalManagedSetsFailureSmoke";
  Require(InitRIRenderer(&init) == RI_SUCCESS, "failure-cycle renderer initializes");
  RIPhysicalAdapter adapter[1] = {};
  uint32_t count = 1;
  Require(EnumerateRIAdapters(adapter, &count) == RI_SUCCESS && count == 1,
          "failure-cycle adapter enumeration succeeds");
  RIDevice device;
  RIDeviceDesc desc = {};
  desc.physicalAdapter = adapter;
  Require(device.init(&desc) == RI_SUCCESS, "failure-cycle device initializes");
  GlobalManagedSets sets;
  sets.m_objectBuffer = MakeBuffer(&device, 4096,
                                   RI_BUFFER_USAGE_TRANSFER_DST | RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE);
  RIBuffer invalid = MakeBuffer(&device, 0, RI_BUFFER_USAGE_TRANSFER_DST);
  Require(invalid.isEmpty(), "invalid zero-sized global buffer creation fails cleanly");
  invalid.dispose(&device);
  sets.destroy(&device);
  sets.destroy(&device);
  Require(sets.m_objectBuffer.isEmpty(), "partial global-set ownership is released after failure");
  device.dispose();
  ShutdownRIRenderer();
}

} // namespace

int main() {
  RunCycle();
  RunCreationFailureCleanup();
  return 0;
}
