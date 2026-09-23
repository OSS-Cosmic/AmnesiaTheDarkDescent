#ifndef RI_DESCRIPTOR_H
#define RI_DESCRIPTOR_H

// Descriptors, samplers and acceleration structures. Depends only on the
// prelude + resource leaf headers; RIDevice is used by pointer only, so a
// forward declaration keeps this below RIDevice.h in the include layering.
#include "graphics/RIPreamble.h"
#include "graphics/RIBarrier.h"    // RIResourceState_e (RIDescriptor::sampledImage)
#include "graphics/RIBuffer.h"     // RIBuffer (descriptor / accel geometry refs)
#include "graphics/RIFormat.h"     // RI_Format_e (RIAccelTrianglesDesc)
#include "graphics/RIPipeline.h"   // RIIndexType_e (RIAccelTrianglesDesc)
#include "graphics/RITextureView.h" // RITextureView (image descriptors)
#include "system/Hasher.h"         // hash_t / hash_data / HASH_INITIAL_VALUE
#include <cstring>                 // memset / strlen

struct RIDevice;
#if (DEVICE_IMPL_D3D12)
namespace D3D12MA { class Allocation; }
#endif

// Backend-neutral descriptor type (RIDescriptor::type); Vulkan maps it at bind
// via ri_vk_BindlessDescriptorType. The engine uses separate sampled images +
// samplers (no combined-image-sampler).
enum RIDescriptorType_e {
  RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
  RI_DESCRIPTOR_TYPE_STORAGE_IMAGE,
  RI_DESCRIPTOR_TYPE_SAMPLER,
  RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
  RI_DESCRIPTOR_TYPE_STORAGE_BUFFER,
  RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE,
};

enum RIAccelStructureType_e {
  RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL,
  RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL
};

enum RIAccelStructureBuildBits_e {
  RI_ACCEL_BUILD_NONE = 0,
  RI_ACCEL_BUILD_ALLOW_UPDATE = 0x1,
  RI_ACCEL_BUILD_ALLOW_COMPACTION = 0x2,
  RI_ACCEL_BUILD_ALLOW_DATA_ACCESS = 0x4,
  RI_ACCEL_BUILD_PREFER_FAST_TRACE = 0x8,
  RI_ACCEL_BUILD_PREFER_FAST_BUILD = 0x10,
  RI_ACCEL_BUILD_MINIMIZE_MEMORY = 0x20
};

enum RIAccelGeometryType_e {
  RI_ACCEL_GEOMETRY_TYPE_TRIANGLES,
  RI_ACCEL_GEOMETRY_TYPE_AABBS
};

enum RIAccelGeometryBits_e {
  RI_ACCEL_GEOMETRY_NONE = 0,
  RI_ACCEL_GEOMETRY_OPAQUE = 0x1,
  RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION = 0x2
};

enum RIAccelInstanceBits_e {
  RI_ACCEL_INSTANCE_NONE = 0,
  RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE = 0x1,
  RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING = 0x2,
  RI_ACCEL_INSTANCE_FORCE_OPAQUE = 0x4,
  RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE = 0x8
};

// requires src and dst, both built with RI_ACCEL_BUILD_ALLOW_UPDATE
enum RIAccelBuildMode_e {
  RI_ACCEL_BUILD_MODE_BUILD,
  RI_ACCEL_BUILD_MODE_UPDATE
};

enum RIDescriptorFlags_e {
  RI_VK_DESC_BEGIN = 0,
  RI_VK_DESC_OWN_SAMPLER = 0x1,   // owns the backing sampler
  RI_VK_DESC_OWN_IMAGE_VIEW = 0x2 // owns the backing image view
};

// matches VkAabbPositionsKHR layout
struct RIAccelAabb {
  RIAccelAabb() { memset(this, 0, sizeof(*this)); }
  float minX, minY, minZ;
  float maxX, maxY, maxZ;
};

struct RIAccelTrianglesDesc {
  struct RIBuffer *vertexBuffer;
  uint64_t vertexOffset;
  uint32_t vertexNum;
  uint16_t vertexStride;
  enum RI_Format_e vertexFormat;

  struct RIBuffer *indexBuffer; // optional, NULL = unindexed
  uint64_t indexOffset;
  uint32_t indexNum;
  enum RIIndexType_e indexType;

  struct RIBuffer
      *transformBuffer; // optional, points to RIAccelTransform entries
  uint64_t transformOffset;
};

