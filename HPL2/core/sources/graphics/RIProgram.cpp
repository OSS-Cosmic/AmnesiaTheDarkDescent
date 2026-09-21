#include "graphics/RIProgram.h"
#include "system/Platform.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <rapidjson/document.h>
#include <string>
#include <string_view>
#include <system/Types.h>
#include <system/stb_ds.h>

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIRenderer.h"
#include "graphics/spirv_reflect.h"

#include <fmt/format.h>

#if (DEVICE_IMPL_VULKAN)
#include "graphics/RIVK.h" // RIFormatToVK + the ri_vk_RI*ToVK pipeline translators
#endif

#if (DEVICE_IMPL_D3D12)
#include <D3D12MemAlloc.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#if defined(_MSC_VER)
#pragma comment(lib, "d3dcompiler.lib")
#endif
#endif

namespace hpl {

#if (DEVICE_IMPL_D3D12)
static void ri_d3d12_replace_slot(ID3D12Resource *&oldResource,
                                  D3D12MA::Allocation *&oldAllocation,
                                  ID3D12Resource *resource,
                                  D3D12MA::Allocation *allocation) {
  if (oldResource == resource && oldAllocation == allocation)
    return;
  if (resource)
    resource->AddRef();
  if (allocation)
    allocation->AddRef();
  if (oldResource)
    oldResource->Release();
  if (oldAllocation)
    oldAllocation->Release();
  oldResource = resource;
  oldAllocation = allocation;
}

// Drops the references a descriptor-cache entry retained for its bound
// resources, leaving every slot null.
static void
ri_d3d12_release_cache_refs(std::vector<ID3D12Resource *> &resources,
                            std::vector<D3D12MA::Allocation *> &allocations) {
  for (ID3D12Resource *&resource : resources) {
    if (resource)
      resource->Release();
    resource = nullptr;
  }
  for (D3D12MA::Allocation *&allocation : allocations) {
    if (allocation)
      allocation->Release();
    allocation = nullptr;
  }
}

// Completion fence for a descriptor-arena range that graphics work may still
// reference: the next value the graphics queue will signal. Every signal
// pre-increments nextFenceValue, so a command list recording right now is
// submitted at or before that signal. Releasing with a null fence instead
// quarantines the range forever, which leaked every table on each renderer
// backend toggle until the 2048-entry sampler heap ran out.
static RIDescriptorArenaFence ri_d3d12_graphics_retire_fence(RIDevice *device) {
  const RIQueue &queue = device->queues[RI_QUEUE_GRAPHICS];
  return {queue.d3d12.fence, queue.d3d12.nextFenceValue + 1};
}

static void ri_d3d12_descriptor_identity(const RIDescriptor &descriptor,
                                         ID3D12Resource **resource,
                                         D3D12MA::Allocation **allocation) {
  *resource = nullptr;
  *allocation = nullptr;
  switch (static_cast<RIDescriptorType_e>(descriptor.type)) {
  case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
  case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    *resource = descriptor.payload.buffer.nativeResource;
    *allocation = descriptor.payload.buffer.allocation;
    break;
  case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
  case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    *resource = descriptor.payload.texture.nativeResource;
    *allocation = descriptor.payload.texture.allocation;
    break;
  case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
    *resource = descriptor.payload.accel.nativeResource;
    *allocation = descriptor.payload.accel.allocation;
    break;
  default:
    break;
  }
}

static D3D12_CPU_DESCRIPTOR_HANDLE
ri_d3d12_cpu(ID3D12DescriptorHeap *heap, uint32_t index, uint32_t stride) {
  D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += SIZE_T(index) * stride;
  return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE
ri_d3d12_gpu(ID3D12DescriptorHeap *heap, uint32_t index, uint32_t stride) {
  D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
  h.ptr += UINT64(index) * stride;
  return h;
}

struct RID3D12BufferShape {
  bool raw = false;
  uint32_t stride = 0;
};

static bool ri_d3d12_resolveBufferShape(
    const RIProgram::ShaderResourceReflection *reflection,
    const RIDescriptorBufferPayload &payload, RID3D12BufferShape &shape) {
  if (!reflection) {
    Error("RIProgram: storage-buffer reflection is unavailable\n");
    return false;
  }
  std::string type = reflection->type;
  std::transform(type.begin(), type.end(), type.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  const bool reflectedRaw = type.find("raw") != std::string::npos ||
                            type.find("byteaddress") != std::string::npos;
  const bool reflectedStructured =
      type.find("structured") != std::string::npos || reflection->stride != 0;
  if (reflectedRaw == reflectedStructured ||
      (reflectedStructured && reflection->stride == 0)) {
    Error("RIProgram: storage-buffer reflection has an ambiguous/unsupported "
          "shape\n");
    return false;
  }
  shape.raw = reflectedRaw;
  shape.stride = reflectedStructured ? reflection->stride : 0;

  // Payload shape fields are optional compatibility constraints.  A zero
  // value means "unspecified"; nonzero values must agree with reflection.
  if ((payload.raw && !shape.raw) || (payload.structured && shape.raw) ||
      (payload.stride && payload.stride != shape.stride) ||
      (payload.raw && payload.structured)) {
    Error(
        "RIProgram: storage-buffer payload shape conflicts with reflection\n");
    return false;
  }
  return true;
}

// Depth formats are DSV-only; sampling reads the depth plane through its
// SRV-compatible format (the resource is created typeless for this).
static DXGI_FORMAT ri_d3d12_srv_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_R32_TYPELESS:
    return DXGI_FORMAT_R32_FLOAT;
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24G8_TYPELESS:
    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_R16_TYPELESS:
    return DXGI_FORMAT_R16_UNORM;
  default:
    return format;
  }
}

static bool ri_d3d12_texture_view(D3D12_SHADER_RESOURCE_VIEW_DESC &desc,
                                  const RIDescriptorTexturePayload &texture) {
  auto type = static_cast<RITextureViewType_e>(texture.viewType);
  desc = {};
  desc.Format =
      ri_d3d12_srv_format(static_cast<DXGI_FORMAT>(texture.nativeFormat));
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  // The mirror of the remap in ri_d3d12_texture_uav below: Vulkan has one image
  // view for both access kinds, so engine code also binds storage-typed views
  // as sampled (the Hi-Z pyramid reads mip N-1 through the same view it wrote).
  // The view type only picks the SRV dimension here; whether the texture may be
  // sampled at all is decided by its RI_USAGE_SHADER_RESOURCE bit at creation,
  // which CreateShaderResourceView enforces.
  switch (type) {
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D:
    type = RI_VIEWTYPE_SHADER_RESOURCE_1D;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY:
    type = RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D:
    type = RI_VIEWTYPE_SHADER_RESOURCE_2D;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY:
    type = RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D:
    type = RI_VIEWTYPE_SHADER_RESOURCE_3D;
    break;
  default:
    break;
  }
  switch (type) {
  case RI_VIEWTYPE_SHADER_RESOURCE_1D:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
    desc.Texture1D.MostDetailedMip = texture.baseMip;
    desc.Texture1D.MipLevels = texture.mipNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    desc.Texture1DArray.MostDetailedMip = texture.baseMip;
    desc.Texture1DArray.MipLevels = texture.mipNum;
    desc.Texture1DArray.FirstArraySlice = texture.baseLayer;
    desc.Texture1DArray.ArraySize = texture.layerNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = texture.baseMip;
    desc.Texture2D.MipLevels = texture.mipNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MostDetailedMip = texture.baseMip;
    desc.Texture2DArray.MipLevels = texture.mipNum;
    desc.Texture2DArray.FirstArraySlice = texture.baseLayer;
    desc.Texture2DArray.ArraySize = texture.layerNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube.MostDetailedMip = texture.baseMip;
    desc.TextureCube.MipLevels = texture.mipNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    desc.TextureCubeArray.MostDetailedMip = texture.baseMip;
    desc.TextureCubeArray.MipLevels = texture.mipNum;
    desc.TextureCubeArray.First2DArrayFace = texture.baseLayer;
    desc.TextureCubeArray.NumCubes = texture.layerNum / 6;
    return texture.layerNum >= 6 && texture.layerNum % 6 == 0;
  case RI_VIEWTYPE_SHADER_RESOURCE_3D:
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MostDetailedMip = texture.baseMip;
    desc.Texture3D.MipLevels = texture.mipNum;
    return true;
  default:
    return false;
  }
}

static bool ri_d3d12_texture_uav(D3D12_UNORDERED_ACCESS_VIEW_DESC &desc,
                                 const RIDescriptorTexturePayload &texture,
                                 ID3D12Resource *resource) {
  desc = {};
  desc.Format = static_cast<DXGI_FORMAT>(texture.nativeFormat);
  // Vulkan has a single image-view type for both sampled and storage access, so
  // shared engine code routinely creates one SHADER_RESOURCE_* view and binds
  // it as a storage image. Accept those here rather than making every such
  // texture carry a second view: the view type only selects the UAV dimension,
  // which is unambiguous, and what actually authorizes a UAV is the texture's
  // ALLOW_UNORDERED_ACCESS flag (set from RI_USAGE_SHADER_RESOURCE_STORAGE at
  // creation). Check that flag directly, so a texture that is genuinely not
  // storage-capable is still rejected. Cube views are deliberately not
  // remapped: a cube UAV has to be declared as a 2D array, so let it fail
  // loudly.
  RITextureViewType_e viewType =
      static_cast<RITextureViewType_e>(texture.viewType);
  const RITextureViewType_e requestedViewType = viewType;
  switch (viewType) {
  case RI_VIEWTYPE_SHADER_RESOURCE_1D:
    viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY:
    viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D:
    viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY:
    viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY;
    break;
  case RI_VIEWTYPE_SHADER_RESOURCE_3D:
    viewType = RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D;
    break;
  default:
    break;
  }
  if (viewType != requestedViewType &&
      (!resource || !(resource->GetDesc().Flags &
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
    return false;
  switch (viewType) {
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D:
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
    desc.Texture1D.MipSlice = texture.baseMip;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY:
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
    desc.Texture1DArray.MipSlice = texture.baseMip;
    desc.Texture1DArray.FirstArraySlice = texture.baseLayer;
    desc.Texture1DArray.ArraySize = texture.layerNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D:
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = texture.baseMip;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY:
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = texture.baseMip;
    desc.Texture2DArray.FirstArraySlice = texture.baseLayer;
    desc.Texture2DArray.ArraySize = texture.layerNum;
    return true;
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D: {
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MipSlice = texture.baseMip;
    desc.Texture3D.FirstWSlice = 0;
    if (!resource || texture.baseLayer != 0 || texture.layerNum != 1)
      return false;
    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (texture.baseMip >= rd.MipLevels || texture.baseMip >= 32)
      return false;
    desc.Texture3D.WSize =
        std::max<UINT>(1, rd.DepthOrArraySize >> texture.baseMip);
    return true;
  }
  default:
    return false;
  }
}
#endif

static const char *ri_stageName(uint8_t stage) {
  static constexpr const char *names[] = {
      "vertex",     "fragment", "compute",      "raygen",  "miss",
      "closesthit", "anyhit",   "intersection", "callable"};
  return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : nullptr;
}

static RIShaderArtifactFormat
ri_detectShaderFormat(std::span<const char> data) {
  if (data.size() >= 4) {
    uint32_t magic = 0;
    memcpy(&magic, data.data(), sizeof(magic));
    if (magic == 0x07230203u || magic == 0x03022307u)
      return RIShaderArtifactFormat::Spirv;
    if (memcmp(data.data(), "DXBC", 4) == 0)
      return RIShaderArtifactFormat::Dxil;
  }
  return RIShaderArtifactFormat::Unknown;
}

static const rapidjson::Value *ri_member(const rapidjson::Value &v,
                                         const char *name);

// Value of a `key=` line in the sidecar envelope, or an empty view when the key
// is absent or carries nothing.
static std::string_view ri_metadataValue(std::string_view text,
                                         const char *key) {
  const size_t pos = text.find(key);
  if (pos == std::string_view::npos)
    return {};
  const size_t start = pos + strlen(key);
  const size_t end = text.find('\n', start);
  if (end == std::string_view::npos)
    return {};
  return text.substr(start, end - start);
}

static RIShaderArtifactFormat
ri_readShaderMetadata(cFileSearcher *searcher, const tString &artifactName,
                      size_t artifactSize,
                      std::string *reflectionJson = nullptr,
                      RIShaderArtifactMeta *outMeta = nullptr) {
  const tString metadataName = artifactName + ".meta";
  const tWString metadataPath = searcher->GetFilePath(metadataName);
  // Raw SPIR-V is an existing deployed resource format.  Its sidecar is
  // optional; generated Slang artifacts get the stronger envelope below.
  if (metadataPath == _W(""))
    return RIShaderArtifactFormat::Spirv;
  const unsigned long metadataSize = cPlatform::GetFileSize(metadataPath);
  std::vector<char> metadata(metadataSize);
  if (metadataSize == 0 || !cPlatform::CopyFileToBuffer(
                               metadataPath, metadata.data(), metadataSize)) {
    FatalError("RIProgram: unable to read shader metadata '%s'\n",
               metadataName.c_str());
  }

  // cmd.exe's `echo` writes CRLF sidecars.  Treat the artifact envelope as a
  // line-oriented format rather than requiring Unix newlines from every
  // producer.
  std::string text(metadata.data(), metadata.size());
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  const std::string magic = std::string(RI_SHADER_ARTIFACT_MAGIC) + "\n";
  const std::string version =
      "version=" + std::to_string(RI_SHADER_ARTIFACT_VERSION) + "\n";
  if (text.substr(0, magic.size()) != magic ||
      text.find(version, magic.size()) != magic.size()) {
    FatalError("RIProgram: invalid shader metadata magic/version for '%s'\n",
               artifactName.c_str());
  }

  RIShaderArtifactFormat format = RIShaderArtifactFormat::Unknown;
  const std::string spirv = "format=spirv\n";
  const std::string dxil = "format=dxil\n";
  if (text.find(spirv) != std::string_view::npos)
    format = RIShaderArtifactFormat::Spirv;
  else if (text.find(dxil) != std::string_view::npos)
    format = RIShaderArtifactFormat::Dxil;
  else
    FatalError("RIProgram: shader metadata has no supported format for '%s'\n",
               artifactName.c_str());

  if (format == RIShaderArtifactFormat::Spirv && artifactSize < 4)
    FatalError("RIProgram: truncated SPIR-V artifact '%s'\n",
               artifactName.c_str());

  if (format == RIShaderArtifactFormat::Dxil) {
    for (const char *key : {"source=", "stage=", "entry=", "reflection="}) {
      if (text.find(key) == std::string_view::npos)
        FatalError("RIProgram: DXIL metadata missing '%s' for '%s'\n", key,
                   artifactName.c_str());
    }
    // The sidecar's stage/entry map is authoritative. Slang omits "stage" from
    // its reflection document for ray-tracing entry points, so for a DXIL
    // library this map is the only record of which stage each entry declared.
    RIShaderArtifactMeta meta;
    if (!ri_parseShaderArtifactEntries(ri_metadataValue(text, "entry="),
                                       ri_metadataValue(text, "stage="), meta))
      FatalError(
          "RIProgram: DXIL metadata has an unusable stage/entry map for '%s'\n",
          artifactName.c_str());

    const size_t reflectionPos = text.find("reflection=");
    const size_t reflectionEnd = text.find('\n', reflectionPos);
    if (reflectionEnd == reflectionPos + strlen("reflection=") ||
        reflectionEnd == std::string_view::npos)
      FatalError(
          "RIProgram: DXIL metadata has no reflection linkage for '%s'\n",
          artifactName.c_str());
    const tString reflectionName(
        text.substr(reflectionPos + strlen("reflection="),
                    reflectionEnd - reflectionPos - strlen("reflection=")));
    const tWString reflectionPath = searcher->GetFilePath(reflectionName);
    if (reflectionPath == _W(""))
      FatalError("RIProgram: missing DXIL reflection '%s' for '%s'\n",
                 reflectionName.c_str(), artifactName.c_str());
    const unsigned long reflectionSize = cPlatform::GetFileSize(reflectionPath);
    if (reflectionSize == 0)
      FatalError("RIProgram: empty DXIL reflection '%s'\n",
                 reflectionName.c_str());
    std::vector<char> reflection(reflectionSize);
    if (!cPlatform::CopyFileToBuffer(reflectionPath, reflection.data(),
                                     reflectionSize))
      FatalError("RIProgram: could not read DXIL reflection '%s'\n",
                 reflectionName.c_str());
    if (reflectionJson)
      reflectionJson->assign(reflection.data(), reflection.size());
    const std::string_view reflectionText(reflection.data(), reflection.size());
    // Slang's reflection-v1 document is the source of truth for the D3D12
    // binding contract. Resource/register/space/layout and semantic fields are
    // present only when that artifact declares them, so validate the required
    // entry-point envelope here rather than rejecting resource-free shaders.
    for (const char *key : {"\"entryPoints\"", "\"name\""}) {
      if (reflectionText.find(key) == std::string_view::npos)
        FatalError("RIProgram: DXIL reflection '%s' missing '%s'\n",
                   reflectionName.c_str(), key);
    }
    if (!meta.isLibrary()) {
      // Single-stage artifacts must still carry a reflected stage; only the
      // ray-tracing entry points in a library are allowed to omit it.
      if (reflectionText.find("\"stage\"") == std::string_view::npos)
        FatalError("RIProgram: DXIL reflection '%s' missing '\"stage\"'\n",
                   reflectionName.c_str());
    } else {
      // A library's reflection may legitimately omit "stage", so instead
      // require that it actually describes every entry the sidecar declares.
      // This catches a sidecar and reflection that have drifted apart, which
      // the blanket "stage" substring check never did.
      rapidjson::Document document;
      document.Parse(reflection.data(), reflection.size());
      const rapidjson::Value *entries =
          document.HasParseError() || !document.IsObject()
              ? nullptr
              : ri_member(document, RI_SHADER_REFLECTION_ENTRY_POINTS_KEY);
      if (!entries || !entries->IsArray())
        FatalError("RIProgram: DXIL reflection '%s' has no entryPoints array\n",
                   reflectionName.c_str());
      for (const auto &declared : meta.entries) {
        const bool present = std::any_of(
            entries->Begin(), entries->End(), [&](const rapidjson::Value &e) {
              const auto *name = ri_member(e, "name");
              return name && name->IsString() &&
                     declared.entry == name->GetString();
            });
        if (!present)
          FatalError("RIProgram: DXIL reflection '%s' does not describe entry "
                     "'%s' declared by '%s'\n",
                     reflectionName.c_str(), declared.entry.c_str(),
                     metadataName.c_str());
      }
    }
    if (outMeta)
      *outMeta = std::move(meta);
  }
  return format;
}

static const rapidjson::Value *ri_member(const rapidjson::Value &v,
                                         const char *name) {
  return v.IsObject() && v.HasMember(name) ? &v[name] : nullptr;
}

static uint32_t ri_uint(const rapidjson::Value *v, uint32_t fallback = 0) {
  return v && v->IsUint() ? v->GetUint() : fallback;
}

static bool ri_isArrayCount(const rapidjson::Value *v, uint32_t &count) {
  if (!v) {
    count = 1;
    return false;
  }
  if (v->IsUint()) {
    count = v->GetUint();
    return false;
  }
  if (v->IsString() && std::string_view(v->GetString()) == "unbounded") {
    count = 0;
    return true;
  }
  count = 1;
  return false;
}

static void ri_find_layout(const rapidjson::Value &v, uint32_t &stride,
                           uint32_t &size) {
  if (v.IsArray()) {
    for (const auto &item : v.GetArray())
      ri_find_layout(item, stride, size);
    return;
  }
  if (!v.IsObject())
    return;
  if (const auto *binding = ri_member(v, "binding")) {
    size = std::max(size, ri_uint(ri_member(*binding, "size")));
    stride = std::max(stride, ri_uint(ri_member(*binding, "elementStride")));
  }
  if (const auto *sizes = ri_member(v, "sizes"); sizes && sizes->IsArray())
    for (const auto &s : sizes->GetArray()) {
      const auto *kind = ri_member(s, "kind");
      const auto *value = ri_member(s, "value");
      if (kind && kind->IsString() && value && value->IsUint() &&
          (std::string_view(kind->GetString()) == "structuredBuffer" ||
           std::string_view(kind->GetString()) == "uniform"))
        stride = std::max(stride, value->GetUint());
    }
  for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
    if (it->value.IsObject() || it->value.IsArray())
      ri_find_layout(it->value, stride, size);
}

static bool ri_find_resource_shape(const rapidjson::Value &v,
                                   std::string &shape) {
  if (v.IsArray()) {
    for (const auto &item : v.GetArray())
      if (ri_find_resource_shape(item, shape))
        return true;
    return false;
  }
  if (!v.IsObject())
    return false;
  if (const auto *baseShape = ri_member(v, "baseShape");
      baseShape && baseShape->IsString()) {
    shape = baseShape->GetString();
    return true;
  }
  for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
    if ((it->value.IsObject() || it->value.IsArray()) &&
        ri_find_resource_shape(it->value, shape))
      return true;
  return false;
}

static const rapidjson::Value *
ri_find_named_parameter(const rapidjson::Value *parameters,
                        std::string_view name) {
  if (!parameters || !parameters->IsArray())
    return nullptr;
  for (const auto &parameter : parameters->GetArray()) {
    const auto *parameterName = ri_member(parameter, "name");
    if (parameterName && parameterName->IsString() &&
        name == parameterName->GetString())
      return &parameter;
  }
  return nullptr;
}

static void
ri_find_push_constant(const rapidjson::Value &v,
                      RIProgram::ShaderPushConstantReflection &out) {
  if (v.IsArray()) {
    for (const auto &item : v.GetArray())
      ri_find_push_constant(item, out);
    return;
  }
  if (!v.IsObject())
    return;
  const auto *kind = ri_member(v, "kind");
  const auto *name = ri_member(v, "name");
  const auto *binding = ri_member(v, "binding");
  const auto *bindingKind = binding ? ri_member(*binding, "kind") : nullptr;
  const bool explicitPush =
      (kind && kind->IsString() &&
       std::string_view(kind->GetString()) == "pushConstant") ||
      (bindingKind && bindingKind->IsString() &&
       std::string_view(bindingKind->GetString()) == "pushConstant");
  const bool productionPush =
      name && name->IsString() &&
      std::string_view(name->GetString()) == "gPushConstants" && bindingKind &&
      bindingKind->IsString() &&
      std::string_view(bindingKind->GetString()) == "constantBuffer";
  if (explicitPush || productionPush) {
    out.present = true;
    if (binding) {
      out.offset = ri_uint(ri_member(*binding, "offset"), out.offset);
      out.size = ri_uint(ri_member(*binding, "size"), out.size);
      // Slang's reflection JSON names the register "index" (as it does for
      // every other resource); without it a block at b1+ would be placed at
      // b0 and overlap a real constant buffer there.
      out.registerIndex =
          ri_uint(ri_member(*binding, "index"), out.registerIndex);
      out.registerIndex =
          ri_uint(ri_member(*binding, "register"), out.registerIndex);
      out.registerIndex =
          ri_uint(ri_member(*binding, "registerIndex"), out.registerIndex);
      out.registerSpace =
          ri_uint(ri_member(*binding, "space"), out.registerSpace);
    }
    uint32_t ignoredStride = 0;
    ri_find_layout(v, ignoredStride, out.size);
  }
  for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
    if (it->value.IsObject() || it->value.IsArray()) {
      if (it->value.IsArray())
        for (const auto &item : it->value.GetArray())
          ri_find_push_constant(item, out);
      else
        ri_find_push_constant(it->value, out);
    }
}

static void ri_find_vertex_inputs(
    const rapidjson::Value &v,
    std::vector<RIProgram::ShaderVertexInputReflection> &out) {
  if (v.IsArray()) {
    for (const auto &item : v.GetArray())
      ri_find_vertex_inputs(item, out);
    return;
  }
  if (!v.IsObject())
    return;
  const auto *semantic = ri_member(v, "semanticName");
  const auto *binding = ri_member(v, "binding");
  const auto *kind = binding ? ri_member(*binding, "kind") : nullptr;
  const auto *index = binding ? ri_member(*binding, "index") : nullptr;
  if (semantic && semantic->IsString() && kind && kind->IsString() &&
      std::string_view(kind->GetString()) == "varyingInput" && index &&
      index->IsUint()) {
    RIProgram::ShaderVertexInputReflection input;
    input.semanticName = semantic->GetString();
    input.semanticIndex = ri_uint(ri_member(v, "semanticIndex"));
    input.location = index->GetUint();
    const bool duplicate =
        std::any_of(out.begin(), out.end(), [&](const auto &item) {
          return item.location == input.location;
        });
    if (!duplicate)
      out.push_back(std::move(input));
  }
  for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
    if (it->value.IsObject() || it->value.IsArray())
      ri_find_vertex_inputs(it->value, out);
}

static std::shared_ptr<const RIProgram::ShaderReflection>
ri_parseShaderReflection(const std::string &json, const char *requestedEntry,
                         const char *requestedStage = nullptr) {
  auto document = std::make_shared<rapidjson::Document>();
  document->Parse(json.data(), json.size());
  if (document->HasParseError() || !document->IsObject())
    FatalError("RIProgram: invalid reflection-v1 JSON\n");
  const auto *entries = ri_member(*document, "entryPoints");
  if (!entries || !entries->IsArray())
    FatalError("RIProgram: reflection has no entryPoints\n");
  const rapidjson::Value *entry = nullptr;
  for (const auto &candidate : entries->GetArray()) {
    const auto *name = ri_member(candidate, "name");
    const auto *stage = ri_member(candidate, "stage");
    // Slang omits "stage" on ray-tracing entry points, so an entry that does
    // not state one is matched on name alone -- names are unique within a
    // document, so this stays unambiguous. An entry that *does* state a stage
    // must still match the requested one exactly.
    const bool stageStated = stage && stage->IsString();
    if (!name || !name->IsString() ||
        (requestedEntry && name->GetString() != std::string(requestedEntry)) ||
        (requestedStage && stageStated &&
         stage->GetString() != std::string(requestedStage)))
      continue;
    if (entry)
      FatalError(
          "RIProgram: requested entry point is ambiguous in reflection\n");
    entry = &candidate;
  }
  if (!entry)
    FatalError("RIProgram: requested entry point is absent from reflection\n");

  auto result = std::make_shared<RIProgram::ShaderReflection>();
  result->json = std::make_shared<const std::string>(json);
  result->entryPoint = ri_member(*entry, "name") && (*entry)["name"].IsString()
                           ? (*entry)["name"].GetString()
                           : "";
  result->stage = ri_member(*entry, "stage") && (*entry)["stage"].IsString()
                      ? (*entry)["stage"].GetString()
                      : "";
  // An entry the reflection left unstaged adopts the caller's stage, which
  // initialize() has already cross-checked against the artifact's sidecar.
  if (result->stage.empty() && requestedStage)
    result->stage = requestedStage;
  if (result->stage.empty())
    FatalError("RIProgram: reflection entry point has no stage\n");
  ri_find_push_constant(*entry, result->pushConstants);
  if (!result->pushConstants.present) {
    if (const auto *globalScope = ri_member(*document, "globalScope"))
      ri_find_push_constant(*globalScope, result->pushConstants);
  }
  const auto *scope = ri_member(*entry, "scope");
  const auto *bindings = ri_member(*entry, "bindings");
  if ((!bindings || !bindings->IsArray()) && scope)
    bindings = ri_member(*scope, "bindings");
  const auto *parameters = ri_member(*entry, "parameters");
  if ((!parameters || !parameters->IsArray()) && scope)
    parameters = ri_member(*scope, "parameters");
  if (!parameters || !parameters->IsArray())
    parameters = ri_member(*document, "parameters");
  if (result->stage == "vertex")
    ri_find_vertex_inputs(*entry, result->vertexInputs);
  // Slang reports entry-point usage under entryPoints[].bindings, while the
  // byte range for a global gPushConstants block remains in top-level
  // parameters. Merge that layout even when the entry binding already marked
  // the block present but carried no size.
  if (parameters && parameters->IsArray() &&
      (!result->pushConstants.present || result->pushConstants.size == 0))
    ri_find_push_constant(*parameters, result->pushConstants);
  if (!result->pushConstants.present || result->pushConstants.size == 0) {
    if (const auto *globalParameters = ri_member(*document, "parameters");
        globalParameters && globalParameters->IsArray())
      ri_find_push_constant(*globalParameters, result->pushConstants);
  }
  if (!result->pushConstants.present || result->pushConstants.size == 0) {
    if (const auto *globalScope = ri_member(*document, "globalScope"))
      if (const auto *globalParameters = ri_member(*globalScope, "parameters");
          globalParameters && globalParameters->IsArray())
        ri_find_push_constant(*globalParameters, result->pushConstants);
  }
  if (bindings && bindings->IsArray())
    for (const auto &binding : bindings->GetArray()) {
      RIProgram::ShaderResourceReflection resource;
      const auto *name = ri_member(binding, "name");
      if (!name || !name->IsString())
        continue;
      resource.name = name->GetString();
      // The production Slang convention lowers this block as b0 in DXIL, but
      // RI exposes it through root constants. It must not also occupy the CBV
      // descriptor table or the generated root signature contains overlapping
      // bindings.
      if (resource.name == "gPushConstants" && result->pushConstants.present)
        continue;
      resource.stage = result->stage;
      const auto *b = ri_member(binding, "binding");
      const auto *kind = b ? ri_member(*b, "kind") : nullptr;
      if (kind && kind->IsString()) {
        const std::string_view k(kind->GetString());
        if (k == "constantBuffer")
          resource.registerClass = RIProgram::ShaderRegisterClass::CBV;
        else if (k == "shaderResource")
          resource.registerClass = RIProgram::ShaderRegisterClass::SRV;
        else if (k == "unorderedAccess")
          resource.registerClass = RIProgram::ShaderRegisterClass::UAV;
        else if (k == "samplerState")
          resource.registerClass = RIProgram::ShaderRegisterClass::Sampler;
      }
      resource.registerIndex = ri_uint(b ? ri_member(*b, "index") : nullptr);
      resource.registerSpace =
          ri_uint(b ? (ri_member(*b, "space") ? ri_member(*b, "space")
                                              : ri_member(*b, "registerSpace"))
                    : nullptr);
      resource.unbounded = ri_isArrayCount(b ? ri_member(*b, "count") : nullptr,
                                           resource.arrayCount);
      resource.used = ri_uint(b ? ri_member(*b, "used") : nullptr, 1) != 0;
      if (const auto *format = ri_member(binding, "format");
          format && format->IsString())
        resource.format = format->GetString();
      const rapidjson::Value *parameter =
          ri_find_named_parameter(parameters, resource.name);
      if (!parameter)
        parameter = ri_find_named_parameter(ri_member(*document, "parameters"),
                                            resource.name);
      if (!parameter)
        if (const auto *globalScope = ri_member(*document, "globalScope"))
          parameter = ri_find_named_parameter(
              ri_member(*globalScope, "parameters"), resource.name);
      if (parameter) {
        const auto *type = ri_member(*parameter, "type");
        if (type && type->IsObject()) {
          ri_find_resource_shape(*type, resource.type);
          if (resource.type.empty())
            for (const char *key : {"kind", "scalarType"})
              if (const auto *value = ri_member(*type, key);
                  value && value->IsString()) {
                resource.type = value->GetString();
                break;
              }
          if (const auto *format = ri_member(*type, "format");
              format && format->IsString())
            resource.format = format->GetString();
          ri_find_layout(*type, resource.stride, resource.size);
        }
      }
      result->resources.push_back(std::move(resource));
    }
  return result;
}

#if (DEVICE_IMPL_D3D12)
// D3D12 has no separate ray-tracing bind point: a DXR dispatch consumes the
// COMPUTE root signature and its root arguments, so ray tracing shares the
// compute bind path and only GRAPHICS takes the graphics one.
static bool ri_d3d12BindPointSupported(VkPipelineBindPoint bindPoint) {
  return bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ||
         bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE ||
         bindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
}

static bool ri_d3d12ComputeRootPath(VkPipelineBindPoint bindPoint) {
  return bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS;
}

static void ri_log_d3d12_blob(const char *prefix, ID3DBlob *blob) {
  if (blob && blob->GetBufferPointer() && blob->GetBufferSize())
    Log("RI: %s: %.*s\n", prefix, static_cast<int>(blob->GetBufferSize()),
        static_cast<const char *>(blob->GetBufferPointer()));
}
#endif

// Map the backend-neutral RIDescriptorType_e to VkDescriptorType for descriptor
// writes. The engine uses separate sampled-image + sampler (no combined).
static VkDescriptorType ri_vk_BindlessDescriptorType(uint8_t t) {
  switch ((enum RIDescriptorType_e)t) {
  case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case RI_DESCRIPTOR_TYPE_SAMPLER:
    return VK_DESCRIPTOR_TYPE_SAMPLER;
  case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
    return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }
  FatalError("Invalid RIDescriptor type: %u\n", static_cast<unsigned>(t));
  return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

static bool IsValidRIDescriptorType(uint8_t t) {
  return t <= RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
}

static void vkDescriptorSetAlloc(struct RIDevice *device,
                                 struct RIDescriptorSetAlloc *alloc) {
  struct RIProgram::DescriptorSetSlot *programDescriptor =
      hpl_container_of(alloc, &RIProgram::DescriptorSetSlot::alloc);
  VkDescriptorPoolSize descriptorPoolSize[16] = {};
  size_t descriptorPoolLen = 0;
  if (programDescriptor->samplerMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_SAMPLER,
        (uint32_t)programDescriptor->samplerMaxNum * DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->combinedImageSamplerMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        (uint32_t)programDescriptor->combinedImageSamplerMaxNum *
            DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->constantBufferMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] =
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             (uint32_t)programDescriptor->constantBufferMaxNum *
                                 DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->dynamicConstantBufferMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        (uint32_t)programDescriptor->dynamicConstantBufferMaxNum *
            DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->textureMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        (uint32_t)programDescriptor->textureMaxNum * DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->storageTextureMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] =
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             (uint32_t)programDescriptor->storageTextureMaxNum *
                                 DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->bufferMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        (uint32_t)programDescriptor->bufferMaxNum * DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->storageBufferMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
        (uint32_t)programDescriptor->storageBufferMaxNum * DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->structuredBufferMaxNum > 0 ||
      programDescriptor->storageStructuredBufferMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        (uint32_t)programDescriptor->structuredBufferMaxNum *
                DESCRIPTOR_MAX_SIZE +
            (uint32_t)programDescriptor->storageStructuredBufferMaxNum *
                DESCRIPTOR_MAX_SIZE};
  if (programDescriptor->accelerationStructureMaxNum > 0)
    descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        (uint32_t)programDescriptor->accelerationStructureMaxNum *
            DESCRIPTOR_MAX_SIZE};
  assert(descriptorPoolLen < ARRAY_COUNT(descriptorPoolSize));
  const VkDescriptorPoolCreateInfo info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      NULL,
      VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      DESCRIPTOR_MAX_SIZE,
      (uint32_t)descriptorPoolLen,
      descriptorPoolSize};
  struct RIDescriptorPoolAllocSlot poolSlot = {};
  VK_WrapResult(vkCreateDescriptorPool(device->vk.device, &info, NULL,
                                       &poolSlot.vk.handle));
  arrpush(alloc->pools, poolSlot);
  for (size_t i = 0; i < DESCRIPTOR_MAX_SIZE; i++) {
    struct RIDescriptorSetSlot *slot = allocDescriptorSetSlot(alloc);
    VkDescriptorSetAllocateInfo info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.pNext = NULL;
    info.descriptorPool = poolSlot.vk.handle;
    info.descriptorSetCount = 1;
    assert(programDescriptor->vk.setLayout != VK_NULL_HANDLE);
    info.pSetLayouts = &programDescriptor->vk.setLayout;
    VK_WrapResult(
        vkAllocateDescriptorSets(device->vk.device, &info, &slot->vk.handle));
    arrpush(alloc->reservedSlots, slot);
  }
}

#if (DEVICE_IMPL_VULKAN)
namespace {

// Owns every sub-struct a VkGraphicsPipelineCreateInfo points at, so the
// pointer chain stays valid for as long as the scratch lives. This is the same
// holder pattern call sites used to write by hand -- the point of the RI desc
// is that it now exists exactly once, here, instead of at every call site.
struct RIVkGraphicsPipelineScratch {
  VkVertexInputBindingDescription bindings[RI_MAX_VERTEX_BINDINGS]{};
  VkVertexInputAttributeDescription attributes[RI_MAX_VERTEX_ATTRIBUTES]{};
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPipelineRasterizationStateCreateInfo rasterization{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  VkPipelineDepthStencilStateCreateInfo depthStencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend[RI_MAX_COLOR_ATTACHMENTS]{};
  VkPipelineColorBlendStateCreateInfo colorBlend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  VkFormat colorFormats[RI_MAX_COLOR_ATTACHMENTS]{};
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkGraphicsPipelineCreateInfo createInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

  RIVkGraphicsPipelineScratch(const RIVkGraphicsPipelineScratch &) = delete;
  RIVkGraphicsPipelineScratch &
  operator=(const RIVkGraphicsPipelineScratch &) = delete;
  RIVkGraphicsPipelineScratch() = default;
};

VkStencilOpState ri_vk_stencilFace(const RIStencilFaceDesc &face,
                                   uint32_t reference) {
  VkStencilOpState out = {};
  out.failOp = ri_vk_RIStencilOpToVK(face.failOp);
  out.passOp = ri_vk_RIStencilOpToVK(face.passOp);
  out.depthFailOp = ri_vk_RIStencilOpToVK(face.depthFailOp);
  out.compareOp = ri_vk_RICompareOpToVK(face.compareFunc);
  out.compareMask = face.compareMask;
  out.writeMask = face.writeMask;
  // Vulkan carries a reference per face; the RI desc has one for both, since
  // D3D12's OMSetStencilRef is not per-face.
  out.reference = reference;
  return out;
}

void ri_vk_buildGraphicsPipeline(const RIGraphicsPipelineDesc &desc,
                                 RIVkGraphicsPipelineScratch &out) {
  assert(desc.vertexInput.bindingCount <= RI_MAX_VERTEX_BINDINGS);
  assert(desc.vertexInput.attributeCount <= RI_MAX_VERTEX_ATTRIBUTES);
  assert(desc.blendCount <= RI_MAX_COLOR_ATTACHMENTS);
  assert(desc.renderTarget.colorCount <= RI_MAX_COLOR_ATTACHMENTS);
  // A colour attachment without a matching blend state (or vice versa) is a
  // pipeline that cannot be created on D3D12 and is almost always a call-site
  // slip on Vulkan. Catch it here rather than in a backend validation layer.
  assert(desc.blendCount == desc.renderTarget.colorCount &&
         "blendCount must match renderTarget.colorCount");

  for (uint32_t i = 0; i < desc.vertexInput.bindingCount; ++i) {
    const RIVertexBindingDesc &b = desc.vertexInput.bindings[i];
    out.bindings[i].binding = b.binding;
    out.bindings[i].stride = b.stride;
    out.bindings[i].inputRate = ri_vk_RIVertexInputRateToVK(b.inputRate);
  }
  for (uint32_t i = 0; i < desc.vertexInput.attributeCount; ++i) {
    const RIVertexAttributeDesc &a = desc.vertexInput.attributes[i];
    out.attributes[i].location = a.location;
    out.attributes[i].binding = a.binding;
    out.attributes[i].offset = a.offset;
    out.attributes[i].format = RIFormatToVK(a.format);
  }
  out.vertexInput.vertexBindingDescriptionCount = desc.vertexInput.bindingCount;
  out.vertexInput.pVertexBindingDescriptions =
      desc.vertexInput.bindingCount ? out.bindings : nullptr;
  out.vertexInput.vertexAttributeDescriptionCount =
      desc.vertexInput.attributeCount;
  out.vertexInput.pVertexAttributeDescriptions =
      desc.vertexInput.attributeCount ? out.attributes : nullptr;

  out.inputAssembly.topology = ri_vk_RITopologyToVK(desc.topology);
  out.inputAssembly.primitiveRestartEnable =
      desc.primitiveRestart ? VK_TRUE : VK_FALSE;

  const RIRasterizationDesc &rs = desc.raster;
  out.rasterization.depthClampEnable = rs.depthClamp ? VK_TRUE : VK_FALSE;
  out.rasterization.rasterizerDiscardEnable = VK_FALSE;
  out.rasterization.polygonMode = ri_vk_RIPolygonModeToVK(rs.polygonMode);
  out.rasterization.cullMode = ri_vk_RICullModeToVK(rs.cullMode);
  out.rasterization.frontFace = ri_vk_RIFrontFaceToVK(rs.frontFace);
  out.rasterization.depthBiasEnable = rs.depthBiasEnable ? VK_TRUE : VK_FALSE;
  out.rasterization.depthBiasConstantFactor = rs.depthBiasConstant;
  out.rasterization.depthBiasClamp = rs.depthBiasClamp;
  out.rasterization.depthBiasSlopeFactor = rs.depthBiasSlope;
  out.rasterization.lineWidth = rs.lineWidth;

  out.dynamic.dynamicStateCount = static_cast<uint32_t>(
      sizeof(out.dynamicStates) / sizeof(out.dynamicStates[0]));
  out.dynamic.pDynamicStates = out.dynamicStates;
  out.viewport.viewportCount = 1;
  out.viewport.scissorCount = 1;

  out.multisample.rasterizationSamples =
      ri_vk_RISampleCountToVK(desc.sampleCount);
  out.multisample.alphaToCoverageEnable =
      desc.alphaToCoverage ? VK_TRUE : VK_FALSE;

  const RIDepthStencilDesc &ds = desc.depthStencil;
  out.depthStencil.depthTestEnable = ds.depthTest ? VK_TRUE : VK_FALSE;
  out.depthStencil.depthWriteEnable = ds.depthWrite ? VK_TRUE : VK_FALSE;
  out.depthStencil.depthCompareOp = ri_vk_RICompareOpToVK(ds.depthCompare);
  out.depthStencil.depthBoundsTestEnable = VK_FALSE;
  out.depthStencil.stencilTestEnable = ds.stencilTest ? VK_TRUE : VK_FALSE;
  out.depthStencil.front = ri_vk_stencilFace(ds.front, ds.stencilReference);
  out.depthStencil.back = ri_vk_stencilFace(ds.back, ds.stencilReference);

  for (uint32_t i = 0; i < desc.blendCount; ++i) {
    const RIBlendAttachmentDesc &b = desc.blend[i];
    out.blend[i].blendEnable = b.blendEnable ? VK_TRUE : VK_FALSE;
    out.blend[i].srcColorBlendFactor = ri_vk_RIBlendFactorToVK(b.srcColor);
    out.blend[i].dstColorBlendFactor = ri_vk_RIBlendFactorToVK(b.dstColor);
    out.blend[i].colorBlendOp = ri_vk_RIBlendOpToVK(b.colorOp);
    out.blend[i].srcAlphaBlendFactor = ri_vk_RIBlendFactorToVK(b.srcAlpha);
    out.blend[i].dstAlphaBlendFactor = ri_vk_RIBlendFactorToVK(b.dstAlpha);
    out.blend[i].alphaBlendOp = ri_vk_RIBlendOpToVK(b.alphaOp);
    out.blend[i].colorWriteMask = ri_vk_RIColorWriteMaskToVK(b.writeMask);
  }
  out.colorBlend.logicOpEnable = VK_FALSE;
  out.colorBlend.attachmentCount = desc.blendCount;
  out.colorBlend.pAttachments = desc.blendCount ? out.blend : nullptr;

  const RIRenderTargetDesc &rt = desc.renderTarget;
  for (uint32_t i = 0; i < rt.colorCount; ++i)
    out.colorFormats[i] = RIFormatToVK(rt.colorFormats[i]);
  out.rendering.colorAttachmentCount = rt.colorCount;
  out.rendering.pColorAttachmentFormats =
      rt.colorCount ? out.colorFormats : nullptr;
  out.rendering.depthAttachmentFormat = rt.depthFormat == RI_FORMAT_UNKNOWN
                                            ? VK_FORMAT_UNDEFINED
                                            : RIFormatToVK(rt.depthFormat);
  out.rendering.stencilAttachmentFormat = rt.stencilFormat == RI_FORMAT_UNKNOWN
                                              ? VK_FORMAT_UNDEFINED
                                              : RIFormatToVK(rt.stencilFormat);

  out.createInfo.pNext = &out.rendering;
  out.createInfo.pVertexInputState = &out.vertexInput;
  out.createInfo.pInputAssemblyState = &out.inputAssembly;
  out.createInfo.pRasterizationState = &out.rasterization;
  out.createInfo.pDynamicState = &out.dynamic;
  out.createInfo.pViewportState = &out.viewport;
  out.createInfo.pMultisampleState = &out.multisample;
  out.createInfo.pDepthStencilState = &out.depthStencil;
  out.createInfo.pColorBlendState = &out.colorBlend;
}

} // namespace
#endif // DEVICE_IMPL_VULKAN

void RIProgram::bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                             hash_t pipelineHashIn, const char *debugName,
                             const RIGraphicsPipelineDesc &desc) {
  // The full pipeline state participates in the cache key on both backends, so
  // the caller's hash is only a variant tag and two distinct descs cannot
  // collide on it.
  const hash_t pipelineHash = RIHashGraphicsPipelineDesc(pipelineHashIn, desc);
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    bindD3D12Pipeline(device, cmd, pipelineHash, debugName, desc);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  assert(shaderBin[PROGRAM_STAGE_COMPUTE].buf.empty() &&
         "compute-only programs must use bindComputePipeline");
  RIVkGraphicsPipelineScratch scratch;
  ri_vk_buildGraphicsPipeline(desc, scratch);
  VkGraphicsPipelineCreateInfo *pipelineCreateInfo = &scratch.createInfo;
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = pipeline.find(pipelineHash);
  if (it == pipeline.end()) {
    uint32_t numModules = 0;
    VkShaderModule modules[4] = {0};
    VkPipelineShaderStageCreateInfo stageCreateInfo[4] = {};
    if (shaderBin[PROGRAM_STAGE_VERTEX].buf.size() > 0 &&
        shaderBin[PROGRAM_STAGE_FRAGMENT].buf.size() > 0) {
      pipelineCreateInfo->stageCount = 2;
      const VkShaderModuleCreateInfo vertModuleCreateInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          NULL,
          (VkShaderModuleCreateFlags)0,
          (size_t)shaderBin[PROGRAM_STAGE_VERTEX].buf.size(),
          (const uint32_t *)shaderBin[PROGRAM_STAGE_VERTEX].buf.data(),
      };
      vkCreateShaderModule(device->vk.device, &vertModuleCreateInfo, NULL,
                           &modules[numModules]);
      stageCreateInfo[0] = VkPipelineShaderStageCreateInfo{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[0].stage = VK_SHADER_STAGE_VERTEX_BIT,
      stageCreateInfo[0].module = modules[numModules],
      stageCreateInfo[0].pName =
          shaderBin[PROGRAM_STAGE_VERTEX].entryPoint.c_str();
      numModules++;

      const VkShaderModuleCreateInfo fragModuleCreateInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          NULL,
          (VkShaderModuleCreateFlags)0,
          (size_t)shaderBin[PROGRAM_STAGE_FRAGMENT].buf.size(),
          (const uint32_t *)shaderBin[PROGRAM_STAGE_FRAGMENT].buf.data(),
      };
      vkCreateShaderModule(device->vk.device, &fragModuleCreateInfo, NULL,
                           &modules[numModules]);
      stageCreateInfo[1] = VkPipelineShaderStageCreateInfo{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stageCreateInfo[1].module = modules[numModules];
      stageCreateInfo[1].pName =
          shaderBin[PROGRAM_STAGE_FRAGMENT].entryPoint.c_str();
      numModules++;
    } else {
      assert(false && "failed to resolve bin");
    }
    pipelineCreateInfo->pStages = stageCreateInfo;
    pipelineCreateInfo->basePipelineIndex = -1;
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    PipelineSlot slot = {};
    VK_WrapResult(vkCreateGraphicsPipelines(device->vk.device, VK_NULL_HANDLE,
                                            1, pipelineCreateInfo, NULL,
                                            &slot.vk.handle));

    if (vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT debugExt = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &debugExt));
    }
    pipelineHandle = slot.vk.handle;
    pipeline[pipelineHash] = slot;
    for (size_t i = 0; i < numModules; i++) {
      vkDestroyShaderModule(device->vk.device, modules[i], NULL);
    }
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineHandle);
#endif
}

