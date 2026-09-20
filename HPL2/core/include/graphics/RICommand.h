#ifndef RI_COMMAND_H
#define RI_COMMAND_H

// Queues, pools, command buffers, and the rect/viewport + rendering/copy
// descriptors they consume. Depends on the prelude + resource leaf headers
// only: RIBarrier.h supplies the barrier types, ScratchBuffer and ri_vk_*
// helpers that RICmd::resourceBarrier's inline body uses, while RIDevice /
// RIRenderer / RIProgram and the BLAS/TLAS descs are forward-declared to keep
// this below RIDevice/RIDescriptor in the include layering.
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
#if (DEVICE_IMPL_D3D12)
static constexpr uint32_t RI_D3D12_VERTEX_STRIDE_COUNT = 16u;
void RID3D12_BindVertexBuffers(struct RICmd &cmd,
                               uint32_t firstBinding, uint32_t count,
                               struct RIBuffer *const *buffers,
                               const uint64_t *offsets,
                               const uint32_t *strides);
void RID3D12_RebindCachedVertexBuffers(struct RICmd &cmd);
void RID3D12_ResourceBarrier(struct RICmd &cmd, uint32_t memoryBarrierNum,
                             const struct RIMemoryBarrier *memoryBarriers,
                             uint32_t bufferBarrierNum,
                             const struct RIBufferBarrier *bufferBarriers,
                             uint32_t textureBarrierNum,
                             const struct RITextureBarrier *textureBarriers);
#endif
static inline bool RIIsTargetSelected(uint8_t targetApi);
struct RIBuildBlasDesc;
struct RIBuildTlasDesc;
struct RICommandRingElement;
namespace hpl {
class RIProgram;
struct RITimeline;
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
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
};

struct RIViewport {
  RIViewport() { memset(this, 0, sizeof(*this)); }
  float x;
  float y;
  float width;
  float height;
  float depthMin;
  float depthMax;
  // RI uses Vulkan-style signed-height viewports. Shared draws use exactly one
  // form, {0, targetHeight, targetWidth, -targetHeight}: width positive, height
  // negative for the Y-flipped/top-left mapping, so y is the far edge. Shaders
  // pair with it by emitting ndc = float2(2,-2)*uv + float2(-1,1).
  //
  // A positive height is Vulkan's native bottom-to-top mapping and has no D3D12
  // equivalent (RSSetViewports takes a positive extent and always maps NDC y=+1
  // to the top), so it renders vertically flipped there; setViewport warns.
  // originBottomLeft requests that form explicitly and D3D12 rejects it rather
  // than silently reorienting -- keep it false for shared draws.
  bool originBottomLeft;
};

struct RIClearValue {
  float color[4];
  float depth;
  uint32_t stencil;
};

// One color or depth/stencil attachment for RICmd::vk_d3d12_beginRendering.
// `view` references the RITextureView abstraction, so one call site serves
// both backends.
struct RIRenderingAttachment {
  struct RITextureView view;
  uint8_t loadOp;  // RIAttachmentLoadOp_e
  uint8_t storeOp; // RIAttachmentStoreOp_e
  // Depth attachment only: bind as DEPTH_READ_ONLY_OPTIMAL (depth-tested but
  // not written) instead of DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
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

#if (DEVICE_IMPL_D3D12)
static constexpr uint32_t RI_D3D12_RTV_DESCRIPTOR_CAPACITY = 256u;
static constexpr uint32_t RI_D3D12_DSV_DESCRIPTOR_CAPACITY = 128u;
static constexpr uint32_t RI_D3D12_MAX_COLOR_ATTACHMENTS = 8u;
struct RID3D12ActiveAttachment {
  ID3D12Resource *resource;
  int32_t x, y;
  uint32_t width, height;
  uint32_t baseMip, baseLayer, layerNum;
  uint8_t loadOp;
  uint8_t storeOp;
  uint8_t stencilLoadOp;
  uint8_t stencilStoreOp;
  bool hasStencil;
  bool depthWritable;
  bool stencilWritable;
};
#endif

struct RIBufferTextureCopyDesc {
  RIDeviceSize bufferOffset;
  // Vulkan reads the texel counts; D3D12 reads the byte pitches and copies
  // with a PLACED_FOOTPRINT source.
  uint32_t bufferRowLength;   // texels (Vulkan VkBufferImageCopy)
  uint32_t bufferImageHeight; // texels (Vulkan VkBufferImageCopy)
  uint32_t bytesPerRow;       // bytes  (D3D12 row pitch)
  uint32_t bytesPerImage;     // bytes  (D3D12 slice pitch)
  uint32_t mipLevel;
  uint32_t arrayLayer;
  int32_t x, y, z;
  uint32_t width, height, depth;
};

// NOTE the field order: the source subresource comes FIRST and the extent
// LAST. A positional `{width, height, 1}` therefore fills srcMipLevel,
// srcArrayLayer and srcX instead, leaving the extent zeroed -- set the fields
// by name. depth defaults to 1 so a 2D copy built with `= {}` is valid.
struct RIImageCopyDesc {
  uint32_t srcMipLevel = 0;
  uint32_t srcArrayLayer = 0;
  int32_t srcX = 0, srcY = 0, srcZ = 0;
  uint32_t dstMipLevel = 0;
  uint32_t dstArrayLayer = 0;
  int32_t dstX = 0, dstY = 0, dstZ = 0;
  uint32_t width = 0, height = 0, depth = 1;
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
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12CommandAllocator *allocator; // owned; released in dispose
      ID3D12CommandQueue *queue;         // borrowed from RIDevice::d3d12.queues[]
      uint8_t type;                      // D3D12_COMMAND_LIST_TYPE captured at init
    } d3d12;
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

