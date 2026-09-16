#include "graphics/GlobalManagedSets.h"

#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/Graphics.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer.h"
#include "math/Math.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/LowLevelSystem.h"

#include <cstring>
#include <iterator>
#include <span>

namespace hpl {

GlobalManagedSets::GlobalManagedSets()
    : m_objectSlots(kObjectSlotCapacity, /*frameInFlight*/ 0),
      m_materialBindless(kMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT) {}

// Out-of-line so the SharedResourceHandle<Image> members release here, where
// Image is complete.
GlobalManagedSets::~GlobalManagedSets() = default;

void GlobalManagedSets::initialize(RIDevice *device, cResources *resources) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  {
    std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
    // Stage mask shared by every binding the RT pipeline touches —
    // raygen/any-hit/closest-hit/miss all need bindless texture+vertex
    // access for the alpha test, camera-ray reconstruction, and material
    // shading inside the path-tracer.
    const VkShaderStageFlags kSharedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                             VK_SHADER_STAGE_FRAGMENT_BIT |
                                             VK_SHADER_STAGE_COMPUTE_BIT;
    const VkShaderStageFlags kRtStages =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    const VkShaderStageFlags kRtSharedStages =
        kSharedStages | (device->rayTracingPipelineEnabled ? kRtStages : 0);
    // textures_2d[] — sampled by the gbuffer, the composite, and the
    // RT pipeline (any-hit alpha test + closest-hit albedo).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // textures_cube[] — point-light gobos + env maps; also sampled by the RT
    // pipeline's miss shader (env-light contribution).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTexturesCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // textures_2d_array[] — one Texture2DArray per animated image (frame = layer).
    // cTextureManager writes a slot's descriptor once at load (update-after-bind).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2DArray, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTexture2DArrayCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // gAnimTex — per-2D-array-slot animation record (frameCount/frameTime/mode),
    // read by the bindless sample helper. One bound buffer (contents written by
    // cTextureManager via the uploader; descriptor written once below).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingAnimTex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages,
        0});
    // (Former opaque*Handles bindings 3..8 removed — the per-stream BDAs now
    // live in UniformObject; those binding slots are unused/reserved.)
    // materialSampler — paired with textures_2d at every sample site.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingMaterialSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kRtSharedStages,
        0});
    // Slot-generation + light-grid SSBOs. Reachable from compute, ray-tracing,
    // and fragment stages (LightGridBuildPass writes the grid; the direct pass,
    // the path tracer's getCellLights and the object-slot consumers read).
    const uint32_t kGridSlotBindings[] = {
        kBindingBindlessSlotGeneration,
        kBindingLightGridCount,
        kBindingLightGridList,
    };
    const VkShaderStageFlags kGridSlotStageFlags =
        kSharedStages | (device->rayTracingPipelineEnabled ? kRtStages : 0);
    for (uint32_t b : kGridSlotBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGridSlotStageFlags, 0});
    }
    // Scene-object + flat material table. Read by the gbuffer, the composite,
    // and the RT pipeline's hit shaders.
    const uint32_t kSceneTableBindings[] = {
        kBindingSceneObjects,
        kBindingMaterials,
    };
    for (uint32_t b : kSceneTableBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    }
    // Point/spot/area lights and fog areas no longer live on set 0. They are
    // cWorld-owned persistent per-world buffers bound on the dedicated per-world
    // set kWorldSet (program-managed + cached) by every pass that reads them —
    // see appendWorldLightFog in HybridRenderer.cpp. set 0 stays pure engine state.

    // core_dissolve noise — one immutable sampled image on set 0, written once
    // at init. UV-sampled by the shared SceneMaterials.alphaTest in every
    // alpha-testing context (gbuffer FS, V-buffer anyhit, shadow and
    // reflection RayQuery loops) — the legacy solid_z dissolve fade.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingDissolveMap, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
        kSharedStages | (device->rayTracingPipelineEnabled ? kRtStages : 0),
        0});

    VkDescriptorPoolSize poolSizes[3] = {};
    // Sampled-image budget covers textures_2d[] + textures_cube[] +
    // textures_2d_array[] + the dissolve noise map.
    poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        kTextureSlotCapacity * 2 +
                                            kTexture2DArrayCapacity + 2};
    // Storage-buffer pool budget: 3 slot-generation / light-grid bindings
    // (kGridSlotBindings) + 2 scene/material + 1 animTex = 6 actually bound.
    // The per-world light/fog SSBOs are no longer here (they ride kWorldSet).
    // 16 keeps slack for future set-0 SSBOs.
    poolSizes[1] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    // One sampler: gMaterialSampler.
    poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};

    m_bindlessSet.initialize(device, bindings, poolSizes);
  }

  const VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  m_objectBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(UniformObject), kStorage,
      /*deviceLocalOnly*/ true);
  // (The six per-stream BDA handle buffers are gone — their addresses now ride
  // the UniformObject in m_objectBuffer.)

  // Per-2D-array-slot animation record table (gAnimTex). Contents written by
  // cTextureManager (uploader copies) when an animated image is assigned its
  // slot, so it must be device-local with TRANSFER_DST. Built through
  // CreateBindlessSlotBuffer like every other set-0 buffer — `kStorage` is raw
  // VkBufferUsageFlags, which that helper feeds straight into VkBufferCreateInfo.
  // (RIBuffer::create instead expects RI_BUFFER_USAGE_* flags, so passing kStorage
  // there mistranslated to STORAGE|INDIRECT and dropped TRANSFER_DST.)
  m_animTexBuffer = detail::CreateBindlessSlotBuffer(
      device, kTexture2DArrayCapacity, sizeof(AnimTexRec), kStorage,
      /*deviceLocalOnly*/ true);

  // Coarse world-space light grid. GPU-only: zeroed each frame via
  // vkCmdFillBuffer and (re)filled by LightGridBuildPass before the ray trace,
  // so no host seeding / mapping is needed (deviceLocalOnly).
  m_lightGridCountBuffer = detail::CreateBindlessSlotBuffer(
      device, kLightGridCellCount, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  m_lightGridListBuffer = detail::CreateBindlessSlotBuffer(
      device, kLightGridCellCount * kLightsPerCellMax, sizeof(uint32_t),
      kStorage, /*deviceLocalOnly*/ true);

  // Slot-reuse generation buffer (see m_bindlessSlotGenerationBuffer). Starts
  // at 0; the host bumps a slot's generation to a unique nonzero value the
  // first time it's assigned (in Draw), so 0 can never alias a live slot.
  m_bindlessSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  // m_bindlessSlotGenerationBuffer is device-local (no mappedAddress); the CPU
  // shadow mirror is zero-initialized by init() below and markAllDirty() forces
  // the first frame's flushMirrors() to seed the device buffer to zero before
  // any consumer reads it.

  // Shadow mirror for the device-local slot-generation buffer. Host writes
  // target this (never GPU-mapped memory); flushMirrors() stages the dirty range
  // once per frame. markAllDirty() seeds the device buffer from the zeroed shadow
  // on the first frame, giving a clean device == mirror invariant for every slot.
  // (Per-stream BDA handles now ride the UniformObject upload — no mirrors.)
  m_bindlessSlotGenerationMirror.init(kObjectSlotCapacity, sizeof(uint32_t));
  m_bindlessSlotGenerationMirror.markAllDirty();

  // Light SSBOs (point/spot/box) + fog/decal/object-decal-index. Unlike the
  // bindless slot buffers above these are device-local — the per-frame fill in
  // Draw() stages through Interface<cGraphics>::Get()->uploader rather than memcpy'ing into mapped memory
  // the GPU may still be reading from a prior frame.
  // Point/spot/area light buffers and the fog buffer are no longer owned here —
  // cWorld owns them as persistent per-world buffers on the per-world set
  // kWorldSet, bound by each consuming pass.

  // One flat material table (Falcor MaterialSystem model): a fixed-size
  // MaterialDataBlob per slot, indexed by a flat materialID. submitMaterial packs
  // the typed material struct into a blob (type tag in the header) and uploads it;
  // the shader reinterprets the blob into the concrete struct. One LRU pool.
  m_materialBindless.reset(kMaterialCapacity);
  m_materialBuffer = detail::CreateBindlessSlotBuffer(
      device, kMaterialCapacity, sizeof(MaterialDataBlob), kStorage,
      /*deviceLocalOnly*/ true);

  // Default linear/wrap sampler for all bindless texture fetches. The
  // engine's filter cache (cGraphics::resolve_filter_descriptor) hands
  // back a finalized RIDescriptor with a non-zero cookie, which is
  // exactly what bindDescriptors needs.
  m_materialSampler = pGraphics->resolve_filter_descriptor(
      eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
      eTextureFilter_Trilinear);

  {
    const struct {
      uint32_t binding;
      RIBuffer *buffer;
      VkDeviceSize range;
    } ssbos[] = {
        {kBindingBindlessSlotGeneration, &m_bindlessSlotGenerationBuffer,
         kObjectSlotCapacity * sizeof(uint32_t)},
        {kBindingLightGridCount, &m_lightGridCountBuffer,
         kLightGridCellCount * sizeof(uint32_t)},
        {kBindingLightGridList, &m_lightGridListBuffer,
         (size_t)kLightGridCellCount * kLightsPerCellMax * sizeof(uint32_t)},
        {kBindingSceneObjects, &m_objectBuffer,
         kObjectSlotCapacity * sizeof(UniformObject)},
        {kBindingMaterials, &m_materialBuffer,
         kMaterialCapacity * sizeof(MaterialDataBlob)},
        {kBindingAnimTex, &m_animTexBuffer,
         kTexture2DArrayCapacity * sizeof(AnimTexRec)},
        // The per-world light/fog SSBOs are not here — they ride kWorldSet, bound
        // per-pass from cWorld's persistent buffers.
    };

    RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 4] = {};
    size_t count = 0;
    for (uint32_t i = 0; i < std::size(ssbos); ++i) {
      writes[count].binding = ssbos[i].binding;
      writes[count].arrayElement = 0;
      writes[count].descriptor = RIDescriptor::storageBuffer(
          device, ssbos[i].buffer, 0, ssbos[i].range);
      count++;
    }
    writes[count].binding = kBindingMaterialSampler;
    writes[count].arrayElement = 0;
    writes[count].descriptor = *m_materialSampler;
    count++;
    // gDissolveMap — the legacy 128×128 dissolve noise (solid_z.frag.fsl),
    // bound once here on set 0. UV-sampled by the shared
    // SceneMaterials.alphaTest for the CoverageAmount fade.
    m_dissolveMap = resources->GetTextureManager()->Create2DImage(
        "core_dissolve.tga", false);
    if (auto disTex = m_dissolveMap ? m_dissolveMap->GetTexture() : nullptr) {
      writes[count].binding = kBindingDissolveMap;
      writes[count].arrayElement = 0;
      writes[count].descriptor = disTex->descriptor();
      count++;
    } else {
      Warning("Failed to load core_dissolve.tga; dissolve fade unbound\n");
    }
    m_bindlessSet.writeDescriptors(device, std::span(writes).subspan(0, count));
  }
}

