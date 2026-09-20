#ifndef RI_BUFFER_H
#define RI_BUFFER_H

#include "graphics/RIDefines.h"
#include "graphics/RIPreamble.h"
#include "system/Hasher.h"
#include <cstring>
#include <optional>
#include <stdint.h>


struct RIDevice;
struct RIRenderer;
#if (DEVICE_IMPL_D3D12)
struct ID3D12Resource;
namespace D3D12MA {
class Allocation;
}
#endif

enum RIBufferUsage_e {
  RI_BUFFER_USAGE_NONE = 0,
  RI_BUFFER_USAGE_SHADER_RESOURCE = 0x1,
  RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_BUFFER_USAGE_VERTEX_BUFFER = 0x4,
  RI_BUFFER_USAGE_INDEX_BUFFER = 0x8,
  RI_BUFFER_USAGE_CONSTANT_BUFFER = 0x10,
  RI_BUFFER_USAGE_ARGUMENT_BUFFER = 0x20,

  RI_BUFFER_USAGE_SCRATCH = 0x40,
  RI_BUFFER_USAGE_BINDING_TABLE = 0x80,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT = 0x100,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE = 0x200,
  RI_BUFFER_USAGE_TRANSFER_SRC = 0x400,
  RI_BUFFER_USAGE_TRANSFER_DST = 0x800,
  RI_BUFFER_USAGE_INDIRECT = 0x1000,
  // Buffer must be addressable as a raw GPU pointer (Vulkan BDA /
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT).
  RI_BUFFER_USAGE_DEVICE_ADDRESS = 0x2000,
};

// Where a buffer's memory lives. Maps to VMA usage + flags on Vulkan, to a
// D3D12MA heap type on D3D12.
enum RIMemoryLocation_e {
  // Device-local, not host-mapped (VMA AUTO_PREFER_DEVICE). mappedAddress is
  // null; seed via the resource uploader.
  RI_MEMORY_DEVICE,
  // Persistently mapped for sequential host writes
  // (VMA AUTO + MAPPED + HOST_ACCESS_SEQUENTIAL_WRITE).
  RI_MEMORY_HOST_UPLOAD,
  // Persistently mapped for random host reads — a GPU->CPU readback
  // destination (VMA AUTO + MAPPED + HOST_ACCESS_RANDOM). mappedAddress is
  // host-readable; invalidate the allocation before reading if it landed in
  // HOST_CACHED memory.
  RI_MEMORY_HOST_READBACK,
};

// Backend-neutral buffer creation descriptor consumed by RIBuffer::create.
struct RIBufferDesc {
  uint64_t size;
  uint32_t usage; // RIBufferUsage_e bitmask
  RIMemoryLocation_e location;
  // 0 = no requirement; otherwise the buffer's GPU address is a multiple of
  // this. Vulkan passes it to VMA unchanged. D3D12 follows D3D12MA, which
  // aligns every buffer to at least 256 bytes (the 64 KiB resource-placement
  // constant is not itself a GPU-VA guarantee).
  uint64_t alignment;
};

struct RIBuffer {
  RIBuffer() { memset(this, 0, sizeof(*this)); }

  void dispose(struct RIDevice *device);
  // Backend-neutral buffer factory: allocates (VMA / D3D12MA), sets
  // mappedAddress for host-upload buffers, and stamps the cookie. The cookie is
  // a globally-unique random value, since handle reuse makes the backend handle
  // unsafe as an identity; pass `hash` for a stable/shared cookie instead.
  static struct RIBuffer create(struct RIDevice *device,
                                const struct RIBufferDesc &desc,
                                std::optional<hash_t> hash = {});
  void setDebugObjectName(struct RIDevice *device, const char *name);
  // The buffer's GPU virtual address (VK buffer device address / D3D12 GPU VA),
  // 0 when empty. Not a descriptor index: shaders consuming it must use
  // raw-address addressing (root SRV/UAV on D3D12, BDA on Vulkan).
  uint64_t GetDeviceHandle(struct RIDevice *device);
  // Handle used by the geometry-pull shader. Vulkan uses the buffer device
  // address; D3D12 returns ((SRV index + 1) << 32) from the t4/space3 geometry
  // range, low word reserved for the stream byte offset. Separate from
  // GetDeviceHandle because BLAS construction still needs a real GPU VA.
  uint64_t GetShaderResourceHandle(struct RIDevice *device) const;
  void flushMappedRange(struct RIDevice *device, uint64_t offset,
                        uint64_t size);
  void invalidateMappedRange(struct RIDevice *device, uint64_t offset,
                             uint64_t size);
  bool isEmpty() const;

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      struct VmaAllocation_T *allocation;
      VkBuffer buffer;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12Resource *resource;
      D3D12MA::Allocation *allocation;
      uint64_t requestedSize;
      uint64_t allocationSize;
      uint32_t usage;
      uint8_t location;
      // UINT32_MAX means that the D3D12 binding layer has not registered the
      // buffer in the geometry raw-SRV table yet. It must never be confused
      // with a GPU virtual address.
      uint32_t shaderResourceIndex;
      bool shaderResourceArenaOwned;
    } d3d12;
#endif
  };
  void *mappedAddress;
  // Stable identity / descriptor-set cache key, stamped at creation from the
  // backend handle (0 == empty). RIDescriptor derives its cookie from this.
  hash_t cookie;
};

#endif // RI_BUFFER_H
