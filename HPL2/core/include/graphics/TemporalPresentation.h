#ifndef HPL_TEMPORAL_PRESENTATION_H
#define HPL_TEMPORAL_PRESENTATION_H

#include "graphics/TemporalUpscaler.h"

#include <array>
#include <cstdint>
#include <memory>

namespace hpl {

class cFileSearcher;
class RIProgram;

// The authored part of the scene color that is valid in the input image. The
// rectangle is kept separate from the physical texture extent because a
// future guard band or crop must not be mistaken for authored pixels.
struct TemporalPresentationRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool IsValid() const { return width != 0 && height != 0; }
};

// Borrowed scene-side resources. The depth texture is deliberately named
// separately from its two views: the combined view is for depth/stencil
// attachment use, while the sampled view must contain only the depth aspect.
struct TemporalPresentationSceneBindings {
  TemporalUpscalerTextureBinding hdrColor = {};
  TemporalPresentationRect hdrValidRect = {};

  RITexture *renderDepthTexture = nullptr;
  RITextureView *renderDepthAttachmentView = nullptr;
  RITextureView *renderDepthSampleView = nullptr;
  RIResourceState_e renderDepthEntryState = RI_RESOURCE_STATE_DEPTH_READ;
  RIResourceState_e renderDepthExitState = RI_RESOURCE_STATE_DEPTH_READ;

  TemporalUpscalerTextureBinding motionVectors = {};
  TemporalUpscalerTextureBinding opaqueColor = {};
  TemporalUpscalerTextureBinding reactiveMaskJittered = {};
  TemporalUpscalerTextureBinding compositionMaskJittered = {};
  TemporalUpscalerTextureBinding responsiveMaskUnjittered = {};
};

// One call records the display-depth reconstruction and, when a provider is
// supplied, the provider's temporal color resolve. All pointers in this
// structure are borrowed unless explicitly documented as an output below.
struct TemporalPresentationFrameInput {
  cGraphics::FrameContext *frameContext = nullptr;
  RICmd *cmd = nullptr;

  TemporalUpscalerExtent renderExtent = {};
  TemporalUpscalerExtent displayExtent = {};
  TemporalPresentationSceneBindings scene = {};

  iTemporalUpscaler *provider = nullptr;
  TemporalUpscalerSettings settings = {};

  float jitterPixels[2] = {};
  float previousJitterPixels[2] = {};
  float viewMat[16] = {};
  float unjitteredProjMat[16] = {};
  float previousViewMat[16] = {};
  float previousUnjitteredProjMat[16] = {};
  float zNear = 0.0f;
  float zFar = 0.0f;
  float verticalFovRadians = 0.0f;
  float deltaTimeMs = 0.0f;
  uint32_t frameIndex = 0;
  float preExposure = 1.0f;
  bool resetHistory = false;
};

struct TemporalPresentationResult {
  // A display-sized HDR image produced for this call. A provider failure never
  // clears providerFailed: when colorIsSpatialFallback is true as well, this
  // is the module-owned spatial reconstruction of the current scene HDR input
  // and the caller may present it despite providerFailed. It is display-sized,
  // linear RGBA16 HDR in `color`, with `colorState` set to SHADER_RESOURCE.
  // It is never a provider/SDK image.
  bool colorProduced = false;
  TemporalUpscalerTextureBinding color = {};
  RIResourceState_e colorState = RI_RESOURCE_STATE_UNDEFINED;
  uint32_t colorEntryStage = RI_STAGE_NONE;
  uint32_t colorExitStage = RI_STAGE_NONE;
  bool colorIsSpatialFallback = false;

  // Exactly one depth route is selected: either this frame produced the
  // display attachment, or the caller must keep using the render-depth input.
  // A provider failure does not clear a display attachment already produced
  // earlier in this same Resolve call.
  bool displayDepthProduced = false;
  bool depthAliasesRender = true;
  RITexture *displayDepthTexture = nullptr;
  RITextureView *displayDepthAttachmentView = nullptr;

