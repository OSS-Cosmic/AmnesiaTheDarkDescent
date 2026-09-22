#include "graphics/HybridRenderer.h"
#include "graphics/Image.h" // Image::GetBindlessSlot (light gobos read the slot)
#include "graphics/RITypes.h"

#include "graphics/DebugDraw.h"
#include "graphics/DecalPipelineDesc.h"
#include "graphics/GBufferMRTPipelineDesc.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/LightProbeQuery.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/ParticlePipelineDesc.h"
#include "graphics/PathTracePayload.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/Graphics.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/Renderable.h"
#include "graphics/TemporalReactiveMask.h"
#include "graphics/TemporalUpscalerPolicy.h"
#include "graphics/TranslucentMeshPipelineDesc.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "scene/Viewport.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "scene/Decal.h"
#include "scene/FogArea.h"
#include "scene/Light.h"
#include "scene/LightArea.h"
#include "scene/LightSpot.h"
#include "scene/ParticleEmitter.h"
#include "scene/RenderableSet.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>


namespace hpl {

// Ring capacities for the translucent occlusion cull. A family that needs more
// draws than kHybridCullMaxDraws falls back to direct draws for that family
// rather than culling part of its list.
static constexpr uint32_t kHybridCullMaxDraws = 4096;
// Opaque two-phase camera cull. Its candidates get their own ring: the
// translucent families cap at kHybridCullMaxDraws, but the opaque set is
// whatever the frustum keeps and is addressed through the much larger
// m_indirectDrawBuffer, so it must not inherit the translucent ceiling.
static constexpr uint32_t kHybridCameraMaxDraws = 16384;
// Persistent visibility slots, hashed from the renderable cookie. A collision
// makes phase 1 draw something hidden (depth rejects it) or skip something
// visible (phase 2 draws it): efficiency, never correctness.
static constexpr uint32_t kHybridCullVisibilityKeys = 65536;
// One tile per family per frame (water, particles, meshes), times the frames
// the rings keep in flight, with slack.
static constexpr uint32_t kHybridCullMaxTiles = 64;
static constexpr uint32_t kHybridCullMaxCameras = 16;
// Groups are shared by the translucent families AND the opaque two-phase cull,
// whose candidate set is the much larger kHybridCameraMaxDraws. Sizing this for
// the translucent families alone would make the opaque group request fail in
// any scene past ~8k draws, silently turning the cull off rather than breaking
// anything -- the kind of limit that never shows up as a bug report.
static constexpr uint32_t kHybridCullMaxGroups =
    (kHybridCullMaxDraws + kHybridCameraMaxDraws) / kStandardCullGroupSize +
    kHybridCullMaxTiles;
// The command ring is addressed by the kernel as a flat uint[] through
// gCullIndirectWords, and a slot is five words.
static constexpr uint32_t kHybridCullCommandWords =
    static_cast<uint32_t>(sizeof(VkDrawIndexedIndirectCommand) / sizeof(uint32_t));

namespace detail {

static inline bool BindVertexStreams(struct RICmd *cmd, cVertexBuffer *pVB,
                                     const char *passLabel,
                                     uint32_t *outPresentMask) {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  auto *vbri = static_cast<cVertexBuffer *>(pVB);
  auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
    const auto *element = vbri->GetElement(type);
    return element ? element->GetBuffer() : nullptr;
  };
  // Position + index are the only truly required streams — without geometry
  // there's nothing to draw.
  RIBuffer *pos = bufOf(eVertexBufferElement_Position);
  RIBuffer *idx = vbri->GetIndexRIBuffer();
  if (!pos || !idx) {
    Warning("%s mesh missing position / index — skipping", passLabel);
    return false;
  }
  // Optional streams: bind the real buffer when present, else the global
  // single-vertex default (normal = +Z, tangent = +X/handedness, color = white,
  // uv = 0). No capacity limit — the pipeline zeroes the absent binding's
  // stride, so the one fallback element is reread for every vertex.
  RIBuffer *nrm = bufOf(eVertexBufferElement_Normal);
  RIBuffer *tan = bufOf(eVertexBufferElement_Texture1Tangent);
  RIBuffer *col = bufOf(eVertexBufferElement_Color0);
  RIBuffer *uv = bufOf(eVertexBufferElement_Texture0);
  uint32_t mask =
      eVertexElementFlag_Position; // required, present per check above
  if (nrm)
    mask |= eVertexElementFlag_Normal;
  if (tan)
    mask |= eVertexElementFlag_Texture1;
  if (col)
    mask |= eVertexElementFlag_Color0;
  if (uv)
    mask |= eVertexElementFlag_Texture0;
  if (outPresentMask)
    *outPresentMask = mask;
  RIBuffer *vertBufs[5] = {
      pos,
      nrm ? nrm : &pGraphics->fallbackNormalVertex,
      tan ? tan : &pGraphics->fallbackTangentVertex,
      col ? col : &pGraphics->fallbackColorVertex,
      uv ? uv : &pGraphics->fallbackUv0Vertex,
  };
  cmd->bindVertexBuffers<5>(0, 5, vertBufs);
  cmd->bindIndexBuffer(&pGraphics->device, idx, 0, RI_INDEX_TYPE_32);
  return true;
}

} // namespace detail

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources) {
  {
    // The global bindless descriptor set (set 0) and every buffer bound to it
    // are an engine-lifetime singleton, constructed in cGraphics::Init via
    // InitGlobalManagedSets() before any renderer exists. We just borrow its
    // layout here.
    const RIBindlessLayout externalLayouts[] = {
        mpGraphics->globalset->m_bindlessSet.layout()};
    {
      // Gbuffer pass: one source, two entry points (vsMain / psMain). Load it
      // once per stage rather than sharing one blob: Vulkan can select an entry
      // point at pipeline creation, but a multi-entry DXIL source compiles to a
      // lib_6_8 library that no graphics PSO can consume, so each stage needs
      // its own per-entry executable. loadShaderStage picks it from the entry
      // name on D3D12 and resolves to the same .spv on Vulkan.
      auto gbuffer_vs = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "VBufferRaster.3d", "vsMain");
      auto gbuffer_ps = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "VBufferRaster.3d", "psMain");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_vs,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_ps,
                                 "psMain"}};
      m_gbuffer.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.gbuffer");
    }

    auto loadComputeProgram = [&](RIProgram &prog, const char *name) {
      auto bin =
          RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE, bin}};
      prog.initialize(&mpGraphics->device, stages, externalLayouts, name);
    };
    // Compute load that passes the Slang entry-point name through to
    // ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                            name, entryPoint);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&mpGraphics->device, stages, externalLayouts, name);
    };
    // VBufferPomBary — compute pass that copies the raster V-buffer into
    // packedHitInfoTexture and applies parallax-occlusion barycentric
    // correction for height-mapped diffuse surfaces.
    loadSlangCompute(m_vBufferPomBary, "VBufferPomBary.cs", "csMain");
    // PathTracePass — per-pixel reference path tracer. One .spv, four entry
    // points (rayGen / ptMiss / ptCloseHit / ptAnyHit). Shadow rays use inline
    // RayQuery, so no second hit group is needed (SBT stays single-ray-type).
    {
      auto pt_bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "PathTracePass.rt");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN, pt_bin,
                                 "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS, pt_bin,
                                 "ptMiss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, pt_bin,
                                 "ptCloseHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT, pt_bin,
                                 "ptAnyHit"}};
      m_pathTrace.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.pathTrace");
    }
    // LightGridBuildPass — single compute entry (binLights) that bins
    // point/spot lights into the coarse world-space light grid each frame.
    loadSlangCompute(m_lightGrid, "LightGridBuildPass.cs", "binLights");
    // Composite — compute pass: one thread per pixel writes the composite
    // (albedo + inline decals + lighting) into the pogo attach bound as
    // gOutput. The renderer transitions the attach to GENERAL around the
    // dispatch and back to COLOR_ATTACHMENT_OPTIMAL afterwards.
    loadSlangCompute(m_composite, "MainCompositePass.cs", "csMain");
    loadSlangCompute(m_directLighting, "DirectLightingPass.cs", "csMain");
    loadSlangCompute(m_directSpatialReuse, "DirectSpatialReusePass.cs",
                     "csMain");
    loadSlangCompute(m_nrdPack, "NrdPack.cs", "csMain");
    // Gameplay illumination sensor — see m_lightProbe in the header.
    loadSlangCompute(m_lightProbe, "LightProbePass.cs", "csMain");
    {
      // Particle pass (amnesia/slang/Particle).
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag,
                                 "psMain"}};
      m_particle.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.particle");
    }
    {
      // Translucent mesh pass (amnesia/slang/Translucent). Shares
      // externalLayouts with m_particle so the same bindless set / per-frame
      // UBO bindings light up.
      auto t_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.vert");
      auto t_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.frag");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, t_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, t_frag,
                                 "psMain"}};
      m_translucentMesh.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.translucentMesh");
    }
    {
      // Decal pass (amnesia/slang/Decal). Reuses the translucent 5-stream
      // vertex layout + bindless/UBO layouts.
      auto d_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.vert");
      auto d_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.frag");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, d_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, d_frag,
                                 "psMain"}};
      m_decal.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.decal");
    }
    {
      auto w_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.vert");
      auto w_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.frag");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, w_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, w_frag,
                                 "psMain"}};
      m_water.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.water");
      m_waterReflection.Initialize(mpGraphics, apResources);
    }

    RISegmentAllocDesc indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    // Host-built, with the opaque cull rewriting instanceCount -- staged on
    // D3D12, where an upload heap cannot be a UAV.
    m_indirectDrawBuffer.Create(&mpGraphics->device, indirectDesc.maxElements,
                                sizeof(VkDrawIndirectCommand),
                                /*hostWritten*/ true,
                                "HybridRenderer.indirectDraws");
    m_indirectDrawFirstUse = true;

    // --- GPU occlusion cull for the translucent families -----------------
    //
    // Host-mapped where the host writes and the GPU only reads, so a staged
    // copy would buy nothing. Anything the kernel writes is device-local or a
    // StagedIndirectBuffer instead: D3D12 upload heaps cannot be UAVs.
    const auto makeSegment =
        [&](RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> *segment, uint32_t elements,
            uint32_t stride) {
          RISegmentAllocDesc desc = {};
          desc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
          desc.elementStride = static_cast<uint16_t>(stride);
          desc.maxElements = elements;
          *segment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&desc);
        };
    const auto makeCullBuffer =
        [&](RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> *segment, uint32_t elements,
            uint32_t stride, uint32_t usage, const char *debugName,
            bool deviceLocal = false) {
          makeSegment(segment, elements, stride);
          return detail::CreateBindlessSlotBuffer(&mpGraphics->device, elements,
                                                  stride, usage, deviceLocal,
                                                  debugName);
        };
    m_cullCandidateBuffer =
        makeCullBuffer(&m_cullCandidateSegment, kHybridCullMaxDraws,
                       sizeof(StandardCullCandidate),
                       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                       "HybridRenderer.cullCandidates");
    // A uniform 5-word slot (the indexed command's size) for indexed and
    // non-indexed draws alike: every draw is its own drawIndirect with
    // drawCount 1, so Vulkan never reads the stride and a uniform one keeps
    // the kernel's word arithmetic trivial.
    makeSegment(&m_cullCommandSegment, kHybridCullMaxDraws,
                sizeof(VkDrawIndexedIndirectCommand));
    m_cullCommandBuffer.Create(&mpGraphics->device, kHybridCullMaxDraws,
                               sizeof(VkDrawIndexedIndirectCommand),
                               /*hostWritten*/ true,
                               "HybridRenderer.cullCommands");
    m_cullCommandFirstUse = true;
    m_cullTileBuffer =
        makeCullBuffer(&m_cullTileSegment, kHybridCullMaxTiles,
                       sizeof(StandardCullTile),
                       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                       "HybridRenderer.cullTiles");
    m_cullGroupBuffer =
        makeCullBuffer(&m_cullGroupSegment, kHybridCullMaxGroups,
                       sizeof(StandardCullGroup),
                       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                       "HybridRenderer.cullGroups");
    m_cullCameraBuffer =
        makeCullBuffer(&m_cullCameraSegment, kHybridCullMaxCameras,
                       sizeof(StandardCullCamera),
                       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                       "HybridRenderer.cullCameras");
    m_cullDrawCountBuffer = makeCullBuffer(
        &m_cullDrawCountSegment, kHybridCullMaxTiles, sizeof(uint32_t),
        RI_BUFFER_USAGE_INDIRECT | RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
        "HybridRenderer.cullDrawCounts", /*deviceLocal*/ true);
    // Persistent across frames -- phase 1 reads what the previous frame's phase
    // 2 wrote -- so it gets no segment allocator. The kernel reads and writes
    // it, so it is device-local and the zero seed is staged through the
    // uploader: every entry must start "not visible", which makes the first
    // frame draw everything in phase 2 and nothing in phase 1.
    m_cullVisibilityBuffer = detail::CreateBindlessSlotBuffer(
        &mpGraphics->device, kHybridCullVisibilityKeys, sizeof(uint32_t),
        RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_DST,
        /*deviceLocalOnly*/ true, "HybridRenderer.cullVisibility");
    if (!m_cullVisibilityBuffer.isEmpty()) {
      const size_t bytes =
          static_cast<size_t>(kHybridCullVisibilityKeys) * sizeof(uint32_t);
      RIResourceBufferTransaction seed = {};
      seed.target = m_cullVisibilityBuffer;
      seed.size = bytes;
      seed.offset = 0;
      seed.currentState = RI_RESOURCE_STATE_UNDEFINED;
      seed.currentStages = RI_STAGE_NONE;
      seed.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      seed.postStages = RI_STAGE_COMPUTE;
      RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                                 &seed);
      if (seed.mapped.data) {
        std::memset(seed.mapped.data, 0, bytes);
        RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                                 &seed);
      }
    }
    m_cameraCandidateBuffer = makeCullBuffer(
        &m_cameraCandidateSegment, kHybridCameraMaxDraws,
                       sizeof(StandardCullCandidate),
                       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE,
                       "HybridRenderer.cameraCullCandidates");

    m_hiZ = std::make_unique<cStandardHiZPass>(mpGraphics, apResources);
    m_cull = std::make_unique<cStandardShadowCullPass>(mpGraphics, apResources);
    // Non-fatal: without these the translucent families draw exactly as they
    // did before, just without occlusion culling.
    m_cullLoaded = m_hiZ->LoadData() && m_cull->LoadData();
    if (!m_cullLoaded)
      Error("Hybrid renderer: HiZ/cull passes failed to load; translucent "
            "occlusion culling is off this run\n");
  }
}

