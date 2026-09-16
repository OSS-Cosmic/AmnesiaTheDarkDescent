#pragma once

#include "graphics/RIProgram.h"
#include "graphics/Graphics.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/StandardLightData.h"
#include <memory>
#include <span>

namespace hpl {
class cFrustum;
class cMaterial;
class cResources;
class cWorld;
class iRenderable;

// RT-free translucent pass used by cStandardRenderer after environment
// compositing. Geometry is always copied into the frame scratch ring, so a
// viewport never consumes another viewport's billboard/beam orientation.
class cStandardParticlePass {
public:
  cStandardParticlePass(cGraphics *graphics, cResources *resources);
  ~cStandardParticlePass();
  bool LoadData();
  void DestroyData();

  bool Render(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
              uint32_t width, uint32_t height, RITexture *target,
              RITextureView *targetView, RITextureView *depthSampleView,
              cFrustum *frustum, float frameTime,
              std::span<iRenderable *> translucents, cWorld *world,
              uint32_t viewportSalt,
              RIProgram::DescriptorBinding *frameBinding,
              const RIProgram::DescriptorBinding &fogBinding,
              RISharedPointer<RIBuffer> pointLights,
              RISharedPointer<RIBuffer> spotLights,
              uint32_t pointLightCount, uint32_t spotLightCount);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
