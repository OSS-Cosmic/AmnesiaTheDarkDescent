// D3D12 (DXR) acceleration structures, paralleling the Vulkan
// vkCreateAccelerationStructureKHR / vkCmdBuildAccelerationStructuresKHR paths
// in RIRenderer.cpp.
//
// The central difference from Vulkan: D3D12 has no acceleration-structure
// object. An AS *is* a region of a buffer created with
// RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE, which RID3D12Buffer.cpp
// already places in D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE with
// ALLOW_UNORDERED_ACCESS. RIAccelStructure therefore only snapshots that
// region's GPU virtual address and *borrows* the storage buffer's native
// handles for debug naming; it owns nothing and releases nothing.
//
// Barriers: callers already emit ACCEL_WRITE -> ACCEL_READ memory barriers
// around builds (e.g. World.cpp after buildTlas). RID3D12_ResourceBarrier
// lowers a memory barrier to a global UAV barrier on the legacy path and to a
// D3D12_GLOBAL_BARRIER on the enhanced path, which is exactly what DXR
// requires between a build and any read, so these functions emit none of their
// own.
#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RIBuffer.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIDevice.h"
#include "graphics/RIFormat.h"
#include "graphics/RIPipeline.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

#include <D3D12MemAlloc.h>

#include <cassert>
#include <cstring>
#include <vector>

// RIAccelStructureBuildBits_e -> D3D12 build flags. Mirrors RI_VK_AccelBuildFlags
// (RIVK.h). RI_ACCEL_BUILD_ALLOW_DATA_ACCESS has no D3D12 counterpart
// (VK_KHR_acceleration_structure's vertex-data readback is Vulkan-only) and is
// dropped rather than approximated.
static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS
ri_d3d12_AccelBuildFlags(uint32_t flags) {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS out =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  if (flags & RI_ACCEL_BUILD_ALLOW_UPDATE)
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
  if (flags & RI_ACCEL_BUILD_ALLOW_COMPACTION)
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_TRACE)
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_BUILD)
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  if (flags & RI_ACCEL_BUILD_MINIMIZE_MEMORY)
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
  return out;
}

static D3D12_RAYTRACING_GEOMETRY_FLAGS
ri_d3d12_AccelGeometryFlags(uint32_t flags) {
  D3D12_RAYTRACING_GEOMETRY_FLAGS out = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
  if (flags & RI_ACCEL_GEOMETRY_OPAQUE)
    out |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  if (flags & RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION)
    out |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
  return out;
}

// RIAccelGeometryDesc -> D3D12_RAYTRACING_GEOMETRY_DESC. The D3D12 twin of
// RI_VK_FillGeometry; `resolveAddresses` is false for prebuild size queries,
// where only the shape (counts, formats, strides) is consulted and the buffers
// need not be bound yet.
static void ri_d3d12_FillGeometry(struct RIDevice &device,
                                  const struct RIAccelGeometryDesc *src,
                                  D3D12_RAYTRACING_GEOMETRY_DESC *out,
                                  uint32_t *outMaxPrimitiveCount,
                                  bool resolveAddresses) {
  memset(out, 0, sizeof(*out));
  out->Flags = ri_d3d12_AccelGeometryFlags(src->flags);
  *outMaxPrimitiveCount = 0;

  switch (src->type) {
  case RI_ACCEL_GEOMETRY_TYPE_TRIANGLES: {
    const struct RIAccelTrianglesDesc *tri = &src->triangles;
    out->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC *t = &out->Triangles;
    t->VertexFormat = RIFormatToD3D12((uint32_t)tri->vertexFormat);
    t->VertexCount = tri->vertexNum;
    t->VertexBuffer.StrideInBytes = tri->vertexStride;
    // Unindexed geometry passes a null indexBuffer; D3D12 signals that with
    // DXGI_FORMAT_UNKNOWN and IndexCount 0 (VK_INDEX_TYPE_NONE_KHR's twin).
    t->IndexFormat = tri->indexBuffer ? (tri->indexType == RI_INDEX_TYPE_16
                                             ? DXGI_FORMAT_R16_UINT
                                             : DXGI_FORMAT_R32_UINT)
                                      : DXGI_FORMAT_UNKNOWN;
    t->IndexCount = tri->indexBuffer ? tri->indexNum : 0;
    if (resolveAddresses) {
      t->VertexBuffer.StartAddress =
          tri->vertexBuffer->GetDeviceHandle(&device) + tri->vertexOffset;
      // indexBuffer / transformBuffer are optional. A zero Transform3x4
      // address means identity, matching Vulkan's transformData semantics.
      t->IndexBuffer =
          tri->indexBuffer
              ? tri->indexBuffer->GetDeviceHandle(&device) + tri->indexOffset
              : 0;
      t->Transform3x4 = tri->transformBuffer
                            ? tri->transformBuffer->GetDeviceHandle(&device) +
                                  tri->transformOffset
                            : 0;
    }
    // Primitive count is triangleCount = indexNum/3 (indexed) or vertexNum/3
    // (unindexed) -- identical to the Vulkan build-range computation.
    const uint32_t indexCount =
        tri->indexBuffer ? tri->indexNum : tri->vertexNum;
    *outMaxPrimitiveCount = indexCount / 3;
    break;
  }
  case RI_ACCEL_GEOMETRY_TYPE_AABBS: {
    const struct RIAccelAabbsDesc *aab = &src->aabbs;
    out->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    out->AABBs.AABBCount = aab->num;
    out->AABBs.AABBs.StrideInBytes =
        aab->stride ? aab->stride : sizeof(struct RIAccelAabb);
    if (resolveAddresses)
      out->AABBs.AABBs.StartAddress =
          aab->buffer->GetDeviceHandle(&device) + aab->offset;
    *outMaxPrimitiveCount = aab->num;
    break;
  }
  }
}