bool cHybridRenderer::ReserveCull(uint32_t worstCase,
                                  const cMatrixf &viewProjection,
                                  uint32_t cameraIndex, TranslucentCull &out) {
  out = TranslucentCull{};
  if (!m_cullLoaded || !m_cull || worstCase == 0 ||
      worstCase > kHybridCullMaxDraws)
    return false;
  // No camera means no pyramid was built this frame, so the only test left
  // would be the frustum -- and WalkAndPrepareRenderList already frustum-culled
  // this list on the CPU. Occlusion is the only new information the kernel has,
  // so without it there is nothing to win and a whole dispatch to lose.
  if (cameraIndex == kStandardCullNoCamera)
    return false;

  const uint32_t groupCount =
      (worstCase + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;

  // One request per ring, never one per draw: RISegmentAlloc restarts at 0 when
  // a request does not fit the tail, so per-draw requests could split a family
  // across the wrap and the tile's candidateBase could not describe it.
  RISegmentReq candidateReq = {};
  RISegmentReq commandReq = {};
  RISegmentReq tileReq = {};
  RISegmentReq groupReq = {};
  const uint32_t frame = mpGraphics->frameIndex;
  if (!m_cullCandidateSegment.request(frame, worstCase, &candidateReq) ||
      !m_cullCommandSegment.request(frame, worstCase, &commandReq) ||
      !m_cullTileSegment.request(frame, 1, &tileReq) ||
      !m_cullGroupSegment.request(frame, groupCount, &groupReq))
    return false;
  if (!m_cullCandidateBuffer.mappedAddress || !m_cullCommandBuffer.mapped() ||
      !m_cullTileBuffer.mappedAddress || !m_cullGroupBuffer.mappedAddress)
    return false;

  out.candidateBase = candidateReq.elementOffset;
  out.commandBase = commandReq.elementOffset;
  out.tileBase = tileReq.elementOffset;
  out.groupBase = groupReq.elementOffset;
  out.capacity = worstCase;
  out.groupCapacity = groupCount;
  out.candidates =
      reinterpret_cast<StandardCullCandidate *>(
          static_cast<uint8_t *>(m_cullCandidateBuffer.mappedAddress) +
          static_cast<size_t>(candidateReq.elementOffset) *
              sizeof(StandardCullCandidate));
  out.commandWords =
      reinterpret_cast<uint32_t *>(
          static_cast<uint8_t *>(m_cullCommandBuffer.mapped()) +
          static_cast<size_t>(commandReq.elementOffset) *
              sizeof(VkDrawIndexedIndirectCommand));
  out.tile = reinterpret_cast<StandardCullTile *>(
      static_cast<uint8_t *>(m_cullTileBuffer.mappedAddress) +
      static_cast<size_t>(tileReq.elementOffset) * sizeof(StandardCullTile));
  out.groups = reinterpret_cast<StandardCullGroup *>(
      static_cast<uint8_t *>(m_cullGroupBuffer.mappedAddress) +
      static_cast<size_t>(groupReq.elementOffset) * sizeof(StandardCullGroup));

  StandardCullTile tile{};
  StandardExtractFrustumPlanes(viewProjection.v, tile.planes);
  tile.planeCount = 6u;
  // A camera tile keeps everything the frustum keeps; the variability gate
  // exists for lights, which choose which casters they accept.
  tile.variabilityMask =
      kStandardCullVariabilityStatic | kStandardCullVariabilityDynamic;
  tile.cameraIndex = cameraIndex;
  // candidateBase/Count are finished in DispatchCull, once the family knows how
  // many draws it actually produced.
  *out.tile = tile;

  out.usable = true;
  return true;
}

void cHybridRenderer::WriteCullDraw(TranslucentCull &cull, uint32_t slot,
                                    bool indexed, uint32_t elementCount,
                                    uint32_t objectSlot,
                                    const cVector3f &boundsMin,
                                    const cVector3f &boundsMax, bool isStatic,
                                    bool neverOcclude) {
  if (!cull.usable || slot >= cull.capacity)
    return;

  uint32_t *words = cull.commandWords +
                    static_cast<size_t>(slot) * kHybridCullCommandWords;
  if (indexed) {
    // VkDrawIndexedIndirectCommand
    words[0] = elementCount;   // indexCount
    words[1] = 1u;             // instanceCount -- the word the kernel rewrites
    words[2] = 0u;             // firstIndex
    words[3] = 0u;             // vertexOffset
    words[4] = objectSlot;     // firstInstance
  } else {
    // VkDrawIndirectCommand, in the first four words of the same 5-word slot.
    words[0] = elementCount;   // vertexCount
    words[1] = 1u;             // instanceCount
    words[2] = 0u;             // firstVertex
    words[3] = objectSlot;     // firstInstance
    words[4] = 0u;
  }

  StandardCullCandidate &candidate = cull.candidates[slot];
  candidate = StandardCullCandidate{};
  candidate.aabbMinX = boundsMin.x;
  candidate.aabbMinY = boundsMin.y;
  candidate.aabbMinZ = boundsMin.z;
  candidate.aabbMaxX = boundsMax.x;
  candidate.aabbMaxY = boundsMax.y;
  candidate.aabbMaxZ = boundsMax.z;
  candidate.objectSlot = objectSlot;
  candidate.vertexCount = elementCount;
  // The shared predicate gates on the shadow-caster bit and the
  // static/dynamic variability mask before it reaches the frustum or occlusion
  // test. Neither gate means anything for a camera tile, so set the bits that
  // let every candidate through.
  candidate.renderFlags =
      kStandardCullShadowCasterBit | (isStatic ? kStandardCullStaticBit : 0u);
  candidate.cullFlags = neverOcclude ? kStandardCullFlagNeverOcclude : 0u;
  candidate.commandWordOffset =
      static_cast<uint32_t>(
          (static_cast<size_t>(cull.commandBase) + slot) * kHybridCullCommandWords) +
      1u;  // instanceCount

  if (slot + 1u > cull.commandCount)
    cull.commandCount = slot + 1u;
}

bool cHybridRenderer::DispatchCull(RICmd *cmd, TranslucentCull &cull,
                                   RITextureView *hiZ) {
  if (!cull.usable || cull.commandCount == 0 || !m_cullLoaded || !m_cull)
    return false;

  const uint32_t groupCount =
      (cull.commandCount + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
  if (groupCount > cull.groupCapacity)
    return false;

  cull.tile->candidateBase = cull.candidateBase;
  cull.tile->candidateCount = cull.commandCount;
  for (uint32_t group = 0; group < groupCount; ++group) {
    cull.groups[group].tileIndex = cull.tileBase;
    cull.groups[group].candidateOffset = group * kStandardCullGroupSize;
  }

  cStandardShadowCullPass::Buffers buffers{};
  buffers.candidates = &m_cullCandidateBuffer;
  buffers.tiles = &m_cullTileBuffer;
  buffers.groups = &m_cullGroupBuffer;
  buffers.indirect = m_cullCommandBuffer.gpu();
  buffers.drawCounts = &m_cullDrawCountBuffer;
  buffers.cameras = &m_cullCameraBuffer;
  buffers.visibility = &m_cullVisibilityBuffer;
  buffers.hiZ = hiZ;
  buffers.candidateCapacity = kHybridCullMaxDraws;
  buffers.indirectCapacity = kHybridCullMaxDraws;
  buffers.indirectWordCapacity = kHybridCullMaxDraws * kHybridCullCommandWords;
  buffers.tileCapacity = kHybridCullMaxTiles;
  buffers.groupCapacity = kHybridCullMaxGroups;
  buffers.drawCountCapacity = kHybridCullMaxTiles;
  buffers.cameraCapacity = kHybridCullMaxCameras;
  buffers.visibilityCapacity = kHybridCullVisibilityKeys;

  // Instance-mask: the host already wrote the whole command and only its
  // instanceCount is the kernel's. Slot order stays the host's back-to-front
  // sort, which is the whole point for translucency.
  //
  // The three families dispatch separately, so a later family's compute writes
  // land while an earlier family's indirect draws may still be reading the same
  // buffer. That is safe without an extra barrier only because each family
  // holds a DISJOINT ring range: RISegmentAlloc hands out non-overlapping
  // slices, so there is no write-after-read on any word. Dispatch's own closing
  // barrier covers the write-then-read this family needs.
  // D3D12 only: land this family's host-written commands in the device copy
  // the kernel rewrites and the draw reads.
  m_cullCommandBuffer.Flush(
      &mpGraphics->device, cmd,
      static_cast<uint64_t>(cull.commandBase) * sizeof(VkDrawIndexedIndirectCommand),
      static_cast<uint64_t>(cull.commandCount) * sizeof(VkDrawIndexedIndirectCommand),
      m_cullCommandFirstUse, /*cullFollows*/ true);
  m_cullCommandFirstUse = false;
  const bool dispatched =
      m_cull->Dispatch(cmd, mpGraphics->frameIndex, buffers, cull.tileBase, 1u,
                       cull.groupBase, groupCount, kStandardCullModeInstanceMask);
  // Flush left the device copy in STORAGE_WRITE for the kernel; without it the
  // caller draws directly, so restore the state every later Flush assumes.
  if (!dispatched && m_cullCommandBuffer.staged)
    cmd->vk_d3d12_bufferBarrier(RIBufferBarrier(
        m_cullCommandBuffer.gpu(), RI_RESOURCE_STATE_STORAGE_WRITE,
        RI_RESOURCE_STATE_INDIRECT_ARGUMENT, RI_STAGE_COPY,
        RI_STAGE_DRAW_INDIRECT));
  return dispatched;
}

void cViewport::HybridViewportState::Update(cGraphics::FrameContext *cntx,
                                            cVector2l size) {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  if (size.x <= 0 || size.y <= 0) {
    return;
  }
  const uint32_t renderW = (uint32_t)size.x;
  const uint32_t renderH = (uint32_t)size.y;
  if (width == renderW && height == renderH && targetWidth == renderW &&
      targetHeight == renderH) {
    return;
  }

  *this = {}; // defer the old resources, reset to empty (see operator=)

  width = renderW;
  height = renderH;
  // Guard band is disabled: the image extent is the negotiated render extent,
  // and the crop window is the whole authored rectangle inside that image.
  // These crop fields are INPUT-space and reserved for a future guard band.
  targetWidth = renderW;
  targetHeight = renderH;

  for (uint32_t i = 0; i < pGraphics->swapchain->imageCount; i++) {
    // Negotiated render-extent HDR color target: the compute composite writes
    // it as a storage image, the forward raster passes attach it, and cScene's
    // pogo feed blits its authored window out (TRANSFER_SRC).
    CreateViewportColorTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_TRANSFER_SRC |
            RI_USAGE_TRANSFER_DST,
        &renderTarget[i], &renderTargetView[i],
        "HybridViewportState.renderTarget");
    CreateViewportColorAttachmentView(&pGraphics->device, &renderTarget[i],
                                      cGraphics::PogoColorFormat,
                                      &renderTargetAttachmentView[i]);

    // SAMPLED lets the compute passes bind the depth as `sampler2D depthMap`
    // after the gbuffer pass flips it to SHADER_READ_ONLY.
    // DEPTH|STENCIL view: the Z passes only touch the depth aspect, but
    // cLuxEffectRenderer's outline pass binds this same view as a stencil
    // attachment (mark silhouette -> NOTEQUAL composite), so the view must
    // carry the stencil aspect. For sampling the depth aspect (soft particles)
    // we add a separate depth-only SRV below — a combined view can't be
    // sampled.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::DepthFormat,
        RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT, &depthTextures[i], &depthView[i],
        "HybridViewportState.depth");

    // Second view of the SAME depth image, depth-aspect only (SHADER_RESOURCE
    // view → RITextureView::create selects DEPTH_BIT, dropping the stencil bit
    // that makes the combined depthView above unsampleable). The particle pass
    // binds this as gSceneDepth for the soft-particle depth fade.
    {
      RITextureViewDesc dsv = {};
      dsv.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      dsv.format = cGraphics::DepthFormat;
      dsv.mipNum = 1;
      dsv.layerNum = 1;
      RITextureView v =
          RITextureView::create(&pGraphics->device, depthTextures[i].Get(), dsv);
      depthSampleView[i] = RISharedPointer<RITextureView>(&pGraphics->device, v);
    }

    // Depth pyramid the translucent occlusion cull tests against. Failure is
    // not fatal: hiZ.IsUsable() then reports false and the translucent
    // families draw without culling.
    hiZ.Create(pGraphics, i, renderW, renderH);

    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VisibilityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &visibilityTexture[i],
        &visibilityView[i], "HybridViewportState.visibility");
    CreateViewportColorAttachmentView(&pGraphics->device, &visibilityTexture[i],
                                      cGraphics::VisibilityFormat,
                                      &visibilityAttachmentView[i]);

    // Packed visibility — RT pipeline storage write, sampled by the
    // path-tracing / direct / composite passes.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VisibilityFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &packedHitInfoTexture[i],
        &packedHitInfoView[i], "HybridViewportState.packedHitInfo");

    // Screen-space velocity — gbuffer MRT #2, sampled by temporal passes.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VelocityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &velocityTexture[i], &velocityView[i],
        "HybridViewportState.velocity");
    CreateViewportColorAttachmentView(&pGraphics->device, &velocityTexture[i],
                                      cGraphics::VelocityFormat,
                                      &velocityAttachmentView[i]);

    // Decal accumulators — Mul/MulX2 into decalMul, Add into decalAdd; the
    // composite applies albedo = albedo*decalMul + decalAdd before lighting.
    // RGBA16F (PogoColorFormat) so the linear factors don't band;
    // COLOR_ATTACHMENT for the raster, SHADER_RESOURCE for the composite read.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalMulTexture[i], &decalMulView[i],
        "HybridViewportState.decalMul");
    CreateViewportColorAttachmentView(&pGraphics->device, &decalMulTexture[i],
                                      cGraphics::PogoColorFormat,
                                      &decalMulAttachmentView[i]);
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalAddTexture[i], &decalAddView[i],
        "HybridViewportState.decalAdd");
    CreateViewportColorAttachmentView(&pGraphics->device, &decalAddTexture[i],
                                      cGraphics::PogoColorFormat,
                                      &decalAddAttachmentView[i]);
  }

  // Ping-ponged ReSTIR DI surface key and reservoir.
  // DirectLightingPass reprojects last frame's reservoir and rejects on last
  // frame's key, so both need a history slot. STORAGE (compute write) +
  // SAMPLED (history reproject) + TRANSFER_DST (first-use clear); kept in
  // GENERAL, toggled per frame by directLightingIndex.
  for (uint32_t i = 0; i < 2; i++) {
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &directKeyTexture[i], &directKeyView[i],
        "HybridViewportState.directKey");
    // ReSTIR reservoir history ping-pong (RGBA32F: asfloat(lightIndex), W, M).
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTexture[i], &reservoirView[i],
        "HybridViewportState.reservoir");
  }

  // Everything below is written and consumed within a single frame; indirect
  // denoiser history is owned by NRD, so one slot each, no ping-pong.
  //
  // ReSTIR DI's resolved direct irradiance, and the path tracer's two lighting
  // channels plus the two halves of its surface key.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &directLightingTexture,
      &directLightingView, "HybridViewportState.directLighting");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectRadianceTexture,
      &indirectRadianceView, "HybridViewportState.indirectRadiance");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectSpecularTexture,
      &indirectSpecularView, "HybridViewportState.indirectSpecular");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectKeyTexture, &indirectKeyView,
      "HybridViewportState.indirectKey");
  // Second half of the indirect surface key: primary-hit GGX alpha in .x,
  // diffuse primary hit distance in metres in .y, specular primary hit distance
  // in .z; both are 0 for no hit/skipped lobe, and .w is reserved.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectKeyExtraTexture,
      &indirectKeyExtraView, "HybridViewportState.indirectKeyExtra");

  // NRD frontend inputs. The normal target uses the exact configured
  // R10G10B10A2_UNORM NRD encoding; the radiance targets carry YCoCg +
  // normalized hit distance in RGBA16F; viewZ is a linear R32F guide.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_R10_G10_B10_A2_UNORM,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdNormalRoughnessTexture,
      &nrdNormalRoughnessView, "HybridViewportState.nrdNormalRoughness");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_R32_SFLOAT,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdViewZTexture, &nrdViewZView,
      "HybridViewportState.nrdViewZ");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdDiffuseRadianceHitDistTexture,
      &nrdDiffuseRadianceHitDistView,
      "HybridViewportState.nrdDiffuseRadianceHitDist");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdSpecularRadianceHitDistTexture,
      &nrdSpecularRadianceHitDistView,
      "HybridViewportState.nrdSpecularRadianceHitDist");
  // NRD declares IN_MV as an output in temporal stabilization. Keep this
  // RG16F copy private to NRD; the shared velocity attachment remains a
  // read-only input for the rest of the frame.
  //
  // SIMULTANEOUS_ACCESS because REBLUR samples IN_MV in its temporal passes and
  // stores to it in stabilization, with only memory barriers in between (see
  // NrdIntegration's dispatch loop) -- the one texture the renderer deliberately
  // leaves in a combined read+write state. Vulkan expresses that as GENERAL; on
  // D3D12 no layout admits both accesses, so it needs the flag.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::VelocityFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST | RI_USAGE_SIMULTANEOUS_ACCESS,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdMotionVectorsTexture,
      &nrdMotionVectorsView, "HybridViewportState.nrdMotionVectors");

  // The denoiser is per-viewport: NRD sizes its history and pools to one
  // extent, so sharing one instance across differently-sized viewports would
  // thrash both. Recreating on resize would also be wasteful, hence OnResize.
  //
  // NRD is runtime-loaded and may legitimately be absent (it ships separately
  // under its own license). Both handles then stay null and Draw composites the
  // undenoised lighting instead.
  if (NrdIntegration::IsAvailable()) {
    if (!nrd)
      nrd = std::make_shared<NrdIntegration>(pGraphics);
    nrd->OnResize(renderW, renderH);
    if (!directNrd)
      directNrd = std::make_shared<NrdIntegration>(
          pGraphics, NrdDenoiserMode::DirectDiffuse);
    directNrd->OnResize(renderW, renderH);
  }
  // Resource recreation invalidates every temporal history.
  indirectHistoryReset = true;
  nrdInputInShaderResource = false;
  indirectInShaderResource = false;

  // Intra-frame reservoir hand-off (temporal pass → spatial pass), RGBA32F.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTemporalTexture,
      &reservoirTemporalView, "HybridViewportState.reservoirTemporal");

  // Recreation invalidated every history: re-arm the one-time direct- and
  // indirect-lighting init/clear and reset the per-viewport temporal state on
  // the next Draw.
  directLightingIndex = 0;
  directLightingInit = false;
  indirectLightingInit = false;
}

cViewport::HybridViewportState::~HybridViewportState() {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
    pGraphics->graphicsDefer.push(renderTarget[i]);
    pGraphics->graphicsDefer.push(depthTextures[i]);
    pGraphics->graphicsDefer.push(visibilityTexture[i]);
    pGraphics->graphicsDefer.push(packedHitInfoTexture[i]);
    pGraphics->graphicsDefer.push(velocityTexture[i]);
    pGraphics->graphicsDefer.push(decalMulTexture[i]);
    pGraphics->graphicsDefer.push(decalAddTexture[i]);

    pGraphics->graphicsDefer.push(renderTargetView[i]);
    pGraphics->graphicsDefer.push(depthView[i]);
    pGraphics->graphicsDefer.push(depthSampleView[i]);
    pGraphics->graphicsDefer.push(visibilityView[i]);
    pGraphics->graphicsDefer.push(packedHitInfoView[i]);
    pGraphics->graphicsDefer.push(velocityView[i]);
    pGraphics->graphicsDefer.push(decalMulView[i]);
    pGraphics->graphicsDefer.push(decalAddView[i]);

    // Companion COLOR_ATTACHMENT views of the same images.
    pGraphics->graphicsDefer.push(renderTargetAttachmentView[i]);
    pGraphics->graphicsDefer.push(visibilityAttachmentView[i]);
    pGraphics->graphicsDefer.push(velocityAttachmentView[i]);
    pGraphics->graphicsDefer.push(decalMulAttachmentView[i]);
    pGraphics->graphicsDefer.push(decalAddAttachmentView[i]);
  }
  // Ping-ponged ReSTIR surface key and reservoir.
  for (uint32_t i = 0; i < 2; i++) {
    pGraphics->graphicsDefer.push(directKeyTexture[i]);
    pGraphics->graphicsDefer.push(reservoirTexture[i]);
    pGraphics->graphicsDefer.push(directKeyView[i]);
    pGraphics->graphicsDefer.push(reservoirView[i]);
  }
  // NRD owns Vulkan pipelines and its own texture pool, and it disposes them
  // eagerly rather than through graphicsDefer. Destroying it here would call
  // vkDestroyPipeline on pipelines the in-flight command buffers still
  // reference (the viewport dies while frames are outstanding, unlike the
  // renderer, which is torn down after the device is idle). So hand ownership
  // to the deferral: the lambda runs once the GPU has passed this frame.
  if (nrd)
    pGraphics->graphicsDefer.push(
        std::function<void()>([keep = std::move(nrd)]() mutable { keep.reset(); }));
  if (directNrd)
    pGraphics->graphicsDefer.push(std::function<void()>(
        [keep = std::move(directNrd)]() mutable { keep.reset(); }));

  // Single-slot, intra-frame resources.
  pGraphics->graphicsDefer.push(directLightingTexture);
  pGraphics->graphicsDefer.push(indirectRadianceTexture);
  pGraphics->graphicsDefer.push(indirectSpecularTexture);
  pGraphics->graphicsDefer.push(indirectKeyTexture);
  pGraphics->graphicsDefer.push(indirectKeyExtraTexture);
  pGraphics->graphicsDefer.push(nrdNormalRoughnessTexture);
  pGraphics->graphicsDefer.push(nrdViewZTexture);
  pGraphics->graphicsDefer.push(nrdDiffuseRadianceHitDistTexture);
  pGraphics->graphicsDefer.push(nrdSpecularRadianceHitDistTexture);
  pGraphics->graphicsDefer.push(nrdMotionVectorsTexture);

  pGraphics->graphicsDefer.push(directLightingView);
  pGraphics->graphicsDefer.push(indirectRadianceView);
  pGraphics->graphicsDefer.push(indirectSpecularView);
  pGraphics->graphicsDefer.push(indirectKeyView);
  pGraphics->graphicsDefer.push(indirectKeyExtraView);
  pGraphics->graphicsDefer.push(nrdNormalRoughnessView);
  pGraphics->graphicsDefer.push(nrdViewZView);
  pGraphics->graphicsDefer.push(nrdDiffuseRadianceHitDistView);
  pGraphics->graphicsDefer.push(nrdSpecularRadianceHitDistView);
  pGraphics->graphicsDefer.push(nrdMotionVectorsView);
  pGraphics->graphicsDefer.push(reservoirTemporalTexture);
  pGraphics->graphicsDefer.push(reservoirTemporalView);
  hiZ.Defer(pGraphics);
}

// Defer our current resources (~HybridViewportState defers each shared handle
// to the graphics freelist), then move-construct rhs's shared handles into us
// (a pointer steal — no refcount churn, no dispose). The resize path uses this
// as
// `*this = {}` to reset in place.
cViewport::HybridViewportState &
cViewport::HybridViewportState::operator=(HybridViewportState &&rhs) noexcept {
  if (this != &rhs) {
    this->~HybridViewportState();
    new (this) HybridViewportState(std::move(rhs));
  }
  return *this;
}

// Append the per-world light/fog SSBO bindings (set kWorldSet) to a pass's
// descriptor-binding vector. Every pass that reads lights/fog calls this before
// its bindDescriptors; RIProgram routes them to kWorldSet by reflection and
// caches the resulting set — stable world buffers hash to a cache hit, so the
// descriptor set is written once and reused until a re-bake swaps a buffer.
// Names a pass doesn't reflect are skipped, so binding all four everywhere is
// safe. The buffers are always >= 1 element after cWorld::PrepareFrame's bake
// (which runs first in Draw), so a reflected binding is never left unbound.
static void appendWorldLightFog(std::vector<RIProgram::DescriptorBinding> &bnd,
                                cWorld *apWorld) {
  if (!apWorld)
    return;
  auto add = [&](const char *name, RIBuffer *buf, uint32_t cnt, size_t stride) {
    if (!buf) {
      // Null before the first PrepareFrame bake. Push an empty descriptor
      // flagged optional rather than nothing: an empty entry is skipped by the
      // write/hash exactly as before, but it tells the debug unwritten-binding
      // check the omission is intentional.
      bnd.emplace_back(name, RIDescriptor(), 0, true);
      return;
    }
    bnd.emplace_back(
        name, RIDescriptor::storageBuffer(
                  &Interface<cGraphics>::Get()->device, buf, 0,
                  std::max<uint32_t>(cnt, 1u) * stride,
                  static_cast<uint32_t>(stride), false, true));
  };
  add("gPointLights", apWorld->GetPointLightBuffer(),
      apWorld->GetPointLightCount(), sizeof(PointLight));
  add("gSpotLights", apWorld->GetSpotLightBuffer(),
      apWorld->GetSpotLightCount(), sizeof(SpotLight));
  add("gAreaLights", apWorld->GetAreaLightBuffer(),
      apWorld->GetAreaLightCount(), sizeof(RectLight));
  add("gFogAreas", apWorld->GetFogAreaBuffer(), apWorld->GetFogAreaCount(),
      sizeof(FogAreaParams));
}

