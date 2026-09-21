#ifndef RI_D3D12_H
#define RI_D3D12_H

// DirectX 12 backend entry points, called from the runtime-dispatched lifecycle
// paths in RIRenderer.cpp when the active backend is RI_DEVICE_API_D3D12. Only
// visible when DEVICE_SUPPORT_D3D12 is defined by the premake build.
#include "graphics/RIPreamble.h"
#include "graphics/RIPipeline.h" // RIBlendFactor_e, RICompareFunc_e, ... (ri_d3d12_*ToD3D12)

#include <atomic>
#include <cassert>

#if DEVICE_IMPL_D3D12

static constexpr uint32_t RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY = 32768u;

struct RIRenderer;
struct RIPhysicalAdapter;
struct RIDevice;
struct RIBackendInit;
struct RIDeviceDesc;
struct RIBufferDesc;
struct RIBuffer;
struct RICmd;
struct RIMemoryBarrier;
struct RIBufferBarrier;
struct RITextureBarrier;

struct RIMemoryStats;

extern uint32_t g_riD3D12EnhancedBarrierCallCount;
extern uint32_t g_riD3D12LegacyBarrierCallCount;
// Live RIProgram D3D12 descriptor-cache entries summed over every program.
// Each entry holds references on the resources it last bound, so steady growth
// here means memory that graphicsDefer released is still pinned.
extern std::atomic<uint32_t> g_riD3D12DescriptorCacheEntries;
bool RID3D12_QueryMemoryStats(const struct RIDevice &device,
                              struct RIMemoryStats *out);
void RID3D12_ResourceBarrier(struct RICmd &cmd, uint32_t memoryBarrierNum,
                             const struct RIMemoryBarrier *memoryBarriers,
                             uint32_t bufferBarrierNum,
                             const struct RIBufferBarrier *bufferBarriers,
                             uint32_t textureBarrierNum,
                             const struct RITextureBarrier *textureBarriers);

// Test-only knob. When true, RID3D12_InitRenderer enables the debug layer and
// GPU-based validation; when false the debug layer is left off. Callers set
// this AFTER `RIBackendInit` is prepared but BEFORE calling InitRIRenderer, so
// keep it an ordinary global here rather than a bit in RIBackendInit.
extern bool g_riD3D12EnableDebugLayer;

int RID3D12_InitRenderer(struct RIRenderer &renderer, const struct RIBackendInit *init);
void RID3D12_ShutdownRenderer(struct RIRenderer &renderer);
int RID3D12_EnumerateAdapters(struct RIRenderer &renderer, struct RIPhysicalAdapter *adapters, uint32_t *numAdapters);
int RID3D12_InitDevice(struct RIDevice &device, const struct RIDeviceDesc *init);
void RID3D12_DisposeDevice(struct RIDevice &device);
// Releases the D3D12 buffer-registration quarantine after the device queues
// have been drained.  Buffer disposal cannot provide a submission fence in
// the current API, so the registry owns its extra native references until
// this device-destroy point.
void RID3D12_DrainBufferRegistry(struct RIDevice &device);
// Frame-timeline reclaim for the same quarantine. Seal stamps every retired,
// unsealed registration with the timeline value the current frame's submission
// signals; Reclaim releases the native references of every sealed
// registration whose value has completed. Same contract as FrameDeferral.
void RID3D12_SealRetiredBuffers(struct RIDevice &device,
                                uint64_t timelineValue);
void RID3D12_ReclaimRetiredBuffers(struct RIDevice &device,
                                   uint64_t completedValue);
void RID3D12_BufferRegistryStats(const struct RIDevice &device,
                                 uint32_t *registered, uint64_t *retiredBytes);
