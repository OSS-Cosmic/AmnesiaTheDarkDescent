#ifndef HPL_TEMPORAL_UPSCALER_H
#define HPL_TEMPORAL_UPSCALER_H

#include <cstdint>
#include <memory>

#include "graphics/Graphics.h" // cGraphics::FrameContext and RI enums
#include "graphics/TemporalUpscalerTypes.h"

struct RICmd;
struct RITexture;
struct RITextureView;

namespace hpl {

// A BORROWED texture binding.  The caller owns the RITexture and RITextureView
// and their lifetime; an implementation must not dispose either resource. The
// caller declares the subresource's entryState, and the implementation must
// leave that subresource in exitState when it returns.
struct TemporalUpscalerTextureBinding {
  // The backing RITexture (and therefore its backend image), not an image-view
  // handle reinterpreted as an image.
  struct RITexture *texture = nullptr;
  struct RITextureView *view = nullptr;
  enum RI_Format_e format = RI_FORMAT_UNKNOWN;
  TemporalUpscalerExtent extent = {};
  uint32_t mipOffset = 0;
  uint32_t mipCount = 1;
  uint32_t layerOffset = 0;
  uint32_t layerCount = 1;
  enum RIResourceState_e entryState = RI_RESOURCE_STATE_UNDEFINED;
  enum RIResourceState_e exitState = RI_RESOURCE_STATE_UNDEFINED;
  uint32_t entryStage = RI_STAGE_NONE;
  uint32_t exitStage = RI_STAGE_NONE;

  bool IsValid() const {
    return texture != nullptr && view != nullptr &&
           format != RI_FORMAT_UNKNOWN && extent.width != 0 &&
           extent.height != 0 && mipCount != 0 && layerCount != 0;
  }
};

struct TemporalUpscalerFrameInput {
  // HDR color and depth are bindings at the current render extent.  Motion is
  // CURRENT minus PREVIOUS unjittered UV motion, in normalized top-left UV
  // space belonging to the render extent.
  TemporalUpscalerTextureBinding color = {};
  TemporalUpscalerTextureBinding depth = {};
  TemporalUpscalerTextureBinding motionVectors = {};

  // Opaque-only color binding at the render extent, used for reactivity.  It
  // may be absent (an invalid binding) when the renderer has no separate
  // opaque color view.
  TemporalUpscalerTextureBinding opaqueColor = {};

  // Caller-produced mask inputs, already written before RecordResolve is
  // called. The implementation only reads these bindings and must not write
  // them. The two *Jittered masks use the ORIGINAL JITTERED input grid:
  // reactiveMaskJittered is the FSR reactive mask and
  // compositionMaskJittered is the FSR transparency-and-composition mask.
  // responsiveMaskUnjittered is in UNJITTERED image coordinates and is the
  // XeSS responsive mask. When any mask is valid, the provider must use it and
  // must not generate its own automatic reactive mask. When all are invalid,
  // the provider falls back to its documented default automatic behavior,
  // whose neutral response is the fallback.
  TemporalUpscalerTextureBinding reactiveMaskJittered = {};
  TemporalUpscalerTextureBinding compositionMaskJittered = {};
  TemporalUpscalerTextureBinding responsiveMaskUnjittered = {};

  // Caller-owned writable HDR destination at the output extent.  On success,
  // the implementation writes this binding and leaves it in output.exitState;
  // the caller retains ownership and may blit it or manage its lifetime after
  // RecordResolve returns.
  TemporalUpscalerTextureBinding output = {};

  RICmd *cmd = nullptr;
  uint32_t frameIndex = 0;
  float deltaTimeMs = 0.0f;

  // Jitter is the sample offset relative to pixel center in INPUT
  // (render-extent) pixels.  Coordinates are top-left pixel coordinates and
  // each component must be in [-0.5, 0.5].  These conventions are also
  // defined by HPL2/core/include/graphics/TemporalCamera.h; that header uses
  // jitterUV = jitterPixels / renderExtent.
  float jitterPixels[2] = {};
  float prevJitterPixels[2] = {};

  // Unjittered camera matrices, column-major and column-vector like
  // NrdFrameData.  The current and previous matrices correspond to the
  // current and previous camera poses respectively.
  float viewMat[16] = {};
  float unjitteredProjMat[16] = {};
  float prevViewMat[16] = {};
  float prevUnjitteredProjMat[16] = {};
  float zNear = 0.0f;
  float zFar = 0.0f;
  float verticalFovRadians = 0.0f;
  bool resetHistory = false;