void RIProgram::bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                                    hash_t pipelineHash,
                                    const char *debugName) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    bindD3D12ComputePipeline(device, cmd, pipelineHash, debugName);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  // A compute pipeline carries no caller state: the stage and layout below are
  // the whole of it, so this builds its own create-info.
  VkComputePipelineCreateInfo createInfoStorage = {
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  VkComputePipelineCreateInfo *pipelineCreateInfo = &createInfoStorage;
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = pipeline.find(pipelineHash);
  if (it == pipeline.end()) {
    assert(shaderBin[PROGRAM_STAGE_COMPUTE].buf.size() > 0 &&
           "no compute shader binary");
    VkShaderModule module = VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo moduleCreateInfo = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        (VkShaderModuleCreateFlags)0,
        (size_t)shaderBin[PROGRAM_STAGE_COMPUTE].buf.size(),
        (const uint32_t *)shaderBin[PROGRAM_STAGE_COMPUTE].buf.data(),
    };
    vkCreateShaderModule(device->vk.device, &moduleCreateInfo, NULL, &module);

    pipelineCreateInfo->stage = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineCreateInfo->stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCreateInfo->stage.module = module;
    pipelineCreateInfo->stage.pName =
        shaderBin[PROGRAM_STAGE_COMPUTE].entryPoint.c_str();
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    pipelineCreateInfo->basePipelineIndex = -1;

    PipelineSlot slot = {};
    VK_WrapResult(vkCreateComputePipelines(device->vk.device, VK_NULL_HANDLE, 1,
                                           pipelineCreateInfo, NULL,
                                           &slot.vk.handle));

    if (vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT debugExt = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &debugExt));
    }
    pipelineHandle = slot.vk.handle;
    pipeline[pipelineHash] = slot;
    vkDestroyShaderModule(device->vk.device, module, NULL);
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipelineHandle);
#endif
}

