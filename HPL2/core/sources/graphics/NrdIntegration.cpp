#include "graphics/NrdIntegration.h"

#include "Constants.h"
#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIDescriptor.h"
#include "graphics/RIProgram.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "system/LowLevelSystem.h"

#include <NRD.h>

// NRD is resolved at runtime rather than linked; see NrdApi below.
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hpl {
namespace {

constexpr RI_Format_e NrdOutputFormat = RI_FORMAT_RGBA16_SFLOAT;

static void NrdRequire(bool condition, const char *message) {
  if (!condition) {
    assert(condition && message);
    FatalError("NrdIntegration: %s\n", message);
  }
}

static RI_Format_e NrdFormatToRI(nrd::Format format) {
  switch (format) {
  case nrd::Format::R8_UNORM:
    return RI_FORMAT_R8_UNORM;
  case nrd::Format::R8_SNORM:
    return RI_FORMAT_R8_SNORM;
  case nrd::Format::R8_UINT:
    return RI_FORMAT_R8_UINT;
  case nrd::Format::R8_SINT:
    return RI_FORMAT_R8_SINT;

  case nrd::Format::RG8_UNORM:
    return RI_FORMAT_RG8_UNORM;
  case nrd::Format::RG8_SNORM:
    return RI_FORMAT_RG8_SNORM;
  case nrd::Format::RG8_UINT:
    return RI_FORMAT_RG8_UINT;
  case nrd::Format::RG8_SINT:
    return RI_FORMAT_RG8_SINT;

  case nrd::Format::RGBA8_UNORM:
    return RI_FORMAT_RGBA8_UNORM;
  case nrd::Format::RGBA8_SNORM:
    return RI_FORMAT_RGBA8_SNORM;
  case nrd::Format::RGBA8_UINT:
    return RI_FORMAT_RGBA8_UINT;
  case nrd::Format::RGBA8_SINT:
    return RI_FORMAT_RGBA8_SINT;
  case nrd::Format::RGBA8_SRGB:
    return RI_FORMAT_RGBA8_SRGB;

  case nrd::Format::R16_UNORM:
    return RI_FORMAT_R16_UNORM;
  case nrd::Format::R16_SNORM:
    return RI_FORMAT_R16_SNORM;
  case nrd::Format::R16_UINT:
    return RI_FORMAT_R16_UINT;
  case nrd::Format::R16_SINT:
    return RI_FORMAT_R16_SINT;
  case nrd::Format::R16_SFLOAT:
    return RI_FORMAT_R16_SFLOAT;

  case nrd::Format::RG16_UNORM:
    return RI_FORMAT_RG16_UNORM;
  case nrd::Format::RG16_SNORM:
    return RI_FORMAT_RG16_SNORM;
  case nrd::Format::RG16_UINT:
    return RI_FORMAT_RG16_UINT;
  case nrd::Format::RG16_SINT:
    return RI_FORMAT_RG16_SINT;
  case nrd::Format::RG16_SFLOAT:
    return RI_FORMAT_RG16_SFLOAT;

  case nrd::Format::RGBA16_UNORM:
    return RI_FORMAT_RGBA16_UNORM;
  case nrd::Format::RGBA16_SNORM:
    return RI_FORMAT_RGBA16_SNORM;
  case nrd::Format::RGBA16_UINT:
    return RI_FORMAT_RGBA16_UINT;
  case nrd::Format::RGBA16_SINT:
    return RI_FORMAT_RGBA16_SINT;
  case nrd::Format::RGBA16_SFLOAT:
    return RI_FORMAT_RGBA16_SFLOAT;

  case nrd::Format::R32_UINT:
    return RI_FORMAT_R32_UINT;
  case nrd::Format::R32_SINT:
    return RI_FORMAT_R32_SINT;
  case nrd::Format::R32_SFLOAT:
    return RI_FORMAT_R32_SFLOAT;

  case nrd::Format::RG32_UINT:
    return RI_FORMAT_RG32_UINT;
  case nrd::Format::RG32_SINT:
    return RI_FORMAT_RG32_SINT;
  case nrd::Format::RG32_SFLOAT:
    return RI_FORMAT_RG32_SFLOAT;

  case nrd::Format::RGB32_UINT:
    return RI_FORMAT_RGB32_UINT;
  case nrd::Format::RGB32_SINT:
    return RI_FORMAT_RGB32_SINT;
  case nrd::Format::RGB32_SFLOAT:
    return RI_FORMAT_RGB32_SFLOAT;

  case nrd::Format::RGBA32_UINT:
    return RI_FORMAT_RGBA32_UINT;
  case nrd::Format::RGBA32_SINT:
    return RI_FORMAT_RGBA32_SINT;
  case nrd::Format::RGBA32_SFLOAT:
    return RI_FORMAT_RGBA32_SFLOAT;

  case nrd::Format::R10_G10_B10_A2_UNORM:
    return RI_FORMAT_R10_G10_B10_A2_UNORM;
  case nrd::Format::R10_G10_B10_A2_UINT:
    return RI_FORMAT_R10_G10_B10_A2_UINT;
  case nrd::Format::R11_G11_B10_UFLOAT:
    return RI_FORMAT_R11_G11_B10_UFLOAT;
  // RI uses the historical UNORM suffix for the shared-exponent E5B9G9R9
  // Vulkan format; it is the same bit layout as NRD's UFLOAT value.
  case nrd::Format::R9_G9_B9_E5_UFLOAT:
    return RI_FORMAT_R9_G9_B9_E5_UNORM;

  case nrd::Format::MAX_NUM:
    break;
  }

  assert(false && "NrdIntegration: unmapped NRD format");
  FatalError("NrdIntegration: no RI format mapping for NRD format %u\n",
             static_cast<unsigned>(format));
  return RI_FORMAT_UNKNOWN;
}

struct NrdTexture {
  RISharedPointer<RITexture> texture;
  RISharedPointer<RITextureView> sampledView;
  RISharedPointer<RITextureView> storageView;
};

static NrdTexture CreateNrdTexture(cGraphics *graphics, uint32_t width,
                                   uint32_t height, RI_Format_e format,
                                   const char *name) {
  RITextureDesc textureDesc = {};
  textureDesc.type = RI_TEXTURE_2D;
  textureDesc.format = format;
  textureDesc.width = width;
  textureDesc.height = height;
  textureDesc.depth = 1;
  textureDesc.mipNum = 1;
  textureDesc.layerNum = 1;
  textureDesc.sampleCount = RI_SAMPLE_COUNT_1;
  // These are sampled and stored with no layout transition between -- see
  // MakeTextureDescriptor, which deliberately binds the sampled view in
  // GENERAL for the texture's whole lifetime. Vulkan expresses that with
  // VK_IMAGE_LAYOUT_GENERAL; on D3D12 no ordinary layout admits both accesses,
  // so it needs the flag, exactly as nrdMotionVectors does. The cost is lost
  // compression and no UAV clears; NRD never clears these.
  textureDesc.usage = RI_USAGE_SHADER_RESOURCE |
                      RI_USAGE_SHADER_RESOURCE_STORAGE |
                      RI_USAGE_SIMULTANEOUS_ACCESS;

  NrdTexture result;
  result.texture = RISharedPointer<RITexture>(
      &graphics->device, RITexture::create(&graphics->device, textureDesc));
  NrdRequire(!result.texture.isEmpty(), name);
  result.texture->setDebugObjectName(&graphics->device, name);

  RITextureViewDesc viewDesc = {};
  viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
  viewDesc.format = format;
  viewDesc.mipNum = 1;
  viewDesc.layerNum = 1;
  RITextureView sampled = RITextureView::create(
      &graphics->device, result.texture.Get(), viewDesc);
  result.sampledView = RISharedPointer<RITextureView>(&graphics->device,
                                                       sampled);
  NrdRequire(!result.sampledView.isEmpty(), name);

  viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D;
  RITextureView storage = RITextureView::create(
      &graphics->device, result.texture.Get(), viewDesc);
  result.storageView = RISharedPointer<RITextureView>(&graphics->device,
                                                       storage);
  NrdRequire(!result.storageView.isEmpty(), name);
  return result;
}

static uint32_t NrdTextureExtent(uint32_t extent, uint16_t downsampleFactor) {
  NrdRequire(downsampleFactor != 0, "NRD texture has a zero downsample factor");
  return (extent + static_cast<uint32_t>(downsampleFactor) - 1u) /
         static_cast<uint32_t>(downsampleFactor);
}

struct NrdApi {
  decltype(&nrd::CreateInstance) CreateInstance = nullptr;
  decltype(&nrd::DestroyInstance) DestroyInstance = nullptr;
  decltype(&nrd::GetLibraryDesc) GetLibraryDesc = nullptr;
  decltype(&nrd::GetInstanceDesc) GetInstanceDesc = nullptr;
  decltype(&nrd::SetCommonSettings) SetCommonSettings = nullptr;
  decltype(&nrd::SetDenoiserSettings) SetDenoiserSettings = nullptr;
  decltype(&nrd::GetComputeDispatches) GetComputeDispatches = nullptr;
  decltype(&nrd::GetResourceTypeString) GetResourceTypeString = nullptr;
  decltype(&nrd::GetDenoiserString) GetDenoiserString = nullptr;

  bool available = false;
  char unavailableReason[128] = {};
};

static void NrdSetUnavailable(NrdApi &api, const char *reason) {
  api.available = false;
  if (!reason || !reason[0])
    reason = "unspecified reason";
  std::strncpy(api.unavailableReason, reason,
               sizeof(api.unavailableReason) - 1);
  api.unavailableReason[sizeof(api.unavailableReason) - 1] = '\0';
}

static void NrdLoadApi(NrdApi &api) {
  NrdSetUnavailable(api, "NRD has not been loaded");

#if defined(_WIN32)
  // The staging project puts NRD.dll next to the executable, which is the
  // first directory the default search order looks in.
  const char *libraryName = "NRD.dll";
  HMODULE library = LoadLibraryA(libraryName);
#else
  const char *libraryName = "libNRD.so";
  // The game and tools link with -Wl,-rpath,'$ORIGIN/libs', where the staging
  // project puts it. RTLD_LOCAL keeps NRD's symbols out of the global scope.
  void *library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
#endif
  if (!library) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "%s could not be loaded; the denoiser is disabled", libraryName);
    NrdSetUnavailable(api, reason);
    return;
  }

  // The library is deliberately never unloaded: NRD's embedded shader blobs are
  // read straight out of its data segment, and NRD state lives for the process.
#if defined(_WIN32)
#define NRD_LOAD_SYMBOL(member, name)                                          \
  api.member =                                                                 \
      reinterpret_cast<decltype(api.member)>(GetProcAddress(library, name));
#else
#define NRD_LOAD_SYMBOL(member, name)                                          \
  api.member = reinterpret_cast<decltype(api.member)>(dlsym(library, name));
#endif

#define NRD_REQUIRE_SYMBOL(member, name)                                       \
  NRD_LOAD_SYMBOL(member, name)                                                \
  if (!api.member) {                                                           \
    char reason[128];                                                          \
    snprintf(reason, sizeof(reason), "%s is missing the symbol %s",            \
             libraryName, name);                                               \
    NrdSetUnavailable(api, reason);                                            \
    return;                                                                    \
  }

  NRD_REQUIRE_SYMBOL(CreateInstance, "CreateInstance");
  NRD_REQUIRE_SYMBOL(DestroyInstance, "DestroyInstance");
  NRD_REQUIRE_SYMBOL(GetLibraryDesc, "GetLibraryDesc");
  NRD_REQUIRE_SYMBOL(GetInstanceDesc, "GetInstanceDesc");
  NRD_REQUIRE_SYMBOL(SetCommonSettings, "SetCommonSettings");
  NRD_REQUIRE_SYMBOL(SetDenoiserSettings, "SetDenoiserSettings");
  NRD_REQUIRE_SYMBOL(GetComputeDispatches, "GetComputeDispatches");
  NRD_REQUIRE_SYMBOL(GetResourceTypeString, "GetResourceTypeString");
  NRD_REQUIRE_SYMBOL(GetDenoiserString, "GetDenoiserString");

#undef NRD_REQUIRE_SYMBOL
#undef NRD_LOAD_SYMBOL

  // The library is now a separately shipped file that can drift from the
  // headers this was built against -- something a static link made impossible.
  // The descriptor layouts and enum values are version-specific, so a mismatch
  // has to disable the denoiser rather than corrupt NRD's structs.
  const nrd::LibraryDesc *desc = api.GetLibraryDesc();
  if (!desc) {
    NrdSetUnavailable(api, "NRD library description is null");
    return;
  }
  if (desc->versionMajor != NRD_VERSION_MAJOR ||
      desc->versionMinor != NRD_VERSION_MINOR) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "%s is version %u.%u, expected %u.%u", libraryName,
             static_cast<unsigned>(desc->versionMajor),
             static_cast<unsigned>(desc->versionMinor),
             static_cast<unsigned>(NRD_VERSION_MAJOR),
             static_cast<unsigned>(NRD_VERSION_MINOR));
    NrdSetUnavailable(api, reason);
    return;
  }

  api.available = true;
  api.unavailableReason[0] = '\0';
}

