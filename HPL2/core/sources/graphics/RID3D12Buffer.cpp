#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RIBuffer.h"
#include "graphics/RIDevice.h"
#include "graphics/RIDescriptorSetAllocator.h"
#include "system/LowLevelSystem.h"
#include <D3D12MemAlloc.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// Registration is deliberately device-local and bounded by the fixed
// geometry namespace.  A retired entry is kept until device teardown: the
// public buffer-dispose API does not identify the submission fence that last
// used the buffer, so neither the descriptor nor the native allocation can
// safely be recycled at dispose time.
struct RID3D12BufferRegistration {
  RIDevice *device;
  hash_t cookie;
  ID3D12Resource *resource;
  D3D12MA::Allocation *allocation;
  RIDescriptorArenaAllocation descriptor;
  uint32_t owners;
  bool retired;
};

static std::vector<RID3D12BufferRegistration> g_bufferRegistrations;

static RID3D12BufferRegistration *ri_d3d12_find_buffer_registration(
    RIDevice &device, const RIBuffer &buffer, bool includeRetired = false) {
  for (RID3D12BufferRegistration &entry : g_bufferRegistrations) {
    if (entry.device != &device || entry.resource != buffer.d3d12.resource ||
        entry.cookie != buffer.cookie || (!includeRetired && entry.retired))
      continue;
    return &entry;
  }
  return nullptr;
}

static void ri_d3d12_release_registration_refs(
    RID3D12BufferRegistration &entry) {
  if (entry.resource)
    entry.resource->Release();
  if (entry.allocation)
    entry.allocation->Release();
  entry.resource = nullptr;
  entry.allocation = nullptr;
}

static bool ri_d3d12_track_buffer_resource(RIDevice &device, RIBuffer &buffer) {
  RID3D12BufferRegistration *existing =
      ri_d3d12_find_buffer_registration(device, buffer);
  if (existing) {
    ++existing->owners;
    return true;
  }
  if (g_bufferRegistrations.size() >= RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY)
    return false;

  // The registry keeps the resource/allocation alive after the caller's
  // RIBuffer releases its own references.  This is the quarantine proof for
  // submissions whose completion fence is not available at disposal time.
  if (buffer.d3d12.resource)
    buffer.d3d12.resource->AddRef();
  if (buffer.d3d12.allocation)
    buffer.d3d12.allocation->AddRef();
  g_bufferRegistrations.push_back(
      {&device, buffer.cookie, buffer.d3d12.resource, buffer.d3d12.allocation,
       {}, 1, false});
  return true;
}

static bool ri_d3d12_set_buffer_descriptor(
    RIDevice &device, RIBuffer &buffer,
    const RIDescriptorArenaAllocation &descriptor) {
  RID3D12BufferRegistration *entry =
      ri_d3d12_find_buffer_registration(device, buffer);
  if (!entry || entry->retired)
    return false;
  if (entry->descriptor.resourceCount)
    return entry->descriptor.resourceOffset == descriptor.resourceOffset;
  entry->descriptor = descriptor;
  return true;
}

static void ri_d3d12_retire_buffer_registration(RIDevice &device,
                                                 RIBuffer &buffer) {
  RID3D12BufferRegistration *entry =
      ri_d3d12_find_buffer_registration(device, buffer);
  if (!entry || entry->retired || !entry->owners)
    return;
  if (--entry->owners)
    return;

  // Geometry descriptor slots are intentionally monotonic and are not
  // reused.  Release through the arena's geometry path so live ownership is
  // removed, while the registry-held native references remain quarantined.
  releaseGeometryDescriptorArena(&device, &entry->descriptor);
  entry->retired = true;
}

} // namespace

// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT is the alignment of a resource
// within a heap, not a guarantee about the GPU VA returned for an allocated
// buffer.  Ordinary buffer GPU addresses are only guaranteed to meet the
// buffer-use alignment required by CBVs and ray-tracing scratch buffers.
static constexpr uint64_t kRiD3D12BufferGpuVaAlignment =
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(kRiD3D12BufferGpuVaAlignment ==
                  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
              "D3D12 buffer GPU-VA alignment contract must cover CBV and AS scratch");

static bool ri_d3d12_buffer_range_is_valid(const RIBuffer &buffer,
                                           uint64_t offset, uint64_t size) {
  if (size == 0)
    return true;
  if (offset > buffer.d3d12.requestedSize ||
      size > buffer.d3d12.requestedSize - offset) {
    hpl::Warning("RI D3D12: buffer range (%llu, %llu) exceeds requested size %llu\n",
                 (unsigned long long)offset, (unsigned long long)size,
                 (unsigned long long)buffer.d3d12.requestedSize);
    return false;
  }
  return true;
}

static void ri_d3d12_release_buffer(RIBuffer &buffer) {
  if (buffer.mappedAddress && buffer.d3d12.resource)
    buffer.d3d12.resource->Unmap(0, nullptr);
  buffer.mappedAddress = nullptr;

  // CreateResource returns an explicit resource reference in addition to the
  // allocation reference. Release the resource first, but tolerate buffers
  // created by older code that only retained the raw resource.
  if (buffer.d3d12.resource)
    buffer.d3d12.resource->Release();
  if (buffer.d3d12.allocation)
    buffer.d3d12.allocation->Release();
  buffer = RIBuffer{};
}

