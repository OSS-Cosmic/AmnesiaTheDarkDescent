#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
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
class cVertexBuffer;
class iRenderable;

// Per-viewport billboard halo occlusion queries (legacy RendererDeferred
// m_query). Each in-flight frame owns one occlusion pool; its counts are read
// back once the graphics timeline has passed the frame that recorded them.
struct StandardHaloQueryState {
  // Two queries per halo, the 4094 queries legacy MaxOcclusionDescSize allowed.
  static constexpr uint32_t kMaxHalos = 2047;
  struct Slot {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint64_t timelineValue = 0;
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

// Reads `count` 64-bit sample counts from a pool; VK_SUCCESS when all are ready.
using StandardHaloQueryReader =
    std::function<VkResult(VkQueryPool pool, uint32_t count, uint64_t *counts)>;

// Reads every slot whose frame the GPU has finished into latestVisibility; the
// newest frame wins. A slot whose counts are not ready yet stays pending.
void ResolveStandardHaloQueries(StandardHaloQueryState &state, uint64_t completedTimeline,
                                const StandardHaloQueryReader &readCounts);

// Legacy RendererDeferred billboard halos: each halo billboard's source box is
// drawn twice against the opaque depth, counting the texels that pass the depth
// test and all texels it covers. The halo alpha is the on-screen fraction of
// the box times visible / all, so a glow fades as it is occluded.
class cStandardHaloPass {
public:
  explicit cStandardHaloPass(cGraphics *graphics);
  ~cStandardHaloPass();
  void DestroyData();

  // Applies the newest finished counts to the halos in `translucents`.
  void Resolve(StandardHaloQueryState &state, std::span<iRenderable *> translucents,
               cFrustum *frustum);

  // Records this frame's queries against the opaque depth, which arrives and
  // leaves in SHADER_RESOURCE. `meshDecal` is the Decal.vert/frag program; the
  // pass warns once and records nothing without it.
  void Record(cGraphics::FrameContext *frame, StandardHaloQueryState &state,
              std::span<iRenderable *> translucents, RIProgram *meshDecal,
              RITexture *depthTexture, RITextureView *depthView, uint32_t width,
              uint32_t height, RIProgram::DescriptorBinding frameBinding, uint32_t paneSalt);

private:
  std::vector<cBillboard *> CollectHalos(std::span<iRenderable *> translucents) const;

  cGraphics *mpGraphics;
  // Unit box scaled by each halo's source size (legacy m_box).
  std::shared_ptr<cVertexBuffer> m_box;
  bool m_warnedUnavailable = false;
};

} // namespace hpl