// Resolved once on first use. The reason is logged here, so a build running
// without NRD says so exactly once instead of on every denoiser construction.
static const NrdApi &Nrd() {
  static const NrdApi api = [] {
    NrdApi loaded;
    NrdLoadApi(loaded);
    if (loaded.available)
      Log("NrdIntegration: NRD %u.%u loaded\n",
          static_cast<unsigned>(NRD_VERSION_MAJOR),
          static_cast<unsigned>(NRD_VERSION_MINOR));
    else
      Warning("NrdIntegration: %s\n", loaded.unavailableReason);
    return loaded;
  }();
  return api;
}

std::atomic<uint32_t> g_nrdInstancesCreated{0};

} // namespace

struct NrdIntegration::Impl {
  explicit Impl(cGraphics *graphics, NrdDenoiserMode mode)
      : graphics(graphics),
        denoiser(mode == NrdDenoiserMode::Specular
                     ? nrd::Denoiser::REBLUR_SPECULAR
                     : mode == NrdDenoiserMode::DirectDiffuse
                           ? nrd::Denoiser::RELAX_DIFFUSE
                           : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR),
        denoiserIdentifier(static_cast<nrd::Identifier>(denoiser)) {
    NrdRequire(graphics != nullptr, "graphics is null");
    // Callers are required to gate on NrdIntegration::IsAvailable(); reaching
    // here without the library is a bug in the caller, not a missing file.
    NrdRequire(Nrd().available,
               "constructed while NRD is unavailable -- callers must check "
               "NrdIntegration::IsAvailable()");

    // Keep NRD's C++ normalization settings in lockstep with the shader-side
    // REBLUR_FrontEnd_GetNormHitDist parameters.
    reblurSettings.hitDistanceParameters.A = kNrdHitDistanceParameters.x;
    reblurSettings.hitDistanceParameters.B = kNrdHitDistanceParameters.y;
    reblurSettings.hitDistanceParameters.C = kNrdHitDistanceParameters.z;

    if (denoiser == nrd::Denoiser::RELAX_DIFFUSE) {
      // ReSTIR resolves direct irradiance at every surface pixel. Let RELAX's
      // variance-guided A-trous passes remove noise while stopping at lighting
      // edges; a broad prepass would soften shadows before edge detection.
      // Diffuse hit distance is used only by that prepass, so no synthetic
      // shadow/indirect hit-distance blend is needed for this input.
      relaxSettings.diffusePrepassBlurRadius = 0.0f;
    } else if (denoiser == nrd::Denoiser::REBLUR_SPECULAR) {
      // The specular-only consumer traces a specular sample at every valid
      // texel of a half-width and half-height reflection buffer, so there is
      // no missing lobe distance to reconstruct and no checkerboard encoding
      // in play.
      reblurSettings.hitDistanceReconstructionMode =
          nrd::HitDistanceReconstructionMode::OFF;
    } else {
      // The tracer's probabilistic lobe split leaves a 0 hit distance in the
      // skipped channel every frame, so enable reconstruction for the missing
      // lobe distance.
      reblurSettings.hitDistanceReconstructionMode =
          nrd::HitDistanceReconstructionMode::AREA_3X3;
    }

    const nrd::LibraryDesc *libraryDesc = Nrd().GetLibraryDesc();
    NrdRequire(libraryDesc != nullptr, "NRD library description is null");
    library = *libraryDesc;

    nrd::DenoiserDesc denoiserDesc = {};
    denoiserDesc.identifier = denoiserIdentifier;
    denoiserDesc.denoiser = denoiser;
    nrd::InstanceCreationDesc creationDesc = {};
    creationDesc.denoisers = &denoiserDesc;
    creationDesc.denoisersNum = 1;
    NrdRequire(Nrd().CreateInstance(creationDesc, instance) ==
                   nrd::Result::SUCCESS,
               "failed to create NRD instance");

    const nrd::InstanceDesc *desc = Nrd().GetInstanceDesc(*instance);
    NrdRequire(desc != nullptr, "NRD instance description is null");
    instanceDesc = *desc;

    NrdRequire(instanceDesc.constantBufferAndSamplersSpaceIndex <
                   RIProgram::DESCRIPTOR_SET_MAX,
               "NRD constant/sampler set is outside RIProgram's four-set limit");
    NrdRequire(instanceDesc.resourcesSpaceIndex < RIProgram::DESCRIPTOR_SET_MAX,
               "NRD resource set is outside RIProgram's four-set limit");

    BuildPrograms();
    ++g_nrdInstancesCreated;
  }

