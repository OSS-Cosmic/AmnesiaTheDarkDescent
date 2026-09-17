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

#ifndef HPL_GRAPHICS_H
#define HPL_GRAPHICS_H

#include "engine/Interface.h"
#include "engine/Updateable.h"
#include "graphics/RIResourceUploader.h"
#include "math/MathTypes.h"
#include "system/Event.h"
#include "system/SystemTypes.h"

#include "graphics/GraphicsTypes.h"
#include "graphics/RIProgram.h"
#include "graphics/RIScratchAlloc.h"
#include "graphics/RISwapchain.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"

// Render-interface state merged into cGraphics (formerly RIBootstrap).
#include "engine/EngineInitVars.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITimeline.h"
#include "resources/ResourceBase.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace hpl {

class cResources;
class iRenderer;
class cPostEffectsComposite;
class iPostEffectType;
class iPostEffectParams;
class iPostEffect;
class iLowLevelResources;
class cWindow;
class cMeshCreator;
class cTextureCreator;
class cDecalCreator;
class DebugDraw;
class iMaterialType;
class cPostEffectComposite;
class cParserVarContainer;
struct cTexture;
// Owned by cGraphics as `globalset` (pointer breaks the
// GlobalManagedSets.h -> Graphics.h header cycle).
class GlobalManagedSets;
class cLightProbeQuery;
class cGpuParticleSystem;

typedef std::list<cPostEffectComposite *> tPostEffectCompositeList;
typedef tPostEffectCompositeList::iterator tPostEffectCompositeListIt;

typedef std::list<iPostEffect *> tPostEffectList;
typedef tPostEffectList::iterator tPostEffectListIt;

typedef std::map<tString, iMaterialType *> tMaterialTypeMap;
typedef tMaterialTypeMap::iterator tMaterialTypeMapIt;

//------------------------------------------------------
class cGraphics : public iUpdateable {
public:
  cGraphics(cWindow *apWindow, iLowLevelResources *apLowLevelResources);
  ~cGraphics();

  // Initializes the window, RI backend, and render systems. Init either
  // succeeds or does not return: unrecoverable failures (no Vulkan, no
  // adapter, device/swapchain creation) FatalError (message box + exit).
  void Init(const cEngineInitVars::cGraphicsVars &aVars,
            cResources *apResources, tFlag alHplSetupFlags);

  void Update(float afTimeStep);

  // The OS window (SDL). NULL never — created by the engine setup even for
  // headless runs (it just stays un-Init'd there).
  cWindow *GetWindow() { return mpWindow; }

  iRenderer *GetRenderer(eRenderer aType);
  // The lit renderer for one specific backend, or NULL when it was not built.
  // Only ever non-NULL for both backends when the editor asked for a runtime
  // switch and the device came up ray-tracing-capable.
  iRenderer *GetLitRenderer(eRendererBackend aBackend);
  // True when both lit renderers exist, i.e. SetRendererBackend can succeed.
  bool CanSwitchRendererBackend() const;
  // Repoints eRenderer_Main (and GetRendererBackend) at the other lit
  // renderer. No GPU work and nothing destroyed: the per-viewport target sets
  // swap themselves on the next Draw (cViewport::PrepareToRender). Returns
  // false, changing nothing, when that renderer was not built.
  bool SetRendererBackend(eRendererBackend aBackend);

  // Ask for a different lit renderer. Applied at the next frame boundary, not
  // here: callers are typically inside a frame with the primary command buffer
  // open, and the switch destroys a renderer. Same request/apply split as
  // SetVsync. Repeated requests in one frame collapse to the last one.
  void RequestRendererBackend(eRendererBackend aBackend);
  // Whether the DEVICE could host the other lit renderer. The device's
  // capability mode is fixed for the session, so this is what decides if a
  // switch is offered at all.
  bool CanHostRendererBackend(eRendererBackend aBackend) const;

  // cScene registers these: viewports must let go of the outgoing renderer
  // before it is destroyed, and pick the incoming one up after it is built.
  // cGraphics owns the timing; the scene owns what is attached to what.
  void SetRendererBackendHandlers(std::function<void(iRenderer*)> aDetach,
                                  std::function<void(eRendererBackend)> aAdopt);

private:
  // Applies a pending RequestRendererBackend at a frame boundary.
  void ApplyPendingBackendSwitch();

public:
  void ReloadRendererData();

  cPostEffectComposite *CreatePostEffectComposite();
  void DestroyPostEffectComposite(cPostEffectComposite *apComposite);

  void AddPostEffectType(iPostEffectType *apPostEffectBase);

  iPostEffect *CreatePostEffect(iPostEffectParams *apParams);
  void DestroyPostEffect(iPostEffect *apPostEffect);