  // True only for a provider failure in this call. ConsumeProviderFailure()
  // retains the same fact as a latch for the caller's next-frame fallback.
  // This remains true when colorIsSpatialFallback is true; that combination
  // means `color` is valid to present while the provider is disabled and
  // history is reset for the next frame.
  bool providerFailed = false;
};

class cTemporalPresentation {
public:
  // `shaderFiles` is borrowed and must outlive this viewport-owned object. The
  // depth and spatial-fallback programs are loaded lazily, so the native path
  // does not create a pipeline or any display-side GPU resource.
  explicit cTemporalPresentation(cFileSearcher *shaderFiles);
  ~cTemporalPresentation();

  cTemporalPresentation(const cTemporalPresentation &) = delete;
  cTemporalPresentation &operator=(const cTemporalPresentation &) = delete;
  cTemporalPresentation(cTemporalPresentation &&) = delete;
  cTemporalPresentation &operator=(cTemporalPresentation &&) = delete;

  TemporalPresentationResult Resolve(
      const TemporalPresentationFrameInput &input);

  // Resize and destruction both use this path. `frameContext` is the active
  // frame supplied by the viewport; GPU ownership is parked in graphicsDefer.
  void Release(cGraphics::FrameContext *frameContext);

  // Returns and clears the provider-failure latch. The caller should use this
  // to force native extent / zero jitter and reset history on the next frame.
  bool ConsumeProviderFailure();

  // These accessors are deliberately frame-scoped. They return null unless
  // Resolve produced display depth for exactly `frameIndex` and `displayExtent`.
  RITexture *GetDisplayDepthTexture(
      uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const;
  RITextureView *GetDisplayDepthAttachmentView(
      uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const;
  RITextureView *GetDisplayDepthSampleView(
      uint32_t frameIndex, TemporalUpscalerExtent displayExtent) const;

private:
  bool EnsureResolveDepthProgram();
  bool EnsureSpatialFallbackProgram();
  bool EnsureDisplayDepth(TemporalUpscalerExtent extent,
                          cGraphics::FrameContext *frameContext);
  bool EnsureDisplayColor(TemporalUpscalerExtent extent,
                          cGraphics::FrameContext *frameContext);
  bool RecordDepthResolve(const TemporalPresentationFrameInput &input);
  bool RecordSpatialFallback(const TemporalPresentationFrameInput &input);
  void InvalidateDisplayDepth();
  void LatchProviderFailure();
  bool HasOwnedResources() const;
  bool IsCurrentDisplayDepth(uint32_t frameIndex,
                             TemporalUpscalerExtent displayExtent) const;

  cFileSearcher *mpShaderFiles = nullptr;
  std::shared_ptr<RIProgram> mpResolveDepth;
  std::shared_ptr<RIProgram> mpSpatialFallback;

  std::array<RISharedPointer<RITexture>, RI_MAX_SWAPCHAIN_IMAGES>
      m_displayDepthTextures;
  std::array<RISharedPointer<RITextureView>, RI_MAX_SWAPCHAIN_IMAGES>
      m_displayDepthViews;
  std::array<RISharedPointer<RITextureView>, RI_MAX_SWAPCHAIN_IMAGES>
      m_displayDepthSampleViews;
  std::array<RISharedPointer<RITexture>, RI_MAX_SWAPCHAIN_IMAGES>
      m_displayColorTextures;
  std::array<RISharedPointer<RITextureView>, RI_MAX_SWAPCHAIN_IMAGES>
      m_displayColorViews;

  TemporalUpscalerExtent m_resourceExtent = {};
  bool m_resourceExtentValid = false;
  uint32_t m_resourceImageCount = 0;
  bool m_displayDepthAllocated = false;
  bool m_displayColorAllocated = false;

  TemporalUpscalerExtent m_displayDepthExtent = {};
  uint32_t m_displayDepthFrameIndex = 0;
  bool m_displayDepthValid = false;
  bool m_providerFailureLatched = false;
  bool m_providerFailureReported = false;
};

} // namespace hpl

#endif // HPL_TEMPORAL_PRESENTATION_H
