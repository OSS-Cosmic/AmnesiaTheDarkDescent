#ifndef RI_BARRIER_H
#define RI_BARRIER_H

#include "graphics/RIDefines.h"
#include <cassert>
#include <cstring>
#include <stdint.h>
#include <vector>

#include "graphics/RIPreamble.h"

struct RITexture;
struct RIBuffer;

// Combined access+layout resource state (Forge-style). Each bit encodes one
// (VkAccessFlags2, VkImageLayout) contribution and bits may be OR'd (e.g.
// STORAGE_WRITE | COPY_DST for a producer that both stored and copied); the
// pipeline stages are derived conservatively from the state unless narrowed
// by an RIStageBits_e hint.
enum RIResourceState_e {
  RI_RESOURCE_STATE_UNDEFINED          = 0,       // UNDEFINED layout, no access
  RI_RESOURCE_STATE_GENERAL            = 0x00001, // GENERAL layout, broad read/write
  RI_RESOURCE_STATE_RENDER_TARGET      = 0x00002, // COLOR_ATTACHMENT_OPTIMAL, write
  RI_RESOURCE_STATE_RENDER_TARGET_READ = 0x00004, // COLOR_ATTACHMENT_OPTIMAL, read|write (blend)
  RI_RESOURCE_STATE_DEPTH_WRITE        = 0x00008, // DEPTH_ATTACHMENT_OPTIMAL
  RI_RESOURCE_STATE_DEPTH_READ         = 0x00010, // DEPTH_READ_ONLY_OPTIMAL
  RI_RESOURCE_STATE_SHADER_RESOURCE    = 0x00020, // SHADER_READ_ONLY_OPTIMAL, sampled
  RI_RESOURCE_STATE_STORAGE_READ       = 0x00040, // GENERAL, storage read
  RI_RESOURCE_STATE_STORAGE_WRITE      = 0x00080, // GENERAL, storage write
  RI_RESOURCE_STATE_UNORDERED_ACCESS   = 0x000C0, // STORAGE_READ | STORAGE_WRITE
  RI_RESOURCE_STATE_COPY_SRC           = 0x00100, // TRANSFER_SRC_OPTIMAL
  RI_RESOURCE_STATE_COPY_DST           = 0x00200, // TRANSFER_DST_OPTIMAL
  RI_RESOURCE_STATE_PRESENT            = 0x00400, // PRESENT_SRC_KHR
  RI_RESOURCE_STATE_INDIRECT_ARGUMENT  = 0x00800, // buffer only
  RI_RESOURCE_STATE_VERTEX_BUFFER      = 0x01000, // buffer only
  RI_RESOURCE_STATE_INDEX_BUFFER       = 0x02000, // buffer only
  RI_RESOURCE_STATE_CONSTANT_BUFFER    = 0x04000, // buffer only
  RI_RESOURCE_STATE_ACCEL_READ         = 0x08000, // acceleration structure read
  RI_RESOURCE_STATE_ACCEL_WRITE        = 0x10000, // acceleration structure build write
  RI_RESOURCE_STATE_CLEAR_STORAGE      = 0x20000, // GENERAL, vkCmdClear* transfer write
  // Read of the buffers an acceleration-structure BUILD consumes -- instance
  // descriptors, vertex, index, transform -- as opposed to a read of an
  // acceleration structure itself, which is ACCEL_READ.
  //
  // Vulkan covers both with VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT, so
  // the two states map identically there. D3D12 does not: its AS access bits
  // are legal only on a resource created as an acceleration structure, and a
  // build input is an ordinary buffer, so it must use SHADER_RESOURCE. One
  // shared state cannot express that, which is why this bit exists.
  RI_RESOURCE_STATE_ACCEL_BUILD_INPUT  = 0x40000, // buffer only
};

