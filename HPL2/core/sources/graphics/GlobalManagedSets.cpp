#include "graphics/GlobalManagedSets.h"

#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/Graphics.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "math/Math.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/LowLevelSystem.h"

#include <cstring>
#include <iterator>
#include <limits>
#include <span>

namespace hpl {

// Set-0 structured buffers are viewed with their element size as the D3D12
// StructureByteStride, which must be a non-zero multiple of 4 and at most
// 2048 bytes.
static_assert(sizeof(UniformObject) % 4 == 0 && sizeof(UniformObject) <= 2048);
static_assert(sizeof(MaterialDataBlob) % 4 == 0 &&
              sizeof(MaterialDataBlob) <= 2048);
static_assert(sizeof(AnimTexRec) % 4 == 0 && sizeof(AnimTexRec) <= 2048);

namespace {

bool resolveExplicitStreamRef(RIDevice *device,
                              const ObjectSubmitDesc::StreamRefs::Ref &ref,
                              uint64_t &outHandle) {
  outHandle = 0;
  if (!ref.buffer)
    return ref.byteOffset == 0;
  if (ref.byteOffset > std::numeric_limits<uint32_t>::max() ||
      (ref.byteOffset & 3u) != 0 || ref.buffer->isEmpty())
    return false;

  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
#if (DEVICE_IMPL_VULKAN)
    const uint64_t base = ref.buffer->GetDeviceHandle(device);
    if (base == 0 ||
        base > std::numeric_limits<uint64_t>::max() - ref.byteOffset)
      return false;
    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device->vk.device, ref.buffer->vk.buffer,
                                  &requirements);
    if (requirements.size < sizeof(uint32_t) ||
        ref.byteOffset > requirements.size - sizeof(uint32_t))
      return false;
    outHandle = base + ref.byteOffset;
    return true;
#else
    Error("Vulkan implementation not available");
#endif
  }
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
#if (DEVICE_IMPL_D3D12)
    const uint64_t base = ref.buffer->GetShaderResourceHandle(device);
    if (base == 0 || (base >> 32) == 0 ||
        ref.buffer->d3d12.requestedSize < sizeof(uint32_t) ||
        ref.byteOffset > ref.buffer->d3d12.requestedSize - sizeof(uint32_t))
      return false;
    outHandle = base | uint64_t(static_cast<uint32_t>(ref.byteOffset));
    return true;
#else
    Error("D3D12 implementation not available");
#endif
  }
  return false;
}

} // namespace

GlobalManagedSets::GlobalManagedSets()
    : m_objectSlots(kObjectSlotCapacity, /*frameInFlight*/ 0),
      m_materialBindless(kMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT) {}

// Out-of-line so the SharedResourceHandle<Image> members release here, where
// Image is complete.
GlobalManagedSets::~GlobalManagedSets() = default;

bool GlobalManagedSets::initialize(RIDevice *device, cResources *resources) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  {
    std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
    // RT stages (raygen/any-hit/closest-hit/miss) all need bindless
    // texture + vertex access for alpha test, camera rays and material shading.
    const VkShaderStageFlags kSharedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                             VK_SHADER_STAGE_FRAGMENT_BIT |
                                             VK_SHADER_STAGE_COMPUTE_BIT;
    const VkShaderStageFlags kRtStages =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    const VkShaderStageFlags kRtSharedStages =
        kSharedStages | (device->rayTracingPipelineEnabled ? kRtStages : 0);
    // textures_2d[] — gbuffer, composite, RT any-hit alpha test + closest-hit
    // albedo.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // textures_cube[] — point-light gobos + env maps (also RT miss shader).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTexturesCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // textures_2d_array[] — one Texture2DArray per animated image (frame =
    // layer); cTextureManager writes each slot's descriptor once at load.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2DArray, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTexture2DArrayCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // gAnimTex — per-2D-array-slot animation record (frameCount/frameTime/mode)
    // read by the bindless sample helper.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingAnimTex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages,
        0});
    // Bindings 3..8 are unused/reserved (per-stream BDAs live in UniformObject).
    // materialSampler — paired with textures_2d at every sample site.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingMaterialSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kRtSharedStages,
        0});
    // Slot-generation + light-grid SSBOs. LightGridBuildPass writes the grid;
    // the direct pass, path tracer and object-slot consumers read.
    const uint32_t kGridSlotBindings[] = {
        kBindingBindlessSlotGeneration,
        kBindingLightGridCount,
        kBindingLightGridList,
        kBindingLightGridWeight,
    };
    const VkShaderStageFlags kGridSlotStageFlags =
        kSharedStages | (device->rayTracingPipelineEnabled ? kRtStages : 0);
    for (uint32_t b : kGridSlotBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGridSlotStageFlags, 0});
    }
    // Scene-object + flat material table.
    const uint32_t kSceneTableBindings[] = {
        kBindingSceneObjects,
        kBindingMaterials,
    };
    for (uint32_t b : kSceneTableBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    }
    // Lights and fog live on kWorldSet, not here (see appendWorldLightFog in
    // HybridRenderer.cpp); set 0 is engine state only.

    // core_dissolve noise — immutable image written once at init, sampled by
    // SceneMaterials.alphaTest in every alpha-testing context.
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
    // Storage-buffer budget: 4 grid/slot-generation + 2 scene/material + 1
    // animTex = 7 bound; 16 leaves slack for future set-0 SSBOs.
    poolSizes[1] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    // One sampler: gMaterialSampler.
    poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};

    if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