void cHybridRenderer::Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  const cVector2l displayExtent = viewport->GetDisplayExtent();
  const cVector2l renderExtent = viewport->GetRenderExtent();
  if (displayExtent.x <= 0 || displayExtent.y <= 0 || renderExtent.x <= 0 ||
      renderExtent.y <= 0) {
    return;
  }

  cViewport::HybridViewportState *pState =
      viewport->PrepareToRender<cViewport::HybridViewportState>(cntx,
                                                                renderExtent);
  if (pState == nullptr || pState->width == 0 || pState->height == 0) {
    return;
  }
  cViewport::HybridViewportState &state = *pState;
  if (viewport->ConsumeTemporalHistoryReset())
    state.indirectHistoryReset = true;
  // The opaque temporal block below consumes and clears this flag before the
  // water pass runs, so capture it before any temporal work can touch it.
  const bool historyResetForFrame = state.indirectHistoryReset;
  const uint32_t renderWidth = state.width; // negotiated scene/input extent
  const uint32_t renderHeight = state.height;

  // NOTE: HybridViewportState::Update creates and sizes state.nrd alongside the
  // packed-input textures, so it is non-null and correctly sized here.
  //
  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();

  const cVector3f waterCameraPos = apFrustum->GetOrigin();
  const cVector3f waterCameraDir =
      cMath::Vector3Normalize(apFrustum->GetForward());
  const cMatrixf &waterProjectionMat = apFrustum->GetProjectionMatrix();
  bool waterProjectionChanged = true;
  if (state.waterPrevCameraValid) {
    waterProjectionChanged = false;
    for (uint32_t i = 0; i < 16u; ++i) {
      if (waterProjectionMat.v[i] != state.waterPrevProjMat.v[i]) {
        waterProjectionChanged = true;
        break;
      }
    }
  }
  // Deliberate water-only cut heuristic: ordinary camera movement and animated
  // wave normals must not reset accumulation; only a >5-unit teleport, a >45°
  // turn, or a changed projection is treated as a camera cut.
  const bool waterCameraCut =
      !state.waterPrevCameraValid ||
      cMath::Vector3DistSqr(state.waterPrevCameraPos, waterCameraPos) > 25.0f ||
      cMath::Vector3Dot(state.waterPrevCameraDir, waterCameraDir) <
          cosf(45.0f * 3.14159265358979323846f / 180.0f) ||
      waterProjectionChanged;
  const bool waterResetForFrame = historyResetForFrame ||
                                  state.waterHistoryReset || waterCameraCut;
  state.waterPrevCameraPos = waterCameraPos;
  state.waterPrevCameraDir = waterCameraDir;
  state.waterPrevProjMat = waterProjectionMat;
  state.waterPrevCameraValid = true;
  state.waterHistoryReset = false;

  hpl::TemporalFrameDesc temporalDesc = {};
  temporalDesc.viewMat = mainFrustumViewMat.a;
  temporalDesc.unjitteredProjMat = mainFrustumProjMat.a;
  temporalDesc.renderWidth = renderWidth;
  temporalDesc.renderHeight = renderHeight;
  // NativeAA is prepared at the display extent and still uses the active
  // temporal jitter. Off, unprepared, or failed preparation reports no phase
  // count and keeps the existing zero-jitter path.
  const uint32_t jitterPhaseCount = viewport->GetTemporalJitterPhaseCount();
  const hpl::TemporalJitter pendingJitter =
      jitterPhaseCount == 0
          ? hpl::TemporalJitter{}
          : hpl::TemporalPendingJitter(state.temporal, jitterPhaseCount);
  temporalDesc.jitterPixels[0] = pendingJitter.x;
  temporalDesc.jitterPixels[1] = pendingJitter.y;
  temporalDesc.forceHistoryReset = historyResetForFrame;
  const hpl::TemporalFrameSnapshot temporalFrame =
      hpl::TemporalBeginFrame(state.temporal, temporalDesc);
  viewport->PublishRasterCamera(temporalFrame.viewMat, temporalFrame.projMat);
  viewport->PublishRasterTemporalFrame(temporalFrame, afFrameTime * 1000.0f);
  {
    m_rendererList.BeginAndReset(afFrameTime, apFrustum);
    auto *dynamicContainer =
        apWorld->GetRenderableSet(eWorldContainerType_Dynamic);
    auto *staticContainer =
        apWorld->GetRenderableSet(eWorldContainerType_Static);
    dynamicContainer->UpdateBeforeRendering();
    staticContainer->UpdateBeforeRendering();

    auto prepareObjectHandler = [&](iRenderable *pObject) {
      if (!rendering::IsObjectIsVisible(
              pObject, eRenderableFlag_VisibleInNonReflection, {})) {
        return;
      }
      m_rendererList.AddObject(pObject);
    };
    // Frustum-cull the raster render list: the visibility/gbuffer + translucent
    // passes only need what the camera sees, and culling keeps the per-frame
    // translucent/particle Update* work bounded. Whole-map RT geometry
    // (shadows/ GI need everything, including behind the camera) is no longer
    // sourced here — cWorld::PrepareFrame walks its own renderables unculled to
    // build the TLAS.
    rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    rendering::WalkAndPrepareRenderList(staticContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    m_rendererList.End(
        eRenderListCompileFlag_Diffuse | eRenderListCompileFlag_Translucent |
        eRenderListCompileFlag_Decal | eRenderListCompileFlag_Illumination |
        eRenderListCompileFlag_FogArea);

    // The TLAS (now owned by cWorld) can keep referencing the BLAS device
    // addresses of geometry freed on a map transition until it's rebuilt. No
    // per-frame pinning is needed for that: a cVertexBuffer owns its BLAS by
    // value and defers it (RISharedPointer on Interface<cGraphics>::Get()->graphicsDefer) on rebuild and
    // in its destructor, so any BLAS the TLAS can still reference outlives the
    // in-flight window even after its owning renderable is destroyed.
  }

  // --------------------------------------------------------------------
  // Per-frame prepare for every VISIBLE translucent renderable (particles +
  // meshes + billboards + beams). UpdateGraphicsForFrame/ForViewport recompute
  // dynamic geometry (billboard facing, beam stretch, emitter step) and mark
  // the VB dirty; SubmitToGPU then allocates/uploads dirty streams for the
  // raster particle + mesh passes. The render list is frustum-culled above, so
  // only the on-screen set pays this cost. BLAS builds for ray-traced meshes
  // happen in cWorld::PrepareFrame (TLAS owner), not here.
  //
  // Must run BEFORE any vkCmdBeginRendering so the uploader's barriers don't
  // collide with a dynamic-rendering scope.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj)
      continue;
    pObj->UpdateGraphicsForFrame(afFrameTime);
    pObj->UpdateGraphicsForViewport(apFrustum, afFrameTime);
    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      vbri->SubmitToGPU(&mpGraphics->device);
    }
  }

  // Same prepare for decals (a separate list from Translucent). Their
  // Update*ForFrame already ran in AddObject, but SubmitToGPU (allocates
  // vk.buffer + uploads streams) is renderer-side and must run here, before any
  // vkCmdBeginRendering — else the decal pass hits the missing-position guard
  // and draws nothing.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
    if (!pObj)
      continue;

    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      // Decals are never TLAS instances — upload streams, no BLAS.
      vbri->SubmitToGPU(&mpGraphics->device);
    }
  }

  SceneConstants perFrame{};
  std::memcpy(perFrame.viewMat, mainFrustumViewMat.a, sizeof(perFrame.viewMat));
  std::memcpy(perFrame.invViewMat, mainFrustumViewInvMat.a,
              sizeof(perFrame.invViewMat));
  std::memcpy(perFrame.projMat, temporalFrame.projMat,
              sizeof(perFrame.projMat));
  std::memcpy(perFrame.invProjMat, temporalFrame.invProjMat,
              sizeof(perFrame.invProjMat));
  std::memcpy(perFrame.unjitteredProjMat, temporalFrame.unjitteredProjMat,
              sizeof(perFrame.unjitteredProjMat));
  std::memcpy(perFrame.prevViewMat, temporalFrame.prevViewMat,
              sizeof(perFrame.prevViewMat));
  std::memcpy(perFrame.prevProjMat, temporalFrame.prevUnjitteredProjMat,
              sizeof(perFrame.prevProjMat));
  perFrame.prevJitterX = temporalFrame.prevJitterUV[0];
  perFrame.prevJitterY = temporalFrame.prevJitterUV[1];
  // viewProjMat = proj * view (column-major); fill via direct ml composition
  // when needed. Leaving as identity-stub for now — first pass writes only
  // visibility; lighting in the FS reads viewMat/invViewMat which are correct.
  perFrame.viewportSize[0] = (float)renderWidth;
  perFrame.viewportSize[1] = (float)renderHeight;
  perFrame.viewTexel[0] = renderWidth ? 1.0f / (float)renderWidth : 0.0f;
  perFrame.viewTexel[1] = renderHeight ? 1.0f / (float)renderHeight : 0.0f;
  // Only a prepared temporal provider at a genuinely reduced shaded extent
  // biases material mips; Off, NativeAA, and native-extent frames stay at 0.
  // This reaches only gradient-based material sampling in sampleBindless2D;
  // explicit-LOD paths and temporal/presentation shaders are unchanged.
  // DevRenderScale has no provider behind it: GetTemporalJitterPhaseCount() is
  // 0 and the frame is not accumulated, so it is a plain display stretch, not
  // reconstruction. The vendor formula's -1.0 is paid for by subpixel
  // accumulation across jittered frames; applying it here buys aliasing, not
  // detail. Thus the dev override and a same-extent provider deliberately
  // do not sample the same mips.
  perFrame.materialMipBias = hpl::TemporalMaterialMipBias(
      {renderWidth, renderHeight},
      {static_cast<uint32_t>(displayExtent.x),
       static_cast<uint32_t>(displayExtent.y)},
      viewport->IsTemporalProviderPrepared());
  // Accumulated animation time (iRenderer::mfTimeCount, advanced each frame in
  // iRenderer::Update via cGraphics::Update) — NOT the per-frame delta. The
  // water wave phase is afT * waveSpeed; feeding the delta froze the waves and
  // jittered them with frametime variance (stutter-in-place). Matches the
  // reference's afT = GetTimeCount().
  perFrame.afT = GetTimeCount();
  perFrame.totalFrames = mpGraphics->frameIndex;
  perFrame.cameraFov = apFrustum->GetFOV();
  perFrame.fireflyClampThreshold = 10.0f;
  perFrame.zNear = apFrustum->GetNearPlane();
  perFrame.zFar = apFrustum->GetFarPlane();
  perFrame.allLightsCastShadows = mpGraphics->allLightsCastShadows ? 1u : 0u;
  // invViewRotationMat = rotation part of the inverse view matrix (camera
  // world-space basis, translation zeroed). Translucent.frag rotates the
  // view-space cube-map reflection vector into world space with it (matching
  // the base game's a_mtxInvViewRotation). Translation lives at .a[12..14]
  // (the camera-basis extraction below reads posW from invV[12,13,14]); zero
  // it so a direction (passed w=1 in the shader) isn't offset by the camera
  // position.
  {
    ml::float4x4 invViewRot = mainFrustumViewInvMat;
    invViewRot.a[12] = 0.0f;
    invViewRot.a[13] = 0.0f;
    invViewRot.a[14] = 0.0f;
    std::memcpy(perFrame.invViewRotationMat, invViewRot.a,
                sizeof(perFrame.invViewRotationMat));
  }
  // World fog — copy the per-world settings into the per-frame UBO so
  // Fog.slang's world-fog block activates. Mirrors cWorld::BuildFogParams
  // colour handling (sRGB->linear, alpha kept linear). Leaving worldFogLength
  // at 0 (the default zero-init) disables world fog, matching the shader's
  // `worldFogLength > 0` guard.
  if (apWorld->GetFogActive()) {
    const cColor fc = apWorld->GetFogColor();
    perFrame.worldFogStart = apWorld->GetFogStart();
    perFrame.worldFogLength = apWorld->GetFogEnd() - apWorld->GetFogStart();
    perFrame.fogFalloffExp = apWorld->GetFogFalloffExp();
    perFrame.worldFogColor = float4{sRGBToLinear(fc.r), sRGBToLinear(fc.g),
                                    sRGBToLinear(fc.b), fc.a};
    perFrame.oneMinusFogAlpha = 1.0f - fc.a;
  }

  // Pinhole camera basis from the view-inverse rows (camera world-space
  // right/up/back/origin). The -matrix-layout-column-major slangc flag makes
  // these offsets line up with the shader's column-vector math.
  {
    const float *invV = mainFrustumViewInvMat.a;
    const hpl::float3 rightW{invV[0], invV[1], invV[2]};
    const hpl::float3 upW{invV[4], invV[5], invV[6]};
    const hpl::float3 backW{invV[8], invV[9], invV[10]};
    const hpl::float3 posW{invV[12], invV[13], invV[14]};

    const float aspect = apFrustum->GetAspect();
    const float tanHalfFov = std::tan(0.5f * apFrustum->GetFOV());
    constexpr float focalLength = 1.0f;
    // The guard band is disabled, so the ray cone matches the negotiated
    // scene/input image. Primary hits and velocity themselves come from
    // VBufferRaster.3d, not from a traced primary ray.
    const float uScale = focalLength * tanHalfFov * aspect;
    const float vScale = focalLength * tanHalfFov;

    perFrame.posW = posW;
    perFrame.cameraU = {uScale * rightW.x, uScale * rightW.y,
                        uScale * rightW.z};
    perFrame.cameraV = {vScale * upW.x, vScale * upW.y, vScale * upW.z};
    // cameraW points from the camera through the image-plane center =
    // focalLength * forward. The view-inverse stores back (negative forward)
    // in column 2, so negate.
    perFrame.cameraW = {-focalLength * backW.x, -focalLength * backW.y,
                        -focalLength * backW.z};
    // The RT pinhole convention is this same snapshot projection represented
    // as the unjittered basis plus this snapshot's jitterUV; apply the offset
    // once, matching the raster projection above.
    // Top-left normalized sample offset from the pixel center: jitterPixels /
    // renderExtent, with +y down. The temporal frame supplies these values;
    // they remain zero while no temporal provider is active.
    perFrame.jitterX = temporalFrame.jitterUV[0];
    perFrame.jitterY = temporalFrame.jitterUV[1];
  }

  auto solids = m_rendererList.GetSolidObjects();
  // Lights are no longer pulled from the per-frame render list — cWorld owns
  // the per-world light buffers (rebuilt once per frame by
  // cWorld::PrepareFrame, driven from cScene before the viewport loop).
  RISegmentReq indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(mpGraphics->frameIndex, solids.size(), &indirectReq);
  assert(indirectOk);
  auto *indirectDst = reinterpret_cast<VkDrawIndirectCommand *>(
      static_cast<uint8_t *>(m_indirectDrawBuffer.mapped()) +
      (size_t)indirectReq.elementOffset * sizeof(VkDrawIndirectCommand));
  uint32_t writtenDraws = 0;

  // The world's per-frame GPU memory (light/fog/decal buffers, TLAS, bindless
  // object/material slots) is published once per frame by cWorld::PrepareFrame,
  // driven by cScene before the viewport loop — not here. Read the per-world
  // counts it produced into this viewport's SceneConstants.
  perFrame.pointLightCount = apWorld->GetPointLightCount();
  perFrame.spotLightCount = apWorld->GetSpotLightCount();
  perFrame.areaLightCount = apWorld->GetAreaLightCount();
  // Light-grid origin: this camera's position snapped DOWN to a whole cell, so
  // the grid translates one cell at a time instead of sliding with the camera.
  // binLights and getCellLights both read this, so it must be computed once
  // here rather than derived independently on either side. floor (not round) so
  // the mapping is monotonic and a point never jumps two cells at once.
  const auto snapToCell = [](float v) {
    return std::floor(v / kLightGridUnit) * kLightGridUnit;
  };
  perFrame.lightGridOriginW = float3(snapToCell(perFrame.posW.x),
                                     snapToCell(perFrame.posW.y),
                                     snapToCell(perFrame.posW.z));
  perFrame._padLightGridOrigin = 0.0f;
  // Fog composition is order-dependent. Reuse the visible, back-to-front
  // list that RenderList builds for this camera, rather than treating the
  // world's upload order as draw order (or rendering editor-hidden areas).
  // Keep indices per viewport: sorting the shared world buffer would make
  // one editor viewport change another viewport's fog.
  perFrame.fogAreaCount = 0;
  for (cFogArea *fog : m_rendererList.GetFogAreas()) {
    if (perFrame.fogAreaCount == kFogAreaCapacity)
      break;
    auto worldFog = apWorld->GetFogAreaIterator();
    uint32_t index = 0;
    while (worldFog.HasNext()) {
      if (worldFog.Next() == fog) {
        const uint32_t slot = perFrame.fogAreaCount++;
        perFrame.fogAreaIndices[slot >> 2][slot & 3] = index;
        break;
      }
      ++index;
    }
  }
  perFrame.decalCount = apWorld->GetDecalCount();

  // Opaque two-phase cull. Phase 1 replays what was visible last frame so the
  // pyramid has something to be built from; phase 2 tests everything against
  // that pyramid and draws whatever phase 1 missed. Both phases draw the SAME
  // geometry from two ranges of m_indirectDrawBuffer whose instanceCount the
  // kernel owns, so the host fills both identically and never needs the answer.
  RISegmentReq opaquePhaseTwoReq = {};
  RISegmentReq opaqueCandidateReq = {};
  VkDrawIndirectCommand *opaquePhaseTwoDst = nullptr;
  StandardCullCandidate *opaqueCandidates = nullptr;
  const bool opaqueCullReady =
      indirectOk && indirectDst != nullptr &&
      m_cullLoaded && m_cull && m_hiZ &&
      state.hiZ.IsUsable(mpGraphics->swapchainIndex) &&
      solids.size() <= kHybridCameraMaxDraws &&
      m_indirectSegment.request(mpGraphics->frameIndex, solids.size(),
                                &opaquePhaseTwoReq) &&
      m_cameraCandidateSegment.request(mpGraphics->frameIndex, solids.size(),
                                       &opaqueCandidateReq) &&
      m_cameraCandidateBuffer.mappedAddress != nullptr;
  if (opaqueCullReady) {
    opaquePhaseTwoDst = reinterpret_cast<VkDrawIndirectCommand *>(
        static_cast<uint8_t *>(m_indirectDrawBuffer.mapped()) +
        static_cast<size_t>(opaquePhaseTwoReq.elementOffset) *
            sizeof(VkDrawIndirectCommand));
    opaqueCandidates = reinterpret_cast<StandardCullCandidate *>(
        static_cast<uint8_t *>(m_cameraCandidateBuffer.mappedAddress) +
        static_cast<size_t>(opaqueCandidateReq.elementOffset) *
            sizeof(StandardCullCandidate));
  }

  for (iRenderable *pObject : solids) {
    cVertexBuffer *pVB = pObject->GetVertexBuffer();
    if (!pVB)
      continue;

    // Object slot for the indirect draw's firstInstance. cWorld::PrepareFrame
    // already submitted this object (same cookie), built its BLAS, and uploaded
    // its geometry for the TLAS this frame, so this is an idempotent cache hit
    // returning the same slot. Skips on material/pool exhaustion.
    const uint32_t slot =
        apWorld->SubmitRenderableObject(pObject, cntx, apFrustum);
    if (slot == UINT32_MAX)
      continue;

    if (writtenDraws < indirectReq.numElements) {
      const VkDrawIndirectCommand command{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/slot,
      };
      if (opaqueCullReady) {
        cBoundingVolume *bounds = pObject->GetBoundingVolume();
        StandardCullCandidate candidate{};
        if (bounds) {
          const cVector3f boundsMin = bounds->GetMin();
          const cVector3f boundsMax = bounds->GetMax();
          candidate.aabbMinX = boundsMin.x;
          candidate.aabbMinY = boundsMin.y;
          candidate.aabbMinZ = boundsMin.z;
          candidate.aabbMaxX = boundsMax.x;
          candidate.aabbMaxY = boundsMax.y;
          candidate.aabbMaxZ = boundsMax.z;
        } else {
          // No bounds means nothing to test against: a box that swallows any
          // frustum, exempt from occlusion, so exactly one phase draws it. A
          // zeroed candidate would fail the caster gate and be dropped by both.
          const float huge = 3.0e38f;
          candidate.aabbMinX = candidate.aabbMinY = candidate.aabbMinZ = -huge;
          candidate.aabbMaxX = candidate.aabbMaxY = candidate.aabbMaxZ = huge;
          candidate.cullFlags = kStandardCullFlagNeverOcclude;
        }
        candidate.objectSlot = slot;
        candidate.vertexCount = command.vertexCount;
        // The shared predicate gates on the caster bit and the variability mask
        // before the frustum test; neither means anything for a camera tile, so
        // the host sets the bits that let every candidate through.
        candidate.renderFlags =
            kStandardCullShadowCasterBit |
            (pObject->IsStatic() ? kStandardCullStaticBit : 0u);
        // NOT salted per viewport: the table is persistent and shared, so a
        // renderable should hash to the same slot every frame from every camera.
        candidate.visibilityKey =
            static_cast<uint32_t>(
                hash_u64(HASH_INITIAL_VALUE, pObject->GetUniqueCookie())) %
            kHybridCullVisibilityKeys;
        candidate.commandWordOffset =
            static_cast<uint32_t>(
                (indirectReq.elementOffset + writtenDraws) *
                (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t))) +
            1u;  // instanceCount
        opaqueCandidates[writtenDraws] = candidate;
        opaquePhaseTwoDst[writtenDraws] = command;
      }
      indirectDst[writtenDraws++] = command;
    } else {
      // The ring is sized kObjectSlotCapacity, so this is a far-off cliff --
      // but when it is hit the object simply never draws, and without a word
      // here that reads as geometry randomly missing.
      static bool warnedIndirectFull = false;
      if (!warnedIndirectFull) {
        warnedIndirectFull = true;
        Warning("Hybrid renderer: indirect draw ring full at %u draws; "
                "geometry beyond this is not drawn\n",
                writtenDraws);
      }
    }
  }

  // state.packedHitInfoView and the
  // freshly built TLAS now live on set 1 and are pushed per-dispatch via
  // RIProgram::bindDescriptors below (see the m_vBufferPomBary / m_pathTrace
  // / m_composite call sites).
  // Set 1 is allocated from a frame-rotated pool, so each frame's writes
  // land on an idle descriptor set.

  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(16);
  // Per-pass image / TLAS bindings are pushed inline below. Note: the storage
  // image `gPackedHitInfo` uses GENERAL layout, which satisfies both storage
  // and sampled access.
  {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gPerFrame");
    mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
    bindings.push_back(b);
  }

  // Scene's rendering was already ended above (before the BLAS/TLAS work).
  // Transition the MRT target (single packed-TriangleHit attachment) and
  // the depth image into their gbuffer-pass layouts. Both use the
  // UNDEFINED-discard pattern: loadOp=CLEAR on both attachments means we
  // never need prior contents preserved, so it doesn't matter what layout
  // the previous frame's last consumer left them in (depth is left in
  // DEPTH_READ_ONLY_OPTIMAL by the translucent/decal flipDepthToReadOnly
  // path below, which the gbuffer's expected DEPTH_ATTACHMENT_OPTIMAL
  // wouldn't otherwise match).
  {
    RITextureBarrier attachmentBarriers[3] = {
        {state.visibilityTexture[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET},
        {state.depthTextures[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_DEPTH_WRITE,
         RI_STAGE_NONE, RI_STAGE_NONE, RI_BARRIER_ASPECT_DEPTH},
        // Velocity MRT — same UNDEFINED→COLOR transition as the visibility
        // target (loadOp=CLEAR, so prior contents don't matter).
        {state.velocityTexture[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<3>(3, attachmentBarriers);
  }

  // MRT color targets, both cleared to all-zero. Visibility: psMain writes .w=1
  // (valid hit sentinel) or zero on sky/miss pixels (clear value). Velocity:
  // static/uncovered pixels read zero motion. (uint vs float clear is
  // bit-identical at zero.)
  RIRenderingAttachment gbufferColorAttachments[2] = {};
  gbufferColorAttachments[0].view =
      *state.visibilityAttachmentView[mpGraphics->swapchainIndex];
  gbufferColorAttachments[0].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[0].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  gbufferColorAttachments[1].view =
      *state.velocityAttachmentView[mpGraphics->swapchainIndex];
  gbufferColorAttachments[1].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[1].storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  // MRT owns the per-frame depth clear.
  RIRenderingAttachment depthAttachment = {};
  depthAttachment.view = *state.depthView[mpGraphics->swapchainIndex];
  depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.clearValue.depth = 1.0f;

  // ----------------------------------------------------------------------
  // World-space light grid build (feeds the path tracer's NEE importance
  // sampling + the Composite direct cull). Per-cell gather: one thread
  // per grid cell walks the light list and writes that cell's count + list. The
  // light SSBOs were uploaded + barriered to SHADER_READ earlier this frame, so
  // binLights reads them directly. No per-cell count clear is needed — every
  // cell's count is written unconditionally by its thread. Runs before any
  // consumer of the grid (direct lighting, path trace, composite).
  // ----------------------------------------------------------------------
  {
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsLightGrid(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                               "LightGrid");
    m_lightGrid.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], kHash,
                                       "LightGrid.cs:binLights");
    m_lightGrid.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                             &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
                                             VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    appendWorldLightFog(bnd, apWorld);
    m_lightGrid.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                   mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per grid cell (the shader early-outs past
    // kLightGridCellCount).
    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (kLightGridCellCount + 63u) / 64u,
                                1u, 1u);
  }

  {
    // binLights writes are read by several consumers later this frame: the
    // path tracer's NEE (ray tracing), the direct-lighting pass (compute) and
    // the MainCompositePass direct cull (fragment — walks the per-cell light
    // list). Every stage must be in dst or the fragment reads see an empty grid
    // and drop every point/spot light.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_RAY_TRACING | RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT});
  }

  // ----------------------------------------------------------------------
  // LightProbePass — the gameplay illumination sensor (cLightProbeQuery).
  //
  // Sits here because it needs exactly what the two lines above just
  // guaranteed: a populated light grid, and a TLAS (built by
  // cWorld::PrepareFrame before Draw). It writes nothing the frame displays —
  // its output is copied to a host-readable buffer and picked up by gameplay a
  // couple of frames later, which is why it can afford to run this early and be
  // skipped whenever nobody asked a question.
  // ----------------------------------------------------------------------
  if (mpGraphics->lightProbe) {
    cLightProbeQuery *pProbe = mpGraphics->lightProbe;

    // Harvest anything the GPU finished since last frame. Unconditional: a
    // sensor that stopped submitting still has an answer in flight to collect.
    pProbe->Poll(&mpGraphics->device,
                 mpGraphics->graphicsTimeline.completed(&mpGraphics->device));

    // Without a TLAS, keep the player's retained/default reading rather than
    // submitting an unoccluded measurement.
    RIBuffer *pRequests = pProbe->GetRequestBuffer(mpGraphics->frameIndex);
    RIBuffer *pResults = pProbe->GetResultBuffer(mpGraphics->frameIndex);
    if (pProbe->WantsDispatch() && apWorld->GetTlas() != nullptr &&
        pRequests && pResults && pProbe->BeginFrame(mpGraphics->frameIndex)) {
      const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
      RIGpuScope _gsLightProbe(&mpGraphics->profiler,
                               &mpGraphics->primary.cmds[0], "LightProbe");
      m_lightProbe.bindComputePipeline(&mpGraphics->device,
                                       &mpGraphics->primary.cmds[0], kHash,
                                       "LightProbe.cs");
      m_lightProbe.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0,
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> bnd;
      bnd.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        bnd.push_back(b);
      }
      bnd.emplace_back("gRtAccel",
                       RIDescriptor::accelerationStructure(&mpGraphics->device,
                                                           apWorld->GetTlas()));
      bnd.emplace_back("gProbeRequests",
                       RIDescriptor::storageBuffer(
                           &mpGraphics->device, pRequests, 0,
                           pProbe->GetRequestBufferRange()));
      bnd.emplace_back("gProbeResults",
                       RIDescriptor::storageBuffer(
                           &mpGraphics->device, pResults, 0,
                           pProbe->GetResultBufferRange()));
      appendWorldLightFog(bnd, apWorld);
      m_lightProbe.bindDescriptors(&mpGraphics->device,
                                   &mpGraphics->primary.cmds[0],
                                   mpGraphics->frameIndex, bnd.data(),
                                   bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

      const cLightProbeQuery::cPushConstants push = pProbe->GetPushConstants();
      mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
          &mpGraphics->device, m_lightProbe, 0, sizeof(push), &push);

      // One thread per probe, one group: the cap is kMaxLightProbes and the
      // shader early-outs past the submitted count.
      mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, 1u, 1u, 1u);

      // The copy rides this frame's submit, so it has executed once the
      // graphics timeline passes the value that submit will signal.
      pProbe->RecordReadback(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                             mpGraphics->frameIndex,
                             mpGraphics->graphicsTimeline.pending() + 1);
    }
  }

  // Computed unconditionally, and BEFORE any guard: both the opaque cull below
  // and ReserveCull later extract this frame's frustum planes from it, and an
  // identity matrix there would hand the kernel six plausible-looking planes
  // describing the wrong volume. Row-major, the order
  // StandardExtractFrustumPlanes and standardCullProjectAabb both read -- NOT
  // the transposed dump the shaders are handed.
  const cMatrixf cullViewProjection = cMath::MatrixMul(
      apFrustum->GetProjectionMatrix(), apFrustum->GetViewMatrix());

  // Phase 1 of the opaque cull: mark the commands that were visible last frame.
  // Recorded before the scope opens -- a dispatch cannot run inside one.
  bool opaqueCullDispatched = false;
  bool opaqueIndirectFlushed = false;
  uint32_t opaqueCommandWordDelta = 0;
  RISegmentReq opaqueTileReq = {};
  RISegmentReq opaqueCameraReq = {};
  RISegmentReq opaqueGroupReq = {};
  cStandardShadowCullPass::Buffers opaqueCullBuffers{};
  if (opaqueCullReady && writtenDraws > 0) {
    const uint32_t groupCount =
        (writtenDraws + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    const uint32_t frame = mpGraphics->frameIndex;
    if (m_cullTileSegment.request(frame, 1, &opaqueTileReq) &&
        m_cullCameraSegment.request(frame, 1, &opaqueCameraReq) &&
        m_cullGroupSegment.request(frame, groupCount, &opaqueGroupReq) &&
        m_cullTileBuffer.mappedAddress && m_cullCameraBuffer.mappedAddress &&
        m_cullGroupBuffer.mappedAddress) {
      auto *cameraSlot = reinterpret_cast<StandardCullCamera *>(
          static_cast<uint8_t *>(m_cullCameraBuffer.mappedAddress) +
          static_cast<size_t>(opaqueCameraReq.elementOffset) *
              sizeof(StandardCullCamera));
      StandardCullCamera camera{};
      std::memcpy(camera.viewProjection, cullViewProjection.v,
                  sizeof(camera.viewProjection));
      camera.hiZWidth = state.hiZ.width;
      camera.hiZHeight = state.hiZ.height;
      camera.hiZMipCount = state.hiZ.mipCount;
      *cameraSlot = camera;

      auto *tileSlot = reinterpret_cast<StandardCullTile *>(
          static_cast<uint8_t *>(m_cullTileBuffer.mappedAddress) +
          static_cast<size_t>(opaqueTileReq.elementOffset) *
              sizeof(StandardCullTile));
      StandardCullTile tile{};
      StandardExtractFrustumPlanes(cullViewProjection.v, tile.planes);
      tile.planeCount = 6u;
      // A camera tile keeps whatever the frustum keeps; the variability gate
      // exists for lights, which choose which casters they accept.
      tile.variabilityMask =
          kStandardCullVariabilityStatic | kStandardCullVariabilityDynamic;
      tile.candidateBase =
          static_cast<uint32_t>(opaqueCandidateReq.elementOffset);
      tile.candidateCount = writtenDraws;
      // Set for both dispatches. Phase 1 runs no occlusion test regardless --
      // the kernel skips it in replay mode -- so this only matters to phase 2.
      tile.cameraIndex = static_cast<uint32_t>(opaqueCameraReq.elementOffset);
      *tileSlot = tile;

      auto *groupSlots = reinterpret_cast<StandardCullGroup *>(
          static_cast<uint8_t *>(m_cullGroupBuffer.mappedAddress) +
          static_cast<size_t>(opaqueGroupReq.elementOffset) *
              sizeof(StandardCullGroup));
      for (uint32_t group = 0; group < groupCount; ++group) {
        groupSlots[group].tileIndex =
            static_cast<uint32_t>(opaqueTileReq.elementOffset);
        groupSlots[group].candidateOffset = group * kStandardCullGroupSize;
      }

      // The opaque commands live in m_indirectDrawBuffer, not the translucent
      // families' 5-word ring, so the word capacity follows that buffer.
      opaqueCullBuffers.candidates = &m_cameraCandidateBuffer;
      opaqueCullBuffers.tiles = &m_cullTileBuffer;
      opaqueCullBuffers.groups = &m_cullGroupBuffer;
      opaqueCullBuffers.indirect = m_indirectDrawBuffer.gpu();
      opaqueCullBuffers.drawCounts = &m_cullDrawCountBuffer;
      opaqueCullBuffers.cameras = &m_cullCameraBuffer;
      opaqueCullBuffers.visibility = &m_cullVisibilityBuffer;
      opaqueCullBuffers.hiZ = state.hiZ.sampleView[mpGraphics->swapchainIndex].Get();
      opaqueCullBuffers.candidateCapacity = kHybridCameraMaxDraws;
      opaqueCullBuffers.indirectCapacity = kObjectSlotCapacity;
      opaqueCullBuffers.indirectWordCapacity =
          kObjectSlotCapacity *
          (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t));
      opaqueCullBuffers.tileCapacity = kHybridCullMaxTiles;
      opaqueCullBuffers.groupCapacity = kHybridCullMaxGroups;
      opaqueCullBuffers.drawCountCapacity = kHybridCullMaxTiles;
      opaqueCullBuffers.cameraCapacity = kHybridCullMaxCameras;
      opaqueCullBuffers.visibilityCapacity = kHybridCullVisibilityKeys;
      // Words from a candidate's phase-1 command to its phase-2 one. Both
      // ranges live in the same ring, so this is their element distance.
      opaqueCommandWordDelta = static_cast<uint32_t>(
          (opaquePhaseTwoReq.elementOffset - indirectReq.elementOffset) *
          (sizeof(VkDrawIndirectCommand) / sizeof(uint32_t)));

      // Both phases' ranges go over in one copy; the kernel rewrites
      // instanceCount in each.
      const uint64_t first =
          std::min(indirectReq.elementOffset, opaquePhaseTwoReq.elementOffset);
      const uint64_t last =
          std::max(indirectReq.elementOffset, opaquePhaseTwoReq.elementOffset) +
          writtenDraws;
      m_indirectDrawBuffer.Flush(
          &mpGraphics->device, &mpGraphics->primary.cmds[0],
          first * sizeof(VkDrawIndirectCommand),
          (last - first) * sizeof(VkDrawIndirectCommand),
          m_indirectDrawFirstUse, /*cullFollows*/ true);
      m_indirectDrawFirstUse = false;
      opaqueIndirectFlushed = true;
      opaqueCullDispatched = m_cull->Dispatch(
          &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
          opaqueCullBuffers,
          static_cast<uint32_t>(opaqueTileReq.elementOffset), 1u,
          static_cast<uint32_t>(opaqueGroupReq.elementOffset), groupCount,
          kStandardCullModeVisibilityReplay);
      if (!opaqueCullDispatched && m_indirectDrawBuffer.staged)
        mpGraphics->primary.cmds[0].vk_d3d12_bufferBarrier(RIBufferBarrier(
            m_indirectDrawBuffer.gpu(), RI_RESOURCE_STATE_STORAGE_WRITE,
            RI_RESOURCE_STATE_INDIRECT_ARGUMENT, RI_STAGE_COPY,
            RI_STAGE_DRAW_INDIRECT));
    }
  }
  // No cull this frame: the draw reads the host's commands as written.
  if (!opaqueIndirectFlushed && writtenDraws > 0) {
    m_indirectDrawBuffer.Flush(
        &mpGraphics->device, &mpGraphics->primary.cmds[0],
        static_cast<uint64_t>(indirectReq.elementOffset) *
            sizeof(VkDrawIndirectCommand),
        static_cast<uint64_t>(writtenDraws) * sizeof(VkDrawIndirectCommand),
        m_indirectDrawFirstUse, /*cullFollows*/ false);
    m_indirectDrawFirstUse = false;
  }

  RIBeginRenderingDesc gbufferBeginDesc = {};
  gbufferBeginDesc.renderArea.width = renderWidth;
  gbufferBeginDesc.renderArea.height = renderHeight;
  gbufferBeginDesc.colorCount = 2;
  gbufferBeginDesc.colors = gbufferColorAttachments;
  gbufferBeginDesc.depthStencil = &depthAttachment;
  {
    RIGpuScope _gsGBuffer(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "GBuffer");
    mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, gbufferBeginDesc);

    RIViewport vkViewport = {};
    vkViewport.x = 0.0f;
    vkViewport.y = (float)renderHeight;
    vkViewport.width = (float)renderWidth;
    vkViewport.height = -(float)renderHeight;
    vkViewport.depthMin = 0.0f;
    vkViewport.depthMax = 1.0f;
    RIRect scissor = {};
    scissor.width = renderWidth;
    scissor.height = renderHeight;
    mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vkViewport);
    mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, scissor);

    if (writtenDraws > 0) {
      const RIGraphicsPipelineDesc pipelineDesc = MakeGBufferMRTPipelineDesc(
          cGraphics::VisibilityFormat, cGraphics::VelocityFormat,
          cGraphics::DepthFormat);
      m_gbuffer.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                             HASH_INITIAL_VALUE, "VBufferRaster.3d",
                             pipelineDesc);
      m_gbuffer.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                          &mpGraphics->globalset->m_bindlessSet, uint32_t(0));
      m_gbuffer.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
                                bindings.data(), bindings.size());
      mpGraphics->primary.cmds[0].drawIndirect(&mpGraphics->device, m_indirectDrawBuffer.gpu(),
                                      (VkDeviceSize)indirectReq.elementOffset *
                                          sizeof(VkDrawIndirectCommand),
                                      writtenDraws,
                                      (uint32_t)sizeof(VkDrawIndirectCommand));
    }

    mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
  }

  // ---------------------------------------------------------------------
  // Phase 2 of the opaque cull.
  //
  // What phase 1 drew is last frame's visible set, which is enough depth to
  // build a pyramid from. Test every candidate against it, draw the ones
  // visible now that phase 1 did not draw, and record the answer for next
  // frame.
  //
  // The scope had to close and reopen around this: the pyramid build and the
  // cull are compute dispatches and neither can run inside a render pass. The
  // second scope LOADS every attachment so phase 1's output survives.
  //
  // The TLAS is untouched by any of this -- it is whole-scene and built in
  // cWorld::PrepareFrame -- so RT shadows, GI and reflections still see the
  // geometry this pass skips.
  // ---------------------------------------------------------------------
  if (opaqueCullDispatched) {
    RICmd *opaqueCmd = &mpGraphics->primary.cmds[0];
    RIGpuScope _gsPhaseTwo(&mpGraphics->profiler, opaqueCmd, "GBuffer.phase2");
    opaqueCmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state.depthTextures[mpGraphics->swapchainIndex].Get(),
        RI_RESOURCE_STATE_DEPTH_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
        RI_STAGE_NONE, RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_DEPTH));
    const bool built =
        !state.depthSampleView[mpGraphics->swapchainIndex].isEmpty() &&
        m_hiZ->Build(opaqueCmd, mpGraphics->frameIndex, state.hiZ,
                     mpGraphics->swapchainIndex, state.width, state.height,
                     state.depthSampleView[mpGraphics->swapchainIndex].Get());
    opaqueCmd->vk_d3d12_textureBarrier(RITextureBarrier(
        state.depthTextures[mpGraphics->swapchainIndex].Get(),
        RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_DEPTH_WRITE,
        RI_STAGE_COMPUTE, RI_STAGE_NONE, RI_BARRIER_ASPECT_DEPTH));

    const uint32_t groupCount =
        (writtenDraws + kStandardCullGroupSize - 1u) / kStandardCullGroupSize;
    const bool culled =
        built && m_cull->Dispatch(
                     opaqueCmd, mpGraphics->frameIndex, opaqueCullBuffers,
                     static_cast<uint32_t>(opaqueTileReq.elementOffset), 1u,
                     static_cast<uint32_t>(opaqueGroupReq.elementOffset),
                     groupCount, kStandardCullModeVisibilityUpdate,
                     opaqueCommandWordDelta);

    if (culled) {
      gbufferColorAttachments[0].loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      gbufferColorAttachments[1].loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      opaqueCmd->vk_d3d12_beginRendering(&mpGraphics->device, gbufferBeginDesc);
      RIViewport phaseTwoViewport = {};
      phaseTwoViewport.x = 0.0f;
      phaseTwoViewport.y = (float)renderHeight;
      phaseTwoViewport.width = (float)renderWidth;
      phaseTwoViewport.height = -(float)renderHeight;
      phaseTwoViewport.depthMin = 0.0f;
      phaseTwoViewport.depthMax = 1.0f;
      RIRect phaseTwoScissor = {};
      phaseTwoScissor.width = (int16_t)renderWidth;
      phaseTwoScissor.height = (int16_t)renderHeight;
      opaqueCmd->setViewport(&mpGraphics->device, phaseTwoViewport);
      opaqueCmd->setScissor(&mpGraphics->device, phaseTwoScissor);

      const RIGraphicsPipelineDesc pipelineDesc = MakeGBufferMRTPipelineDesc(
          cGraphics::VisibilityFormat, cGraphics::VelocityFormat,
          cGraphics::DepthFormat);
      m_gbuffer.bindPipeline(&mpGraphics->device, opaqueCmd, HASH_INITIAL_VALUE,
                             "VBufferRaster.3d", pipelineDesc);
      m_gbuffer.bindBindlessDescriptorSet(
          opaqueCmd, &mpGraphics->globalset->m_bindlessSet, 0);
      m_gbuffer.bindDescriptors(&mpGraphics->device, opaqueCmd,
                                mpGraphics->frameIndex, bindings.data(),
                                bindings.size());
      opaqueCmd->drawIndirect(&mpGraphics->device, m_indirectDrawBuffer.gpu(),
                              (VkDeviceSize)opaquePhaseTwoReq.elementOffset *
                                  sizeof(VkDrawIndirectCommand),
                              writtenDraws,
                              (uint32_t)sizeof(VkDrawIndirectCommand));
      opaqueCmd->vk_d3d12_endRendering(&mpGraphics->device);
    }
  }

  // Gbuffer output -> SHADER_READ_ONLY for the downstream compute
  // passes (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
  // packedHitInfoTexture transitions UNDEFINED -> STORAGE_WRITE for the
  // POM compute pass that follows immediately.
  {
    RITextureBarrier toRead[4] = {};
    // Visibility -> SHADER_READ for the fragment + compute consumers.
    toRead[0] = {state.visibilityTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE};

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1] = {state.depthTextures[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_DEPTH_WRITE,
                 RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_STAGE_NONE,
                 RI_STAGE_COMPUTE,
                 RI_BARRIER_ASPECT_DEPTH};

    // Velocity (gbuffer MRT) -> SHADER_READ for the direct-lighting pass.
    toRead[2] = {state.velocityTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_COMPUTE};

    // packedHitInfo: UNDEFINED -> STORAGE_WRITE for the POM compute pass.
    // Previously written by the Stage B RT V-buffer; now produced by
    // VBufferPomBary.cs immediately after this barrier.
    toRead[3] = {state.packedHitInfoTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
                 RI_STAGE_NONE, RI_STAGE_COMPUTE};

    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<4>(4, toRead);
  }

  // ----------------------------------------------------------------------
  // Depth pyramid for the translucent occlusion cull.
  //
  // Built here, right after the G-buffer, because this is the cheapest point
  // at which the depth it needs is final: the G-buffer is the only pass that
  // writes depth, the barrier above has just put it in SHADER_RESOURCE for
  // COMPUTE, and every later pass reads depth rather than writing it. Building
  // it later, next to the translucent passes, would mean undoing
  // flipDepthToReadOnly() first for no gain.
  //
  // Also publishes this frame's camera record: one for all three translucent
  // families, since they share the camera and the pyramid.
  // ----------------------------------------------------------------------
  uint32_t cullCameraIndex = kStandardCullNoCamera;
  // cullViewProjection is hoisted above the G-buffer, where the opaque cull's
  // phase 1 needs it too.
  if (m_cullLoaded && m_hiZ &&
      state.hiZ.IsUsable(mpGraphics->swapchainIndex) &&
      !state.depthSampleView[mpGraphics->swapchainIndex].isEmpty()) {
    m_hiZ->Build(&mpGraphics->primary.cmds[0], mpGraphics->frameIndex, state.hiZ,
                 mpGraphics->swapchainIndex, state.width, state.height,
                 state.depthSampleView[mpGraphics->swapchainIndex].Get());

    RISegmentReq cameraReq = {};
    if (m_cullCameraSegment.request(mpGraphics->frameIndex, 1, &cameraReq) &&
        m_cullCameraBuffer.mappedAddress) {
      auto *cameraSlot = reinterpret_cast<StandardCullCamera *>(
          static_cast<uint8_t *>(m_cullCameraBuffer.mappedAddress) +
          static_cast<size_t>(cameraReq.elementOffset) *
              sizeof(StandardCullCamera));
      StandardCullCamera camera{};
      std::memcpy(camera.viewProjection, cullViewProjection.v,
                  sizeof(camera.viewProjection));
      camera.hiZWidth = state.hiZ.width;
      camera.hiZHeight = state.hiZ.height;
      camera.hiZMipCount = state.hiZ.mipCount;
      *cameraSlot = camera;
      cullCameraIndex = cameraReq.elementOffset;
    }
  }

  // ----------------------------------------------------------------------
  // Stage B — POM barycentric correction (replaces the old RT V-buffer).
  //
  // Copies visibilityTexture (raw raster hit, gPackedHitInfoRaster) into
  // packedHitInfoTexture (gPackedHitInfo) and applies the parallax-occlusion
  // barycentric perturbation for height-mapped diffuse surfaces. Water/glass
  // refraction is not handled here; those pixels carry the raster surface
  // hit in gPackedHitInfo until a future sparse refraction RT pass lands.
  // ----------------------------------------------------------------------
  {
    RIGpuScope _gsVBufferPomBary(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                "VBufferPomBary");
    const hash_t kPomHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_vBufferPomBary.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                        kPomHash, "VBufferPomBary.cs");
    m_vBufferPomBary.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                              &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
                                              VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> pomBnd;
    pomBnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      pomBnd.push_back(b);
    }
    pomBnd.emplace_back("gPackedHitInfoRaster",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.visibilityView[mpGraphics->swapchainIndex].Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
    pomBnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));

    m_vBufferPomBary.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                    mpGraphics->frameIndex, pomBnd.data(), pomBnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);
    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }
  {
    // packedHitInfo storage write -> shader read for integrate/generate/
    // direct-lighting/composite passes. Layout stays GENERAL.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT | RI_STAGE_RAY_TRACING});
  }

  // --------------------------------------------------------------------
  // ReSTIR DI: temporal/spatial reservoir reuse and shadow resolve, followed
  // by independent RELAX denoising below. Direct lighting bypasses REBLUR's
  // GI filter and is added to its indirect output in the composite.
  // --------------------------------------------------------------------
  RITextureView *directResultView = nullptr;
  {
    const uint32_t dlCur = state.directLightingIndex;
    const uint32_t dlPrev = dlCur ^ 1u;

    if (!state.directLightingInit) {
      // First use: the direct target plus the ping-ponged key/reservoir
      // textures UNDEFINED -> cleared, so the history reads are defined. The
      // clears leave them in CLEAR_STORAGE; the barriers after the clear put
      // each one into the state its role this frame needs, which is what the
      // steady-state path below then maintains.
      RITextureBarrier toGen[6] = {
          {state.directLightingTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<6>(6, toGen);

      const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (uint32_t i = 0; i < 6; ++i)
        mpGraphics->primary.cmds[0].clearStorageImage(&mpGraphics->device, toGen[i].texture, clr);

      // Establish the per-role states the two passes below expect. These used
      // to be left in GENERAL behind a single memory barrier, which Vulkan
      // accepts for both sampled and storage access but D3D12 does not: a
      // sampled read needs a shader-resource layout, and UNORDERED_ACCESS
      // cannot serve one.
      RITextureBarrier toRole[6] = {
          // Written by the spatial pass below.
          {state.directLightingTexture.Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE},
          // History, sampled by the temporal pass.
          {state.directKeyTexture[dlPrev].Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE, RI_STAGE_COMPUTE},
          {state.reservoirTexture[dlPrev].Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE, RI_STAGE_COMPUTE},
          // Written by the temporal pass, then sampled by the spatial pass.
          {state.directKeyTexture[dlCur].Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE},
          {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE},
          // Written by the spatial pass; starts read-side so the single
          // pre-spatial barrier below is the same on every frame.
          {state.reservoirTexture[dlCur].Get(), RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE, RI_STAGE_COMPUTE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<6>(6, toRole);
      state.directLightingInit = true;
    } else {
      // Last frame's RELAX input becomes this frame's resolve output.
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          {state.directLightingTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
      // Flip the ping-pong textures into this frame's roles. The indices swap
      // every frame, so each texture alternates between being sampled as
      // history and being written as the current target, and D3D12 needs a
      // real layout transition for that -- a memory barrier alone (what this
      // used to be) leaves a UAV layout that cannot serve a sampled read.
      //
      // Incoming states, all established by last frame's sequence below:
      //   reservoir[dlPrev]   STORAGE_WRITE   (last frame's spatial output)
      //   reservoirTemporal   SHADER_RESOURCE (read by last frame's spatial)
      //   directKey[dlCur]    SHADER_RESOURCE (read by last frame's spatial)
      //   directKey[dlPrev]   SHADER_RESOURCE (already correct, no transition)
      //   reservoir[dlCur]    SHADER_RESOURCE (flipped before the spatial pass)
      RITextureBarrier toRole[3] = {
          {state.reservoirTexture[dlPrev].Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.directKeyTexture[dlCur].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<3>(3, toRole);

      if (historyResetForFrame) {
        // Invalidate the reservoir history; RELAX's history is reset below.
        // The passes below overwrite every current texel.
        RITextureBarrier resetToClear[2] = {
            {state.directKeyTexture[dlPrev].Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_CLEAR_STORAGE, RI_STAGE_COMPUTE,
             RI_STAGE_NONE},
            {state.reservoirTexture[dlPrev].Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_CLEAR_STORAGE, RI_STAGE_COMPUTE,
             RI_STAGE_NONE}};
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<2>(2,
                                                                  resetToClear);

        const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        mpGraphics->primary.cmds[0].clearStorageImage(
            &mpGraphics->device, resetToClear[0].texture, clr);
        mpGraphics->primary.cmds[0].clearStorageImage(
            &mpGraphics->device, resetToClear[1].texture, clr);

        // Back to the sampled state the temporal pass reads them in, not to
        // GENERAL: these are history inputs and D3D12 samples only from a
        // shader-resource layout.
        RITextureBarrier resetAfterClear[2] = {
            {state.directKeyTexture[dlPrev].Get(),
             RI_RESOURCE_STATE_CLEAR_STORAGE, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_STAGE_NONE, RI_STAGE_COMPUTE},
            {state.reservoirTexture[dlPrev].Get(),
             RI_RESOURCE_STATE_CLEAR_STORAGE, RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_STAGE_NONE, RI_STAGE_COMPUTE}};
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<2>(2,
                                                                  resetAfterClear);
      }
    }

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    {
      RIGpuScope _gsDirectLighting(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                   "DirectLighting");
      m_directLighting.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                           kHash, "DirectLightingPass.cs");
      m_directLighting.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> bnd;
      bnd.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        bnd.push_back(b);
      }
      // Temporal pass traces no rays — only builds + reprojects reservoirs.
      bnd.emplace_back(
          "gPackedHitInfo",
          RIDescriptor::storageImage(
              &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
      bnd.emplace_back("gVelocity",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device,
                           state.velocityView[mpGraphics->swapchainIndex].Get(),
                           RI_RESOURCE_STATE_SHADER_RESOURCE));
      bnd.emplace_back("gReservoirHistory",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device, state.reservoirView[dlPrev].Get(),
                           RI_RESOURCE_STATE_SHADER_RESOURCE));
      bnd.emplace_back("gDirectKeyHistory",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device, state.directKeyView[dlPrev].Get(),
                           RI_RESOURCE_STATE_SHADER_RESOURCE));
      bnd.emplace_back("gReservoirOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.reservoirTemporalView.Get()));
      bnd.emplace_back("gDirectKeyOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.directKeyView[dlCur].Get()));

      appendWorldLightFog(bnd, apWorld);
      m_directLighting.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                       mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                       VK_PIPELINE_BIND_POINT_COMPUTE);
      mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);
    }

    // Temporal pass writes -> spatial pass sampled reads, plus the flip of
    // reservoir[dlCur] from its read-side state into the spatial pass's output.
    RITextureBarrier toSpatial[3] = {
        {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
        {state.directKeyTexture[dlCur].Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
        {state.reservoirTexture[dlCur].Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<3>(3, toSpatial);

    // ----------------------------------------------------------------
    // DirectSpatialReusePass — ReSTIR DI spatial reuse + resolve. Merges a few
    // same-surface neighbours' reservoirs, then traces ONE soft shadow ray for
    // the chosen light to demodulated irradiance. Writes reservoir[dlCur] (next
    // frame's temporal history) and raw directLighting for RELAX.
    // ----------------------------------------------------------------
    {
      RIGpuScope _gsDirectSpatialReuse(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                       "DirectSpatialReuse");
      m_directSpatialReuse.bindComputePipeline(
          &mpGraphics->device, &mpGraphics->primary.cmds[0], kHash,
          "DirectSpatialReusePass.cs");
      m_directSpatialReuse.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> sb;
      sb.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        sb.push_back(b);
      }
      sb.emplace_back(
          "gPackedHitInfo",
          RIDescriptor::storageImage(
              &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
      // optional: the TLAS is null until the first build, and this call site
      // (unlike the path tracer's) isn't guarded on it.
      sb.emplace_back(
          "gRtAccel",
          RIDescriptor::accelerationStructure(
              &mpGraphics->device, apWorld->GetTlas()), // resolve shadow ray
          0, true);
      sb.emplace_back("gReservoirIn",
                      RIDescriptor::sampledImage(
                          &mpGraphics->device, state.reservoirTemporalView.Get(),
                          RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gDirectKey",
                      RIDescriptor::sampledImage(
                          &mpGraphics->device, state.directKeyView[dlCur].Get(),
                          RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gReservoirOut",
                      RIDescriptor::storageImage(
                          &mpGraphics->device, state.reservoirView[dlCur].Get()));
      sb.emplace_back("gDirectLighting",
                      RIDescriptor::storageImage(
                          &mpGraphics->device, state.directLightingView.Get()));

      appendWorldLightFog(sb, apWorld);
      m_directSpatialReuse.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                           mpGraphics->frameIndex, sb.data(), sb.size(),
                                           VK_PIPELINE_BIND_POINT_COMPUTE);
      mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);
    }

    // Make the spatial pass's writes visible. reservoir[dlCur] is deliberately
    // left in STORAGE_WRITE: next frame it becomes dlPrev and the role flip at
    // the top of this block transitions it to SHADER_RESOURCE from exactly
    // that state.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

    // RELAX reads linear irradiance directly. Its diffuse prepass is disabled,
    // so it does not consume the unused alpha as a hit distance.
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.directLightingTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

  }

  // REBLUR only filters indirect diffuse (demodulated) and specular radiance.
  RITextureView *indirectResultView = nullptr;
  RITextureView *indirectSpecularResultView = nullptr;

  if (!state.indirectLightingInit) {
    // First use: UNDEFINED -> GENERAL + cleared so every read is defined even
    // on a frame with no TLAS; they stay GENERAL thereafter.
    RITextureBarrier toGen[9] = {
        {state.indirectRadianceTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectSpecularTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectKeyTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectKeyExtraTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdNormalRoughnessTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdDiffuseRadianceHitDistTexture.Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdSpecularRadianceHitDistTexture.Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE},
        // Kept last: barriered with the rest but deliberately not cleared. It is
        // RI_USAGE_SIMULTANEOUS_ACCESS (REBLUR samples and stores IN_MV with no
        // layout transition between), and such a texture can never be
        // UAV-cleared -- D3D12 pins its layout to COMMON, and
        // ClearUnorderedAccessView* rejects COMMON. The clear is only defensive
        // in the first place: NrdPack writes gNrdMotionVectors in full every
        // frame before any NRD dispatch reads it, so its contents are already
        // defined at every read. Clearing the others still matters because a
        // frame with no TLAS skips the path tracer that fills them.
        {state.nrdMotionVectorsTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<9>(9, toGen);

    const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < 8; ++i)
      mpGraphics->primary.cmds[0].clearStorageImage(&mpGraphics->device, toGen[i].texture, clr);

    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_CLEAR_STORAGE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
    // The clears leave these in CLEAR_STORAGE, which is already a UAV layout,
    // so the path tracer can write them without a further transition. The
    // sampled reads happen after the trace, via the barrier below.
    state.indirectInShaderResource = false;
    state.indirectLightingInit = true;
    state.indirectHistoryReset = false;
  } else {
    if (historyResetForFrame) {
      // The engine-side textures hold no history any more — they are rewritten
      // every frame — so there is nothing here to clear. All temporal state
      // lives inside NRD, which discards it via CLEAR_AND_RESTART on the next
      // Denoise call.
      if (state.nrd)
        state.nrd->ResetHistory();
      if (state.directNrd)
        state.directNrd->ResetHistory();
      state.indirectHistoryReset = false;
    }
    // Make last frame's writes visible to this frame's RT write / pack read.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
  }

  // Hand the four indirect targets back to the path tracer. Last frame ended
  // with them in the sampled state for NrdPack; the trace writes them as
  // storage, which on D3D12 is a different layout and needs a real transition.
  if (state.indirectInShaderResource) {
    RITextureBarrier toStorage[4] = {
        {state.indirectRadianceTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_RAY_TRACING},
        {state.indirectSpecularTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_RAY_TRACING},
        {state.indirectKeyTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_RAY_TRACING},
        {state.indirectKeyExtraTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_RAY_TRACING}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<4>(4, toStorage);
    state.indirectInShaderResource = false;
  }

  if (apWorld->GetTlas() != nullptr) {
    // ----------------------------------------------------------------
    // PathTracePass — per-pixel reference path tracer. Rooted at the primary
    // V-buffer hit; writes two indirect channels (albedo-demodulated diffuse +
    // undemodulated specular) and the surface key both denoise chains reject
    // on (viewZ/normal, plus GGX alpha and primary hit distance in the key extra). Needs this
    // frame's POM-corrected gPackedHitInfo, hence its position after
    // VBufferPomBary.
    // ----------------------------------------------------------------
    RIGpuScope _gsPathTrace(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                            "PathTrace");
    RIRayTracingPipelineDesc ptCreate = {};
    ptCreate.maxRecursionDepth = 1;
    ptCreate.maxPayloadSize = kScatterPayloadSize;
    ptCreate.maxAttributeSize = kTriangleAttributeSize;
    const hash_t kPtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_pathTrace.bindRayTracingPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                       kPtHash, "PathTracePass.rt", ptCreate);
    m_pathTrace.bindBindlessDescriptorSet(
        &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> ptBnd;
    ptBnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      ptBnd.push_back(b);
    }
    ptBnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
    ptBnd.emplace_back("gRtAccel", RIDescriptor::accelerationStructure(
                                       &mpGraphics->device, apWorld->GetTlas()));
    // Two radiance channels: diffuse is albedo-demodulated (the composite
    // re-applies albedo), specular is left undemodulated.
    ptBnd.emplace_back("gIndirectDiffuse",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectRadianceView.Get()));
    ptBnd.emplace_back("gIndirectSpecular",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectSpecularView.Get()));
    ptBnd.emplace_back("gIndirectKeyOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectKeyView.Get()));
    // Key extra: primary-hit GGX alpha in .x, diffuse primary hit distance in .y,
    // specular primary hit distance in .z, and .w reserved.
    ptBnd.emplace_back("gIndirectKeyExtra",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectKeyExtraView.Get()));
    appendWorldLightFog(ptBnd, apWorld);
    m_pathTrace.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                mpGraphics->frameIndex, ptBnd.data(), ptBnd.size(),
                                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
    m_pathTrace.traceRays(&mpGraphics->primary.cmds[0], kPtHash, renderWidth,
                          renderHeight, 1u);
  }
  // PathTracePass storage writes -> sampled read by the denoiser below. The
  // layout transition is unconditional even though the trace above is guarded
  // on a TLAS: NrdPack samples these every frame, so they must end up in the
  // sampled state whether or not anything was traced into them this frame.
  {
    RITextureBarrier toSampled[4] = {
        {state.indirectRadianceTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE},
        {state.indirectSpecularTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE},
        {state.indirectKeyTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE},
        {state.indirectKeyExtraTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<4>(4, toSampled);
    state.indirectInShaderResource = true;
  }

  // ----------------------------------------------------------------------
  // Denoise — NrdPack repacks this frame's lighting into NRD's layouts, then
  // one REBLUR_DIFFUSE_SPECULAR instance filters both lobes. 1 spp is far too
  // noisy to composite raw. This runs even with no TLAS (the path tracer is
  // skipped, but the pack targets and NRD's history must still advance).
  //
  // The REBLUR channels carry only path-traced indirect lighting. RELAX
  // independently filters direct irradiance using the same surface guides.
  // ----------------------------------------------------------------------
  {
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);

    {
      // Repack the path tracer's engine-native key/radiance/velocity buffers
      // into the layouts required by NRD. The pack targets are read-only after
      // this pass, except for NRD's private IN_MV, which stabilization writes;
      // transition only the slot that was previously consumed by NRD.
      if (state.nrdInputInShaderResource) {
        RITextureBarrier toStorage[5] = {
            {state.nrdNormalRoughnessTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdDiffuseRadianceHitDistTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdSpecularRadianceHitDistTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
        toStorage[4] = {
            state.nrdMotionVectorsTexture.Get(),
            RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
            RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<5>(5, toStorage);
      }

      {
        RIGpuScope _gsNrdPack(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                              "NRD.Pack");
        m_nrdPack.bindComputePipeline(&mpGraphics->device,
                                      &mpGraphics->primary.cmds[0], kHash,
                                      "NrdPack.cs");
        m_nrdPack.bindBindlessDescriptorSet(
            &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet,
            uint32_t(0), VK_PIPELINE_BIND_POINT_COMPUTE);

        std::vector<RIProgram::DescriptorBinding> nb;
        nb.reserve(12);
        {
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          nb.push_back(b);
        }
        nb.emplace_back("gIndirectKey",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device, state.indirectKeyView.Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        nb.emplace_back("gIndirectKeyExtra",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectKeyExtraView.Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        nb.emplace_back("gIndirectDiffuse",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectRadianceView.Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        nb.emplace_back("gIndirectSpecular",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectSpecularView.Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        nb.emplace_back("gVelocity",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.velocityView[mpGraphics->swapchainIndex].Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        nb.emplace_back("gNrdNormalRoughness",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdNormalRoughnessView.Get()));
        nb.emplace_back("gNrdViewZ",
                        RIDescriptor::storageImage(
                            &mpGraphics->device, state.nrdViewZView.Get()));
        nb.emplace_back("gNrdDiffuseRadianceHitDist",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdDiffuseRadianceHitDistView.Get()));
        nb.emplace_back("gNrdSpecularRadianceHitDist",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdSpecularRadianceHitDistView.Get()));
        nb.emplace_back("gNrdMotionVectors",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdMotionVectorsView.Get()));

        m_nrdPack.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                  mpGraphics->frameIndex, nb.data(), nb.size(),
                                  VK_PIPELINE_BIND_POINT_COMPUTE);
        mpGraphics->primary.cmds[0].dispatch(
            &mpGraphics->device, (renderWidth + 15u) / 16u,
            (renderHeight + 15u) / 16u, 1u);
      }

      RITextureBarrier toSampled[5] = {
          {state.nrdNormalRoughnessTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdDiffuseRadianceHitDistTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdSpecularRadianceHitDistTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          // IN_MV is sampled by most NRD dispatches and written by temporal
          // stabilization, so leave it in GENERAL with both accesses enabled.
          {state.nrdMotionVectorsTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<5>(5, toSampled);
      state.nrdInputInShaderResource = true;

      NrdFrameData nrdFrame = {};
      // NRD requires UNJITTERED camera matrices. This instance runs at the
      // full render extent, so these pixel offsets already match its INPUT
      // pixels. The half-resolution water reflection denoiser performs its one
      // conversion to input pixels in WaterReflectionPass::BeginFrame.
      std::memcpy(nrdFrame.viewToClipMatrix, temporalFrame.unjitteredProjMat,
                  sizeof(nrdFrame.viewToClipMatrix));
      std::memcpy(nrdFrame.viewToClipMatrixPrev,
                  temporalFrame.prevUnjitteredProjMat,
                  sizeof(nrdFrame.viewToClipMatrixPrev));
      std::memcpy(nrdFrame.worldToViewMatrix, temporalFrame.viewMat,
                  sizeof(nrdFrame.worldToViewMatrix));
      std::memcpy(nrdFrame.worldToViewMatrixPrev, temporalFrame.prevViewMat,
                  sizeof(nrdFrame.worldToViewMatrixPrev));
      nrdFrame.cameraJitter[0] = temporalFrame.jitterPixels[0];
      nrdFrame.cameraJitter[1] = temporalFrame.jitterPixels[1];
      nrdFrame.cameraJitterPrev[0] = temporalFrame.prevJitterPixels[0];
      nrdFrame.cameraJitterPrev[1] = temporalFrame.prevJitterPixels[1];
      nrdFrame.frameIndex = perFrame.totalFrames;
      // Past the far plane a pixel is sky; NrdPack already writes NRD_INF into
      // viewZ there, and this keeps the two consistent.
      nrdFrame.denoisingRange = apFrustum->GetFarPlane();
      nrdFrame.timeDeltaMs = afFrameTime * 1000.0f;

      NrdDenoiseInputs nrdInputs = {};
      nrdInputs.normalRoughness = state.nrdNormalRoughnessView.Get();
      nrdInputs.viewZ = state.nrdViewZView.Get();
      nrdInputs.motionVectors =
          state.nrdMotionVectorsView.Get();
      nrdInputs.diffuseRadianceHitDistance =
          state.nrdDiffuseRadianceHitDistView.Get();
      nrdInputs.specularRadianceHitDistance =
          state.nrdSpecularRadianceHitDistView.Get();

      if (state.nrd) {
        NrdDenoiseOutputs nrdOutputs = {};
        {
          RIGpuScope _gsNrdDenoise(&mpGraphics->profiler,
                                   &mpGraphics->primary.cmds[0], "NRD.Denoise");
          nrdOutputs = state.nrd->Denoise(&mpGraphics->primary.cmds[0], nrdFrame,
                                     nrdInputs);
        }
        indirectResultView = nrdOutputs.diffuseRadianceHitDistance;
        indirectSpecularResultView = nrdOutputs.specularRadianceHitDistance;
      } else {
        // NRD is absent: composite the packed radiance straight through. The
        // pack targets already carry the YCoCg encoding the composite decodes
        // (REBLUR preserves it), so only the noise differs.
        indirectResultView = nrdInputs.diffuseRadianceHitDistance;
        indirectSpecularResultView = nrdInputs.specularRadianceHitDistance;
      }

      if (state.directNrd) {
        NrdDenoiseInputs directInputs = {};
        directInputs.normalRoughness = state.nrdNormalRoughnessView.Get();
        directInputs.viewZ = state.nrdViewZView.Get();
        // RELAX does not modify motion vectors. Use the raster velocity, not
        // REBLUR's private copy that its stabilization pass may have changed.
        directInputs.motionVectors =
            state.velocityView[mpGraphics->swapchainIndex].Get();
        directInputs.diffuseRadianceHitDistance = state.directLightingView.Get();
        {
          RIGpuScope scope(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                           "NRD.DirectDenoise");
          directResultView = state.directNrd->Denoise(
              &mpGraphics->primary.cmds[0], nrdFrame, directInputs)
                                 .diffuseRadianceHitDistance;
        }
      } else {
        // RELAX takes and returns linear irradiance, so the undenoised
        // ReSTIR resolve is a drop-in substitute here.
        directResultView = state.directLightingView.Get();
      }
    }
  }

  // --------------------------------------------------------------------
  // Composite — compute pass. Reads independently filtered direct lighting,
  // REBLUR's indirect diffuse/specular outputs, and the V-buffer / TLAS / frame.
  // Writes the viewport render target. The forward passes draw on top; the
  // tail crop-blits it into the viewport backbuffer, which Scene.cpp's
  // post-effect chain + swapchain tail blit consume.
  // --------------------------------------------------------------------

  // Composite + forward passes render into the negotiated scene/input render
  // target (single image — the main draw never ping-pongs); its authored crop
  // window is handed to the viewport backbuffer at the end of Draw.

  // Barrier: make the lighting results visible to the COMPUTE composite, and
  // put the render target into GENERAL for the storage write. (The raster
  // visibility buffer and the VBufferPomBary output were already barriered to
  // the COMPUTE stage upstream.)
  {
    // SHADER_RESOURCE for the three filtered lighting image
    // samples; STORAGE_READ for the SSBOs the composite walks (light grid,
    // object / material pools) written earlier this frame.
    RIMemoryBarrier mem = {RI_RESOURCE_STATE_STORAGE_WRITE,
                           RI_RESOURCE_STATE_SHADER_RESOURCE |
                               RI_RESOURCE_STATE_STORAGE_READ,
                           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};

    RITextureBarrier imageBarriers[1] = {
        // Pogo attach -> GENERAL for the compute storage write. Discard prior
        // contents (UNDEFINED): the dispatch writes every pixel, matching the
        // old fragment pass's LOAD_OP_DONT_CARE. This also covers the
        // first-frame init for the attach half.
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE}};

    mpGraphics->primary.cmds[0].vk_d3d12_resourceBarrier<1, 0, 1>(1, &mem, 0, NULL, 1,
                                                         imageBarriers);
  }

  // Depth flip shared by the decal pre-pass (below) and the particle /
  // translucent passes further down: depth arrives in SHADER_READ_ONLY from
  // the gbuffer barrier and flips once to DEPTH_READ_ONLY for any depth-tested
  // pass.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_SHADER_RESOURCE,
         static_cast<RIResourceState_e>(RI_RESOURCE_STATE_DEPTH_READ |
                                        RI_RESOURCE_STATE_SHADER_RESOURCE),
         RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH});
    depthFlippedForReadOnly = true;
  };

  // --------------------------------------------------------------------
  // Decal pre-pass — rasterize Type="Decal" meshes (dirt_floor / moist_wall /
  // pool_wine / trails, …) into two per-viewport accumulators BEFORE the
  // composite, which then applies albedo = albedo*decalMul + decalAdd ahead of
  // lighting + fog so the decals are lit + fogged like the surface they sit on
  // (the V-buffer renderer has no albedo G-buffer to write them into the way
  // the original deferred engine did). These decals carry no alpha —
  // transparency is the blend-mode identity (white for Mul/MulX2, black for
  // Add) — so a single premultiplied-over buffer turned their no-op areas
  // opaque. Two accumulators reproduce the real blend: Mul/MulX2 multiply into
  // decalMul (cleared white), Add adds into decalAdd (cleared black).
  // Depth-tested ≤ against the gbuffer depth (no write); both accumulators are
  // CLEARED every frame so the composite never samples stale contents.
  // (Linear×linear multiply equals the legacy gamma multiply via the power law;
  // Add is a close linear approximation.)
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsDecal(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Decal");
    std::vector<iRenderable *> mulDecals; // Mul / MulX2 -> decalMul
    std::vector<iRenderable *> addDecals; // Add        -> decalAdd
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      switch (pMat->GetBlendMode()) {
      case eMaterialBlendMode_Mul:
      case eMaterialBlendMode_MulX2:
        mulDecals.push_back(pObj);
        break;
      case eMaterialBlendMode_Add:
        addDecals.push_back(pObj);
        break;
      default:
        break; // Type="Decal" content is Mul/MulX2/Add only; skip others.
      }
    }

    auto decalBlend = [](eMaterialBlendMode m) -> DecalPipelineDesc::BlendMode {
      switch (m) {
      case eMaterialBlendMode_MulX2:
        return DecalPipelineDesc::BLEND_MULX2;
      case eMaterialBlendMode_Add:
        return DecalPipelineDesc::BLEND_ADD;
      default:
        return DecalPipelineDesc::BLEND_MUL;
      }
    };

    flipDepthToReadOnly();

    // One accumulator pass: clear `tex` to the blend identity, depth-test the
    // family's decals against the gbuffer depth (read-only), and blend each
    // with its material blend mode. Always run (even empty) so the texture is
    // cleared and ends in SHADER_RESOURCE for the composite.
    auto renderDecalAccumulator = [&](RITexture *tex, const RITextureView &view,
                                      const float clearRGBA[4],
                                      const std::vector<iRenderable *> &list) {
      // Dst stage hint deliberately NONE: RENDER_TARGET must sync against
      // COLOR_ATTACHMENT_OUTPUT (derived from the state), not FRAGMENT_SHADER —
      // an explicit FRAGMENT hint here pairs COLOR_ATTACHMENT_WRITE access with
      // a stage that doesn't support it (VUID-VkImageMemoryBarrier2-dstAccessMask-03911).
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
           RI_STAGE_NONE, RI_STAGE_NONE});

      RIRenderingAttachment color = {};
      color.view = view;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      color.clearValue.color[0] = clearRGBA[0];
      color.clearValue.color[1] = clearRGBA[1];
      color.clearValue.color[2] = clearRGBA[2];
      color.clearValue.color[3] = clearRGBA[3];

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = renderWidth;
      beginDesc.renderArea.height = renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = renderWidth;
      sc.height = renderHeight;
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      if (!list.empty()) {
        m_decal.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                          &mpGraphics->globalset->m_bindlessSet, uint32_t(0));
        {
          // VS reads gPerFrame (view/proj) + gSceneObjects; FS emits the linear
          // decal colour. No fog/light buffers — the composite lights and fogs.
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          m_decal.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                  mpGraphics->frameIndex, &b, 1);
        }

        for (iRenderable *pObj : list) {
          cVertexBuffer *pVB = pObj->GetVertexBuffer();
          cMaterial *pMat = pObj->GetMaterial();
          const int indexCount = pVB->GetIndexNum();

          uint32_t materialId =
              mpGraphics->globalset->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
                  .materialId;
          if (materialId == UINT32_MAX) {
            Warning("Material Slot exhausted (decal)");
            continue;
          }

          ObjectSubmitDesc d; // decals fold into albedo; lit by the composite
          d.modelMatrix = pObj->GetModelMatrix(apFrustum);
          d.uvMatrix = pMat->GetUvMatrix();
          d.materialId = materialId;
          d.dissolveAmount = pObj->GetCoverageAmount();
          d.renderFlags = pObj->GetRenderFlags();

          const uint32_t slot = mpGraphics->globalset->submitObject(
              pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
              static_cast<cVertexBuffer *>(pVB), d);
          if (slot == UINT32_MAX) {
            Warning("bindless pool exhausted (decal)");
            continue;
          }

          uint32_t vtxMask = 0;
          if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB, "decal",
                                         &vtxMask))
            continue;

          m_decal.bindPipeline(
              &mpGraphics->device, &mpGraphics->primary.cmds[0],
              HASH_INITIAL_VALUE, "Decal",
              MakeDecalPipelineDesc(cGraphics::PogoColorFormat,
                                    cGraphics::DepthFormat,
                                    decalBlend(pMat->GetBlendMode()), vtxMask));

          mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

      // COLOR_ATTACHMENT -> SHADER_RESOURCE for the composite read.
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_RENDER_TARGET,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
           RI_STAGE_COMPUTE});
    };

    const float kIdentityMul[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // ×1
    const float kIdentityAdd[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // +0
    renderDecalAccumulator(
        state.decalMulTexture[mpGraphics->swapchainIndex].Get(),
        *state.decalMulAttachmentView[mpGraphics->swapchainIndex], kIdentityMul,
        mulDecals);
    renderDecalAccumulator(
        state.decalAddTexture[mpGraphics->swapchainIndex].Get(),
        *state.decalAddAttachmentView[mpGraphics->swapchainIndex], kIdentityAdd,
        addDecals);
  }

  // Composite compute pass — one thread per pixel writes the composite into the
  // pogo attach bound as gOutput (storage image, GENERAL).
  {
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsComposite(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                "Composite");
    m_composite.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], kHash,
                                        "Composite.cs");
    m_composite.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                              &mpGraphics->globalset->m_bindlessSet, uint32_t(0),
                                              VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(11);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    bnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
    // optional: the TLAS is null until the first build, and this call site
    // isn't guarded on it (the composite shader may or may not reflect it).
    bnd.emplace_back("gRtAccel",
                     RIDescriptor::accelerationStructure(&mpGraphics->device,
                                                         apWorld->GetTlas()),
                     0, true);
    // Direct lighting is linear RGB. REBLUR's two indirect outputs are
    // YCoCg-packed and decoded in the shader. NRD owns its outputs and leaves
    // all three in GENERAL. Without NRD these are the pack targets and the
    // ReSTIR resolve instead, which the passes above left in SHADER_RESOURCE.
    const RIResourceState_e lightingResultState =
        state.nrd ? RI_RESOURCE_STATE_GENERAL
                  : RI_RESOURCE_STATE_SHADER_RESOURCE;
    bnd.emplace_back("gDirectLighting",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                directResultView,
                                                lightingResultState));
    bnd.emplace_back("gIndirectLighting",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectResultView,
                                                lightingResultState));
    bnd.emplace_back("gIndirectSpecular",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectSpecularResultView,
                                                lightingResultState));
    // gOutput — the viewport render target bound as a storage image (GENERAL).
    bnd.emplace_back(
        "gOutput",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.renderTargetView[mpGraphics->swapchainIndex].Get()));

    // Decal accumulators from the pre-pass above (already in SHADER_RESOURCE):
    // the composite applies albedo = albedo*gDecalMul + gDecalAdd before
    // lighting.
    bnd.emplace_back(
        "gDecalMul",
        RIDescriptor::sampledImage(
            &mpGraphics->device, state.decalMulView[mpGraphics->swapchainIndex].Get()));
    bnd.emplace_back(
        "gDecalAdd",
        RIDescriptor::sampledImage(
            &mpGraphics->device, state.decalAddView[mpGraphics->swapchainIndex].Get()));

    // Per-world static decal buffers (set kWorldDecalSet), baked once by
    // cWorld::Compile. RIProgram reflects them as set 2 and binds a rotated
    // set. Worlds compiled under the hybrid renderer always have valid (>=1
    // element) buffers, so set 2 is never left unbound.
    if (RIBuffer *decalBuf = apWorld->GetDecalBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gDecals");
      b.descriptor = RIDescriptor::storageBuffer(
          &mpGraphics->device, decalBuf, 0,
          std::max<size_t>(apWorld->GetDecalCount(), 1) * sizeof(GpuDecal));
      bnd.push_back(b);
    } else {
      // optional: absent until the world is compiled.
      bnd.emplace_back("gDecals", RIDescriptor(), 0, true);
    }
    if (RIBuffer *idxBuf = apWorld->GetDecalObjectIndexBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gObjectDecalIndices");
      b.descriptor = RIDescriptor::storageBuffer(
          &mpGraphics->device, idxBuf, 0,
          std::max<size_t>(apWorld->GetDecalObjectIndices().size(), 1) *
              sizeof(uint32_t));
      bnd.push_back(b);
    } else {
      // optional: absent until the world is compiled.
      bnd.emplace_back("gObjectDecalIndices", RIDescriptor(), 0, true);
    }

    appendWorldLightFog(bnd, apWorld);
    m_composite.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                    mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);

    struct MainCompositePushConstants {
      uint32_t overlayMode;
      uint32_t hasTlas;
    };
    static_assert(sizeof(MainCompositePushConstants) == 8);
    const MainCompositePushConstants push{m_overlayMode,
                                         apWorld->GetTlas() ? 1u : 0u};
    mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
        &mpGraphics->device, m_composite, 0, sizeof(push), &push);

    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }

  // Toggle the ReSTIR key/reservoir ping-pong: this frame's writes become next
  // frame's history. Nothing else ping-pongs — NRD keeps its own history.
  state.directLightingIndex ^= 1u;

  // Render target: GENERAL (compute write) -> COLOR_ATTACHMENT_OPTIMAL so the
  // downstream raster passes find the layout they expect.
  {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_RENDER_TARGET,
         RI_STAGE_COMPUTE});
  }

  // Single render target — no toggle: the main draw never ping-pongs. The
  // downstream raster passes flip it COLOR -> SHADER_READ as they go (each
  // pass transitions in before drawing and back out after); the tail blits
  // it into the viewport backbuffer, where the post-effect chain + tail blit
  // in cScene::Render consume it.
  {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
  }

  // The composite has finished the opaque scene image, while water, particles,
  // and translucent meshes have not modified it yet. RecordOpaqueSnapshot
  // restores SHADER_RESOURCE/FRAGMENT, so the water pass sees the same state as
  // before; this boundary is outside any dynamic-rendering scope.
  if (cTemporalReactiveMask *reactiveMask = viewport->GetTemporalReactiveMask()) {
    TemporalReactiveMaskSnapshotDesc snapshot = {};
    snapshot.cmd = &mpGraphics->primary.cmds[0];
    snapshot.sceneColor = state.renderTarget[mpGraphics->swapchainIndex].Get();
    snapshot.sceneColorFormat = cGraphics::PogoColorFormat;
    snapshot.extent = {renderWidth, renderHeight};
    snapshot.frameIndex = mpGraphics->frameIndex;
    snapshot.viewportCookie = viewport;
    snapshot.sceneColorEntryState = RI_RESOURCE_STATE_SHADER_RESOURCE;
    snapshot.sceneColorExitState = RI_RESOURCE_STATE_SHADER_RESOURCE;
    snapshot.sceneColorEntryStage = RI_STAGE_FRAGMENT;
    snapshot.sceneColorExitStage = RI_STAGE_FRAGMENT;
    reactiveMask->RecordOpaqueSnapshot(snapshot);
  }

  // (depthFlippedForReadOnly + flipDepthToReadOnly are defined above, before
  // the decal pre-pass, and shared with the particle / translucent passes
  // below.)

  // (The Type="Decal" mesh decals are rasterized in the decal pre-pass above,
  // before the composite, into the decal-overlay target — not here. The
  // composite folds that overlay onto the base albedo so they are lit +
  // fogged.)

  // --------------------------------------------------------------------
  // Water pass — raster the water surface over the background the GI
  // composite already shaded. That background is NOT refracted: no refraction
  // pass runs, so the primary hit under the water is the plain rasterized
  // front surface. WaterReflectionPass produces a denoised half-resolution
  // reflection and the m_water program samples it in the ADD draw; the MUL
  // draw still applies tint + refraction exposure. The pair composes as a
  // nested over, so this pass is order-dependent: water meshes are sorted
  // back-to-front here explicitly instead of inheriting the shared translucent
  // sort. The per-object MUL-then-ADD interleaving is deliberate and required —
  // the reflection producer's guide images are one reusable scratch set, and
  // hoisting all the MULs ahead of all the ADDs would both overwrite those
  // guides before they are sampled and stop a far surface's reflection and fog
  // from being attenuated through the near surface in front of it. The blend
  // algebra is bg·M0·M1 + A0·M1 + A1. Reuses the translucent 5-stream layout +
  // TranslucentMeshPipelineDesc state (depth ≤, no write); the m_water program
  // supplies the shaders (Water.vert/frag).
  // Pogo-read-half barriers as the other translucent sub-passes.
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsWater(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Water");
    std::vector<iRenderable *> waters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat || pMat->GetMaterialID() != MaterialID::Water)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      waters.push_back(pObj);
    }

    // View space is -Z-forward here (the water fragment shader negates it), so
    // ascending view-space Z is farthest first, i.e. back-to-front. Stable so
    // meshes at equal depth keep the render list's relative order.
    std::stable_sort(waters.begin(), waters.end(),
                     [](const iRenderable *a, const iRenderable *b) {
                       return a->GetViewSpaceZ() < b->GetViewSpaceZ();
                     });

    if (!waters.empty()) {
      flipDepthToReadOnly();

      const uint32_t imageIndex = mpGraphics->swapchainIndex;

      if (!state.waterReflection)
        state.waterReflection = std::make_unique<WaterReflectionViewportState>();

      WaterReflectionFrameDesc waterFrame = {};
      waterFrame.width = renderWidth;
      waterFrame.height = renderHeight;
      waterFrame.frameIndex = perFrame.totalFrames;
      waterFrame.resetHistory = waterResetForFrame;
      std::memcpy(waterFrame.nrd.viewToClipMatrix,
                  temporalFrame.unjitteredProjMat,
                  sizeof(waterFrame.nrd.viewToClipMatrix));
      std::memcpy(waterFrame.nrd.viewToClipMatrixPrev,
                  temporalFrame.prevUnjitteredProjMat,
                  sizeof(waterFrame.nrd.viewToClipMatrixPrev));
      std::memcpy(waterFrame.nrd.worldToViewMatrix, temporalFrame.viewMat,
                  sizeof(waterFrame.nrd.worldToViewMatrix));
      std::memcpy(waterFrame.nrd.worldToViewMatrixPrev,
                  temporalFrame.prevViewMat,
                  sizeof(waterFrame.nrd.worldToViewMatrixPrev));
      // WaterReflectionPass::BeginFrame converts the full-extent temporal
      // pixel offsets into this instance's half-resolution input units.
      waterFrame.nrd.cameraJitter[0] = temporalFrame.jitterPixels[0];
      waterFrame.nrd.cameraJitter[1] = temporalFrame.jitterPixels[1];
      waterFrame.nrd.cameraJitterPrev[0] =
          temporalFrame.prevJitterPixels[0];
      waterFrame.nrd.cameraJitterPrev[1] =
          temporalFrame.prevJitterPixels[1];
      waterFrame.nrd.frameIndex = perFrame.totalFrames;
      waterFrame.nrd.denoisingRange = apFrustum->GetFarPlane();
      waterFrame.nrd.timeDeltaMs = afFrameTime * 1000.0f;
      m_waterReflection.BeginFrame(*state.waterReflection, waterFrame);

      // Shared by every producer invocation: the per-frame UBO, optional TLAS,
      // and world light/fog buffers consumed by the guide/trace/pack shaders.
      std::vector<RIProgram::DescriptorBinding> sharedBindings;
      sharedBindings.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        sharedBindings.push_back(b);
      }
      sharedBindings.emplace_back(
          "gRtAccel",
          RIDescriptor::accelerationStructure(&mpGraphics->device,
                                               apWorld->GetTlas()),
          0, true);
      appendWorldLightFog(sharedBindings, apWorld);

      // The raster water shader no longer declares gRtAccel. Keep its per-frame
      // and fog inputs, then append the three reflection views per surface
      // after RecordSurface has produced them.
      std::vector<RIProgram::DescriptorBinding> waterGraphicsBindings;
      waterGraphicsBindings.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        waterGraphicsBindings.push_back(b);
      }
      appendWorldLightFog(waterGraphicsBindings, apWorld);

      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoAttachmentBarrier(
              state.renderTarget[mpGraphics->swapchainIndex].Get(),
              /*initial=*/false));

      RITextureView colorView =
          *state.renderTargetAttachmentView[mpGraphics->swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = renderWidth;
      beginDesc.renderArea.height = renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = renderWidth;
      sc.height = renderHeight;

      struct WaterPush {
        uint32_t pass;
        uint32_t reflectionAvailable;
        uint32_t p1, p2;
      };

      // Each surface now opens its own rendering scope, so the blend chain
      // bg*M0*M1 + A0*M1 + A1 spans several render pass instances instead of
      // one. Vulkan orders colour-attachment accesses only within a single
      // instance, so the previous surface's ADD write has to be made visible
      // to the next surface's MUL blend read explicitly. The layout does not
      // change (COLOR_ATTACHMENT_OPTIMAL either way); this is purely the
      // COLOR_ATTACHMENT_OUTPUT write -> read|write dependency.
      bool waterSurfaceComposited = false;

      // Resolve slots and describe the draws before the first rendering scope
      // opens, so the cull dispatch has somewhere legal to sit.
      //
      // What this buys for water is smaller than for the other two families:
      // it hides the MUL/ADD composite draws of an occluded surface, but NOT
      // the WaterReflectionPass::RecordSurface work below, which is the
      // expensive half and is recorded unconditionally. Skipping that needs a
      // CPU-visible cull result, i.e. a readback a frame later.
      struct WaterDraw {
        iRenderable *object = nullptr;
        uint32_t slot = 0;
        uint32_t materialId = 0;
        uint32_t indexCount = 0;
        uint32_t commandSlot = 0;   // first of the MUL/ADD pair
      };
      std::vector<WaterDraw> waterDraws;
      waterDraws.reserve(waters.size());
      TranslucentCull waterCull{};
      // Two draws per surface: the MUL pass and the ADD pass.
      const bool waterCullReserved =
          ReserveCull(static_cast<uint32_t>(waters.size()) * 2u,
                      cullViewProjection, cullCameraIndex, waterCull);
      for (iRenderable *pObj : waters) {
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        const auto mat = mpGraphics->globalset->submitMaterial(
            cntx, pMat, (uint32_t)mpGraphics->frameIndex);
        if (mat.materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (water)");
          continue;
        }
        ObjectSubmitDesc d;
        d.modelMatrix = pObj->GetModelMatrix(apFrustum);
        d.uvMatrix = pMat->GetUvMatrix();
        d.materialId =
            mat.materialId; // water ids fall in the water range of materialID
        d.dissolveAmount = pObj->GetCoverageAmount();
        d.renderFlags = pObj->GetRenderFlags();
        const uint32_t slot = mpGraphics->globalset->submitObject(
            pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
            static_cast<cVertexBuffer *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (water)");
          continue;
        }

        WaterDraw draw{};
        draw.object = pObj;
        draw.slot = slot;
        draw.materialId = mat.materialId;
        draw.indexCount = static_cast<uint32_t>(pVB->GetIndexNum());
        draw.commandSlot = waterCull.commandCount;
        if (waterCull.usable) {
          cBoundingVolume *bounds = pObj->GetBoundingVolume();
          if (!bounds) {
            waterCull.usable = false;
          } else {
            for (uint32_t pass = 0; pass < 2u; ++pass)
              WriteCullDraw(waterCull, draw.commandSlot + pass,
                            /*indexed=*/true, draw.indexCount, slot,
                            bounds->GetMin(), bounds->GetMax(),
                            pObj->IsStatic(), /*neverOcclude=*/false);
          }
        }
        waterDraws.push_back(draw);
      }

      const bool waterCulled =
          waterCullReserved && waterCull.usable &&
          DispatchCull(&mpGraphics->primary.cmds[0], waterCull,
                       state.hiZ.sampleView[mpGraphics->swapchainIndex].Get());

      for (const WaterDraw &draw : waterDraws) {
        iRenderable *pObj = draw.object;
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        const int indexCount = static_cast<int>(draw.indexCount);
        const uint32_t slot = draw.slot;

        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB,
                                       "water", &vtxMask))
          continue;

        // The cookie identifies the material instance and Generation changes
        // only when its data changes; neither component varies per frame.
        const uint64_t materialSignature = hash_u64(
            hash_u64(HASH_INITIAL_VALUE, pMat->GetUniqueCookie()),
            static_cast<uint64_t>(pMat->Generation()));

        WaterReflectionSurfaceDesc surface = {};
        surface.renderableCookie = pObj->GetUniqueCookie();
        surface.materialSignature = materialSignature;
        surface.objectSlot = slot;
        surface.indexCount = static_cast<uint32_t>(indexCount);
        surface.vertexPresentMask = vtxMask;
        surface.opaqueDepthView = state.depthView[mpGraphics->swapchainIndex].Get();
        surface.tlas = apWorld->GetTlas();
        surface.sharedBindings = sharedBindings.data();
        surface.sharedBindingCount = sharedBindings.size();

        // RecordSurface is deliberately outside dynamic rendering. Its own
        // tracked transitions move the reusable half guides from the previous
        // surface's FRAGMENT reads into this surface's producer writes, so no
        // extra inter-surface barrier is needed here.
        const WaterReflectionResult result = m_waterReflection.RecordSurface(
            &mpGraphics->primary.cmds[0], *state.waterReflection, surface);

        {
          RIGpuScope compositeScope(&mpGraphics->profiler,
                                    &mpGraphics->primary.cmds[0],
                                    "Water.Composite");
          if (waterSurfaceComposited) {
            mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
                RITextureBarrier(
                    state.renderTarget[mpGraphics->swapchainIndex].Get(),
                    RI_RESOURCE_STATE_RENDER_TARGET |
                        RI_RESOURCE_STATE_RENDER_TARGET_READ,
                    RI_RESOURCE_STATE_RENDER_TARGET |
                        RI_RESOURCE_STATE_RENDER_TARGET_READ));
          }
          mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(
              &mpGraphics->device, beginDesc);

          // RecordSurface's compute/RT work clobbers graphics state. Rebind
          // every graphics descriptor set, pipeline, viewport and scissor for
          // this surface before sampling its returned views.
          m_water.bindBindlessDescriptorSet(
              &mpGraphics->primary.cmds[0],
              &mpGraphics->globalset->m_bindlessSet, uint32_t(0));
          std::vector<RIProgram::DescriptorBinding> graphicsBindings =
              waterGraphicsBindings;
          if (result.available) {
            // The specular result is an NRD output, which NRD keeps in GENERAL
            // for its whole lifetime; that stays legal on D3D12 because those
            // textures are RI_USAGE_SIMULTANEOUS_ACCESS. The two guides are
            // WaterReflectionPass's own and are left in the sampled state by
            // the transitions at the end of its Build.
            graphicsBindings.emplace_back(
                "gWaterReflectionSpecular",
                RIDescriptor::sampledImage(
                    &mpGraphics->device, result.specularRadianceHitDist,
                    RI_RESOURCE_STATE_GENERAL));
            graphicsBindings.emplace_back(
                "gWaterReflectionGuidePosViewZ",
                RIDescriptor::sampledImage(
                    &mpGraphics->device, result.halfPositionViewZ,
                    RI_RESOURCE_STATE_SHADER_RESOURCE));
            graphicsBindings.emplace_back(
                "gWaterReflectionGuideNormalWeight",
                RIDescriptor::sampledImage(
                    &mpGraphics->device, result.halfNormalWeight,
                    RI_RESOURCE_STATE_SHADER_RESOURCE));
          } else {
            // The shader branches before sampling when unavailable, but all
            // three reflected descriptors must still be valid and written.
            const RIDescriptor placeholder =
                mpGraphics->whiteTexture2DDescriptor();
            graphicsBindings.emplace_back("gWaterReflectionSpecular",
                                          placeholder);
            graphicsBindings.emplace_back("gWaterReflectionGuidePosViewZ",
                                          placeholder);
            graphicsBindings.emplace_back(
                "gWaterReflectionGuideNormalWeight", placeholder);
          }
          m_water.bindDescriptors(
              &mpGraphics->device, &mpGraphics->primary.cmds[0],
              mpGraphics->frameIndex, graphicsBindings.data(),
              graphicsBindings.size());
          mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
          mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

          // Two draws into the pogo: tint (MUL) then denoised reflection (ADD).
          // Salt the pipeline hash so it does not collide with the translucent
          // program's cache (same fixed-function state, different shaders).
          const TranslucentMeshPipelineDesc::BlendMode modes[2] = {
              TranslucentMeshPipelineDesc::BLEND_MUL,
              TranslucentMeshPipelineDesc::BLEND_ADD};
          for (uint32_t pass = 0; pass < 2u; ++pass) {
            m_water.bindPipeline(&mpGraphics->device,
                                 &mpGraphics->primary.cmds[0],
                                 HASH_INITIAL_VALUE, "Water",
                                 MakeTranslucentMeshPipelineDesc(
                                     cGraphics::PogoColorFormat,
                                     cGraphics::DepthFormat, modes[pass],
                                     vtxMask));
            WaterPush push = {pass, result.available ? 1u : 0u, 0u, 0u};
            mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
                &mpGraphics->device, m_water, 0, sizeof(push), &push);
            // The MUL and ADD passes own consecutive command slots. Both carry
            // the same bounds, so the cull either keeps or hides the pair
            // together and the bg*M0*M1 + A0*M1 + A1 algebra stays consistent.
            if (waterCulled) {
              const uint64_t offset =
                  static_cast<uint64_t>(waterCull.commandBase +
                                        draw.commandSlot + pass) *
                  sizeof(VkDrawIndexedIndirectCommand);
              mpGraphics->primary.cmds[0].drawIndexedIndirect(
                  &mpGraphics->device, m_cullCommandBuffer.gpu(), offset, 1,
                  sizeof(VkDrawIndexedIndirectCommand));
            } else {
              mpGraphics->primary.cmds[0].drawIndexed(
                  &mpGraphics->device, static_cast<uint32_t>(indexCount), 1u,
                  0u, 0, slot);
            }
          }
          mpGraphics->primary.cmds[0].vk_d3d12_endRendering(
              &mpGraphics->device);
          waterSurfaceComposited = true;

        }
      }

      m_waterReflection.EndFrame(*state.waterReflection);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          RI_PogoShaderBarrier(
              state.renderTarget[mpGraphics->swapchainIndex].Get(),
              /*initial=*/false));
    }
  }

  // --------------------------------------------------------------------
  // Translucent pass — two sub-passes, both into the pogo "read" half:
  //   1. Particle pass (this block) — particle emitters only.
  //   2. Mesh pass (below)          — non-particle translucent meshes.
  // Each opens its own begin/endRendering + pogo barriers, so either is
  // skippable when empty.
  //
  // Resources reused from the opaque path:
  //   - Interface<cGraphics>::Get()->globalset->m_objectSlots / m_objectBuffer (per-renderable OBJECT
  //   slot)
  //   - m_opaque*Handles (BDA fan-out: particles/meshes overload
  //                       position/uv0/color/index with their own VB addresses)
  //   - Interface<cGraphics>::Get()->globalset->m_materialBindless / m_materialBuffer (material slot)
  //
  // Sync: (a) swapchain stays COLOR_ATTACHMENT_OPTIMAL from the composite
  //           (load to preserve it); (b) flipDepthToReadOnly() moves depth back
  //           to DEPTH_READ_ONLY_OPTIMAL (shared with the decal pass).
  // --------------------------------------------------------------------
  {
    // Collect particle emitters from the translucent list once so we can
    // skip the whole pass (and its barriers/begin-rendering) when empty.
    RIGpuScope _gsParticle(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Particle");
    std::vector<iParticleEmitter *> emitters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj || pObj->GetRenderType() != eRenderableType_ParticleEmitter)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None ||
          mode >= eMaterialBlendMode_LastEnum)
        continue;
      emitters.push_back(static_cast<iParticleEmitter *>(pObj));
    }

    // Build every emitter's scratch geometry and resolve its object slot here,
    // before the render scope opens, so the occlusion cull can describe the
    // draws and record its dispatch -- which cannot happen inside dynamic
    // rendering. The emitters keep their relative order, so the scratch
    // segments are filled exactly as they were when this ran inside the loop.
    //
    // Note the cull only saves rasterization for particles: the scratch
    // geometry is still built for an emitter the GPU later hides, because the
    // cull result lives on the GPU and is never read back.
    struct ParticleDraw {
      iParticleEmitter *emitter = nullptr;
      cMaterial *material = nullptr;
      uint32_t slot = 0;
      uint32_t indexCount = 0;
      uint32_t commandSlot = 0;
    };
    std::vector<ParticleDraw> particleDraws;
    particleDraws.reserve(emitters.size());
    TranslucentCull particleCull{};
    const bool particleCullReserved =
        ReserveCull(static_cast<uint32_t>(emitters.size()), cullViewProjection,
                    cullCameraIndex, particleCull);
    for (iParticleEmitter *pEmitter : emitters) {
      cMaterial *pMat = pEmitter->GetMaterial();
      if (!pMat)
        continue;
      // Per-frame scratch geometry (no persistent emitter VB): build this
      // frame's camera-facing quads into the shared translucentVtx/Idx
      // segments -- the same single producer as the wireframe/simple panes.
      auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime,
                                                 /*withUv=*/true);
      if (!geom.valid)
        continue;

      const uint32_t materialId =
          mpGraphics->globalset
              ->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
              .materialId;
      if (materialId == UINT32_MAX) {
        Warning("Material Slot exhausted (particle)");
        continue;
      }

      ObjectSubmitDesc d; // particle: identity uv, no dissolve/illum
      d.modelMatrix = pEmitter->GetModelMatrix(apFrustum);
      d.materialId = materialId;
      // Keep backend-neutral buffer references here. submitObject resolves them
      // to Vulkan device addresses or D3D12 raw-SRV indices for the active API.
      d.streamRefs.pos = {mpGraphics->translucentVtxBuffer.Get(),
                          geom.posByteOffset};
      d.streamRefs.color = {mpGraphics->translucentVtxBuffer.Get(),
                            geom.colByteOffset};
      d.streamRefs.uv0 = {mpGraphics->translucentVtxBuffer.Get(),
                          geom.uvByteOffset};
      d.streamRefs.index = {mpGraphics->translucentIdxBuffer.Get(),
                            geom.idxByteOffset};
      d.streamRefs.set = true;

      // Particles share the object-slot pool with opaque solids; the payload
      // submit also bumps the slot generation when the slot is (re)assigned --
      // so a consumer still anchored to the slot's previous opaque occupant
      // self-invalidates before it dereferences this slot's smaller streams.
      const uint32_t slot = mpGraphics->globalset->submitObject(
          pEmitter->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex, nullptr,
          d, kSubmitData);
      if (slot == UINT32_MAX) {
        Warning("bindless pool exhausted (particle)");
        continue;
      }

      ParticleDraw draw{};
      draw.emitter = pEmitter;
      draw.material = pMat;
      draw.slot = slot;
      draw.indexCount = static_cast<uint32_t>(geom.indexCount);
      draw.commandSlot = particleCull.commandCount;
      if (particleCull.usable) {
        cBoundingVolume *bounds = pEmitter->GetBoundingVolume();
        if (!bounds) {
          particleCull.usable = false;
        } else {
          // Non-indexed: the particle VS pulls indices via BDA, so the draw is
          // a vertex-count draw and the command is the 4-word layout.
          WriteCullDraw(particleCull, draw.commandSlot, /*indexed=*/false,
                        draw.indexCount, slot, bounds->GetMin(),
                        bounds->GetMax(), pEmitter->IsStatic(),
                        /*neverOcclude=*/false);
        }
      }
      particleDraws.push_back(draw);
    }

    const bool particleCulled =
        particleCullReserved && particleCull.usable &&
        DispatchCull(&mpGraphics->primary.cmds[0], particleCull,
                     state.hiZ.sampleView[mpGraphics->swapchainIndex].Get());

    if (!particleDraws.empty()) {
      // Per-emitter UpdateGraphicsForFrame / UpdateGraphicsForViewport +
      // SubmitToGPU already happened in the consolidated translucent prepare
      // loop near the top of Draw(); the particle VBs are uploaded and
      // attached to the frame context by the time we get here.
      flipDepthToReadOnly();

      // Render translucent particles INTO the pogo "read" half (which holds the
      // composited + post-effected scene), not the swapchain — so the pogo (and
      // anything that samples it, e.g. the menu/inventory screen capture)
      // carries particles too. Flip that half COLOR_ATTACHMENT for the draw,
      // then back to SHADER_READ after, so the tail blit below can sample it.
      const RI_Format_e particleTargetFormat = cGraphics::PogoColorFormat;
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView =
          *state.renderTargetAttachmentView[mpGraphics->swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = renderWidth;
      beginDesc.renderArea.height = renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = renderWidth;
      sc.height = renderHeight;
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      m_particle.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                           &mpGraphics->globalset->m_bindlessSet, uint32_t(0));

      std::vector<RIProgram::DescriptorBinding> particleBindings;
      particleBindings.reserve(2);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        particleBindings.push_back(b);
      }
      // Soft particles: scene depth (opaque geometry) for the per-pixel fade.
      // Runs after flipDepthToReadOnly() above and declares the same combined
      // state that barrier applies. The depth attachment is read-only, so the
      // feedback loop is legal with no extra barrier. The shader-resource bit
      // is required: D3D12 samples only from a layout that admits an SRV, and
      // DEPTH_READ alone maps to DEPTH_STENCIL_READ, which does not. Vulkan is
      // unaffected -- DEPTH_READ still wins the layout choice, so this stays
      // DEPTH_READ_ONLY_OPTIMAL there.
      particleBindings.emplace_back(
          "gSceneDepth",
          RIDescriptor::sampledImage(
              &mpGraphics->device, state.depthSampleView[mpGraphics->swapchainIndex].Get(),
              static_cast<RIResourceState_e>(RI_RESOURCE_STATE_DEPTH_READ |
                                             RI_RESOURCE_STATE_SHADER_RESOURCE)));
      appendWorldLightFog(particleBindings, apWorld);
      m_particle.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
                                 particleBindings.data(),
                                 particleBindings.size());

      // Map eMaterialBlendMode -> ParticlePipelineDesc::BlendMode +
      // shader-side BLEND_MODE_*. eMaterialBlendMode_None is filtered above.
      auto remapBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:
          return ParticlePipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:
          return ParticlePipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:
          return ParticlePipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:
          return ParticlePipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:
          return ParticlePipelineDesc::BLEND_PREMUL_ALPHA;
        default:
          return ParticlePipelineDesc::BLEND_ADD;
        }
      };

      struct PushBlock {
        uint32_t blendMode;
        float sceneAlpha;
      };

      for (const ParticleDraw &draw : particleDraws) {
        cMaterial *pMat = draw.material;
        // Scratch geometry, material slot and object slot were all resolved
        // before the render scope opened, so the cull could describe this draw.
        const int indexCount = (int)draw.indexCount;
        const uint32_t slot = draw.slot;

        const ParticlePipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        m_particle.bindPipeline(
            &mpGraphics->device, &mpGraphics->primary.cmds[0],
            HASH_INITIAL_VALUE, "Particle",
            MakeParticlePipelineDesc(particleTargetFormat,
                                     cGraphics::DepthFormat, mode));

        // Fog (world + per-area) is applied per-pixel in Particle.frag.slang
        // by walking gFogAreas. sceneAlpha is now the unmodified per-object
        // scalar (1.0 by default) — kept in the push block for parity with the
        // mesh path and any future per-object alpha gates.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha};
        mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(&mpGraphics->device, m_particle, 0,
                                                     sizeof(push), &push);

        if (particleCulled) {
          // drawCount is 1, so Vulkan never reads the stride and the uniform
          // 5-word slot serves the 4-word non-indexed command fine.
          const uint64_t offset =
              static_cast<uint64_t>(particleCull.commandBase + draw.commandSlot) *
              sizeof(VkDrawIndexedIndirectCommand);
          mpGraphics->primary.cmds[0].drawIndirect(
              &mpGraphics->device, m_cullCommandBuffer.gpu(), offset, 1,
              sizeof(VkDrawIndexedIndirectCommand));
        } else {
          mpGraphics->primary.cmds[0].draw(&mpGraphics->device, (uint32_t)indexCount, 1u, 0u, slot);
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample
      // it.
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
      }
    }
  }

  // --------------------------------------------------------------------
  // Translucent mesh pass (sub-pass 2 — see the comment above the particle
  // block for the overall plan). Runs after particles so any glow billboards
  // composite under solid translucent geometry, mirroring the legacy
  // back-to-front order. SortFunc_Translucent in RenderList.cpp already
  // ordered the translucent list back-to-front, so iterating in list order
  // here gives correct Alpha-blend results.
  //
  // Refraction is currently NOT implemented: the legacy screen-copy
  // bend-behind distortion was dropped, and the RT V-buffer pass meant to
  // replace it (bending the primary ray at refractive surfaces) was retired
  // before it ever ran. So refractive translucents just alpha-blend over the
  // unrefracted composite. Water is filtered out of the mesh collection
  // below; it has its own raster pass (m_water) over that same unrefracted
  // background.
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsTranslucent(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                              "TranslucentMesh");
    std::vector<iRenderable *> meshes;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj)
        continue;
      if (pObj->GetRenderType() == eRenderableType_ParticleEmitter)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None ||
          mode >= eMaterialBlendMode_LastEnum)
        continue;
      // No material-flag filtering here: HasRefraction draws through the
      // standard blend pipeline (the background it blends over is the
      // unrefracted composite — no refraction pass runs); HasWorldReflection
      // is harmless (the legacy-only planar reflection buffer is unused).
      // Water is skipped here because it has its own raster sub-pass
      // (m_water, above) with the MUL + ADD draw pair.
      if (pMat->GetMaterialID() == MaterialID::Water)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      meshes.push_back(pObj);
    }

    // Resolve every mesh's material and object slot BEFORE the render scope
    // opens: the occlusion cull needs each draw's command written and its
    // dispatch recorded, and a dispatch cannot be recorded inside dynamic
    // rendering. The draw loop below reuses what this pass resolved instead of
    // submitting again.
    struct MeshDraw {
      iRenderable *object = nullptr;
      uint32_t slot = 0;
      uint32_t materialId = 0;
      uint32_t indexCount = 0;
      bool cubeMap = false;
      uint32_t commandSlot = 0;   // first of 1 or 2 consecutive slots
    };
    std::vector<MeshDraw> meshDraws;
    meshDraws.reserve(meshes.size());
    TranslucentCull meshCull{};
    // Worst case is two draws per mesh: the base draw plus the cube-map one.
    const bool meshCullReserved =
        ReserveCull(static_cast<uint32_t>(meshes.size()) * 2u,
                    cullViewProjection, cullCameraIndex, meshCull);
    for (iRenderable *pObj : meshes) {
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      cMaterial *pMat = pObj->GetMaterial();
      const uint32_t materialId =
          mpGraphics->globalset
              ->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
              .materialId;
      if (materialId == UINT32_MAX) {
        Warning("Material Slot exhausted (translucent mesh)");
        continue;
      }
      ObjectSubmitDesc d;
      d.modelMatrix = pObj->GetModelMatrix(apFrustum);
      d.uvMatrix = pMat->GetUvMatrix();
      d.materialId = materialId;
      d.dissolveAmount = pObj->GetCoverageAmount();
      d.renderFlags = pObj->GetRenderFlags();
      const uint32_t slot = mpGraphics->globalset->submitObject(
          pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
          static_cast<cVertexBuffer *>(pVB), d);
      if (slot == UINT32_MAX) {
        Warning("bindless pool exhausted (translucent mesh)");
        continue;
      }

      MeshDraw draw{};
      draw.object = pObj;
      draw.slot = slot;
      draw.materialId = materialId;
      draw.indexCount = static_cast<uint32_t>(pVB->GetIndexNum());
      draw.cubeMap = pMat->GetImage(eMaterialTexture_CubeMap) != nullptr;
      draw.commandSlot = meshCull.commandCount;
      const uint32_t draws = draw.cubeMap ? 2u : 1u;
      if (meshCull.usable) {
        // A renderable with no bounds cannot be occlusion-tested, and leaving
        // its command unwritten would draw garbage -- so the whole family
        // falls back rather than culling part of it.
        cBoundingVolume *bounds = pObj->GetBoundingVolume();
        if (!bounds) {
          meshCull.usable = false;
        } else {
          // One candidate per DRAW, not per mesh: a candidate owns exactly one
          // instanceCount word and the cube-map variant has its own.
          for (uint32_t i = 0; i < draws; ++i)
            WriteCullDraw(meshCull, draw.commandSlot + i, /*indexed=*/true,
                          draw.indexCount, slot, bounds->GetMin(),
                          bounds->GetMax(), pObj->IsStatic(),
                          /*neverOcclude=*/false);
        }
      }
      meshDraws.push_back(draw);
    }

    // All or nothing: the family either culls every one of its draws or none
    // of them, so a short reservation never silently drops geometry.
    const bool meshCulled =
        meshCullReserved && meshCull.usable &&
        DispatchCull(&mpGraphics->primary.cmds[0], meshCull,
                     state.hiZ.sampleView[mpGraphics->swapchainIndex].Get());

    if (!meshDraws.empty()) {
      // Per-mesh UpdateGraphicsForFrame / UpdateGraphicsForViewport +
      // SubmitToGPU already happened in the consolidated translucent prepare
      // loop near the top of Draw(); billboards / beams / glass / water all
      // have valid vk.buffer + (where applicable) BLAS by the time we get
      // here. Depth flip is idempotent — safe to call regardless of whether
      // the particle pass ran above.
      flipDepthToReadOnly();

      const RI_Format_e meshTargetFormat = cGraphics::PogoColorFormat;

      // SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL. If the particle pass
      // ran above, that block left the pogo half in SHADER_READ_ONLY (for
      // a tail blit that never got to run); if it didn't, the visibility
      // composite + post-effect chain also left it in SHADER_READ_ONLY. The
      // barrier helper handles either source state.
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView =
          *state.renderTargetAttachmentView[mpGraphics->swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = renderWidth;
      beginDesc.renderArea.height = renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = renderWidth;
      sc.height = renderHeight;
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      m_translucentMesh.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, uint32_t(0));

      std::vector<RIProgram::DescriptorBinding> meshBindings;
      meshBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        meshBindings.push_back(b);
      }
      appendWorldLightFog(meshBindings, apWorld);
      // Lit diffuse is guarded by a push flag until the first TLAS build.
      meshBindings.emplace_back("gRtAccel",
          RIDescriptor::accelerationStructure(&mpGraphics->device, apWorld->GetTlas()),
          0, true);
      m_translucentMesh.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                        mpGraphics->frameIndex, meshBindings.data(),
                                        meshBindings.size());

      // Map eMaterialBlendMode -> TranslucentMeshPipelineDesc::BlendMode +
      // shader-side BLEND_MODE_*. Mirrors the same remap used by the
      // particle pass; eMaterialBlendMode_None is filtered above.
      auto remapBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:
          return TranslucentMeshPipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:
          return TranslucentMeshPipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:
          return TranslucentMeshPipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:
          return TranslucentMeshPipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:
          return TranslucentMeshPipelineDesc::BLEND_PREMUL_ALPHA;
        default:
          return TranslucentMeshPipelineDesc::BLEND_ADD;
        }
      };

      // Mirrors TranslucentPushConstants in Translucent.frag.slang. Options
      // bitfield carries TRANS_OPT_USE_ILLUMINATION for the optional second
      // cube-map-only draw and TLAS availability for direct lighting.
      struct PushBlock {
        uint32_t blendMode;
        float sceneAlpha;
        uint32_t options;
        uint32_t _pad;
      };
      constexpr uint32_t kTransOptUseIllumination = 1u << 0;
      constexpr uint32_t kTransOptHasTlas = 1u << 1;
      const uint32_t lightingOptions = apWorld->GetTlas() ? kTransOptHasTlas : 0u;

      for (const MeshDraw &draw : meshDraws) {
        iRenderable *pObj = draw.object;
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        // Filter above already rejected null pVB / pMat and 0-index VBs.
        const int indexCount = static_cast<int>(draw.indexCount);
        // The material and object slots were resolved before the render scope
        // opened, so the cull could describe this draw. AffectedByLightLevel
        // dimming is evaluated per-vertex on the GPU (Translucent.vert.slang →
        // gScene.lightLevelAt, gated on kMaterialFlagAffectedByLightLevel in
        // the material config) — the legacy per-object CPU light loop that
        // used to live here is gone.
        const uint32_t slot = draw.slot;

        // Per-vertex streams use fixed-function vertex fetch (the pipeline
        // declares them as VkVertexInputBindingDescriptions — see
        // TranslucentMeshPipelineDesc). Bindless OBJECT-slot handle arrays
        // (m_opaque*Mirror) stay populated only on the opaque + TLAS paths.
        // detail::BindVertexStreams binds pos/normal/tangent/color/uv0 + index
        // (with fallbacks for absent optional streams) for both draws below.
        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB, "translucent",
                                       &vtxMask))
          continue;

        const TranslucentMeshPipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        m_translucentMesh.bindPipeline(
            &mpGraphics->device, &mpGraphics->primary.cmds[0],
            HASH_INITIAL_VALUE, "TranslucentMesh",
            MakeTranslucentMeshPipelineDesc(meshTargetFormat,
                                            cGraphics::DepthFormat, mode,
                                            vtxMask, pMat->GetDepthTest()));

        // Fog (world + per-area) is applied per-pixel in Translucent.frag.slang
        // by walking gFogAreas. sceneAlpha stays 1.0 for the no-extra-alpha
        // common path.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha, lightingOptions, 0u};
        mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
            &mpGraphics->device, m_translucentMesh, 0, sizeof(push), &push);

        // Culled draws go through the command the cull kernel just vetted: it
        // rewrote instanceCount to 0 for anything provably hidden, so a culled
        // mesh rasterizes nothing while every per-item bind above still ran.
        // drawCount is 1, so Vulkan never reads the stride.
        if (meshCulled) {
          const uint64_t offset =
              static_cast<uint64_t>(meshCull.commandBase + draw.commandSlot) *
              sizeof(VkDrawIndexedIndirectCommand);
          mpGraphics->primary.cmds[0].drawIndexedIndirect(
              &mpGraphics->device, m_cullCommandBuffer.gpu(), offset, 1,
              sizeof(VkDrawIndexedIndirectCommand));
        } else {
          mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u, 0u,
                                         0, slot);
        }

        // Second draw for the cube-map Fresnel + rim contribution.
        // Reference gates this on `cubeMap && !isRefraction`
        // (RendererDeferred.cpp:4660) because its refraction path consumed
        // the screen copy the cube-map draw would have overwritten. Nothing
        // here refracts (no refraction pass exists), so there is no such
        // conflict and the cube-map second draw stays gated only on whether
        // the material carries a cube map.
        if (draw.cubeMap) {
          m_translucentMesh.bindPipeline(
              &mpGraphics->device, &mpGraphics->primary.cmds[0],
              HASH_INITIAL_VALUE, "TranslucentMeshIllum",
              MakeTranslucentMeshPipelineDesc(
                  meshTargetFormat, cGraphics::DepthFormat,
                  TranslucentMeshPipelineDesc::BLEND_ADD, vtxMask,
                  pMat->GetDepthTest()));
          PushBlock pushIllum = {
              (uint32_t)TranslucentMeshPipelineDesc::BLEND_ADD, sceneAlpha,
              kTransOptUseIllumination | lightingOptions, 0u};
          mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
              &mpGraphics->device, m_translucentMesh, 0, sizeof(pushIllum), &pushIllum);
          // Vertex / index buffers stay bound from the main draw above —
          // same renderable, just a second pipeline + push-constant set.
          // It owns the NEXT command slot: its own candidate, so the cull can
          // hide the reflection pass independently of the base draw.
          if (meshCulled) {
            const uint64_t offset =
                static_cast<uint64_t>(meshCull.commandBase + draw.commandSlot +
                                      1u) *
                sizeof(VkDrawIndexedIndirectCommand);
            mpGraphics->primary.cmds[0].drawIndexedIndirect(
                &mpGraphics->device, m_cullCommandBuffer.gpu(), offset, 1,
                sizeof(VkDrawIndexedIndirectCommand));
          } else {
            mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u,
                                           0u, 0, slot);
          }
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
          state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
    }
  }

  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout. Depth ends here
  // either in SHADER_RESOURCE (compute-only) or, when flipDepthToReadOnly ran
  // for particle/decal, in the combined depth-read + shader-resource state
  // that lambda applies. The before-state must name that combination exactly:
  // the two differ on D3D12 (DIRECT_QUEUE_GENERIC_READ, the only read layout
  // admitting both depth-test and SRV, versus DEPTH_STENCIL_READ), and a
  // mismatch here is rejected as INCOMPATIBLE_BARRIER_LAYOUT. On Vulkan both
  // spell DEPTH_READ_ONLY_OPTIMAL, since DEPTH_READ wins the layout choice.
  {
    const uint32_t beforeState = depthFlippedForReadOnly
                                     ? (RI_RESOURCE_STATE_DEPTH_READ |
                                        RI_RESOURCE_STATE_SHADER_RESOURCE)
                                     : RI_RESOURCE_STATE_SHADER_RESOURCE;
    const uint32_t beforeStages = depthFlippedForReadOnly
                                      ? RI_STAGE_NONE
                                      : (RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE);

    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[mpGraphics->swapchainIndex].Get(), beforeState,
         RI_RESOURCE_STATE_DEPTH_WRITE, beforeStages, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH});
  }

  // DebugDraw overlay (editor panes: grid / gizmos / icons, enqueued by the
  // viewport's OnPreWorldDraw callbacks): draw into the finished scene in
  // the render target against the scene depth, so cScene's pogo feed carries
  // the overlay along. The render target sits in SHADER_READ_ONLY (left by
  // the post-composite flip / last forward pass); flip it to COLOR for the
  // overlay pass and back to SHADER_READ after — Draw's contract is to leave
  // the finished frame in the render target, SHADER_READ (the BackBuffer).
  DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
  const bool debugOverlayDrawn = debugDraw && debugDraw->HasRequests();
  if (debugOverlayDrawn) {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT});

    {
      RITextureView colorView =
          *state.renderTargetAttachmentView[mpGraphics->swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth, restored to DEPTH_ATTACHMENT_OPTIMAL just above; the
      // overlay pipelines test against it but never write. LOAD, no clear.
      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = renderWidth;
      beginDesc.renderArea.height = renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

      debugDraw->flush(cntx, &mpGraphics->primary.cmds[0], apFrustum, renderWidth,
                       renderHeight, cGraphics::PogoColorFormat);

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
    }
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
  }

  // Commit every bindless handle / slot-generation write made this frame. The
  // uploader records into its own transfer cmd buffer, flushed as a fenced
  // pre-pass the primary submit waits on (cGraphics), so this single tail
  // call lands all copies ahead of every primary read regardless of recording
  // order.
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);
}

