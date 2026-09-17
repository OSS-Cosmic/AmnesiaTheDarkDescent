/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_VIEWPORT_H
#define HPL_VIEWPORT_H

#include "graphics/DisplayDepthPolicy.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/HiZPyramid.h"
#include "graphics/NrdIntegration.h"
#include "graphics/TemporalCamera.h"
#include "graphics/TemporalUpscaler.h"
#include "graphics/TemporalUpscalerPolicy.h"
#include "graphics/Graphics.h"
#include "graphics/RIPogoBuffer.h"
#include "gui/GuiTypes.h"
#include "math/MathTypes.h"
#include "scene/SceneTypes.h"
#include "system/Event.h"

#include <memory>
#include <type_traits>
#include <variant>
#include <vector>

// The graphics headers above drag in the Vulkan WSI / X11 headers, which
// re-define None after DisplayDepthPolicy.h dropped it. Keep it out of every
// consumer of this header too (see DebugDraw.h for the same clash).
#ifdef None
#undef None
#endif

namespace hpl {
class WaterReflectionViewportState;
struct StandardWaterReflectionState;
struct StandardHaloQueryState;
}

namespace std {

// HybridViewportState's existing out-of-line destructor is implemented by
// the renderer translation unit, while the water state intentionally remains
// forward-declared in this header. Keep the default deleter's call out of
// line so that unique_ptr can retain its exact default-deleter type without
// requiring the water pass header here.
template <> struct default_delete<hpl::WaterReflectionViewportState> {
  constexpr default_delete() noexcept = default;
  void operator()(hpl::WaterReflectionViewportState *apState) const noexcept;
};

} // namespace std

namespace hpl {

//------------------------------------------

class cScene;
class cCamera;
class cFrustum;
class iRenderer;
class cRenderSettings;
class cPostEffectComposite;
class cWorld;
class cGuiSet;
class Image;
class cViewport;
class cTemporalPresentation;
class cTemporalReactiveMask;
struct cTexture;

//------------------------------------------

// Per-signal context for the viewport pipeline-stage events. Evaluate builds
// one at each signal site so handlers don't have to capture the firing cViewport
// or reach into the global RI bootstrap for the command buffer / frame state.
//
// Rather than one struct with fields that are null at the stages that don't
// produce them, each event carries ONLY what that stage exposes — a common
// recording/camera base plus stage-specific render handles:
//   - OnPreWorldDraw / OnPostDelivery : WorldDrawCtx          (base only)
//   - OnPostTranslucenceDraw          : PostTranslucenceDrawCtx (+ depthView;
//        the linear-HDR scene is reached via viewport->GetBackBuffer(), the
//        pogo does not exist yet)
//   - OnPostWorldDraw                 : PostWorldDrawCtx        (+ pogo +
//        depthView; pogo read half = final post-processed image)
struct WorldDrawCtx {
  cViewport                 *viewport;   // who fired — replaces a captured mpViewport
  struct RICmd            *cmd;        // = &Interface<cGraphics>::Get()->primary.cmds[0]
  struct RIDevice         *device;     // = &Interface<cGraphics>::Get()->device
  cGraphics::FrameContext *frame;      // active set (scratch allocs, freelist)
  cFrustum                  *frustum;
  uint32_t                   width, height;
  uint32_t                   frameIndex;
  float                      frameTime;
};

// After the renderer drew the scene (linear HDR in viewport->GetBackBuffer())
// but BEFORE the feed blit + post chain. Depth is read-only
// (DEPTH_ATTACHMENT_OPTIMAL); the pogo is not created yet.
struct PostTranslucenceDrawCtx : WorldDrawCtx {
  struct RITextureView    *depthView;
  // ACTUAL jittered raster view / projection used to draw the scene this
  // frame. Column-major, column-vector (ml::float4x4 storage order).
  // ALWAYS populated, including the native zero-jitter case, where they
  // equal the frustum's own view / projection.
  float viewMat[16];
  float projMat[16];
};

// After the feed blit + post-effect chain: the pogo read half holds the final
// image and the scene depth is still available.
struct PostWorldDrawCtx : WorldDrawCtx {
  struct RI_PogoBuffer      *pogo;
  struct RITextureView    *depthView;
};

//------------------------------------------

// Texture helpers the viewport-state backends build their targets with
// (Update + destructor impls live in the renderer .cpps). Create lives against
// the device; Release hands every GPU handle to the frame freelist (drained
// once the pipeline is done with them — or at cGraphics::Dispose).

// One color image + a cookie-stamped sampled view — the single-image
// equivalent of RI_PogoBufferInit (same create flags / view usage so the
// existing pogo-shaped barriers and pipelines keep matching). The descriptor
// is produced on demand from the view.
bool CreateViewportColorTexture(struct RIDevice *device, uint32_t width,
                                uint32_t height, enum RI_Format_e format,
                                uint32_t usage, // RITextureUsageBits_e bitmask
                                RISharedPointer<RITexture> *tex,
                                RISharedPointer<RITextureView> *view,
                                const char *what);

// One attachment image + a plain view (depth / visibility targets). layerNum > 1
// makes a 2D array image whose view spans every layer.
bool CreateViewportAttachmentTexture(struct RIDevice *device, uint32_t width,
                                     uint32_t height, enum RI_Format_e format,
                                     uint32_t usage, // RITextureUsageBits_e
                                     enum RITextureViewType_e viewType,
                                     RISharedPointer<RITexture> *tex,
                                     RISharedPointer<RITextureView> *view,
                                     const char *what, uint32_t layerNum = 1);

// Defer the attachment's shared handles to the graphics freelist and reset both
// to empty (used by headless one-off targets like the editor thumbnail builder).
void ReleaseViewportAttachmentTexture(RISharedPointer<RITexture> *tex,
                                      RISharedPointer<RITextureView> *view);

//------------------------------------------

class cViewport {
public:
  enum class eRenderExtentOwner {
    Native,
    DevRenderScale,
    Provider
  };