  // Leaf dispatch/draw commands: the "go" calls that issue the actual work,
  // with pipeline binding done separately via RIProgram::bind*Pipeline. Each
  // dispatches on RIIsTargetSelected, recording vkCmd* into vk.cmd or the
  // equivalent onto the D3D12 command list.
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
  // [vk/d3d12] The draw count is read from countBuffer at countOffset (a
  // 4-byte-aligned uint32) and clamped to maxDrawCount, so a compute pass can
  // decide how many draws happen without a readback.
  //
  // Requires device->physicalAdapter.isDrawIndirectCountSupported. countBuffer
  // must carry RI_BUFFER_USAGE_INDIRECT and be transitioned to
  // RI_RESOURCE_STATE_INDIRECT_ARGUMENT for RI_STAGE_DRAW_INDIRECT, like the
  // argument buffer.
  void drawIndirectCount(struct RIDevice *device, struct RIBuffer *buffer,
                         RIDeviceSize offset, struct RIBuffer *countBuffer,
                         RIDeviceSize countOffset, uint32_t maxDrawCount,
                         uint32_t stride);

  // [vk/d3d12] Buffer-to-buffer copy.
  void copyBuffer(struct RIDevice *device, struct RIBuffer *src,
                  RIDeviceSize srcOffset, struct RIBuffer *dst,
                  RIDeviceSize dstOffset, RIDeviceSize size);

  // [vk/d3d12] Buffer-to-texture copy of a single subresource region. On D3D12
  // the staging RowPitch must be 256 B aligned and bufferOffset 512 B aligned
  // (D3D12_TEXTURE_DATA_PITCH/PLACEMENT_ALIGNMENT); the resource uploader
  // supplies both. The desc carries Vulkan texel counts alongside D3D12 byte
  // row/slice pitches.
  void copyBufferToTexture(struct RIDevice *device, struct RIBuffer *src,
                           struct RITexture *dst,
                           const struct RIBufferTextureCopyDesc &desc);

  // [vk/d3d12] Image-to-image copy of a single 1:1 region (no scaling).
  // Caller owns the surrounding barriers.
  void copyImage(struct RIDevice *device, struct RITexture *src,
                 struct RITexture *dst, const struct RIImageCopyDesc &desc);

  // [vk] Clear a storage image (mip 0, layer 0) in GENERAL layout.
  // Not implemented on D3D12; the backend asserts.
  void clearStorageImage(struct RIDevice *device, struct RITexture *image,
                         const float color[4]);

  // [vk/d3d12] Dynamic-rendering scope (vkCmdBeginRendering/EndRendering).
  void vk_d3d12_beginRendering(struct RIDevice *device,
                               const struct RIBeginRenderingDesc &desc);
  void vk_d3d12_endRendering(struct RIDevice *device);

  void setViewport(struct RIDevice *device,
                   const struct RIViewport &viewport);
  void setScissor(struct RIDevice *device, const struct RIRect &scissor);

  // [vk/d3d12] Push constants. Stage flags and layout come from the program's
  // reflection, so the call site supplies only the data range.
  //
  // D3D12 ordering: bindPipeline/bindComputePipeline must run first, and since
  // binding a root signature invalidates its root parameters, descriptor and
  // root-signature binds must precede this too. Writes are DWORD-aligned and
  // partial writes touch only the requested reflected subrange; an empty write
  // is an explicit no-op.
  void vk_d3d12_setPushConstants(struct RIDevice *device,
                                 hpl::RIProgram &program, uint32_t offset,
                                 uint32_t size, const void *data);