void RIProgram::bindRayTracingPipeline(struct RIDevice *device,
                                       struct RICmd *cmd, hash_t pipelineHash,
                                       const char *debugName,
                                       const RIRayTracingPipelineDesc &desc) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    bindD3D12RayTracingPipeline(device, cmd, pipelineHash, debugName, desc);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  // pStages, pGroups and layout are filled in below from the program's own
  // shaderBin and pipelineLayout; recursion depth is the only thing the caller
  // supplies.
  VkRayTracingPipelineCreateInfoKHR createInfoStorage = {
      VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  createInfoStorage.maxPipelineRayRecursionDepth = desc.maxRecursionDepth;
  VkRayTracingPipelineCreateInfoKHR *pipelineCreateInfo = &createInfoStorage;
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = rtPipeline.find(pipelineHash);
  if (it == rtPipeline.end()) {
    // Query SBT alignment/size requirements for this physical device.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(device->physicalAdapter.vk.physicalDevice,
                                   &props2);

    // RT stage table: parallel to the populated shaderBin[] slots.
    static const struct {
      ProgramStages programStage;
      VkShaderStageFlagBits vkStage;
    } kRTStages[] = {
        {PROGRAM_STAGE_RAYGEN, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {PROGRAM_STAGE_MISS, VK_SHADER_STAGE_MISS_BIT_KHR},
        {PROGRAM_STAGE_CLOSEST_HIT, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {PROGRAM_STAGE_ANY_HIT, VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
        {PROGRAM_STAGE_INTERSECTION, VK_SHADER_STAGE_INTERSECTION_BIT_KHR},
        {PROGRAM_STAGE_CALLABLE, VK_SHADER_STAGE_CALLABLE_BIT_KHR},
    };
    constexpr uint32_t RT_STAGE_COUNT = ARRAY_COUNT(kRTStages);

    VkShaderModule modules[RT_STAGE_COUNT] = {};
    VkPipelineShaderStageCreateInfo stages[RT_STAGE_COUNT] = {};
    int32_t stageIdx[RT_STAGE_COUNT];
    uint32_t stageCount = 0;
    for (uint32_t i = 0; i < RT_STAGE_COUNT; i++)
      stageIdx[i] = -1;
    for (uint32_t i = 0; i < RT_STAGE_COUNT; i++) {
      const auto &bin = shaderBin[kRTStages[i].programStage];
      if (bin.buf.empty())
        continue;
      VkShaderModuleCreateInfo modInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      modInfo.codeSize = (size_t)bin.buf.size();
      modInfo.pCode = (const uint32_t *)bin.buf.data();
      vkCreateShaderModule(device->vk.device, &modInfo, NULL,
                           &modules[stageCount]);
      stages[stageCount].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[stageCount].stage = kRTStages[i].vkStage;
      stages[stageCount].module = modules[stageCount];
      stages[stageCount].pName =
          shaderBin[kRTStages[i].programStage].entryPoint.c_str();
      stageIdx[i] = (int32_t)stageCount;
      stageCount++;
    }
    assert(stageIdx[0] >= 0 && "ray-tracing pipeline requires a raygen shader");

    // Group layout: 1 raygen, 1 miss (optional), 1 hit group (optional),
    // 1 callable (optional). Hit group is triangles if no intersection
    // shader, procedural otherwise.
    VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};
    uint32_t groupCount = 0;
    uint32_t raygenGroupOffset = 0, raygenGroupCount = 0;
    uint32_t missGroupOffset = 0, missGroupCount = 0;
    uint32_t hitGroupOffset = 0, hitGroupCount = 0;
    uint32_t callableGroupOffset = 0, callableGroupCount = 0;

    auto pushGeneral = [&](int32_t shader) {
      groups[groupCount].sType =
          VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
      groups[groupCount].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
      groups[groupCount].generalShader = (uint32_t)shader;
      groups[groupCount].closestHitShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].anyHitShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].intersectionShader = VK_SHADER_UNUSED_KHR;
      groupCount++;
    };

    raygenGroupOffset = groupCount;
    pushGeneral(stageIdx[0]);
    raygenGroupCount = 1;

    missGroupOffset = groupCount;
    if (stageIdx[1] >= 0) {
      pushGeneral(stageIdx[1]);
      missGroupCount = 1;
    }

    hitGroupOffset = groupCount;
    if (stageIdx[2] >= 0 || stageIdx[3] >= 0 || stageIdx[4] >= 0) {
      groups[groupCount].sType =
          VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
      groups[groupCount].type =
          (stageIdx[4] >= 0)
              ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
              : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
      groups[groupCount].generalShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].closestHitShader =
          stageIdx[2] >= 0 ? (uint32_t)stageIdx[2] : VK_SHADER_UNUSED_KHR;
      groups[groupCount].anyHitShader =
          stageIdx[3] >= 0 ? (uint32_t)stageIdx[3] : VK_SHADER_UNUSED_KHR;
      groups[groupCount].intersectionShader =
          stageIdx[4] >= 0 ? (uint32_t)stageIdx[4] : VK_SHADER_UNUSED_KHR;
      groupCount++;
      hitGroupCount = 1;
    }

    callableGroupOffset = groupCount;
    if (stageIdx[5] >= 0) {
      pushGeneral(stageIdx[5]);
      callableGroupCount = 1;
    }

    pipelineCreateInfo->sType =
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineCreateInfo->stageCount = stageCount;
    pipelineCreateInfo->pStages = stages;
    pipelineCreateInfo->groupCount = groupCount;
    pipelineCreateInfo->pGroups = groups;
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    if (pipelineCreateInfo->maxPipelineRayRecursionDepth == 0)
      pipelineCreateInfo->maxPipelineRayRecursionDepth = 1;
    pipelineCreateInfo->basePipelineIndex = -1;

    RTPipelineSlot slot = {};
    VK_WrapResult(vkCreateRayTracingPipelinesKHR(
        device->vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
        pipelineCreateInfo, NULL, &slot.vk.handle));

    // SBT: one record per group, each padded to handleAlignment; each
    // region (raygen/miss/hit/callable) starts on a baseAlignment offset.
    auto alignUp = [](VkDeviceSize x, VkDeviceSize a) -> VkDeviceSize {
      return (x + a - 1) & ~(a - 1);
    };
    const VkDeviceSize handleSize = rtProps.shaderGroupHandleSize;
    const VkDeviceSize handleStride =
        alignUp(handleSize, rtProps.shaderGroupHandleAlignment);
    const VkDeviceSize baseAlign = rtProps.shaderGroupBaseAlignment;
    const VkDeviceSize raygenRegionSize =
        alignUp(handleStride * raygenGroupCount, baseAlign);
    const VkDeviceSize missRegionSize =
        alignUp(handleStride * missGroupCount, baseAlign);
    const VkDeviceSize hitRegionSize =
        alignUp(handleStride * hitGroupCount, baseAlign);
    const VkDeviceSize callableRegionSize =
        alignUp(handleStride * callableGroupCount, baseAlign);
    const VkDeviceSize sbtSize =
        raygenRegionSize + missRegionSize + hitRegionSize + callableRegionSize;

    RIBuffer sbtBuf =
        RIBuffer::create(device, {(uint64_t)sbtSize,
                                  RI_BUFFER_USAGE_BINDING_TABLE |
                                      RI_BUFFER_USAGE_DEVICE_ADDRESS |
                                      RI_BUFFER_USAGE_TRANSFER_DST,
                                  RI_MEMORY_HOST_UPLOAD, baseAlign});
    slot.vk.sbtBuffer = sbtBuf.vk.buffer;
    slot.vk.sbtAlloc = sbtBuf.vk.allocation;

    std::vector<uint8_t> handles((size_t)handleSize * groupCount);
    VK_WrapResult(vkGetRayTracingShaderGroupHandlesKHR(
        device->vk.device, slot.vk.handle, 0, groupCount, handles.size(),
        handles.data()));

    auto writeRegion = [&](uint8_t *dst, uint32_t groupOff, uint32_t count) {
      for (uint32_t i = 0; i < count; i++) {
        memcpy(dst + (VkDeviceSize)i * handleStride,
               handles.data() + (size_t)(groupOff + i) * handleSize,
               (size_t)handleSize);
      }
    };
    uint8_t *sbtMapped = (uint8_t *)sbtBuf.mappedAddress;
    writeRegion(sbtMapped + 0, raygenGroupOffset, raygenGroupCount);
    writeRegion(sbtMapped + raygenRegionSize, missGroupOffset, missGroupCount);
    writeRegion(sbtMapped + raygenRegionSize + missRegionSize, hitGroupOffset,
                hitGroupCount);
    writeRegion(sbtMapped + raygenRegionSize + missRegionSize + hitRegionSize,
                callableGroupOffset, callableGroupCount);

    VkBufferDeviceAddressInfo bdaInfo = {
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bdaInfo.buffer = slot.vk.sbtBuffer;
    const VkDeviceAddress sbtBase =
        vkGetBufferDeviceAddress(device->vk.device, &bdaInfo);

    slot.vk.raygenRegion.deviceAddress = sbtBase;
    // raygen region stride MUST equal size per the spec.
    slot.vk.raygenRegion.stride = raygenRegionSize;
    slot.vk.raygenRegion.size = raygenRegionSize;
    slot.vk.missRegion.deviceAddress = sbtBase + raygenRegionSize;
    slot.vk.missRegion.stride = missGroupCount ? handleStride : 0;
    slot.vk.missRegion.size = missRegionSize;
    slot.vk.hitRegion.deviceAddress =
        sbtBase + raygenRegionSize + missRegionSize;
    slot.vk.hitRegion.stride = hitGroupCount ? handleStride : 0;
    slot.vk.hitRegion.size = hitRegionSize;
    slot.vk.callableRegion.deviceAddress =
        sbtBase + raygenRegionSize + missRegionSize + hitRegionSize;
    slot.vk.callableRegion.stride = callableGroupCount ? handleStride : 0;
    slot.vk.callableRegion.size = callableRegionSize;

    if (vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT debugExt = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &debugExt));
    }
    pipelineHandle = slot.vk.handle;
    rtPipeline[pipelineHash] = slot;
    for (uint32_t i = 0; i < stageCount; i++) {
      vkDestroyShaderModule(device->vk.device, modules[i], NULL);
    }
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                    pipelineHandle);
#else
  (void)device;
  (void)cmd;
  (void)pipelineHash;
  (void)debugName;
  (void)desc;
  // Reached only when the active target is neither of the compiled backends.
  FatalError("RIProgram: no ray-tracing pipeline implementation for the "
             "active backend\n");
#endif
}

void RIProgram::traceRays(struct RICmd *cmd, hash_t pipelineHash,
                          uint32_t width, uint32_t height, uint32_t depth) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    traceD3D12Rays(cmd, pipelineHash, width, height, depth);
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  auto it = rtPipeline.find(pipelineHash);
  assert(it != rtPipeline.end() &&
         "traceRays called before bindRayTracingPipeline");
  const auto &slot = it->second.vk;
  vkCmdTraceRaysKHR(cmd->vk.cmd, &slot.raygenRegion, &slot.missRegion,
                    &slot.hitRegion, &slot.callableRegion, width, height,
                    depth);
#endif
}

void RIProgram::traceRaysIndirect(struct RICmd *cmd, hash_t pipelineHash,
                                  VkDeviceAddress indirectAddress) {
#if (DEVICE_IMPL_D3D12)
  // Runtime, not #if: DEVICE_SUPPORT_VULKAN is unconditional, so the #else
  // below is dead code and a D3D12 run would otherwise fall straight into
  // vkCmdTraceRaysIndirectKHR with a reinterpreted command-list pointer.
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    (void)cmd;
    (void)pipelineHash;
    (void)indirectAddress;
    FatalError("RIProgram: traceRaysIndirect requires the Vulkan backend\n");
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  auto it = rtPipeline.find(pipelineHash);
  assert(it != rtPipeline.end() &&
         "traceRaysIndirect called before bindRayTracingPipeline");
  const auto &slot = it->second.vk;
  vkCmdTraceRaysIndirectKHR(cmd->vk.cmd, &slot.raygenRegion, &slot.missRegion,
                            &slot.hitRegion, &slot.callableRegion,
                            indirectAddress);
#else
  (void)cmd;
  (void)pipelineHash;
  (void)indirectAddress;
  // Deliberately not implemented on D3D12 -- see the header. Failing loudly
  // beats the silent no-op this used to be, which would have dropped every
  // indirect trace on the floor.
  FatalError("RIProgram: traceRaysIndirect requires the Vulkan backend\n");
#endif
}