  ~Impl() {
    ReleaseTextures();
    for (std::unique_ptr<RIProgram> &program : programs) {
      if (program)
        program->dispose(&graphics->device);
    }
    if (instance) {
      Nrd().DestroyInstance(*instance);
      instance = nullptr;
    }
  }

  // True when NRD's DXIL blobs are the ones to feed RIProgram. NRD ships every
  // container it was built with, so this follows the active backend rather than
  // whichever happens to be populated.
  static bool UseDxilShaders() {
#if (DEVICE_IMPL_D3D12)
    return RIIsTargetSelected(RI_DEVICE_API_D3D12);
#else
    return false;
#endif
  }

  const char *EntryPoint() const {
    return instanceDesc.shaderEntryPoint ? instanceDesc.shaderEntryPoint
                                         : "main";
  }

  // D3D12 binds NRD's descriptors by name rather than by (space, register).
  // RIProgram::findReflectionBySlot keys on (set, register) alone, and DXIL
  // keeps the native registers -- no SPIR-V shifts -- so t0 and u0 both land on
  // (resourcesSpaceIndex, 0), as do b0 and s0 in the sampler space. Those shifts
  // are exactly what keeps the two apart on Vulkan. The names are ours to pick,
  // so this scheme makes every binding unique by construction, and it is shared
  // with the reflection below so the two cannot drift apart.
  static std::string BindingName(RIProgram::ShaderRegisterClass registerClass,
                                 uint32_t registerIndex,
                                 uint32_t registerSpace) {
    const char letter =
        registerClass == RIProgram::ShaderRegisterClass::CBV     ? 'b'
        : registerClass == RIProgram::ShaderRegisterClass::SRV   ? 't'
        : registerClass == RIProgram::ShaderRegisterClass::UAV   ? 'u'
                                                                 : 's';
    char name[64];
    snprintf(name, sizeof(name), "%c%u_space%u", letter,
             static_cast<unsigned>(registerIndex),
             static_cast<unsigned>(registerSpace));
    return name;
  }