// Drains pending debug-layer messages from the device's ID3D12InfoQueue to
// stderr when the callback path (ID3D12InfoQueue1) is unavailable. Safe to
// call unconditionally; a no-op when the debug layer is off or the
// callback-based path already handled the messages.
void RID3D12_DrainDeviceMessages(struct RIDevice &device);
// Returns false while the device is healthy. On device removal, logs the
// removal reason, pending debug-layer messages and (when enabled) DRED
// breadcrumbs / page-fault allocations, then raises FatalError. `where` names
// the call site that observed the loss.
bool RID3D12_CheckDeviceRemoved(struct RIDevice &device, const char *where);
bool RID3D12_DeviceIsValid(const struct RIDevice &device);
int RID3D12_CreateBuffer(struct RIDevice &device, const struct RIBufferDesc &desc, struct RIBuffer &out);
void RID3D12_DisposeBuffer(struct RIDevice &device, struct RIBuffer &buffer);
void RID3D12_SetBufferDebugName(struct RIDevice &device, struct RIBuffer &buffer, const char *name);
uint64_t RID3D12_BufferGpuAddress(struct RIDevice &device, struct RIBuffer &buffer);
// Registers a buffer in the device-owned raw ByteAddressBuffer table
// (t4, space3), creating its SRV and assigning a stable slot. The separate
// register space is required because HLSL requires an unbounded range to be
// the last range in a register space.
bool RID3D12_RegisterBufferShaderResource(struct RIDevice &device,
                                          struct RIBuffer &buffer);
// Explicit override for integrations that own the table. The value is the
// raw SRV index; GetShaderResourceHandle performs the packed encoding.
void RID3D12_SetBufferShaderResourceIndex(struct RIBuffer &buffer,
                                          uint32_t descriptorIndex);
void RID3D12_FlushBufferRange(struct RIDevice &device, struct RIBuffer &buffer, uint64_t offset, uint64_t size);
void RID3D12_InvalidateBufferRange(struct RIDevice &device, struct RIBuffer &buffer, uint64_t offset, uint64_t size);
bool RID3D12_BufferIsEmpty(const struct RIBuffer &buffer);

struct RIPool;
struct RICmd;
struct RIQueue;

// Command pool lifecycle. RIPool::init/reset/dispose in RIRenderer.cpp
// forward to these when the active backend is D3D12.
void RID3D12_PoolInit(struct RIDevice &device, struct RIPool &pool, struct RIQueue &queue);
void RID3D12_PoolReset(struct RIDevice &device, struct RIPool &pool);
void RID3D12_PoolDispose(struct RIDevice &device, struct RIPool &pool);

// Command list lifecycle. RICmd::init/begin/end/dispose in RIRenderer.cpp
// forward to these when the active backend is D3D12.
void RID3D12_CmdInit(struct RIDevice &device, struct RICmd &cmd, struct RIPool &pool);
void RID3D12_CmdBegin(struct RIDevice &device, struct RICmd &cmd);
void RID3D12_CmdEnd(struct RIDevice &device, struct RICmd &cmd);
void RID3D12_CmdDispose(struct RIDevice &device, struct RICmd &cmd);

// Root-signature and descriptor-heap binds, filtered against what the command
// list already has bound. Both invalidate every descriptor-table root
// argument when they take effect, so every bind must go through these rather
// than calling the command list directly -- otherwise a pass that binds
// descriptors once and then binds a pipeline per draw loses its tables.
void RID3D12_SetGraphicsRootSignature(struct RICmd &cmd,
                                      ID3D12RootSignature *rootSignature);
void RID3D12_SetComputeRootSignature(struct RICmd &cmd,
                                     ID3D12RootSignature *rootSignature);
void RID3D12_SetDescriptorHeaps(struct RICmd &cmd,
                                ID3D12DescriptorHeap *resourceHeap,
                                ID3D12DescriptorHeap *samplerHeap);

// Drop every redundancy cache above, without touching the command list. Used
// where the command list's own bindings are known to be gone, such as after a
// Reset.
void RID3D12_InvalidateCachedBindings(struct RICmd &cmd);

// Put the command list back under engine control after third-party code was
// handed the raw ID3D12GraphicsCommandList. ffx-api and XeSS both call
// SetDescriptorHeaps and SetComputeRootSignature on it directly, which leaves
// it bound to the SDK's heap and signature while the caches above still name
// the engine's -- the next engine bind would be elided as redundant and the
// dispatch would read descriptors computed against a heap that is no longer
// bound. This drops those caches and re-binds the descriptor arena, restoring
// the same state RID3D12_CmdBegin establishes. Call it immediately after any
// such dispatch returns, on every path that reached the dispatch.
void RID3D12_RestoreCachedBindings(struct RIDevice &device, struct RICmd &cmd);

// Root-argument writes. Going through these keeps the command list's record
// of which root parameters are live, which RID3D12_CheckRootArguments reads.
void RID3D12_SetGraphicsRootDescriptorTable(struct RICmd &cmd, uint32_t parameter,
                                            D3D12_GPU_DESCRIPTOR_HANDLE handle);
