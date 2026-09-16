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
} // namespace hpl