// Optional per-side stage narrowing; 0 derives a conservative mask from the
// resource state (e.g. SHADER_RESOURCE -> all shader stages).
enum RIStageBits_e {
  RI_STAGE_NONE          = 0,
  RI_STAGE_VERTEX        = 0x0001,
  RI_STAGE_FRAGMENT      = 0x0002,
  RI_STAGE_COMPUTE       = 0x0004,
  RI_STAGE_RAY_TRACING   = 0x0008,
  RI_STAGE_DRAW_INDIRECT = 0x0010,
  RI_STAGE_COPY          = 0x0020,
  RI_STAGE_BLIT          = 0x0040,
  RI_STAGE_CLEAR         = 0x0080,
  RI_STAGE_ACCEL_BUILD   = 0x0100,
  RI_STAGE_ALL_GRAPHICS  = RI_STAGE_VERTEX | RI_STAGE_FRAGMENT,
  RI_STAGE_ALL_SHADER    = RI_STAGE_VERTEX | RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING,
};

// These are the logical-device feature bits relevant to barrier conversion.
// Value-initialization (RIBarrierCapabilities{}) sets both bits to false;
// RICmd also clears them through its memset-based initialization.
struct RIBarrierCapabilities {
  bool rayTracingPipelineEnabled;
  bool accelerationStructureEnabled;
};

enum RIBarrierAspect_e {
  RI_BARRIER_ASPECT_COLOR = 0,
  RI_BARRIER_ASPECT_DEPTH,
  RI_BARRIER_ASPECT_STENCIL,
  RI_BARRIER_ASPECT_DEPTH_STENCIL,
};

struct RITextureBarrier {
  RITextureBarrier() { memset(this, 0, sizeof(*this)); }
  // Whole-resource transition (mip/layer count 0 = REMAINING), COLOR aspect
  // by default; stage hints of 0 derive conservatively from the states.
  // Narrow the subresource range via the trailing members afterwards.
  RITextureBarrier(struct RITexture *texture, uint32_t before,
                     uint32_t after, uint32_t beforeStages = 0,
                     uint32_t afterStages = 0,
                     enum RIBarrierAspect_e aspect = RI_BARRIER_ASPECT_COLOR)
      : texture(texture), before(before), after(after),
        beforeStages(beforeStages), afterStages(afterStages), aspect(aspect),
        baseMip(0), mipCount(0), baseLayer(0), layerCount(0) {}
  struct RITexture *texture;
  uint32_t before;       // RIResourceState_e bits
  uint32_t after;        // RIResourceState_e bits
  uint32_t beforeStages; // RIStageBits_e; 0 => derive from 'before'
  uint32_t afterStages;  // RIStageBits_e; 0 => derive from 'after'
  enum RIBarrierAspect_e aspect; // default COLOR
  uint16_t baseMip;
  uint16_t mipCount;   // 0 => VK_REMAINING_MIP_LEVELS
  uint16_t baseLayer;
  uint16_t layerCount; // 0 => VK_REMAINING_ARRAY_LAYERS
};

struct RIBufferBarrier {
  RIBufferBarrier() { memset(this, 0, sizeof(*this)); }
  // Whole-buffer transition (size 0 = WHOLE_SIZE); stage hints of 0 derive
  // conservatively from the states.
  RIBufferBarrier(struct RIBuffer *buffer, uint32_t before, uint32_t after,
                    uint32_t beforeStages = 0, uint32_t afterStages = 0,
                    uint64_t offset = 0, uint64_t size = 0)
      : buffer(buffer), before(before), after(after),
        beforeStages(beforeStages), afterStages(afterStages), offset(offset),
        size(size) {}
  struct RIBuffer *buffer;
  uint32_t before;       // RIResourceState_e bits
  uint32_t after;        // RIResourceState_e bits
  uint32_t beforeStages; // RIStageBits_e; 0 => derive from 'before'
  uint32_t afterStages;  // RIStageBits_e; 0 => derive from 'after'
  uint64_t offset;
  uint64_t size; // 0 => VK_WHOLE_SIZE
};

// Global execution+memory barrier (no resource handle).
struct RIMemoryBarrier {
  RIMemoryBarrier() { memset(this, 0, sizeof(*this)); }
  RIMemoryBarrier(uint32_t before, uint32_t after, uint32_t beforeStages = 0,
                    uint32_t afterStages = 0)
      : before(before), after(after), beforeStages(beforeStages),
        afterStages(afterStages) {}
  uint32_t before;       // RIResourceState_e bits
  uint32_t after;        // RIResourceState_e bits
  uint32_t beforeStages; // RIStageBits_e; 0 => derive from 'before'
  uint32_t afterStages;  // RIStageBits_e; 0 => derive from 'after'
};