  cViewport(cScene *apScene);
  ~cViewport();

  // Connected event handlers, the backend state, and the pogo buffer are all
  // held by address — a viewport never copies or moves.
  cViewport(const cViewport &) = delete;
  cViewport &operator=(const cViewport &) = delete;
  cViewport(cViewport &&) = delete;
  cViewport &operator=(cViewport &&) = delete;

  // The state's finished color target for the current swapchain image, as
  // the consumer should read it: {x, y, width, height} describe the valid
  // authored window inside the scene image. The hybrid guard band is disabled,
  // so this is currently the whole image; the simple backend is also 1:1.
  // cScene feeds this window into the viewport pogo in the post-processing
  // step.
  struct BackBuffer {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    struct RITexture renderTarget = {};
    struct RITextureView renderTargetView = {};
  };

  // The matrices and temporal inputs the renderer actually used for this
  // viewport's raster frame. viewMat is the unjittered view matrix and
  // projMat is the jittered projection matrix. All matrices are column-major
  // column-vector (ml::float4x4 storage order). The record is stale when its
  // frameIndex differs from the current graphics frame.
  struct RasterCamera {
    float viewMat[16] = {};
    float projMat[16] = {};
    float unjitteredProjMat[16] = {};
    float previousViewMat[16] = {};
    float previousUnjitteredProjMat[16] = {};
    float jitterPixels[2] = {};
    float previousJitterPixels[2] = {};
    float deltaTimeMs = 0.0f;
    uint32_t frameIndex = 0;
    bool historyReset = false;
    bool valid = false;
  };

  void PublishRasterCamera(const float aViewMat[16],
                           const float aProjMat[16]);
  void PublishRasterTemporalFrame(const TemporalFrameSnapshot &aFrame,
                                  float aDeltaTimeMs);
  const RasterCamera &GetRasterCamera() const;

  // cHybridRenderer: the renderTarget is a SINGLE scene image (the main draw
  // never ping-pongs; the composite writes once and the forward passes draw on
  // top), with depth + packed-TriangleHit visibility backing the same frame.
  // The guard band is disabled: width/height are the negotiated render extent,
  // and targetWidth/targetHeight are the whole authored crop window inside
  // that image. The crop fields are INPUT-space and reserved for a future
  // guard band. Arrays are indexed by Interface<cGraphics>::Get()->swapchainIndex.
  struct HybridViewportState {
    uint32_t width = 0;        // negotiated scene/input image extent
    uint32_t height = 0;
    uint32_t targetWidth = 0;  // authored crop window inside the input image
    uint32_t targetHeight = 0;

    HybridViewportState();
    ~HybridViewportState();
    HybridViewportState(const HybridViewportState &) = delete;
    HybridViewportState &operator=(const HybridViewportState &) = delete;
    HybridViewportState(HybridViewportState &&rhs) noexcept;
    HybridViewportState &operator=(HybridViewportState &&rhs) noexcept;

    void Update(cGraphics::FrameContext *cntx, cVector2l size);
    BackBuffer GetBackBuffer() {
      const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
      if (width == 0 || height == 0 || renderTarget[swapchainIndex].isEmpty() ||
          renderTargetView[swapchainIndex].isEmpty())
        return {};

      // These fields are input-space authored bounds. Keep malformed state
      // inside the physical scene image before doing unsigned crop arithmetic.
      const uint32_t validTargetWidth =
          targetWidth > width ? width : targetWidth;
      const uint32_t validTargetHeight =
          targetHeight > height ? height : targetHeight;
      if (validTargetWidth == 0 || validTargetHeight == 0)
        return {};
      return {(width - validTargetWidth) / 2,
              (height - validTargetHeight) / 2,
              validTargetWidth,
              validTargetHeight,
              *renderTarget[swapchainIndex],
              *renderTargetView[swapchainIndex]};
    }

