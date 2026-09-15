#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "scene/Viewport.h"

#include <memory>

namespace hpl {
class cResources;
class cFrustum;

class cStandardAmbientOcclusionPass {
public:
  cStandardAmbientOcclusionPass(cGraphics *graphics, cResources *resources);
  ~cStandardAmbientOcclusionPass();

  bool LoadData();
  void DestroyData();
  bool IsLoaded() const { return m_loaded && m_programs[0] != nullptr; }

  bool Render(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
              cViewport::StandardViewportState *state, uint32_t image,
              cFrustum *frustum, RIProgram::DescriptorBinding *frameBinding);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_programs[3];
  bool m_loaded = false;
};
}
