#ifndef HPL_TEMPORAL_REACTIVE_MASK_H
#define HPL_TEMPORAL_REACTIVE_MASK_H

#include "graphics/TemporalReactiveMaskMath.h"
#include "graphics/TemporalUpscaler.h"

#include <array>
#include <cstdint>
#include <memory>

namespace hpl {

class cFileSearcher;
class RIProgram;

// `x` and `y` are UNJITTERED mask pixel coordinates. Return the UV at which to
// sample the JITTERED render-extent scene mask. The pixel-center term is
// explicit: jitterPixels is the sample offset relative to the pixel center in
// render-extent pixels, with components in [-0.5, 0.5] (see
// notes/temporal_upscaling.md and TemporalCamera.h). Geometry in the jittered
// image moves by -jitter, so recovering the unjittered grid subtracts
// jitterPixels/extent:
//   outUV = ((pixel + 0.5) - jitterPixels) / extent.
inline void TemporalReactiveMaskUnjitteredSampleUV(
    uint32_t x, uint32_t y, TemporalUpscalerExtent extent,
    const float jitterPixels[2], float outUV[2]) {
  TemporalReactiveMaskUnjitteredSampleUV(
      x, y, TemporalReactiveMaskMathExtent{extent.width, extent.height},
      jitterPixels, outUV);
}

struct TemporalReactiveMaskFrameDesc {
  cGraphics::FrameContext *frameContext = nullptr;
  uint32_t frameIndex = 0;
  const void *viewportCookie = nullptr;
  TemporalUpscalerExtent renderExtent = {};
  bool enabled = false;              // a temporal provider is prepared this frame
  bool needUnjitteredVariant = false; // XeSS responsive mask requested
};

struct TemporalReactiveMaskSnapshotDesc {
  RICmd *cmd = nullptr;
  RITexture *sceneColor = nullptr;
  RI_Format_e sceneColorFormat = RI_FORMAT_UNKNOWN;
  TemporalUpscalerExtent extent = {};
  uint32_t frameIndex = 0;
  const void *viewportCookie = nullptr;
  RIResourceState_e sceneColorEntryState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  RIResourceState_e sceneColorExitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  uint32_t sceneColorEntryStage = RI_STAGE_FRAGMENT;
  uint32_t sceneColorExitStage = RI_STAGE_FRAGMENT;
};

struct TemporalReactiveMaskRecordDesc {
  RICmd *cmd = nullptr;
  RITexture *finalColor = nullptr;
  RITextureView *finalColorView = nullptr;
  RI_Format_e finalColorFormat = RI_FORMAT_UNKNOWN;
  TemporalUpscalerExtent extent = {};
  uint32_t frameIndex = 0;
  const void *viewportCookie = nullptr;
  RIResourceState_e finalColorEntryState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  RIResourceState_e finalColorExitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  float jitterPixels[2] = {};
  TemporalUpscalerProvider provider = TemporalUpscalerProvider::Off;
  // Selected by the viewport for the active renderer and upscaler.
  TemporalReactiveMaskParams params = {};
};

// Explicitly named variants so an adapter cannot bind the wrong one.
struct TemporalReactiveMaskBindings {
  bool valid = false;
  TemporalUpscalerTextureBinding opaqueColor = {};
  TemporalUpscalerTextureBinding reactiveMaskJittered = {};
  TemporalUpscalerTextureBinding compositionMaskJittered = {};
  TemporalUpscalerTextureBinding responsiveMaskUnjittered = {};
};

class cTemporalReactiveMask {
public:
  explicit cTemporalReactiveMask(cFileSearcher *shaderFiles);
  ~cTemporalReactiveMask();

  cTemporalReactiveMask(const cTemporalReactiveMask &) = delete;
  cTemporalReactiveMask &operator=(const cTemporalReactiveMask &) = delete;
  cTemporalReactiveMask(cTemporalReactiveMask &&) = delete;
  cTemporalReactiveMask &operator=(cTemporalReactiveMask &&) = delete;

  bool BeginFrame(const TemporalReactiveMaskFrameDesc &desc);
  bool RecordOpaqueSnapshot(const TemporalReactiveMaskSnapshotDesc &desc);
  bool RecordMasks(const TemporalReactiveMaskRecordDesc &desc);
  TemporalReactiveMaskBindings GetBindings(
      uint32_t frameIndex, TemporalUpscalerExtent extent,
      const void *viewportCookie) const;
  void Release(cGraphics::FrameContext *frameContext);

private:
  struct ScratchImage {
    RISharedPointer<RITexture> texture;
    RISharedPointer<RITextureView> view;
    RIResourceState_e state = RI_RESOURCE_STATE_UNDEFINED;
    uint32_t stage = RI_STAGE_NONE;
  };

  bool EnsureReactiveMaskProgram();
  bool AllocateResources(cGraphics *graphics, TemporalUpscalerExtent extent,
                         uint32_t imageCount, RI_Format_e maskFormat);
  bool IsCurrentFrame(uint32_t frameIndex, TemporalUpscalerExtent extent,
                      const void *viewportCookie) const;
  bool IsCurrentSnapshot(uint32_t frameIndex, TemporalUpscalerExtent extent,
                         const void *viewportCookie) const;
  void InvalidateFrame();
  static void ResetImageState(ScratchImage &image);
  static void Transition(RICmd *cmd, ScratchImage &image,
                         RIResourceState_e after, uint32_t afterStage);
  static TemporalUpscalerTextureBinding MakeBinding(
      const ScratchImage &image, RI_Format_e format,
      TemporalUpscalerExtent extent);

  cFileSearcher *mpShaderFiles = nullptr;
  std::shared_ptr<RIProgram> mpReactiveMask;

  std::array<ScratchImage, RI_MAX_SWAPCHAIN_IMAGES> m_opaqueColors;
  std::array<ScratchImage, RI_MAX_SWAPCHAIN_IMAGES> m_reactiveMasks;
  std::array<ScratchImage, RI_MAX_SWAPCHAIN_IMAGES> m_compositionMasks;
  std::array<ScratchImage, RI_MAX_SWAPCHAIN_IMAGES> m_responsiveMasks;

  TemporalUpscalerExtent m_resourceExtent = {};
  bool m_resourcesAllocated = false;
  uint32_t m_resourceImageCount = 0;
  bool m_needUnjitteredVariant = false;
  RI_Format_e m_maskFormat = RI_FORMAT_UNKNOWN;

  bool m_frameValid = false;
  uint32_t m_frameIndex = 0;
  const void *mpViewportCookie = nullptr;
  TemporalUpscalerExtent m_frameExtent = {};
  bool m_snapshotValid = false;
  bool m_masksValid = false;
};

} // namespace hpl

#endif // HPL_TEMPORAL_REACTIVE_MASK_H