void RID3D12_SetComputeRootDescriptorTable(struct RICmd &cmd, uint32_t parameter,
                                           D3D12_GPU_DESCRIPTOR_HANDLE handle);
void RID3D12_SetGraphicsRoot32BitConstants(struct RICmd &cmd, uint32_t parameter,
                                           uint32_t count, const void *data,
                                           uint32_t offset);
void RID3D12_SetComputeRoot32BitConstants(struct RICmd &cmd, uint32_t parameter,
                                          uint32_t count, const void *data,
                                          uint32_t offset);
// Record which root parameters the newly bound program's shaders actually
// read, so a draw or dispatch can be checked against what has been bound.
void RID3D12_NoteBoundProgramRootArgs(struct RICmd &cmd, bool compute,
                                      uint64_t required, const char *debugName);
// Reports, once per parameter per root-signature change, any root parameter
// the bound program needs that nothing has written. `what` names the command
// ("draw", "dispatch", ...). Counts into g_riD3D12MissingRootArgumentCount.
void RID3D12_CheckRootArguments(struct RICmd &cmd, const char *what);
extern uint32_t g_riD3D12MissingRootArgumentCount;

struct RITexture;
struct RITextureDesc;
struct RITextureView;
struct RITextureViewDesc;
struct RISampler;
struct RIAccelStructure;
struct RIAccelStructureDesc;

// RI_Format_e -> DXGI_FORMAT. Symmetric with RIFormatToVK in RIVK.h.
DXGI_FORMAT RIFormatToD3D12(uint32_t format);

// Pipeline / draw-state translation, mirroring the ri_vk_RI*ToVK set in
// RIVK.h. Both backends' converters for a domain live beside each other by
// convention (see RIBarrier.h). These take RI enums directly -- the pipeline
// path deliberately no longer routes state through Vulkan types on its way to
// a PSO.
static inline D3D12_BLEND ri_d3d12_RIBlendFactorToD3D12(enum RIBlendFactor_e factor) {
  switch (factor) {
  case RI_BLEND_ZERO:
    return D3D12_BLEND_ZERO;
  case RI_BLEND_ONE:
    return D3D12_BLEND_ONE;
  case RI_BLEND_SRC_COLOR:
    return D3D12_BLEND_SRC_COLOR;
  case RI_BLEND_ONE_MINUS_SRC_COLOR:
    return D3D12_BLEND_INV_SRC_COLOR;
  case RI_BLEND_DST_COLOR:
    return D3D12_BLEND_DEST_COLOR;
  case RI_BLEND_ONE_MINUS_DST_COLOR:
    return D3D12_BLEND_INV_DEST_COLOR;
  case RI_BLEND_SRC_ALPHA:
    return D3D12_BLEND_SRC_ALPHA;
  case RI_BLEND_ONE_MINUS_SRC_ALPHA:
    return D3D12_BLEND_INV_SRC_ALPHA;
  case RI_BLEND_DST_ALPHA:
    return D3D12_BLEND_DEST_ALPHA;
  case RI_BLEND_ONE_MINUS_DST_ALPHA:
    return D3D12_BLEND_INV_DEST_ALPHA;
  case RI_BLEND_SRC_ALPHA_SATURATE:
    return D3D12_BLEND_SRC_ALPHA_SAT;
  case RI_BLEND_SRC1_COLOR:
    return D3D12_BLEND_SRC1_COLOR;
  case RI_BLEND_ONE_MINUS_SRC1_COLOR:
    return D3D12_BLEND_INV_SRC1_COLOR;
  case RI_BLEND_SRC1_ALPHA:
    return D3D12_BLEND_SRC1_ALPHA;
  case RI_BLEND_ONE_MINUS_SRC1_ALPHA:
    return D3D12_BLEND_INV_SRC1_ALPHA;
  // D3D12 has a single blend-factor register rather than Vulkan's separate
  // colour and alpha constants, so all four RI constants map onto it.
  case RI_BLEND_CONSTANT_COLOR:
  case RI_BLEND_CONSTANT_ALPHA:
    return D3D12_BLEND_BLEND_FACTOR;
  case RI_BLEND_ONE_MINUS_CONSTANT_COLOR:
  case RI_BLEND_ONE_MINUS_CONSTANT_ALPHA:
    return D3D12_BLEND_INV_BLEND_FACTOR;
  }
  assert(false);
  return D3D12_BLEND_ZERO;
}

