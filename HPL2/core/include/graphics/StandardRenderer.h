/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 */

#ifndef HPL_RENDERER_STANDARD_H
#define HPL_RENDERER_STANDARD_H

#include "graphics/Renderer.h"
#include "graphics/RenderList.h"
#include "graphics/RIProgram.h"
#include "graphics/Image.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITypes.h"
#include "graphics/StandardLightData.h"
#include "graphics/StandardParticlePass.h"
#include "graphics/StandardShadowAtlas.h"
#include "graphics/StandardShadowPass.h"
#include "graphics/StandardHaloPass.h"
#include "graphics/StandardWaterPass.h"
#include <memory>

namespace hpl {
class cStandardEnvironmentPass;
class cStandardDecalPass;
class cStandardTranslucentPass;
class cStandardAmbientOcclusionPass;
class cVertexBuffer;

// Independent opaque raster backend. It records a packed TriangleHit target
// when available and always keeps a conventional interpolated material path
// available for devices/tools that force it.
class cStandardRenderer : public iRenderer {
public:
  cStandardRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cStandardRenderer() override;

  bool LoadData() override;
  void DestroyData() override;

  void Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
            float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
            cRenderSettings *apSettings,
            bool abSendFrameBufferToPostEffects) override;

private:
  cRenderList2 m_rendererList;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer m_indirectDrawBuffer = {};
  // Shadow draws must not consume or alias the camera-visible indirect range.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowIndirectSegment;
  struct RIBuffer m_shadowIndirectBuffer = {};
  std::shared_ptr<RIProgram> m_visibility;
  std::shared_ptr<RIProgram> m_fallback;
  std::shared_ptr<RIProgram> m_reconstruct;
  std::shared_ptr<RIProgram> m_lighting;
  // Type="Decal" meshes (Decal.vert/frag, shared with the Hybrid renderer).
  std::shared_ptr<RIProgram> m_meshDecal;
  // Legacy scatter-disk shadow offsets, rebuilt when iRenderer shadow quality changes.
  SharedResourceHandle<Image> m_shadowJitter;
  int m_shadowJitterQuality = -1;
  // Shadow atlas pages (one array layer per page), kept across Draws. The
  // array grows when a Draw packs more pages and is recreated when the page
  // size follows the resolution cap. The cleared 1x1 page stands in when no
  // light casts this Draw.
  RISharedPointer<RITexture> m_shadowAtlas;
  uint32_t m_shadowAtlasSize = 0;
  uint32_t m_shadowAtlasLayers = 0;
  RISharedPointer<RITexture> m_shadowFallback;
  RISharedPointer<RITextureView> m_shadowFallbackView;
  std::unique_ptr<cStandardEnvironmentPass> m_environment;
  std::unique_ptr<cStandardParticlePass> m_particles;
  std::unique_ptr<cStandardShadowPass> m_shadow;
  std::unique_ptr<cStandardHaloPass> m_halo;
  std::unique_ptr<cStandardDecalPass> m_decals;
  std::unique_ptr<cStandardTranslucentPass> m_translucent;
  std::unique_ptr<cStandardWaterPass> m_water;
  std::unique_ptr<cStandardAmbientOcclusionPass> m_ambientOcclusion;
  bool m_forceFallback = false;
  bool m_visibilityLoaded = false;
  bool m_fallbackLoaded = false;
  bool m_reconstructLoaded = false;
  bool m_lightingLoaded = false;
  bool m_meshDecalLoaded = false;
  bool m_meshDecalWarned = false;
  bool m_shadowLoaded = false;
  bool m_ambientOcclusionLoaded = false;
};

} // namespace hpl

#endif // HPL_RENDERER_STANDARD_H
