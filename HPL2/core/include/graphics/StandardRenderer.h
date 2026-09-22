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
#include "graphics/StandardHiZPass.h"
#include "graphics/StandardShadowCullPass.h"
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
  // Allocate / hand back every GPU cull buffer. Creation is idempotent so the
  // constructor and a post-DestroyData LoadData share one path.
  void CreateCullBuffers();
  void DisposeCullBuffers();
  // Seeds the device-local visibility buffer to "nothing visible" through the
  // uploader. The RI layer has no fillBuffer, so this is a staged copy.
  void ZeroCullVisibility();

  cRenderList2 m_rendererList;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  StagedIndirectBuffer m_indirectDrawBuffer;
  // Shadow draws must not consume or alias the camera-visible indirect range.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowIndirectSegment;
  StagedIndirectBuffer m_shadowIndirectBuffer;
  // Set once per (re)create: true until each staged buffer has been copied at
  // least once, so the first Flush transitions from UNDEFINED rather than
  // claiming a draw left it in INDIRECT_ARGUMENT. The shadow buffer needs no
  // flag -- the kernel authors it, so nothing is ever staged into it.
  bool m_indirectDrawFirstUse = true;
  bool m_translucentCommandFirstUse = true;
  // GPU shadow cull inputs. All three are host-mapped and written once per
  // Draw, so their visibility to the GPU comes from the implicit host-write
  // barrier at queue submit, exactly like the indirect buffers above.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowCandidateSegment;
  struct RIBuffer m_shadowCandidateBuffer = {};
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowCullTileSegment;
  struct RIBuffer m_shadowCullTileBuffer = {};
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowCullGroupSegment;
  struct RIBuffer m_shadowCullGroupBuffer = {};
  // Survivor counts, one uint per tile. Written by the cull kernel and read by
  // vkCmdDrawIndirectCount, so it needs INDIRECT usage as well as STORAGE.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_shadowDrawCountSegment;
  struct RIBuffer m_shadowDrawCountBuffer = {};
  // Camera occlusion parameters, one record per tile that occlusion-tests.
  // Shadow tiles never allocate one; the buffer still has to exist and be
  // bound, because the kernel reflects it on every dispatch.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullCameraSegment;
  struct RIBuffer m_cullCameraBuffer = {};
  // Translucent occlusion cull. Commands get their own ring because their slot
  // is VkDrawIndexedIndirectCommand sized for indexed and non-indexed draws
  // alike, which the 16-byte shadow command ring cannot express.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_translucentCommandSegment;
  StagedIndirectBuffer m_translucentCommandBuffer;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_translucentCandidateSegment;
  struct RIBuffer m_translucentCandidateBuffer = {};
  // Opaque two-phase camera cull. The candidate ring is per frame; the
  // visibility table deliberately is NOT -- phase 1 of this frame reads what
  // phase 2 of the last one wrote.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cameraCandidateSegment;
  struct RIBuffer m_cameraCandidateBuffer = {};
  struct RIBuffer m_cullVisibilityBuffer = {};
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
  std::unique_ptr<cStandardShadowCullPass> m_shadowCull;
  std::unique_ptr<cStandardHiZPass> m_hiZ;
  std::unique_ptr<cStandardHaloPass> m_halo;
  std::unique_ptr<cStandardDecalPass> m_decals;
  std::unique_ptr<cStandardTranslucentPass> m_translucent;
  std::unique_ptr<cStandardWaterPass> m_water;
  std::unique_ptr<cStandardAmbientOcclusionPass> m_ambientOcclusion;
  bool m_forceFallback = false;
  // HPL_STANDARD_LIGHT_CULL=0 turns off the camera-frustum partition in
  // BuildStandardLights, so every enabled light lands in the visible prefix.
  // An A/B against the default isolates the partition from the rest of the
  // light path without a rebuild.
  bool m_visibilityLoaded = false;
  bool m_fallbackLoaded = false;
  bool m_reconstructLoaded = false;
  bool m_lightingLoaded = false;
  bool m_meshDecalLoaded = false;
  bool m_meshDecalWarned = false;
  bool m_shadowLoaded = false;
  bool m_hiZLoaded = false;
  bool m_ambientOcclusionLoaded = false;
};

} // namespace hpl

#endif // HPL_RENDERER_STANDARD_H
