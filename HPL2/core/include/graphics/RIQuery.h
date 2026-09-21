#ifndef RI_QUERY_H
#define RI_QUERY_H

#include "graphics/RIBuffer.h"
#include "graphics/RIDefines.h"
#include "graphics/RIPreamble.h"
#include <stdint.h>

struct RIDevice;
#if (DEVICE_IMPL_D3D12)
struct ID3D12QueryHeap;
#endif

// What a pool's queries count. Both occlusion variants come from the same
// backend heap; the distinction is made at begin/end/resolve time
// (VK_QUERY_CONTROL_PRECISE_BIT / D3D12_QUERY_TYPE_{,BINARY_}OCCLUSION).
enum RIQueryType_e {
  // Exact number of samples that passed. Vulkan needs
  // RIDevice::occlusionQueryPreciseEnabled; init downgrades to _BINARY and
  // republishes the effective type in RIQueryPool::type when it is absent.
  // D3D12 always supports it.
  RI_QUERY_TYPE_OCCLUSION,
  // "Did any sample pass" -- the result is 0 or a non-zero value with no
  // defined magnitude.
  RI_QUERY_TYPE_OCCLUSION_BINARY,
};

struct RIQueryPoolDesc {
  RIQueryType_e type;
  uint32_t queryCount;
};

// A pool of GPU queries with a backend-neutral host read. Results are 64-bit
// and only meaningful once the submit that recorded them has completed on the
// graphics timeline -- the caller gates on that, as RIGpuProfiler and
// cLightProbeQuery already do.
//
// Backend asymmetry, all of it contained here:
//   Vulkan  reads the pool directly, and requires every query be RESET on a
//           command buffer before it is begun.
//   D3D12   has no reset (EndQuery overwrites the slot) but cannot be read at
//           all until the data has been RESOLVED into a buffer, so this owns an
//           8-byte-per-query RI_MEMORY_HOST_READBACK buffer that
//           RICmd::vk_d3d12_resolveQueryPool drains into. Readback-heap
//           resources live permanently in COPY_DEST, so the resolve needs no
//           barrier.
// Record both vk_d3d12_resetQueryPool and vk_d3d12_resolveQueryPool on every
// backend; each is a no-op on the backend that does not need it.
//
// The backend members are parallel #if blocks rather than a union because
// RIBuffer has a non-trivial default constructor, which an anonymous union
// member would delete.
struct RIQueryPool {
  // Creates the VkQueryPool / ID3D12QueryHeap (+ the D3D12 readback buffer).
  // Returns false and leaves the pool empty on failure, or on a backend with no
  // occlusion-query support; every caller must degrade gracefully.
  bool init(struct RIDevice *device, const struct RIQueryPoolDesc &desc);
  void dispose(struct RIDevice *device);
  void setDebugObjectName(struct RIDevice *device, const char *name);
  bool isEmpty() const;
  // The type actually granted (see RI_QUERY_TYPE_OCCLUSION).
  bool isPrecise() const { return type == RI_QUERY_TYPE_OCCLUSION; }

  // Host read of `count` 64-bit results starting at `first`. False when the
  // results are not ready yet (Vulkan VK_NOT_READY), the pool is empty, or --
  // on D3D12 -- no resolve has covered that range. Never blocks.
  bool getResults(struct RIDevice *device, uint32_t first, uint32_t count,
                  uint64_t *results);

  uint32_t queryCount = 0;
  // Queries covered by the most recent vk_d3d12_resolveQueryPool. Vulkan leaves
  // it at queryCount (the pool is always readable); D3D12 uses it to reject a
  // getResults for a range no resolve has filled, which would otherwise hand
  // back the previous frame's counts.
  uint32_t resolvedCount = 0;
  uint8_t type = RI_QUERY_TYPE_OCCLUSION_BINARY; // RIQueryType_e, as granted
#if (DEVICE_IMPL_VULKAN)
  struct {
    VkQueryPool pool = VK_NULL_HANDLE;
  } vk;
#endif
#if (DEVICE_IMPL_D3D12)
  struct {
    ID3D12QueryHeap *heap = nullptr; // owned; released in dispose
    RIBuffer readback = {};          // 8 B/query, RI_MEMORY_HOST_READBACK
    uint64_t *mapped = nullptr;      // persistently mapped readback memory
  } d3d12;
#endif
};

#endif // RI_QUERY_H
