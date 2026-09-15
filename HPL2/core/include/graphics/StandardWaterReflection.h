#pragma once

#include "graphics/Graphics.h"
#include "scene/Viewport.h"
#include "graphics/RenderList.h"
#include "graphics/StandardWaterMath.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace hpl {
class cResources;
class cWorld;
class cFrustum;
class cFogArea;
class iRenderable;

struct StandardWaterReflectionState;

class cStandardWaterReflection {
public:
  struct Sample {
    RITextureView *view = nullptr; // shader-readable after callback returns
    cMatrixf viewProjection = cMatrixf::Identity;
    bool available = false;
  };
  cStandardWaterReflection(cGraphics *graphics, cResources *resources);
  ~cStandardWaterReflection();
  cStandardWaterReflection(const cStandardWaterReflection &) = delete;

  bool LoadData();
  void DestroyData();
  Sample RecordSurface(cGraphics::FrameContext *frame,
      cViewport::StandardViewportState *state, uint32_t image,
      RITexture *reflectionImage, iRenderable *surface, cFrustum *mainFrustum,
      cWorld *world, RIProgram::DescriptorBinding *mainFrameBinding = nullptr,
      RIProgram::DescriptorBinding *fogBinding = nullptr,
      RISharedPointer<RIBuffer> *pointLights = nullptr,
      RISharedPointer<RIBuffer> *spotLights = nullptr,
      uint32_t pointLightCount = 0, uint32_t spotLightCount = 0,
      RITextureView *shadowView = nullptr,
      std::span<cFogArea *> visibleFogAreas = {},
      RISharedPointer<RIBuffer> *boxLights = nullptr,
      uint32_t boxLightCount = 0);
  static constexpr uint32_t kMaxCapturesPerFrame = 2;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace hpl