  // NRD's shaders carry no reflection: they are embedded blobs with no sidecar,
  // and NRD compiles them with --stripReflection. The D3D12 path needs one
  // anyway, because that is what its root signature is built from. NRD's layout
  // is uniform and fully described by its own API, so it can be reconstructed
  // exactly rather than recovered from the container.
  std::shared_ptr<const RIProgram::ShaderReflection>
  SynthesizeReflection(const nrd::PipelineDesc &pipelineDesc) const {
    auto reflection = std::make_shared<RIProgram::ShaderReflection>();
    reflection->entryPoint = EntryPoint();
    // Must match ri_stageName(PROGRAM_STAGE_COMPUTE); initialize() compares the
    // two directly for a non-library artifact.
    reflection->stage = "compute";
    // NRD passes its parameters in a constant buffer, never as root constants.
    reflection->pushConstants.present = false;

    const auto add = [&](RIProgram::ShaderRegisterClass registerClass,
                         uint32_t registerIndex, uint32_t registerSpace) {
      RIProgram::ShaderResourceReflection resource;
      resource.name = BindingName(registerClass, registerIndex, registerSpace);
      resource.stage = reflection->stage;
      resource.registerClass = registerClass;
      resource.registerIndex = registerIndex;
      resource.registerSpace = registerSpace;
      resource.arrayCount = 1;
      resource.used = true;
      reflection->resources.push_back(std::move(resource));
    };

    // Ranges are ordered inputs-then-outputs, and SRVs and UAVs are separate
    // register namespaces in HLSL, so both count up from the same base.
    for (uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum;
         ++rangeIndex) {
      const nrd::ResourceRangeDesc &range = pipelineDesc.resourceRanges[rangeIndex];
      const RIProgram::ShaderRegisterClass registerClass =
          range.descriptorType == nrd::DescriptorType::TEXTURE
              ? RIProgram::ShaderRegisterClass::SRV
              : RIProgram::ShaderRegisterClass::UAV;
      for (uint32_t i = 0; i < range.descriptorsNum; ++i)
        add(registerClass, instanceDesc.resourcesBaseRegisterIndex + i,
            instanceDesc.resourcesSpaceIndex);
    }
    for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
      add(RIProgram::ShaderRegisterClass::Sampler,
          instanceDesc.samplersBaseRegisterIndex + i,
          instanceDesc.constantBufferAndSamplersSpaceIndex);
    if (pipelineDesc.hasConstantData)
      add(RIProgram::ShaderRegisterClass::CBV,
          instanceDesc.constantBufferRegisterIndex,
          instanceDesc.constantBufferAndSamplersSpaceIndex);
    return reflection;
  }

  void BuildPrograms() {
    NrdRequire(instanceDesc.pipelines != nullptr || instanceDesc.pipelinesNum == 0,
               "NRD pipeline description is null");
    const bool dxil = UseDxilShaders();
    programs.resize(instanceDesc.pipelinesNum);
    for (uint32_t i = 0; i < instanceDesc.pipelinesNum; ++i) {
      const nrd::PipelineDesc &pipelineDesc = instanceDesc.pipelines[i];
      const nrd::ComputeShaderDesc &shader = dxil
                                                 ? pipelineDesc.computeShaderDXIL
                                                 : pipelineDesc.computeShaderSPIRV;
      // An empty container means NRD was configured without it -- see
      // NRD_EMBEDS_*_SHADERS in premake/external.lua -- so say which one is
      // missing rather than letting it surface later as a format error.
      NrdRequire(shader.bytecode != nullptr && shader.size != 0,
                 dxil ? "NRD was built without DXIL shaders (needs "
                        "NRD_EMBEDS_DXIL_SHADERS=ON)"
                      : "NRD was built without SPIR-V shaders (needs "
                        "NRD_EMBEDS_SPIRV_SHADERS=ON)");

      auto program = std::make_unique<RIProgram>();
      RIProgram::ModuleStage stage = {};
      stage.stage = RIProgram::PROGRAM_STAGE_COMPUTE;
      stage.data = std::span<char>(
          static_cast<char *>(const_cast<void *>(shader.bytecode)),
          static_cast<size_t>(shader.size));
      stage.entryPoint = EntryPoint();
      if (dxil) {
        // D3D12 refuses a module with no retained reflection, so the bytes and
        // the synthesized reflection travel together as an artifact. `json`
        // stays null, which is what makes initialize() keep this reflection
        // instead of re-parsing one.
        stage.artifact.bytes = std::make_shared<const std::vector<char>>(
            stage.data.begin(), stage.data.end());
        stage.artifact.format = RIShaderArtifactFormat::Dxil;
        stage.artifact.reflection = SynthesizeReflection(pipelineDesc);
        stage.data = stage.artifact;
        stage.format = stage.artifact.format;
      }
      program->initialize(&graphics->device, std::span<RIProgram::ModuleStage>(
                                                   &stage, 1),
                          {}, pipelineDesc.shaderIdentifier);
      programs[i] = std::move(program);
    }
  }

