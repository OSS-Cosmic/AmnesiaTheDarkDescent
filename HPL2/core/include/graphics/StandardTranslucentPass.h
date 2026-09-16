#pragma once

#include "graphics/RIProgram.h"
#include "graphics/StandardShadowCullPass.h"
#include "graphics/TranslucentMeshPipelineDesc.h"
#include "graphics/Graphics.h"
#include "scene/Viewport.h"
#include <memory>
#include <span>

namespace hpl {
class cGraphics;
class cResources;
class cWorld;
class cFrustum;
class cViewport;
class iRenderable;
class cFogArea;

// Forward-only, RT-free mesh translucency for the Standard renderer.  The
// caller owns the frame/light bindings and the viewport targets; this keeps
// the pass composable with alternate Standard composites.
class cStandardTranslucentPass {
public:
  // GPU occlusion cull for this Draw. Entirely optional: with `pass` null, or
  // a reservation too small for the draws this call turns out to need, the
  // pass records ordinary draws exactly as it did before.
  //
  // The caller reserves the ranges because it owns the rings; it sizes them to
  // the worst case (TWO commands per renderable -- the base draw plus the
  // cube-map reflection draw) since only this pass knows how many draws each
  // item really produces.
  //
  // Commands are 5-word slots, the size of VkDrawIndexedIndirectCommand, for
  // indexed and non-indexed draws alike. Each draw is its own
  // drawIndirect/drawIndexedIndirect with drawCount 1, so the slot stride is
  // never read by Vulkan -- only the offset is -- and a uniform stride keeps
  // the word arithmetic the kernel does trivial.
  struct OcclusionCull {
    cStandardShadowCullPass *pass = nullptr;
    cStandardShadowCullPass::Buffers buffers{};
    // Absolute element indices handed out by the caller's ring allocators.
    uint32_t candidateBase = 0;
    uint32_t commandBase = 0;   // in 5-word slots
    uint32_t capacity = 0;      // slots and candidates available
    uint32_t tileBase = 0;      // absolute index of the tile record
    uint32_t groupBase = 0;
    uint32_t groupCapacity = 0;
    // Host-mapped writable pointers into those same rings.
    StandardCullCandidate *candidateSlots = nullptr;
    uint32_t *commandWords = nullptr;   // 5 per slot, starting at commandBase
    StandardCullTile *tileSlot = nullptr;
    StandardCullGroup *groupSlots = nullptr;
    bool IsUsable() const {
      return pass != nullptr && capacity > 0 && candidateSlots != nullptr &&
             commandWords != nullptr && tileSlot != nullptr &&
             groupSlots != nullptr;
    }
  };

  cStandardTranslucentPass(cGraphics *, cResources *);
  ~cStandardTranslucentPass();
  bool LoadData();
  void DestroyData();
  // Inputs are already sorted by the caller. On success, target and depth
  // leave this pass shader-readable; on validation/submission failure no
  // attachment transitions are recorded.
  bool Draw(cGraphics::FrameContext *, cViewport::StandardViewportState *,
            uint32_t imageIndex, std::span<iRenderable *>, cFrustum *, cWorld *,
            RIProgram::DescriptorBinding *, RIProgram::DescriptorBinding *,
            RITextureView *,
            RISharedPointer<RIBuffer> *,
            RISharedPointer<RIBuffer> *, uint32_t, uint32_t,
            const OcclusionCull *cull = nullptr);
private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