    RISharedPointer<RITexture> renderTarget[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> renderTargetView[RI_MAX_SWAPCHAIN_IMAGES];

    RISharedPointer<RITexture> depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> depthView[RI_MAX_SWAPCHAIN_IMAGES];
    // Depth-aspect-only SRV of the SAME depthTextures[] image — the combined
    // depthView above can't be sampled (Vulkan forbids sampling a DEPTH|STENCIL
    // view). Bound by the particle pass for soft-particle scene-depth reads.
    RISharedPointer<RITextureView> depthSampleView[RI_MAX_SWAPCHAIN_IMAGES];
    // Depth pyramid for the GPU occlusion cull the translucent passes run. See
    // HiZPyramid.h. Built from the G-buffer's depth, which is final opaque
    // depth: nothing writes depth again before the translucent families.
    HiZPyramid hiZ;
    // Lazy full-resolution nearest water view-depth, used only by particles.

    RISharedPointer<RITexture> visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> visibilityView[RI_MAX_SWAPCHAIN_IMAGES];

    // Packed visibility — RGBA32UI storage image written by the V-buffer
    // pass, sampled by the direct-lighting, path-tracing and composite
    // passes. Per-frame layout is UNDEFINED → GENERAL →
    // SHADER_READ_ONLY, like the gbuffer outputs.
    RISharedPointer<RITexture> packedHitInfoTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> packedHitInfoView[RI_MAX_SWAPCHAIN_IMAGES];

    // Screen-space velocity (motion vectors), RG16F — the gbuffer's 2nd
    // color target; sampled by temporal passes.
    RISharedPointer<RITexture> velocityTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> velocityView[RI_MAX_SWAPCHAIN_IMAGES];

    // Decal accumulators — Type="Decal" mesh decals (Mul/MulX2/Add, no alpha)
    // rasterized each frame before the composite. The composite applies
    // albedo = albedo * decalMul + decalAdd before lighting, so decals are lit +
    // fogged like the surface. Two targets because Mul and Add can't share one
    // op: decalMul holds the multiplicative factor (cleared WHITE = identity;
    // Mul/MulX2 accumulate), decalAdd the additive term (cleared BLACK = identity;
    // Add accumulates). COLOR_ATTACHMENT for the raster, SHADER_RESOURCE for the
    // composite read.
    RISharedPointer<RITexture> decalMulTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalMulView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> decalAddTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalAddView[RI_MAX_SWAPCHAIN_IMAGES];

    // ReSTIR DI's raw demodulated irradiance, overwritten each frame in GENERAL
    // and transitioned to SHADER_RESOURCE for the direct RELAX instance.
    // directLightingInit initializes/clears on first use, re-armed on resize.
    RISharedPointer<RITexture> directLightingTexture;
    RISharedPointer<RITextureView> directLightingView;
    // Surface-key history (viewZ, geometric normal) for reservoir reuse.
    RISharedPointer<RITexture> directKeyTexture[2];
    RISharedPointer<RITextureView> directKeyView[2];
    uint32_t directLightingIndex = 0;
    bool directLightingInit = false;

    // Path tracer output, consumed by NrdPack in the same frame. NRD owns all
    // denoiser history, so none of these ping-pong any more and none is
    // swapchain-indexed. indirectLightingInit triggers the one-time
    // UNDEFINED→GENERAL + clear, re-armed by Update on resize.
    //
    // The diffuse channel is albedo-demodulated (the composite re-applies
    // albedo); the specular channel is NOT demodulated and the composite adds
    // it straight on top.
    RISharedPointer<RITexture> indirectRadianceTexture;
    RISharedPointer<RITextureView> indirectRadianceView;
    RISharedPointer<RITexture> indirectSpecularTexture;
    RISharedPointer<RITextureView> indirectSpecularView;
    // Surface key (viewZ, normal.xyz), and its second half: primary-hit GGX
    // alpha in .x, per-lobe primary hit distance in metres in .y (diffuse) and
    // .z (specular), 0 = lobe not sampled / sky, .w reserved.
    RISharedPointer<RITexture> indirectKeyTexture;
    RISharedPointer<RITextureView> indirectKeyView;
    RISharedPointer<RITexture> indirectKeyExtraTexture;
    RISharedPointer<RITextureView> indirectKeyExtraView;

    // NRD frontend inputs, repacked from the path-traced textures by NrdPack.
    // Written and consumed within one frame — NRD keeps its own history
    // internally — so a single slot each.
    RISharedPointer<RITexture> nrdNormalRoughnessTexture;
    RISharedPointer<RITextureView> nrdNormalRoughnessView;
    RISharedPointer<RITexture> nrdViewZTexture;
    RISharedPointer<RITextureView> nrdViewZView;
    RISharedPointer<RITexture> nrdDiffuseRadianceHitDistTexture;
    RISharedPointer<RITextureView> nrdDiffuseRadianceHitDistView;
    RISharedPointer<RITexture> nrdSpecularRadianceHitDistTexture;
    RISharedPointer<RITextureView> nrdSpecularRadianceHitDistView;
    // NRD writes IN_MV during temporal stabilization, so keep a private
    // writable copy instead of handing it the shared velocity attachment.
    RISharedPointer<RITexture> nrdMotionVectorsTexture;
    RISharedPointer<RITextureView> nrdMotionVectorsView;

    // The denoiser itself lives HERE, not on the renderer: NRD's history and
    // its internal pool are sized to one extent, so a renderer-scoped instance
    // shared between two differently-sized viewports would thrash both every
    // frame. Created lazily by Update once the extent is known.
    //
    // shared_ptr, not unique_ptr: ~HybridViewportState hands ownership to
    // graphicsDefer inside a std::function so NRD's pipelines outlive the
    // in-flight command buffers still referencing them, and std::function
    // requires a copyable capture.
    std::shared_ptr<NrdIntegration> nrd;
    // Independent direct diffuse filtering; never shares GI history/hit distance.
    std::shared_ptr<NrdIntegration> directNrd;

    // Reflection history belongs to one (viewport, water object) pair. The
    // state is deliberately a unique_ptr because WaterReflectionViewportState
    // is neither copyable nor movable, while HybridViewportState must remain
    // movable for the resize path. It is created lazily on first water use;
    // destroying/resetting it invokes its own deferred GPU-release contract.
    std::unique_ptr<WaterReflectionViewportState> waterReflection;

    bool indirectLightingInit = false;

    // Water-only cut-detection state. These are intentionally separate from
    // indirectHistoryReset, which the opaque NRD block consumes and clears
    // earlier in the frame. Normal camera movement and animated wave normals
    // do not reset reflection history; only resize, world/camera changes, or
    // first use do.
    bool waterHistoryReset = false;
    bool waterPrevCameraValid = false;
    cVector3f waterPrevCameraPos;
    cVector3f waterPrevCameraDir;
    cMatrixf waterPrevProjMat = cMatrixf::Identity;

    // Set when the opaque temporal history must be discarded (resize, level
    // load, teleport). Separate from indirectLightingInit: the latter
    // describes resource lifetime and must not reissue UNDEFINED barriers
    // mid-stream.
    bool indirectHistoryReset = false;
    bool nrdInputInShaderResource = false;

    // ReSTIR DI reservoirs (RGBA32F = packed light index + W + M; exact uint
    // index needs full-float storage). [reservoirHistory] ping-pongs across
    // frames (shares directLightingIndex with directKey): DirectLightingPass reads [^1] as the
    // temporal history and DirectSpatialReusePass writes [cur] as next frame's
    // history. [reservoirTemporal] is the intra-frame hand-off from the temporal
    // pass to the spatial pass. All GENERAL, one-time clear with the others.
    RISharedPointer<RITexture> reservoirTexture[2];
    RISharedPointer<RITextureView> reservoirView[2];
    RISharedPointer<RITexture> reservoirTemporalTexture;
    RISharedPointer<RITextureView> reservoirTemporalView;

    // Previous UNJITTERED camera and sample offset for velocity / history
    // reprojection, kept independently for each viewport.
    hpl::TemporalViewportState temporal;
  };