int RID3D12_CreateBuffer(struct RIDevice &device, const struct RIBufferDesc &desc,
                         struct RIBuffer &out) {
  const hash_t cookie = out.cookie;
  out = RIBuffer{};
  out.cookie = cookie;

  const uint32_t forbiddenHostUsage =
      RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
      RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE | RI_BUFFER_USAGE_SCRATCH;
  if (desc.size == 0) {
    hpl::Warning("RI D3D12: buffer size must be non-zero\n");
    return RI_FAIL;
  }
  if (desc.alignment != 0 &&
      ((desc.alignment & (desc.alignment - 1)) != 0 ||
      desc.alignment > kRiD3D12BufferGpuVaAlignment)) {
    hpl::Warning(
        "RI D3D12: buffer alignment must be a power of two no larger than "
        "the D3D12MA buffer GPU-VA alignment contract (256 bytes)\n");
    return RI_FAIL;
  }
  if (desc.location == RI_MEMORY_HOST_UPLOAD &&
      (desc.usage & forbiddenHostUsage)) {
    hpl::Warning("RI D3D12: upload buffers cannot use storage, acceleration structure, or scratch usage\n");
    return RI_FAIL;
  }
  // DEVICE_ADDRESS is published as the same raw ByteAddressBuffer SRV on
  // D3D12.  Raw views address DWORDs, so the complete registered resource
  // must satisfy the raw-view alignment/range contract as well.
  if ((desc.usage & (RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                     RI_BUFFER_USAGE_DEVICE_ADDRESS)) &&
      (desc.size < 4 || (desc.size % 4) != 0)) {
    hpl::Warning("RI D3D12: raw geometry buffers must be a multiple of 4 bytes\n");
    return RI_FAIL;
  }
  if ((desc.usage & (RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                     RI_BUFFER_USAGE_DEVICE_ADDRESS)) &&
      desc.size / 4ull > UINT_MAX) {
    hpl::Warning("RI D3D12: raw geometry buffer is too large for a D3D12 SRV\n");
    return RI_FAIL;
  }
  if ((desc.usage & RI_BUFFER_USAGE_CONSTANT_BUFFER) &&
      desc.size > UINT64_MAX - 255ull) {
    hpl::Warning("RI D3D12: constant buffer size overflows 256-byte alignment\n");
    return RI_FAIL;
  }
  if (desc.location == RI_MEMORY_HOST_READBACK &&
      ((desc.usage & forbiddenHostUsage) ||
       (desc.usage & ~(RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST)))) {
    hpl::Warning("RI D3D12: readback buffers may only use transfer source/destination usage\n");
    return RI_FAIL;
  }
  if (desc.location != RI_MEMORY_DEVICE &&
      (desc.usage & RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE)) {
    hpl::Warning("RI D3D12: acceleration-structure storage requires device memory\n");
    return RI_FAIL;
  }
  if (!device.d3d12.device) {
    hpl::Warning("RI D3D12: cannot create buffer without a device\n");
    return RI_FAIL;
  }
  if (!device.d3d12.allocator) {
    hpl::Warning("RI D3D12: cannot create buffer without a memory allocator\n");
    return RI_FAIL;
  }
  if (desc.location != RI_MEMORY_DEVICE &&
      desc.location != RI_MEMORY_HOST_UPLOAD &&
      desc.location != RI_MEMORY_HOST_READBACK) {
    hpl::Warning("RI D3D12: invalid buffer memory location\n");
    return RI_FAIL;
  }

  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  // D3D12 buffers use their natural resource alignment when this is
  // zero. The RIBufferDesc alignment is a GPU-address postcondition, not a
  // request to put an unsupported value into this resource descriptor.
  rd.Alignment = 0;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc = {1, 0};
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (desc.usage & (RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                    RI_BUFFER_USAGE_SCRATCH |
                    RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE))
    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  rd.Width = desc.size;
  if (desc.usage & RI_BUFFER_USAGE_CONSTANT_BUFFER)
    rd.Width = (rd.Width + 255ull) & ~255ull;

  D3D12_HEAP_PROPERTIES heap = {};
  D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
  if (desc.location == RI_MEMORY_HOST_UPLOAD) {
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
  } else if (desc.location == RI_MEMORY_HOST_READBACK) {
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    initialState = D3D12_RESOURCE_STATE_COPY_DEST;
  } else {
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (desc.usage & RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE)
      initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
  }

  D3D12MA::ALLOCATION_DESC allocationDesc = {};
  allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
  allocationDesc.HeapType = heap.Type;
  allocationDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
  HRESULT hr = device.d3d12.allocator->CreateResource(
      &allocationDesc, &rd, initialState, nullptr, &out.d3d12.allocation,
      IID_PPV_ARGS(&out.d3d12.resource));
  if (!D3D12_WrapResult(hr)) {
    ri_d3d12_release_buffer(out);
    return RI_FAIL;
  }

  if (desc.alignment != 0 &&
      out.d3d12.resource->GetGPUVirtualAddress() % desc.alignment != 0) {
    hpl::Warning(
        "RI D3D12: allocator buffer GPU address does not satisfy requested "
        "alignment of %llu bytes\n",
        (unsigned long long)desc.alignment);
    ri_d3d12_release_buffer(out);
    return RI_FAIL;
  }

  out.d3d12.requestedSize = desc.size;
  out.d3d12.allocationSize = out.d3d12.resource->GetDesc().Width;
  out.d3d12.usage = desc.usage;
  out.d3d12.location = uint8_t(desc.location);
  out.d3d12.shaderResourceIndex = UINT32_MAX;
  out.d3d12.shaderResourceArenaOwned = false;

  if (desc.location == RI_MEMORY_HOST_UPLOAD ||
      desc.location == RI_MEMORY_HOST_READBACK) {
    D3D12_RANGE readRange = {0, 0};
    void *mapped = nullptr;
    hr = out.d3d12.resource->Map(
        0, desc.location == RI_MEMORY_HOST_UPLOAD ? &readRange : nullptr,
        &mapped);
    if (!D3D12_WrapResult(hr)) {
      ri_d3d12_release_buffer(out);
      return RI_FAIL;
    }
    out.mappedAddress = mapped;
  }

  // Register only after every fallible resource initialization step. This
  // keeps failure cleanup local to the resource/allocation pair and avoids
  // leaking a descriptor-arena allocation if persistent mapping fails.
  // DEVICE_ADDRESS is the cross-backend geometry-pull contract: Vulkan uses
  // a BDA while D3D12 publishes the same buffer as a raw ByteAddressBuffer
  // SRV. This also covers host-upload particle scratch rings.
  if (desc.usage & (RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                    RI_BUFFER_USAGE_DEVICE_ADDRESS)) {
    if (!RID3D12_RegisterBufferShaderResource(device, out)) {
      ri_d3d12_retire_buffer_registration(device, out);
      ri_d3d12_release_buffer(out);
      return RI_FAIL;
    }
  }
  return RI_SUCCESS;
}

