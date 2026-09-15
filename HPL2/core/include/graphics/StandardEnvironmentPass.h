#pragma once

#include "graphics/RIProgram.h"
#include "graphics/Graphics.h"
#include "graphics/RICommand.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/GlobalManagedSets.h"
#include "math/Frustum.h"
#include "graphics/Renderer.h"
#include "graphics/TemporalCamera.h"
#include <memory>
#include <vector>
#include <span>

namespace hpl {
class cResources;
class cWorld;
class cFogArea;

class cStandardEnvironmentPass {
public:
  cStandardEnvironmentPass(cGraphics *graphics, cResources *resources);
  ~cStandardEnvironmentPass();
  bool LoadData();
  void DestroyData();
  bool PrepareFrame(cGraphics::FrameContext *frame, cFrustum *frustum,
                    uint32_t width, uint32_t height, float time, cWorld *world,
                    std::span<cFogArea *> visibleFogAreas,
                    const cColor &clearColor,
                    RIProgram::DescriptorBinding *frameBinding,
                    const hpl::TemporalFrameSnapshot *temporalSnapshot = nullptr);
  void AppendFogBindings(cWorld *world,
                         std::vector<RIProgram::DescriptorBinding> &bindings);
  bool Render(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
              uint32_t width, uint32_t height, RITexture *sceneColor,
              RITextureView *sceneColorView, RITextureView *positionView,
              RITextureView *outputView, cWorld *world,
              RIProgram::DescriptorBinding *frameBinding);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  RISharedPointer<RIBuffer> m_dummyFog;
  bool m_loaded = false;
};
}