struct RIAccelAabbsDesc {
  struct RIBuffer *buffer; // points to RIAccelAabb entries
  uint64_t offset;
  uint32_t num;
  uint32_t stride;
};

struct RIAccelGeometryDesc {
  RIAccelGeometryDesc() { memset(this, 0, sizeof(*this)); }
  enum RIAccelGeometryType_e type;
  uint32_t flags; // RIAccelGeometryBits_e
  union {
    struct RIAccelTrianglesDesc triangles;
    struct RIAccelAabbsDesc aabbs;
  };
};

struct DescriptorBindingID {
  DescriptorBindingID() { memset(this, 0, sizeof(*this)); }
  const char *name;
  hash_t hash;
  static DescriptorBindingID Create(const char *name) {
    struct DescriptorBindingID key;
    key.name = name;
    key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
    return key;
  }
};

static inline struct DescriptorBindingID
CreateDescriptorBindingID(const char *name) {
  struct DescriptorBindingID key;
  key.name = name;
  key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
  return key;
}

struct RIAccelStructure;
struct RIAccelStructureDesc;

// Backend sampler object. Created/cached once (cGraphics filter cache) and
// freed via dispose(); RIDescriptor snapshots the description it needs.
struct RISampler {
  RISampler() { memset(this, 0, sizeof(*this)); }
  void dispose(struct RIDevice *device);
  bool isEmpty() const;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSampler sampler;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      D3D12_SAMPLER_DESC desc;
      uint32_t initialized;
    } d3d12;
#endif
  };
  // Backend-neutral sampler description retained alongside the native object.
  // Values use the engine's eTextureWrap/eTextureFilter numeric enums.
  uint32_t wrapS;
  uint32_t wrapT;
  uint32_t wrapR;
  uint32_t filter;
  hash_t cookie;
};

struct RIDescriptorBufferPayload {
  const struct RIBuffer *resource; // borrowed RI resource identity
#if (DEVICE_IMPL_D3D12)
  ID3D12Resource *nativeResource; // borrowed native handle; no COM ownership
  D3D12MA::Allocation *allocation; // borrowed allocation identity snapshot
  uint64_t size;
#endif
  uint64_t offset;
  uint64_t range;
  uint32_t stride;
  uint8_t raw;
  uint8_t structured;
};

struct RIDescriptorTexturePayload {
  const struct RITexture *resource; // borrowed backing RI resource identity
  uint32_t dimension; // RITextureType_e (texture resource dimension)
  uint32_t viewType;  // RITextureViewType_e (including array/cube/storage)
  uint32_t format;    // RI_Format_e
  uint32_t baseMip;
  uint32_t mipNum;
  uint32_t baseLayer;
  uint32_t layerNum;
#if (DEVICE_IMPL_D3D12)
  ID3D12Resource *nativeResource; // borrowed native handle; no COM ownership
  D3D12MA::Allocation *allocation; // borrowed allocation identity snapshot
  uint32_t nativeFormat;
#endif
  uint32_t state; // RIResourceState_e requested for image access
};

struct RIDescriptorSamplerPayload {
  uint32_t wrapS;
  uint32_t wrapT;
  uint32_t wrapR;
  uint32_t filter;
#if (DEVICE_IMPL_D3D12)
  D3D12_SAMPLER_DESC d3d12Desc;
  uint32_t initialized;
#endif
};

struct RIDescriptorAccelPayload {
  uint64_t gpuVA;
#if (DEVICE_IMPL_D3D12)
  ID3D12Resource *nativeResource; // borrowed native handle; no COM ownership
  D3D12MA::Allocation *allocation; // borrowed allocation identity snapshot
#endif
};

#if (DEVICE_IMPL_VULKAN)
// Resolved Vulkan descriptor payload. This is deliberately a named native
// payload rather than a cast of the backend-neutral payload; D3D12 consumers
// must never reinterpret Vulkan storage (and vice versa).
struct RIDescriptorVulkanPayload {
  union {
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    VkAccelerationStructureKHR accelStructure;
  };
};
#endif

struct RIDescriptor {
  RIDescriptor() { memset(this, 0, sizeof(*this)); }

