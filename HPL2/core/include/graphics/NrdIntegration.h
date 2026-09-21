#ifndef HPL_NRD_INTEGRATION_H
#define HPL_NRD_INTEGRATION_H

#include <cstdint>
#include <memory>

struct RICmd;
struct RITextureView;

namespace hpl {

class cGraphics;

enum class NrdDenoiserMode { DiffuseSpecular, Specular, DirectDiffuse };

// The matrices are copied from the renderer's gPerFrame values.  They are
// column-major, column-vector matrices: this is both the engine convention
// (the shaders use mul(proj, view) and mul(view, position)) and NRD's required
// CommonSettings convention.
struct NrdFrameData {
  float viewToClipMatrix[16] = {};
  float viewToClipMatrixPrev[16] = {};
  float worldToViewMatrix[16] = {};
  float worldToViewMatrixPrev[16] = {};
  // Sample offset relative to the pixel center, in THIS NRD instance's INPUT
  // pixel units (the extent passed to OnResize; callers must convert jitter
  // to the instance's own pixels for a half-resolution instance). Coordinates
  // are top-left pixel coordinates, each component is in [-0.5, 0.5],
  // matching NRD's CommonSettings contract: sampleUv = pixelUv +
  // cameraJitter. Zero means no jitter. NRD requires both viewToClip matrices
  // to be UNJITTERED.
  float cameraJitter[2] = {};
  float cameraJitterPrev[2] = {};
  uint32_t frameIndex = 0;
  // View-space depth past which a pixel counts as sky/background. 0 keeps
  // NRD's own default.
  float denoisingRange = 0.0f;
  // Milliseconds since the previous frame. 0 lets NRD derive a fixed step from
  // frameIndex, which desyncs it from a variable frame rate.
  float timeDeltaMs = 0.0f;
};

struct NrdDenoiseInputs {
  RITextureView *normalRoughness = nullptr;
  RITextureView *viewZ = nullptr;
  RITextureView *motionVectors = nullptr;
  RITextureView *diffuseRadianceHitDistance = nullptr;
  RITextureView *specularRadianceHitDistance = nullptr;
};

struct NrdDenoiseOutputs {
  RITextureView *diffuseRadianceHitDistance = nullptr;
  RITextureView *specularRadianceHitDistance = nullptr;
};

// Native integration for one NRD denoiser instance.  Textures and compute
// programs are owned by this object and are recreated only when the extent
// changes.  The caller owns the input views and records Denoise into cmd.
class NrdIntegration {
public:
  static bool IsAvailable();
  // Instances constructed since startup. Each one builds its own texture pools
  // and pipelines, so a count that keeps climbing in play is VRAM churn.
  static uint32_t InstancesCreated();

  explicit NrdIntegration(
      cGraphics *graphics,
      NrdDenoiserMode mode = NrdDenoiserMode::DiffuseSpecular);
  ~NrdIntegration();

  NrdIntegration(const NrdIntegration &) = delete;
  NrdIntegration &operator=(const NrdIntegration &) = delete;

  void OnResize(uint32_t width, uint32_t height);
  // Discard the temporal history without reallocating NRD resources. The next
  // Denoise call uses NRD's CLEAR_AND_RESTART accumulation mode.
  void ResetHistory();

  // Denoise leaves the returned outputs in GENERAL and emits only a
  // compute-to-compute memory dependency.  A consumer that samples an output
  // from a FRAGMENT shader must add the compute-to-fragment dependency itself.
  // The caller must provide a private, writable motion-vector texture because
  // REBLUR's stabilization pass writes IN_MV. In DirectDiffuse (RELAX) mode,
  // motion vectors are read-only SHADER_RESOURCE and can use the raster input;
  // specular input/output are null. In Specular mode, diffuse input/output
  // are null. DirectDiffuse accepts linear RGB; hit distance is unused because
  // the diffuse prepass is disabled (every pixel has a resolved sample).
  NrdDenoiseOutputs Denoise(RICmd *cmd, const NrdFrameData &frame,
                            const NrdDenoiseInputs &inputs);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace hpl

#endif // HPL_NRD_INTEGRATION_H