void RIProgram::bindDescriptors(struct RIDevice *device, struct RICmd *cmd,
                                uint32_t frameIndex,
                                DescriptorBinding *bindings,
                                size_t bindingCount,
                                VkPipelineBindPoint bindPoint) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!device || device != this->device || !cmd || !cmd->d3d12.cmdList ||
        !ri_d3d12BindPointSupported(bindPoint))
      return;
    auto lookup = [this](const DescriptorBinding &b) {
      return b.useSlot ? findReflectionBySlot(b.slotSet, b.slotBinding)
                       : findReflection(b.handle);
    };
    auto reflectedResource = [this](const BindingReflection *binding)
        -> const ShaderResourceReflection * {
      if (!binding)
        return nullptr;
      for (const auto &binary : shaderBin)
        if (binary.reflection) {
          for (const auto &resource : binary.reflection->resources) {
            const DescriptorBindingID id =
                CreateDescriptorBindingID(resource.name.c_str());
            if (id.hash == binding->hash &&
                static_cast<uint32_t>(
                    resource.registerClass == ShaderRegisterClass::CBV
                        ? RIBindlessRegisterClass::CBV
                    : resource.registerClass == ShaderRegisterClass::SRV
                        ? RIBindlessRegisterClass::SRV
                    : resource.registerClass == ShaderRegisterClass::UAV
                        ? RIBindlessRegisterClass::UAV
                        : RIBindlessRegisterClass::Sampler) ==
                    static_cast<uint32_t>(binding->registerClass))
              return &resource;
          }
        }
      return nullptr;
    };
    // Names the offending binding for the diagnostics below. Callers identify a
    // binding either by reflected name (hashed into `handle`, so unrecoverable
    // here) or by an explicit (set, binding) slot, hence the two shapes. The
    // reflected name is only retained in debug builds.
    auto bindingLabel = [&](size_t index, const DescriptorBinding &binding,
                            const BindingReflection *r) {
      auto out = fmt::memory_buffer();
      fmt::format_to(std::back_inserter(out), "binding %zu", index);
      if (binding.useSlot)
        fmt::format_to(std::back_inserter(out)," (set %u, slot %u)", binding.slotSet, binding.slotBinding);
      if (r)
        fmt::format_to(std::back_inserter(out), " [register class %u, base register %u]", (unsigned)r->registerClass, (unsigned)r->baseRegisterIndex);
#if !defined(NDEBUG)
      if (r && !r->debugName.empty())
        fmt::format_to(std::back_inserter(out), " '%s'", r->debugName.c_str()                                                                       );
#endif
      return fmt::to_string(out);
    };
    auto validBinding = [&](size_t index, const DescriptorBinding &binding,
                            const BindingReflection *r) {
      if (!r || binding.descriptor.isEmpty())
        return true;
      if (r->d3d12External) {
        ValidationFailed(
            "RIProgram: %s: external D3D12 descriptors must be written through "
            "the bindless set\n",
            bindingLabel(index, binding, r).c_str());
        return false;
      }
      const RIDescriptor &d = binding.descriptor;
      const auto expected = r->registerClass;
      const bool classOk =
          (d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
           expected == RIBindlessRegisterClass::CBV) ||
          (d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
           (expected == RIBindlessRegisterClass::SRV ||
            expected == RIBindlessRegisterClass::UAV)) ||
          (d.type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
           expected == RIBindlessRegisterClass::SRV) ||
          (d.type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
           expected == RIBindlessRegisterClass::UAV) ||
          (d.type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE &&
           expected == RIBindlessRegisterClass::SRV) ||
          (d.type == RI_DESCRIPTOR_TYPE_SAMPLER &&
           expected == RIBindlessRegisterClass::Sampler);
      const uint32_t count = std::max(1u, r->descriptorCount);
      const bool elementOk = r->isArray ? binding.registerOffset < count
                                        : binding.registerOffset == 0;
      if (!classOk || !elementOk) {
        ValidationFailed(
            "RIProgram: %s: D3D12 descriptor does not match the reflected %s "
            "(descriptor type %u, reflected register class %u, register offset "
            "%u, reflected count %u)\n",
            bindingLabel(index, binding, r).c_str(),
            !classOk ? "register class" : "array element", (unsigned)d.type,
            (unsigned)expected, binding.registerOffset, count);
        return false;
      }
      if (d.type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) {
        if (d.payload.accel.gpuVA == 0) {
          ValidationFailed(
              "RIProgram: %s: D3D12 acceleration-structure descriptor has a "
              "null GPU address; the structure has not been built\n",
              bindingLabel(index, binding, r).c_str());
          return false;
        }
        return true;
      }
      if (d.type == RI_DESCRIPTOR_TYPE_SAMPLER) {
        if (d.payload.sampler.initialized == 0 ||
            d.payload.sampler.d3d12Desc.MaxLOD <
                d.payload.sampler.d3d12Desc.MinLOD) {
          ValidationFailed("RIProgram: %s: D3D12 sampler descriptor is %s\n",
                           bindingLabel(index, binding, r).c_str(),
                           d.payload.sampler.initialized == 0
                               ? "uninitialized"
                               : "inverted (MaxLOD < MinLOD)");
          return false;
        }
        return true;
      }
      if (d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        ID3D12Resource *resource = d.payload.buffer.nativeResource;
        if (!resource || !d.payload.buffer.size || !d.payload.buffer.range ||
            d.payload.buffer.offset > UINT64_MAX - d.payload.buffer.range) {
          ValidationFailed(
              "RIProgram: %s: D3D12 buffer descriptor payload is invalid "
              "(resource %p, size %llu, range %llu, offset %llu)\n",
              bindingLabel(index, binding, r).c_str(), (void *)resource,
              (unsigned long long)d.payload.buffer.size,
              (unsigned long long)d.payload.buffer.range,
              (unsigned long long)d.payload.buffer.offset);
          return false;
        }
        const bool cbvRoundable = d.payload.buffer.range <= UINT64_MAX - 255u;
        const uint64_t cbvSize =
            cbvRoundable ? ((d.payload.buffer.range + 255u) & ~UINT64(255u))
                         : 0;
        const ShaderResourceReflection *sr = reflectedResource(r);
        RID3D12BufferShape shape;
        const bool bufferShapeOk =
            d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                ? (!d.payload.buffer.raw && !d.payload.buffer.structured)
                : ri_d3d12_resolveBufferShape(sr, d.payload.buffer, shape);
        const uint64_t elementSize = d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                         ? (shape.raw ? 4 : shape.stride)
                                         : 0;
        const uint64_t firstElement =
            elementSize ? d.payload.buffer.offset / elementSize : 0;
        const uint64_t elementCount =
            elementSize ? d.payload.buffer.range / elementSize : 0;
        const bool boundOk =
            d.payload.buffer.offset <= d.payload.buffer.size &&
            d.payload.buffer.range <=
                d.payload.buffer.size - d.payload.buffer.offset;
        const bool cbvOk =
            d.type != RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (d.payload.buffer.offset %
                     D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT ==
                 0 &&
             cbvSize != 0 && cbvSize <= UINT_MAX &&
             d.payload.buffer.offset <= d.payload.buffer.size &&
             cbvSize <= d.payload.buffer.size - d.payload.buffer.offset);
        const bool elementsOk = d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                                (elementSize != 0 && firstElement <= UINT_MAX &&
                                 elementCount != 0 && elementCount <= UINT_MAX);
        if (!boundOk || !cbvOk || !elementsOk || !bufferShapeOk) {
          ValidationFailed(
              "RIProgram: %s: D3D12 buffer descriptor %s (size %llu, offset "
              "%llu, range %llu, element size %llu, first element %llu, "
              "element count %llu)\n",
              bindingLabel(index, binding, r).c_str(),
              !boundOk ? "range falls outside the buffer"
              : !cbvOk ? "is a CBV with a misaligned or out-of-bounds range"
              : !elementsOk ? "has a zero or out-of-range element count"
                            : "shape does not match the reflected resource",
              (unsigned long long)d.payload.buffer.size,
              (unsigned long long)d.payload.buffer.offset,
              (unsigned long long)d.payload.buffer.range,
              (unsigned long long)elementSize, (unsigned long long)firstElement,
              (unsigned long long)elementCount);
          return false;
        }
        return true;
      }
      const auto &texture = d.payload.texture;
      ID3D12Resource *resource = texture.nativeResource;
      if (!resource || texture.nativeFormat == DXGI_FORMAT_UNKNOWN ||
          !texture.mipNum || !texture.layerNum) {
        ValidationFailed(
            "RIProgram: %s: D3D12 image descriptor payload is invalid "
            "(resource %p, format %u, mipNum %u, layerNum %u)\n",
            bindingLabel(index, binding, r).c_str(), (void *)resource,
            (unsigned)texture.nativeFormat, (unsigned)texture.mipNum,
            (unsigned)texture.layerNum);
        return false;
      }
      const D3D12_RESOURCE_DESC rd = resource->GetDesc();
      const bool sampled = d.type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
      const bool viewOk = sampled
                              ? ri_d3d12_texture_view(srv, texture)
                              : ri_d3d12_texture_uav(uav, texture, resource);
      // A simultaneous-access texture holds its layout at COMMON, which is the
      // one layout admitting SRV reads and UAV writes at once -- that is the
      // whole reason for the flag (see RID3D12Barrier.cpp's texture-barrier
      // loop). Such a texture is therefore legitimately sampled from a combined
      // read+write state like GENERAL, which no ordinary texture may do. This
      // is deliberately narrow: it is not a general escape from the state rules
      // below, only a recognition of the resource class that opts into them.
      const bool simultaneous =
          (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
      const bool stateOk =
          sampled
              ? simultaneous ||
                    ((texture.state & RI_RESOURCE_STATE_SHADER_RESOURCE) != 0 &&
                     (texture.state & (RI_RESOURCE_STATE_STORAGE_READ |
                                       RI_RESOURCE_STATE_STORAGE_WRITE |
                                       RI_RESOURCE_STATE_COPY_DST |
                                       RI_RESOURCE_STATE_COPY_SRC)) == 0)
              : (texture.state &
                 (RI_RESOURCE_STATE_GENERAL | RI_RESOURCE_STATE_STORAGE_READ |
                  RI_RESOURCE_STATE_STORAGE_WRITE |
                  RI_RESOURCE_STATE_UNORDERED_ACCESS)) != 0;
      const auto viewType = static_cast<RITextureViewType_e>(texture.viewType);
      const bool resourceDimensionOk =
          ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) &&
           (viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY)) ||
          ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) &&
           (viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY)) ||
          ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) &&
           (viewType == RI_VIEWTYPE_SHADER_RESOURCE_3D ||
            viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D));
      const bool is3D = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      const bool layerRangeOk =
          is3D ? (texture.baseLayer == 0 && texture.layerNum == 1)
               : (texture.baseLayer < rd.DepthOrArraySize &&
                  texture.layerNum <= rd.DepthOrArraySize - texture.baseLayer);
      const bool mipRangeOk = texture.baseMip < rd.MipLevels &&
                              texture.mipNum <= rd.MipLevels - texture.baseMip;
      if (!stateOk || !mipRangeOk || !layerRangeOk || !viewOk ||
          !resourceDimensionOk) {
        // Report which condition failed. These six used to collapse into one
        // "format/state/range is invalid", which said nothing about which of
        // them tripped and made the NRD failure far harder to place than it
        // needed to be.
        ValidationFailed(
            "RIProgram: %s: D3D12 image view is invalid -- %s (descriptor %s, "
            "resource state 0x%x, view type %u, mips %u+%u of %u, layers %u+%u "
            "of %u, resource dimension %u%s)\n",
            bindingLabel(index, binding, r).c_str(),
            !stateOk        ? "resource state does not permit this access"
            : !mipRangeOk   ? "mip range falls outside the resource"
            : !layerRangeOk ? "layer range falls outside the resource"
            : !viewOk       ? "the view description could not be built"
                            : "view type does not match the resource dimension",
            sampled ? "sampled" : "storage", (unsigned)texture.state,
            (unsigned)viewType, (unsigned)texture.baseMip,
            (unsigned)texture.mipNum, (unsigned)rd.MipLevels,
            (unsigned)texture.baseLayer, (unsigned)texture.layerNum,
            (unsigned)rd.DepthOrArraySize, (unsigned)rd.Dimension,
            simultaneous ? ", simultaneous-access" : "");
        return false;
      }
      const D3D12_RESOURCE_FLAGS required =
          sampled ? D3D12_RESOURCE_FLAG_NONE
                  : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if ((sampled && (rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
          (required && !(rd.Flags & required))) {
        ValidationFailed(
            "RIProgram: %s: D3D12 image resource usage is incompatible with a "
            "%s descriptor -- the texture was created %s (resource flags "
            "0x%x)\n",
            bindingLabel(index, binding, r).c_str(),
            sampled ? "sampled" : "storage",
            sampled ? "with DENY_SHADER_RESOURCE (a depth-stencil texture "
                      "lacking RI_USAGE_SHADER_RESOURCE)"
                    : "without ALLOW_UNORDERED_ACCESS (missing "
                      "RI_USAGE_SHADER_RESOURCE_STORAGE)",
            (unsigned)rd.Flags);
        return false;
      }
      return true;
    };
    for (size_t i = 0; i < bindingCount; ++i)
      if (!validBinding(i, bindings[i], lookup(bindings[i])))
        return;
    hash_t hash = HASH_INITIAL_VALUE;
    for (const BindingReflection &r : bindingReflection) {
      if (r.d3d12External)
        continue;
      hash = hash_u64(hash, r.hash);
      hash = hash_u64(hash, r.d3d12DescriptorOffset);
      hash = hash_u64(hash, r.descriptorCount);
      for (size_t i = 0; i < bindingCount; ++i) {
        const BindingReflection *supplied = lookup(bindings[i]);
        if (supplied == &r) {
          hash = hash_u64(hash, bindings[i].registerOffset);
          hash = hash_u64(hash, bindings[i].descriptor.isEmpty()
                                    ? 0
                                    : bindings[i].descriptor.cookie);
        }
      }
    }
    // Writes the full table (null descriptors, then the supplied payloads) into
    // the entry's arena range and retains the bound resources. Used for both
    // fresh and recycled entries; on failure the caller drops the entry.
    auto writeEntry = [&](D3D12DescriptorCacheEntry &entry) -> bool {
      ID3D12DescriptorHeap *rh = nullptr, *sh = nullptr;
      if (!getDescriptorArenaHeaps(device, &rh, &sh))
        return false;
      const uint32_t rs =
          device->d3d12.device->GetDescriptorHandleIncrementSize(
              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      const uint32_t ss =
          device->d3d12.device->GetDescriptorHandleIncrementSize(
              D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      // Every reflected slot gets a valid null descriptor first. Optional
      // bindings can therefore remain absent without exposing uninitialized
      // shader-visible heap memory; supplied payloads overwrite these slots.
      for (const BindingReflection &r : bindingReflection) {
        if (r.d3d12External)
          continue;
        const uint32_t count = std::max(1u, r.descriptorCount);
        for (uint32_t element = 0; element < count; ++element) {
          if (r.d3d12Sampler) {
            D3D12_SAMPLER_DESC desc = {};
            desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            desc.AddressU = desc.AddressV = desc.AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            desc.MinLOD = 0.0f;
            desc.MaxLOD = D3D12_FLOAT32_MAX;
            device->d3d12.device->CreateSampler(
                &desc, ri_d3d12_cpu(sh,
                                    entry.allocation.samplerOffset +
                                        r.d3d12DescriptorOffset + element,
                                    ss));
          } else {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                ri_d3d12_cpu(rh,
                             entry.allocation.resourceOffset +
                                 r.d3d12DescriptorOffset + element,
                             rs);
            switch (r.registerClass) {
            case RIBindlessRegisterClass::CBV: {
              D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
              desc.BufferLocation = 0;
              desc.SizeInBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
              device->d3d12.device->CreateConstantBufferView(&desc, cpu);
            } break;
            case RIBindlessRegisterClass::UAV: {
              D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
              const ShaderResourceReflection *sr = reflectedResource(&r);
              const std::string_view type =
                  sr ? std::string_view(sr->type) : std::string_view();
              if (type.find("1D") != std::string_view::npos)
                desc.ViewDimension = r.isArray
                                         ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY
                                         : D3D12_UAV_DIMENSION_TEXTURE1D;
              else if (type.find("3D") != std::string_view::npos)
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
              else if (type.find("2D") != std::string_view::npos)
                desc.ViewDimension = r.isArray
                                         ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                         : D3D12_UAV_DIMENSION_TEXTURE2D;
              else {
                desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
                desc.Buffer.NumElements = 1;
                desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
              }
              if (desc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER) {
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY)
                  desc.Texture1DArray.ArraySize = 1;
                else if (desc.ViewDimension ==
                         D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
                  desc.Texture2DArray.ArraySize = 1;
                else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D)
                  desc.Texture3D.WSize = 1;
              }
              device->d3d12.device->CreateUnorderedAccessView(nullptr, nullptr,
                                                              &desc, cpu);
              break;
            }
            default: {
              D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
              const ShaderResourceReflection *sr = reflectedResource(&r);
              const std::string_view type =
                  sr ? std::string_view(sr->type) : std::string_view();
              if (type.find("cube") != std::string_view::npos ||
                  type.find("Cube") != std::string_view::npos)
                desc.ViewDimension = r.isArray
                                         ? D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
                                         : D3D12_SRV_DIMENSION_TEXTURECUBE;
              else if (type.find("1D") != std::string_view::npos)
                desc.ViewDimension = r.isArray
                                         ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY
                                         : D3D12_SRV_DIMENSION_TEXTURE1D;
              else if (type.find("3D") != std::string_view::npos)
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
              else if (type.find("2D") != std::string_view::npos)
                desc.ViewDimension = r.isArray
                                         ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                         : D3D12_SRV_DIMENSION_TEXTURE2D;
              else {
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
                desc.Buffer.NumElements = 1;
                desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
              }
              if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
              switch (desc.ViewDimension) {
              case D3D12_SRV_DIMENSION_TEXTURE1D:
                desc.Texture1D.MipLevels = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
                desc.Texture1DArray.MipLevels = 1;
                desc.Texture1DArray.ArraySize = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURE2D:
                desc.Texture2D.MipLevels = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                desc.Texture2DArray.MipLevels = 1;
                desc.Texture2DArray.ArraySize = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURECUBE:
                desc.TextureCube.MipLevels = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
                desc.TextureCubeArray.MipLevels = 1;
                desc.TextureCubeArray.NumCubes = 1;
                break;
              case D3D12_SRV_DIMENSION_TEXTURE3D:
                desc.Texture3D.MipLevels = 1;
                break;
              default:
                break;
              }
              desc.Shader4ComponentMapping =
                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
              device->d3d12.device->CreateShaderResourceView(nullptr, &desc,
                                                             cpu);
              break;
            }
            }
          }
        }
      }
      for (size_t i = 0; i < bindingCount; ++i) {
        const BindingReflection *r = lookup(bindings[i]);
        const RIDescriptor &d = bindings[i].descriptor;
        if (!r || d.isEmpty())
          continue;
        if (bindings[i].registerOffset >= std::max(1u, r->descriptorCount) ||
            r->d3d12DescriptorOffset >
                UINT32_MAX - bindings[i].registerOffset ||
            r->d3d12DescriptorOffset + bindings[i].registerOffset >=
                (r->d3d12Sampler ? entry.allocation.samplerCount
                                 : entry.allocation.resourceCount)) {
          Error("RIProgram: D3D12 reflected descriptor offset exceeds "
                "allocated table\n");
          return false;
        }
        const uint32_t n =
            r->d3d12DescriptorOffset + bindings[i].registerOffset;
        if (r->d3d12Sampler) {
          if (d.type != RI_DESCRIPTOR_TYPE_SAMPLER) {
            Error("RIProgram: D3D12 sampler binding has incompatible "
                  "descriptor\n");
            return false;
          }
          device->d3d12.device->CreateSampler(
              &d.payload.sampler.d3d12Desc,
              ri_d3d12_cpu(sh, entry.allocation.samplerOffset + n, ss));
        } else {
          D3D12_CPU_DESCRIPTOR_HANDLE cpu =
              ri_d3d12_cpu(rh, entry.allocation.resourceOffset + n, rs);
          ID3D12Resource *resource =
              d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                      d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER
                  ? d.payload.buffer.nativeResource
                  : d.payload.texture.nativeResource;
          if (d.type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
            v.ViewDimension =
                D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            v.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.RaytracingAccelerationStructure.Location = d.payload.accel.gpuVA;
            device->d3d12.device->CreateShaderResourceView(nullptr, &v, cpu);
          } else if (d.type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
            if (!ri_d3d12_texture_view(v, d.payload.texture)) {
              Error("RIProgram: invalid D3D12 sampled-image view payload\n");
              return false;
            }
            device->d3d12.device->CreateShaderResourceView(resource, &v, cpu);
          } else if (d.type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
            if (!ri_d3d12_texture_uav(v, d.payload.texture, resource)) {
              Error("RIProgram: invalid D3D12 storage-image view payload\n");
              return false;
            }
            device->d3d12.device->CreateUnorderedAccessView(resource, nullptr,
                                                            &v, cpu);
          } else if (!resource) {
            Error(
                "RIProgram: D3D12 descriptor has no native resource payload\n");
            return false;
          } else if (d.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC v = {
                resource->GetGPUVirtualAddress() + d.payload.buffer.offset,
                (UINT)((d.payload.buffer.range + 255) & ~UINT64(255))};
            device->d3d12.device->CreateConstantBufferView(&v, cpu);
          } else if (d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            RID3D12BufferShape shape;
            if (!ri_d3d12_resolveBufferShape(reflectedResource(r),
                                             d.payload.buffer, shape))
              return false;
            const UINT first = (UINT)(d.payload.buffer.offset /
                                      (shape.raw ? 4 : shape.stride));
            const UINT elements =
                (UINT)(d.payload.buffer.range / (shape.raw ? 4 : shape.stride));
            if (r->registerClass == RIBindlessRegisterClass::SRV) {
              D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
              v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
              v.Format =
                  shape.raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
              v.Buffer.FirstElement = first;
              v.Buffer.NumElements = elements;
              v.Buffer.StructureByteStride = shape.raw ? 0 : shape.stride;
              v.Buffer.Flags = shape.raw ? D3D12_BUFFER_SRV_FLAG_RAW
                                         : D3D12_BUFFER_SRV_FLAG_NONE;
              v.Shader4ComponentMapping =
                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
              device->d3d12.device->CreateShaderResourceView(resource, &v, cpu);
            } else {
              D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
              v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
              v.Format =
                  shape.raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
              v.Buffer.FirstElement = first;
              v.Buffer.NumElements = elements;
              v.Buffer.StructureByteStride = shape.raw ? 0 : shape.stride;
              v.Buffer.Flags = shape.raw ? D3D12_BUFFER_UAV_FLAG_RAW
                                         : D3D12_BUFFER_UAV_FLAG_NONE;
              device->d3d12.device->CreateUnorderedAccessView(resource, nullptr,
                                                              &v, cpu);
            }
          } else {
            Error("RIProgram: unsupported D3D12 descriptor payload type\n");
            return false;
          }
        }
      }
      entry.resources.assign(entry.allocation.resourceCount, nullptr);
      entry.allocations.assign(entry.allocation.resourceCount, nullptr);
      for (size_t i = 0; i < bindingCount; ++i) {
        const BindingReflection *r = lookup(bindings[i]);
        const RIDescriptor &d = bindings[i].descriptor;
        if (!r || d.isEmpty() || r->d3d12Sampler)
          continue;
        const uint32_t slot =
            r->d3d12DescriptorOffset + bindings[i].registerOffset;
        if (slot >= entry.resources.size())
          return false;
        ID3D12Resource *resource = nullptr;
        D3D12MA::Allocation *allocation = nullptr;
        ri_d3d12_descriptor_identity(d, &resource, &allocation);
        ri_d3d12_replace_slot(entry.resources[slot], entry.allocations[slot],
                              resource, allocation);
      }
      return true;
    };

    uint32_t entryIndex = UINT32_MAX;
    if (auto indexed = d3d12DescriptorIndex.find(hash);
        indexed != d3d12DescriptorIndex.end()) {
      entryIndex = indexed->second;
    } else {
      uint32_t resourceCount = 0, samplerCount = 0;
      for (const BindingReflection &r : bindingReflection) {
        if (r.d3d12External)
          continue;
        const uint32_t count = std::max(1u, r.descriptorCount);
        if (r.d3d12Sampler) {
          if (samplerCount > UINT32_MAX - count)
            return;
          samplerCount += count;
        } else {
          if (resourceCount > UINT32_MAX - count)
            return;
          resourceCount += count;
        }
      }
      for (const BindingReflection &r : bindingReflection) {
        if (r.d3d12External)
          continue;
        const uint32_t count = std::max(1u, r.descriptorCount);
        const uint32_t capacity = r.d3d12Sampler ? samplerCount : resourceCount;
        if (r.d3d12DescriptorOffset > capacity ||
            count > capacity - r.d3d12DescriptorOffset) {
          Error("RIProgram: reflected D3D12 descriptor layout exceeds arena "
                "table\n");
          return;
        }
      }
      // Clock sweep for an entry no in-flight frame can still reference. The
      // graphics ring waits on its pool fence before reuse, so a table last
      // bound more than RI_NUMBER_FRAMES_FLIGHT frames ago has retired. Table
      // counts derive from reflection only, so any entry's range fits.
      const uint32_t cacheSize =
          static_cast<uint32_t>(d3d12DescriptorCache.size());
      for (uint32_t step = 0; step < cacheSize; ++step) {
        const uint32_t candidate =
            (d3d12DescriptorRecycleCursor + step) % cacheSize;
        const D3D12DescriptorCacheEntry &entry =
            d3d12DescriptorCache[candidate];
        if (frameIndex > entry.lastUsedFrame + RI_NUMBER_FRAMES_FLIGHT &&
            entry.allocation.resourceCount == resourceCount &&
            entry.allocation.samplerCount == samplerCount) {
          entryIndex = candidate;
          d3d12DescriptorRecycleCursor = (candidate + 1) % cacheSize;
          break;
        }
      }
      if (entryIndex != UINT32_MAX) {
        D3D12DescriptorCacheEntry &entry = d3d12DescriptorCache[entryIndex];
        d3d12DescriptorIndex.erase(entry.hash);
        ri_d3d12_release_cache_refs(entry.resources, entry.allocations);
      } else {
        D3D12DescriptorCacheEntry entry;
        if (!allocateDescriptorArena(device, resourceCount, samplerCount,
                                     &entry.allocation)) {
          if (!d3d12ArenaExhaustedReported) {
            d3d12ArenaExhaustedReported = true;
            Error(
                "RIProgram: D3D12 descriptor arena exhausted (%u resource / %u "
                "sampler descriptors requested, %zu tables cached)\n",
                resourceCount, samplerCount, d3d12DescriptorCache.size());
          }
          return;
        }
        entryIndex = static_cast<uint32_t>(d3d12DescriptorCache.size());
        d3d12DescriptorCache.push_back(std::move(entry));
        ++g_riD3D12DescriptorCacheEntries;
      }
      D3D12DescriptorCacheEntry &entry = d3d12DescriptorCache[entryIndex];
      entry.hash = hash;
      if (!writeEntry(entry)) {
        ri_d3d12_release_cache_refs(entry.resources, entry.allocations);
        const RIDescriptorArenaFence retire =
            ri_d3d12_graphics_retire_fence(device);
        releaseDescriptorArena(device, &entry.allocation, &retire);
        // Swap-remove, keeping the moved entry's index current.
        const uint32_t last =
            static_cast<uint32_t>(d3d12DescriptorCache.size() - 1);
        if (entryIndex != last) {
          d3d12DescriptorCache[entryIndex] =
              std::move(d3d12DescriptorCache[last]);
          d3d12DescriptorIndex[d3d12DescriptorCache[entryIndex].hash] =
              entryIndex;
        }
        d3d12DescriptorCache.pop_back();
        --g_riD3D12DescriptorCacheEntries;
        d3d12DescriptorRecycleCursor = 0;
        return;
      }
      d3d12DescriptorIndex.emplace(hash, entryIndex);
    }
    D3D12DescriptorCacheEntry &cached = d3d12DescriptorCache[entryIndex];
    cached.lastUsedFrame = frameIndex;
    const RIDescriptorArenaAllocation &allocation = cached.allocation;
    ID3D12DescriptorHeap *heaps[] = {nullptr, nullptr};
    const bool hasResourceTable = allocation.resourceCount != 0;
    const bool hasSamplerTable = allocation.samplerCount != 0;
    if (hasResourceTable || hasSamplerTable) {
      if (!getDescriptorArenaHeaps(device, &heaps[0], &heaps[1]))
        return;
      RID3D12_SetDescriptorHeaps(*cmd, heaps[0], heaps[1]);
    }
    const uint32_t rs = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t ss = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    // Bind this program's root signature before writing its tables. Table
    // indices are meaningless against another program's layout, so a caller
    // that binds descriptors before the pipeline would otherwise write into
    // whatever signature the previous pass left behind. The bind is elided
    // when it is already current, so the reverse order costs nothing.
    if (ri_d3d12ComputeRootPath(bindPoint)) {
      cmd->d3d12.computePipelineBound = true;
      RID3D12_SetComputeRootSignature(*cmd, impl.d3d12.rootSignature);
      if (impl.d3d12.resourceRootParameter != UINT32_MAX && hasResourceTable)
        RID3D12_SetComputeRootDescriptorTable(
            *cmd, impl.d3d12.resourceRootParameter,
            ri_d3d12_gpu(heaps[0], allocation.resourceOffset, rs));
      if (impl.d3d12.samplerRootParameter != UINT32_MAX && hasSamplerTable)
        RID3D12_SetComputeRootDescriptorTable(
            *cmd, impl.d3d12.samplerRootParameter,
            ri_d3d12_gpu(heaps[1], allocation.samplerOffset, ss));
    } else {
      cmd->d3d12.computePipelineBound = false;
      RID3D12_SetGraphicsRootSignature(*cmd, impl.d3d12.rootSignature);
      if (impl.d3d12.resourceRootParameter != UINT32_MAX && hasResourceTable)
        RID3D12_SetGraphicsRootDescriptorTable(
            *cmd, impl.d3d12.resourceRootParameter,
            ri_d3d12_gpu(heaps[0], allocation.resourceOffset, rs));
      if (impl.d3d12.samplerRootParameter != UINT32_MAX && hasSamplerTable)
        RID3D12_SetGraphicsRootDescriptorTable(
            *cmd, impl.d3d12.samplerRootParameter,
            ri_d3d12_gpu(heaps[1], allocation.samplerOffset, ss));
    }
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  {
    VkDescriptorSet setsToBind[DESCRIPTOR_SET_MAX] = {VK_NULL_HANDLE};
    uint32_t firstSetToBind = 0;
    uint32_t setsToBindCount = 0;

    for (uint32_t setIndex = 0; setIndex < DESCRIPTOR_SET_MAX; setIndex++) {
      // External sets are bound by the caller via bindBindlessDescriptorSet
      // (or vkCmdBindDescriptorSets directly). Skip alloc/write/bind here.
      if (programDescriptors[setIndex].isExternal)
        continue;
      auto findBindingReflection = [this](const DescriptorBinding &binding) {
        return binding.useSlot
                   ? findReflectionBySlot(binding.slotSet, binding.slotBinding)
                   : findReflection(binding.handle);
      };
      hash_t hash = HASH_INITIAL_VALUE;
#if !defined(NDEBUG)
      // Debug-only bookkeeping for the unwritten-binding check below: which
      // reflected bindings of this set the caller actually wrote, and which
      // ones every supplied entry flagged as optional.
      std::unordered_set<hash_t> coveredBindings;
      std::unordered_map<hash_t, bool> suppliedOptional;
#endif
      for (size_t i = 0; i < bindingCount; i++) {
        const struct RIProgram::BindingReflection *refl =
            findBindingReflection(bindings[i]);
#if !defined(NDEBUG)
        if (refl && setIndex == (uint32_t)refl->set) {
          auto opt = suppliedOptional.emplace(refl->hash, true).first;
          opt->second = opt->second && bindings[i].optional;
          if (!bindings[i].descriptor.isEmpty())
            coveredBindings.insert(refl->hash);
        }
#endif
        if (!refl || setIndex != refl->set || bindings[i].descriptor.isEmpty())
          continue;
        hash = hash_u64(hash, refl->hash);
        hash = hash_u64(hash, bindings[i].registerOffset);
        assert(bindings[i].descriptor.cookie != 0);
        hash = hash_u64(hash, bindings[i].descriptor.cookie);
      }
#if !defined(NDEBUG)
      // A binding the shader reflects but nobody wrote is a descriptor the
      // shader reads undefined. Report it once per program; never assert,
      // the warm-up frames before the first TLAS / world-buffer build hit
      // this legitimately and must stay runnable.
      {
        const bool setNeverBound = (hash == HASH_INITIAL_VALUE);
        for (const struct RIProgram::BindingReflection &refl :
             bindingReflection) {
          if ((uint32_t)refl.set != setIndex)
            continue;
          // Array bindings are laid out with
          // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, so unwritten
          // elements are legal.
          if (refl.isArray)
            continue;
          if (coveredBindings.count(refl.hash) > 0)
            continue;
          const auto opt = suppliedOptional.find(refl.hash);
          if (opt != suppliedOptional.end() && opt->second)
            continue;
          const uint64_t reportKey =
              hash_u64(hash_u32(HASH_INITIAL_VALUE, setIndex), refl.hash);
          if (!m_reportedUnwrittenBindings.insert(reportKey).second)
            continue;
          Error("RIProgram(%s): descriptor set %u binding %u '%s' is reflected "
                "by the shader but was never written%s — the shader will read "
                "an undefined descriptor (VUID-vkCmdDrawIndexed-None-08114). "
                "Add it to the DescriptorBinding list, or construct it with "
                "optional=true if it may legitimately be empty.\n",
                m_debugName.c_str(), setIndex, (unsigned)refl.baseRegisterIndex,
                refl.debugName.c_str(),
                setNeverBound ? " (the caller supplied nothing at all for this "
                                "set, so it is never bound)"
                              : "");
        }
      }
#endif
      if (hash == HASH_INITIAL_VALUE)
        continue;
      struct DescriptorSetSlot *info = &programDescriptors[setIndex];
      struct RIDescriptorSetResult result =
          resolveDescriptorSetAlloc(device, &info->alloc, frameIndex, hash);
      if (!result.found) {
        size_t numWrites = 0;
        VkWriteDescriptorSet descriptorWrite[32];
        // Transient handle payloads — the new RIDescriptor stores RI-object
        // pointers, so we build the VkDescriptor*Info / accel handle from the
        // accessors here and keep them alive (one slot per descriptorWrite[i])
        // across the vkUpdateDescriptorSets flush.
        VkWriteDescriptorSetAccelerationStructureKHR accelWrites[32] = {};
        VkDescriptorImageInfo imageInfos[32] = {};
        VkDescriptorBufferInfo bufferInfos[32] = {};
        VkAccelerationStructureKHR accelHandles[32] = {};
        for (size_t i = 0; i < bindingCount; i++) {
          const struct RIProgram::BindingReflection *refl =
              findBindingReflection(bindings[i]);
          if (!refl || setIndex != refl->set ||
              bindings[i].descriptor.isEmpty())
            continue;

          if (numWrites == ARRAY_COUNT(descriptorWrite)) {
            vkUpdateDescriptorSets(device->vk.device,
                                   static_cast<uint32_t>(numWrites),
                                   descriptorWrite, 0, NULL);
            numWrites = 0;
          }
          VkWriteDescriptorSet *vkDesc = descriptorWrite + (numWrites++);
          memset(vkDesc, 0, sizeof(VkWriteDescriptorSet));
          vkDesc->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          vkDesc->dstSet = result.set->vk.handle;
          if (refl->isArray) {
            vkDesc->dstBinding = refl->baseRegisterIndex;
            vkDesc->dstArrayElement = bindings[i].registerOffset;
          } else {
            vkDesc->dstBinding =
                refl->baseRegisterIndex + bindings[i].registerOffset;
            vkDesc->dstArrayElement = 0;
          }
          vkDesc->descriptorCount = 1;
          const struct RIDescriptor &d = bindings[i].descriptor;
          const size_t w = numWrites - 1;
          vkDesc->descriptorType = ri_vk_BindlessDescriptorType(d.type);
          switch ((enum RIDescriptorType_e)d.type) {
          case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            bufferInfos[w] = {d.vkBuffer(), d.payload.buffer.offset,
                              d.payload.buffer.range};
            vkDesc->pBufferInfo = &bufferInfos[w];
            break;
          case RI_DESCRIPTOR_TYPE_SAMPLER:
          case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            imageInfos[w] = d.vk.image;
            vkDesc->pImageInfo = &imageInfos[w];
            break;
          case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
            accelHandles[w] = d.vkAccel();
            VkWriteDescriptorSetAccelerationStructureKHR *aw = &accelWrites[w];
            aw->sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            aw->pNext = NULL;
            aw->accelerationStructureCount = 1;
            aw->pAccelerationStructures = &accelHandles[w];
            vkDesc->pNext = aw;
            break;
          }
          default:
            assert(false);
            break;
          }
        }
        if (numWrites > 0) {
          vkUpdateDescriptorSets(device->vk.device,
                                 static_cast<uint32_t>(numWrites),
                                 descriptorWrite, 0, NULL);
        }
      }

      if (setsToBindCount > 0 && firstSetToBind + setsToBindCount != setIndex) {
        vkCmdBindDescriptorSets(cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
                                firstSetToBind, setsToBindCount, setsToBind, 0,
                                NULL);
        setsToBindCount = 0;
      }
      if (setsToBindCount == 0) {
        firstSetToBind = setIndex;
      }
      setsToBind[setsToBindCount++] = result.set->vk.handle;
    }
    if (setsToBindCount > 0) {
      vkCmdBindDescriptorSets(cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
                              firstSetToBind, setsToBindCount, setsToBind, 0,
                              NULL);
    }
  }
#endif
}

bool RIProgram::bindBindlessDescriptorSet(struct RICmd *cmd,
                                          RIBindlessDescriptorSet *bindless,
                                          uint32_t setIndex,
                                          VkPipelineBindPoint bindPoint) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!ri_d3d12BindPointSupported(bindPoint))
      return false;
    if (setIndex >= programDescriptors.size() ||
        !programDescriptors[setIndex].isExternal)
      return false;
    const auto &slot = programDescriptors[setIndex].d3d12;
    if (!bindless || !cmd || !impl.d3d12.rootSignature ||
        (bindless->d3d12.rootSignature &&
         bindless->d3d12.rootSignature != impl.d3d12.rootSignature) ||
        (programDescriptors[setIndex].d3d12ExternalTables.empty() &&
         ((bindless->d3d12.resourceRootParameter != UINT32_MAX &&
           slot.resourceRootParameter !=
               bindless->d3d12.resourceRootParameter) ||
          (bindless->d3d12.samplerRootParameter != UINT32_MAX &&
           slot.samplerRootParameter != bindless->d3d12.samplerRootParameter) ||
          (bindless->d3d12.geometryRootParameter != UINT32_MAX &&
           slot.geometryRootParameter !=
               bindless->d3d12.geometryRootParameter) ||
          slot.geometryRangeOffset != bindless->d3d12.geometryRangeOffset)))
      return false;
    return bindD3D12BindlessDescriptorSet(
        cmd, bindless, ri_d3d12ComputeRootPath(bindPoint), setIndex);
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  if (!cmd || !bindless || setIndex >= programDescriptors.size() ||
      !programDescriptors[setIndex].isExternal ||
      bindless->vk.m_bindlessSet == VK_NULL_HANDLE)
    return false;
  vkCmdBindDescriptorSets(cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
                          setIndex, 1, &bindless->vk.m_bindlessSet, 0, NULL);
  return true;
#endif
  (void)cmd;
  (void)bindless;
  (void)setIndex;
  (void)bindPoint;
  return false;
}

#if (DEVICE_IMPL_D3D12)
bool RIProgram::bindD3D12BindlessDescriptorSet(
    struct RICmd *cmd, RIBindlessDescriptorSet *bindless, bool compute,
    uint32_t setIndex) {
  if (!cmd || !cmd->d3d12.cmdList || !bindless ||
      !bindless->d3d12.ownsAllocation)
    return false;
  if (setIndex >= programDescriptors.size())
    return false;
  const auto &externalTables = programDescriptors[setIndex].d3d12ExternalTables;
  ID3D12DescriptorHeap *resourceHeap = nullptr, *samplerHeap = nullptr;
  if (!externalTables.empty()) {
    if (!getDescriptorArenaHeaps(device, &resourceHeap, &samplerHeap))
      return false;
    RID3D12_SetDescriptorHeaps(*cmd, resourceHeap, samplerHeap);
    if (compute) {
      cmd->d3d12.computePipelineBound = true;
      RID3D12_SetComputeRootSignature(*cmd, impl.d3d12.rootSignature);
    } else {
      cmd->d3d12.computePipelineBound = false;
      RID3D12_SetGraphicsRootSignature(*cmd, impl.d3d12.rootSignature);
    }
    const uint32_t resourceStride =
        device->d3d12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t samplerStride =
        device->d3d12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (const auto &table : externalTables) {
      const auto binding = std::find_if(
          bindless->d3d12.bindings.begin(), bindless->d3d12.bindings.end(),
          [&](const auto &candidate) {
            return candidate.binding == table.binding &&
                   candidate.registerClass == table.registerClass &&
                   candidate.registerIndex == table.registerIndex &&
                   candidate.registerSpace == table.registerSpace;
          });
      if (binding == bindless->d3d12.bindings.end())
        return false;
      const bool sampler =
          table.registerClass == RIBindlessRegisterClass::Sampler;
      const uint32_t offset =
          table.geometryHeap
              ? 0u
              : (sampler ? bindless->d3d12.allocation.samplerOffset
                         : bindless->d3d12.resourceTableBase) +
                    binding->descriptorOffset;
      const D3D12_GPU_DESCRIPTOR_HANDLE handle =
          sampler ? ri_d3d12_gpu(samplerHeap, offset, samplerStride)
                  : ri_d3d12_gpu(resourceHeap, offset, resourceStride);
      if (compute)
        RID3D12_SetComputeRootDescriptorTable(*cmd, table.rootParameter,
                                              handle);
      else
        RID3D12_SetGraphicsRootDescriptorTable(*cmd, table.rootParameter,
                                               handle);
    }
    bindless->d3d12.hasBeenBound = true;
    return true;
  }
  const bool hasResourceTable = bindless->d3d12.allocation.resourceCount != 0;
  const bool hasSamplerTable = bindless->d3d12.allocation.samplerCount != 0;
  const bool hasGeometryTable =
      impl.d3d12.geometryRootParameter != UINT32_MAX &&
      bindless->d3d12.geometryRangeCount != 0;
  if (hasResourceTable || hasSamplerTable || hasGeometryTable) {
    if (!getDescriptorArenaHeaps(device, &resourceHeap, &samplerHeap))
      return false;
    RID3D12_SetDescriptorHeaps(*cmd, resourceHeap, samplerHeap);
  }
  if (compute) {
    cmd->d3d12.computePipelineBound = true;
    RID3D12_SetComputeRootSignature(*cmd, impl.d3d12.rootSignature);
    const uint32_t rs = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t ss = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (impl.d3d12.resourceRootParameter != UINT32_MAX && hasResourceTable)
      RID3D12_SetComputeRootDescriptorTable(
          *cmd, impl.d3d12.resourceRootParameter,
          ri_d3d12_gpu(resourceHeap, bindless->d3d12.resourceTableBase, rs));
    if (impl.d3d12.geometryRootParameter != UINT32_MAX &&
        bindless->d3d12.geometryRangeCount)
      RID3D12_SetComputeRootDescriptorTable(*cmd,
                                            impl.d3d12.geometryRootParameter,
                                            ri_d3d12_gpu(resourceHeap, 0, rs));
    if (impl.d3d12.samplerRootParameter != UINT32_MAX && hasSamplerTable)
      RID3D12_SetComputeRootDescriptorTable(
          *cmd, impl.d3d12.samplerRootParameter,
          ri_d3d12_gpu(samplerHeap, bindless->d3d12.allocation.samplerOffset,
                       ss));
  } else {
    cmd->d3d12.computePipelineBound = false;
    RID3D12_SetGraphicsRootSignature(*cmd, impl.d3d12.rootSignature);
    const uint32_t rs = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t ss = device->d3d12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (impl.d3d12.resourceRootParameter != UINT32_MAX && hasResourceTable)
      RID3D12_SetGraphicsRootDescriptorTable(
          *cmd, impl.d3d12.resourceRootParameter,
          ri_d3d12_gpu(resourceHeap, bindless->d3d12.resourceTableBase, rs));
    if (impl.d3d12.geometryRootParameter != UINT32_MAX &&
        bindless->d3d12.geometryRangeCount)
      RID3D12_SetGraphicsRootDescriptorTable(*cmd,
                                             impl.d3d12.geometryRootParameter,
                                             ri_d3d12_gpu(resourceHeap, 0, rs));
    if (impl.d3d12.samplerRootParameter != UINT32_MAX && hasSamplerTable)
      RID3D12_SetGraphicsRootDescriptorTable(
          *cmd, impl.d3d12.samplerRootParameter,
          ri_d3d12_gpu(samplerHeap, bindless->d3d12.allocation.samplerOffset,
                       ss));
  }
  bindless->d3d12.hasBeenBound = true;
  return true;
}
#endif

