#pragma once

#include "graphics/RIProgram.h"
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
            RISharedPointer<RIBuffer> *, uint32_t, uint32_t);
private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