template <typename T, uint32_t N> struct ScratchBuffer {
  T *get(uint32_t count) {
    assert(count <= N);
    (void)count;
    return data;
  }
  T data[N];
};
template <typename T> struct ScratchBuffer<T, 0> {
  T *get(uint32_t count) {
    heap.resize(count);
    return heap.data();
  }
  std::vector<T> heap;
};

#if (DEVICE_IMPL_VULKAN)

static inline VkImageLayout ri_vk_RIResourceStateToImageLayout(uint32_t state) {
  if (state == RI_RESOURCE_STATE_UNDEFINED)
    return VK_IMAGE_LAYOUT_UNDEFINED;
  // GENERAL wins over the optimal layouts: a state that mixes storage access
  // with anything else (e.g. STORAGE | SHADER_RESOURCE for a sampled view of
  // a storage image) can only be satisfied by the GENERAL layout.
  if (state & (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_STORAGE_READ |
               RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_CLEAR_STORAGE))
    return VK_IMAGE_LAYOUT_GENERAL;
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
    return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  if (state & RI_RESOURCE_STATE_DEPTH_READ)
    return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
  if (state & RI_RESOURCE_STATE_SHADER_RESOURCE)
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (state & RI_RESOURCE_STATE_COPY_SRC)
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (state & RI_RESOURCE_STATE_COPY_DST)
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  if (state & RI_RESOURCE_STATE_PRESENT)
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  // buffer-only / accel states have no image layout
  assert(false);
  return VK_IMAGE_LAYOUT_UNDEFINED;
}

// Map a resource state for one image aspect. The legacy overload above keeps
// the color/default mapping used by existing callers; depth/stencil barriers
// can select Vulkan's separate stencil layouts when the aspects are split.
static inline VkImageLayout
ri_vk_RIResourceStateToImageLayout(uint32_t state,
                                   enum RIBarrierAspect_e aspect) {
  if (aspect == RI_BARRIER_ASPECT_STENCIL) {
    if (state == RI_RESOURCE_STATE_UNDEFINED)
      return VK_IMAGE_LAYOUT_UNDEFINED;
    if (state & (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_STORAGE_READ |
                 RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_CLEAR_STORAGE))
      return VK_IMAGE_LAYOUT_GENERAL;
    if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
      return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
    if (state & RI_RESOURCE_STATE_DEPTH_READ)
      return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
  }
  if (aspect == RI_BARRIER_ASPECT_DEPTH_STENCIL) {
    if (state == RI_RESOURCE_STATE_UNDEFINED)
      return VK_IMAGE_LAYOUT_UNDEFINED;
    if (state & (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_STORAGE_READ |
                 RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_CLEAR_STORAGE))
      return VK_IMAGE_LAYOUT_GENERAL;
    if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (state & RI_RESOURCE_STATE_DEPTH_READ)
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  }
  return ri_vk_RIResourceStateToImageLayout(state);
}

static inline VkAccessFlags2 ri_vk_RIResourceStateToAccess(uint32_t state) {
  VkAccessFlags2 access = VK_ACCESS_2_NONE;
  if (state & RI_RESOURCE_STATE_GENERAL)
    access |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_RENDER_TARGET)
    access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_RENDER_TARGET_READ)
    access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
    access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_DEPTH_READ)
    access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  if (state & RI_RESOURCE_STATE_SHADER_RESOURCE)
    access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
  if (state & RI_RESOURCE_STATE_STORAGE_READ)
    access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
  if (state & RI_RESOURCE_STATE_STORAGE_WRITE)
    access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_COPY_SRC)
    access |= VK_ACCESS_2_TRANSFER_READ_BIT;
  if (state & RI_RESOURCE_STATE_COPY_DST)
    access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  if (state & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  if (state & RI_RESOURCE_STATE_VERTEX_BUFFER)
    access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
  if (state & RI_RESOURCE_STATE_INDEX_BUFFER)
    access |= VK_ACCESS_2_INDEX_READ_BIT;
  if (state & RI_RESOURCE_STATE_CONSTANT_BUFFER)
    access |= VK_ACCESS_2_UNIFORM_READ_BIT;
  // A build reading its inputs takes the same Vulkan access as a read of the
  // structure itself; only D3D12 needs them apart.
  if (state & (RI_RESOURCE_STATE_ACCEL_READ |
               RI_RESOURCE_STATE_ACCEL_BUILD_INPUT))
    access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  if (state & RI_RESOURCE_STATE_ACCEL_WRITE)
    access |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  if (state & RI_RESOURCE_STATE_CLEAR_STORAGE)
    access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  return access;
}