bool ri_d3d12_dxrAvailable(const struct RIDevice &device) {
  return device.d3d12.device5 != nullptr &&
         device.physicalAdapter.d3d12.rayTracingTier >= 1;
}

void RID3D12_AccelStructureGetMemoryReqs(
    struct RIDevice &device, const struct RIAccelStructureDesc *desc,
    uint64_t *outStorageSize, uint64_t *outBuildScratchSize,
    uint64_t *outUpdateScratchSize) {
  if (outStorageSize)
    *outStorageSize = 0;
  if (outBuildScratchSize)
    *outBuildScratchSize = 0;
  if (outUpdateScratchSize)
    *outUpdateScratchSize = 0;
  if (!desc || !ri_d3d12_dxrAvailable(device))
    return;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.Flags = ri_d3d12_AccelBuildFlags(desc->flags);
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  if (desc->type == RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL) {
    inputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    geometries.resize(desc->geometryOrInstanceNum);
    for (uint32_t i = 0; i < desc->geometryOrInstanceNum; ++i) {
      uint32_t maxPrims = 0;
      ri_d3d12_FillGeometry(device, &desc->geometries[i], &geometries[i],
                            &maxPrims, /*resolveAddresses=*/false);
    }
    inputs.NumDescs = desc->geometryOrInstanceNum;
    inputs.pGeometryDescs = geometries.data();
  } else {
    // TLAS sizing scales with the instance count only; InstanceDescs is not
    // read by the prebuild query.
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.NumDescs = desc->geometryOrInstanceNum;
  }

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device.d3d12.device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,
                                                                      &info);
  if (outStorageSize)
    *outStorageSize = info.ResultDataMaxSizeInBytes;
  if (outBuildScratchSize)
    *outBuildScratchSize = info.ScratchDataSizeInBytes;
  if (outUpdateScratchSize)
    *outUpdateScratchSize = info.UpdateScratchDataSizeInBytes;
}

int RID3D12_InitAccelStructure(struct RIDevice &device,
                               struct RIAccelStructure &as,
                               const struct RIAccelStructureDesc *desc) {
  memset(&as.d3d12, 0, sizeof(as.d3d12));
  if (!ri_d3d12_dxrAvailable(device)) {
    hpl::Error("RI D3D12: acceleration structure requested on an adapter "
               "without DXR support\n");
    return RI_FAIL;
  }
  if (!desc || !desc->storage || desc->storage->isEmpty() ||
      desc->storageSize == 0) {
    hpl::Error("RI D3D12: acceleration structure needs a non-empty storage "
               "buffer and a storageSize from getMemoryReqs\n");
    return RI_FAIL;
  }

  const uint64_t base = desc->storage->GetDeviceHandle(&device);
  if (base == 0) {
    hpl::Error("RI D3D12: acceleration-structure storage buffer has no GPU "
               "virtual address\n");
    return RI_FAIL;
  }
  const uint64_t address = base + desc->storageOffset;
  // D3D12 requires the destination of a build to be aligned. The buffer base
  // is already aligned by RID3D12_CreateBuffer; a misaligned storageOffset is
  // a caller bug that the debug layer would otherwise report as a build error.
  if (address % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT != 0) {
    hpl::Error("RI D3D12: acceleration-structure storage offset is not "
               "%u-byte aligned\n",
               (unsigned)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    return RI_FAIL;
  }

  as.type = desc->type;
  as.flags = desc->flags;
  as.d3d12.deviceAddress = address;
  // Borrowed, not owned: the storage RIBuffer holds the only COM reference.
  // Kept so setDebugObjectName can name the underlying resource.
  as.d3d12.resource = desc->storage->d3d12.resource;
  as.d3d12.allocation = desc->storage->d3d12.allocation;
  // Globally-unique identity: a later allocation can reuse the same address,
  // which would collide in the descriptor-set cache. Same reasoning as the
  // Vulkan path.
  as.cookie = hash_random();
  return RI_SUCCESS;
}

void RID3D12_DisposeAccelStructure(struct RIDevice &device,
                                   struct RIAccelStructure &as) {
  (void)device;
  // No Release(): resource/allocation are borrowed from the caller-owned
  // storage buffer, which disposes them itself.
  memset(&as.d3d12, 0, sizeof(as.d3d12));
  as.cookie = 0;
}

void RID3D12_SetAccelStructureDebugName(struct RIDevice &device,
                                        struct RIAccelStructure &as,
                                        const char *name) {
  (void)device;
  if (!as.d3d12.resource || !name)
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
    as.d3d12.resource->SetName(wide);
    if (as.d3d12.allocation)
      as.d3d12.allocation->SetName(wide);
  }
}