  // cRendererWireFrame + cRendererSimple (identical needs): they draw 1:1
  // into their own color render target and only need a matching depth
  // attachment.
  struct SimpleViewportState {
    uint32_t width = 0;
    uint32_t height = 0;

    SimpleViewportState() = default;
    // See HybridViewportState: destruction defers the owned GPU resources;
    // copy banned, move defers-then-takes so the resize path can `*this = {}`.
    ~SimpleViewportState();
    SimpleViewportState(const SimpleViewportState &) = delete;
    SimpleViewportState &operator=(const SimpleViewportState &) = delete;
    SimpleViewportState(SimpleViewportState &&rhs) noexcept = default;
    SimpleViewportState &operator=(SimpleViewportState &&rhs) noexcept;

    void Update(cGraphics::FrameContext *cntx, cVector2l size);
    BackBuffer GetBackBuffer() {
      const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
      if (width == 0 || height == 0 || renderTarget[swapchainIndex].isEmpty() ||
          renderTargetView[swapchainIndex].isEmpty())
        return {};
      return {0, 0, width, height, *renderTarget[swapchainIndex],
              *renderTargetView[swapchainIndex]};
    }

    RISharedPointer<RITexture> renderTarget[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> renderTargetView[RI_MAX_SWAPCHAIN_IMAGES];

    RISharedPointer<RITexture> depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> depthView[RI_MAX_SWAPCHAIN_IMAGES];
  };

  // cStandardRenderer: full-resolution material G-buffer plus packed visibility
  // and depth. Visibility has a shader-resource view and a separate color
  // attachment view because Vulkan image views encode the aspect/layout use.
  struct StandardViewportState {
    uint32_t width = 0;
    uint32_t height = 0;

    StandardViewportState() = default;
    ~StandardViewportState();
    StandardViewportState(const StandardViewportState &) = delete;
    StandardViewportState &operator=(const StandardViewportState &) = delete;
    StandardViewportState(StandardViewportState &&rhs) noexcept;
    StandardViewportState &operator=(StandardViewportState &&rhs) noexcept;

