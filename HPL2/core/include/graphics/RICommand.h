#ifndef RI_COMMAND_H
#define RI_COMMAND_H

// Queues, pools, command buffers, and the rect/viewport + rendering/copy
// descriptors they consume — grouped by use case (mirrors ref_nri/ri_command.h).
// Depends on the prelude + resource leaf headers only. RIBarrier.h provides the
// barrier types, the ScratchBuffer template and the ri_vk_* helpers that RICmd's
// inline resourceBarrier template body uses; RIDevice / RIRenderer / RIProgram
// and the BLAS/TLAS build descs are referenced by pointer/reference only, so a
// forward declaration is enough (keeps this header below RIDevice/RIDescriptor
// in the include layering).
#include "graphics/RIPreamble.h"
#include "graphics/RIBarrier.h"     // RIMemoryBarrier/…, ScratchBuffer, ri_vk_*
#include "graphics/RIBuffer.h"      // RIBuffer (bind / barrier / copy)
#include "graphics/RITexture.h"     // RITexture (copy / barrier)
#include "graphics/RITextureView.h" // RITextureView (RIRenderingAttachment)
#include "graphics/RIPipeline.h"    // RIIndexType_e (bindIndexBuffer)
#include <cassert>
#include <cstring>

struct RIDevice;
struct RIRenderer;
struct RIBuildBlasDesc;
struct RIBuildTlasDesc;
namespace hpl {
class RIProgram;
}

enum RIQueueType_e {
  RI_QUEUE_GRAPHICS,
  RI_QUEUE_COMPUTE,
  RI_QUEUE_COPY,
  RI_QUEUE_LEN
};

typedef uint64_t RIDeviceSize;

enum RIAttachmentLoadOp_e {
  RI_ATTACHMENT_LOAD_OP_LOAD,
  RI_ATTACHMENT_LOAD_OP_CLEAR,
  RI_ATTACHMENT_LOAD_OP_DONT_CARE,
};

enum RIAttachmentStoreOp_e {
  RI_ATTACHMENT_STORE_OP_STORE,
  RI_ATTACHMENT_STORE_OP_DONT_CARE,
};

struct RIRect {
  RIRect() { memset(this, 0, sizeof(*this)); }
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

struct RIViewport {
  RIViewport() { memset(this, 0, sizeof(*this)); }
  float x;
  float y;
  float width;
  float height;
  float depthMin;
  float depthMax;
  bool originBottomLeft; // expects "isViewportOriginBottomLeftSupported"
};

struct RIClearValue {
  float color[4];
  float depth;
  uint32_t stencil;
};

// One color or depth/stencil attachment for RICmd::vk_d3d12_beginRendering /
// mtl_encoderDraw. `view` references the RITextureView abstraction
// (vk.image / mtl.view), so the same call site works on either backend.
struct RIRenderingAttachment {
  struct RITextureView view;
  uint8_t loadOp;  // RIAttachmentLoadOp_e
  uint8_t storeOp; // RIAttachmentStoreOp_e
  // Depth attachment only: bind as DEPTH_READ_ONLY_OPTIMAL (depth-tested but
  // not written) instead of DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Ignored by Metal,
  // which expresses read-only depth through the pipeline's depth-stencil state.
  bool readOnly;
  // Depth/stencil attachment: when the view's format carries a stencil aspect,
  // set hasStencil to also bind it as a stencil attachment with its own
  // load/store (a pass may load depth but clear stencil). clearValue.stencil
  // supplies the clear. When hasStencil, the depth aspect binds as
  // DEPTH_ATTACHMENT_OPTIMAL and the stencil aspect as
  // STENCIL_ATTACHMENT_OPTIMAL.
  bool hasStencil;
  uint8_t stencilLoadOp;  // RIAttachmentLoadOp_e
  uint8_t stencilStoreOp; // RIAttachmentStoreOp_e
  struct RIClearValue clearValue;
};

struct RIBeginRenderingDesc {
  struct RIRect renderArea;
  uint32_t colorCount;
  const struct RIRenderingAttachment *colors;
  const struct RIRenderingAttachment *depthStencil; // nullable
};

struct RIBufferTextureCopyDesc {
  RIDeviceSize bufferOffset;
  uint32_t bufferRowLength;   // texels (Vulkan VkBufferImageCopy)
  uint32_t bufferImageHeight; // texels (Vulkan VkBufferImageCopy)
  uint32_t bytesPerRow;       // bytes  (Metal copyFromBuffer)
  uint32_t bytesPerImage;     // bytes  (Metal copyFromBuffer)
  uint32_t mipLevel;
  uint32_t arrayLayer;
  int32_t x, y, z;
  uint32_t width, height, depth;
};

struct RIImageCopyDesc {
  uint32_t srcMipLevel;
  uint32_t srcArrayLayer;
  int32_t srcX, srcY, srcZ;
  uint32_t dstMipLevel;
  uint32_t dstArrayLayer;
  int32_t dstX, dstY, dstZ;
  uint32_t width, height, depth;
};

struct RIPool {
  RIPool() { memset(this, 0, sizeof(*this)); }
  // Creates the command pool on the queue's family.
  void init(struct RIDevice *device, struct RIQueue *queue);
  // Resets every command buffer allocated from the pool.
  void reset(struct RIDevice *device);
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueue queue;
      VkCommandPool pool;
    } vk;
#endif
  };
};