#if (DEVICE_IMPL_D3D12)
static bool ri_d3d12_registerClass(VkDescriptorType type,
                                   RIBindlessRegisterClass &out) {
  switch (type) {
  case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
  case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    out = RIBindlessRegisterClass::CBV;
    return true;
  case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
  case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
    out = RIBindlessRegisterClass::UAV;
    return true;
  case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    out = RIBindlessRegisterClass::SRV;
    return true;
  case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    out = RIBindlessRegisterClass::UAV;
    return true;
  case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
    out = RIBindlessRegisterClass::SRV;
    return true;
  case VK_DESCRIPTOR_TYPE_SAMPLER:
    out = RIBindlessRegisterClass::Sampler;
    return true;
  default:
    return false;
  }
}

bool RIBindlessDescriptorSet::convertBinding(const Binding &binding,
                                             BackendBinding &out) {
  RIBindlessRegisterClass registerClass;
  if (!ri_d3d12_registerClass(binding.descriptorType, registerClass) ||
      binding.descriptorCount == 0)
    return false;
  out.binding = binding.binding;
  out.registerClass = registerClass;
  out.descriptorCount = binding.descriptorCount;
  // Vulkan bindless bindings conventionally use binding == register index;
  // register-space remapping is supplied by the reflected D3D12 layout.
  out.registerIndex = binding.binding;
  out.registerSpace = 0;
  out.descriptorType =
      binding.d3d12DescriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM
          ? binding.descriptorType
          : binding.d3d12DescriptorType;
  out.srvDimension = binding.d3d12SrvDimension;
  out.uavDimension = binding.d3d12UavDimension;
  out.format = binding.d3d12Format;
  // Legacy Binding initializers do not carry D3D12 view metadata.  Keep that
  // source-compatible form useful with a legal typed 2D null; reflected
  // callers override these fields with the exact dimension and format.
  if (out.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
      out.srvDimension == D3D12_SRV_DIMENSION_BUFFER) {
    out.srvDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    if (out.format == DXGI_FORMAT_UNKNOWN)
      out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  if (out.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
      out.uavDimension == D3D12_UAV_DIMENSION_BUFFER) {
    out.uavDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (out.format == DXGI_FORMAT_UNKNOWN)
      out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  if (out.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
    out.srvDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  return true;
}

static void
ri_d3d12_null_srv(D3D12_SHADER_RESOURCE_VIEW_DESC &desc,
                  const RIBindlessDescriptorSet::BackendBinding &binding) {
  desc = {};
  desc.Format = binding.format;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.ViewDimension = binding.srvDimension;
  switch (binding.srvDimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    desc.Format = binding.format == DXGI_FORMAT_UNKNOWN
                      ? DXGI_FORMAT_R32_TYPELESS
                      : binding.format;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = desc.Format == DXGI_FORMAT_R32_TYPELESS
                            ? D3D12_BUFFER_SRV_FLAG_RAW
                            : D3D12_BUFFER_SRV_FLAG_NONE;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    desc.Texture1D.MipLevels = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    desc.Texture1DArray.MipLevels = 1;
    desc.Texture1DArray.ArraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    desc.Texture2D.MipLevels = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    desc.Texture2DArray.MipLevels = 1;
    desc.Texture2DArray.ArraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    desc.TextureCube.MipLevels = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    desc.TextureCubeArray.MipLevels = 1;
    desc.TextureCubeArray.NumCubes = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    desc.Texture3D.MipLevels = 1;
    break;
  case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
    desc.RaytracingAccelerationStructure.Location = 0;
    break;
  default:
    break;
  }
}

static bool
ri_d3d12_null_uav(D3D12_UNORDERED_ACCESS_VIEW_DESC &desc,
                  const RIBindlessDescriptorSet::BackendBinding &binding) {
  desc = {};
  desc.Format = binding.format;
  desc.ViewDimension = binding.uavDimension;
  switch (binding.uavDimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
    desc.Format = binding.format == DXGI_FORMAT_UNKNOWN
                      ? DXGI_FORMAT_R32_TYPELESS
                      : binding.format;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = desc.Format == DXGI_FORMAT_R32_TYPELESS
                            ? D3D12_BUFFER_UAV_FLAG_RAW
                            : D3D12_BUFFER_UAV_FLAG_NONE;
    return true;
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    return true;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    desc.Texture1DArray.ArraySize = 1;
    return true;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    return true;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    desc.Texture2DArray.ArraySize = 1;
    return true;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    desc.Texture3D.WSize = 1;
    return true;
  default:
    return false;
  }
}

// Write the typed null descriptor for `binding` into `cpu`. Shared by
// initialize()'s one-time table fill and by the release path in
// writeDescriptors(), so a slot handed back to the pool reads exactly like one
// that was never written — which is what lets the next occupant claim it.
static bool
ri_d3d12_write_null_slot(RIDevice *device,
                         const RIBindlessDescriptorSet::BackendBinding &binding,
                         D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
  switch (binding.registerClass) {
  case RIBindlessRegisterClass::Sampler: {
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    device->d3d12.device->CreateSampler(&desc, cpu);
    return true;
  }
  case RIBindlessRegisterClass::CBV: {
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
    desc.BufferLocation = 0;
    desc.SizeInBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    device->d3d12.device->CreateConstantBufferView(&desc, cpu);
    return true;
  }
  case RIBindlessRegisterClass::SRV:
  case RIBindlessRegisterClass::UAV:
    if (binding.descriptorType ==
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
      desc.ViewDimension =
          D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
      desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      desc.RaytracingAccelerationStructure.Location = 0;
      device->d3d12.device->CreateShaderResourceView(nullptr, &desc, cpu);
      return true;
    }
    if (binding.registerClass == RIBindlessRegisterClass::SRV) {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
      ri_d3d12_null_srv(desc, binding);
      device->d3d12.device->CreateShaderResourceView(nullptr, &desc, cpu);
      return true;
    }
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
      if (!ri_d3d12_null_uav(desc, binding))
        return false;
      device->d3d12.device->CreateUnorderedAccessView(nullptr, nullptr, &desc,
                                                      cpu);
    }
    return true;
  default:
    return false;
  }
}

bool RIBindlessDescriptorSet::initialize(
    RIDevice *device, std::span<const BackendBinding> bindings,
    const RIBindlessD3D12Layout &layout) {
  if (!device || d3d12.ownsAllocation)
    return false;
  uint32_t resources = 0, samplers = 0;
  bool hasGeometryBinding = false;
  auto isGeometryBinding = [](const BackendBinding &binding) {
    return binding.geometryHeap;
  };
  std::unordered_set<uint32_t> seenBindings;
  for (const BackendBinding &binding : bindings) {
    const bool geometryBinding = isGeometryBinding(binding);
    if (!binding.descriptorCount ||
        !seenBindings.insert(binding.binding).second)
      return false;
    if (!geometryBinding &&
        binding.registerIndex > UINT32_MAX - binding.descriptorCount)
      return false;
    if (binding.registerClass == RIBindlessRegisterClass::Sampler) {
      if (binding.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER)
        return false;
    } else if (binding.registerClass == RIBindlessRegisterClass::CBV) {
      if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
          binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        return false;
    } else if (binding.registerClass == RIBindlessRegisterClass::SRV) {
      if (binding.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
          binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
          binding.descriptorType !=
              VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
        return false;
      if (binding.descriptorType ==
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        if (binding.srvDimension !=
            D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
          return false;
      } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                 (binding.srvDimension == D3D12_SRV_DIMENSION_UNKNOWN ||
                  binding.srvDimension == D3D12_SRV_DIMENSION_BUFFER ||
                  binding.format == DXGI_FORMAT_UNKNOWN))
        return false;
      if (geometryBinding)
        hasGeometryBinding = true;
    } else if (binding.registerClass == RIBindlessRegisterClass::UAV) {
      if (binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
          binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        return false;
      if (binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
          (binding.uavDimension == D3D12_UAV_DIMENSION_UNKNOWN ||
           binding.uavDimension == D3D12_UAV_DIMENSION_BUFFER ||
           binding.format == DXGI_FORMAT_UNKNOWN))
        return false;
    } else
      return false;
    // Geometry is backed by the dedicated device-wide arena and does not
    // consume slots in the ordinary descriptor-set allocation.
    const uint32_t descriptorSpan =
        geometryBinding ? 0u : binding.descriptorCount;
    switch (binding.registerClass) {
    case RIBindlessRegisterClass::Sampler:
      if (samplers > UINT32_MAX - descriptorSpan)
        return false;
      samplers += descriptorSpan;
      break;
    case RIBindlessRegisterClass::CBV:
    case RIBindlessRegisterClass::SRV:
    case RIBindlessRegisterClass::UAV:
      if (resources > UINT32_MAX - descriptorSpan)
        return false;
      resources += descriptorSpan;
      break;
    default:
      return false;
    }
  }
  if (hasGeometryBinding && (layout.geometryRangeOffset != 0 ||
                             layout.geometryRangeCount != UINT_MAX))
    return false;
  // Geometry handles are absolute indices into the dedicated geometry arena.
  // Keep that table independent from ordinary descriptor-set allocations.
  if (!allocateDescriptorArena(device, resources, samplers,
                               &d3d12.allocation)) {
    return false;
  }
  d3d12.rootSignature = layout.rootSignature;
  d3d12.resourceRootParameter = layout.resourceRootParameter;
  d3d12.samplerRootParameter = layout.samplerRootParameter;
  d3d12.geometryRootParameter = layout.geometryRootParameter;
  d3d12.geometryRangeOffset = layout.geometryRangeOffset;
  d3d12.geometryRangeCount = layout.geometryRangeCount;
  d3d12.resourceTableBase = d3d12.allocation.resourceOffset;
  // Geometry descriptors are populated in the device-wide geometry arena;
  // this set owns only its ordinary table allocation.
  d3d12.usesGeometryAllocation = false;
  d3d12.ownsAllocation = true;
  d3d12.bindings.assign(bindings.begin(), bindings.end());
  d3d12.resources.assign(resources, nullptr);
  d3d12.allocations.assign(resources, nullptr);
  uint32_t resourceBindingOffset = 0, samplerBindingOffset = 0;
  for (BackendBinding &binding : d3d12.bindings) {
    binding.descriptorOffset =
        isGeometryBinding(binding)
            ? 0u
            : (binding.registerClass == RIBindlessRegisterClass::Sampler
                   ? samplerBindingOffset
                   : resourceBindingOffset);
    if (binding.registerClass == RIBindlessRegisterClass::Sampler) {
      const uint32_t descriptorSpan =
          isGeometryBinding(binding) ? 0u : binding.descriptorCount;
      if (samplerBindingOffset > UINT32_MAX - descriptorSpan) {
        destroy(device);
        return false;
      }
      samplerBindingOffset += descriptorSpan;
    } else {
      const uint32_t descriptorSpan =
          isGeometryBinding(binding) ? 0u : binding.descriptorCount;
      if (resourceBindingOffset > UINT32_MAX - descriptorSpan) {
        destroy(device);
        return false;
      }
      resourceBindingOffset += descriptorSpan;
    }
  }
  ID3D12DescriptorHeap *resourceHeap = nullptr, *samplerHeap = nullptr;
  if (!getDescriptorArenaHeaps(device, &resourceHeap, &samplerHeap)) {
    destroy(device);
    return false;
  }
  const uint32_t resourceStride =
      device->d3d12.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const uint32_t samplerStride =
      device->d3d12.device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  uint32_t resourceOffset = d3d12.resourceTableBase;
  uint32_t samplerOffset = d3d12.allocation.samplerOffset;
  for (const BackendBinding &binding : d3d12.bindings) {
    // The geometry range is backed by the device-wide raw-SRV arena. It is
    // reflected/bound in this table, but must not be initialized as an
    // ordinary descriptor array (its reflected count is UINT_MAX).
    if (isGeometryBinding(binding))
      continue;
    const uint32_t descriptorSpan = binding.descriptorCount;
    if (binding.registerClass == RIBindlessRegisterClass::Sampler)
      samplerOffset = d3d12.allocation.samplerOffset + binding.descriptorOffset;
    else
      resourceOffset = d3d12.resourceTableBase + binding.descriptorOffset;
    for (uint32_t n = 0; n < descriptorSpan; ++n) {
      const bool isSampler =
          binding.registerClass == RIBindlessRegisterClass::Sampler;
      const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
          isSampler
              ? ri_d3d12_cpu(samplerHeap, samplerOffset++, samplerStride)
              : ri_d3d12_cpu(resourceHeap, resourceOffset++, resourceStride);
      if (!ri_d3d12_write_null_slot(device, binding, cpu)) {
        // An unsupported UAV shape is the only recoverable case here; the
        // original code tore the set down for it, so keep that behaviour.
        if (binding.registerClass == RIBindlessRegisterClass::UAV)
          destroy(device);
        return false;
      }
    }
  }
  return true;
}

static const RIBindlessDescriptorSet::BackendBinding *
ri_find_binding(const RIBindlessDescriptorSet &set, uint32_t binding) {
  for (const auto &candidate : set.d3d12.bindings)
    if (candidate.binding == binding)
      return &candidate;
  return nullptr;
}
#endif

void RIBindlessDescriptorSet::initialize(
    RIDevice *device, std::span<const Binding> bindings,
    std::span<const VkDescriptorPoolSize> poolSizes) {
  std::vector<VkDescriptorSetLayoutBinding> lbBindings(bindings.size());
  std::vector<VkDescriptorBindingFlags> lbFlags(bindings.size());

  for (size_t i = 0; i < bindings.size(); ++i) {
    lbBindings[i].binding = bindings[i].binding;
    lbBindings[i].descriptorType = bindings[i].descriptorType;
    lbBindings[i].descriptorCount = bindings[i].descriptorCount;
    lbBindings[i].stageFlags = bindings[i].stageFlags;
    lbBindings[i].pImmutableSamplers = nullptr;
    lbFlags[i] = bindings[i].flags;
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flagsInfo.bindingCount = (uint32_t)bindings.size();
  flagsInfo.pBindingFlags = lbFlags.data();

  VkDescriptorSetLayoutCreateInfo layoutInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutInfo.bindingCount = (uint32_t)lbBindings.size();
  layoutInfo.pBindings = lbBindings.data();
  layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layoutInfo.pNext = &flagsInfo;
  VK_WrapResult(vkCreateDescriptorSetLayout(device->vk.device, &layoutInfo,
                                            NULL, &vk.m_bindlessSetLayout));

  VkDescriptorPoolCreateInfo poolInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
  poolInfo.pPoolSizes = poolSizes.data();
  VK_WrapResult(vkCreateDescriptorPool(device->vk.device, &poolInfo, NULL,
                                       &vk.m_bindlessPool));

  VkDescriptorSetAllocateInfo setAlloc = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setAlloc.descriptorPool = vk.m_bindlessPool;
  setAlloc.descriptorSetCount = 1;
  setAlloc.pSetLayouts = &vk.m_bindlessSetLayout;
  VK_WrapResult(vkAllocateDescriptorSets(device->vk.device, &setAlloc,
                                         &vk.m_bindlessSet));
}

void RIBindlessDescriptorSet::destroy(RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (vk.m_bindlessPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device->vk.device, vk.m_bindlessPool, NULL);
    vk.m_bindlessPool = VK_NULL_HANDLE;
    vk.m_bindlessSet = VK_NULL_HANDLE;
  }
  if (vk.m_bindlessSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device->vk.device, vk.m_bindlessSetLayout,
                                 NULL);
    vk.m_bindlessSetLayout = VK_NULL_HANDLE;
  }
#endif
#if (DEVICE_IMPL_D3D12)
  for (ID3D12Resource *resource : d3d12.resources)
    if (resource)
      resource->Release();
  d3d12.resources.clear();
  for (D3D12MA::Allocation *allocation : d3d12.allocations)
    if (allocation)
      allocation->Release();
  d3d12.allocations.clear();
  if (d3d12.ownsAllocation) {
    if (d3d12.usesGeometryAllocation)
      releaseGeometryDescriptorArena(device, &d3d12.allocation);
    else {
      const RIDescriptorArenaFence retire =
          ri_d3d12_graphics_retire_fence(device);
      releaseDescriptorArena(device, &d3d12.allocation, &retire);
    }
    if (d3d12.geometryAllocation.resourceCount)
      releaseGeometryDescriptorArena(device, &d3d12.geometryAllocation);
    d3d12.ownsAllocation = false;
  }
  d3d12 = {};
#endif
}

bool RIBindlessDescriptorSet::writeDescriptors(
    RIDevice *device, std::span<const WriteBinding> writes,
    const RIDescriptorArenaFence *completionFence) {
#if (DEVICE_IMPL_VULKAN)
#if DEVICE_MULTI_BACKEND
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
#endif
    if (writes.empty())
      return true;

    std::vector<VkWriteDescriptorSet> vkWrites(writes.size());
    // Transient handle payloads must outlive the vkUpdateDescriptorSets call;
    // sized for the worst case so pointers into the vectors stay stable across
    // reallocs. The new RIDescriptor stores RI-object pointers, so handles are
    // pulled here via the accessors.
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelWrites(
        writes.size());
    std::vector<VkDescriptorImageInfo> imageInfos(writes.size());
    std::vector<VkDescriptorBufferInfo> bufferInfos(writes.size());
    std::vector<VkAccelerationStructureKHR> accelHandles(writes.size());
    // An empty descriptor RELEASES a slot, and on Vulkan that is a no-op: the
    // set retains no per-slot reference to drop (only the D3D12 arm tracks
    // `resources[]`), and every bindless array binding carries
    // PARTIALLY_BOUND, so an element no shader indexes is never read whatever
    // it still holds. Skipping is not just an optimization -- translating one
    // would submit a SAMPLED_IMAGE write with a VK_NULL_HANDLE imageView
    // (an empty RIDescriptor's `type` is 0, which IS SAMPLED_IMAGE), and
    // nothing here enables robustness2/nullDescriptor.
    //
    // Empties are compacted out rather than left as holes: `count` indexes the
    // payload vectors, which stay sized for the worst case so the pImageInfo /
    // pBufferInfo pointers stored below cannot be invalidated by a realloc.
    size_t count = 0;
    for (size_t i = 0; i < writes.size(); ++i) {
      const WriteBinding &w = writes[i];
      const struct RIDescriptor &d = w.descriptor;
      if (d.isEmpty())
        continue;
      VkWriteDescriptorSet &vkDesc = vkWrites[count];
      vkDesc = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      vkDesc.dstSet = vk.m_bindlessSet;
      vkDesc.dstBinding = w.binding;
      vkDesc.dstArrayElement = w.arrayElement;
      vkDesc.descriptorCount = 1;
      vkDesc.descriptorType = ri_vk_BindlessDescriptorType(d.type);
      switch ((enum RIDescriptorType_e)d.type) {
      case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        bufferInfos[count] = {d.vkBuffer(), d.payload.buffer.offset,
                              d.payload.buffer.range};
        vkDesc.pBufferInfo = &bufferInfos[count];
        break;
      case RI_DESCRIPTOR_TYPE_SAMPLER:
      case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        imageInfos[count] = d.vk.image;
        vkDesc.pImageInfo = &imageInfos[count];
        break;
      case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
        accelHandles[count] = d.vkAccel();
        VkWriteDescriptorSetAccelerationStructureKHR &aw = accelWrites[count];
        aw = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        aw.accelerationStructureCount = 1;
        aw.pAccelerationStructures = &accelHandles[count];
        vkDesc.pNext = &aw;
        break;
      }
      default:
        assert(false && "RIBindlessDescriptorSet::writeDescriptors: "
                        "unsupported descriptor type");
        break;
      }
      ++count;
    }
    if (count == 0)
      return true;
    vkUpdateDescriptorSets(device->vk.device, (uint32_t)count, vkWrites.data(),
                           0, NULL);
    return true;
#if DEVICE_MULTI_BACKEND
  }
#endif
#endif
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    if (!device || !d3d12.ownsAllocation)
      return false;
    if (writes.empty())
      return true;

    // The fence gate below protects descriptors that submitted work may still
    // read. It is per SLOT, not per set: an array element that has never been
    // written still holds the null descriptor initialize() placed there, so no
    // command list can be reading anything through it and filling it in is safe
    // at any time. Only overwriting a slot that already resolves to a resource
    // needs the fence. Applying the gate to the whole set instead rejected
    // every texture loaded after the first frame that bound the set, leaving
    // those slots null while their index still looked valid to the shader. A
    // slot is not recycled until graphicsDefer has drained the index return
    // past the frames in flight (see ReleaseImageBindlessSlot), so a freshly
    // allocated element is never one an in-flight frame indexes.
    const bool fenceCompleted =
        completionFence && completionFence->fence &&
        completionFence->fence->GetCompletedValue() >= completionFence->value;

    // Validate every write before touching a descriptor or retaining a new
    // resource. Unsupported image/AS descriptors are rejected rather than
    // being presented as D3D12 support by accident.
    bool allWritesValid = true;
    for (const WriteBinding &write : writes) {
      const auto *binding = ri_find_binding(*this, write.binding);
      bool valid = binding && write.arrayElement <
                                  (binding ? binding->descriptorCount : 0);
      if (valid && binding->descriptorOffset > UINT32_MAX - write.arrayElement)
        valid = false;
      // A geometry binding is backed by the device-wide raw-SRV arena, not by
      // this set's allocation: initialize() skips it and forces its
      // descriptorOffset to 0. Writing one here would resolve to
      // resourceTableBase + arrayElement and silently clobber an unrelated
      // ordinary descriptor -- in GlobalManagedSets, the textures_2d array.
      // Its reflected descriptorCount is UINT_MAX, so the range check above
      // catches nothing.
      if (valid && binding->geometryHeap)
        valid = false;
      if (valid && !write.descriptor.isEmpty()) {
        const enum RIDescriptorType_e type =
            static_cast<RIDescriptorType_e>(write.descriptor.type);
        const bool descriptorClassOk =
            (binding->registerClass == RIBindlessRegisterClass::CBV &&
             type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ||
            (binding->registerClass == RIBindlessRegisterClass::SRV &&
             (type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
              type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
              type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE)) ||
            (binding->registerClass == RIBindlessRegisterClass::UAV &&
             (type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
              type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE)) ||
            (binding->registerClass == RIBindlessRegisterClass::Sampler &&
             type == RI_DESCRIPTOR_TYPE_SAMPLER);
        valid =
            IsValidRIDescriptorType(write.descriptor.type) && descriptorClassOk;
        if (valid &&
            binding->registerClass == RIBindlessRegisterClass::Sampler) {
          valid = type == RI_DESCRIPTOR_TYPE_SAMPLER &&
                  write.descriptor.payload.sampler.initialized != 0 &&
                  write.descriptor.payload.sampler.d3d12Desc.MaxLOD >=
                      write.descriptor.payload.sampler.d3d12Desc.MinLOD;
        } else if (valid &&
                   binding->registerClass == RIBindlessRegisterClass::CBV) {
          const auto &buffer = write.descriptor.payload.buffer;
          const bool roundable = buffer.range <= UINT64_MAX - 255u;
          const uint64_t rounded =
              roundable ? ((buffer.range + 255u) & ~UINT64(255u)) : 0;
          valid =
              type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
              buffer.nativeResource != nullptr && buffer.size != 0 &&
              buffer.range != 0 && rounded != 0 && rounded <= UINT_MAX &&
              buffer.offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT ==
                  0 &&
              buffer.offset <= buffer.size &&
              rounded <= buffer.size - buffer.offset;
        } else if (valid &&
                   (binding->registerClass == RIBindlessRegisterClass::SRV ||
                    binding->registerClass == RIBindlessRegisterClass::UAV)) {
          if (type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) {
            valid = binding->registerClass == RIBindlessRegisterClass::SRV &&
                    write.descriptor.payload.accel.gpuVA != 0 &&
                    write.descriptor.payload.accel.nativeResource != nullptr;
          } else if (type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                     type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const bool sampled = type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            const auto &texture = write.descriptor.payload.texture;
            ID3D12Resource *resource = texture.nativeResource;
            if ((sampled &&
                 binding->registerClass != RIBindlessRegisterClass::SRV) ||
                (!sampled &&
                 binding->registerClass != RIBindlessRegisterClass::UAV) ||
                !resource || texture.nativeFormat == DXGI_FORMAT_UNKNOWN ||
                !texture.mipNum || !texture.layerNum) {
              valid = false;
            } else {
              const D3D12_RESOURCE_DESC rd = resource->GetDesc();
              D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
              D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
              const bool viewOk =
                  sampled ? ri_d3d12_texture_view(srv, texture)
                          : ri_d3d12_texture_uav(uav, texture, resource);
              // Layout pinned to COMMON, so SRV and UAV access coexist and a
              // combined read+write state is legal when sampling; see the
              // matching allowance in bindDescriptors' validBinding.
              const bool simultaneous =
                  (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) !=
                  0;
              const bool stateOk =
                  sampled ? simultaneous ||
                                ((texture.state &
                                  RI_RESOURCE_STATE_SHADER_RESOURCE) != 0 &&
                                 !(texture.state &
                                   (RI_RESOURCE_STATE_STORAGE_READ |
                                    RI_RESOURCE_STATE_STORAGE_WRITE |
                                    RI_RESOURCE_STATE_COPY_DST |
                                    RI_RESOURCE_STATE_COPY_SRC)))
                          : (texture.state &
                             (RI_RESOURCE_STATE_GENERAL |
                              RI_RESOURCE_STATE_STORAGE_READ |
                              RI_RESOURCE_STATE_STORAGE_WRITE |
                              RI_RESOURCE_STATE_UNORDERED_ACCESS)) != 0;
              const auto viewDimension =
                  sampled ? srv.ViewDimension : uav.ViewDimension;
              const auto viewFormat = sampled ? srv.Format : uav.Format;
              const auto viewType =
                  static_cast<RITextureViewType_e>(texture.viewType);
              const bool resourceDimensionOk =
                  ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) &&
                   (viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D ||
                    viewType ==
                        RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY)) ||
                  ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) &&
                   (viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D ||
                    viewType ==
                        RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY)) ||
                  ((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) &&
                   (viewType == RI_VIEWTYPE_SHADER_RESOURCE_3D ||
                    viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D));
              const bool layerRangeOk =
                  rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                      ? (texture.baseLayer == 0 && texture.layerNum == 1)
                      : (texture.baseLayer < rd.DepthOrArraySize &&
                         texture.layerNum <=
                             rd.DepthOrArraySize - texture.baseLayer);
              // binding->format only types the NULL descriptor written at
              // initialize(); a sampled Texture2D slot accepts any SRV format,
              // so bindless texture writes (BC, sRGB, R8, ...) must not be held
              // to it. Typed UAVs still need the declared format.
              const bool formatOk = sampled ? viewFormat != DXGI_FORMAT_UNKNOWN
                                            : viewFormat == binding->format;
              valid =
                  viewOk && stateOk && layerRangeOk && resourceDimensionOk &&
                  viewDimension == (sampled ? binding->srvDimension
                                            : binding->uavDimension) &&
                  formatOk && texture.baseMip < rd.MipLevels &&
                  texture.mipNum <= rd.MipLevels - texture.baseMip &&
                  (!sampled ||
                   !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) &&
                  (sampled ||
                   (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
            }
          } else {
            const auto &buffer = write.descriptor.payload.buffer;
            // This API has no shader reflection.  Keep the legacy four-argument
            // storageBuffer form valid by treating an entirely unspecified
            // shape as raw; explicit stride/structured metadata remains
            // honored.
            const bool raw = buffer.raw || (!buffer.raw && !buffer.structured &&
                                            buffer.stride == 0);
            const bool structured = !raw;
            const uint32_t stride = raw ? 0 : buffer.stride;
            const uint64_t elementSize = raw ? 4 : stride;
            const uint64_t firstElement =
                elementSize ? buffer.offset / elementSize : 0;
            const uint64_t elementCount =
                elementSize ? buffer.range / elementSize : 0;
            valid =
                type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                buffer.nativeResource != nullptr && buffer.size != 0 &&
                buffer.range != 0 && buffer.offset <= buffer.size &&
                buffer.range <= buffer.size - buffer.offset &&
                !(buffer.raw && buffer.structured) && elementSize != 0 &&
                firstElement <= UINT_MAX && elementCount != 0 &&
                elementCount <= UINT_MAX &&
                ((raw && buffer.offset % 4 == 0 && buffer.range % 4 == 0) ||
                 (structured && stride != 0 && buffer.offset % stride == 0 &&
                  buffer.range % stride == 0));
          }
        } else {
          valid = false;
        }
      }
      if (!valid) {
        hpl::Warning(
            "RIBindlessDescriptorSet::writeDescriptors (D3D12): rejected "
            "write binding=%u element=%u type=%u%s\n",
            write.binding, write.arrayElement, unsigned(write.descriptor.type),
            binding ? "" : " (binding not in set)");
        allWritesValid = false;
        continue;
      }
      // Sampler bindings keep no per-slot resource record, so they stay gated
      // as a whole once the set has been bound. An out-of-range slot is treated
      // as live for the same reason: the write loop below would skip the
      // retain, so there is no evidence the element is untouched.
      if (d3d12.hasBeenBound && !fenceCompleted &&
          !write.descriptor.isEmpty()) {
        const uint64_t slot =
            uint64_t(binding->descriptorOffset) + write.arrayElement;
        const bool live =
            binding->registerClass == RIBindlessRegisterClass::Sampler ||
            slot >= d3d12.resources.size() ||
            d3d12.resources[size_t(slot)] != nullptr;
        if (live) {
          hpl::Warning(
              "RIBindlessDescriptorSet::writeDescriptors (D3D12): rejected "
              "write binding=%u element=%u; the slot is live and no "
              "completed fence was supplied\n",
              write.binding, write.arrayElement);
          allWritesValid = false;
        }
      }
    }
    if (!allWritesValid)
      return false;
    ID3D12DescriptorHeap *resourceHeap = nullptr, *samplerHeap = nullptr;
    if (!getDescriptorArenaHeaps(device, &resourceHeap, &samplerHeap))
      return false;
    const uint32_t resourceStride =
        device->d3d12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t samplerStride =
        device->d3d12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (const WriteBinding &write : writes) {
      const auto *binding = ri_find_binding(*this, write.binding);
      const uint32_t base =
          binding->registerClass == RIBindlessRegisterClass::Sampler
              ? d3d12.allocation.samplerOffset
              : d3d12.resourceTableBase;
      uint32_t offset = base + binding->descriptorOffset + write.arrayElement;
      // An empty descriptor RELEASES the slot: restore the typed null
      // descriptor initialize() put there and drop the retained resource, so
      // the slot stops counting as live. Without this the index can be returned
      // to its pool and handed out again while `resources[slot]` still holds
      // the old resource, and the fence gate above then rejects every write the
      // next occupant makes — leaving it reading the previous texture. The gate
      // deliberately exempts empty descriptors, so a release needs no fence;
      // callers must still only release once the GPU is past the frames that
      // referenced the slot (see cTextureManager::ReturnBindlessSlot, which runs
      // from the graphicsDefer drain).
      if (write.descriptor.isEmpty()) {
        const uint32_t resourceSlot =
            binding->descriptorOffset + write.arrayElement;
        const bool isSampler =
            binding->registerClass == RIBindlessRegisterClass::Sampler;
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            isSampler ? ri_d3d12_cpu(samplerHeap, offset, samplerStride)
                      : ri_d3d12_cpu(resourceHeap, offset, resourceStride);
        if (!ri_d3d12_write_null_slot(device, *binding, cpu))
          return false;
        if (!isSampler && resourceSlot < d3d12.resources.size())
          ri_d3d12_replace_slot(d3d12.resources[resourceSlot],
                                d3d12.allocations[resourceSlot], nullptr,
                                nullptr);
        continue;
      }
      if (binding->registerClass == RIBindlessRegisterClass::Sampler) {
        if (write.descriptor.type != RI_DESCRIPTOR_TYPE_SAMPLER ||
            !write.descriptor.payload.sampler.initialized ||
            write.descriptor.payload.sampler.d3d12Desc.MaxLOD <
                write.descriptor.payload.sampler.d3d12Desc.MinLOD)
          return false;
        device->d3d12.device->CreateSampler(
            &write.descriptor.payload.sampler.d3d12Desc,
            ri_d3d12_cpu(samplerHeap, offset, samplerStride));
        continue;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE cpu =
          ri_d3d12_cpu(resourceHeap, offset, resourceStride);
      const RIDescriptor &descriptor = write.descriptor;
      if (binding->registerClass == RIBindlessRegisterClass::CBV &&
          descriptor.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
        desc.BufferLocation = descriptor.payload.buffer.nativeResource
                                  ? descriptor.payload.buffer.nativeResource
                                            ->GetGPUVirtualAddress() +
                                        descriptor.payload.buffer.offset
                                  : 0;
        desc.SizeInBytes =
            (UINT)((descriptor.payload.buffer.range + 255) & ~UINT64(255));
        device->d3d12.device->CreateConstantBufferView(&desc, cpu);
      } else if (descriptor.type == RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE &&
                 binding->registerClass == RIBindlessRegisterClass::SRV) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.ViewDimension =
            D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.RaytracingAccelerationStructure.Location =
            descriptor.payload.accel.gpuVA;
        device->d3d12.device->CreateShaderResourceView(nullptr, &desc, cpu);
      } else if (descriptor.type == RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                 binding->registerClass == RIBindlessRegisterClass::SRV) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        if (!ri_d3d12_texture_view(desc, descriptor.payload.texture))
          return false;
        device->d3d12.device->CreateShaderResourceView(
            descriptor.payload.texture.nativeResource, &desc, cpu);
      } else if (descriptor.type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                 binding->registerClass == RIBindlessRegisterClass::UAV) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        if (!ri_d3d12_texture_uav(desc, descriptor.payload.texture,
                                  descriptor.payload.texture.nativeResource))
          return false;
        device->d3d12.device->CreateUnorderedAccessView(
            descriptor.payload.texture.nativeResource, nullptr, &desc, cpu);
      } else if ((binding->registerClass == RIBindlessRegisterClass::SRV ||
                  binding->registerClass == RIBindlessRegisterClass::UAV) &&
                 (descriptor.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                  descriptor.type == RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) {
        const bool raw = descriptor.payload.buffer.raw ||
                         (!descriptor.payload.buffer.raw &&
                          !descriptor.payload.buffer.structured &&
                          descriptor.payload.buffer.stride == 0);
        const uint32_t stride = raw ? 0 : descriptor.payload.buffer.stride;
        const uint64_t elements =
            descriptor.payload.buffer.range / (raw ? 4 : stride);
        if (binding->registerClass == RIBindlessRegisterClass::SRV) {
          D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
          desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          desc.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
          desc.Buffer.FirstElement =
              descriptor.payload.buffer.offset / (raw ? 4 : stride);
          desc.Buffer.NumElements = (UINT)elements;
          desc.Buffer.StructureByteStride = raw ? 0 : stride;
          desc.Buffer.Flags =
              raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
          desc.Shader4ComponentMapping =
              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          device->d3d12.device->CreateShaderResourceView(
              descriptor.payload.buffer.nativeResource, &desc, cpu);
        } else {
          D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
          desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          desc.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
          desc.Buffer.FirstElement =
              descriptor.payload.buffer.offset / (raw ? 4 : stride);
          desc.Buffer.NumElements = (UINT)elements;
          desc.Buffer.StructureByteStride = raw ? 0 : stride;
          desc.Buffer.Flags =
              raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
          device->d3d12.device->CreateUnorderedAccessView(
              descriptor.payload.buffer.nativeResource, nullptr, &desc, cpu);
        }
      }

      const uint32_t resourceSlot =
          binding->descriptorOffset + write.arrayElement;
      if (resourceSlot < d3d12.resources.size()) {
        ID3D12Resource *resource = nullptr;
        D3D12MA::Allocation *allocation = nullptr;
        ri_d3d12_descriptor_identity(descriptor, &resource, &allocation);
        ri_d3d12_replace_slot(d3d12.resources[resourceSlot],
                              d3d12.allocations[resourceSlot], resource,
                              allocation);
      }
    }
    return true;
  }