void RID3D12_DisposeBuffer(struct RIDevice &device, struct RIBuffer &buffer) {
  // No queue-wide wait here.  The registration quarantine retains the native
  // resource/allocation until device teardown, when all queues are idle.
  if (buffer.d3d12.resource || buffer.d3d12.allocation)
    ri_d3d12_retire_buffer_registration(device, buffer);
  if (!buffer.d3d12.resource && !buffer.d3d12.allocation)
    return;
  ri_d3d12_release_buffer(buffer);
}

void RID3D12_DrainBufferRegistry(struct RIDevice &device) {
  for (RID3D12BufferRegistration &entry : g_bufferRegistrations) {
    if (entry.device != &device)
      continue;
    if (!entry.retired)
      releaseGeometryDescriptorArena(&device, &entry.descriptor);
    ri_d3d12_release_registration_refs(entry);
  }
  g_bufferRegistrations.erase(
      std::remove_if(g_bufferRegistrations.begin(), g_bufferRegistrations.end(),
                     [&device](const RID3D12BufferRegistration &entry) {
                       return entry.device == &device;
                     }),
      g_bufferRegistrations.end());
}

void RID3D12_SetBufferDebugName(struct RIDevice &device, struct RIBuffer &buffer,
                                const char *name) {
  (void)device;
  if (!buffer.d3d12.resource || !name)
    return;

  wchar_t wide[256] = {};
  int sourceLength = int(strlen(name));
  int converted = 0;
  while (sourceLength > 0) {
    converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                    sourceLength, wide, 255);
    if (converted > 0)
      break;
    --sourceLength;
  }
  if (converted > 0) {
    wide[converted] = L'\0';
    buffer.d3d12.resource->SetName(wide);
    if (buffer.d3d12.allocation)
      buffer.d3d12.allocation->SetName(wide);
  }
}

uint64_t RID3D12_BufferGpuAddress(struct RIDevice &device,
                                  struct RIBuffer &buffer) {
  (void)device;
  return buffer.d3d12.resource ? buffer.d3d12.resource->GetGPUVirtualAddress() : 0;
}

void RID3D12_SetBufferShaderResourceIndex(struct RIBuffer &buffer,
                                          uint32_t descriptorIndex) {
  if (buffer.d3d12.resource) {
    buffer.d3d12.shaderResourceIndex = descriptorIndex;
    buffer.d3d12.shaderResourceArenaOwned = false;
  }
}

