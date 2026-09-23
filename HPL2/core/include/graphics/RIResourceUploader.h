#ifndef RI_RESOURCE_UPLOAD_H
#define RI_RESOURCE_UPLOAD_H

#define RI_RESOURCE_MAX_SETS 2
#define RI_RESOURCE_STAGE_BUFFER_SIZE (8 * MB_TO_BYTE)

#include "RITypes.h"
#include "graphics/RITimeline.h"
#if (DEVICE_IMPL_D3D12)
#include "graphics/RID3D12MipGeneration.h"
#endif

// Slice of mapped staging memory handed back to a transaction.
struct RIMappedMemoryRange {
  size_t offset;
  size_t size;
  void *data;
  // Snapshot of the source staging buffer. The resource remains owned by the
  // transfer group's staging/temporary buffer storage.
  struct RIBuffer buffer;
};

// One queue + per-set cmd pool / cmd buffer / staging buffer / fence /
// semaphore.
struct RITransferCommandGroup {
  struct RIQueue *queue;

  bool is_recording;
  size_t active_set;

  struct RIPool cmd_pool[RI_RESOURCE_MAX_SETS];
  struct RICmd cmd[RI_RESOURCE_MAX_SETS];

  size_t staging_buffer_offset; // running tail in active set's staging buffer
  struct RIBuffer staging_buffer[RI_RESOURCE_MAX_SETS];

  // per-set overflow buffers (stb_ds arrays); freed when the set is reused
  struct RIBuffer *temporary_buffers[RI_RESOURCE_MAX_SETS];

#if (DEVICE_IMPL_D3D12)
  hpl::RITimeline d3d12_timeline;
  uint64_t d3d12_set_signal_values[RI_RESOURCE_MAX_SETS];
  RID3D12MipGeneration *d3d12_mip_owner;
#endif

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkFence fences[RI_RESOURCE_MAX_SETS];
      VkSemaphore semaphores[RI_RESOURCE_MAX_SETS];
    } vk;
#endif
  };
};

struct RIResourceUploader {
  struct RITransferCommandGroup upload_resource; // graphics queue
  struct RITransferCommandGroup copy_resource;   // transfer / copy queue
#if (DEVICE_IMPL_D3D12)
  RID3D12MipGeneration d3d12_mip_generation;
#endif
};

struct RIResourceBufferTransaction {
  struct RIBuffer target;
  size_t size;
  size_t offset; // destination offset inside 'target'

  // State the target is in before the copy and the state to return it to
  // afterwards (RIBarrier.h). currentState == COPY_DST skips the pre-barrier;
  // postState == UNDEFINED or COPY_DST skips the post-barrier. Stage hints
  // follow the usual rule: 0 derives a conservative mask from the state.
  enum RIResourceState_e currentState;
  uint32_t currentStages; // RIStageBits_e
  enum RIResourceState_e postState;
  uint32_t postStages; // RIStageBits_e

  // filled by RI_ResourceBeginCopyBuffer
  struct RIMappedMemoryRange mapped;
};

struct RIResourceTextureTransaction {
  struct RITexture target;

  // Same semantics as the buffer transaction states above; barriers cover
  // only the single (mipOffset, arrayOffset) subresource being written.
  enum RIResourceState_e currentState;
  uint32_t currentStages; // RIStageBits_e
  enum RIResourceState_e postState;
  uint32_t postStages; // RIStageBits_e

  uint32_t format; // RI_Format_e
  uint32_t sliceNum;
  uint32_t rowPitch;

  uint16_t x;
  uint16_t y;
  uint16_t z;
  uint32_t width;
  uint32_t height;
  uint32_t depth;

  uint32_t arrayOffset;
  uint32_t mipOffset;

  // filled by RI_ResourceBeginCopyTexture
  uint32_t alignRowPitch;
  uint32_t alignSlicePitch;
  struct RIMappedMemoryRange mapped;
};

struct RIGenerateMipsDesc {
  struct RITexture target;

  uint32_t format; // RI_Format_e
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t mipNum;
  uint32_t arrayOffset;
  uint32_t layerNum;

  // State of mip 0 before generation and state of all mips afterwards.
  enum RIResourceState_e currentState;
  uint32_t currentStages; // RIStageBits_e
  enum RIResourceState_e postState;
  uint32_t postStages; // RIStageBits_e
};

void RI_InitResourceUploader(struct RIDevice *device,
                             struct RIResourceUploader *res);
void RI_FreeResourceUploader(struct RIDevice *device,
                             struct RIResourceUploader *res);

void RI_ResourceBeginCopyBuffer(struct RIDevice *device,
                                struct RIResourceUploader *res,
                                struct RIResourceBufferTransaction *trans);
void RI_ResourceEndCopyBuffer(struct RIDevice *device,
                              struct RIResourceUploader *res,
                              struct RIResourceBufferTransaction *trans);

void RI_ResourceBeginCopyTexture(struct RIDevice *device,
                                 struct RIResourceUploader *res,
                                 struct RIResourceTextureTransaction *trans);
void RI_ResourceEndCopyTexture(struct RIDevice *device,
                               struct RIResourceUploader *res,
                               struct RIResourceTextureTransaction *trans);

bool RI_FormatSupportsMipGeneration(struct RIDevice *device,
                                    uint32_t format /* RI_Format_e */);

// vkCmdBlitImage performs the format's own texel conversion on read and write:
// for an sRGB VkFormat it decodes source texels to linear, filters linearly,
// and re-encodes to sRGB. That is correct; a naive CPU box filter over sRGB-
// encoded bytes would darken every mip.
void RI_ResourceGenerateMips(struct RIDevice *device,
                             struct RIResourceUploader *res,
                             struct RIGenerateMipsDesc *desc);

struct RIResourceUploaderVKResult {
  bool signaled;

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkFence fence;
      VkSemaphore semaphore;
    } vk;
#endif
  };
};

#if (DEVICE_IMPL_VULKAN)
struct RIResourceUploaderVKResult
RI_VKFlushResourceUpdate(struct RIDevice *device,
                         struct RIResourceUploader *res, size_t num_semaphores,
                         VkSemaphoreSubmitInfo *wait_semaphore_info);
#endif

struct RIResourceUploaderD3D12Result {
  bool success;
  bool signaled;
  hpl::RITimeline *timeline;
  uint64_t value;
};

#if (DEVICE_IMPL_D3D12)
struct RIResourceUploaderD3D12Result
RI_D3D12FlushResourceUpdate(struct RIDevice *device,
                            struct RIResourceUploader *res);
#endif

#endif