GlobalManagedSets::MaterialSubmitResult
GlobalManagedSets::submitMaterial(cGraphics::FrameContext *cntx, cMaterial *mat,
                                  uint32_t frameIndex) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // cTextureManager stamps each Image with a lifetime-stable bindless slot at
  // load (binding 0 = textures_2d[], binding 1 = textures_cube[]); read it AND
  // pin the Image for this frame. The hybrid renderer references material
  // textures purely by slot (no per-draw descriptor binding), so without this pin
  // an entity destroyed mid-frame — e.g. a picked-up item in
  // UpdateToBeDestroyedEntities — would drop the Image's last ref and free its
  // image view (cTexture::~cTexture) while the GPU's bindless set still references
  // it (VUID-vkDestroyImageView-imageView-01026). The pin parks the Image in
  // graphicsDefer until the GPU passes this frame, then it frees safely.
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    Image *img = mat->GetImage(type);
    if (!img)
      return kInvalidTextureIndex;
    pGraphics->graphicsDefer.push(PinResource(img));
    return img->GetBindlessSlot();
  };

  // tex[] order must match the DiffuseMaterial struct in SceneTypes.slang.
  DiffuseMaterial gpu = {};
  gpu.type = MATERIAL_TYPE_DIFFUSE;
  gpu.tex[0] = slotFor(eMaterialTexture_Diffuse);
  gpu.tex[1] = slotFor(eMaterialTexture_NMap);
  gpu.tex[2] = slotFor(eMaterialTexture_Alpha);
  gpu.tex[3] = slotFor(eMaterialTexture_Specular);
  gpu.tex[4] = slotFor(eMaterialTexture_Height);
  gpu.tex[5] = slotFor(eMaterialTexture_Illumination);
  gpu.tex[6] = slotFor(eMaterialTexture_DissolveAlpha);
  gpu.tex[7] = slotFor(eMaterialTexture_CubeMapAlpha);
  // Reflection cube map — separate bindless table (textures_cube[]). A cube
  // Image's slot indexes textures_cube[] (cTextureManager assigned it from the
  // cube pool), so slotFor's GetBindlessSlot() read + per-frame pin works here
  // too; stored outside tex[].
  gpu.cubeMapTextureIndex = slotFor(eMaterialTexture_CubeMap);

  auto isSingleChannel = [](const Image *image) {
    if (!image) {
      return false;
    }
    const auto texture = image->GetTexture();
    return texture && RIFormatChannelCount(texture->format) == 1;
  };
  const Image *alphaImage = mat->GetImage(eMaterialTexture_Alpha);
  const Image *heightImage = mat->GetImage(eMaterialTexture_Height);
  gpu.materialConfig =
      (mat->GetImage(eMaterialTexture_Diffuse) ? kMaterialFlagEnableDiffuse
                                               : 0) |
      (mat->GetImage(eMaterialTexture_NMap) ? kMaterialFlagEnableNormal : 0) |
      (mat->GetImage(eMaterialTexture_Specular) ? kMaterialFlagEnableSpecular
                                                : 0) |
      (alphaImage ? kMaterialFlagEnableAlpha : 0) |
      (isSingleChannel(alphaImage) ? kMaterialFlagIsAlphaSingleChannel : 0) |
      (heightImage ? kMaterialFlagEnableHeight : 0) |
      (isSingleChannel(heightImage) ? kMaterialFlagIsHeightMapSingleChannel
                                    : 0) |
      (mat->GetImage(eMaterialTexture_Illumination)
           ? kMaterialFlagEnableIllumination
           : 0) |
      (mat->GetImage(eMaterialTexture_CubeMap) ? kMaterialFlagEnableCubeMap
                                               : 0) |
      (mat->GetImage(eMaterialTexture_DissolveAlpha)
           ? kMaterialFlagEnableDissolveAlpha
           : 0) |
      (mat->GetImage(eMaterialTexture_CubeMapAlpha)
           ? kMaterialFlagEnableCubeMapAlpha
           : 0);

  // The typed material tables share the same leading layout
  // (type, materialConfig, tex[8]); only the trailing scalars differ. Copy the
  // already-resolved config + texture slots out of `gpu` so each branch below
  // only has to fill its own scalar tail.
  auto copyShared = [&](auto &dst) {
    dst.materialConfig = gpu.materialConfig;
    std::memcpy(dst.tex, gpu.tex, sizeof(gpu.tex));
  };
  // Dispatch on the per-type data blob: each alternative builds its typed GPU
  // material struct (folding its config flags + scalars into the shared `gpu`
  // payload), packs it into a fixed-size MaterialDataBlob (type tag in the
  // header), then uploads the blob to the single flat material table and returns
  // a flat materialID. The shader reinterprets the blob via its type tag.
  return std::visit(
      [&](const auto &data) -> MaterialSubmitResult {
        using T = std::decay_t<decltype(data)>;

        static_assert(
            sizeof(MaterialDataBlob) >= sizeof(DiffuseMaterial) &&
                sizeof(MaterialDataBlob) >= sizeof(TranslucentMaterial) &&
                sizeof(MaterialDataBlob) >= sizeof(WaterMaterial),
            "MaterialDataBlob must hold the largest typed material struct");
        MaterialDataBlob blob = {};

        if constexpr (std::is_same_v<T, MaterialTranslucent>) {
          gpu.materialConfig |=
              (data.m_refractionNormals ? kMaterialFlagUseRefractionNormals
                                        : 0) |
              (mat->HasRefraction() && data.m_refractionEdgeCheck
                   ? kMaterialFlagUseRefractionEdgeCheck
                   : 0) |
              (mat->HasRefraction() ? kMaterialFlagHasRefraction : 0) |
              (data.m_isAffectedByLightLevel ? kMaterialFlagAffectedByLightLevel
                                             : 0) |
              (data.m_diffuseIsMask ? kMaterialFlagDiffuseIsMask : 0) |
              (data.m_smoothHalo ? kMaterialFlagSmoothHalo : 0) |
              (data.m_litDiffuse ? kMaterialFlagLitDiffuse : 0);
          TranslucentMaterial trans = {};
          trans.type = MATERIAL_TYPE_TRANSLUCENT;
          copyShared(trans);
          trans.cubeMapTextureIndex = gpu.cubeMapTextureIndex;
          trans.refractionScale = data.m_refractionScale;
          trans.frenselBias = data.m_frenselBias;
          trans.frenselPow = data.m_frenselPow;
          trans.rimLightMul = data.m_rimLightMul;
          trans.rimLightPow = data.m_rimLightPow;
          trans.particleOpacityScale = data.m_particleOpacityScale;
          trans.particleBrightnessScale = data.m_particleBrightnessScale;
          trans.litDiffuseScale = data.m_litDiffuseScale;
          std::memcpy(blob.data, &trans, sizeof(trans));
        } else if constexpr (std::is_same_v<T, MaterialWater>) {
          // Water is flagged as always refracting + reflecting. Reflection is
          // real (Water.frag traces it with an inline RayQuery); refraction is
          // not — no refraction pass runs, so the background under the water
          // surface is the unrefracted composite. The HasRefraction flag is
          // kept because the CPU side reads it (e.g. TLAS instance gathering in
          // cWorld) and it stays correct once a refraction pass lands.
          gpu.materialConfig |=
              kMaterialFlagIsWater | kMaterialFlagHasRefraction;
          WaterMaterial water = {};
          water.type = MATERIAL_TYPE_WATER;
          copyShared(water);
          water.refractionScale = data.m_refractionScale;
          water.frenselBias = data.m_frenselBias;
          water.frenselPow = data.m_frenselPow;
          water.reflectionFadeStart = data.m_reflectionFadeStart;
          water.reflectionFadeEnd = data.m_reflectionFadeEnd;
          water.waveSpeed = data.m_waveSpeed;
          water.waveAmplitude = data.m_waveAmplitude;
          water.waveFreq = data.m_waveFreq;
          std::memcpy(blob.data, &water, sizeof(water));
        } else {
          // SolidDiffuse, Decal and the blank/unknown (monostate) material all
          // use the DiffuseMaterial layout.
          if constexpr (std::is_same_v<T, MaterialDiffuseSolid>) {
            gpu.materialConfig |=
                (data.m_alphaDissolveFilter ? kMaterialFlagUseDissolveFilter
                                            : 0);
            gpu.heightMapScale = data.m_heightMapScale;
            gpu.heightMapBias = data.m_heightMapBias;
            gpu.frenselBias = data.m_frenselBias;
            gpu.frenselPow = data.m_frenselPow;
          }
          std::memcpy(blob.data, &gpu, sizeof(gpu));
        }

        // One flat table, keyed by a cookie over the packed blob (folds in the
        // material generation so edits re-upload). Allocate a slot on first
        // sight; re-upload only when the blob changed.
        hash_t cookie = hash_u64(HASH_INITIAL_VALUE, mat->GetUniqueCookie());
        cookie = hash_u64(cookie, (uint64_t)mat->Generation());
        cookie = hash_data(cookie, &blob, sizeof(blob));

        auto req = m_materialBindless.request(cookie, frameIndex);
        if (req.exhausted)
          return {UINT32_MAX};
        if (req.found)
          return {req.id};

        RIResourceBufferTransaction trans = {};
        trans.target = m_materialBuffer;
        trans.size = sizeof(MaterialDataBlob);
        trans.offset = (size_t)req.id * sizeof(MaterialDataBlob);
        trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
        trans.currentStages = RI_STAGE_ALL_SHADER;
        trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
        trans.postStages = RI_STAGE_ALL_SHADER;
        RI_ResourceBeginCopyBuffer(&pGraphics->device, &pGraphics->uploader,
                                   &trans);
        std::memcpy(trans.mapped.data, &blob, sizeof(blob));
        RI_ResourceEndCopyBuffer(&pGraphics->device, &pGraphics->uploader,
                                 &trans);
        return {req.id};
      },
      mat->Data());
}