  void AddMaterialType(iMaterialType *apType, const tString &asName);
  iMaterialType *GetMaterialType(const tString &asName);
  tStringVec GetMaterialTypeNames();
  void ReloadMaterials();

  cMeshCreator *GetMeshCreator() { return mpMeshCreator; }
  cTextureCreator *GetTextureCreator() { return mpTextureCreator; }
  cDecalCreator *GetDecalCreator() { return mpDecalCreator; }

  // Editor / debug overlay batcher (global so thumbnails and previews
  // can reuse it). Only created with eHplSetup_Screen.
  DebugDraw *GetDebugDraw() { return mpDebugDraw; }

  bool GetScreenIsSetUp() { return mbScreenIsSetup; }

  eRendererBackend GetRendererBackend() const;
  // True when the selected adapter can run the ray-traced backend,
  // whichever backend this session actually started.
  bool IsRayTracedSupported() const { return mbRayTracedSupported; }

  // The window/screen size is owned by cWindow — query it via
  // Interface<cWindow>::Get()->GetSize() / GetSizeF(). The swapchain follows
  // the window; BeginActiveSet reconciles the two each frame.

  // ============================================================
  // Render-interface state (merged from the former RIBootstrap;
  // public because external code reaches it via
  // Interface<cGraphics>::Get()).
  // ============================================================

  // CPU-side queue of deferred resource releases latched to a GPU timeline
  // value (graphicsTimeline). Entries release in their destructor; stored
  // directly in the variant. `entries` is a FIFO of pending deferrals;
  // `batches` a ring of {timeline value, count} partitioning it across
  // in-flight frames. Single-threaded (render thread).
  struct FrameDeferral {
    static constexpr size_t CAPACITY = RI_NUMBER_FRAMES_FLIGHT + 2;

    using Entry =
        std::variant<SharedResourcePin, RISharedPointer<RIAccelStructure>,
                     RISharedPointer<RISampler>, RISharedPointer<RITextureView>,
                     RISharedPointer<RIBuffer>, RISharedPointer<RITexture>,
                     RISharedPointer<RISwapchain>, std::function<void()>>;

    struct Batch {
      uint64_t value;
      size_t count;
    };

    // Queue a deferral for the frame currently being built.
    template <typename T> void push(T e) {
      if constexpr (std::is_same_v<T, RISharedPointer<RIAccelStructure>> ||
                    std::is_same_v<T, RISharedPointer<RISampler>> ||
                    std::is_same_v<T, RISharedPointer<RITextureView>> ||
                    std::is_same_v<T, RISharedPointer<RIBuffer>> ||
                    std::is_same_v<T, RISharedPointer<RITexture>> ||
                    std::is_same_v<T, RISharedPointer<RISwapchain>>) {
        if (!e.isEmpty()) {
          entries.push_back(std::move(e));
          ++pendingCount;
        }
      } else {
        entries.push_back(std::move(e));
        ++pendingCount;
      }
    }

    // At submit: seal everything accumulated since the last seal() under
    // this frame's timeline value. No-op when nothing was deferred.
    void seal(uint64_t value) {
      if (pendingCount == 0)
        return;
      assert(batchCount < CAPACITY && "FrameDeferral ring overflow");
      batches[(head + batchCount) % CAPACITY] = {value, pendingCount};
      pendingCount = 0;
      ++batchCount;
    }

    // At frame start: release every sealed batch whose value the GPU has
    // reached. Batches seal in non-decreasing value order.
    void drain(uint64_t completed) {
      while (batchCount > 0 && batches[head].value <= completed) {
        releaseFront(batches[head].count);
        head = (head + 1) % CAPACITY;
        --batchCount;
      }
    }

    // Teardown: release everything. Caller must ensure the GPU is idle.
    void drainAll() {
      releaseFront(entries.size());
      head = 0;
      batchCount = 0;
      pendingCount = 0;
    }

    std::deque<Entry> entries; // FIFO, oldest at front
    std::array<Batch, CAPACITY> batches = {};
    size_t head = 0; // ring window [head, head+batchCount) modulo CAPACITY
    size_t batchCount = 0;
    size_t pendingCount = 0; // entries pushed since the last seal()

  private:
    void releaseFront(size_t n) {
      for (size_t i = 0; i < n; ++i) {
        Entry &e = entries.front();
        if (auto *op = std::get_if<std::function<void()>>(&e); op && *op)
          (*op)();
        entries.pop_front();
      }
    }
  };

