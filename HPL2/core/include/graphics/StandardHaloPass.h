#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIProgram.h"
#include "graphics/RIQuery.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace hpl {
class cBillboard;
class cFrustum;
class cResources;
class cVertexBuffer;
class iRenderable;

// Per-viewport billboard halo occlusion queries (legacy RendererDeferred
// m_query). Each in-flight frame owns one occlusion pool; its counts are read
// back once the graphics timeline has passed the frame that recorded them. On
// D3D12 the pool also owns the readback buffer its results are resolved into --
// see RIQueryPool. The 1:1 slot-to-pool mapping is load-bearing: RIQueryPool
// tracks resolve coverage per pool, so two slots must never share one.
struct StandardHaloQueryState {
  // Two queries per halo, the 4094 queries legacy MaxOcclusionDescSize allowed.
  static constexpr uint32_t kMaxHalos = 2047;
  struct Slot {
    RIQueryPool pool;
    uint64_t timelineValue = 0;
    // The frame that recorded these queries, for the staleness check in
    // ResolveStandardHaloQueries.
    uint64_t recordedFrame = UINT64_MAX;
    bool resolved = true;
    // Plain (non-precise) queries only report whether any sample passed.
    bool precise = true;
    // Query 2i is depth-tested (visible samples), 2i+1 depth ALWAYS (max),
    // for the billboard with unique cookie cookies[i].
    std::vector<uint64_t> cookies;
  };
  cGraphics *graphics = nullptr;
  std::array<Slot, RI_NUMBER_FRAMES_FLIGHT> slots{};
  // Newest resolved visible/max ratio per billboard unique cookie.
  std::unordered_map<uint64_t, float> latestVisibility;
  uint64_t latestTimeline = 0;
  uint64_t lastRecordedFrame = UINT64_MAX;

  ~StandardHaloQueryState();
};

// Occluded-texel counts to halo visibility: precise queries give the visible
// fraction of the source box, plain queries only visible (1) or hidden (0).
float StandardHaloVisibility(uint64_t visible, uint64_t maximum, bool precise);

// Reads `count` 64-bit sample counts from a pool; false when they are not ready
// yet, which leaves the slot pending for a later Draw.
using StandardHaloQueryReader =
    std::function<bool(RIQueryPool *pool, uint32_t count, uint64_t *counts)>;

// Reads every slot whose frame the GPU has finished into latestVisibility; the
// newest frame wins. A slot whose counts are not ready yet stays pending.
//
// `currentFrame` drives a staleness escape hatch. cGraphics::Draw has paths
// that end the command list and advance the frame WITHOUT submitting, which
// strands a slot on a timeline value nothing will ever signal. On Vulkan such a
// slot merely stays pending forever and its halos freeze; on D3D12 a later
// submit can push the timeline past that value, so the counts must be dropped
// rather than trusted. Either way, a slot older than a couple of frame cycles
// is abandoned and discarded.
void ResolveStandardHaloQueries(StandardHaloQueryState &state, uint64_t completedTimeline,
                                uint64_t currentFrame,
                                const StandardHaloQueryReader &readCounts);

// Legacy RendererDeferred billboard halos: each halo billboard's source box is
// drawn twice against the opaque depth, counting the texels that pass the depth
// test and all texels it covers. The halo alpha is the on-screen fraction of
// the box times visible / all, so a glow fades as it is occluded.
class cStandardHaloPass {
public:
  cStandardHaloPass(cGraphics *graphics, cResources *resources);
  ~cStandardHaloPass();
  void DestroyData();

  // Applies the newest finished counts to the halos in `translucents`.
  void Resolve(StandardHaloQueryState &state, std::span<iRenderable *> translucents,
               cFrustum *frustum);

  // Records this frame's queries against the opaque depth, which arrives and
  // leaves in `depthState`. SHADER_RESOURCE (Standard) is flipped to
  // DEPTH_READ around the queries; a state that already admits DEPTH_READ
  // (the hybrid renderer's read-only depth) is used as-is. Without its
  // program the pass warns once and records nothing.
  void Record(cGraphics::FrameContext *frame, StandardHaloQueryState &state,
              std::span<iRenderable *> translucents, RITexture *depthTexture, RITextureView *depthView, uint32_t width,
              uint32_t height, RIProgram::DescriptorBinding frameBinding, uint32_t paneSalt,
              RIResourceState_e depthState = RI_RESOURCE_STATE_SHADER_RESOURCE);

private:
  std::vector<cBillboard *> CollectHalos(std::span<iRenderable *> translucents) const;
  // Decal.vert:vsOcclusion + Decal.frag:psOcclusion, loaded on first Record. The
  // queries render with no colour attachment and read position only, so the
  // decal's own entries would leave a colour write and two attributes unused.
  bool LoadProgram();

  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_programTried = false;
  // Unit box scaled by each halo's source size (legacy m_box).
  std::shared_ptr<cVertexBuffer> m_box;
  bool m_warnedUnavailable = false;
};

} // namespace hpl