#if (DEVICE_IMPL_D3D12)
      std::vector<RIBindlessDescriptorSet::BackendBinding> d3d12Bindings;
      d3d12Bindings.reserve(bindings.size() + 1);
      for (const auto &binding : bindings) {
        RIBindlessDescriptorSet::BackendBinding converted{};
        if (!RIBindlessDescriptorSet::convertBinding(binding, converted))
          return false;
        // Production bindless resources each occupy a dedicated D3D12
        // register space. This lets RIProgram expose every unbounded array as
        // its own root table while the Vulkan set/binding contract stays
        // unchanged. Keep this formula in sync with bindless.slang.
        converted.registerIndex = 0;
        converted.registerSpace = 32u + binding.binding;
        // Vulkan storage buffers cover both read-only and read/write access.
        // DXIL reflects StructuredBuffer as an SRV and RWStructuredBuffer as
        // a UAV, so preserve that distinction in the external-table contract.
        if (binding.binding == kBindingAnimTex ||
            binding.binding == kBindingMaterials)
          converted.registerClass = RIBindlessRegisterClass::SRV;
        if (binding.binding == kBindingTexturesCube)
          converted.srvDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        else if (binding.binding == kBindingTextures2DArray)
          converted.srvDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d3d12Bindings.push_back(converted);
      }
      // This is metadata for the program's dedicated geometry table.  The
      // descriptor count is intentionally skipped by the D3D12 allocator.
      d3d12Bindings.push_back(
          {4, RIBindlessRegisterClass::SRV, UINT_MAX, 0, 36, 0,
           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, D3D12_SRV_DIMENSION_BUFFER,
           D3D12_UAV_DIMENSION_BUFFER, DXGI_FORMAT_R32_TYPELESS, true});
      RIBindlessD3D12Layout layout{};
      layout.geometryRangeCount = UINT_MAX;
      layout.geometryRangeOffset = 0;
      if (!m_bindlessSet.initialize(device, d3d12Bindings, layout))
        return false;
#else
      Error("D3D12 implementation not available");
      return false;