  // The HDR color view is pre-exposed by this scalar.  FSR and XeSS consume
  // this scalar exposure metadata; this contract intentionally has no
  // exposure-texture field.  A value of 1 means no pre-exposure.
  float preExposure = 1.0f;
};

struct TemporalUpscalerOutput {
  bool success = false;
  // On success, result identifies the caller-owned binding that actually holds
  // the HDR result at the prepared output extent.  The caller owns this image
  // and view; the implementation must not dispose them.  On failure, result
  // is invalid and no output contents may be consumed.
  TemporalUpscalerTextureBinding result = {};
  // The actual state/layout in which result was left.  On success the caller
  // may assume result is already in resultState and may use that state for a
  // subsequent blit or other consumer; implementations should normally make
  // it equal to result.exitState.
  enum RIResourceState_e resultState = RI_RESOURCE_STATE_UNDEFINED;
  uint32_t resultStage = RI_STAGE_NONE;
};

// Shared contract for FSR/XeSS temporal upscaling on Windows and Linux.
// Implementations contain all SDK and backend details; this interface exposes
// only engine-neutral resource bindings and command-buffer declarations.
class iTemporalUpscaler {
public:
  virtual ~iTemporalUpscaler() = default;

  virtual bool Supports(TemporalUpscalerProvider provider,
                         TemporalUpscalerQuality quality) const = 0;

  virtual TemporalUpscalerExtent GetRecommendedRenderExtent(
      TemporalUpscalerExtent output,
      TemporalUpscalerQuality quality) const = 0;

  // The phase count is for the prepared render/output extent pair.  The
  // renderer should cycle phases using frameIndex and pass the resulting
  // jitter through TemporalUpscalerFrameInput.
  virtual uint32_t GetJitterPhaseCount(TemporalUpscalerExtent render,
                                       TemporalUpscalerExtent output) const = 0;

  // This must succeed before the renderer is allowed to reduce render
  // resolution or enable camera jitter.  `frame` must identify the active
  // frame whose command buffers may still be in flight.  If replacing a prepared
  // SDK context, the implementation must defer destruction of the entire old
  // context and all of its GPU resources through the owning cGraphics object's
  // graphicsDefer; it must never dispose them mid-frame.  On failure, NOTHING
  // in the frame input's output binding may be assumed written; the caller
  // must fall back to native extent with zero jitter and reset temporal history
  // on the next frame.
  virtual bool PrepareContext(const TemporalUpscalerSettings &settings,
                              TemporalUpscalerExtent render,
                              TemporalUpscalerExtent output,
                              cGraphics::FrameContext *frame) = 0;

  // Records the temporal upscale/resolve into input.cmd and returns the HDR
  // output binding.  PrepareContext must have succeeded for this same extent
  // pair before this method is called.  If this returns success == false,
  // NOTHING in input.output may be assumed written; the caller must fall back
  // to native extent with zero jitter and reset temporal history on the next
  // frame.  On success, the returned result is caller-owned and is left in
  // resultState (normally input.output.exitState), which the caller may rely
  // on when it blits or otherwise consumes the result.
  virtual TemporalUpscalerOutput RecordResolve(
      TemporalUpscalerExtent render, TemporalUpscalerExtent output,
      const TemporalUpscalerFrameInput &input) = 0;
};

// Static, side-effect-free availability. Creates no context, allocates no GPU
// resource and records no command. Off is logically available to
// TemporalUpscalerQuery because it needs no adapter, but this function returns
// false with "provider disabled" for Off so its result means that no provider
// should be created.
bool TemporalUpscalerAvailable(TemporalUpscalerProvider provider,
                               const char **outReason = nullptr);

// Writes the qualities this build/platform can actually use for `provider`
// into `outQualities` (capacity `maxQualities`); returns how many were written.
uint32_t TemporalUpscalerAvailableQualities(
    TemporalUpscalerProvider provider, TemporalUpscalerQuality *outQualities,
    uint32_t maxQualities);

// Resolves a desired setting to what would actually run. This query never
// creates a context, allocates a GPU resource or records a command; it is safe
// to call from an Options-menu callback. An unavailable provider or unsupported
// quality resolves to Off with a reason rather than silently changing provider
// or quality.
TemporalUpscalerStatus
TemporalUpscalerQuery(const TemporalUpscalerSettings &desired);

// Creates the adapter for a provider, or null for Off / unavailable. This
// allocates the adapter object only; PrepareContext still owns SDK setup.
std::unique_ptr<iTemporalUpscaler> TemporalUpscalerCreate(
    TemporalUpscalerProvider provider, cGraphics *graphics);

} // namespace hpl

#endif // HPL_TEMPORAL_UPSCALER_H