  // Builders snapshot value data and copy borrowed native handles; the handles
  // carry no ownership, so the consumer must keep the underlying resource alive
  // for the descriptor's use. `cookie` is derived from the resource's own cookie
  // folded with the binding parameters, and a resource with cookie == 0 yields
  // an empty descriptor. `state` selects the image layout/resource state.
  static RIDescriptor uniformBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range, uint32_t stride = 0,
                                    bool raw = false, bool structured = false);
  static RIDescriptor storageBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range, uint32_t stride = 0,
                                    bool raw = false, bool structured = false);
  static RIDescriptor sampledImage(struct RIDevice *device,
                                   struct RITextureView *view,
                                   enum RIResourceState_e state =
                                       RI_RESOURCE_STATE_SHADER_RESOURCE);
  static RIDescriptor storageImage(struct RIDevice *device,
                                   struct RITextureView *view);
  static RIDescriptor accelerationStructure(struct RIDevice *device,
                                            struct RIAccelStructure *as);
  static RIDescriptor sampler(struct RIDevice *device,
                              struct RISampler *sampler);

  bool isEmpty() const { return cookie == 0; }

  // Backend handle accessors — read the resolved handle stored inline at build
  // time. Vulkan handles are copied values; D3D12 native handles are borrowed
  // and never released by RIDescriptor.
#if (DEVICE_IMPL_VULKAN)
  VkImageView vkImageView() const;
  VkBuffer vkBuffer() const;
  VkSampler vkSampler() const;
  VkAccelerationStructureKHR vkAccel() const;
  VkImageLayout vkLayout() const;
#endif

  // unique id / descriptor-set cache key (0 == empty)
  hash_t cookie;
  // Backend-neutral descriptor type (RIDescriptorType_e).
  uint8_t type;
  // Kept separate from native backend storage so no backend needs to
  // reinterpret another backend's resource representation.
  union {
    RIDescriptorBufferPayload buffer;
    RIDescriptorTexturePayload texture;
    RIDescriptorSamplerPayload sampler;
    RIDescriptorAccelPayload accel;
  } payload;
  // Resolved backend descriptor info, filled in by the builders. Exactly one
  // union member is live per `type`; VkDescriptorImageInfo / VkDescriptorBufferInfo
  // are what the descriptor-set writer consumes directly.
  union {
#if (DEVICE_IMPL_VULKAN)
    RIDescriptorVulkanPayload vk;
#endif
  };
};

struct RIAccelStructure {
  RIAccelStructure() { memset(this, 0, sizeof(*this)); }

  // VK: creates a VkAccelerationStructureKHR over caller-allocated storage,
  // which must carry ACCELERATION_STRUCTURE_STORAGE + SHADER_DEVICE_ADDRESS
  // usage. D3D12 has no separate AS object and allocates its own resource
  // instead; see RID3D12_InitAccelStructure.
  int init(struct RIDevice *device, const struct RIAccelStructureDesc *desc);
  void dispose(struct RIDevice *device);
  bool isEmpty() const;
  uint64_t getDeviceAddress(struct RIDevice *device) const;
  void setDebugObjectName(struct RIDevice *device, const char *name);

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e snapshot
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkAccelerationStructureKHR handle;
      VkDeviceAddress deviceAddress;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12Resource *resource;  // owned; ray-tracing storage
      D3D12MA::Allocation *allocation; // owned; released after resource
      uint64_t deviceAddress;
    } d3d12;
#endif
  };
  hash_t cookie;
};

struct RIAccelStructureDesc {
  RIAccelStructureDesc() { memset(this, 0, sizeof(*this)); }

  // Query backing-storage and scratch sizes before RIAccelStructure::init;
  // any out-pointer may be NULL. For BLAS, geometries describes the geometry
  // layout used to compute sizes; the actual vertex/index buffer addresses
  // don't need to be valid until the build command.
  void getMemoryReqs(struct RIDevice *device, uint64_t *outStorageSize,
                     uint64_t *outBuildScratchSize,
                     uint64_t *outUpdateScratchSize) const;

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e
  uint32_t
      geometryOrInstanceNum; // BLAS: geometry count, TLAS: max instance count
  const struct RIAccelGeometryDesc *geometries; // BLAS only; NULL for TLAS
  struct RIBuffer *storage;
  uint64_t storageOffset;
  uint64_t storageSize; // from getMemoryReqs
};

struct RIBuildBlasDesc {
  RIBuildBlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  const struct RIAccelGeometryDesc *geometries;
  uint32_t geometryNum;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;
};

struct RIBuildTlasDesc {
  RIBuildTlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  uint32_t instanceNum;
  struct RIBuffer *instanceBuffer; // RIAccelInstance entries
  uint64_t instanceOffset;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;
};

#endif // RI_DESCRIPTOR_H