  void ReleaseTexture(NrdTexture &texture) {
    // A mode that does not allocate this channel leaves the whole entry empty.
    if (texture.texture.isEmpty())
      return;
    // Views are parked before their image so Vulkan never observes a live view
    // after the image is released.  The deferral queue releases these after
    // the current frame's graphics timeline value.
    graphics->graphicsDefer.push(std::move(texture.sampledView));
    graphics->graphicsDefer.push(std::move(texture.storageView));
    graphics->graphicsDefer.push(std::move(texture.texture));
  }

  void ReleaseTextures() {
    for (NrdTexture &texture : permanentPool)
      ReleaseTexture(texture);
    for (NrdTexture &texture : transientPool)
      ReleaseTexture(texture);
    ReleaseTexture(diffuseOutput);
    ReleaseTexture(specularOutput);
    permanentPool.clear();
    transientPool.clear();
  }

  void OnResize(uint32_t newWidth, uint32_t newHeight) {
    if (newWidth == width && newHeight == height)
      return;

    ReleaseTextures();
    width = newWidth;
    height = newHeight;
    texturesInGeneral = false;
    historyReset = true;
    if (width == 0 || height == 0)
      return;

    NrdRequire(width <= std::numeric_limits<uint16_t>::max() &&
                   height <= std::numeric_limits<uint16_t>::max(),
               "render extent exceeds NRD CommonSettings uint16 dimensions");

    permanentPool.resize(instanceDesc.permanentPoolSize);
    for (uint32_t i = 0; i < instanceDesc.permanentPoolSize; ++i) {
      NrdRequire(instanceDesc.permanentPool != nullptr,
                 "NRD permanent pool description is null");
      const nrd::TextureDesc &desc = instanceDesc.permanentPool[i];
      const uint32_t textureWidth = NrdTextureExtent(width, desc.downsampleFactor);
      const uint32_t textureHeight =
          NrdTextureExtent(height, desc.downsampleFactor);
      permanentPool[i] = CreateNrdTexture(
          graphics, textureWidth, textureHeight, NrdFormatToRI(desc.format),
          "failed to create NRD permanent-pool texture");
    }

    transientPool.resize(instanceDesc.transientPoolSize);
    for (uint32_t i = 0; i < instanceDesc.transientPoolSize; ++i) {
      NrdRequire(instanceDesc.transientPool != nullptr,
                 "NRD transient pool description is null");
      const nrd::TextureDesc &desc = instanceDesc.transientPool[i];
      const uint32_t textureWidth = NrdTextureExtent(width, desc.downsampleFactor);
      const uint32_t textureHeight =
          NrdTextureExtent(height, desc.downsampleFactor);
      transientPool[i] = CreateNrdTexture(
          graphics, textureWidth, textureHeight, NrdFormatToRI(desc.format),
          "failed to create NRD transient-pool texture");
    }

    // NRD's OUT_* resources are application-owned resources, not entries in
    // either pool.  Keep them in the same storage-capable RGBA16F convention
    // as the engine's radiance+hit-distance inputs and return their SRV views.
    if (denoiser != nrd::Denoiser::REBLUR_SPECULAR) {
      diffuseOutput = CreateNrdTexture(
          graphics, width, height, NrdOutputFormat,
          "failed to create NRD diffuse output");
    }
    if (denoiser != nrd::Denoiser::RELAX_DIFFUSE)
      specularOutput = CreateNrdTexture(graphics, width, height, NrdOutputFormat,
                                        "failed to create NRD specular output");
  }

  void ResetHistory() { historyReset = true; }

  void TransitionTexturesToGeneral(RICmd *cmd) {
    std::vector<RITextureBarrier> barriers;
    barriers.reserve(permanentPool.size() + transientPool.size() +
                     (denoiser == nrd::Denoiser::REBLUR_SPECULAR ? 1 : 2));
    auto add = [&barriers](NrdTexture &texture) {
      if (texture.texture.isEmpty())
        return;
      barriers.emplace_back(texture.texture.Get(), RI_RESOURCE_STATE_UNDEFINED,
                            RI_RESOURCE_STATE_GENERAL, RI_STAGE_NONE,
                            RI_STAGE_COMPUTE);
    };
    for (NrdTexture &texture : permanentPool)
      add(texture);
    for (NrdTexture &texture : transientPool)
      add(texture);
    add(diffuseOutput);
    add(specularOutput);
    if (!barriers.empty())
      cmd->vk_d3d12_textureBarriers<0>(static_cast<uint32_t>(barriers.size()),
                                        barriers.data());
    texturesInGeneral = true;
  }

  RITextureView *ResolvePoolResource(nrd::ResourceType type,
                                     uint16_t indexInPool,
                                     nrd::DescriptorType descriptorType) {
    std::vector<NrdTexture> *pool = nullptr;
    switch (type) {
    case nrd::ResourceType::TRANSIENT_POOL:
      pool = &transientPool;
      break;
    case nrd::ResourceType::PERMANENT_POOL:
      pool = &permanentPool;
      break;
    default:
      break;
    }
    NrdRequire(pool != nullptr, "NRD resource is not a pool resource");
    NrdRequire(indexInPool < pool->size(),
               "NRD pool resource index is out of bounds");
    NrdTexture &texture = (*pool)[indexInPool];
    return descriptorType == nrd::DescriptorType::STORAGE_TEXTURE
               ? texture.storageView.Get()
               : texture.sampledView.Get();
  }