// Same mapping for the SrcBlendAlpha / DestBlendAlpha slots, which D3D12
// restricts: a factor derived from a colour channel is illegal there and makes
// CreateGraphicsPipelineState fail with E_INVALIDARG, naming nothing unless the
// debug layer happens to be on. Vulkan permits the same factor in an alpha slot
// (it just uses the alpha component), so a desc written against Vulkan can
// carry one across without anything complaining until the PSO build. Fail the
// debug build at the point that knows which field is at fault.
static inline D3D12_BLEND
ri_d3d12_RIBlendFactorToD3D12Alpha(enum RIBlendFactor_e factor) {
  const D3D12_BLEND blend = ri_d3d12_RIBlendFactorToD3D12(factor);
  assert(blend != D3D12_BLEND_SRC_COLOR && blend != D3D12_BLEND_INV_SRC_COLOR &&
         blend != D3D12_BLEND_DEST_COLOR &&
         blend != D3D12_BLEND_INV_DEST_COLOR &&
         blend != D3D12_BLEND_SRC1_COLOR &&
         blend != D3D12_BLEND_INV_SRC1_COLOR &&
         "D3D12 forbids a colour blend factor in an alpha blend slot");
  return blend;
}

static inline D3D12_BLEND_OP ri_d3d12_RIBlendOpToD3D12(enum RIBlendOp_e op) {
  switch (op) {
  case RI_BLEND_OP_ADD:
    return D3D12_BLEND_OP_ADD;
  case RI_BLEND_OP_SUBTRACT:
    return D3D12_BLEND_OP_SUBTRACT;
  case RI_BLEND_OP_REVERSE_SUBTRACT:
    return D3D12_BLEND_OP_REV_SUBTRACT;
  case RI_BLEND_OP_MIN:
    return D3D12_BLEND_OP_MIN;
  case RI_BLEND_OP_MAX:
    return D3D12_BLEND_OP_MAX;
  }
  assert(false);
  return D3D12_BLEND_OP_ADD;
}

static inline D3D12_COMPARISON_FUNC
ri_d3d12_RICompareFuncToD3D12(enum RICompareFunc_e func) {
  switch (func) {
  case RI_COMPARE_NONE:
    // "Test disabled" has no D3D12 spelling; the caller clears the matching
    // enable bit instead. ALWAYS is the behavioural equivalent if one leaks
    // through with the test still enabled.
    return D3D12_COMPARISON_FUNC_ALWAYS;
  case RI_COMPARE_ALWAYS:
    return D3D12_COMPARISON_FUNC_ALWAYS;
  case RI_COMPARE_NEVER:
    return D3D12_COMPARISON_FUNC_NEVER;
  case RI_COMPARE_EQUAL:
    return D3D12_COMPARISON_FUNC_EQUAL;
  case RI_COMPARE_NOT_EQUAL:
    return D3D12_COMPARISON_FUNC_NOT_EQUAL;
  case RI_COMPARE_LESS:
    return D3D12_COMPARISON_FUNC_LESS;
  case RI_COMPARE_LESS_EQUAL:
    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
  case RI_COMPARE_GREATER:
    return D3D12_COMPARISON_FUNC_GREATER;
  case RI_COMPARE_GREATER_EQUAL:
    return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
  }
  assert(false);
  return D3D12_COMPARISON_FUNC_ALWAYS;
}

static inline D3D12_STENCIL_OP ri_d3d12_RIStencilOpToD3D12(enum RIStencilOp_e op) {
  switch (op) {
  case RI_STENCIL_OP_KEEP:
    return D3D12_STENCIL_OP_KEEP;
  case RI_STENCIL_OP_ZERO:
    return D3D12_STENCIL_OP_ZERO;
  case RI_STENCIL_OP_REPLACE:
    return D3D12_STENCIL_OP_REPLACE;
  case RI_STENCIL_OP_INCREMENT_AND_CLAMP:
    return D3D12_STENCIL_OP_INCR_SAT;
  case RI_STENCIL_OP_DECREMENT_AND_CLAMP:
    return D3D12_STENCIL_OP_DECR_SAT;
  case RI_STENCIL_OP_INVERT:
    return D3D12_STENCIL_OP_INVERT;
  case RI_STENCIL_OP_INCREMENT_AND_WRAP:
    return D3D12_STENCIL_OP_INCR;
  case RI_STENCIL_OP_DECREMENT_AND_WRAP:
    return D3D12_STENCIL_OP_DECR;
  }
  assert(false);
  return D3D12_STENCIL_OP_KEEP;
}