bool RID3D12_AccelStructureIsEmpty(const struct RIAccelStructure &as) {
  return as.d3d12.deviceAddress == 0;
}

void RID3D12_BuildBlas(struct RIDevice &device, struct RICmd &cmd,
                       const struct RIBuildBlasDesc *descs, uint32_t numDescs) {
  if (numDescs == 0)
    return;
  assert(descs);
  assert(cmd.d3d12.cmdList7 &&
         "DXR builds need ID3D12GraphicsCommandList4 or newer");
  if (!ri_d3d12_dxrAvailable(device) || !cmd.d3d12.cmdList7)
    return;

  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  for (uint32_t i = 0; i < numDescs; ++i) {
    const struct RIBuildBlasDesc *d = &descs[i];
    assert(d->dst);
    assert(d->dst->d3d12.deviceAddress != 0); // dst must be init()'d
    assert(d->scratchBuffer);
    assert(d->geometryNum > 0);
    assert(d->geometries);

    geometries.clear();
    geometries.resize(d->geometryNum);
    for (uint32_t g = 0; g < d->geometryNum; ++g) {
      uint32_t maxPrims = 0;
      ri_d3d12_FillGeometry(device, &d->geometries[g], &geometries[g],
                            &maxPrims, /*resolveAddresses=*/true);
      // A BLAS built over unbound or freed geometry produces an acceleration
      // structure whose address later faults the TLAS build that references
      // it. Catch it here rather than at the driver. Mirrors the Vulkan
      // asserts in RICmd::buildBlas.
      assert(maxPrims > 0);
      if (geometries[g].Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES)
        assert(geometries[g].Triangles.VertexBuffer.StartAddress != 0);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    build.Inputs.Flags = ri_d3d12_AccelBuildFlags(d->dst->flags);
    build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    build.Inputs.NumDescs = d->geometryNum;
    build.Inputs.pGeometryDescs = geometries.data();
    if (d->mode == RI_ACCEL_BUILD_MODE_UPDATE) {
      build.Inputs.Flags |=
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
      assert(d->src && "UPDATE mode needs a source acceleration structure");
    }
    build.DestAccelerationStructureData = d->dst->d3d12.deviceAddress;
    build.SourceAccelerationStructureData =
        d->src ? d->src->d3d12.deviceAddress : 0;
    build.ScratchAccelerationStructureData =
        d->scratchBuffer->GetDeviceHandle(&device) + d->scratchOffset;
    assert(build.ScratchAccelerationStructureData %
               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT ==
           0);

    // D3D12 has no batched form; one call per acceleration structure.
    cmd.d3d12.cmdList7->BuildRaytracingAccelerationStructure(&build, 0,
                                                             nullptr);
  }
}

void RID3D12_BuildTlas(struct RIDevice &device, struct RICmd &cmd,
                       const struct RIBuildTlasDesc *descs, uint32_t numDescs) {
  if (numDescs == 0)
    return;
  assert(descs);
  assert(cmd.d3d12.cmdList7 &&
         "DXR builds need ID3D12GraphicsCommandList4 or newer");
  if (!ri_d3d12_dxrAvailable(device) || !cmd.d3d12.cmdList7)
    return;

  for (uint32_t i = 0; i < numDescs; ++i) {
    const struct RIBuildTlasDesc *d = &descs[i];
    assert(d->dst);
    assert(d->dst->d3d12.deviceAddress != 0); // dst must be init()'d
    assert(d->scratchBuffer);
    assert(d->instanceBuffer);
    // instanceNum == 0 is a legal build: HybridRenderer emits an empty TLAS
    // for worlds with no RT geometry so the RT descriptor pushes always have a
    // valid handle. Matches the Vulkan branch.

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    build.Inputs.Flags = ri_d3d12_AccelBuildFlags(d->dst->flags);
    build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    build.Inputs.NumDescs = d->instanceNum;
    if (d->instanceNum) {
      const uint64_t instanceAddress =
          d->instanceBuffer->GetDeviceHandle(&device);
      assert(instanceAddress != 0);
      build.Inputs.InstanceDescs = instanceAddress + d->instanceOffset;
    }
    if (d->mode == RI_ACCEL_BUILD_MODE_UPDATE) {
      build.Inputs.Flags |=
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
      assert(d->src && "UPDATE mode needs a source acceleration structure");
    }
    build.DestAccelerationStructureData = d->dst->d3d12.deviceAddress;
    build.SourceAccelerationStructureData =
        d->src ? d->src->d3d12.deviceAddress : 0;
    build.ScratchAccelerationStructureData =
        d->scratchBuffer->GetDeviceHandle(&device) + d->scratchOffset;
    assert(build.ScratchAccelerationStructureData %
               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT ==
           0);

    cmd.d3d12.cmdList7->BuildRaytracingAccelerationStructure(&build, 0,
                                                             nullptr);
  }
}

#endif // DEVICE_IMPL_D3D12