uint32_t GlobalManagedSets::submitObject(uint64_t objectCookie,
                                         uint32_t frameIndex, cVertexBuffer *vb,
                                         const ObjectSubmitDesc &desc,
                                         uint32_t flags) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // Stable slot per object: keyed on the renderable's unique cookie only, so a
  // moving object keeps its slot (the per-frame modelMat upload below carries
  // the movement). frameInFlight = 0, so a slot is only unavailable if every
  // slot is already taken this frame.
  auto req = m_objectSlots.request(objectCookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  const uint32_t slot = req.id;

  // Generation bump triggers (invalidate anything holding a cached reference to
  // this object slot): a fresh occupant (!found) or a topology change. The only
  // hard fault is primitiveIndex >= triangleCount, so a change in the VB's index
  // count is the one geometry edit that must invalidate; a same-count realloc
  // just refreshes the BDA (every frame) and stays valid.
  const uint32_t indexCount = vb ? (uint32_t)vb->GetIndexNum() : 0u;
  if (!req.found && vb) {
    // Bind the VB-destroy hook on a fresh slot: when the geometry is freed, bump
    // this slot's generation so any cached reference to it goes stale before it
    // dereferences the freed vertex/index BDA. Writes the CPU shadow only (may
    // fire off-frame during teardown); flushMirrors() stages it next frame.
    req.state->onDestroy = EventHandler<>([this, slot]() {
      m_bindlessSlotGenerationMirror.write<uint32_t>(slot,
                                                     ++m_nextSlotGeneration);
    });
    req.state->onDestroy.Connect(vb->OnDestroyed());
  }
  const bool bumpGeneration = !req.found || req.state->indexCount != indexCount;
  req.state->indexCount = indexCount;
  if (bumpGeneration)
    m_bindlessSlotGenerationMirror.write<uint32_t>(slot,
                                                   ++m_nextSlotGeneration);

  // kSubmitData: build the GPU payload from the descriptor (transpose the model
  // matrix to the GPU's float4x4, derive invModelMat, transpose the uv matrix,
  // copy scalars) and stage it into m_objectBuffer[slot].
  if (flags & kSubmitData) {
    UniformObject payload{};
    payload.dissolveAmount = desc.dissolveAmount;
    payload.materialID = desc.materialId;
    payload.illuminationAmount = desc.illuminationAmount;
    payload.decalList = desc.decalList;
    payload.renderFlags = desc.renderFlags;
    const ml::float4x4 modelF4 = cMath::ToFloatTranspose4x4(
        desc.modelMatrix ? *desc.modelMatrix : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    // prevModelMat for motion vectors. Rotate prev<-cur ONLY ONCE per frame per
    // slot (keyed on frameIndex): submitObject may run multiple times per frame for
    // the same object (cWorld::PrepareFrame's whole-scene submit + the renderer's
    // raster loop). Every call must publish the SAME prev (last frame's matrix);
    // rotating on each call would set prev==cur on the 2nd call and zero the
    // object's velocity → temporal smear on animated/physics objects. New occupant
    // (!found): prev = cur so the first frame reads zero velocity, not a teleport.
    if (!req.found) {
      std::memcpy(req.state->prevModelMat, modelF4.a,
                  sizeof(req.state->prevModelMat));
      std::memcpy(req.state->curModelMat, modelF4.a,
                  sizeof(req.state->curModelMat));
    } else if (req.state->lastSubmitFrame != frameIndex) {
      std::memcpy(req.state->prevModelMat, req.state->curModelMat,
                  sizeof(req.state->prevModelMat));
      std::memcpy(req.state->curModelMat, modelF4.a,
                  sizeof(req.state->curModelMat));
    }
    req.state->lastSubmitFrame = frameIndex;
    std::memcpy(payload.prevModelMat, req.state->prevModelMat,
                sizeof(payload.prevModelMat));
    const ml::float4x4 uvF4 = cMath::ToFloatTranspose4x4(desc.uvMatrix);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    // Per-stream vertex/index BDAs, folded into the UniformObject (were the six
    // gOpaque*Handles buffers). Priority: explicit override (particles → scratch
    // ring) > derive from vb (vertex-pull passes; absent stream/flag → 0) > carry
    // forward the slot's existing handles (data-only passes that bind raw
    // VkBuffers — don't zero a slot a handle-reading pass populated). Rewritten
    // every frame so a SubmitToGPU realloc can't dangle them; the memcmp-skip
    // below keeps a stable-source object's upload skipped after frame 0.
    if (desc.streamHandles.set) {
      payload.posHandle = desc.streamHandles.pos;
      payload.normalHandle = desc.streamHandles.normal;
      payload.tangentHandle = desc.streamHandles.tangent;
      payload.uv0Handle = desc.streamHandles.uv0;
      payload.colorHandle = desc.streamHandles.color;
      payload.indexHandle = desc.streamHandles.index;
    } else if (vb && (flags & (kSubmitVertex | kSubmitIndex))) {
      auto bdaOf = [&](eVertexBufferElement type) -> uint64_t {
        const auto *element = vb->GetElement(type);
        RIBuffer *buf = element ? element->GetBuffer() : nullptr;
        return buf ? buf->GetDeviceHandle(&pGraphics->device) : 0;
      };
      if (flags & kSubmitVertex) {
        payload.posHandle = bdaOf(eVertexBufferElement_Position);
        payload.normalHandle = bdaOf(eVertexBufferElement_Normal);
        payload.tangentHandle = bdaOf(eVertexBufferElement_Texture1Tangent);
        payload.colorHandle = bdaOf(eVertexBufferElement_Color0);
        payload.uv0Handle = bdaOf(eVertexBufferElement_Texture0);
      }
      if (flags & kSubmitIndex)
        payload.indexHandle =
            vb->GetIndexRIBuffer()
                ? vb->GetIndexRIBuffer()->GetDeviceHandle(&pGraphics->device)
                : 0;
    } else if (req.found) {
      payload.posHandle = req.state->lastPayload.posHandle;
      payload.normalHandle = req.state->lastPayload.normalHandle;
      payload.tangentHandle = req.state->lastPayload.tangentHandle;
      payload.uv0Handle = req.state->lastPayload.uv0Handle;
      payload.colorHandle = req.state->lastPayload.colorHandle;
      payload.indexHandle = req.state->lastPayload.indexHandle;
    }

    // Permissive upload: a new occupant (!found, so lastPayload still belongs to
    // the prior object — guard with req.found) or any field change re-stages; an
    // unchanged static object skips the uploader entirely. m_objectBuffer is a
    // single persistent device buffer, so the slot keeps last frame's value when
    // skipped.
    const bool payloadChanged =
        !req.found ||
        std::memcmp(&payload, &req.state->lastPayload, sizeof(payload)) != 0;
    if (payloadChanged) {
      RIResourceBufferTransaction trans = {};
      trans.target = m_objectBuffer;
      trans.size = sizeof(UniformObject);
      trans.offset = (size_t)slot * sizeof(UniformObject);
      trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.currentStages = RI_STAGE_ALL_SHADER;
      trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.postStages = RI_STAGE_ALL_SHADER;
      RI_ResourceBeginCopyBuffer(&pGraphics->device, &pGraphics->uploader,
                                 &trans);
      std::memcpy(trans.mapped.data, &payload, sizeof(payload));
      RI_ResourceEndCopyBuffer(&pGraphics->device, &pGraphics->uploader,
                               &trans);
      req.state->lastPayload = payload;
    }
  }

  // (The per-stream BDA handles are now part of the UniformObject payload built
  // above, staged in the single m_objectBuffer[slot] copy — no separate buffers.)
  return slot;
}

void GlobalManagedSets::flushMirrors(RIDevice *device) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  struct Item {
    RIBuffer *buf;
    BindlessShadowMirror *mir;
  };
  const Item items[] = {
      {&m_bindlessSlotGenerationBuffer, &m_bindlessSlotGenerationMirror},
  };
  for (const auto &it : items) {
    if (!it.mir->hasDirty())
      continue;
    const size_t off = it.mir->dirtyMinByte;
    const size_t sz = it.mir->dirtyMaxByte - off;
    RIResourceBufferTransaction trans = {};
    trans.target = *it.buf;
    trans.size = sz;
    trans.offset = off;
    // Read as storage buffers by the gbuffer VS (vertex pull) and the
    // ray-trace chit+ahit stages. The WAR
    // pre-barrier (currentState/currentStages) waits on the prior frame's reads before
    // the staged copy overwrites the slot; this is the m_objectBuffer path.
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_ALL_SHADER;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_ALL_SHADER;
    RI_ResourceBeginCopyBuffer(device, &pGraphics->uploader, &trans);
    std::memcpy(trans.mapped.data, it.mir->shadow.data() + off, sz);
    RI_ResourceEndCopyBuffer(device, &pGraphics->uploader, &trans);
    it.mir->clearDirty();
  }
}