static inline D3D12_FILL_MODE ri_d3d12_RIPolygonModeToD3D12(enum RIPolygonMode_e mode) {
  switch (mode) {
  case RI_POLYGON_MODE_FILL:
    return D3D12_FILL_MODE_SOLID;
  case RI_POLYGON_MODE_LINE:
    return D3D12_FILL_MODE_WIREFRAME;
  }
  assert(false);
  return D3D12_FILL_MODE_SOLID;
}

static inline D3D12_CULL_MODE ri_d3d12_RICullModeToD3D12(enum RICullMode_e mode) {
  switch (mode) {
  case RI_CULL_MODE_NONE:
    return D3D12_CULL_MODE_NONE;
  case RI_CULL_MODE_FRONT:
    return D3D12_CULL_MODE_FRONT;
  case RI_CULL_MODE_BACK:
    return D3D12_CULL_MODE_BACK;
  case RI_CULL_MODE_BOTH:
    // D3D12 has no both-faces cull mode; Vulkan's FRONT|BACK discards all
    // triangles. Callers wanting that must disable the draw instead.
    break;
  }
  assert(false);
  return D3D12_CULL_MODE_NONE;
}

// D3D12 splits Vulkan's topology into PSO-time topology *type* and
// command-list-time topology. This is the PSO half.
static inline D3D12_PRIMITIVE_TOPOLOGY_TYPE
ri_d3d12_RITopologyTypeToD3D12(enum RITopology_e topology) {
  switch (topology) {
  case RI_TOPOLOGY_POINT_LIST:
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  case RI_TOPOLOGY_LINE_LIST:
  case RI_TOPOLOGY_LINE_STRIP:
  case RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
  case RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  case RI_TOPOLOGY_TRIANGLE_LIST:
  case RI_TOPOLOGY_TRIANGLE_STRIP:
  case RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
  case RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  case RI_TOPOLOGY_PATCH_LIST:
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  }
  assert(false);
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

// ...and this is the command-list half, cached in PipelineSlot::d3d12 and
// replayed by IASetPrimitiveTopology.
static inline D3D12_PRIMITIVE_TOPOLOGY
ri_d3d12_RITopologyToD3D12(enum RITopology_e topology) {
  switch (topology) {
  case RI_TOPOLOGY_POINT_LIST:
    return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  case RI_TOPOLOGY_LINE_LIST:
    return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
  case RI_TOPOLOGY_LINE_STRIP:
    return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
  case RI_TOPOLOGY_TRIANGLE_LIST:
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  case RI_TOPOLOGY_TRIANGLE_STRIP:
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  case RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
  case RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
    return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
  case RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
  case RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  case RI_TOPOLOGY_PATCH_LIST:
    // Control-point count is not part of RITopology_e; a tessellating pass
    // must extend the RI vocabulary before it can express this.
    break;
  }
  assert(false);
  return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

static inline UINT8 ri_d3d12_RIColorWriteMaskToD3D12(enum RIColorWriteMask_e mask) {
  UINT8 out = 0;
  if (mask & RI_COLOR_WRITE_R)
    out |= D3D12_COLOR_WRITE_ENABLE_RED;
  if (mask & RI_COLOR_WRITE_G)
    out |= D3D12_COLOR_WRITE_ENABLE_GREEN;
  if (mask & RI_COLOR_WRITE_B)
    out |= D3D12_COLOR_WRITE_ENABLE_BLUE;
  if (mask & RI_COLOR_WRITE_A)
    out |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
  return out;
}

static inline D3D12_INPUT_CLASSIFICATION
ri_d3d12_RIVertexInputRateToD3D12(enum RIVertexInputRate_e rate) {
  switch (rate) {
  case RI_VERTEX_INPUT_RATE_VERTEX:
    return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  case RI_VERTEX_INPUT_RATE_INSTANCE:
    return D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
  }
  assert(false);
  return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
}

int  RID3D12_CreateTexture(struct RIDevice &device, const struct RITextureDesc &desc, struct RITexture &out);
void RID3D12_DisposeTexture(struct RIDevice &device, struct RITexture &texture);
void RID3D12_SetTextureDebugName(struct RIDevice &device, struct RITexture &texture,
                                 const char *name);
bool RID3D12_TextureIsEmpty(const struct RITexture &texture);

int  RID3D12_CreateTextureView(struct RIDevice &device, const struct RITexture &tex, const struct RITextureViewDesc &desc, struct RITextureView &out);
void RID3D12_DisposeTextureView(struct RIDevice &device, struct RITextureView &view);
bool RID3D12_TextureViewIsEmpty(const struct RITextureView &view);

void RID3D12_DisposeSampler(struct RIDevice &device, struct RISampler &sampler);
bool RID3D12_SamplerIsEmpty(const struct RISampler &sampler);

struct RIBuildBlasDesc;
struct RIBuildTlasDesc;

// DXR acceleration structures (RID3D12AccelStructure.cpp). Unlike Vulkan there
// is no separate acceleration-structure object: a D3D12 AS *is* a region of a
// caller-owned buffer created with RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE.
// RIAccelStructure therefore only snapshots that region's address and borrows
// the storage buffer's native handles; it never owns them.
//
// Every entry point requires device.d3d12.device5 and a rayTracingTier >= 1
// adapter. Init returns RI_FAIL when either is missing, so callers that probe
// acceleration structures on a non-DXR adapter still fail fast.
int  RID3D12_InitAccelStructure(struct RIDevice &device, struct RIAccelStructure &as, const struct RIAccelStructureDesc *desc);
void RID3D12_DisposeAccelStructure(struct RIDevice &device, struct RIAccelStructure &as);
void RID3D12_SetAccelStructureDebugName(struct RIDevice &device,
                                        struct RIAccelStructure &as,
                                        const char *name);
bool RID3D12_AccelStructureIsEmpty(const struct RIAccelStructure &as);
// Prebuild sizes for `desc`. Any out-pointer may be NULL. Mirrors the Vulkan
// vkGetAccelerationStructureBuildSizesKHR path: geometry buffer addresses are
// not consulted, only the shape (counts, formats, strides).
void RID3D12_AccelStructureGetMemoryReqs(struct RIDevice &device,
                                         const struct RIAccelStructureDesc *desc,
                                         uint64_t *outStorageSize,
                                         uint64_t *outBuildScratchSize,
                                         uint64_t *outUpdateScratchSize);
// Record BLAS / TLAS builds. D3D12 has no batched build call, so these loop
// over the descs issuing one BuildRaytracingAccelerationStructure each, then
// emit a single UAV barrier covering the whole batch.
void RID3D12_BuildBlas(struct RIDevice &device, struct RICmd &cmd,
                       const struct RIBuildBlasDesc *descs, uint32_t numDescs);
void RID3D12_BuildTlas(struct RIDevice &device, struct RICmd &cmd,
                       const struct RIBuildTlasDesc *descs, uint32_t numDescs);


// True when this device can service DXR calls at all: acceleration-structure
// builds, ray query and state objects all funnel through it, so a non-DXR
// adapter degrades to a clear failure rather than a null-pointer dereference.
bool ri_d3d12_dxrAvailable(const struct RIDevice &device);

// Blocks the CPU until every command list already submitted on queue has
// finished executing. Implemented with an ID3D12Fence + event.
void RID3D12_QueueWaitIdle(struct RIDevice &device, struct RIQueue &queue);

struct RID3D12FenceOp {
  ID3D12Fence *fence;
  uint64_t value;
};

struct RID3D12SubmitDesc {
  ID3D12CommandList *const *lists;
  uint32_t listCount;
  const RID3D12FenceOp *waits;
  uint32_t waitCount;
  const RID3D12FenceOp *signals;
  uint32_t signalCount;
  // Optional trailing completion Signal after ExecuteCommandLists. When
  // completionFence is null the field pair is ignored; when non-null it is
  // signaled with completionValue as the final queue op. Callers use this to
  // stamp a monotonically increasing completion value onto a command-ring
  // element after a successful submit.
  ID3D12Fence *completionFence;
  uint64_t completionValue;
};

// Validates that every list's D3D12_COMMAND_LIST_TYPE matches queue's type,
// performs Wait / ExecuteCommandLists / Signal in that order. Returns RI_FAIL
// on any validation failure and skips ExecuteCommandLists + the signals,
// leaving no queue op that could leak a stale completion token. Returns
// RI_SUCCESS on the happy path.
enum RIResult_e RID3D12_QueueSubmit(struct RIDevice &device,
                                    struct RIQueue &queue,
                                    const RID3D12SubmitDesc &desc);

#endif // DEVICE_IMPL_D3D12

#endif // RI_D3D12_H