#endif
    } else {
#if (DEVICE_IMPL_VULKAN)
      m_bindlessSet.initialize(device, bindings, poolSizes);
#else
      Error("Vulkan implementation not available");
      return false;
#endif
    }
  }

  const uint32_t kStorage = RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                            RI_BUFFER_USAGE_TRANSFER_DST |
                            RI_BUFFER_USAGE_TRANSFER_SRC;
  m_objectBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(UniformObject), kStorage,
      /*deviceLocalOnly*/ true);

  // Per-2D-array-slot animation records (gAnimTex). cTextureManager fills them
  // via uploader copies when an animated image gets its slot.
  m_animTexBuffer = detail::CreateBindlessSlotBuffer(
      device, kTexture2DArrayCapacity, sizeof(AnimTexRec), kStorage,
      /*deviceLocalOnly*/ true);

  // Coarse world-space light grid. GPU-only: zeroed each frame and refilled by
  // LightGridBuildPass before the ray trace.
  m_lightGridCountBuffer = detail::CreateBindlessSlotBuffer(
      device, kLightGridCellCount, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  const uint64_t lightGridListSlots =
      uint64_t(kLightGridCellCount) * uint64_t(kLightsPerCellMax);
  m_lightGridListBuffer = detail::CreateBindlessSlotBuffer(
      device, lightGridListSlots, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  // Per-cell sampling CDF, parallel to the list, so single-sample NEE draws
  // lights proportional to contribution. Written by binLights with the ids.
  m_lightGridWeightBuffer = detail::CreateBindlessSlotBuffer(
      device, lightGridListSlots, sizeof(float), kStorage,
      /*deviceLocalOnly*/ true);

  // Slot-reuse generation buffer. Starts at 0; the host bumps a slot's
  // generation to a unique nonzero value on first assignment, so 0 never
  // aliases a live slot.
  m_bindlessSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);

  // CPU shadow of the device-local generation buffer. Host writes land here;
  // flushMirrors() stages the dirty range once per frame. markAllDirty() makes
  // the first flush seed the device buffer to zero before any consumer reads it.
  m_bindlessSlotGenerationMirror.init(kObjectSlotCapacity, sizeof(uint32_t));
  m_bindlessSlotGenerationMirror.markAllDirty();

  // One flat material table (Falcor MaterialSystem model): a fixed-size
  // MaterialDataBlob per slot, indexed by materialID. One LRU pool.
  m_materialBindless.reset(kMaterialCapacity);
  m_materialBuffer = detail::CreateBindlessSlotBuffer(
      device, kMaterialCapacity, sizeof(MaterialDataBlob), kStorage,
      /*deviceLocalOnly*/ true);

  // Default trilinear/wrap sampler for all bindless texture fetches.
  m_materialSampler = pGraphics->resolve_filter_descriptor(
      eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
      eTextureFilter_Trilinear);

  {
    // Views carry the element stride: D3D12 needs it to build a structured
    // SRV/UAV (stride-less becomes RAW and mismatches bindless.slang). Vulkan
    // ignores it.
    const struct {
      uint32_t binding;
      RIBuffer *buffer;
      VkDeviceSize range;
      uint32_t stride;
    } ssbos[] = {
        {kBindingBindlessSlotGeneration, &m_bindlessSlotGenerationBuffer,
         kObjectSlotCapacity * sizeof(uint32_t), sizeof(uint32_t)},
        {kBindingLightGridCount, &m_lightGridCountBuffer,
         kLightGridCellCount * sizeof(uint32_t), sizeof(uint32_t)},
        {kBindingLightGridList, &m_lightGridListBuffer,
         uint64_t(kLightGridCellCount) * uint64_t(kLightsPerCellMax) *
             sizeof(uint32_t),
         sizeof(uint32_t)},
        {kBindingLightGridWeight, &m_lightGridWeightBuffer,
         uint64_t(kLightGridCellCount) * uint64_t(kLightsPerCellMax) *
             sizeof(float),
         sizeof(float)},
        {kBindingSceneObjects, &m_objectBuffer,
         kObjectSlotCapacity * sizeof(UniformObject), sizeof(UniformObject)},
        {kBindingMaterials, &m_materialBuffer,
         kMaterialCapacity * sizeof(MaterialDataBlob),
         sizeof(MaterialDataBlob)},
        {kBindingAnimTex, &m_animTexBuffer,
         kTexture2DArrayCapacity * sizeof(AnimTexRec), sizeof(AnimTexRec)},
    };

    RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 4] = {};
    size_t count = 0;
    for (uint32_t i = 0; i < std::size(ssbos); ++i) {
      writes[count].binding = ssbos[i].binding;
      writes[count].arrayElement = 0;
      writes[count].descriptor = RIDescriptor::storageBuffer(
          device, ssbos[i].buffer, 0, ssbos[i].range, ssbos[i].stride,
          /*raw*/ false, /*structured*/ true);
      count++;
    }
    writes[count].binding = kBindingMaterialSampler;
    writes[count].arrayElement = 0;
    writes[count].descriptor = *m_materialSampler;
    count++;
    // gDissolveMap — legacy 128×128 dissolve noise (solid_z.frag.fsl).
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
    // One write per call: the D3D12 writer is all-or-nothing per batch, so one
    // rejected descriptor must not null every other slot.
    for (size_t i = 0; i < count; ++i) {
      if (!m_bindlessSet.writeDescriptors(device, {&writes[i], 1}))
        Warning(
            "GlobalManagedSets: set-0 descriptor write failed (binding %u)\n",
            writes[i].binding);
    }
  }

  const RIBuffer *ownedBuffers[] = {
      &m_objectBuffer,          &m_bindlessSlotGenerationBuffer,
      &m_lightGridCountBuffer,  &m_lightGridListBuffer,
      &m_lightGridWeightBuffer, &m_materialBuffer,
      &m_animTexBuffer};
  for (const RIBuffer *buffer : ownedBuffers) {
    if (buffer->isEmpty()) {
      Warning("GlobalManagedSets initialization failed: scene buffer creation "
              "failed\n");
      destroy(device);
      return false;
    }
  }
  return true;
}