  // Acceleration-structure builds; numDescs structures per call. scratchBuffer
  // needs RI_BUFFER_USAGE_SCRATCH + RI_BUFFER_USAGE_DEVICE_ADDRESS with
  // scratchOffset at accelerationStructureScratchOffsetAlignment, and the
  // vertex/index/instance inputs need
  // RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT +
  // RI_BUFFER_USAGE_DEVICE_ADDRESS.
  void buildBlas(struct RIDevice *device,
                 const struct RIBuildBlasDesc *descs, uint32_t numDescs);
  void buildTlas(struct RIDevice *device,
                 const struct RIBuildTlasDesc *descs, uint32_t numDescs);

  // Emit pipeline barriers from RI resource-state transitions (see
  // RIBarrier.h). All groups are batched into a single backend barrier command
  // (vkCmdPipelineBarrier2 / RID3D12_ResourceBarrier); any count may be zero.
  // MemN/BufN/TexN are the stack capacities for the barrier scratch arrays; a
  // capacity of 0 moves that group to the heap for dynamically sized batches.
  template <uint32_t MemN, uint32_t BufN, uint32_t TexN>
  void vk_d3d12_resourceBarrier(
      uint32_t memoryBarrierNum, const struct RIMemoryBarrier *memoryBarriers,
      uint32_t bufferBarrierNum, const struct RIBufferBarrier *bufferBarriers,
      uint32_t textureBarrierNum,
      const struct RITextureBarrier *textureBarriers) {
    if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
      return;

#if (DEVICE_IMPL_VULKAN)
    if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
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
      dst.oldLayout =
          ri_vk_RIResourceStateToImageLayout(src.before, src.aspect);
      dst.newLayout =
          ri_vk_RIResourceStateToImageLayout(src.after, src.aspect);
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
    }
  #endif

#if (DEVICE_IMPL_D3D12)
      if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
        RID3D12_ResourceBarrier(*this, memoryBarrierNum, memoryBarriers,
                                 bufferBarrierNum, bufferBarriers,
                                 textureBarrierNum, textureBarriers);
        return;
      }
  #endif
    }

  // Single-barrier conveniences.
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

  // Bind a single index buffer. Takes an RIBuffer* rather than a backend
  // handle so one call site serves both backends.
  void bindIndexBuffer(struct RIDevice *device, struct RIBuffer *buffer,
                       RIDeviceSize offset, enum RIIndexType_e indexType);

  // Bind `count` vertex buffers. The template parameter N is only the stack
  // capacity reserved for the backend handle scratch array (compile-time
  // sized, no heap); `count` is the actual number bound and must be <= N.
  // `buffers` is a raw RIBuffer* array of length `count` (a null entry binds
  // nothing); `offsets` is a parallel byte-offset array. e.g. for a fixed
  // 5-stream layout where all 5 are live: cmd->bindVertexBuffers<5>(0, 5, bufs).
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers,
                         const RIDeviceSize *offsets) {
    assert(count <= N);
    assert(buffers || count == 0);
#if (DEVICE_IMPL_VULKAN)
    if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
      VkBuffer vkBufs[N];
      for (uint32_t i = 0; i < count; ++i)
        vkBufs[i] = buffers[i] ? buffers[i]->vk.buffer : VK_NULL_HANDLE;
      vkCmdBindVertexBuffers(vk.cmd, firstBinding, count, vkBufs, offsets);
      return;
    }
#endif
#if (DEVICE_IMPL_D3D12)
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
      RID3D12_BindVertexBuffers(*this, firstBinding, count, buffers, offsets,
                                d3d12.vertexBindingStrides);
      return;
    }