#endif
  (void)device;
  (void)writes;
  (void)completionFence;
  return false;
}

const struct RIProgram::BindingReflection *
RIProgram::findReflection(const struct DescriptorBindingID &handle) {
  for (auto &ref : bindingReflection) {
    if (ref.hash == handle.hash) {
      return &ref;
    }
  }
  return NULL;
}

const struct RIProgram::BindingReflection *
RIProgram::findReflectionBySlot(uint32_t set, uint32_t binding) {
  for (auto &ref : bindingReflection) {
    if (ref.set == set && ref.baseRegisterIndex == binding)
      return &ref;
  }
  return NULL;
}

RIProgram::ShaderArtifact
RIProgram::loadShaderArtifact(cFileSearcher *searcher, const tString &asName,
                              const char *entryPoint) {
  ShaderArtifact artifact;
  // Callers name shaders logically; the active backend picks the extension.
  // Both producers emit the same basename into their own folder, so no
  // filename surgery or directory scan is needed to find the artifact.
  const bool d3d12 = RIIsTargetSelected(RI_DEVICE_API_D3D12);
  tString artifactName = asName + (d3d12 ? ".dxil" : ".spv");
  // A multi-entry D3D12 source also emits a per-entry executable beside the
  // lib_6_8 library, because graphics and compute PSOs cannot consume a
  // library. Prefer it when one exists.
  if (d3d12 && entryPoint && *entryPoint) {
    const tString perEntry = asName + "." + entryPoint + ".dxil";
    if (searcher->GetFilePath(perEntry) != _W(""))
      artifactName = perEntry;
  }
  tWString sPath = searcher->GetFilePath(artifactName);
  if (sPath == _W("")) {
    FatalError("Couldn't find shader artifact '%s' in resources!\n",
               artifactName.c_str());
    return artifact;
  }
  unsigned int fileSize = cPlatform::GetFileSize(sPath);
  if (fileSize == 0)
    FatalError("RIProgram: shader artifact '%s' is empty\n",
               artifactName.c_str());
  auto bytes = std::make_shared<std::vector<char>>(fileSize);
  if (!cPlatform::CopyFileToBuffer(sPath, bytes->data(), fileSize))
    FatalError("RIProgram: could not read shader artifact '%s'\n",
               artifactName.c_str());
  artifact.format = ri_detectShaderFormat(
      std::span<const char>(bytes->data(), bytes->size()));
  artifact.bytes = bytes;
  const ShaderArtifact &result = artifact;
  RIShaderArtifactFormat metadataFormat = artifact.format;
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    std::string reflectionJson;
    RIShaderArtifactMeta artifactMeta;
    const RIShaderArtifactFormat format = ri_readShaderMetadata(
        searcher, artifactName, result.size(), &reflectionJson, &artifactMeta);
    metadataFormat = format;
    const RIShaderArtifactFormat expected = RIShaderArtifactFormat::Dxil;
    if (format != expected)
      FatalError("RIProgram: shader metadata format does not match active "
                 "backend for '%s'\n",
                 artifactName.c_str());
    if (RIIsTargetSelected(RI_DEVICE_API_D3D12) &&
        ri_detectShaderFormat(
            std::span<const char>(bytes->data(), bytes->size())) != format)
      FatalError(
          "RIProgram: shader artifact bytes do not match metadata for '%s'\n",
          artifactName.c_str());
    artifact.format = metadataFormat;
    // A production DXIL library can contain several entry points. Retain the
    // validated document without selecting one here; initialize() has the
    // requested entry name and stage and performs the strict, unique match.
    if (!reflectionJson.empty()) {
      auto reflection = std::make_shared<ShaderReflection>();
      reflection->json =
          std::make_shared<const std::string>(std::move(reflectionJson));
      artifact.reflection = std::move(reflection);
    }
    // Retained alongside the reflection: initialize() cross-checks each
    // requested (entry, stage) pair against this map, which is the only stage
    // record a ray-tracing library has.
    artifact.meta =
        std::make_shared<const RIShaderArtifactMeta>(std::move(artifactMeta));
  }
#endif
  return artifact;
}

RIProgram::ShaderArtifact RIProgram::loadShaderStage(cFileSearcher *searcher,
                                                     const tString &asName,
                                                     const char *entryPoint) {
  return loadShaderArtifact(searcher, asName, entryPoint);
}

#if (DEVICE_IMPL_D3D12)
// Program-wide push-constant range, merged across stages. A D3D12 root
// signature is shared by every stage of a program, so every stage must agree
// exactly on where the block lives -- unlike Vulkan, which merges per-stage
// ranges into one layout. Both the reflected and the embedded-signature paths
// need the same scan, so it lives here rather than being written twice.
struct RIPushConstantRange {
  bool present = false;
  uint32_t offset = 0, size = 0, registerIndex = 0, registerSpace = 0;
  // The stage that established the range, for the conflict message.
  std::string stage, entryPoint;
};

static const char *ri_push_label(const std::string &s) {
  return s.empty() ? "<unknown>" : s.c_str();
}

// Folds one stage's reflected push block into `range`, aborting with a message
// that names the program and both disagreeing stages. `programName` may be
// null.
static void
ri_merge_push_constant_range(RIPushConstantRange &range,
                             const RIProgram::ShaderReflection &reflection,
                             const char *programName) {
  const auto &push = reflection.pushConstants;
  if (!push.present)
    return;
  const char *program =
      programName && *programName ? programName : "<unnamed program>";
  if (!push.size || (push.offset & 3u) || (push.size & 3u) ||
      push.offset > UINT32_MAX - push.size ||
      (push.offset + push.size) / 4 > 64)
    FatalError(
        "RIProgram '%s': %s (%s) has an invalid D3D12 push-constant range\n"
        "  offset=%u size=%u: both must be 4-byte aligned and (offset+size)/4 "
        "must not exceed 64 DWORDs\n",
        program, ri_push_label(reflection.stage),
        ri_push_label(reflection.entryPoint), push.offset, push.size);
  if (!range.present) {
    range.present = true;
    range.offset = push.offset;
    range.size = push.size;
    range.registerIndex = push.registerIndex;
    range.registerSpace = push.registerSpace;
    range.stage = reflection.stage;
    range.entryPoint = reflection.entryPoint;
    return;
  }
  if (range.offset == push.offset && range.size == push.size &&
      range.registerIndex == push.registerIndex &&
      range.registerSpace == push.registerSpace)
    return;
  FatalError(
      "RIProgram '%s': conflicting reflected D3D12 push-constant ranges across "
      "shader stages\n"
      "  one root signature serves every stage, so 'gPushConstants' must land "
      "on the same register in all of them.\n"
      "  slangc numbers registers per compilation unit and each entry point is "
      "compiled separately, so a\n"
      "  vertex/fragment pair split across two files disagrees whenever they "
      "declare different globals.\n"
      "  Pin the block with HPL_PUSH_CONSTANT_REGISTER "
      "(amnesia/slang/HostDefinitions.h).\n"
      "  %s (%s): offset=%u size=%u register=b%u space=%u\n"
      "  %s (%s): offset=%u size=%u register=b%u space=%u\n",
      program, ri_push_label(range.stage), ri_push_label(range.entryPoint),
      range.offset, range.size, range.registerIndex, range.registerSpace,
      ri_push_label(reflection.stage), ri_push_label(reflection.entryPoint),
      push.offset, push.size, push.registerIndex, push.registerSpace);
}

static bool ri_create_reflected_d3d12_root_signature(
    RIDevice *device, const char *programName,
    const std::array<RIProgram::ShaderBinary, RIProgram::PROGRAM_STAGES_MAX>
        &bins,
    std::span<const RIBindlessLayout> externalLayouts,
    std::array<RIProgram::DescriptorSetSlot, RIProgram::DESCRIPTOR_SET_MAX>
        *descriptorSlots,
    ID3D12RootSignature **out, uint32_t *resourceParameter,
    uint32_t *geometryParameter, uint32_t *samplerParameter,
    uint32_t *geometryRangeOffset, uint32_t *geometryRangeCount,
    uint32_t *pushConstantParameter, uint32_t *pushConstantOffset,
    uint32_t *pushConstantSize) {
  std::vector<D3D12_DESCRIPTOR_RANGE1> resources, samplers;
  struct ExternalPending {
    D3D12_DESCRIPTOR_RANGE1 range{};
    uint32_t set = 0;
    uint32_t binding = 0;
    RIBindlessRegisterClass registerClass = RIBindlessRegisterClass::SRV;
    bool geometryHeap = false;
    uint32_t stages = 0;
  };
  std::vector<ExternalPending> external;
  // Which root parameter a deduplicated register landed in, so the stage mask
  // accumulated across stages can be folded back into that parameter's
  // ShaderVisibility once every stage has been walked.
  enum class KeyTable : uint8_t { Resource, Sampler, External };
  struct Key {
    RIProgram::ShaderRegisterClass cls;
    uint32_t reg, space, count;
    std::string name;
    KeyTable table = KeyTable::Resource;
    uint32_t externalIndex = 0;
    uint32_t stages = 0;
  };
  std::vector<Key> keys;
  // Bit per PROGRAM_STAGE_* that reflects each resource / the push block.
  uint32_t programStages = 0, pushStages = 0;
  std::unordered_map<std::string, RIProgram::ShaderResourceReflection> merged;
  auto reflectedClass = [](RIProgram::ShaderRegisterClass cls) {
    switch (cls) {
    case RIProgram::ShaderRegisterClass::CBV:
      return RIBindlessRegisterClass::CBV;
    case RIProgram::ShaderRegisterClass::SRV:
      return RIBindlessRegisterClass::SRV;
    case RIProgram::ShaderRegisterClass::UAV:
      return RIBindlessRegisterClass::UAV;
    case RIProgram::ShaderRegisterClass::Sampler:
      return RIBindlessRegisterClass::Sampler;
    default:
      return RIBindlessRegisterClass::SRV;
    }
  };
  auto findExternal = [&](const RIProgram::ShaderResourceReflection &resource,
                          uint32_t &setOut,
                          const RIBindlessD3D12Binding *&bindingOut) {
    bindingOut = nullptr;
    for (uint32_t set = 0;
         set < externalLayouts.size() && set < RIProgram::DESCRIPTOR_SET_MAX;
         ++set) {
      for (const auto &binding : externalLayouts[set].d3d12.bindings) {
        if (binding.registerClass != reflectedClass(resource.registerClass) ||
            binding.registerIndex != resource.registerIndex ||
            binding.registerSpace != resource.registerSpace)
          continue;
        const uint32_t needed =
            resource.unbounded ? 1u : std::max(1u, resource.arrayCount);
        if (!binding.descriptorCount || (binding.descriptorCount != UINT_MAX &&
                                         binding.descriptorCount < needed))
          FatalError("RIProgram: external D3D12 binding '%s' has insufficient "
                     "descriptors\n",
                     resource.name.c_str());
        if (bindingOut)
          FatalError("RIProgram: reflected D3D12 resource '%s' matches "
                     "multiple external layouts\n",
                     resource.name.c_str());
        setOut = set;
        bindingOut = &binding;
      }
    }
    return bindingOut != nullptr;
  };
  for (size_t stageIndex = 0; stageIndex < bins.size(); ++stageIndex) {
    const auto &bin = bins[stageIndex];
    const uint32_t stageBit = 1u << stageIndex;
    if (bin.reflection && !bin.buf.empty())
      programStages |= stageBit;
    if (bin.reflection)
      for (const auto &r : bin.reflection->resources) {
        if (!r.used || r.name.empty())
          continue;
        auto mergedIt = merged.find(r.name);
        if (mergedIt == merged.end()) {
          merged.emplace(r.name, r);
        } else {
          const auto &prior = mergedIt->second;
          const bool shapeConflict =
              prior.registerClass != r.registerClass ||
              prior.registerIndex != r.registerIndex ||
              prior.registerSpace != r.registerSpace ||
              prior.arrayCount != r.arrayCount ||
              prior.unbounded != r.unbounded ||
              (prior.stride && r.stride && prior.stride != r.stride) ||
              (prior.size && r.size && prior.size != r.size) ||
              (!prior.type.empty() && !r.type.empty() &&
               prior.type != r.type) ||
              (!prior.format.empty() && !r.format.empty() &&
               prior.format != r.format);
          if (shapeConflict)
            FatalError(
                "RIProgram: conflicting reflected D3D12 resource '%s' across "
                "shader stages\n"
                "  %s: class=%u register=%u space=%u count=%u unbounded=%u "
                "stride=%u size=%u type='%s' format='%s'\n"
                "  %s: class=%u register=%u space=%u count=%u unbounded=%u "
                "stride=%u size=%u type='%s' format='%s'\n",
                r.name.c_str(),
                prior.stage.empty() ? "<unstaged>" : prior.stage.c_str(),
                unsigned(prior.registerClass), prior.registerIndex,
                prior.registerSpace, prior.arrayCount,
                unsigned(prior.unbounded), prior.stride, prior.size,
                prior.type.c_str(), prior.format.c_str(),
                r.stage.empty() ? "<unstaged>" : r.stage.c_str(),
                unsigned(r.registerClass), r.registerIndex, r.registerSpace,
                r.arrayCount, unsigned(r.unbounded), r.stride, r.size,
                r.type.c_str(), r.format.c_str());
        }
        RIProgram::ShaderRegisterClass cls = r.registerClass;
        if (cls == RIProgram::ShaderRegisterClass::Unknown)
          FatalError(
              "RIProgram: reflected D3D12 resource has no register class\n");
        const uint32_t count =
            r.unbounded ? UINT_MAX : std::max(1u, r.arrayCount);
        // D3D12 keeps CBV, SRV, UAV, and sampler registers in independent
        // namespaces. b0, t0, u0, and s0 may therefore coexist in the same
        // space.
        auto it = std::find_if(keys.begin(), keys.end(), [&](const Key &k) {
          return k.cls == cls && k.reg == r.registerIndex &&
                 k.space == r.registerSpace;
        });
        if (it != keys.end()) {
          if (it->name != r.name)
            FatalError("RIProgram: conflicting reflected D3D12 resources '%s' "
                       "and '%s' share register %u/space%u\n",
                       it->name.c_str(), r.name.c_str(), r.registerIndex,
                       r.registerSpace);
          if (it->count != count)
            FatalError(
                "RIProgram: conflicting reflected D3D12 descriptor layouts\n");
          // A register reflected by several stages is one descriptor range, but
          // every stage that reads it must stay visible to it.
          it->stages |= stageBit;
          continue;
        }
        keys.push_back({cls, r.registerIndex, r.registerSpace, count, r.name});
        Key &key = keys.back();
        key.stages = stageBit;
        D3D12_DESCRIPTOR_RANGE1 range{};
        range.RangeType = cls == RIProgram::ShaderRegisterClass::CBV
                              ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                          : cls == RIProgram::ShaderRegisterClass::SRV
                              ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                          : cls == RIProgram::ShaderRegisterClass::UAV
                              ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                              : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        range.NumDescriptors = count;
        range.BaseShaderRegister = r.registerIndex;
        range.RegisterSpace = r.registerSpace;
        range.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uint32_t externalSet = 0;
        const RIBindlessD3D12Binding *externalBinding = nullptr;
        if (findExternal(r, externalSet, externalBinding)) {
          range.OffsetInDescriptorsFromTableStart = 0;
          range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
          // DATA_* flags apply to CBV/SRV/UAV descriptor contents. Sampler
          // ranges may only describe descriptor volatility.
          if (cls != RIProgram::ShaderRegisterClass::Sampler)
            range.Flags |= D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
          key.table = KeyTable::External;
          key.externalIndex = static_cast<uint32_t>(external.size());
          external.push_back({range, externalSet, externalBinding->binding,
                              externalBinding->registerClass,
                              externalBinding->geometryHeap});
        } else {
          const bool sampler = cls == RIProgram::ShaderRegisterClass::Sampler;
          key.table = sampler ? KeyTable::Sampler : KeyTable::Resource;
          (sampler ? samplers : resources).push_back(range);
        }
      }
  }
  uint32_t resourceStages = 0, samplerStages = 0;
  for (const Key &key : keys) {
    switch (key.table) {
    case KeyTable::Resource:
      resourceStages |= key.stages;
      break;
    case KeyTable::Sampler:
      samplerStages |= key.stages;
      break;
    case KeyTable::External:
      external[key.externalIndex].stages |= key.stages;
      break;
    }
  }
  // Root parameters default to SHADER_VISIBILITY_ALL, which makes every
  // binding reachable from every graphics stage. Narrowing a table to the one
  // stage that reads it shrinks the root signature and, more usefully, makes a
  // debug-layer report name the stage that actually owns the binding instead
  // of whichever stage happens to come first. Compute and ray-tracing root
  // signatures must stay ALL, so this only applies to graphics programs.
  const uint32_t kVertexBit = 1u << RIProgram::PROGRAM_STAGE_VERTEX;
  const uint32_t kFragmentBit = 1u << RIProgram::PROGRAM_STAGE_FRAGMENT;
  const bool graphicsProgram =
      (programStages & (kVertexBit | kFragmentBit)) != 0 &&
      (programStages & ~(kVertexBit | kFragmentBit)) == 0;
  auto visibilityFor = [&](uint32_t stages) {
    if (!graphicsProgram)
      return D3D12_SHADER_VISIBILITY_ALL;
    if (stages == kVertexBit)
      return D3D12_SHADER_VISIBILITY_VERTEX;
    if (stages == kFragmentBit)
      return D3D12_SHADER_VISIBILITY_PIXEL;
    return D3D12_SHADER_VISIBILITY_ALL;
  };
  auto enforceUnboundedRangeOrder = [](auto &ranges, const char *tableName) {
    const size_t unboundedCount = static_cast<size_t>(
        std::count_if(ranges.begin(), ranges.end(), [](const auto &range) {
          return range.NumDescriptors == UINT_MAX;
        }));
    if (unboundedCount > 1)
      FatalError("RIProgram: D3D12 %s table has multiple unbounded descriptor "
                 "ranges\n",
                 tableName);
    // D3D12 requires an unbounded range to be the final range in its table.
    // stable_partition keeps the reflected order within both groups.
    std::stable_partition(ranges.begin(), ranges.end(), [](const auto &range) {
      return range.NumDescriptors != UINT_MAX;
    });
  };
  enforceUnboundedRangeOrder(resources, "resource");
  enforceUnboundedRangeOrder(samplers, "sampler");
  *geometryParameter = UINT32_MAX;
  *geometryRangeOffset = UINT32_MAX;
  *geometryRangeCount = 0;
  if (descriptorSlots)
    for (auto &slot : *descriptorSlots) {
      slot.d3d12ExternalTables.clear();
      slot.isExternal = false;
    }
  std::vector<D3D12_ROOT_PARAMETER1> params;
  RIPushConstantRange pushRange;
  for (size_t stageIndex = 0; stageIndex < bins.size(); ++stageIndex) {
    const auto &bin = bins[stageIndex];
    if (!bin.reflection || !bin.reflection->pushConstants.present)
      continue;
    pushStages |= 1u << stageIndex;
    ri_merge_push_constant_range(pushRange, *bin.reflection, programName);
  }
  const bool reflectedPush = pushRange.present;
  const uint32_t reflectedPushOffset = pushRange.offset,
                 reflectedPushSize = pushRange.size,
                 reflectedPushRegister = pushRange.registerIndex,
                 reflectedPushSpace = pushRange.registerSpace;
  if (!resources.empty()) {
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(resources.size());
    p.DescriptorTable.pDescriptorRanges = resources.data();
    p.ShaderVisibility = visibilityFor(resourceStages);
    *resourceParameter = static_cast<uint32_t>(params.size());
    params.push_back(p);
  } else {
    *resourceParameter = UINT32_MAX;
  }
  if (!samplers.empty()) {
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(samplers.size());
    p.DescriptorTable.pDescriptorRanges = samplers.data();
    p.ShaderVisibility = visibilityFor(samplerStages);
    *samplerParameter = static_cast<uint32_t>(params.size());
    params.push_back(p);
  } else {
    *samplerParameter = UINT32_MAX;
  }
  for (auto &pending : external) {
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &pending.range;
    p.ShaderVisibility = visibilityFor(pending.stages);
    const uint32_t parameter = static_cast<uint32_t>(params.size());
    params.push_back(p);
    if (descriptorSlots && pending.set < descriptorSlots->size()) {
      auto &slot = (*descriptorSlots)[pending.set];
      slot.isExternal = true;
      slot.d3d12ExternalTables.push_back(
          {pending.binding, pending.registerClass,
           pending.range.BaseShaderRegister, pending.range.RegisterSpace,
           parameter, pending.geometryHeap});
    }
    if (pending.geometryHeap) {
      *geometryParameter = parameter;
      *geometryRangeOffset = 0;
      *geometryRangeCount = pending.range.NumDescriptors;
    }
  }
  // Keep resource/sampler parameter indices stable for external bindless
  // layouts; append push constants after those descriptor tables.
  *pushConstantOffset = reflectedPush ? reflectedPushOffset : 0;
  *pushConstantSize = reflectedPush ? reflectedPushSize : 0;
  *pushConstantParameter = UINT32_MAX;
  if (reflectedPush) {
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.Num32BitValues = (reflectedPushOffset + reflectedPushSize) / 4;
    p.Constants.ShaderRegister = reflectedPushRegister;
    p.Constants.RegisterSpace = reflectedPushSpace;
    p.ShaderVisibility = visibilityFor(pushStages);
    *pushConstantParameter = static_cast<uint32_t>(params.size());
    params.push_back(p);
  }
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
  desc.Desc_1_1.pParameters = params.data();
  desc.Desc_1_1.NumStaticSamplers = 0;
  desc.Desc_1_1.pStaticSamplers = nullptr;
  // The input-assembler flag is meaningful only to a graphics signature. A DXR
  // global root signature carrying it is rejected by the debug layer, and on a
  // compute signature it is inert, so scope it to graphics programs.
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  if (graphicsProgram) {
    desc.Desc_1_1.Flags |=
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    // The engine only ever binds vertex and pixel stages, so denying the rest
    // is free. Denying an absent VS/PS as well keeps the signature honest
    // about which stages can reach a binding.
    desc.Desc_1_1.Flags |=
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;
    if (!(programStages & kVertexBit))
      desc.Desc_1_1.Flags |=
          D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;
    if (!(programStages & kFragmentBit))
      desc.Desc_1_1.Flags |=
          D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
  }
  ID3DBlob *serialized = nullptr, *error = nullptr;
  const bool ok = D3D12_WrapResult(D3D12SerializeVersionedRootSignature(
                      &desc, &serialized, &error)) &&
                  serialized &&
                  D3D12_WrapResult(device->d3d12.device->CreateRootSignature(
                      0, serialized->GetBufferPointer(),
                      serialized->GetBufferSize(), IID_PPV_ARGS(out)));
  if (!ok) {
    // The serializer explains a rejected layout only through this blob.
    hpl::Error("RIProgram '%s': D3D12 root signature rejected (%u params, push "
               "constants %u DWORDs @ b%u space%u): %s\n",
               programName && *programName ? programName : "<unnamed program>",
               unsigned(params.size()),
               reflectedPush
                   ? unsigned((reflectedPushOffset + reflectedPushSize) / 4)
                   : 0u,
               reflectedPushRegister, reflectedPushSpace,
               error ? static_cast<const char *>(error->GetBufferPointer())
                     : "<no serializer message>");
  }
  if (error)
    error->Release();
  if (serialized)
    serialized->Release();
  return ok;
}
#endif