    void Update(cGraphics::FrameContext *cntx, cVector2l size);
    BackBuffer GetBackBuffer() {
      const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
      if (width == 0 || height == 0 ||
          renderTarget[swapchainIndex].isEmpty() ||
          renderTargetView[swapchainIndex].isEmpty())
        return {};
      return {0, 0, width, height, *renderTarget[swapchainIndex],
              *renderTargetView[swapchainIndex]};
    }

    RISharedPointer<RITexture> renderTarget[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> renderTargetView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> depthView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> depthSampleView[RI_MAX_SWAPCHAIN_IMAGES];
    // Depth pyramid for the camera occlusion cull. See HiZPyramid.h.
    HiZPyramid hiZ;
    RISharedPointer<RITexture> visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> visibilityView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> visibilityAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> positionTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> positionView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> positionAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> normalTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> normalView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> normalAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    // Geometric view normal is retained for receiver tests/AO. Shading normal
    // is the authored normal-map result produced by the Standard material pass.
    RISharedPointer<RITexture> shadingNormalTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> shadingNormalView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> shadingNormalAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> surfaceTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> surfaceView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> surfaceAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> materialColorTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> materialColorView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> materialColorAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    // Decal pass output, kept separate so lighting consumes modified albedo.
    RISharedPointer<RITexture> decalColorTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalColorView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalColorAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    bool decalColorInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    // Type="Decal" mesh accumulators: multiply (cleared white) and add
    // (cleared black), folded into albedo by the light resolve.
    RISharedPointer<RITexture> decalMulTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalMulView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalMulAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> decalAddTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalAddView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> decalAddAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> environmentTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> environmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> environmentAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    // Per-mesh refractive source. It is never used as an attachment: the
    // Standard translucent pass copies the completed scene into it before a
    // refractive draw, then returns it to COPY_DST for the next mesh.
    RISharedPointer<RITexture> translucentSceneCopy[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> translucentSceneCopyView[RI_MAX_SWAPCHAIN_IMAGES];
    // Once initialized, each copy remains shader-readable between Draw calls;
    // do not reissue an UNDEFINED transition and discard the prior read dependency.
    bool translucentSceneCopyInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    // Planar reflection and its refraction snapshot are owned by this
    // viewport, not by the renderer.  Every swapchain image has independent
    // storage so an in-flight capture can never alias another image.
    RISharedPointer<RITexture> waterReflectionTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    // Opaque planar capture inputs for the environment compositor.  These are
    // separate from the final reflection image so the capture can depth-test
    // geometry and retain world positions for sky/fog composition.
    RISharedPointer<RITexture> waterReflectionPositionTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionPositionView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionPositionAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> waterReflectionOpaqueTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionOpaqueView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionOpaqueAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> waterReflectionDepthTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionDepthView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionDepthSampleView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionDepthAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> waterSceneCopy[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterSceneCopyView[RI_MAX_SWAPCHAIN_IMAGES];
    // Refraction source for translucents drawn INSIDE the planar capture.
    // Same contract as translucentSceneCopy, at the capture's half resolution.
    RISharedPointer<RITexture> waterReflectionSceneCopy[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> waterReflectionSceneCopyView[RI_MAX_SWAPCHAIN_IMAGES];
    bool waterReflectionSceneCopyInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    bool waterReflectionInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    bool waterSceneCopyInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    std::shared_ptr<StandardWaterReflectionState> waterReflection;
    // Billboard halo occlusion queries, one pool per in-flight frame.
    std::shared_ptr<StandardHaloQueryState> haloQueries;
    bool environmentInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
    RISharedPointer<RITexture> aoPreparedDepthTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> aoPreparedDepthStorageView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> aoQuarterTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> aoQuarterStorageView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITexture> aoTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> aoStorageView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> aoView[RI_MAX_SWAPCHAIN_IMAGES];
    // One per swapchain image, like every other per-image target here: Update
    // allocates and Draw indexes these by swapchain image index.
    RISharedPointer<RITexture> velocityTexture[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> velocityView[RI_MAX_SWAPCHAIN_IMAGES];
    RISharedPointer<RITextureView> velocityAttachmentView[RI_MAX_SWAPCHAIN_IMAGES];
    hpl::TemporalViewportState temporal;
    bool aoInitialized[RI_MAX_SWAPCHAIN_IMAGES] = {};
  };

  using ViewportState =
      std::variant<std::monostate, HybridViewportState, SimpleViewportState,
                   StandardViewportState>;

  // Where the viewport's finished image is delivered by cScene after the
  // viewport is fully evaluated (world draw + post processing):
  //  - TargetSwapchain: composited to the swapchain (tail blit + GUI block);
  //    the viewport extent follows the swapchain. At most one visible
  //    viewport may target the swapchain (asserted by GetPrimaryViewport).
  //  - TargetView: standalone at the given extent; when `view` is non-null
  //    the pogo read half is drawn into it at the tail and left
  //    SHADER_READ_ONLY for the consumer (editor panes sample it).
  struct TargetSwapchain {};
  struct TargetView {
    uint32_t width;
    uint32_t height;
    // Caller-owned color destination (e.g. the editor's pane texture):
    // cScene's delivery renders the viewport's finished pogo read half into
    // `view` and leaves the image SHADER_READ_ONLY for the consumer.
    // `texture` is the view's backing image — needed for the layout
    // transitions around the delivery draw.
    struct RITexture texture;
    RITextureView view;
    // Format of `view` — the delivery draw's color attachment (and pipeline
    // key). Defaults to the pogo format; readback consumers (thumbnails) can
    // hand an RGBA8 target instead.
    enum RI_Format_e format = cGraphics::PogoColorFormat;
  };
  using Target = std::variant<TargetSwapchain, TargetView>;

  void SetActive(bool abX) { mbActive = abX; }
  void SetVisible(bool abX) { mbVisible = abX; }
  bool IsActive() { return mbActive; }
  bool IsVisible() { return mbVisible; }

  void SetIsListener(bool abX) { mbIsListener = abX; }
  bool IsListener() { return mbIsListener; }

  void SetCamera(cCamera *apCamera);
  cCamera *GetCamera() { return mpCamera; }

  void SetWorld(cWorld *apWorld);
  cWorld *GetWorld() { return mpWorld; }

  // A different renderer means a different target set (PrepareToRender swaps
  // the m_state alternative on the next draw) but the viewport-level upscaler
  // and presentation history is reused, so it has to be cut here.
  // Drop the per-backend render targets. The next PrepareToRender<Backend>()
  // emplaces a fresh set; the discarded one defers its GPU resources as usual.
  // Used when the renderer that owned them is about to be destroyed.
  void ReleaseRenderState() { m_state.emplace<std::monostate>(); }

  void SetRenderer(iRenderer *apRenderer) {
    if (mpRenderer == apRenderer)
      return;
    mpRenderer = apRenderer;
    mTemporalHistoryReset = true;
  }
  iRenderer *GetRenderer() { return mpRenderer; }

  cRenderSettings *GetRenderSettings() { return mpRenderSettings.get(); }

  void SetPostEffectComposite(cPostEffectComposite *apPostEffectComposite) {
    mpPostEffectComposite = apPostEffectComposite;
  }
  cPostEffectComposite *GetPostEffectComposite() {
    return mpPostEffectComposite;
  }

  void AddGuiSet(cGuiSet *apSet);
  void RemoveGuiSet(cGuiSet *apSet);
  const std::vector<cGuiSet *> &GetGuiSets() const { return m_guiSets; }

  // Per-viewport pogo (ping-pong), sized to GetTargetSize(). Resizes hand
  // the old halves to the frame freelist — no stall.
  RI_PogoBuffer *PreparePogoBuffer(cGraphics::FrameContext *cntx);
  // Read-only: the existing pogo, or nullptr if none was created yet (no
  // world evaluated at this viewport so far).
  RI_PogoBuffer *PogoBuffer() {
    return mlPogoWidth != 0 && mlPogoHeight != 0 &&
                   !mPogoBuffer.textures[0].isEmpty() &&
                   !mPogoBuffer.textures[1].isEmpty() &&
                   !mPogoBuffer.pogoView[0].isEmpty() &&
                   !mPogoBuffer.pogoView[1].isEmpty()
               ? &mPogoBuffer
               : nullptr;
  }

  void SetTarget(const Target &aTarget);
  const Target &GetTarget() const { return mTarget; }
  cVector2l GetTargetSize() const;
  cVector2l GetDisplayExtent() const { return GetTargetSize(); }

  // Negotiated SCENE/INPUT extent used for shading. (0,0) means native —
  // follow the display extent. GetRenderExtent() clamps a non-native value to
  // at least 1x1 and at most the display extent, and falls back to the display
  // extent when the display is degenerate. SetRenderExtent() is the raw setter
  // and does not change ownership; use the provider ownership entry points
  // below for a successfully prepared temporal-upscaler context. The provider
  // owns the extent in preference to the development render-scale override.
  void SetRenderExtent(cVector2l aRenderExtent) { mRenderExtent = aRenderExtent; }
  // Claims the render extent for a successfully prepared temporal-upscaler
  // context. Takes precedence over the development render-scale override.
  // Returns true when this call changed the extent or took ownership; a
  // changed extent also flags the temporal history reset.
  bool ClaimProviderRenderExtent(cVector2l aRenderExtent);
  // Releases provider ownership: restores the (0,0) native sentinel and lets
  // the development override take the extent again.
  void ReleaseProviderRenderExtent();
  cVector2l GetRenderExtent() const;

  // The provider selection for this viewport. Only a ticket that has
  // successfully prepared an iTemporalUpscaler context may set a non-Off
  // provider; the reactive-mask work is gated on it, so the Off/native path
  // allocates nothing and records nothing.
  const TemporalUpscalerSettings &GetTemporalUpscalerSettings() const;
  void SetTemporalUpscalerSettings(const TemporalUpscalerSettings &aSettings);

  // Non-null only when a temporal upscaling provider is prepared for the
  // current frame and BeginFrame succeeded; null on the Off/native path.
  class cTemporalReactiveMask *GetTemporalReactiveMask();

  // Returns the number of jitter phases for the provider successfully prepared
  // for the current frame, or zero when no provider is prepared.
  uint32_t GetTemporalJitterPhaseCount() const;
  // Returns whether a temporal provider successfully prepared the current
  // frame.
  bool IsTemporalProviderPrepared() const;
  // Returns the requested and actual effective temporal-upscaler settings and
  // the reason an effective provider is unavailable, when one exists.
  TemporalUpscalerStatus GetTemporalUpscalerStatus() const;

  // Consumed by the renderer at the next snapshot point after a provider
  // failure. This forces its temporal history and the render extent back to
  // the native path without embedding presentation state in HybridViewportState.
  bool ConsumeTemporalHistoryReset();

  // Fully evaluate the viewport for this frame: world draw -> feed (the
  // BackBuffer crop window blitted into the pogo read half) -> post-effect
  // chain -> delivery per Target (TargetView draw / swapchain tail blit; the
  // primary viewport's GUI block stays in cScene::Render). Returns whether
  // the world was rendered (the GUI block keys LOAD vs CLEAR off this).
  //
  // Normally driven by cScene::Render for visible viewports, but callable
  // directly during the command-recording window (OnDraw ->
  // FlushRendering) for HEADLESS viewports outside the visible set — e.g.
  // the editor thumbnail builder. At most ONE Evaluate per viewport per
  // frame: the renderers' initial UNDEFINED backbuffer barriers have an
  // empty before-scope, so a second draw would race the first evaluation's
  // pending feed blit of the shared renderTarget[Interface<cGraphics>::Get()->swapchainIndex].
  bool Evaluate(cGraphics::FrameContext *cntx, float afFrameTime, tFlag alFlags);


  // Use the render-extent pair for scene-side access before the pre-feed HDR
  // hook.
  struct RITextureView *GetRenderDepthView();
  struct RITexture *GetRenderDepthTexture();
  struct RITextureView *GetRenderDepthSampleView();

  // The current frame's display attachment is preferred. Otherwise this
  // returns the scene pair only when its actually allocated size matches the
  // display extent and the logical render extent also matches; otherwise no
  // depth is returned.
  struct RITextureView *GetDepthView();
  // The depth+stencil image backing the same selected pair as GetDepthView(),
  // following the same allocation-size contract.
  struct RITexture *GetDepthTexture();
  // GetDepthView(), but only when the selected pair's actual allocation is
  // exactly aWidth x aHeight — the extent of the rendering instance it would
  // be attached to. Both dimensions must match; otherwise nullptr is
  // returned and the caller takes its no-depth path.
  struct RITextureView *GetDepthViewForExtent(uint32_t alWidth,
                                              uint32_t alHeight);
  BackBuffer GetBackBuffer();

  // Pipeline-stage events, signaled by Evaluate. Handlers run inside the
  // frame's command-recording window and may record their own commands into
  // Interface<cGraphics>::Get()->primary.cmds[0] (e.g. a readback copy of the delivered TargetView —
  // see the editor thumbnail builder).
  //  - OnPreWorldDraw: before the renderer Draw (also on world-less frames).
  //  - OnPostTranslucenceDraw: after the renderer Draw but BEFORE the feed
  //    blit + post-effect chain — the BackBuffer (GetBackBuffer()) still holds
  //    the LINEAR-HDR scene and depth is DEPTH_ATTACHMENT_OPTIMAL, so handlers
  //    can draw additive/translucent geometry that bloom + tonemap then
  //    process (e.g. the pickup-flash / enemy glow). pogo is null here (not yet
  //    created). Only signaled when the world was actually rendered.
  //  - OnPostWorldDraw: after the world draw + feed blit + post-effect
  //    chain — the pogo read half holds the final image. Only signaled when
  //    the world was actually rendered this Evaluate.
  //  - OnPostDelivery: after the Target delivery (TargetView left
  //    SHADER_READ_ONLY / swapchain tail-blitted). Only signaled when
  //    delivery actually ran, so handlers never touch a stale target.
  Event<const WorldDrawCtx &> &OnPreWorldDraw() { return m_onPreWorldDraw; }
  Event<const PostTranslucenceDrawCtx &> &OnPostTranslucenceDraw() {
    return m_onPostTranslucenceDraw;
  }
  Event<const PostWorldDrawCtx &> &OnPostWorldDraw() { return m_onPostWorldDraw; }
  Event<const WorldDrawCtx &> &OnPostDelivery() { return m_onPostDelivery; }

  template <typename Backend>
  Backend *PrepareToRender(cGraphics::FrameContext *cntx,
                           cVector2l aRenderExtent = cVector2l(0, 0)) {
    const cVector2l size =
        (aRenderExtent.x == 0 && aRenderExtent.y == 0)
            ? GetRenderExtent()
            : aRenderExtent;
    if (size.x <= 0 || size.y <= 0)
      return nullptr;
    if (!std::holds_alternative<Backend>(m_state)) {
      // emplace destroys the prior alternative, whose destructor defers its
      // GPU resources to the frame freelist — no explicit dispose needed.
      m_state.emplace<Backend>();
    }
    Backend &state = std::get<Backend>(m_state);
    const uint32_t previousWidth = state.width;
    const uint32_t previousHeight = state.height;
    state.Update(cntx, size);
    if constexpr (std::is_same_v<Backend, HybridViewportState>) {
      // Update's resize path resets the state with `*this = {}`. Observe the
      // resulting extent here so the water history is also cut on first use
      // and on every resize, alongside the opaque history reset.
      if (state.width != previousWidth || state.height != previousHeight) {
        state.waterHistoryReset = true;
        state.waterPrevCameraValid = false;
      }
    }
    return &state;
  }

private:
  struct DisplayDepthPair {
    struct RITexture *image = nullptr;
    struct RITextureView *attachmentView = nullptr;
  };

  struct DisplayDepthResources {
    DisplayDepthInputs inputs = {};
    DisplayDepthPair presentation = {};
    DisplayDepthPair scene = {};
  };

  // Builds the policy inputs and captures each candidate's resolved pair once.
  DisplayDepthResources BuildDisplayDepthInputs() const;
  DisplayDepthPair ResolveDisplayDepth(
      const DisplayDepthExtent *apRequestedExtent = nullptr) const;
  // Logical negotiated/clamped extent guard only; it does not prove the
  // scene depth image was allocated at that extent.
  bool RenderExtentEqualsDisplayExtent() const;

  // Hands the pogo halves' GPU resources to the frame freelist.
  void ReleasePogoBuffer(cGraphics::FrameContext *cntx);
  bool PrepareTemporalProvider(cGraphics::FrameContext *cntx,
                               bool worldWillRender);
  void ReleaseTemporalProvider();
  void ClearTemporalProviderFailure();

  cScene *mpScene;

  cCamera *mpCamera;
  cWorld *mpWorld;

  bool mbActive;
  bool mbVisible;

  bool mbIsListener;

  iRenderer *mpRenderer;
  cPostEffectComposite *mpPostEffectComposite;

  RI_PogoBuffer mPogoBuffer = {};
  uint32_t mlPogoWidth = 0;
  uint32_t mlPogoHeight = 0;

  Event<const WorldDrawCtx &> m_onPreWorldDraw;
  Event<const PostTranslucenceDrawCtx &> m_onPostTranslucenceDraw;
  Event<const PostWorldDrawCtx &> m_onPostWorldDraw;
  Event<const WorldDrawCtx &> m_onPostDelivery;
  std::vector<cGuiSet *> m_guiSets;

  ViewportState m_state;
  Target mTarget = TargetSwapchain{};
  cVector2l mRenderExtent = cVector2l(0, 0);
  RasterCamera mRasterCamera = {};
  bool mTemporalHistoryReset = false;
  std::unique_ptr<cTemporalPresentation> mpTemporalPresentation;
  std::unique_ptr<cTemporalReactiveMask> mpTemporalReactiveMask;
  std::shared_ptr<iTemporalUpscaler> mpTemporalUpscalerProvider;
  TemporalUpscalerSettings mTemporalProviderPreparedSettings = {};
  TemporalUpscalerExtent mTemporalProviderPreparedRenderExtent = {};
  TemporalUpscalerExtent mTemporalProviderPreparedDisplayExtent = {};
  bool mbTemporalProviderPrepared = false;
  uint32_t mlTemporalJitterPhaseCount = 0;
  TemporalUpscalerSettings mTemporalUpscalerRequestedSettings = {};
  TemporalUpscalerSettings mTemporalUpscalerSettings = {};
  TemporalUpscalerStatus mTemporalUpscalerStatus = {};
  TemporalProviderFailureState mTemporalProviderFailure = {};
  const char *mpTemporalProviderFailureReason = nullptr;
  const char *mpTemporalProviderUnavailableReasonLogged = nullptr;
  bool mTemporalReactiveMaskActive = false;
  bool mTemporalPresentationDepthValid = false;
  // Tracks which source owns the current render extent. Native means the
  // (0,0) sentinel follows the display; the development override may restore
  // that sentinel only while it owns the extent, so it cannot stomp a
  // provider-owned extent. The provider takes precedence over the override.
  eRenderExtentOwner mRenderExtentOwner = eRenderExtentOwner::Native;
  bool mbDevRenderScaleIgnoredLogged = false;
  std::unique_ptr<cRenderSettings> mpRenderSettings;

  uint32_t mlLastEvaluatedFrame = UINT32_MAX; // once-per-frame guard
};

//------------------------------------------

}; // namespace hpl
#endif // HPL_VIEWPORT_H