  // Raster G-buffer output — packed TriangleHit (uint4).
  static constexpr RI_Format_e VisibilityFormat = RI_FORMAT_RGBA32_UINT;
  // Depth+stencil: depth aspect drives Z passes; stencil aspect used by
  // cLuxEffectRenderer's outline pass.
  static constexpr RI_Format_e DepthFormat = RI_FORMAT_D32_SFLOAT_S8_UINT;
  // Screen-space motion vectors (gbuffer 2nd MRT), RG16F.
  static constexpr RI_Format_e VelocityFormat = RI_FORMAT_RG16_SFLOAT;
  // HDR format for the pogo ping-pong buffer and every post-effect
  // intermediate target that feeds it. Single source of truth.
  static constexpr RI_Format_e PogoColorFormat = RI_FORMAT_RGBA16_SFLOAT;

  struct FrameContext {
    struct RIScratchAlloc uboScratchAlloc;
    struct RIScratchAlloc accelScratchAlloc;
  };

  RIDevice device;
  RIProgram gui;
  // Fullscreen passthrough used to blit the renderer's pogo "read" half
  // to the swapchain.
  RIProgram postEffectBlit;

  // 1x1 white texture used as the default texture binding.
  struct RITexture whiteTexture2D;
  struct RITextureView whiteTexture2DView;

  // Zero-filled vertex buffer for absent vertex input slots.
  struct RIBuffer nulVertexBuffer;

  // Default-value fallback vertex streams (normal +Z, tangent +X, color
  // white, uv 0), one vertex each, bound with zero stride.
  struct RIBuffer fallbackNormalVertex;
  struct RIBuffer fallbackTangentVertex;
  struct RIBuffer fallbackColorVertex;
  struct RIBuffer fallbackUv0Vertex;

  // The swapchain follows the window size (cWindow), which BeginActiveSet
  // reconciles it to each frame. Consumers that need the render size pull it
  // from Interface<cWindow>::Get() each use (viewports via GetTargetSize, GUI
  // sets via GetVirtualSize). Consumers that instead *snapshot* the size (and
  // derive layout from it once) subscribe to cWindow::OnScreenSizeChanged().
  // Ref-counted so a resize can park the retiring swapchain (which owns its
  // per-image views) in graphicsDefer while in-flight frames still reference
  // it, and the replacement is a direct assignment (see the rebuild in
  // BeginActiveSet).
  RISharedPointer<RISwapchain> swapchain;

  RICommandRingBuffer<RI_COMMAND_RING_POOL_COUNT, RI_COMMAND_RING_CMD_PER_POOL>
      graphicsCmdRing;
  struct RICommandRingElement primary;
  struct RICommandRingElement blasSubmit;

  FrameDeferral graphicsDefer;
  RITimeline graphicsTimeline;

  // Per-pass GPU timing + debug-utils labels.
  RIGpuProfiler profiler;

  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiVertexAlloc;
  RISharedPointer<RIBuffer> guiVertexBuffer;
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiIndexAlloc;
  RISharedPointer<RIBuffer> guiIndexBuffer;

  // Per-viewport camera-facing translucent geometry (particles).
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> translucentVtxAlloc;
  RISharedPointer<RIBuffer> translucentVtxBuffer;
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> translucentIdxAlloc;
  RISharedPointer<RIBuffer> translucentIdxBuffer;

