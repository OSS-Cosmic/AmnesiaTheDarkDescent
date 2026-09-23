#ifndef HPL_RI_GPU_PROFILER_H
#define HPL_RI_GPU_PROFILER_H

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIBuffer.h"
#include "graphics/RITypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hpl {

// One resolved pass timing from the most recently completed frame.
struct GpuPassTiming {
  std::string name;
  float ms;
  uint32_t depth; // nesting level; 0 = top-level (the per-Draw "Frame" scope)
};

// Per-pass GPU timing via VK_QUERY_TYPE_TIMESTAMP query pools, paired with
// VK_EXT_debug_utils labels so the same scopes also name regions in RGP/Nsight
// captures. One query pool per frame-in-flight slot; readback is gated on the
// graphics timeline so it never blocks. RIGpuScope (below) is the RAII wrapper
// used at each pass site in cHybridRenderer::Draw.
//
// Single-threaded (render thread), like the rest of the frame loop.
struct RIGpuProfiler {
  // 2 timestamps per scope; caps scopes-per-frame at kMaxQueries/2 across every
  // Draw recorded into the primary command buffer this frame (multiple viewports
  // share one primary CB). Scopes past the cap are timed as 0 (still labeled).
  static constexpr uint32_t kMaxQueries = 256;

  void init(struct RIDevice *device);
  void dispose(struct RIDevice *device);

  // Frame start (after primary.begin, before any pass): reset this slot's pool
  // and make it the active slot. timelineValue is the graphicsTimeline value the
  // upcoming primary submit will signal.
  void beginFrame(struct RICmd *cmd, uint32_t slot, uint64_t timelineValue);

  // Finish recording this frame's timing data. On D3D12 this records the
  // query-heap -> readback copy and must be called before the command list is
  // ended/submitted. Vulkan has no additional end-of-frame command.
  void endFrame(struct RICmd *cmd, bool willSubmit = true);

  // Bracket a pass. beginScope writes a start timestamp and opens a debug label;
  // endScope writes the end timestamp and closes the label. Nestable.
  void beginScope(struct RICmd *cmd, const char *name);
  void endScope(struct RICmd *cmd);

  // Read back any slot the GPU has finished (timelineValue <= completedTimeline)
  // into lastResults. Non-blocking. completedTimeline is
  // cGraphics::graphicsTimeline.completed(device) — passed in so the profiler stays
  // decoupled from the cGraphics frame-orchestration layer.
  void resolve(struct RIDevice *device, uint64_t completedTimeline);

  const std::vector<GpuPassTiming> &lastResults() const { return m_lastResults; }
  float lastTotalMs() const { return m_lastTotalMs; }

private:
  struct Scope {
    std::string name;
    uint32_t depth;
    uint32_t beginIdx; // query index; UINT32_MAX when the cap was exceeded
    uint32_t endIdx;
  };
  struct Slot {
    uint64_t timelineValue = 0; // 0 = never used
    bool resolved = true;
    uint32_t queryCount = 0;
    std::vector<Scope> scopes;
#if (DEVICE_IMPL_VULKAN)
    VkQueryPool pool = VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_D3D12)
    ID3D12QueryHeap *queryHeap = nullptr;
    RIBuffer readback = {};
    uint64_t *mapped = nullptr; // persistently mapped readback memory
#endif
  };

  std::array<Slot, RI_NUMBER_FRAMES_FLIGHT> m_slots;
  std::vector<uint32_t> m_openStack; // indices into the active slot's scopes
  uint32_t m_openLabelCount = 0;
  uint32_t m_activeSlot = 0;
  bool m_enabled = false;            // false until init() succeeds
  double m_ticksToMs = 0.0;          // 1000 / timestampFrequencyHz
  uint64_t m_validBitsMask = ~0ull;  // queue timestampValidBits mask

  std::vector<GpuPassTiming> m_lastResults;
  float m_lastTotalMs = 0.0f;
};

// RAII pass bracket. One line at the top of a pass block:
//   RIGpuScope _s(&cGraphics::profiler, &cGraphics::primary.cmds[0], "Composite");
struct RIGpuScope {
  RIGpuScope(RIGpuProfiler *profiler, struct RICmd *cmd, const char *name)
      : m_profiler(profiler), m_cmd(cmd) {
    if (m_profiler)
      m_profiler->beginScope(m_cmd, name);
  }
  ~RIGpuScope() {
    if (m_profiler)
      m_profiler->endScope(m_cmd);
  }
  RIGpuScope(const RIGpuScope &) = delete;
  RIGpuScope &operator=(const RIGpuScope &) = delete;

  RIGpuProfiler *m_profiler;
  struct RICmd *m_cmd;
};

} // namespace hpl

#endif // HPL_RI_GPU_PROFILER_H