bool RID3D12_RegisterBufferShaderResource(struct RIDevice &device,
                                          struct RIBuffer &buffer) {
  if (!device.d3d12.device || !buffer.d3d12.resource ||
      buffer.d3d12.shaderResourceIndex != UINT32_MAX)
    return false;

  // Track only buffers that enter the geometry SRV namespace. Ordinary
  // buffers have no published packed handle and can retain their normal
  // disposal behavior instead of being quarantined until device teardown.
  if (!ri_d3d12_find_buffer_registration(device, buffer) &&
      !ri_d3d12_track_buffer_resource(device, buffer))
    return false;

  // Reuse the descriptor for an
  // identical cookie/native-resource pair without allocating or overwriting
  // another live heap slot.
  if (RID3D12BufferRegistration *existing =
          ri_d3d12_find_buffer_registration(device, buffer)) {
    if (existing->descriptor.resourceCount) {
      buffer.d3d12.shaderResourceIndex =
          existing->descriptor.resourceOffset;
      buffer.d3d12.shaderResourceArenaOwned = true;
      return true;
    }
  }

  RIDescriptorArenaAllocation allocation = {};
  if (!allocateGeometryDescriptorArena(&device, 1, &allocation))
    return false;
  const uint32_t index = allocation.resourceOffset;
  if (index >= RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY) {
    releaseGeometryDescriptorArena(&device, &allocation);
    return false;
  }
  device.d3d12.nextGeometrySrvIndex =
      std::max(device.d3d12.nextGeometrySrvIndex, index + 1);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R32_TYPELESS;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Buffer.FirstElement = 0;
  srv.Buffer.NumElements = UINT(buffer.d3d12.requestedSize / 4u);
  srv.Buffer.StructureByteStride = 0;
  srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  ID3D12DescriptorHeap *resourceHeap = nullptr;
  if (!getDescriptorArenaHeaps(&device, &resourceHeap, nullptr)) {
    releaseGeometryDescriptorArena(&device, &allocation);
    return false;
  }
  const uint32_t stride = device.d3d12.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE dst = resourceHeap->GetCPUDescriptorHandleForHeapStart();
  dst.ptr += SIZE_T(index) * stride;
  // Do not publish the heap index until the view exists.  A failed view
  // creation would otherwise leave a non-zero packed geometry handle that
  // resolves to an unrelated/stale descriptor in the shader-visible heap.
  // (CreateShaderResourceView is void, so the descriptor itself is the only
  // observable result; validate the inputs that can make this operation
  // invalid before publishing the index.)
  if (srv.Buffer.NumElements == 0 ||
      uint64_t(srv.Buffer.NumElements) * 4ull > buffer.d3d12.requestedSize) {
    releaseGeometryDescriptorArena(&device, &allocation);
    return false;
  }
  device.d3d12.device->CreateShaderResourceView(buffer.d3d12.resource, &srv,
                                                 dst);
  if (!ri_d3d12_set_buffer_descriptor(device, buffer, allocation)) {
    releaseGeometryDescriptorArena(&device, &allocation);
    return false;
  }
  buffer.d3d12.shaderResourceIndex = index;
  buffer.d3d12.shaderResourceArenaOwned = true;
  return true;
}

void RID3D12_FlushBufferRange(struct RIDevice &device, struct RIBuffer &buffer,
                              uint64_t offset, uint64_t size) {
  (void)device;
  if (!buffer.d3d12.resource || !buffer.mappedAddress)
    return;
  ri_d3d12_buffer_range_is_valid(buffer, offset, size);
}

void RID3D12_InvalidateBufferRange(struct RIDevice &device,
                                   struct RIBuffer &buffer, uint64_t offset,
                                   uint64_t size) {
  (void)device;
  if (!buffer.d3d12.resource || !buffer.mappedAddress)
    return;
  ri_d3d12_buffer_range_is_valid(buffer, offset, size);
  if (buffer.d3d12.location == RI_MEMORY_HOST_READBACK) {
    buffer.mappedAddress = nullptr;
    buffer.d3d12.resource->Unmap(0, nullptr);
    void *mapped = nullptr;
    HRESULT hr = buffer.d3d12.resource->Map(0, nullptr, &mapped);
    if (!D3D12_WrapResult(hr)) {
      hpl::Warning("RI D3D12: readback buffer remap failed with HRESULT 0x%08lX\n",
                   static_cast<unsigned long>(hr));
      return;
    }
    buffer.mappedAddress = mapped;
  }
}

bool RID3D12_BufferIsEmpty(const struct RIBuffer &buffer) {
  return buffer.d3d12.resource == nullptr;
}

#endif // DEVICE_IMPL_D3D12