cHybridRenderer::~cHybridRenderer() {
  // The ray-tracing TLAS + its storage/instance buffers now live on cWorld and
  // are disposed with the world.
  // The global managed set is engine-lifetime; ShutdownGlobalManagedSets()
  // (cGraphics teardown) destroys it after the device is idle.
  // Fallback vertex streams are cGraphics members (process-lifetime, like
  // Interface<cGraphics>::Get()->nulVertexBuffer); not freed here.
  // Every pipeline program owned by the renderer. dispose() frees the pipelines,
  // pipeline layout, descriptor-set layouts, and (for m_pathTrace) the ray-tracing
  // SBT buffer — the last of which is a VMA allocation that otherwise trips the
  // vmaDestroyAllocator leak assert at device teardown. Safe on an unused program.
  m_waterReflection.Dispose(mpGraphics);

  RIProgram *programs[] = {
      &m_gbuffer,        &m_vBufferPomBary,
      &m_lightGrid,      &m_composite,           &m_directLighting,
      &m_directSpatialReuse, &m_nrdPack,         &m_lightProbe,
      &m_particle,
      &m_translucentMesh, &m_decal,               &m_water,
      &m_pathTrace,
  };
  for (RIProgram *p : programs)
    p->dispose(&mpGraphics->device);

  // The borrowed Standard passes own only their programs; destroying them here
  // rather than leaving it to the unique_ptr keeps the disposal ordered with
  // the programs above, while the device is still alive.
  if (m_hiZ)
    m_hiZ->DestroyData();
  m_hiZ.reset();
  if (m_cull)
    m_cull->DestroyData();
  m_cull.reset();
  m_cullLoaded = false;

  // Staged indirect buffers: both halves (host staging + device copy).
  for (StagedIndirectBuffer *staged :
       {&m_indirectDrawBuffer, &m_cullCommandBuffer}) {
    staged->host.dispose(&mpGraphics->device);
    staged->device.dispose(&mpGraphics->device);
    *staged = {};
  }

  // The cull rings: the translucent families' plus the opaque two-phase
  // candidate ring and its persistent visibility table. Anything created in the
  // constructor has to appear here or it outlives the device.
  RIBuffer *cullBuffers[] = {
      &m_cullCandidateBuffer,  &m_cullTileBuffer,
      &m_cullGroupBuffer,      &m_cullCameraBuffer,    &m_cullDrawCountBuffer,
      &m_cullVisibilityBuffer, &m_cameraCandidateBuffer};
  for (RIBuffer *b : cullBuffers) {
    b->dispose(&mpGraphics->device);
    *b = {};
  }
}

} // namespace hpl