  std::array<FrameContext, RI_NUMBER_FRAMES_FLIGHT> frameSets;
  // Direct-indexed sampler cache keyed by the three wrap modes and filter
  // mode (4*4*4*3 = 192). cookie == 0 means uninitialized.
  static constexpr size_t kSamplerCombinationCount =
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureFilter_LastEnum);
  std::array<RISampler, kSamplerCombinationCount> cachedSamplers;

  uint32_t swapchainIndex = 0;
  uint32_t frameIndex = 0;

  // Renderer requirement, not a debug override: when set, every light's shadow
  // flag is treated as set AND every opaque instance blocks shadow rays,
  // regardless of the authored CastShadows / eRenderableFlag_ShadowCaster bits.
  // The path tracer's next-event estimation shades the analytic lights at every
  // indirect bounce hit, so an unshadowed light does not just leak locally: the
  // error is accumulated temporally and smeared by the a-trous filter into a
  // room-wide DC offset. The legacy authored-flag behaviour stays reachable via
  // the debug checkbox for A/B comparison.
  bool allLightsCastShadows = true;

  // Shared manual override for the render/display resolution split:
  // 1.0f means native (the (0,0) render-extent sentinel); values in (0,1)
  // make every viewport shade at a reduced render extent while presenting at
  // the display extent. This exercises the spatial-fallback and display-depth
  // reconstruction paths without an FSR/XeSS provider; it wires no provider
  // and enables no camera jitter. Written by the game's saved Graphics/
  // RenderScale setting (cLuxConfigHandler::SetRenderScale) as well as by the
  // temporary debug menu / editor options overrides. A prepared provider owns
  // the extent and takes precedence; this value applies when no provider is
  // prepared.
  float devRenderScale = 1.0f;

  struct RIResourceUploader uploader = {};

  // Engine-lifetime set-0 tables (bindless / object / material / light);
  // built by InitGlobalManagedSets, freed by ShutdownGlobalManagedSets.
  GlobalManagedSets *globalset = nullptr;

  // Engine-lifetime gameplay illumination sensor. Gameplay submits world points
  // and reads back how lit they are a couple of frames later; the renderer
  // evaluates them in LightProbePass.cs alongside the frame it is already
  // shading. Null until Init (and on a build with no device), so every caller
  // null-checks — see cLightProbeQuery for why "no answer yet" is a state
  // consumers must handle anyway.
  cLightProbeQuery *lightProbe = nullptr;

  // Engine-lifetime GPU particle pool: the persistent per-particle state
  // buffer, the authored-parameter table, and the slice allocator that gives
  // each live emitter a fixed range of the pool. Null until Init (and on a
  // build with no device), and inert unless HPL_GPU_PARTICLES is set, so every
  // caller null-checks and falls back to the legacy CPU particle path.
  cGpuParticleSystem *gpuParticles = nullptr;

  void IncrementFrame();
  std::optional<RIDescriptor> resolve_filter_descriptor(eTextureWrap wrapS,
                                                        eTextureWrap wrapT,
                                                        eTextureWrap wrapR,
                                                        eTextureFilter filter);
  // Default 1x1 white texture descriptor, produced on demand.
  RIDescriptor whiteTexture2DDescriptor();
  FrameContext *GetActiveSet() {
    return &frameSets[frameIndex % RI_NUMBER_FRAMES_FLIGHT];
  }

  void UpdateFrameUBO(RIDescriptor *descriptor, void *data, size_t size);

  // Claim per-frame segments of the translucent scratch buffers.
  bool RequestTranslucentVtx(FrameContext *cntx, size_t numFloats,
                             struct RISegmentReq *req);
  bool RequestTranslucentIdx(FrameContext *cntx, size_t numIndices,
                             struct RISegmentReq *req);

  void CloseAndSubmitActiveSet();
  void BeginActiveSet();

  void DestroyRenderObjects();
  void Dispose();

  void SetVsync(bool vsync);

private:
  // Grow-and-recreate helper for the per-frame scratch buffers.
  bool RequestScratchSegment(RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> &alloc,
                             RISharedPointer<RIBuffer> &buffer,
                             uint16_t elementStride, uint32_t usage,
                             size_t numElements, struct RISegmentReq *req);

  bool m_vsync = false;
  bool m_requestedVsync = false;
  bool m_disposed = false;
  bool m_forceSwapchainRebuild = false;
  bool m_frameAcquired = false;

  cWindow *mpWindow = nullptr;
  iLowLevelResources *mpLowLevelResources = nullptr;
  cMeshCreator *mpMeshCreator = nullptr;
  cTextureCreator *mpTextureCreator = nullptr;
  cDecalCreator *mpDecalCreator = nullptr;
  DebugDraw *mpDebugDraw = nullptr;
  cResources *mpResources = nullptr;
  eRendererBackend mRendererBackend = eRendererBackend_RayTraced;
  bool mbRayTracedSupported = false;
  // Editors only: bring the device up ray-tracing-capable whenever the adapter
  // allows it and build BOTH lit renderers, so the backend can change without
  // a restart. The game leaves this false and builds exactly one.
  bool mbRuntimeBackendSwitchAllowed = false;

  // Pending backend switch, applied at the top of BeginActiveSet.
  eRendererBackend mRequestedBackend = eRendererBackend_Standard;
  bool mbBackendSwitchPending = false;
  std::function<void(iRenderer*)> mBackendDetachHandler;
  std::function<void(eRendererBackend)> mBackendAdoptHandler;

  // Owning: one entry per backend, NULL when that renderer was not built.
  iRenderer *mLitRenderers[eRendererBackend_LastEnum] = {};
  // Owning: every renderer built, in creation order. Update/ReloadRendererData/
  // teardown walk this, never mvRenderers — mvRenderers[eRenderer_Main] is an
  // alias of one mLitRenderers entry and would double-free.
  std::vector<iRenderer *> mvOwnedRenderers;
  // Non-owning eRenderer lookup table.
  std::vector<iRenderer *> mvRenderers;
  std::vector<iPostEffectType *> mvPostEffectTypes;

  tPostEffectCompositeList mlstPostEffectComposites;
  tMaterialTypeMap m_mapMaterialTypes;
  tPostEffectList mlstPostEffects;

  bool mbScreenIsSetup;
};

}; // namespace hpl
#endif // HPL_GRAPHICS_H
