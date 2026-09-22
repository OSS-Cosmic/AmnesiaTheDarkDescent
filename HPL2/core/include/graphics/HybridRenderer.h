#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/GlobalManagedSets.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList.h"
#include "graphics/Renderer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include "graphics/StandardHiZPass.h"
#include "graphics/StandardShadowCullPass.h"
#include "graphics/WaterReflectionPass.h"
#include <array>
#include <memory>

#include "Constants.h"

namespace hpl {

class Image;
class cVertexBuffer;

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // Per-viewport internals live in HybridViewportState (pure data — see
  // scene/Viewport.h), held by cViewport; this renderer owns its
  // creation/sizing (file-local helper in HybridRenderer.cpp).

  // kObjectSlotCapacity / kTextureSlotCapacity / kMaterialCapacity come from
  // amnesia/glsl/forward_shared.h.

  virtual void Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) override;

  virtual bool LoadData() override { return true; };
  virtual void DestroyData() override {};

  void SetOverlay(int alOverlay) { m_overlayMode = (uint32_t)alOverlay; }

private:
  cRenderList2 m_rendererList;

  // The object-slot cache now lives in m_global (GlobalManagedSets::
  // m_objectSlots); submitObject() owns slot allocation + the slot-generation
  // bump (on new occupant / index-count change / VB destroy), so the renderer no
  // longer holds the cache, the per-slot hooks, or a geometry-dirty array.

  // Default-value fallback vertex streams for renderables that omit an optional
  // stream now live on the cGraphics singleton (Interface<cGraphics>::Get()->fallback*Vertex), created once
  // at init; detail::BindVertexStreams binds them in the raster passes.

  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  // Host-built opaque commands whose instanceCount the two-phase cull owns.
  StagedIndirectBuffer m_indirectDrawBuffer;
  bool m_indirectDrawFirstUse = true;

  // ---------------------------------------------------------------------
  // GPU occlusion cull for the translucent families.
  //
  // The pyramid is built from the G-buffer's depth -- which is final opaque
  // depth, since nothing writes depth again before water/particles/meshes --
  // and the kernel zeroes the instanceCount word of any draw it can prove is
  // hidden behind it. Both passes are borrowed from the Standard renderer
  // unchanged; only the host wiring below is Hybrid's.
  //
  // Every family runs kStandardCullModeInstanceMask, the only mode that leaves
  // slot order alone: translucency is drawn back-to-front and a compacting
  // mode would reorder it.
  // ---------------------------------------------------------------------
  std::unique_ptr<cStandardHiZPass> m_hiZ;
  std::unique_ptr<cStandardShadowCullPass> m_cull;
  bool m_cullLoaded = false;

  // One candidate and one 5-word command slot per DRAW (not per renderable:
  // the mesh path emits a second draw for the cube-map variant).
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullCandidateSegment;
  struct RIBuffer m_cullCandidateBuffer;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullCommandSegment;
  StagedIndirectBuffer m_cullCommandBuffer;
  bool m_cullCommandFirstUse = true;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullTileSegment;
  struct RIBuffer m_cullTileBuffer;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullGroupSegment;
  struct RIBuffer m_cullGroupBuffer;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullCameraSegment;
  struct RIBuffer m_cullCameraBuffer;
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cullDrawCountSegment;
  struct RIBuffer m_cullDrawCountBuffer;
  // Persistent visibility table for the opaque two-phase cull. Device-local:
  // the kernel reads and writes it, which a host-mapped D3D12 buffer cannot
  // express. Seeded to zero through the uploader.
  struct RIBuffer m_cullVisibilityBuffer;
  // Opaque two-phase camera cull: per-frame candidates, and the opt-in gate.
  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_cameraCandidateSegment;
  struct RIBuffer m_cameraCandidateBuffer;

  // One family's reserved slice of those rings, plus the host-mapped pointers
  // into it. Filled by ReserveCull, consumed by WriteCullDraw + DispatchCull.
  struct TranslucentCull {
    bool usable = false;
    uint32_t candidateBase = 0;
    uint32_t commandBase = 0;   // in 5-word slots
    uint32_t tileBase = 0;
    uint32_t groupBase = 0;
    uint32_t capacity = 0;
    uint32_t groupCapacity = 0;
    StandardCullCandidate *candidates = nullptr;
    uint32_t *commandWords = nullptr;   // 5 per slot, from commandBase
    StandardCullTile *tile = nullptr;
    StandardCullGroup *groups = nullptr;
    uint32_t commandCount = 0;          // draws written so far
  };

  // Reserves one family's ranges and fills its tile record. `cameraIndex` is
  // the ring offset of the StandardCullCamera this Draw published, or
  // kStandardCullNoCamera to frustum-cull only. Returns false -- leaving `out`
  // unusable -- if anything does not fit, in which case the caller draws
  // directly as it did before.
  bool ReserveCull(uint32_t worstCase, const cMatrixf &viewProjection,
                   uint32_t cameraIndex, TranslucentCull &out);
  // Writes one draw's command words and its candidate. `slot` is the draw
  // index within the family, `elementCount` the index count when indexed and
  // the vertex count otherwise.
  void WriteCullDraw(TranslucentCull &cull, uint32_t slot, bool indexed,
                     uint32_t elementCount, uint32_t objectSlot,
                     const cVector3f &boundsMin, const cVector3f &boundsMax,
                     bool isStatic, bool neverOcclude);
  // Records the cull for one family. Must be called outside dynamic rendering.
  bool DispatchCull(RICmd *cmd, TranslucentCull &cull,
                    struct RITextureView *hiZ);

  // The ray-tracing TLAS (storage + instance buffer) is owned by cWorld and built
  // in cWorld::PrepareFrame; each RT pass binds it via apWorld->GetTlas().

  // (The per-viewport frame textures — packed hit info,
  // velocity, direct-lighting history + ping-pong index/init, prev-frame
  // camera — live on cViewport::HybridViewportState; their Update/Dispose
  // are defined in HybridRenderer.cpp.)

  // prepare gbuffer
	RIProgram m_gbuffer;

  // VBuffer POM barycentric correction compute pass (amnesia/slang/VBuffer/).
  // Copies visibilityTexture (raw raster hit) → packedHitInfoTexture and
  // perturbs barycentrics on height-mapped diffuse surfaces so downstream
  // getVertexData() reconstructs the parallax-occluded point.
  // There is no refraction pass: water/glass pixels keep the rasterized
  // front-surface hit, so nothing behind them is bent.
  RIProgram m_vBufferPomBary;

  // Clustered light-grid build (LightGridBuildPass.cs). Bins the frame's
  // lights into the world-space cell grid that next-event estimation reads:
  // both the direct-lighting pass and the path tracer's getCellLights()
  // depend on it, so it runs before either.
  RIProgram m_lightGrid;

  // Per-pixel path tracer (amnesia/slang/PathTracer/PathTracePass.rt.slang).
  // Rooted at the primary V-buffer hit: one cosine-sampled path per pixel,
  // NEE per vertex, writing demodulated indirect irradiance + the
  // (viewZ, normal) denoiser key. The rgen drives an iterative trace loop
  // (no recursive TraceRay); closest-hit handles material shading +
  // next-event estimation using an inline ray-query for shadow visibility.
  RIProgram m_pathTrace;

  // Final composite (amnesia/slang/Composite/MainCompositePass.cs.slang). Reads
  // gPackedHitInfo + the two denoised indirect channels (gIndirectLighting,
  // gIndirectSpecular) + gDirectLighting (the ReSTIR DI chain's denoised term,
  // summed with the indirect diffuse here rather than in the path tracer) and
  // writes gOutput.
  RIProgram m_composite;

  // ReSTIR DI passes, followed by a per-viewport RELAX diffuse denoiser.
  // DirectLightingPass.cs — temporal reservoir reuse against the previous
  // frame's key; writes this frame's key and the merged reservoir.
  RIProgram m_directLighting;
  // ReSTIR DI spatial reuse + one shadow ray to resolve (DirectSpatialReusePass.cs).
  RIProgram m_directSpatialReuse;
  // Packs indirect diffuse and specular into NRD's layout for REBLUR.
  RIProgram m_nrdPack;

  // LightProbePass.cs — the gameplay illumination sensor. Unlike every other
  // pass here it writes nothing the frame displays: it answers "how lit is this
  // world point?" for a handful of points gameplay asked about, into a buffer
  // copied back to the CPU. Runs right after the light-grid build, since it
  // walks the same grid and traces the same TLAS. Skipped entirely on frames
  // with no pending probes. See cLightProbeQuery.
  RIProgram m_lightProbe;


	// Particle (translucent) pass — port of legacy RendererDeferred's
	// translucency_particle.{vert,frag}.fsl. Reuses the opaque object/material
	// bindless pools (one OBJECT slot + one MATERIAL slot per emitter per
	// frame); cHybridRenderer writes the particle VB's BDA into the same
	// opaque*Handles[] arrays so the VS can pull pos/uv/color via instanceId.
	// Hardware blend state varies per blend mode — one pipeline per mode is
	// stamped on demand via the program's PipelineSlot cache.
	RIProgram m_particle;

	// Non-particle translucent meshes (glass, lamp glass, decals tagged
	// translucent, etc.). Renders in its own pass after the particle pass into
	// the same pogo "read" half, depth read-only. One pipeline per
	// eMaterialBlendMode (Add/Mul/MulX2/Alpha/PremulAlpha) is stamped on demand
	// via the program's PipelineSlot cache, mirroring m_particle. Refraction
	// and cube-map reflection materials are filtered out at the call site —
	// those need a screen-color copy + cube-map binding the renderer doesn't
	// have yet.
	RIProgram m_translucentMesh;

	// Decal overlay pass. Thin clipped meshes (blood / scorch / impact marks,
	// posters) drawn after the translucent mesh pass into the same pogo "read"
	// half, depth read-only. Reuses the translucent 5-stream fixed-function
	// vertex layout, the m_diffuseBindless / m_objectBuffer object pool,
	// and resolveMaterial (DiffuseMaterial slot — only tex[0] diffuse is used).
	// One pipeline per eMaterialBlendMode is stamped via the program's
	// PipelineSlot cache. Port of decal.frag.fsl / decal.vert.fsl.
	RIProgram m_decal;
	RIProgram m_water;
	WaterReflectionPass m_waterReflection;

	struct OverlayPushConstants { uint32_t overlayMode; };
	uint32_t m_overlayMode = kDefaultOverlayMode;
};

} // namespace hpl

#endif
