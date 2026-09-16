#pragma once

#include "graphics/RenderList.h"
#include "graphics/RIProgram.h"
#include "graphics/StandardTranslucentPass.h"
#include "graphics/StandardWaterReflection.h"
#include <memory>
#include <span>
#include <vector>

namespace hpl {

// Raster-only Standard water with an owned planar reflection capture.
class cStandardWaterPass {
public:
  cStandardWaterPass(cGraphics *, cResources *);
  ~cStandardWaterPass();
  bool LoadData();
  void DestroyData();
  bool RecordSurface(cGraphics::FrameContext *, cViewport::StandardViewportState *,
                     uint32_t, iRenderable *, cFrustum *, cWorld *,
                     RIProgram::DescriptorBinding *, RIProgram::DescriptorBinding *,
                     RISharedPointer<RIBuffer> *, RISharedPointer<RIBuffer> *,
                     uint32_t, uint32_t, RITextureView *,
                     std::span<cFogArea *> visibleFogAreas = {},
                     RISharedPointer<RIBuffer> *boxLights = nullptr,
                     uint32_t boxLightCount = 0);
  // cRenderSettings::mbRenderWorldReflection. Off disables cube and planar
  // reflection, as the legacy renderer did.
  void SetWorldReflectionEnabled(bool enabled) { m_worldReflectionEnabled = enabled; }
  // Borrowed from the renderer so the planar capture can draw reflected
  // translucents through the same pass the main view uses. Never owned.
  void SetTranslucentPass(cStandardTranslucentPass *pass) {
    if (m_reflection)
      m_reflection->SetTranslucentPass(pass);
  }
  // cRenderSettings::mbClipReflectionScreenRect.
  void SetClipReflectionScreenRect(bool enabled) {
    if (m_reflection)
      m_reflection->SetClipReflectionScreenRect(enabled);
  }
private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  std::unique_ptr<cStandardWaterReflection> m_reflection;
  bool m_loaded = false;
  bool m_worldReflectionEnabled = true;
};
}