  RITextureView *ResolveResource(nrd::ResourceType type, uint16_t indexInPool,
                                 nrd::DescriptorType descriptorType,
                                 const NrdDenoiseInputs &inputs) {
    switch (type) {
    case nrd::ResourceType::IN_MV:
      return inputs.motionVectors;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
      return inputs.normalRoughness;
    case nrd::ResourceType::IN_VIEWZ:
      return inputs.viewZ;
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
      NrdRequire(denoiser != nrd::Denoiser::REBLUR_SPECULAR,
                 "IN_DIFF_RADIANCE_HITDIST is unavailable in specular mode");
      return inputs.diffuseRadianceHitDistance;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
      return inputs.specularRadianceHitDistance;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
      NrdRequire(denoiser != nrd::Denoiser::REBLUR_SPECULAR,
                 "OUT_DIFF_RADIANCE_HITDIST is unavailable in specular mode");
      return descriptorType == nrd::DescriptorType::STORAGE_TEXTURE
                 ? diffuseOutput.storageView.Get()
                 : diffuseOutput.sampledView.Get();
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
      return descriptorType == nrd::DescriptorType::STORAGE_TEXTURE
                 ? specularOutput.storageView.Get()
                 : specularOutput.sampledView.Get();
    case nrd::ResourceType::TRANSIENT_POOL:
    case nrd::ResourceType::PERMANENT_POOL:
      return ResolvePoolResource(type, indexInPool, descriptorType);
    default:
      break;
    }
    assert(false && "NrdIntegration: unsupported resource type");
    FatalError("NrdIntegration: unsupported NRD resource type %u (%s)\n",
               static_cast<unsigned>(type), Nrd().GetResourceTypeString(type));
    return nullptr;
  }

  RIDescriptor MakeTextureDescriptor(RITextureView *view,
                                     nrd::DescriptorType descriptorType) {
    NrdRequire(view != nullptr, "NRD resource view is null");
    if (descriptorType == nrd::DescriptorType::STORAGE_TEXTURE)
      return RIDescriptor::storageImage(&graphics->device, view);

    // Pool and output images stay in GENERAL for their complete lifetime;
    // this sampled descriptor therefore deliberately does not request
    // SHADER_READ_ONLY_OPTIMAL.  External input views retain their caller's
    // read-only layout and are bound with the normal sampled-image factory.
    return RIDescriptor::sampledImage(&graphics->device, view,
                                      RI_RESOURCE_STATE_GENERAL);
  }

  RIDescriptor MakeInputDescriptor(RITextureView *view, nrd::ResourceType type,
                                   nrd::DescriptorType descriptorType) {
    NrdRequire(view != nullptr, "NRD input view is null");
    if (descriptorType == nrd::DescriptorType::STORAGE_TEXTURE)
      return RIDescriptor::storageImage(&graphics->device, view);
    if (type == nrd::ResourceType::IN_MV &&
        denoiser != nrd::Denoiser::RELAX_DIFFUSE) {
      // REBLUR samples IN_MV in temporal passes and writes it during
      // stabilization. Bind its sampled view in GENERAL to match the
      // caller's read+write texture state.
      return RIDescriptor::sampledImage(
          &graphics->device, view,
          static_cast<RIResourceState_e>(
              RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE));
    }
    return RIDescriptor::sampledImage(&graphics->device, view);
  }

  RIDescriptor MakeResourceDescriptor(nrd::ResourceType type,
                                      uint16_t indexInPool,
                                      nrd::DescriptorType descriptorType,
                                      const NrdDenoiseInputs &inputs) {
    RITextureView *view = ResolveResource(type, indexInPool, descriptorType,
                                          inputs);
    const bool isPoolOrOutput =
        type == nrd::ResourceType::TRANSIENT_POOL ||
        type == nrd::ResourceType::PERMANENT_POOL ||
        type == nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST ||
        type == nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST;
    return isPoolOrOutput
               ? MakeTextureDescriptor(view, descriptorType)
               : MakeInputDescriptor(view, type, descriptorType);
  }

  // One binding, addressed the way the active backend's reflection indexes it:
  // by name on D3D12 (see BindingName), by the SPIR-V-shifted (set, binding)
  // slot on Vulkan. Both callers below go through this so the register
  // arithmetic exists in exactly one place.
  RIProgram::DescriptorBinding
  MakeBinding(RIProgram::ShaderRegisterClass registerClass,
              uint32_t registerIndex, uint32_t registerSpace,
              uint32_t spirvOffset, const RIDescriptor &descriptor) {
    if (UseDxilShaders()) {
      nameStorage.push_back(
          BindingName(registerClass, registerIndex, registerSpace));
      return RIProgram::DescriptorBinding(nameStorage.back().c_str(),
                                          descriptor);
    }
    return RIProgram::DescriptorBinding(registerSpace,
                                        registerIndex + spirvOffset,
                                        descriptor);
  }