void GlobalManagedSets::destroy(RIDevice *device) {
  // Everything initialize() created — a missed entry here trips the VMA
  // leak assert in vmaDestroyAllocator at device teardown.
  RIBuffer *ownedBuffers[] = {
      &m_objectBuffer,         &m_bindlessSlotGenerationBuffer,
      &m_lightGridCountBuffer, &m_lightGridListBuffer,
      &m_materialBuffer,       &m_animTexBuffer,
  };
  // Point/spot/area light buffers + the fog buffer are owned + disposed by cWorld.
  for (RIBuffer *buf : ownedBuffers) {
    buf->dispose(device);
    *buf = {};
  }
  m_bindlessSet.destroy(device);
}

// === Engine-lifetime set, owned by cGraphics (the globalset member) ===
// One global set 0 for the whole engine. Heap-owned through the RI global so
// there is a single entry point; a pointer (not a value member on cGraphics)
// keeps GlobalManagedSets.h's include of Graphics.h cycle-free.
void InitGlobalManagedSets(RIDevice *device, cResources *resources) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // Runs in cGraphics::Init before any managed texture is created, so textures
  // write their descriptors directly at load (no catch-up pass needed).
  pGraphics->globalset = new GlobalManagedSets();
  pGraphics->globalset->initialize(device, resources);
}

void ShutdownGlobalManagedSets(RIDevice *device) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  if (pGraphics->globalset) {
    pGraphics->globalset->destroy(device);
    delete pGraphics->globalset;
    pGraphics->globalset = nullptr;
  }
}

} // namespace hpl