void RIProgram::initialize(RIDevice *device, std::span<ModuleStage> moduleInit,
                           std::span<const RIBindlessLayout> externalLayouts,
                           const char *debugName) {
  assert(device);
  this->device = device;
#if !defined(NDEBUG)
  m_debugName = debugName ? debugName : "<unnamed program>";
#endif

  // RIProgram's descriptor reflection and pipeline code are Vulkan-specific.
  // Keep DXIL opaque here until the D3D12 program backend owns its pipeline
  // creation; in particular, never pass a DXBC/DXIL container to SPIRV-Reflect.
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    // The D3D12 PSO caches key on the pipeline desc alone, which is sound only
    // because a program's shaders never change under its own cache. Every
    // reload path installs a fresh RIProgram and defers dispose() on the old
    // one, so this holds -- pin it here rather than paying for a shader-bytes
    // fold in the per-draw cache key.
    assert(pipeline.empty() && rtPipeline.empty() &&
           "RIProgram::initialize on a program that still owns PSOs; "
           "dispose() first");
    d3d12VertexBindingStrides = {};
    d3d12VertexBindingCount = 0;
    impl.d3d12.geometryRootParameter = UINT32_MAX;
    impl.d3d12.geometryRangeOffset = UINT32_MAX;
    impl.d3d12.geometryRangeCount = 0;
    impl.d3d12.pushConstantRootParameter = UINT32_MAX;
    impl.d3d12.pushConstantOffset = 0;
    impl.d3d12.pushConstantSize = 0;
    // DXIL and its reflection are retained together. No global bytecode hash
    // or borrowed JSON pointer is involved, so duplicate modules and parallel
    // program initialization remain independent and deterministic.
    for (auto &init : moduleInit) {
      if (init.stage >= PROGRAM_STAGES_MAX)
        FatalError("RIProgram: D3D12 shader stage index is out of range\n");
      if (init.format == RIShaderArtifactFormat::Unknown)
        init.format = ri_detectShaderFormat(
            std::span<const char>(init.data.data(), init.data.size()));
      if (init.format != RIShaderArtifactFormat::Dxil)
        FatalError("RIProgram: D3D12 program requires DXIL shader artifacts\n");
      if (!init.artifact.reflection)
        FatalError("RIProgram: D3D12 shader stage has no retained reflection "
                   "metadata\n");
      auto *bin = &shaderBin[init.stage];
      bin->buf.assign(init.data.begin(), init.data.end());
      bin->reflection = init.artifact.reflection;
      if (bin->reflection && init.entryPoint && bin->reflection->json)
        bin->reflection = ri_parseShaderReflection(
            *bin->reflection->json, init.entryPoint, ri_stageName(init.stage));
      if (!bin->reflection)
        FatalError("RIProgram: DXIL reflection stage is incompatible with "
                   "requested shader stage\n");
      const char *wantStage = ri_stageName(init.stage);
      if (init.artifact.meta && init.artifact.meta->isLibrary()) {
        // A library's reflection may not state stages at all, so the sidecar's
        // entry map is the authority: the requested (entry, stage) pair must be
        // one the artifact actually declares.
        const char *declared =
            init.entryPoint ? init.artifact.meta->stageForEntry(init.entryPoint)
                            : nullptr;
        if (!declared || !wantStage || strcmp(declared, wantStage) != 0)
          FatalError("RIProgram: DXIL artifact does not declare entry '%s' at "
                     "stage '%s'\n",
                     init.entryPoint ? init.entryPoint : "<none>",
                     wantStage ? wantStage : "<unknown>");
        // Declaring the entry is not enough to *run* it: only a state object
        // consumes a lib_6_x container. A graphics or compute PSO needs the
        // single-entry executable the build emits beside the library, which
        // loadShaderArtifact selects only when the caller names an entry point.
        // Without this, the library bytes reach CreateGraphicsPipelineState and
        // surface as an opaque "Shader version provided: UNRECOGNIZED" from the
        // debug layer — and as nothing at all without it.
        if (init.stage == PROGRAM_STAGE_VERTEX ||
            init.stage == PROGRAM_STAGE_FRAGMENT ||
            init.stage == PROGRAM_STAGE_COMPUTE)
          FatalError(
              "RIProgram '%s': stage '%s' was given the multi-entry DXIL "
              "library, which no graphics or compute PSO can consume. "
              "Pass the entry point to loadShaderStage so it selects "
              "'<name>.%s.dxil' instead.\n",
              debugName ? debugName : "<unnamed program>", wantStage,
              init.entryPoint ? init.entryPoint : "<entry>");
      } else if (bin->reflection->stage != wantStage) {
        FatalError("RIProgram: DXIL reflection stage is incompatible with "
                   "requested shader stage\n");
      }
      if (init.entryPoint && *init.entryPoint)
        bin->entryPoint = init.entryPoint;
    }
    // DXIL carries the authoritative root signature. Deserialize it while the
    // caller-owned stage blob is still available, then retain the COM object
    // on the program; PSO creation and command binding never inspect Vulkan
    // handles or borrow reflection storage.
    // Ray-tracing programs populate none of the raster/compute slots, so the
    // root-signature source is the first populated stage in a fixed priority
    // order rather than a vertex-or-compute pick.
    static constexpr ProgramStages kRootStagePriority[] = {
        PROGRAM_STAGE_VERTEX,       PROGRAM_STAGE_COMPUTE,
        PROGRAM_STAGE_RAYGEN,       PROGRAM_STAGE_MISS,
        PROGRAM_STAGE_CLOSEST_HIT,  PROGRAM_STAGE_ANY_HIT,
        PROGRAM_STAGE_INTERSECTION, PROGRAM_STAGE_CALLABLE};
    const ShaderBinary *rootStage = nullptr;
    for (const ProgramStages candidate : kRootStagePriority) {
      if (!shaderBin[candidate].buf.empty()) {
        rootStage = &shaderBin[candidate];
        break;
      }
    }
    if (!rootStage)
      FatalError("RIProgram: D3D12 program has no shader stages\n");
    // A DXR global root signature is a state-object subobject, never an RTS0
    // container part, so ray-tracing programs always take the reflected path.
    const bool rayTracingProgram =
        shaderBin[PROGRAM_STAGE_VERTEX].buf.empty() &&
        shaderBin[PROGRAM_STAGE_COMPUTE].buf.empty();
    ID3DBlob *rootBlob = nullptr;
    // Not wrapped in D3D12_WrapResult: a container with no RTS0 part is the
    // normal case for any shader without [RootSignature] — NRD's 31 denoiser
    // modules, for one — and the reflected path below handles it. Logging the
    // E_FAIL would print an error-shaped line per program and bury real ones.
    if (rayTracingProgram ||
        FAILED(D3DGetBlobPart(rootStage->buf.data(), rootStage->buf.size(),
                              D3D_BLOB_ROOT_SIGNATURE, 0, &rootBlob)) ||
        !rootBlob) {
      if (!ri_create_reflected_d3d12_root_signature(
              device, debugName, shaderBin, externalLayouts,
              &programDescriptors, &impl.d3d12.rootSignature,
              &impl.d3d12.resourceRootParameter,
              &impl.d3d12.geometryRootParameter,
              &impl.d3d12.samplerRootParameter, &impl.d3d12.geometryRangeOffset,
              &impl.d3d12.geometryRangeCount,
              &impl.d3d12.pushConstantRootParameter,
              &impl.d3d12.pushConstantOffset, &impl.d3d12.pushConstantSize))
        FatalError("RIProgram '%s': DXIL has no embedded root signature and "
                   "reflection root creation failed\n",
                   debugName ? debugName : "<unnamed program>");
    }
    ID3D12VersionedRootSignatureDeserializer *deserializer = nullptr;
    if (rootBlob &&
        !D3D12_WrapResult(D3D12CreateVersionedRootSignatureDeserializer(
            rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
            IID_PPV_ARGS(&deserializer)))) {
      rootBlob->Release();
      FatalError("RIProgram: DXIL artifact has no usable root signature\n");
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *rootDesc =
        deserializer ? deserializer->GetUnconvertedRootSignatureDesc()
                     : nullptr;
    auto validTableParameter = [&](uint32_t parameter) {
      if (parameter == UINT32_MAX)
        return true;
      // A reflection-created signature has no deserializer-owned descriptor
      // tree here.  Its table indices were produced by the same reflection
      // pass, so external layouts may still reuse those indices.
      if (!rootDesc)
        return true;
      switch (rootDesc->Version) {
      case D3D_ROOT_SIGNATURE_VERSION_1_0:
        return parameter < rootDesc->Desc_1_0.NumParameters &&
               rootDesc->Desc_1_0.pParameters[parameter].ParameterType ==
                   D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
               rootDesc->Desc_1_0.pParameters[parameter]
                       .DescriptorTable.NumDescriptorRanges > 0;
      case D3D_ROOT_SIGNATURE_VERSION_1_1:
        return parameter < rootDesc->Desc_1_1.NumParameters &&
               rootDesc->Desc_1_1.pParameters[parameter].ParameterType ==
                   D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
               rootDesc->Desc_1_1.pParameters[parameter]
                       .DescriptorTable.NumDescriptorRanges > 0;
      default:
        return parameter < rootDesc->Desc_1_2.NumParameters &&
               rootDesc->Desc_1_2.pParameters[parameter].ParameterType ==
                   D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
               rootDesc->Desc_1_2.pParameters[parameter]
                       .DescriptorTable.NumDescriptorRanges > 0;
      }
    };
    bool shaderHasGeometry = false;
    bool shaderHasResources = false;
    bool shaderHasSamplers = false;
    for (const auto &bin : shaderBin)
      if (bin.reflection)
        for (const auto &r : bin.reflection->resources)
          if (r.used && !r.name.empty()) {
            const bool geometry = r.registerClass == ShaderRegisterClass::SRV &&
                                  r.registerIndex == 4 &&
                                  r.registerSpace == 3 && r.unbounded;
            shaderHasGeometry |= geometry;
            shaderHasSamplers |=
                r.registerClass == ShaderRegisterClass::Sampler;
            shaderHasResources |=
                r.registerClass != ShaderRegisterClass::Sampler && !geometry;
          }
    ID3DBlob *serializedRoot = nullptr;
    ID3DBlob *rootError = nullptr;
    bool serialized = !deserializer;
    if (deserializer) {
      serialized =
          rootDesc && D3D12_WrapResult(D3D12SerializeVersionedRootSignature(
                          rootDesc, &serializedRoot, &rootError));
    }
    bool rootCreated = !deserializer;
    if (deserializer && serialized && serializedRoot) {
      const HRESULT rootResult = device->d3d12.device->CreateRootSignature(
          0, serializedRoot->GetBufferPointer(),
          serializedRoot->GetBufferSize(),
          IID_PPV_ARGS(&impl.d3d12.rootSignature));
      rootCreated = D3D12_WrapResult(rootResult);
    }
    if (!rootCreated) {
      ri_log_d3d12_blob("root-signature diagnostics", rootError);
      if (rootError)
        rootError->Release();
      if (serializedRoot)
        serializedRoot->Release();
      if (deserializer)
        deserializer->Release();
      rootBlob->Release();
      FatalError("RIProgram: failed to create D3D12 root signature\n");
    }
    if (serializedRoot)
      serializedRoot->Release();
    if (rootError)
      rootError->Release();
    // An embedded DXIL signature is authoritative, but it must expose the
    // same root-constant contract as reflection. Root-signature changes also
    // invalidate all root parameters on a command list; bindPipeline (or an
    // equivalent root-signature bind) must therefore precede push writes.
    RIPushConstantRange embeddedPushRange;
    for (const auto &bin : shaderBin)
      if (bin.reflection)
        ri_merge_push_constant_range(embeddedPushRange, *bin.reflection,
                                     debugName);
    const bool reflectedPush = embeddedPushRange.present;
    const uint32_t reflectedPushOffset = embeddedPushRange.offset,
                   reflectedPushSize = embeddedPushRange.size,
                   reflectedPushRegister = embeddedPushRange.registerIndex,
                   reflectedPushSpace = embeddedPushRange.registerSpace;
    if (deserializer && reflectedPush) {
      const uint32_t end = reflectedPushOffset + reflectedPushSize;
      auto matchesPush = [&](UINT parameterCount, const auto *params) {
        for (UINT p = 0; p < parameterCount; ++p) {
          const auto &param = params[p];
          if (param.ParameterType != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            continue;
          if (param.Constants.ShaderRegister == reflectedPushRegister &&
              param.Constants.RegisterSpace == reflectedPushSpace &&
              param.Constants.Num32BitValues >= end / 4) {
            impl.d3d12.pushConstantRootParameter = p;
            impl.d3d12.pushConstantOffset = reflectedPushOffset;
            impl.d3d12.pushConstantSize = reflectedPushSize;
            return true;
          }
        }
        return false;
      };
      bool matched = false;
      switch (rootDesc->Version) {
      case D3D_ROOT_SIGNATURE_VERSION_1_0:
        matched = matchesPush(rootDesc->Desc_1_0.NumParameters,
                              rootDesc->Desc_1_0.pParameters);
        break;
      case D3D_ROOT_SIGNATURE_VERSION_1_1:
        matched = matchesPush(rootDesc->Desc_1_1.NumParameters,
                              rootDesc->Desc_1_1.pParameters);
        break;
      default:
        matched = matchesPush(rootDesc->Desc_1_2.NumParameters,
                              rootDesc->Desc_1_2.pParameters);
        break;
      }
      if (!matched)
        FatalError("RIProgram: embedded D3D12 root signature has no reflected "
                   "push-constant parameter\n");
    }
    if (!reflectedPush && deserializer) {
      impl.d3d12.pushConstantRootParameter = UINT32_MAX;
      impl.d3d12.pushConstantOffset = impl.d3d12.pushConstantSize = 0;
    }
    if (deserializer) {
      // Embedded signatures are accepted only when one descriptor table
      // covers every reflected resource of each register class.  A class
      // absent from reflection has no table requirement. This keeps the
      // compact descriptor arena offsets compatible with the signature.
      auto tableCovers = [&](uint32_t parameter, bool sampler,
                             bool geometryOnly = false) {
        auto rangeCovers = [&](D3D12_DESCRIPTOR_RANGE_TYPE actualType,
                               D3D12_DESCRIPTOR_RANGE_TYPE expectedType,
                               UINT base, UINT count, UINT space, UINT regSpace,
                               UINT reg, UINT needed) {
          return actualType == expectedType && space == regSpace &&
                 base <= reg &&
                 (count == UINT_MAX || reg - base + needed <= count);
        };
        bool ok = true, saw = false;
        auto inspect = [&](auto *params, UINT count, auto versionTag) {
          (void)versionTag;
          if (parameter >= count ||
              params[parameter].ParameterType !=
                  D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            return;
          const auto &table = params[parameter].DescriptorTable;
          for (const auto &bin : shaderBin)
            if (bin.reflection)
              for (const auto &r : bin.reflection->resources) {
                const bool isGeometry =
                    r.registerClass == ShaderRegisterClass::SRV &&
                    r.registerIndex == 4 && r.registerSpace == 3 && r.unbounded;
                if (!r.used || r.name.empty() ||
                    (r.registerClass == ShaderRegisterClass::Sampler) !=
                        sampler ||
                    isGeometry != geometryOnly)
                  continue;
                const auto type = r.registerClass == ShaderRegisterClass::CBV
                                      ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                                  : r.registerClass == ShaderRegisterClass::SRV
                                      ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                  : r.registerClass == ShaderRegisterClass::UAV
                                      ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                      : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                const UINT needed =
                    r.unbounded ? UINT_MAX : std::max(1u, r.arrayCount);
                bool found = false;
                for (UINT i = 0; i < table.NumDescriptorRanges; ++i) {
                  const auto &range = table.pDescriptorRanges[i];
                  if (rangeCovers(range.RangeType, type,
                                  range.BaseShaderRegister,
                                  range.NumDescriptors, range.RegisterSpace,
                                  r.registerSpace, r.registerIndex, needed)) {
                    found = true;
                    break;
                  }
                }
                saw = true;
                ok &= found;
              }
        };
        switch (rootDesc->Version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0:
          inspect(rootDesc->Desc_1_0.pParameters,
                  rootDesc->Desc_1_0.NumParameters, 0);
          break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1:
          inspect(rootDesc->Desc_1_1.pParameters,
                  rootDesc->Desc_1_1.NumParameters, 0);
          break;
        default:
          inspect(rootDesc->Desc_1_2.pParameters,
                  rootDesc->Desc_1_2.NumParameters, 0);
          break;
        }
        return saw && ok;
      };
      impl.d3d12.resourceRootParameter = UINT32_MAX;
      impl.d3d12.geometryRootParameter = UINT32_MAX;
      impl.d3d12.samplerRootParameter = UINT32_MAX;
      impl.d3d12.geometryRangeOffset = UINT32_MAX;
      impl.d3d12.geometryRangeCount = 0;
      UINT parameterCount =
          rootDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0
              ? rootDesc->Desc_1_0.NumParameters
          : rootDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1
              ? rootDesc->Desc_1_1.NumParameters
              : rootDesc->Desc_1_2.NumParameters;
      for (UINT p = 0; p < parameterCount; ++p) {
        if (tableCovers(p, false))
          impl.d3d12.resourceRootParameter = p;
        if (tableCovers(p, false, true))
          impl.d3d12.geometryRootParameter = p;
        if (tableCovers(p, true))
          impl.d3d12.samplerRootParameter = p;
      }
      if ((shaderHasResources &&
           !validTableParameter(impl.d3d12.resourceRootParameter)) ||
          (shaderHasGeometry &&
           !validTableParameter(impl.d3d12.geometryRootParameter)) ||
          (shaderHasSamplers &&
           !validTableParameter(impl.d3d12.samplerRootParameter)))
        FatalError("RIProgram: embedded D3D12 root signature conflicts with "
                   "reflected bindings\n");

      // Geometry handles contain absolute heap indices.  The dedicated
      // geometry table must therefore expose t4/space3 at table element zero.
      auto captureGeometryRange = [&](uint32_t parameter, auto *params,
                                      UINT count) {
        if (parameter == UINT32_MAX || parameter >= count ||
            params[parameter].ParameterType !=
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
          return false;
        const auto &table = params[parameter].DescriptorTable;
        if (table.NumDescriptorRanges != 1)
          return false;
        uint64_t cursor = 0;
        bool found = false;
        for (UINT i = 0; i < table.NumDescriptorRanges; ++i) {
          const auto &range = table.pDescriptorRanges[i];
          const uint64_t offset = range.OffsetInDescriptorsFromTableStart ==
                                          D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                      ? cursor
                                      : range.OffsetInDescriptorsFromTableStart;
          if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
              range.BaseShaderRegister == 4 && range.RegisterSpace == 3 &&
              range.NumDescriptors == UINT_MAX) {
            if (offset != 0)
              return false;
            impl.d3d12.geometryRootParameter = parameter;
            impl.d3d12.geometryRangeOffset = static_cast<uint32_t>(offset);
            impl.d3d12.geometryRangeCount = range.NumDescriptors;
            found = true;
          }
          if (range.NumDescriptors == UINT_MAX ||
              offset > UINT64_MAX - range.NumDescriptors)
            cursor = UINT64_MAX;
          else
            cursor = offset + range.NumDescriptors;
        }
        return found;
      };
      bool geometryCaptured = false;
      switch (rootDesc->Version) {
      case D3D_ROOT_SIGNATURE_VERSION_1_0:
        geometryCaptured = captureGeometryRange(
            impl.d3d12.geometryRootParameter, rootDesc->Desc_1_0.pParameters,
            rootDesc->Desc_1_0.NumParameters);
        break;
      case D3D_ROOT_SIGNATURE_VERSION_1_1:
        geometryCaptured = captureGeometryRange(
            impl.d3d12.geometryRootParameter, rootDesc->Desc_1_1.pParameters,
            rootDesc->Desc_1_1.NumParameters);
        break;
      default:
        geometryCaptured = captureGeometryRange(
            impl.d3d12.geometryRootParameter, rootDesc->Desc_1_2.pParameters,
            rootDesc->Desc_1_2.NumParameters);
        break;
      }
      if (shaderHasGeometry && !geometryCaptured)
        FatalError("RIProgram: embedded D3D12 root signature has no t4/space3 "
                   "geometry range\n");
      if (shaderHasGeometry &&
          (impl.d3d12.geometryRootParameter ==
               impl.d3d12.resourceRootParameter ||
           impl.d3d12.geometryRootParameter == impl.d3d12.samplerRootParameter))
        FatalError("RIProgram: embedded D3D12 geometry range is not in a "
                   "dedicated root table\n");
    }
    for (size_t set = 0;
         set < externalLayouts.size() && set < programDescriptors.size();
         ++set) {
      const RIBindlessD3D12Layout &layout = externalLayouts[set].d3d12;
      if (!layout.bindings.empty()) {
        if (layout.rootSignature &&
            layout.rootSignature != impl.d3d12.rootSignature)
          FatalError("RIProgram: external D3D12 layout was built for a "
                     "different root signature\n");
        auto &slot = programDescriptors[set];
        slot.isExternal = !slot.d3d12ExternalTables.empty();
        slot.d3d12.rootSignature = impl.d3d12.rootSignature;
        continue;
      }
      // A null root signature is the global-set wildcard: the set is created
      // before programs and therefore cannot borrow one program's signature.
      // Its table indices are likewise wildcards; the program supplies the
      // indices when binding while geometry metadata remains validated.
      if (!layout.rootSignature && (layout.geometryRangeCount != UINT_MAX ||
                                    layout.geometryRangeOffset != 0))
        continue;
      // The command list binds this program's root signature before setting
      // the external tables.  A caller-owned layout is therefore compatible
      // only when it was built for this exact signature and exposes the same
      // table/geometry contract; equivalent-looking parameter indices are not
      // sufficient because descriptor ranges are part of the root signature.
      if ((layout.rootSignature &&
           layout.rootSignature != impl.d3d12.rootSignature) ||
          (layout.rootSignature && shaderHasResources &&
           !validTableParameter(layout.resourceRootParameter)) ||
          (layout.rootSignature && shaderHasGeometry &&
           !validTableParameter(layout.geometryRootParameter)) ||
          (layout.rootSignature && shaderHasSamplers &&
           !validTableParameter(layout.samplerRootParameter)) ||
          (layout.rootSignature &&
           layout.resourceRootParameter != impl.d3d12.resourceRootParameter) ||
          (layout.rootSignature &&
           layout.geometryRootParameter != impl.d3d12.geometryRootParameter) ||
          (layout.rootSignature &&
           layout.samplerRootParameter != impl.d3d12.samplerRootParameter) ||
          (layout.rootSignature && shaderHasGeometry &&
           (layout.geometryRootParameter == layout.resourceRootParameter ||
            layout.geometryRootParameter == layout.samplerRootParameter)) ||
          (shaderHasGeometry &&
           (layout.geometryRangeOffset != 0 ||
            layout.geometryRangeCount != impl.d3d12.geometryRangeCount)))
        FatalError("RIProgram: external D3D12 layout is incompatible with the "
                   "program root signature\n");
      programDescriptors[set].isExternal = true;
      programDescriptors[set].d3d12.rootSignature = impl.d3d12.rootSignature;
      programDescriptors[set].d3d12.resourceRootParameter =
          impl.d3d12.resourceRootParameter;
      programDescriptors[set].d3d12.geometryRootParameter =
          impl.d3d12.geometryRootParameter;
      programDescriptors[set].d3d12.samplerRootParameter =
          impl.d3d12.samplerRootParameter;
      programDescriptors[set].d3d12.geometryRangeOffset =
          layout.geometryRangeOffset;
    }
    if (deserializer)
      deserializer->Release();
    if (rootBlob)
      rootBlob->Release();
    // Root parameters this program's shaders genuinely read. A table the
    // signature declares but no shader touches is left out, so an unused one
    // never trips the draw-time check. Parameters past 63 are not tracked.
    {
      impl.d3d12.rootArgumentMask = 0;
      auto require = [&](uint32_t parameter, bool used) {
        if (used && parameter != UINT32_MAX && parameter < 64)
          impl.d3d12.rootArgumentMask |= 1ull << parameter;
      };
      require(impl.d3d12.resourceRootParameter, shaderHasResources);
      require(impl.d3d12.samplerRootParameter, shaderHasSamplers);
      require(impl.d3d12.geometryRootParameter, shaderHasGeometry);
      require(impl.d3d12.pushConstantRootParameter,
              impl.d3d12.pushConstantSize != 0);
      for (const auto &slot : programDescriptors)
        for (const auto &table : slot.d3d12ExternalTables)
          require(table.rootParameter, true);
    }
    // Carry JSON reflection into the backend-neutral lookup table as well.
    // This closes the transport gap for descriptor-name/slot matching on
    // D3D12 without asking SPIRV-Reflect to parse DXIL.
    auto externalBindingFor =
        [&](const ShaderResourceReflection &resource,
            uint32_t &setOut) -> const RIBindlessD3D12Binding * {
      const RIBindlessRegisterClass registerClass =
          resource.registerClass == ShaderRegisterClass::CBV
              ? RIBindlessRegisterClass::CBV
          : resource.registerClass == ShaderRegisterClass::SRV
              ? RIBindlessRegisterClass::SRV
          : resource.registerClass == ShaderRegisterClass::UAV
              ? RIBindlessRegisterClass::UAV
              : RIBindlessRegisterClass::Sampler;
      const RIBindlessD3D12Binding *match = nullptr;
      for (uint32_t set = 0;
           set < externalLayouts.size() && set < programDescriptors.size();
           ++set) {
        for (const auto &binding : externalLayouts[set].d3d12.bindings) {
          if (binding.registerClass != registerClass ||
              binding.registerIndex != resource.registerIndex ||
              binding.registerSpace != resource.registerSpace)
            continue;
          if (match)
            FatalError("RIProgram: reflected D3D12 binding '%s' matches "
                       "multiple external layouts\n",
                       resource.name.c_str());
          match = &binding;
          setOut = set;
        }
      }
      return match;
    };
    std::unordered_map<std::string, ShaderResourceReflection> mergedResources;
    for (const auto &bin : shaderBin) {
      if (!bin.reflection)
        continue;
      const auto &reflection = *bin.reflection;
      if (reflection.pushConstants.present) {
        hasPushConstant = true;
        reflection_len = static_cast<uint16_t>(
            std::min<uint32_t>(reflection.pushConstants.size, UINT16_MAX));
      }
      for (const auto &resource : reflection.resources) {
        if (!resource.used || resource.name.empty())
          continue;
        auto mergedIt = mergedResources.find(resource.name);
        if (mergedIt == mergedResources.end()) {
          mergedResources.emplace(resource.name, resource);
        } else {
          const auto &prior = mergedIt->second;
          const bool shapeConflict =
              prior.registerClass != resource.registerClass ||
              prior.registerIndex != resource.registerIndex ||
              prior.registerSpace != resource.registerSpace ||
              prior.arrayCount != resource.arrayCount ||
              prior.unbounded != resource.unbounded ||
              (prior.stride && resource.stride &&
               prior.stride != resource.stride) ||
              (prior.size && resource.size && prior.size != resource.size) ||
              (!prior.type.empty() && !resource.type.empty() &&
               prior.type != resource.type) ||
              (!prior.format.empty() && !resource.format.empty() &&
               prior.format != resource.format);
          if (shapeConflict)
            FatalError("RIProgram: conflicting reflected D3D12 resource '%s' "
                       "across shader stages\n",
                       resource.name.c_str());
        }
        const DescriptorBindingID id =
            CreateDescriptorBindingID(resource.name.c_str());
        auto it = std::find_if(
            bindingReflection.begin(), bindingReflection.end(),
            [&](const BindingReflection &r) { return r.hash == id.hash; });
        const bool newReflection = it == bindingReflection.end();
        if (newReflection) {
          bindingReflection.push_back({});
          it = std::prev(bindingReflection.end());
        }
        uint32_t externalSet = UINT32_MAX;
        const RIBindlessD3D12Binding *externalBinding =
            externalBindingFor(resource, externalSet);
        const uint32_t logicalSet =
            externalBinding ? externalSet
                            : std::min<uint32_t>(resource.registerSpace, 7u);
        const uint32_t logicalBinding =
            externalBinding ? externalBinding->binding : resource.registerIndex;
        it->hash = id.hash;
        it->set = static_cast<uint16_t>(logicalSet);
        it->baseRegisterIndex = static_cast<uint16_t>(
            std::min<uint32_t>(logicalBinding, UINT16_MAX));
        it->isArray = resource.unbounded || resource.arrayCount > 1;
        it->dimCount = static_cast<uint16_t>(std::min<uint32_t>(
            resource.unbounded ? 0 : resource.arrayCount, 255));
        switch (resource.registerClass) {
        case ShaderRegisterClass::CBV:
          it->registerClass = RIBindlessRegisterClass::CBV;
          break;
        case ShaderRegisterClass::SRV:
          it->registerClass = RIBindlessRegisterClass::SRV;
          break;
        case ShaderRegisterClass::UAV:
          it->registerClass = RIBindlessRegisterClass::UAV;
          break;
        case ShaderRegisterClass::Sampler:
          it->registerClass = RIBindlessRegisterClass::Sampler;
          break;
        default:
          FatalError(
              "RIProgram: conflicting D3D12 reflection register class\n");
        }
        it->d3d12Sampler =
            it->registerClass == RIBindlessRegisterClass::Sampler;
        it->descriptorCount = std::max(1u, resource.arrayCount);
        it->d3d12External = externalBinding != nullptr;
        it->d3d12ExternalSet =
            externalBinding ? static_cast<uint8_t>(externalSet) : UINT8_MAX;
        if (!newReflection &&
            (it->registerClass !=
                 (resource.registerClass == ShaderRegisterClass::CBV
                      ? RIBindlessRegisterClass::CBV
                  : resource.registerClass == ShaderRegisterClass::SRV
                      ? RIBindlessRegisterClass::SRV
                  : resource.registerClass == ShaderRegisterClass::UAV
                      ? RIBindlessRegisterClass::UAV
                      : RIBindlessRegisterClass::Sampler) ||
             it->baseRegisterIndex != logicalBinding || it->set != logicalSet ||
             it->descriptorCount != std::max(1u, resource.arrayCount) ||
             it->isArray != (resource.unbounded || resource.arrayCount > 1))) {
          FatalError("RIProgram: conflicting reflected D3D12 binding '%s' "
                     "across shader stages\n",
                     resource.name.c_str());
        }
#if !defined(NDEBUG)
        it->debugName = resource.name;
#endif
      }
    }

    // The reflected root-table builder keeps bounded ordinary ranges first and
    // the single unbounded range last. Rebuild compact-arena offsets from that
    // same order after all stages have been merged; assigning them while
    // iterating stages makes a bounded range after an unbounded one overlap
    // the table layout. Geometry remains the dedicated range at element 0.
    auto assignD3D12TableOffsets = [&](bool sampler) {
      uint32_t offset = 0;
      for (int unboundedPass = 0; unboundedPass != 2; ++unboundedPass) {
        for (BindingReflection &binding : bindingReflection) {
          if (binding.d3d12Sampler != sampler || binding.d3d12External)
            continue;
          const bool unbounded = binding.isArray && binding.dimCount == 0;
          if (static_cast<int>(unbounded) != unboundedPass)
            continue;
          if (offset > UINT32_MAX - binding.descriptorCount)
            FatalError(
                "RIProgram: reflected D3D12 descriptor table is too large\n");
          binding.d3d12DescriptorOffset = offset;
          offset += binding.descriptorCount;
        }
      }
    };
    assignD3D12TableOffsets(false);
    assignD3D12TableOffsets(true);
    return;
  }
#endif

#if (DEVICE_IMPL_VULKAN)
  // Mirror the D3D12 arm's runtime dispatch above. Reaching here on a backend
  // that is not Vulkan means no arm initialized the program: shaderBin and
  // impl.vk.pipelineLayout would stay empty and the failure would surface at
  // the first draw as an anonymous assert in bindPipeline. Name it here.
  if (!RIIsTargetSelected(RI_DEVICE_API_VK))
    FatalError("RIProgram '%s': active backend %u has no initialize path\n",
               debugName ? debugName : "<unnamed program>",
               RIActiveBackendApi());

  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  std::vector<VkDescriptorSetLayoutBinding>
      descriptorSetLayoutBindings[DESCRIPTOR_SET_MAX];
  std::vector<VkDescriptorBindingFlags>
      descriptorBindingFlags[DESCRIPTOR_SET_MAX];
  VkDescriptorSetLayout setLayouts[DESCRIPTOR_SET_MAX] = {0};
  VkPushConstantRange pushConstantRange = {0};
  std::vector<SpvReflectDescriptorSet *> reflectionDescSets;

  auto externalLayoutFor = [&](size_t setIndex) -> VkDescriptorSetLayout {
    if (setIndex < externalLayouts.size())
      return externalLayouts[setIndex].vk;
    return VK_NULL_HANDLE;
  };

  for (auto &init : moduleInit) {
    if (init.format == RIShaderArtifactFormat::Unknown)
      init.format = ri_detectShaderFormat(
          std::span<const char>(init.data.data(), init.data.size()));
    const RIShaderArtifactFormat format = init.format;
    if (format != RIShaderArtifactFormat::Spirv)
      FatalError(
          "RIProgram: Vulkan program requires SPIR-V shader artifacts\n");
    auto *bin = &shaderBin[init.stage];
    bin->buf.insert(bin->buf.begin(), init.data.begin(), init.data.end());
    if (init.entryPoint && *init.entryPoint)
      bin->entryPoint = init.entryPoint;
    SpvReflectShaderModule module = {};
    SpvReflectResult result = spvReflectCreateShaderModule(
        init.data.size(), init.data.data(), &module);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    {
      uint32_t pushConstantCount = 0;
      result = spvReflectEnumeratePushConstantBlocks(&module,
                                                     &pushConstantCount, NULL);
      assert(result == SPV_REFLECT_RESULT_SUCCESS);
      if (pushConstantCount > 1) {
        FatalError("RIProgram: stage %u declares %u push constant blocks; only "
                   "1 supported per stage!\n",
                   init.stage, pushConstantCount);
        assert(false && "multiple push-constant blocks in a single stage");
      } else if (pushConstantCount == 1) {
        SpvReflectBlockVariable *blocks[1] = {nullptr};
        result = spvReflectEnumeratePushConstantBlocks(
            &module, &pushConstantCount, blocks);
        assert(result == SPV_REFLECT_RESULT_SUCCESS);

        VkShaderStageFlags stageBit = 0;
        switch (init.stage) {
        case PROGRAM_STAGE_VERTEX:
          stageBit = VK_SHADER_STAGE_VERTEX_BIT;
          break;
        case PROGRAM_STAGE_FRAGMENT:
          stageBit = VK_SHADER_STAGE_FRAGMENT_BIT;
          break;
        case PROGRAM_STAGE_COMPUTE:
          stageBit = VK_SHADER_STAGE_COMPUTE_BIT;
          break;
        case PROGRAM_STAGE_RAYGEN:
          stageBit = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
          break;
        case PROGRAM_STAGE_MISS:
          stageBit = VK_SHADER_STAGE_MISS_BIT_KHR;
          break;
        case PROGRAM_STAGE_CLOSEST_HIT:
          stageBit = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_ANY_HIT:
          stageBit = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_INTERSECTION:
          stageBit = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
          break;
        case PROGRAM_STAGE_CALLABLE:
          stageBit = VK_SHADER_STAGE_CALLABLE_BIT_KHR;
          break;
        default:
          assert(false);
          break;
        }

        pushConstantRange.stageFlags |= stageBit;
        pushConstantRange.size =
            std::max(pushConstantRange.size, blocks[0]->size);
        pushConstantRange.offset = 0;
        impl.vk.pushConstant.shaderStageFlags |= stageBit;
        impl.vk.pushConstant.size =
            std::max<uint32_t>(impl.vk.pushConstant.size, blocks[0]->size);
        hasPushConstant = true;
      }
    }

    if (init.stage == PROGRAM_STAGE_VERTEX) {
      for (size_t i = 0; i < module.input_variable_count; i++) {
        const uint32_t location = module.input_variables[i]->location;
        if (location >= vertex_input_format.size())
          continue;
        vertex_input_mask |= (1u << location);
        vertex_input_format[location] =
            (uint32_t)module.input_variables[i]->format;
      }
    }

    uint32_t reflectionDescriptorCount = 0;
    result = spvReflectEnumerateDescriptorSets(
        &module, &reflectionDescriptorCount, NULL);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    reflectionDescSets.resize(reflectionDescriptorCount);
    result = spvReflectEnumerateDescriptorSets(
        &module, &reflectionDescriptorCount, reflectionDescSets.data());
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    for (size_t i_set = 0; i_set < reflectionDescriptorCount; i_set++) {
      const SpvReflectDescriptorSet *spv_reflection = reflectionDescSets[i_set];
      assert(spv_reflection->set < programDescriptors.size());
      struct DescriptorSetSlot *program_desc =
          &programDescriptors[spv_reflection->set];
      program_desc->alloc.descriptor_alloc_handle = vkDescriptorSetAlloc;
      program_desc->alloc.framesInFlight = RI_NUMBER_FRAMES_FLIGHT;
      for (size_t i_binding = 0; i_binding < spv_reflection->binding_count;
           i_binding++) {
        const SpvReflectDescriptorBinding *reflectionBinding =
            spv_reflection->bindings[i_binding];
        assert(reflectionBinding->array.dims_count <=
               1); // not going to handle multi-dim arrays
        DescriptorBindingID reflID =
            CreateDescriptorBindingID(reflectionBinding->name);
        struct RIProgram::BindingReflection *reflc = NULL;
        for (auto &ref : bindingReflection) {
          if (ref.hash == reflID.hash) {
            reflc = &ref;
            break;
          }
        }
        if (reflc == NULL) {
          bindingReflection.push_back({});
          reflc = &bindingReflection.back();
        }

        reflc->hash = reflID.hash;
        reflc->set = reflectionBinding->set;
        reflc->baseRegisterIndex = reflectionBinding->binding;
        reflc->isArray = reflectionBinding->count > 1;
        reflc->dimCount = std::max<uint16_t>(1, reflectionBinding->count);
#if !defined(NDEBUG)
        reflc->debugName =
            reflectionBinding->name ? reflectionBinding->name : "<unnamed>";
#endif

        VkDescriptorSetLayoutBinding *layoutBinding = NULL;
        VkDescriptorBindingFlags *bindingFlags = NULL;
        for (size_t i = 0;
             i < descriptorSetLayoutBindings[spv_reflection->set].size(); i++) {
          if (descriptorSetLayoutBindings[spv_reflection->set][i].binding ==
              reflectionBinding->binding) {
            layoutBinding =
                &descriptorSetLayoutBindings[spv_reflection->set][i];
            bindingFlags = &descriptorBindingFlags[spv_reflection->set][i];
          }
        }

        if (!layoutBinding) {
          VkDescriptorSetLayoutBinding bindings = {0};
          VkDescriptorBindingFlags flags = 0;
          descriptorSetLayoutBindings[spv_reflection->set].push_back(bindings);
          descriptorBindingFlags[spv_reflection->set].push_back(flags);
          layoutBinding =
              &descriptorSetLayoutBindings
                  [spv_reflection->set]
                  [descriptorSetLayoutBindings[spv_reflection->set].size() - 1];
          bindingFlags =
              &descriptorBindingFlags
                  [spv_reflection->set]
                  [descriptorBindingFlags[spv_reflection->set].size() - 1];
        }

        // Scene TLAS bindings can be absent before the first world build.
        // A shader guard prevents dynamic access, but optional=true alone
        // only suppresses our diagnostic. PARTIALLY_BOUND is what makes an
        // unwritten, dynamically unused descriptor legal in Vulkan.
        if (reflc->isArray ||
            reflectionBinding->descriptor_type ==
                SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
          (*bindingFlags) = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }

        const uint32_t bindingCount =
            std::max<uint32_t>(reflectionBinding->count, 1);
        layoutBinding->binding = reflectionBinding->binding;
        layoutBinding->descriptorCount = bindingCount;
        switch (init.stage) {
        case PROGRAM_STAGE_VERTEX:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
          break;
        case PROGRAM_STAGE_FRAGMENT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
          break;
        case PROGRAM_STAGE_COMPUTE:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
          break;
        case PROGRAM_STAGE_RAYGEN:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
          break;
        case PROGRAM_STAGE_MISS:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_MISS_BIT_KHR;
          break;
        case PROGRAM_STAGE_CLOSEST_HIT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_ANY_HIT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_INTERSECTION:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
          break;
        case PROGRAM_STAGE_CALLABLE:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
          break;
        default:
          assert(false);
          break;
        }
        switch (reflectionBinding->descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
          program_desc->samplerMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          program_desc->textureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
          program_desc->bufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          program_desc->storageTextureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
          program_desc->storageBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          program_desc->constantBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          program_desc->structuredBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          program_desc->combinedImageSamplerMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          program_desc->accelerationStructureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
          assert(false);
          break;
        }
      }
    }

    // Everything needed has been folded into bindingReflection / vertex input
    // masks / the accumulated layout bindings; free the reflection tree.
    spvReflectDestroyShaderModule(&module);
  }

  uint32_t numLayoutCount = 0;
  for (size_t bindingIdx = 0; bindingIdx < DESCRIPTOR_SET_MAX; bindingIdx++) {
    if (descriptorSetLayoutBindings[bindingIdx].size() > 0 ||
        externalLayoutFor(bindingIdx) != VK_NULL_HANDLE) {
      numLayoutCount = static_cast<uint32_t>(bindingIdx + 1);
    }
  }

  for (size_t bindingIdx = 0; bindingIdx < numLayoutCount; bindingIdx++) {
    VkDescriptorSetLayout external = externalLayoutFor(bindingIdx);
    if (external != VK_NULL_HANDLE) {
      // Caller-owned layout. Use it directly; mark the slot external
      // so bindDescriptors skips alloc/write/bind for this set.
      setLayouts[bindingIdx] = external;
      programDescriptors[bindingIdx].isExternal = true;
    } else if (descriptorSetLayoutBindings[bindingIdx].size() > 0) {
      VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo = {
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
      bindingFlagsInfo.bindingCount =
          static_cast<uint32_t>(descriptorBindingFlags[bindingIdx].size());
      bindingFlagsInfo.pBindingFlags =
          descriptorBindingFlags[bindingIdx].data();

      VkDescriptorSetLayoutCreateInfo createSetLayoutInfo = {
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      createSetLayoutInfo.bindingCount =
          static_cast<uint32_t>(descriptorSetLayoutBindings[bindingIdx].size());
      createSetLayoutInfo.pBindings =
          descriptorSetLayoutBindings[bindingIdx].data();
      R_VK_ADD_STRUCT(&createSetLayoutInfo, &bindingFlagsInfo);

      VK_WrapResult(vkCreateDescriptorSetLayout(device->vk.device,
                                                &createSetLayoutInfo, NULL,
                                                setLayouts + bindingIdx));
    } else {
      VkDescriptorSetLayoutCreateInfo createSetLayoutInfo = {
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      VK_WrapResult(vkCreateDescriptorSetLayout(device->vk.device,
                                                &createSetLayoutInfo, NULL,
                                                setLayouts + bindingIdx));
    }
    programDescriptors[bindingIdx].vk.setLayout = setLayouts[bindingIdx];
  }
  pipelineLayoutCreateInfo.pSetLayouts = setLayouts;
  pipelineLayoutCreateInfo.setLayoutCount = numLayoutCount;
  if (pushConstantRange.stageFlags > 0) {
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
  }
  VK_WrapResult(vkCreatePipelineLayout(device->vk.device,
                                       &pipelineLayoutCreateInfo, NULL,
                                       &impl.vk.pipelineLayout));

  // Tag the pipeline layout + owned set layouts so the validation layer
  // reports the owning program by name when it flags a leak at
  // vkDestroyDevice (helps pin an un-disposed RIProgram to a call site).
  if (debugName && vkSetDebugUtilsObjectNameEXT) {
    char nameBuf[128];
    snprintf(nameBuf, sizeof(nameBuf), "%s.pipelineLayout", debugName);
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
        VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)impl.vk.pipelineLayout,
        nameBuf};
    vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo);
    for (size_t bindingIdx = 0; bindingIdx < numLayoutCount; bindingIdx++) {
      if (programDescriptors[bindingIdx].isExternal ||
          setLayouts[bindingIdx] == VK_NULL_HANDLE) {
        continue;
      }
      snprintf(nameBuf, sizeof(nameBuf), "%s.setLayout[%zu]", debugName,
               bindingIdx);
      VkDebugUtilsObjectNameInfoEXT setInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
          (uint64_t)setLayouts[bindingIdx], nameBuf};
      vkSetDebugUtilsObjectNameEXT(device->vk.device, &setInfo);
    }
  }
#endif // DEVICE_IMPL_VULKAN
}

void RIProgram::dispose(RIDevice *device) {
  assert(device);
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12)) {
    const RIDescriptorArenaFence retire =
        ri_d3d12_graphics_retire_fence(device);
    for (auto &entry : d3d12DescriptorCache) {
      ri_d3d12_release_cache_refs(entry.resources, entry.allocations);
      releaseDescriptorArena(device, &entry.allocation, &retire);
    }
    g_riD3D12DescriptorCacheEntries -=
        static_cast<uint32_t>(d3d12DescriptorCache.size());
    d3d12DescriptorCache.clear();
    d3d12DescriptorIndex.clear();
    d3d12DescriptorRecycleCursor = 0;
    d3d12ArenaExhaustedReported = false;
    for (auto &[hash, slot] : pipeline) {
      if (slot.d3d12.handle) {
        slot.d3d12.handle->Release();
        slot.d3d12.handle = nullptr;
      }
      slot.d3d12.topology = 0;
    }
    pipeline.clear();
    if (impl.d3d12.rootSignature) {
      impl.d3d12.rootSignature->Release();
      impl.d3d12.rootSignature = nullptr;
    }
    d3d12VertexBindingStrides = {};
    d3d12VertexBindingCount = 0;
    for (auto &[hash, slot] : rtPipeline) {
      if (slot.d3d12.handle) {
        slot.d3d12.handle->Release();
        slot.d3d12.handle = nullptr;
      }
      // Through RIBuffer::dispose rather than releasing the resource here, so
      // the D3D12 buffer registration is retired with it.
      slot.sbt.dispose(device);
    }
    rtPipeline.clear();
    this->device = nullptr;
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  for (auto &[hash, slot] : pipeline) {
    if (slot.vk.handle != VK_NULL_HANDLE)
      vkDestroyPipeline(device->vk.device, slot.vk.handle, NULL);
  }
  pipeline.clear();
  for (auto &[hash, slot] : rtPipeline) {
    if (slot.vk.handle != VK_NULL_HANDLE)
      vkDestroyPipeline(device->vk.device, slot.vk.handle, NULL);
    if (slot.vk.sbtBuffer != VK_NULL_HANDLE)
      vmaDestroyBuffer(device->vk.vmaAllocator, slot.vk.sbtBuffer,
                       slot.vk.sbtAlloc);
  }
  rtPipeline.clear();
  for (auto &slot : programDescriptors) {
    freeDescriptorSetAlloc(device, &slot.alloc);
    if (!slot.isExternal && slot.vk.setLayout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device->vk.device, slot.vk.setLayout, NULL);
    slot.vk.setLayout = VK_NULL_HANDLE;
    slot.isExternal = false;
  }
  if (impl.vk.pipelineLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device->vk.device, impl.vk.pipelineLayout, NULL);
    impl.vk.pipelineLayout = VK_NULL_HANDLE;
  }
  this->device = NULL;
#endif
}

} // namespace hpl