static inline bool ri_vk_RIBarrierStateSupported(
    uint32_t state, const RIBarrierCapabilities &capabilities) {
  const uint32_t accelStates = RI_RESOURCE_STATE_ACCEL_READ |
                               RI_RESOURCE_STATE_ACCEL_WRITE |
                               RI_RESOURCE_STATE_ACCEL_BUILD_INPUT;
  return (state & accelStates) == 0 ||
         capabilities.accelerationStructureEnabled;
}

// Conservative stage derivation for barriers that omit a stage hint. Returns
// NONE and reports failure for a state that requires a disabled feature.
static inline VkPipelineStageFlags2 ri_vk_RIStageMaskFromState(
    uint32_t state, const RIBarrierCapabilities &capabilities,
    bool *valid) {
  if (valid)
    *valid = ri_vk_RIBarrierStateSupported(state, capabilities);
  if (!ri_vk_RIBarrierStateSupported(state, capabilities))
    return VK_PIPELINE_STAGE_2_NONE;
  VkPipelineStageFlags2 flags = VK_PIPELINE_STAGE_2_NONE;
  if (state & (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_SHADER_RESOURCE |
               RI_RESOURCE_STATE_STORAGE_READ | RI_RESOURCE_STATE_STORAGE_WRITE |
               RI_RESOURCE_STATE_CONSTANT_BUFFER)) {
    flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (capabilities.rayTracingPipelineEnabled)
      flags |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
  }
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (state & (RI_RESOURCE_STATE_DEPTH_WRITE | RI_RESOURCE_STATE_DEPTH_READ))
    flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  if (state & (RI_RESOURCE_STATE_COPY_SRC | RI_RESOURCE_STATE_COPY_DST))
    flags |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT;
  if (state & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  if (state & (RI_RESOURCE_STATE_VERTEX_BUFFER | RI_RESOURCE_STATE_INDEX_BUFFER))
    flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
  // AS reads, AS writes and reads of a build's inputs are all synchronized at
  // the AS build stage. An AS read does not imply that a ray-tracing pipeline
  // is enabled: AS-only devices legitimately use acceleration structures for
  // build/compaction.
  if (state & (RI_RESOURCE_STATE_ACCEL_READ | RI_RESOURCE_STATE_ACCEL_WRITE |
               RI_RESOURCE_STATE_ACCEL_BUILD_INPUT))
    flags |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  if (state & RI_RESOURCE_STATE_CLEAR_STORAGE)
    flags |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
  // UNDEFINED / PRESENT contribute no stages.
  return flags;
}

static inline VkPipelineStageFlags2
ri_vk_RIStageBitsToVK(uint32_t stageBits, uint32_t stateFallback,
                      const RIBarrierCapabilities &capabilities,
                      bool *valid) {
  if (valid)
    *valid = ri_vk_RIBarrierStateSupported(stateFallback, capabilities);
  if (!ri_vk_RIBarrierStateSupported(stateFallback, capabilities))
    return VK_PIPELINE_STAGE_2_NONE;
  if (stageBits == RI_STAGE_NONE)
    return ri_vk_RIStageMaskFromState(stateFallback, capabilities, valid);
  VkPipelineStageFlags2 flags = VK_PIPELINE_STAGE_2_NONE;
  // RIStageBits_e has no attachment-output / depth-test bits, so an explicit
  // hint can never name the fixed-function stages that attachment accesses in
  // the state REQUIRE (VUID-VkImageMemoryBarrier2-src/dstAccessMask-03911/
  // 03893). OR them in from the state so hand-tuned shader-stage hints stay
  // narrow but the mask remains legal.
  if (stateFallback & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (stateFallback & (RI_RESOURCE_STATE_DEPTH_WRITE | RI_RESOURCE_STATE_DEPTH_READ))
    flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  const bool hasTransferHint =
      (stageBits & (RI_STAGE_COPY | RI_STAGE_BLIT | RI_STAGE_CLEAR)) != 0;
  if ((stateFallback & (RI_RESOURCE_STATE_COPY_SRC | RI_RESOURCE_STATE_COPY_DST)) &&
      !hasTransferHint)
    flags |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT;
  if (stateFallback & RI_RESOURCE_STATE_CLEAR_STORAGE)
    flags |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
  if (stateFallback & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  if (stateFallback & (RI_RESOURCE_STATE_VERTEX_BUFFER | RI_RESOURCE_STATE_INDEX_BUFFER))
    flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
  if (stageBits & RI_STAGE_VERTEX)
    flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  if (stageBits & RI_STAGE_FRAGMENT)
    flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  if (stageBits & RI_STAGE_COMPUTE)
    flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if (stageBits & RI_STAGE_RAY_TRACING) {
    // A ray-tracing-only hint cannot be honored on a device without the
    // pipeline feature. In a mixed hint, retain the legal non-RT stages;
    // this is useful for callers sharing a broad shader hint across devices.
    const uint32_t nonRayTracingStages = stageBits & ~RI_STAGE_RAY_TRACING;
    const uint32_t rasterShaderStages = RI_STAGE_VERTEX | RI_STAGE_FRAGMENT |
                                        RI_STAGE_COMPUTE;
    const bool shaderAccess =
        (stateFallback & (RI_RESOURCE_STATE_GENERAL |
                          RI_RESOURCE_STATE_SHADER_RESOURCE |
                          RI_RESOURCE_STATE_STORAGE_READ |
                          RI_RESOURCE_STATE_STORAGE_WRITE |
                          RI_RESOURCE_STATE_CONSTANT_BUFFER)) != 0;
    if (!capabilities.rayTracingPipelineEnabled &&
        (nonRayTracingStages == RI_STAGE_NONE ||
         (((nonRayTracingStages & rasterShaderStages) == RI_STAGE_NONE) &&
          shaderAccess))) {
      if (valid)
        *valid = false;
      return VK_PIPELINE_STAGE_2_NONE;
    }
    if (capabilities.rayTracingPipelineEnabled)
      flags |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
  }
  if (stageBits & RI_STAGE_DRAW_INDIRECT)
    flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  if (stageBits & RI_STAGE_COPY)
    flags |= VK_PIPELINE_STAGE_2_COPY_BIT;
  if (stageBits & RI_STAGE_BLIT)
    flags |= VK_PIPELINE_STAGE_2_BLIT_BIT;
  if (stageBits & RI_STAGE_CLEAR)
    flags |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
  if (stageBits & RI_STAGE_ACCEL_BUILD) {
    if (!capabilities.accelerationStructureEnabled) {
      if (valid)
        *valid = false;
      return VK_PIPELINE_STAGE_2_NONE;
    }
    flags |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  }
  if (stateFallback & RI_RESOURCE_STATE_ACCEL_WRITE)
    flags |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  return flags;
}

static inline VkImageAspectFlags
ri_vk_RIBarrierAspectToVK(enum RIBarrierAspect_e aspect) {
  switch (aspect) {
  case RI_BARRIER_ASPECT_COLOR:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  case RI_BARRIER_ASPECT_DEPTH:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case RI_BARRIER_ASPECT_STENCIL:
    return VK_IMAGE_ASPECT_STENCIL_BIT;
  case RI_BARRIER_ASPECT_DEPTH_STENCIL:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  assert(false);
  return VK_IMAGE_ASPECT_COLOR_BIT;
}

#endif

#if (DEVICE_IMPL_D3D12)

// Translate each RI resource-state bit to its corresponding D3D12 state.
static inline D3D12_RESOURCE_STATES
ri_d3d12_RIResourceStateToStates(uint32_t state) {
  if (state == RI_RESOURCE_STATE_UNDEFINED)
    return D3D12_RESOURCE_STATE_COMMON;

  D3D12_RESOURCE_STATES states = D3D12_RESOURCE_STATE_COMMON;
  if (state & RI_RESOURCE_STATE_GENERAL)
    states |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET |
               RI_RESOURCE_STATE_RENDER_TARGET_READ))
    states |= D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
    states |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
  if (state & RI_RESOURCE_STATE_DEPTH_READ)
    states |= D3D12_RESOURCE_STATE_DEPTH_READ;
  if (state & RI_RESOURCE_STATE_SHADER_RESOURCE)
    states |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (state & (RI_RESOURCE_STATE_STORAGE_READ |
               RI_RESOURCE_STATE_STORAGE_WRITE |
               RI_RESOURCE_STATE_UNORDERED_ACCESS |
               RI_RESOURCE_STATE_CLEAR_STORAGE))
    states |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (state & RI_RESOURCE_STATE_COPY_SRC)
    states |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (state & RI_RESOURCE_STATE_COPY_DST)
    states |= D3D12_RESOURCE_STATE_COPY_DEST;
  if (state & RI_RESOURCE_STATE_PRESENT)
    states |= D3D12_RESOURCE_STATE_PRESENT;
  if (state & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    states |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  if (state & (RI_RESOURCE_STATE_VERTEX_BUFFER |
               RI_RESOURCE_STATE_CONSTANT_BUFFER))
    states |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
  if (state & RI_RESOURCE_STATE_INDEX_BUFFER)
    states |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
  if (state & (RI_RESOURCE_STATE_ACCEL_READ |
               RI_RESOURCE_STATE_ACCEL_WRITE))
    states |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
  // A build's input buffers are ordinary resources that the build reads as
  // shader resources. The RAYTRACING_ACCELERATION_STRUCTURE state is reserved
  // for the structures themselves and is rejected on anything else.
  if (state & RI_RESOURCE_STATE_ACCEL_BUILD_INPUT)
    states |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return states;
}

static inline D3D12_BARRIER_ACCESS
ri_d3d12_RIResourceStateToBarrierAccess(uint32_t state) {
  if (state == RI_RESOURCE_STATE_UNDEFINED)
    return D3D12_BARRIER_ACCESS_NO_ACCESS;
  // D3D12_BARRIER_ACCESS_NO_ACCESS (0x80000000) is mutually exclusive with
  // every real access bit; the fallback must be COMMON (0) so the OR below
  // does not smuggle NO_ACCESS into a valid access mask.
  D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
  if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
    access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
  if (state & RI_RESOURCE_STATE_DEPTH_READ)
    access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
  if (state & RI_RESOURCE_STATE_SHADER_RESOURCE)
    access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  if (state & (RI_RESOURCE_STATE_STORAGE_READ | RI_RESOURCE_STATE_STORAGE_WRITE |
               RI_RESOURCE_STATE_UNORDERED_ACCESS | RI_RESOURCE_STATE_CLEAR_STORAGE |
               RI_RESOURCE_STATE_GENERAL))
    access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
  if (state & RI_RESOURCE_STATE_COPY_SRC)
    access |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
  if (state & RI_RESOURCE_STATE_COPY_DST)
    access |= D3D12_BARRIER_ACCESS_COPY_DEST;
  if (state & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
  if (state & RI_RESOURCE_STATE_VERTEX_BUFFER)
    access |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
  if (state & RI_RESOURCE_STATE_INDEX_BUFFER)
    access |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
  if (state & RI_RESOURCE_STATE_CONSTANT_BUFFER)
    access |= D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
  if (state & RI_RESOURCE_STATE_ACCEL_READ)
    access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
  if (state & RI_RESOURCE_STATE_ACCEL_WRITE)
    access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
  // Deliberately NOT an AS access bit. Those two are valid only on a resource
  // created with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE or the
  // matching legacy initial state; a build input -- the TLAS instance-descriptor
  // array, a BLAS vertex/index buffer -- is neither, and the debug layer
  // rejects the barrier outright (INCOMPATIBLE_BARRIER_ACCESS).
  if (state & RI_RESOURCE_STATE_ACCEL_BUILD_INPUT)
    access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  return access ? access : D3D12_BARRIER_ACCESS_COMMON;
}

static inline D3D12_BARRIER_LAYOUT
ri_d3d12_RIResourceStateToBarrierLayout(uint32_t state) {
  if (state == RI_RESOURCE_STATE_UNDEFINED)
    return D3D12_BARRIER_LAYOUT_UNDEFINED;
  if (state & (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_STORAGE_READ |
               RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_UNORDERED_ACCESS |
               RI_RESOURCE_STATE_CLEAR_STORAGE))
    return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  if (state & RI_RESOURCE_STATE_DEPTH_WRITE)
    return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
  // Each specific read layout admits exactly one access kind
  // (DEPTH_STENCIL_READ -> depth read only, SHADER_RESOURCE -> SRV only).
  // Combined read states need a generic read layout: the direct-queue variant
  // is the only one that also admits depth-stencil read, which is what a
  // read-only depth attachment sampled in the same pass requires.
  const uint32_t readStates = state & (RI_RESOURCE_STATE_DEPTH_READ |
                                       RI_RESOURCE_STATE_SHADER_RESOURCE |
                                       RI_RESOURCE_STATE_COPY_SRC);
  if ((state & RI_RESOURCE_STATE_DEPTH_READ) &&
      readStates != RI_RESOURCE_STATE_DEPTH_READ)
    return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ;
  if (state & RI_RESOURCE_STATE_DEPTH_READ)
    return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
  if ((state & RI_RESOURCE_STATE_SHADER_RESOURCE) &&
      (state & RI_RESOURCE_STATE_COPY_SRC))
    return D3D12_BARRIER_LAYOUT_GENERIC_READ;
  if (state & RI_RESOURCE_STATE_SHADER_RESOURCE)
    return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  if (state & RI_RESOURCE_STATE_COPY_SRC)
    return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
  if (state & RI_RESOURCE_STATE_COPY_DST)
    return D3D12_BARRIER_LAYOUT_COPY_DEST;
  if (state & RI_RESOURCE_STATE_PRESENT)
    return D3D12_BARRIER_LAYOUT_PRESENT;
  return D3D12_BARRIER_LAYOUT_UNDEFINED;
}

// D3D12 permits only a restricted sync set alongside the acceleration-structure
// access bits: no VERTEX_SHADING, no PIXEL_SHADING, no DRAW. Vulkan has no such
// rule -- a pixel shader running an inline ray query is an ordinary
// FRAGMENT-stage AS read, which several shaders here do (Translucent.frag,
// Water*.frag, via traceShadowRay) -- so RI stage hints legitimately name those
// stages, and it is this translation that has to fold them into the coarse
// ALL_SHADING bucket. The compatibility table does allow that one, and it still
// covers pixel shading, so nothing is lost but precision.
//
// Keyed on ACCEL_READ / ACCEL_WRITE only. ACCEL_BUILD_INPUT is deliberately
// excluded: its access is SHADER_RESOURCE, which pairs with PIXEL_SHADING
// perfectly well, and coarsening it would give up precision for no reason.
static inline D3D12_BARRIER_SYNC
ri_d3d12_AccelCompatibleSync(D3D12_BARRIER_SYNC sync, uint32_t state) {
  if (!(state & (RI_RESOURCE_STATE_ACCEL_READ | RI_RESOURCE_STATE_ACCEL_WRITE)))
    return sync;
  const D3D12_BARRIER_SYNC graphicsSync = D3D12_BARRIER_SYNC_VERTEX_SHADING |
                                          D3D12_BARRIER_SYNC_PIXEL_SHADING |
                                          D3D12_BARRIER_SYNC_DRAW;
  if (sync & graphicsSync)
    sync = (D3D12_BARRIER_SYNC)((sync & ~graphicsSync) |
                                D3D12_BARRIER_SYNC_ALL_SHADING);
  return sync;
}

static inline D3D12_BARRIER_SYNC
ri_d3d12_RIStageMaskFromStateBarrier(uint32_t state) {
  D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
  if (state & (RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_READ |
               RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_CONSTANT_BUFFER |
               RI_RESOURCE_STATE_GENERAL))
    sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING |
            D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_RAYTRACING;
  if (state & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
  if (state & (RI_RESOURCE_STATE_DEPTH_WRITE | RI_RESOURCE_STATE_DEPTH_READ))
    sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
  if (state & (RI_RESOURCE_STATE_COPY_SRC | RI_RESOURCE_STATE_COPY_DST))
    sync |= D3D12_BARRIER_SYNC_COPY;
  if (state & RI_RESOURCE_STATE_INDIRECT_ARGUMENT)
    sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
  if (state & (RI_RESOURCE_STATE_VERTEX_BUFFER | RI_RESOURCE_STATE_INDEX_BUFFER))
    sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
  // Build inputs join the AS states here: the build is what reads them, so it
  // is the build that has to be synchronized against, even though the access
  // bit above is SHADER_RESOURCE rather than an AS one.
  if (state & (RI_RESOURCE_STATE_ACCEL_READ | RI_RESOURCE_STATE_ACCEL_WRITE |
               RI_RESOURCE_STATE_ACCEL_BUILD_INPUT))
    sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
  if (state & RI_RESOURCE_STATE_CLEAR_STORAGE)
    sync |= D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
  // A shader-readable state ORed with an accel state lands the graphics sync
  // bits from above next to an AS access bit, which is the illegal pairing.
  sync = ri_d3d12_AccelCompatibleSync(sync, state);
  return sync ? sync : (state == RI_RESOURCE_STATE_UNDEFINED ?
                         D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_ALL);
}

static inline D3D12_BARRIER_SYNC
ri_d3d12_RIStageBitsToBarrierSync(uint32_t stageBits, uint32_t stateFallback) {
  if (stageBits == RI_STAGE_NONE)
    return ri_d3d12_RIStageMaskFromStateBarrier(stateFallback);
  D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
  if (stateFallback & (RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ))
    sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
  if (stateFallback & (RI_RESOURCE_STATE_DEPTH_WRITE | RI_RESOURCE_STATE_DEPTH_READ))
    sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
  // Callers pass Vulkan-style stages (FRAGMENT for depth/colour attachments),
  // but D3D12 rejects sync bits incompatible with the access: an
  // attachment-only state is touched solely by the output merger / depth
  // test, so PIXEL_SHADING etc. alongside DEPTH_STENCIL_WRITE is an invalid
  // barrier. Without the debug layer that removes the device (INVALID_CALL).
  const uint32_t attachmentStates =
      RI_RESOURCE_STATE_RENDER_TARGET | RI_RESOURCE_STATE_RENDER_TARGET_READ |
      RI_RESOURCE_STATE_DEPTH_WRITE | RI_RESOURCE_STATE_DEPTH_READ;
  if (stateFallback != RI_RESOURCE_STATE_UNDEFINED &&
      (stateFallback & ~attachmentStates) == 0)
    return sync;
  if (stageBits & RI_STAGE_VERTEX) sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
  if (stageBits & RI_STAGE_FRAGMENT) sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
  if (stageBits & RI_STAGE_COMPUTE) sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
  if (stageBits & RI_STAGE_RAY_TRACING) sync |= D3D12_BARRIER_SYNC_RAYTRACING;
  if (stageBits & RI_STAGE_DRAW_INDIRECT) sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
  if (stageBits & RI_STAGE_COPY) sync |= D3D12_BARRIER_SYNC_COPY;
  if (stageBits & RI_STAGE_BLIT) sync |= D3D12_BARRIER_SYNC_COPY;
  if (stageBits & RI_STAGE_CLEAR) sync |= D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
  if (stageBits & RI_STAGE_ACCEL_BUILD)
    sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
  // The same rejection the attachment case above guards against, for the
  // acceleration-structure access bits: an RI_STAGE_FRAGMENT hint on an
  // ACCEL_READ barrier -- which is what publishing a freshly built TLAS to
  // fragment-stage ray queries asks for -- would pair PIXEL_SHADING with an AS
  // access and be refused.
  return ri_d3d12_AccelCompatibleSync(sync, stateFallback);
}

// Return the plane index used for typed depth-stencil subresource transitions.
static inline UINT
ri_d3d12_RIBarrierAspectToPlane(enum RIBarrierAspect_e aspect) {
  switch (aspect) {
  case RI_BARRIER_ASPECT_COLOR:
  case RI_BARRIER_ASPECT_DEPTH:
  case RI_BARRIER_ASPECT_DEPTH_STENCIL:
    return 0;
  case RI_BARRIER_ASPECT_STENCIL:
    return 1;
  }
  assert(false);
  return 0;
}

#endif

#endif