  void AppendSamplers(std::vector<RIProgram::DescriptorBinding> &bindings) {
    const uint32_t set = instanceDesc.constantBufferAndSamplersSpaceIndex;
    for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i) {
      eTextureFilter filter;
      switch (instanceDesc.samplers[i]) {
      case nrd::Sampler::NEAREST_CLAMP:
        filter = eTextureFilter_Nearest;
        break;
      case nrd::Sampler::LINEAR_CLAMP:
        filter = eTextureFilter_Bilinear;
        break;
      case nrd::Sampler::MAX_NUM:
        assert(false && "NrdIntegration: invalid NRD sampler");
        FatalError("NrdIntegration: invalid NRD sampler %u\n",
                   static_cast<unsigned>(instanceDesc.samplers[i]));
      }
      std::optional<RIDescriptor> sampler = graphics->resolve_filter_descriptor(
          eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
          eTextureWrap_ClampToEdge, filter);
      NrdRequire(sampler.has_value(), "failed to create NRD sampler");
      bindings.push_back(MakeBinding(RIProgram::ShaderRegisterClass::Sampler,
                                     instanceDesc.samplersBaseRegisterIndex + i,
                                     set,
                                     library.spirvBindingOffsets.samplerOffset,
                                     *sampler));
    }
  }

  void BindDispatch(RICmd *cmd, uint32_t frameIndex,
                    const nrd::DispatchDesc &dispatchDesc,
                    const NrdDenoiseInputs &inputs,
                    RIDescriptor &previousConstantBuffer,
                    bool &hasPreviousConstantBuffer) {
    NrdRequire(dispatchDesc.pipelineIndex < programs.size(),
               "NRD dispatch pipeline index is out of bounds");
    const nrd::PipelineDesc &pipelineDesc =
        instanceDesc.pipelines[dispatchDesc.pipelineIndex];
    RIProgram &program = *programs[dispatchDesc.pipelineIndex];

    std::vector<RIProgram::DescriptorBinding> bindings;
    bindings.reserve(dispatchDesc.resourcesNum + instanceDesc.samplersNum + 1);
    // The names handed out below only need to outlive this dispatch.
    nameStorage.clear();

    uint32_t resourceIndex = 0;
    for (uint32_t rangeIndex = 0;
         rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex) {
      const nrd::ResourceRangeDesc &range =
          pipelineDesc.resourceRanges[rangeIndex];
      for (uint32_t rangeElement = 0; rangeElement < range.descriptorsNum;
           ++rangeElement) {
        NrdRequire(resourceIndex < dispatchDesc.resourcesNum,
                   "NRD dispatch resource range exceeds resource list");
        const nrd::ResourceDesc &resource =
            dispatchDesc.resources[resourceIndex++];
        NrdRequire(resource.descriptorType == range.descriptorType,
                   "NRD dispatch resource range type does not match pipeline");

        const bool isTexture =
            range.descriptorType == nrd::DescriptorType::TEXTURE;
        const uint32_t bindingOffset =
            isTexture ? library.spirvBindingOffsets.textureOffset
                      : library.spirvBindingOffsets.storageTextureAndBufferOffset;
        bindings.push_back(MakeBinding(
            isTexture ? RIProgram::ShaderRegisterClass::SRV
                      : RIProgram::ShaderRegisterClass::UAV,
            instanceDesc.resourcesBaseRegisterIndex + rangeElement,
            instanceDesc.resourcesSpaceIndex, bindingOffset,
            MakeResourceDescriptor(resource.type, resource.indexInPool,
                                   resource.descriptorType, inputs)));
      }
    }
    NrdRequire(resourceIndex == dispatchDesc.resourcesNum,
               "NRD dispatch has resources outside its pipeline ranges");

    AppendSamplers(bindings);

    if (pipelineDesc.hasConstantData) {
      NrdRequire(dispatchDesc.constantBufferData != nullptr &&
                     dispatchDesc.constantBufferDataSize != 0,
                 "NRD dispatch declares constant data but supplied none");
      NrdRequire(
          dispatchDesc.constantBufferDataSize <=
              instanceDesc.constantBufferMaxDataSize,
          "NRD dispatch constant data exceeds instance limit");
      RIDescriptor constantBuffer;
      if (dispatchDesc.constantBufferDataMatchesPreviousDispatch) {
        NrdRequire(hasPreviousConstantBuffer,
                   "NRD dispatch reuses a missing previous constant buffer");
        constantBuffer = previousConstantBuffer;
      } else {
        graphics->UpdateFrameUBO(
            &constantBuffer,
            const_cast<uint8_t *>(dispatchDesc.constantBufferData),
            dispatchDesc.constantBufferDataSize);
        previousConstantBuffer = constantBuffer;
        hasPreviousConstantBuffer = true;
      }
      bindings.push_back(
          MakeBinding(RIProgram::ShaderRegisterClass::CBV,
                      instanceDesc.constantBufferRegisterIndex,
                      instanceDesc.constantBufferAndSamplersSpaceIndex,
                      library.spirvBindingOffsets.constantBufferOffset,
                      constantBuffer));
    }

    const hash_t pipelineHash =
        hash_u32(HASH_INITIAL_VALUE, dispatchDesc.pipelineIndex);
    program.bindComputePipeline(&graphics->device, cmd, pipelineHash,
                                pipelineDesc.shaderIdentifier);
    program.bindDescriptors(&graphics->device, cmd, frameIndex, bindings.data(),
                            bindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
    cmd->dispatch(&graphics->device, dispatchDesc.gridWidth,
                  dispatchDesc.gridHeight, 1);
  }

  NrdDenoiseOutputs Denoise(RICmd *cmd, const NrdFrameData &frame,
                            const NrdDenoiseInputs &inputs) {
    NrdRequire(cmd != nullptr, "command buffer is null");
    NrdRequire(width != 0 && height != 0,
               "OnResize must be called before Denoise");
    NrdRequire(instance != nullptr, "NRD instance is null");

    nrd::CommonSettings commonSettings = {};
    std::memcpy(commonSettings.viewToClipMatrix, frame.viewToClipMatrix,
                sizeof(commonSettings.viewToClipMatrix));
    std::memcpy(commonSettings.viewToClipMatrixPrev,
                frame.viewToClipMatrixPrev,
                sizeof(commonSettings.viewToClipMatrixPrev));
    std::memcpy(commonSettings.worldToViewMatrix, frame.worldToViewMatrix,
                sizeof(commonSettings.worldToViewMatrix));
    std::memcpy(commonSettings.worldToViewMatrixPrev,
                frame.worldToViewMatrixPrev,
                sizeof(commonSettings.worldToViewMatrixPrev));
    commonSettings.cameraJitter[0] = frame.cameraJitter[0];
    commonSettings.cameraJitter[1] = frame.cameraJitter[1];
    commonSettings.cameraJitterPrev[0] = frame.cameraJitterPrev[0];
    commonSettings.cameraJitterPrev[1] = frame.cameraJitterPrev[1];
    // The engine's gVelocity is current - previous and its temporal shaders
    // subtract it from the current UV.  NRD adds MV to current UV, so negate
    // the two screen-space components to provide NRD's previous - current.
    commonSettings.motionVectorScale[0] = -1.0f;
    commonSettings.motionVectorScale[1] = -1.0f;
    commonSettings.motionVectorScale[2] = 0.0f;
    commonSettings.isMotionVectorInWorldSpace = false;
    commonSettings.resourceSize[0] = static_cast<uint16_t>(width);
    commonSettings.resourceSize[1] = static_cast<uint16_t>(height);
    commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(width);
    commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(height);
    commonSettings.rectSize[0] = static_cast<uint16_t>(width);
    commonSettings.rectSize[1] = static_cast<uint16_t>(height);
    commonSettings.rectSizePrev[0] = static_cast<uint16_t>(width);
    commonSettings.rectSizePrev[1] = static_cast<uint16_t>(height);
    commonSettings.frameIndex = frame.frameIndex;
    // Beyond this view-space depth NRD treats a pixel as sky/background and
    // skips it. Fed from the frustum far plane so it tracks the camera.
    if (frame.denoisingRange > 0.0f)
      commonSettings.denoisingRange = frame.denoisingRange;
    // Drives antilag and accumulation speed. Left at 0 NRD derives a fixed step
    // from frameIndex, which desyncs the denoiser from a variable frame rate.
    commonSettings.timeDeltaBetweenFrames = frame.timeDeltaMs;
    commonSettings.accumulationMode =
        historyReset ? nrd::AccumulationMode::CLEAR_AND_RESTART
                     : nrd::AccumulationMode::CONTINUE;

    NrdRequire(Nrd().SetCommonSettings(*instance, commonSettings) ==
                   nrd::Result::SUCCESS,
               "SetCommonSettings failed");
    NrdRequire(Nrd().SetDenoiserSettings(*instance, denoiserIdentifier,
                                        denoiser == nrd::Denoiser::RELAX_DIFFUSE
                                            ? static_cast<const void *>(&relaxSettings)
                                            : static_cast<const void *>(&reblurSettings)) == nrd::Result::SUCCESS,
               "SetDenoiserSettings failed");

    if (!texturesInGeneral)
      TransitionTexturesToGeneral(cmd);

    const nrd::DispatchDesc *dispatchDescs = nullptr;
    uint32_t dispatchDescsNum = 0;
    NrdRequire(Nrd().GetComputeDispatches(*instance, &denoiserIdentifier, 1,
                                         dispatchDescs,
                                         dispatchDescsNum) == nrd::Result::SUCCESS,
               "GetComputeDispatches failed");

    RIGpuScope denoiseScope(&graphics->profiler, cmd,
                            Nrd().GetDenoiserString(denoiser));
    RIDescriptor previousConstantBuffer;
    bool hasPreviousConstantBuffer = false;
    for (uint32_t i = 0; i < dispatchDescsNum; ++i) {
      if (i != 0) {
        // NRD pool images remain in GENERAL.  This is the required execution
        // and memory dependency between a compute storage write and the next
        // dispatch's sampled/storage read; no layout transition is needed.
        cmd->vk_d3d12_memoryBarrier(
            {RI_RESOURCE_STATE_STORAGE_WRITE,
             RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE,
             RI_STAGE_COMPUTE});
      }
      RIGpuScope dispatchScope(
          &graphics->profiler, cmd,
          dispatchDescs[i].name ? dispatchDescs[i].name : "NRD dispatch");
      BindDispatch(cmd, frame.frameIndex, dispatchDescs[i], inputs,
                   previousConstantBuffer, hasPreviousConstantBuffer);
    }
    if (dispatchDescsNum != 0) {
      // Leave the application-owned outputs in GENERAL, but make the final
      // NRD storage write visible to the caller's subsequent shader read.
      cmd->vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE,
           RI_STAGE_COMPUTE});
    }
    historyReset = false;

    if (denoiser == nrd::Denoiser::REBLUR_SPECULAR)
      return {nullptr, specularOutput.sampledView.Get()};
    if (denoiser == nrd::Denoiser::RELAX_DIFFUSE)
      return {diffuseOutput.sampledView.Get(), nullptr};
    return {diffuseOutput.sampledView.Get(), specularOutput.sampledView.Get()};
  }

  cGraphics *graphics = nullptr;
  nrd::Denoiser denoiser;
  nrd::Identifier denoiserIdentifier;
  nrd::Instance *instance = nullptr;
  nrd::LibraryDesc library = {};
  nrd::InstanceDesc instanceDesc = {};
  nrd::ReblurSettings reblurSettings = {};
  nrd::RelaxSettings relaxSettings = {};
  std::vector<std::unique_ptr<RIProgram>> programs;
  // Backing store for the names MakeBinding hands to DescriptorBinding on
  // D3D12. DescriptorBindingID keeps the `const char *` rather than copying it,
  // so the string has to outlive the bindDescriptors call. A deque, not a
  // vector: these names are short enough for the small-string optimization, and
  // a vector reallocation would move the inline buffer out from under a pointer
  // already handed out. Cleared at the top of each BindDispatch.
  std::deque<std::string> nameStorage;
  std::vector<NrdTexture> permanentPool;
  std::vector<NrdTexture> transientPool;
  NrdTexture diffuseOutput;
  NrdTexture specularOutput;
  uint32_t width = 0;
  uint32_t height = 0;
  bool texturesInGeneral = false;
  bool historyReset = true;
};

bool NrdIntegration::IsAvailable() { return Nrd().available; }

uint32_t NrdIntegration::InstancesCreated() { return g_nrdInstancesCreated; }

NrdIntegration::NrdIntegration(cGraphics *graphics, NrdDenoiserMode mode)
    : m_impl(std::make_unique<Impl>(graphics, mode)) {}

NrdIntegration::~NrdIntegration() = default;

void NrdIntegration::OnResize(uint32_t width, uint32_t height) {
  m_impl->OnResize(width, height);
}

void NrdIntegration::ResetHistory() { m_impl->ResetHistory(); }

NrdDenoiseOutputs NrdIntegration::Denoise(RICmd *cmd,
                                          const NrdFrameData &frame,
                                          const NrdDenoiseInputs &inputs) {
  return m_impl->Denoise(cmd, frame, inputs);
}

} // namespace hpl