GlobalManagedSets::MaterialSubmitResult
GlobalManagedSets::submitMaterial(cGraphics::FrameContext *cntx, cMaterial *mat,
                                  uint32_t frameIndex) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // Read the Image's bindless slot (stamped by cTextureManager at load) and pin
  // the Image for this frame. Materials reference textures purely by slot, so an
  // entity destroyed mid-frame would otherwise free the image view while the
  // GPU's bindless set still references it (VUID-vkDestroyImageView-imageView-01026).
  // The pin parks the Image in graphicsDefer until the GPU passes this frame.
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
  // Reflection cube map indexes textures_cube[], so it is stored outside tex[].
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

  // Each alternative builds its typed GPU struct and packs it into a fixed-size
  // MaterialDataBlob (type tag in the header); the blob is uploaded to the flat
  // material table and the shader reinterprets it via the tag.
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
              (data.m_litDiffuse ? kMaterialFlagLitDiffuse : 0) |
              ((static_cast<uint32_t>(mat->GetBlendMode()) &
                kMaterialBlendModeMask)
               << kMaterialBlendModeShift);
          TranslucentMaterial trans = {};
          trans.type = MATERIAL_TYPE_TRANSLUCENT;
          // Typed tables share the leading layout (type, materialConfig,
          // tex[8]); copy the resolved config + texture slots from `gpu`.
          trans.materialConfig = gpu.materialConfig;
          std::memcpy(trans.tex, gpu.tex, sizeof(gpu.tex));
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
          // Water always refracts in the raster pass. HasRefraction also
          // admits it into the TLAS for secondary path-traced transmission.
          gpu.materialConfig |=
              kMaterialFlagIsWater | kMaterialFlagHasRefraction;
          WaterMaterial water = {};
          water.type = MATERIAL_TYPE_WATER;
          water.materialConfig = gpu.materialConfig;
          std::memcpy(water.tex, gpu.tex, sizeof(gpu.tex));
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

        // Keyed by a cookie over the blob plus material generation, so edits
        // re-upload; an unchanged blob reuses its slot.
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
#if (DEVICE_IMPL_D3D12)
  if (desc.streamHandles.set && RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return UINT32_MAX;
#endif
  uint64_t explicitHandles[6] = {};
  if (desc.streamRefs.set) {
    const ObjectSubmitDesc::StreamRefs::Ref refs[6] = {
        desc.streamRefs.pos, desc.streamRefs.normal, desc.streamRefs.tangent,
        desc.streamRefs.uv0, desc.streamRefs.color,  desc.streamRefs.index};
    for (size_t i = 0; i < std::size(refs); ++i) {
      if (!resolveExplicitStreamRef(&pGraphics->device, refs[i],
                                    explicitHandles[i]))
        return UINT32_MAX;
    }
  }
  // Stable slot per object, keyed on the cookie only so a moving object keeps
  // its slot. Exhausted only if every slot is taken this frame.
  auto req = m_objectSlots.request(objectCookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  const uint32_t slot = req.id;

  // Bump the slot generation (invalidating cached references) on a fresh
  // occupant or an index-count change. Index count is the only geometry edit
  // that must invalidate (primitiveIndex >= triangleCount faults); a same-count
  // realloc just refreshes the BDA.
  const uint32_t indexCount = vb ? (uint32_t)vb->GetIndexNum() : 0u;
  if (!req.found && vb) {
    // On VB destroy, bump this slot's generation so cached references go stale
    // before dereferencing the freed BDA. Writes the CPU shadow only (may fire
    // off-frame); flushMirrors() stages it next frame.
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

  // kSubmitData: build the GPU payload from the descriptor and stage it into
  // m_objectBuffer[slot].
  if (flags & kSubmitData) {
    UniformObject payload{};
    payload.dissolveAmount = desc.dissolveAmount;
    payload.materialID = desc.materialId;
    payload.illuminationAmount = desc.illuminationAmount;
    payload.decalList = desc.decalList;
    payload.renderFlags = desc.renderFlags;
    payload.boundsCenterW =
        float4{desc.boundsCenter.x, desc.boundsCenter.y, desc.boundsCenter.z,
               desc.boundsCenterSet ? 1.0f : 0.0f};
    const ml::float4x4 modelF4 = cMath::ToFloatTranspose4x4(
        desc.modelMatrix ? *desc.modelMatrix : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    // prevModelMat for motion vectors. Rotate prev<-cur once per frame per slot
    // (keyed on frameIndex): submitObject can run several times per frame for
    // one object (cWorld::PrepareFrame's scene submit + the raster loop), and
    // rotating on each call would zero its velocity. A new occupant starts with
    // prev = cur so the first frame has no velocity.
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

    // Per-stream shader handles: Vulkan supplies BDAs, DXIL raw-SRV descriptor
    // indices. Priority: explicit override (particles → scratch ring) > derive
    // from vb (absent stream/flag → 0) > carry forward the slot's existing
    // handles (data-only passes must not zero what a handle-reading pass set).
    // Rewritten every frame so a SubmitToGPU realloc can't dangle them.
    if (desc.streamRefs.set) {
      payload.posHandle = explicitHandles[0];
      payload.normalHandle = explicitHandles[1];
      payload.tangentHandle = explicitHandles[2];
      payload.uv0Handle = explicitHandles[3];
      payload.colorHandle = explicitHandles[4];
      payload.indexHandle = explicitHandles[5];
#if (DEVICE_IMPL_VULKAN)
    } else if (desc.streamHandles.set && RIIsTargetSelected(RI_DEVICE_API_VK)) {
      payload.posHandle = desc.streamHandles.pos;
      payload.normalHandle = desc.streamHandles.normal;
      payload.tangentHandle = desc.streamHandles.tangent;
      payload.uv0Handle = desc.streamHandles.uv0;
      payload.colorHandle = desc.streamHandles.color;
      payload.indexHandle = desc.streamHandles.index;
#endif
    } else if (vb && (flags & (kSubmitVertex | kSubmitIndex))) {
      auto shaderHandleOf = [&](eVertexBufferElement type) -> uint64_t {
        const auto *element = vb->GetElement(type);
        RIBuffer *buf = element ? element->GetBuffer() : nullptr;
        // Vulkan returns the existing BDA. D3D12 returns the raw-SRV index
        // registered for ByteAddressBuffer geometry; never feed a D3D12 GPU
        // VA into the DXIL descriptor-index path.
        return buf ? buf->GetShaderResourceHandle(&pGraphics->device) : 0;
      };
      if (flags & kSubmitVertex) {
        payload.posHandle = shaderHandleOf(eVertexBufferElement_Position);
        payload.normalHandle = shaderHandleOf(eVertexBufferElement_Normal);
        payload.tangentHandle =
            shaderHandleOf(eVertexBufferElement_Texture1Tangent);
        payload.colorHandle = shaderHandleOf(eVertexBufferElement_Color0);
        payload.uv0Handle = shaderHandleOf(eVertexBufferElement_Texture0);
      }
      if (flags & kSubmitIndex)
        payload.indexHandle =
            vb->GetIndexRIBuffer()
                ? vb->GetIndexRIBuffer()->GetShaderResourceHandle(
                      &pGraphics->device)
                : 0;
    } else if (req.found) {
      payload.posHandle = req.state->lastPayload.posHandle;
      payload.normalHandle = req.state->lastPayload.normalHandle;
      payload.tangentHandle = req.state->lastPayload.tangentHandle;
      payload.uv0Handle = req.state->lastPayload.uv0Handle;
      payload.colorHandle = req.state->lastPayload.colorHandle;
      payload.indexHandle = req.state->lastPayload.indexHandle;
    }

    // Re-stage on a new occupant (!found: lastPayload belongs to the prior
    // object) or any field change; otherwise skip, since m_objectBuffer
    // persists last frame's value.
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
    // Read as storage by the gbuffer VS and RT chit+ahit; the WAR pre-barrier
    // (currentState/currentStages) waits on prior-frame reads before the copy.
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
      &m_objectBuffer,          &m_bindlessSlotGenerationBuffer,
      &m_lightGridCountBuffer,  &m_lightGridListBuffer,
      &m_lightGridWeightBuffer, &m_materialBuffer,
      &m_animTexBuffer,
  };
  for (RIBuffer *buf : ownedBuffers) {
    buf->dispose(device);
    *buf = {};
  }
  m_bindlessSet.destroy(device);
}

// === Engine-lifetime set, owned by cGraphics (the globalset member) ===
// One global set 0 for the whole engine. Held by pointer (not value) on
// cGraphics to keep GlobalManagedSets.h's include of Graphics.h cycle-free.
void InitGlobalManagedSets(RIDevice *device, cResources *resources) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // Runs in cGraphics::Init before any managed texture exists, so textures
  // write their descriptors directly at load.
  pGraphics->globalset = new GlobalManagedSets();
  if (!pGraphics->globalset->initialize(device, resources)) {
    delete pGraphics->globalset;
    pGraphics->globalset = nullptr;
    FatalError("Failed to initialize global managed scene buffers\n");
  }
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
