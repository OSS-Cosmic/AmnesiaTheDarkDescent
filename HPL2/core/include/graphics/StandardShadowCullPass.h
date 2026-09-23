#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RISharedPointer.h"
#include "graphics/StandardShadowCull.h"

#include <memory>

namespace hpl {
class cResources;

// Dispatches the GPU shadow cull: one reset over the live tiles, then one
// batched cull over every (tile, candidate) chunk.
//
// This replaces the per-tile CPU walk the renderer used to run. It owns no
// buffers -- the renderer owns the ring allocations and hands in the slices for
// this Draw -- so the pass is just the two programs plus the barrier sequence
// that makes the results safe to draw from.
class cStandardShadowCullPass {
public:
  // The renderer's cull buffers for one Draw, already populated on the host.
  struct Buffers {
    struct RIBuffer *candidates = nullptr;
    struct RIBuffer *tiles = nullptr;
    struct RIBuffer *groups = nullptr;
    struct RIBuffer *indirect = nullptr;
    struct RIBuffer *drawCounts = nullptr;
    // Camera occlusion inputs. A tile opts in by setting cameraIndex; every
    // shadow tile leaves it at kStandardCullNoCamera and never samples the
    // pyramid. Both must still be BOUND on every dispatch -- the kernel
    // reflects them whether or not a tile uses them -- so when the caller has
    // no pyramid it passes null and the pass substitutes its own 1x1 stand-in.
    struct RIBuffer *cameras = nullptr;
    struct RITextureView *hiZ = nullptr;
    // Persistent (NOT per-frame) visibility state for the two-phase camera
    // cull, indexed by StandardCullCandidate::visibilityKey. Bound on every
    // dispatch because the kernel reflects it; only the two visibility modes
    // touch it.
    struct RIBuffer *visibility = nullptr;
    // Element capacities, so the descriptor ranges cover the whole ring rather
    // than just this Draw's slice: the kernel indexes with absolute element
    // indices handed out by the renderer's segment allocators.
    uint32_t candidateCapacity = 0;
    uint32_t indirectCapacity = 0;
    uint32_t tileCapacity = 0;
    uint32_t groupCapacity = 0;
    uint32_t drawCountCapacity = 0;
    uint32_t cameraCapacity = 0;
    // Words addressable through gCullIndirectWords. Given explicitly rather
    // than derived from indirectCapacity: the translucent command ring uses a
    // 5-word slot (the indexed command's size) for every draw, so the word
    // count is not indirectCapacity * 4.
    uint32_t indirectWordCapacity = 0;
    uint32_t visibilityCapacity = 0;
  };

  // Pick the mode from the device: compact where vkCmdDrawIndirectCount
  // exists, in-place otherwise. The camera cull passes an explicit mode
  // instead, because its draw order is the host's and must not be compacted.
  static constexpr uint32_t kModeAuto = 0xFFFFFFFFu;

  cStandardShadowCullPass(cGraphics *graphics, cResources *resources);
  ~cStandardShadowCullPass();

  bool LoadData();
  void DestroyData();
  bool IsLoaded() const { return m_loaded && m_reset && m_cull; }

  // True when the device can source a draw count from a buffer. Decides which
  // mode the kernel runs in, and therefore whether the shadow pass may use
  // drawIndirectCount. Valid after LoadData.
  bool UsesDrawIndirectCount() const { return m_useDrawIndirectCount; }

  // Records reset + cull and leaves `indirect` and `drawCounts` readable as
  // indirect arguments.
  //
  // The tile and group ranges are given as base + count because the renderer
  // suballocates them from per-frame ring segments: this Draw's entries start
  // at an arbitrary offset, and the entries around them belong to frames still
  // in flight.
  bool Dispatch(RICmd *cmd, uint32_t frameIndex, const Buffers &buffers,
                uint32_t tileBase, uint32_t tileCount, uint32_t groupBase,
                uint32_t groupCount, uint32_t mode = kModeAuto,
                uint32_t commandWordDelta = 0);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_reset;
  std::shared_ptr<RIProgram> m_cull;
  // 1x1 stand-in bound as gCullHiZ when the caller has no pyramid, so the
  // descriptor set is complete even for a dispatch that never samples it.
  RISharedPointer<RITexture> m_hiZFallback;
  RISharedPointer<RITextureView> m_hiZFallbackView;
  bool m_hiZFallbackReady = false;
  bool m_hiZFallbackPendingTransition = false;
  bool m_loaded = false;
  bool m_useDrawIndirectCount = false;
};

// An indirect-command buffer the cull kernel writes and the draw then consumes
// as arguments. It always needs INDIRECT + SHADER_RESOURCE_STORAGE usage, which
// on D3D12 forces a device-local (DEFAULT-heap) resource: ALLOW_UNORDERED_ACCESS
// is illegal on an upload heap, so the kernel cannot write a host-mapped one.
// Shared by the Standard and Hybrid renderers; implemented in
// StandardRenderer.cpp.
//
// Two shapes, selected by `hostWritten` at Create:
//
//   hostWritten = false -- the kernel writes whole commands (the compact and
//     in-place shadow modes both call makeDrawCommand). Nothing on the host
//     ever touches it, so it is simply device-local on both backends.
//
//   hostWritten = true -- the host builds the commands and the kernel owns only
//     their instanceCount word (the instance-mask and two-phase visibility
//     modes). Vulkan keeps this in one host-mapped buffer as before; on D3D12
//     the host writes `host` and Flush copies the written range into `device`.
//
// Callers write through mapped(), Flush() the range before the dispatch that
// reads it, and bind gpu().
struct StagedIndirectBuffer {
  struct RIBuffer host = {};   // host-mapped staging; empty when !hostWritten
  struct RIBuffer device = {}; // device-local; empty when aliased to host
  bool staged = false;         // true => host is staging for device

  bool isEmpty() const;
  void *mapped() const { return host.mappedAddress; }
  // What the cull kernel binds and the draw reads.
  struct RIBuffer *gpu() { return device.isEmpty() ? &host : &device; }

  // Idempotent. `stride` is the command size, `elements` the slot capacity.
  bool Create(struct RIDevice *device, uint64_t elements, size_t stride,
              bool hostWritten, const char *debugName);
  void Defer(class cGraphics *graphics);
  // Copies [byteOffset, byteOffset + byteSize) into the device buffer. A no-op
  // unless staged. `firstUse` picks the incoming state, since a previous
  // frame's draw leaves the buffer in INDIRECT_ARGUMENT. `cullFollows` picks
  // the outgoing one: STORAGE_WRITE when a cull dispatch reads it next (the
  // state that pass declares on entry), INDIRECT_ARGUMENT when the draw
  // consumes the host's commands unculled.
  void Flush(struct RIDevice *device, struct RICmd *cmd, uint64_t byteOffset,
             uint64_t byteSize, bool firstUse, bool cullFollows);
};

} // namespace hpl