#endif
    assert(false && "unhandled backend");
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
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12CommandAllocator *allocator; // borrowed from the RIPool; not owned
      ID3D12GraphicsCommandList *cmdList; // owned; released in dispose
      // Obtained via cmdList->QueryInterface at init when supported; nullptr on older runtimes.
      ID3D12GraphicsCommandList7 *cmdList7;
      uint32_t enhancedBarriersSupported : 1;
      // CPU-only heaps used to materialize dynamic-rendering attachments.
      // They are command-owned because descriptors are valid only while this
      // command list is being recorded.
      ID3D12DescriptorHeap *rtvHeap;
      ID3D12DescriptorHeap *dsvHeap;
      D3D12_CPU_DESCRIPTOR_HANDLE rtvStart;
      D3D12_CPU_DESCRIPTOR_HANDLE dsvStart;
      uint32_t rtvDescriptorSize;
      uint32_t dsvDescriptorSize;
      uint32_t rtvCount;
      uint32_t dsvCount;
      uint32_t activeColorCount;
      bool activeDepth;
      // Last D3D12 root-signature binding kind; selects Set*Root32BitConstants.
      bool computePipelineBound;
      // Root signature currently bound at each bind point. Setting a root
      // signature invalidates every root argument, so a redundant set has to
      // be elided: passes that bind descriptors once and then call
      // bindPipeline per draw would otherwise lose their descriptor tables on
      // every iteration after the first.
      ID3D12RootSignature *boundGraphicsRootSignature;
      ID3D12RootSignature *boundComputeRootSignature;
      // Shader-visible heaps currently bound. Changing heaps also invalidates
      // descriptor-table root arguments, so the same elision applies.
      ID3D12DescriptorHeap *boundResourceHeap;
      ID3D12DescriptorHeap *boundSamplerHeap;
      // Root-argument bookkeeping, used by the debug-only draw-time check that
      // catches a draw whose program needs a root parameter nothing has
      // written since the last invalidation. Kept out of any #if so a debug
      // and a release translation unit cannot disagree about RICmd's layout.
      // "Set" is every parameter written; "Table" is the subset that is a
      // descriptor table, which a descriptor-heap change invalidates while
      // leaving root constants intact. "Required" comes from the bound
      // program; "Reported" de-duplicates the diagnostic.
      uint64_t graphicsRootArgsSet;
      uint64_t computeRootArgsSet;
      uint64_t graphicsRootTableArgs;
      uint64_t computeRootTableArgs;
      uint64_t graphicsRootArgsRequired;
      uint64_t computeRootArgsRequired;
      uint64_t rootArgsReported;
      const char *boundPipelineDebugName;
      RIDevice *device;
      uint32_t vertexBindingCount;
      uint32_t vertexBindingStrides[RI_D3D12_VERTEX_STRIDE_COUNT];
      uint32_t vertexBufferCacheValidMask;
      D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
      struct RID3D12ActiveAttachment activeColors[RI_D3D12_MAX_COLOR_ATTACHMENTS];
      struct RID3D12ActiveAttachment activeDepthAttachment;
    } d3d12;
#endif
  };
};

// One wait or signal edge in an RISubmitDesc: (timeline, value, stages). The
// stages field is an RIStageBits_e mask (RI_STAGE_NONE means "any / all") and
// controls the Vulkan pipeline-stage translation for VkSemaphoreSubmitInfo.
// D3D12 ignores the stage bits (queue Wait/Signal are stage-agnostic).
struct RITimelineOp {
  hpl::RITimeline *timeline; // required
  uint64_t value;            // returned earlier by timeline->next()
  uint32_t stages;           // RIStageBits_e mask; 0 => ALL_COMMANDS
};

// Backend-neutral submit description. cmds contains the command buffers to
// execute in order; waits and signals name timeline edges; completion (nullable)
// stamps a completion token onto the given command-ring element after a
// successful submit — the callers use it to gate allocator reuse.
struct RISubmitDesc {
  RICmd *const *cmds;
  uint32_t cmdCount;
  const RITimelineOp *waits;
  uint32_t waitCount;
  const RITimelineOp *signals;
  uint32_t signalCount;
  struct RICommandRingElement *completion;
};

struct RIQueue {
  RIQueue() { memset(this, 0, sizeof(*this)); }
  void waitIdle(struct RIDevice *device);
  // Submit an ordered batch of cmds with timeline waits/signals. D3D12 issues
  // queue->Wait, ExecuteCommandLists, queue->Signal in that order; Vulkan emits
  // one vkQueueSubmit2 with translated stage masks. A non-null desc.completion
  // has its fence signaled after a successful submit, and the ring element
  // captures fence + value so element.wait() blocks on it.
  enum RIResult_e submit(struct RIDevice *device, const struct RISubmitDesc &desc);
  uint32_t getFlags(const struct RIRenderer *renderer) const {
#if (DEVICE_IMPL_VULKAN)
    if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
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
    }
#endif
#if (DEVICE_IMPL_D3D12)
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
      // Flags are set at queue creation from D3D12_COMMAND_LIST_TYPE.
      return d3d12.flags;
    }
#endif
    (void)renderer;
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
#if (DEVICE_IMPL_D3D12)
    struct {
      ID3D12CommandQueue *queue; // borrowed from RIDevice::d3d12.queues[i]; not owned here
      uint8_t type;              // D3D12_COMMAND_LIST_TYPE (DIRECT/COMPUTE/COPY)
      uint8_t flags;             // RI_QUEUE_*_BIT bitmask (derived from `type` at creation)
      // Shared by RID3D12_QueueWaitIdle, RIRenderer completion-fence stamping
      // via RID3D12SubmitDesc::completionFence, and RISwapchainPresent; all
      // increment nextFenceValue monotonically.
      ID3D12Fence *fence;        // owned; RID3D12_InitDevice creates it, RID3D12_DisposeDevice releases
      HANDLE fenceEvent;         // manual-reset Win32 event signaled by the fence for waits
      uint64_t nextFenceValue;   // monotonic across all three users
    } d3d12;
#endif
  };
};

#endif // RI_COMMAND_H