struct RICmd {
  RICmd() { memset(this, 0, sizeof(*this)); }

  // Captured at init so every barrier side uses the same logical-device
  // feature set, including command recording after device queries change.
  RIBarrierCapabilities barrierCapabilities;

  // Allocates the command buffer from the pool.
  void init(struct RIDevice *device, struct RIPool *pool);
  // Begins/ends recording (one-time-submit).
  void begin(struct RIDevice *device);
  void end(struct RIDevice *device);
  // Returns the command buffer to its pool and clears the handles.
  void dispose(struct RIDevice *device);
  bool isEmpty() const;

  // Leaf dispatch/draw command methods. Pipeline binding is done separately
  // (RIProgram::bindPipeline / bindComputePipeline / bindRayTracingPipeline);
  // these are the "go" calls that issue the actual work. The device's renderer
  // selects the active backend (is_target_selected); on Metal these route
  // through the open encoder, on Vulkan they record vkCmd* into vk.cmd.
  void dispatch(struct RIDevice *device, uint32_t groupCountX,
                uint32_t groupCountY, uint32_t groupCountZ);
  void dispatchIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                        RIDeviceSize offset);
  void draw(struct RIDevice *device, uint32_t vertexCount,
            uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance);
  void drawIndexed(struct RIDevice *device, uint32_t indexCount,
                   uint32_t instanceCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance);
  void drawIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                    RIDeviceSize offset, uint32_t drawCount, uint32_t stride);
  void drawIndexedIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                           RIDeviceSize offset, uint32_t drawCount,
                           uint32_t stride);

  // [vk/mtl] Buffer-to-buffer copy. Vulkan records vkCmdCopyBuffer; Metal opens
  // a blit encoder and calls copyFromBuffer.
  void copyBuffer(struct RIDevice *device, struct RIBuffer *src,
                  RIDeviceSize srcOffset, struct RIBuffer *dst,
                  RIDeviceSize dstOffset, RIDeviceSize size);

  // [vk/mtl] Buffer-to-texture copy of a single subresource region. The desc
  // carries the staging layout in both texel (Vulkan) and byte (Metal) form.
  void copyBufferToTexture(struct RIDevice *device, struct RIBuffer *src,
                           struct RITexture *dst,
                           const struct RIBufferTextureCopyDesc &desc);

  // [vk/mtl] Image-to-image copy of a single 1:1 region (no scaling). Caller
  // owns the surrounding barriers.
  void copyImage(struct RIDevice *device, struct RITexture *src,
                 struct RITexture *dst, const struct RIImageCopyDesc &desc);

  // [vk/mtl] Clear a storage image (mip 0, layer 0) in GENERAL layout.
  void clearStorageImage(struct RIDevice *device, struct RITexture *image,
                         const float color[4]);

  // [vk/d3d12] Dynamic-rendering scope (vkCmdBeginRendering/EndRendering).
  // Metal uses mtl_encoderDraw / mtl_encoderEnd instead (kept as separate
  // APIs).
  void vk_d3d12_beginRendering(struct RIDevice *device,
                               const struct RIBeginRenderingDesc &desc);
  void vk_d3d12_endRendering(struct RIDevice *device);

  void setViewport(struct RIDevice *device,
                   const struct RIViewport &viewport);
  void setScissor(struct RIDevice *device, const struct RIRect &scissor);

  // [vk/d3d12] Push constants. Metal supplies the same data inline via the
  // [[buffer(0)]] push-constant block (setBytes) at bind/draw time, so it has
  // no discrete command here (vk_d3d12_-prefixed, like beginRendering/barriers).
  // The stage flags and layout come from the program's reflection, so the call
  // site only supplies the data range.
  void vk_d3d12_setPushConstants(struct RIDevice *device,
                                 hpl::RIProgram &program, uint32_t offset,
                                 uint32_t size, const void *data);

  // Acceleration-structure build commands; numDescs structures are submitted
  // in a single backend call. Caller-supplied scratchBuffer must include
  // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, with scratchOffset aligned to
  // minAccelerationStructureScratchOffsetAlignment. Input vertex/index/
  // instance buffers must include
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. Takes the device, which supplies
  // VMA / device address / scratch alignment; the active backend comes from
  // RIIsTargetSelected.
  void buildBlas(struct RIDevice *device,
                 const struct RIBuildBlasDesc *descs, uint32_t numDescs);
  void buildTlas(struct RIDevice *device,
                 const struct RIBuildTlasDesc *descs, uint32_t numDescs);

  // Emit pipeline barriers from RI resource-state transitions (see
  // RIBarrier.h). All groups are batched into a single backend barrier
  // command (vkCmdPipelineBarrier2); any count may be zero. The template
  // parameters MemN/BufN/TexN are the stack capacities reserved for the
  // backend barrier scratch arrays (compile-time sized); a capacity of 0
  // moves that group to the heap instead, for dynamically sized batches.
  template <uint32_t MemN, uint32_t BufN, uint32_t TexN>
  void vk_d3d12_resourceBarrier(
      uint32_t memoryBarrierNum, const struct RIMemoryBarrier *memoryBarriers,
      uint32_t bufferBarrierNum, const struct RIBufferBarrier *bufferBarriers,
      uint32_t textureBarrierNum,
      const struct RITextureBarrier *textureBarriers) {
    if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
      return;

#if (DEVICE_IMPL_VULKAN)
    ScratchBuffer<VkMemoryBarrier2, MemN> memScratch;
    ScratchBuffer<VkBufferMemoryBarrier2, BufN> bufScratch;
    ScratchBuffer<VkImageMemoryBarrier2, TexN> imgScratch;
    VkMemoryBarrier2 *mem = memScratch.get(memoryBarrierNum);
    VkBufferMemoryBarrier2 *buf = bufScratch.get(bufferBarrierNum);
    VkImageMemoryBarrier2 *img = imgScratch.get(textureBarrierNum);

    for (uint32_t i = 0; i < memoryBarrierNum; i++) {
      const struct RIMemoryBarrier &src = memoryBarriers[i];
      VkMemoryBarrier2 &dst = mem[i];
      dst = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      bool valid = true;
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported memory barrier source state/stage\n");
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported memory barrier destination state/stage\n");
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
    }

    for (uint32_t i = 0; i < bufferBarrierNum; i++) {
      const struct RIBufferBarrier &src = bufferBarriers[i];
      VkBufferMemoryBarrier2 &dst = buf[i];
      dst = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      bool valid = true;
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported buffer barrier source state/stage\n");
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported buffer barrier destination state/stage\n");
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.buffer = src.buffer->vk.buffer;
      dst.offset = src.offset;
      dst.size = src.size ? src.size : VK_WHOLE_SIZE;
    }

    for (uint32_t i = 0; i < textureBarrierNum; i++) {
      const struct RITextureBarrier &src = textureBarriers[i];
      VkImageMemoryBarrier2 &dst = img[i];
      dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      bool valid = true;
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported image barrier source state/stage\n");
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after, barrierCapabilities, &valid);
      if (!valid)
        hpl::FatalError("RI: unsupported image barrier destination state/stage\n");
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.oldLayout = ri_vk_RIResourceStateToImageLayout(src.before);
      dst.newLayout = ri_vk_RIResourceStateToImageLayout(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.image = src.texture->vk.image;
      dst.subresourceRange = VkImageSubresourceRange{
          ri_vk_RIBarrierAspectToVK(src.aspect),
          src.baseMip,
          src.mipCount ? src.mipCount : VK_REMAINING_MIP_LEVELS,
          src.baseLayer,
          src.layerCount ? src.layerCount : VK_REMAINING_ARRAY_LAYERS,
      };
    }

    VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.memoryBarrierCount = memoryBarrierNum;
    dependencyInfo.pMemoryBarriers = mem;
    dependencyInfo.bufferMemoryBarrierCount = bufferBarrierNum;
    dependencyInfo.pBufferMemoryBarriers = buf;
    dependencyInfo.imageMemoryBarrierCount = textureBarrierNum;
    dependencyInfo.pImageMemoryBarriers = img;
    vkCmdPipelineBarrier2(vk.cmd, &dependencyInfo);
#endif
  }

  // Single-barrier conveniences (vk_d3d12_-prefixed: no-op on Metal, which
  // tracks hazards automatically).
  void vk_d3d12_memoryBarrier(const struct RIMemoryBarrier &barrier) {
    vk_d3d12_resourceBarrier<1, 0, 0>(1, &barrier, 0, NULL, 0, NULL);
  }
  void vk_d3d12_bufferBarrier(const struct RIBufferBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 1, 0>(0, NULL, 1, &barrier, 0, NULL);
  }
  void vk_d3d12_textureBarrier(const struct RITextureBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 0, 1>(0, NULL, 0, NULL, 1, &barrier);
  }
  // Fixed-capacity texture-batch convenience; N is the stack capacity.
  template <uint32_t N>
  void vk_d3d12_textureBarriers(uint32_t num,
                                const struct RITextureBarrier *barriers) {
    vk_d3d12_resourceBarrier<0, 0, N>(0, NULL, 0, NULL, num, barriers);
  }

  // Bind a single index buffer. Takes an RIBuffer* (the RI abstraction)
  // rather than a backend handle so the same call site survives a future
  // DX12 backend.
  void bindIndexBuffer(struct RIDevice *device, struct RIBuffer *buffer,
                       RIDeviceSize offset, enum RIIndexType_e indexType);

  // Bind `count` vertex buffers. The template parameter N is only the stack
  // capacity reserved for the backend handle scratch array (compile-time
  // sized, no heap); `count` is the actual number bound and must be <= N.
  // `buffers` is a raw RIBuffer* array of length `count` (a null entry
  // binds nothing); `offsets` is a parallel byte-offset array. e.g. for a
  // fixed 5-stream layout where all 5 are live:
  // cmd->bindVertexBuffers<5>(0, 5, bufs). RIBuffer* keeps the call site
  // backend-agnostic for the planned DX12 path.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers,
                         const RIDeviceSize *offsets) {
    assert(count <= N);
#if (DEVICE_IMPL_VULKAN)
    VkBuffer vkBufs[N];
    for (uint32_t i = 0; i < count; ++i)
      vkBufs[i] = buffers[i] ? buffers[i]->vk.buffer : VK_NULL_HANDLE;
    vkCmdBindVertexBuffers(vk.cmd, firstBinding, count, vkBufs, offsets);
#endif
#if (DEVICE_IMPL_MTL)
    // Streams bind at the top of the buffer table (RI_MTL_VertexBufferIndex) —
    // the same mapping the pipeline's MTLVertexDescriptor uses. A null entry
    // binds nothing.
    assert(mtl.render && "bindVertexBuffers requires an open render encoder");
    for (uint32_t i = 0; i < count; ++i)
      if (buffers[i])
        mtl.render->setVertexBuffer(buffers[i]->mtl.buffer,
                                    (NS::UInteger)offsets[i],
                                    RI_MTL_VertexBufferIndex(firstBinding + i));
#endif
  }

  // Convenience overload binding `count` streams at offset 0.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers) {
    const RIDeviceSize offsets[N] = {};
    bindVertexBuffers<N>(firstBinding, count, buffers, offsets);
  }

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkCommandPool pool;
      VkCommandBuffer cmd;
    } vk;
#endif
  };
};

struct RIQueue {
  RIQueue() { memset(this, 0, sizeof(*this)); }
  void waitIdle(struct RIDevice *device);
  uint32_t getFlags(const struct RIRenderer *renderer) const {
#if (DEVICE_IMPL_VULKAN)
    return (vk.queueFlags & VK_QUEUE_GRAPHICS_BIT ? RI_QUEUE_GRAPHICS_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_COMPUTE_BIT ? RI_QUEUE_COMPUTE_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_TRANSFER_BIT ? RI_QUEUE_TRANSFER_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT
                ? RI_QUEUE_SPARSE_BINDING_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR
                ? RI_QUEUE_VIDEO_DECODE_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR
                ? RI_QUEUE_VIDEO_ENCODE_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_PROTECTED_BIT ? RI_QUEUE_PROTECTED_BIT
                                                   : 0) |
           (vk.queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV
                ? RI_QUEUE_OPTICAL_FLOW_BIT_NV
                : 0);
#endif
    return 0;
  }
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueueFlags queueFlags;
      uint16_t queueFamilyIdx;
      uint16_t slotIdx;
      VkQueue queue;
    } vk;
#endif
  };
};

#endif // RI_COMMAND_H
