#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

#include <memory>

namespace hpl {
class cResources;
class cWorld;

class cStandardDecalPass {
public:
  cStandardDecalPass(cGraphics *graphics, cResources *resources);
  ~cStandardDecalPass();

  bool LoadData();
  void DestroyData();
  bool IsLoaded() const { return m_loaded && m_program != nullptr; }

  bool Render(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
              uint32_t width, uint32_t height, RITextureView *colorInput,
              RITextureView *positionInput, RITextureView *normalInput,
              RITextureView *surfaceInput, RITextureView *output,
              cWorld *world, RIProgram::DescriptorBinding *frameBinding);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
