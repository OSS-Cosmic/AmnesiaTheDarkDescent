// D3D12 texture lifecycle helpers, paralleling RID3D12Buffer.cpp.
#include "graphics/RID3D12.h"

#if DEVICE_IMPL_D3D12

#include "graphics/RIDescriptor.h"
#include "graphics/RIDevice.h"
#include "graphics/RIFormat.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"

#include <D3D12MemAlloc.h>

#include <cstring>

// Declared in RID3D12.h, symmetric with RIFormatToVK in RIVK.h. Non-static so
// the acceleration-structure path can translate BLAS vertex formats.
DXGI_FORMAT RIFormatToD3D12(uint32_t format) {
  switch (format) {
  case RI_FORMAT_UNKNOWN:
    return DXGI_FORMAT_UNKNOWN;
  case RI_FORMAT_L8_A8_UNORM:
    return DXGI_FORMAT_R8G8_UNORM;
  case RI_FORMAT_L8_UNORM:
    return DXGI_FORMAT_R8_UNORM;
  case RI_FORMAT_A8_UNORM:
    return DXGI_FORMAT_A8_UNORM;
  case RI_FORMAT_R8_SNORM:
    return DXGI_FORMAT_R8_SNORM;
  case RI_FORMAT_R8_UINT:
    return DXGI_FORMAT_R8_UINT;
  case RI_FORMAT_R8_SINT:
    return DXGI_FORMAT_R8_SINT;
  case RI_FORMAT_RG8_UNORM:
    return DXGI_FORMAT_R8G8_UNORM;
  case RI_FORMAT_RG8_SNORM:
    return DXGI_FORMAT_R8G8_SNORM;
  case RI_FORMAT_RG8_UINT:
    return DXGI_FORMAT_R8G8_UINT;
  case RI_FORMAT_RG8_SINT:
    return DXGI_FORMAT_R8G8_SINT;
  case RI_FORMAT_BGRA8_UNORM:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case RI_FORMAT_BGRA8_SRGB:
    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  case RI_FORMAT_RGBA8_UNORM:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case RI_FORMAT_RGBA8_SNORM:
    return DXGI_FORMAT_R8G8B8A8_SNORM;
  case RI_FORMAT_RGBA8_UINT:
    return DXGI_FORMAT_R8G8B8A8_UINT;
  case RI_FORMAT_RGBA8_SINT:
    return DXGI_FORMAT_R8G8B8A8_SINT;
  case RI_FORMAT_RGBA8_SRGB:
    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case RI_FORMAT_R16_UNORM:
    return DXGI_FORMAT_R16_UNORM;
  case RI_FORMAT_R16_SNORM:
    return DXGI_FORMAT_R16_SNORM;
  case RI_FORMAT_R16_UINT:
    return DXGI_FORMAT_R16_UINT;
  case RI_FORMAT_R16_SINT:
    return DXGI_FORMAT_R16_SINT;
  case RI_FORMAT_R16_SFLOAT:
    return DXGI_FORMAT_R16_FLOAT;
  case RI_FORMAT_RG16_UNORM:
    return DXGI_FORMAT_R16G16_UNORM;
  case RI_FORMAT_RG16_SNORM:
    return DXGI_FORMAT_R16G16_SNORM;
  case RI_FORMAT_RG16_UINT:
    return DXGI_FORMAT_R16G16_UINT;
  case RI_FORMAT_RG16_SINT:
    return DXGI_FORMAT_R16G16_SINT;
  case RI_FORMAT_RG16_SFLOAT:
    return DXGI_FORMAT_R16G16_FLOAT;
  case RI_FORMAT_RGBA16_UNORM:
    return DXGI_FORMAT_R16G16B16A16_UNORM;
  case RI_FORMAT_RGBA16_SNORM:
    return DXGI_FORMAT_R16G16B16A16_SNORM;
  case RI_FORMAT_RGBA16_UINT:
    return DXGI_FORMAT_R16G16B16A16_UINT;
  case RI_FORMAT_RGBA16_SINT:
    return DXGI_FORMAT_R16G16B16A16_SINT;
  case RI_FORMAT_RGBA16_SFLOAT:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case RI_FORMAT_R32_UINT:
    return DXGI_FORMAT_R32_UINT;
  case RI_FORMAT_R32_SINT:
    return DXGI_FORMAT_R32_SINT;
  case RI_FORMAT_R32_SFLOAT:
    return DXGI_FORMAT_R32_FLOAT;
  case RI_FORMAT_RG32_UINT:
    return DXGI_FORMAT_R32G32_UINT;
  case RI_FORMAT_RG32_SINT:
    return DXGI_FORMAT_R32G32_SINT;
  case RI_FORMAT_RG32_SFLOAT:
    return DXGI_FORMAT_R32G32_FLOAT;
  case RI_FORMAT_RGB32_UINT:
    return DXGI_FORMAT_R32G32B32_UINT;
  case RI_FORMAT_RGB32_SINT:
    return DXGI_FORMAT_R32G32B32_SINT;
  case RI_FORMAT_RGB32_SFLOAT:
    return DXGI_FORMAT_R32G32B32_FLOAT;
  case RI_FORMAT_RGBA32_UINT:
    return DXGI_FORMAT_R32G32B32A32_UINT;
  case RI_FORMAT_RGBA32_SINT:
    return DXGI_FORMAT_R32G32B32A32_SINT;
  case RI_FORMAT_RGBA32_SFLOAT:
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
  case RI_FORMAT_R10_G10_B10_A2_UNORM:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case RI_FORMAT_R10_G10_B10_A2_UINT:
    return DXGI_FORMAT_R10G10B10A2_UINT;
  case RI_FORMAT_R11_G11_B10_UFLOAT:
    return DXGI_FORMAT_R11G11B10_FLOAT;
  case RI_FORMAT_R9_G9_B9_E5_UNORM:
    return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
  case RI_FORMAT_R5_G6_B5_UNORM:
    return DXGI_FORMAT_B5G6R5_UNORM;
  case RI_FORMAT_R5_G5_B5_A1_UNORM:
    return DXGI_FORMAT_B5G5R5A1_UNORM;
  case RI_FORMAT_R4_G4_B4_A4_UNORM:
    return DXGI_FORMAT_B4G4R4A4_UNORM;
  case RI_FORMAT_BC1_RGBA_UNORM:
    return DXGI_FORMAT_BC1_UNORM;
  case RI_FORMAT_BC1_RGBA_SRGB:
    return DXGI_FORMAT_BC1_UNORM_SRGB;
  case RI_FORMAT_BC2_RGBA_UNORM:
    return DXGI_FORMAT_BC2_UNORM;
  case RI_FORMAT_BC2_RGBA_SRGB:
    return DXGI_FORMAT_BC2_UNORM_SRGB;
  case RI_FORMAT_BC3_RGBA_UNORM:
    return DXGI_FORMAT_BC3_UNORM;
  case RI_FORMAT_BC3_RGBA_SRGB:
    return DXGI_FORMAT_BC3_UNORM_SRGB;
  case RI_FORMAT_BC4_R_UNORM:
    return DXGI_FORMAT_BC4_UNORM;
  case RI_FORMAT_BC4_R_SNORM:
    return DXGI_FORMAT_BC4_SNORM;
  case RI_FORMAT_BC5_RG_UNORM:
    return DXGI_FORMAT_BC5_UNORM;
  case RI_FORMAT_BC5_RG_SNORM:
    return DXGI_FORMAT_BC5_SNORM;
  case RI_FORMAT_BC6H_RGB_UFLOAT:
    return DXGI_FORMAT_BC6H_UF16;
  case RI_FORMAT_BC6H_RGB_SFLOAT:
    return DXGI_FORMAT_BC6H_SF16;
  case RI_FORMAT_BC7_RGBA_UNORM:
    return DXGI_FORMAT_BC7_UNORM;
  case RI_FORMAT_BC7_RGBA_SRGB:
    return DXGI_FORMAT_BC7_UNORM_SRGB;
  case RI_FORMAT_R8_UNORM:
    return DXGI_FORMAT_R8_UNORM;
  case RI_FORMAT_D32_SFLOAT:
    return DXGI_FORMAT_D32_FLOAT;
  case RI_FORMAT_D24_UNORM_S8_UINT:
    return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case RI_FORMAT_D32_SFLOAT_S8_UINT:
    return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  case RI_FORMAT_D32_SFLOAT_S8_UINT_X24:
    return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  case RI_FORMAT_D16_UNORM:
    return DXGI_FORMAT_D16_UNORM;
  case RI_FORMAT_R24_UNORM_X8:
    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case RI_FORMAT_X24_R8_UINT:
    return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
  case RI_FORMAT_X32_R8_UINT_X24:
    return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  case RI_FORMAT_R32_SFLOAT_X8_X24:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// A depth resource that is also sampled must be created TYPELESS: D3D12 only
// allows the typed DSV format and the SRV read format (see
// ri_d3d12_texture_view) to alias a typeless resource.
static DXGI_FORMAT ri_d3d12_depth_typeless_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
  case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
  case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
  default: return DXGI_FORMAT_UNKNOWN;
  }
}

static void ri_d3d12_release_texture(RITexture &texture) {
  // CreateResource returns an explicit resource reference in addition to the
  // allocation reference. Release the resource first; allocation is allowed
  // to be null for resources that are not owned by D3D12MA.
  if (texture.d3d12.resource)
    texture.d3d12.resource->Release();
  if (texture.d3d12.allocation)
    texture.d3d12.allocation->Release();
  texture = RITexture{};
}

int RID3D12_CreateTexture(struct RIDevice &device,
                          const struct RITextureDesc &desc,
                          struct RITexture &out) {
  out = RITexture{};

  if (desc.type > RI_TEXTURE_3D ||
      desc.width == 0 ||
      (desc.height == 0 && desc.type != RI_TEXTURE_1D) ||
      (desc.type == RI_TEXTURE_3D && desc.depth == 0) ||
      (desc.type == RI_TEXTURE_3D && desc.layerNum > 1) ||
      (desc.type != RI_TEXTURE_3D && desc.depth > 1) ||
      (desc.mipNum > UINT16_MAX) || (desc.layerNum > UINT16_MAX) ||
      (desc.type == RI_TEXTURE_3D && desc.depth > UINT16_MAX)) {
    hpl::Warning("RI D3D12: texture dimensions must be non-zero\n");
    return RI_FAIL;
  }
  if (!device.d3d12.device) {
    hpl::Warning("RI D3D12: cannot create texture without a device\n");
    return RI_FAIL;
  }
  if (!device.d3d12.allocator) {
    hpl::Warning("RI D3D12: cannot create texture without a memory allocator\n");
    return RI_FAIL;
  }

  const DXGI_FORMAT format = RIFormatToD3D12(desc.format);
  if (format == DXGI_FORMAT_UNKNOWN) {
    hpl::Warning("RI D3D12: texture format is unsupported\n");
    return RI_FAIL;
  }

  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = desc.type == RI_TEXTURE_1D
                     ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                     : desc.type == RI_TEXTURE_3D
                         ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = desc.width;
  rd.Height = desc.type == RI_TEXTURE_1D ? 1 : desc.height;
  rd.DepthOrArraySize = desc.type == RI_TEXTURE_3D
                            ? uint16_t(desc.depth)
                            : uint16_t(desc.layerNum ? desc.layerNum : 1);
  rd.MipLevels = uint16_t(desc.mipNum ? desc.mipNum : 1);
  rd.Format = format;
  rd.SampleDesc = {desc.sampleCount ? desc.sampleCount : 1, 0};
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  if (desc.usage & RI_USAGE_SHADER_RESOURCE_STORAGE)
    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (desc.usage & RI_USAGE_COLOR_ATTACHMENT)
    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (desc.usage & RI_USAGE_DEPTH_STENCIL_ATTACHMENT)
    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
      !(desc.usage & RI_USAGE_SHADER_RESOURCE))
    rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
      (desc.usage & RI_USAGE_SHADER_RESOURCE)) {
    const DXGI_FORMAT typeless = ri_d3d12_depth_typeless_format(format);
    if (typeless != DXGI_FORMAT_UNKNOWN)
      rd.Format = typeless;
  }

  // Always COMMON: enhanced Barrier() can only interoperate with a texture
  // created through the legacy-state API when it is in COMMON (debug layer id
  // 1350), and every RI texture starts life with an UNDEFINED -> X barrier,
  // which the legacy path also maps to COMMON as its before-state.
  const D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

  D3D12MA::ALLOCATION_DESC allocationDesc = {};
  allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
  allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
  allocationDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
  HRESULT hr = device.d3d12.allocator->CreateResource(
      &allocationDesc, &rd, initialState, nullptr, &out.d3d12.allocation,
      IID_PPV_ARGS(&out.d3d12.resource));
  if (!D3D12_WrapResult(hr)) {
    ri_d3d12_release_texture(out);
    return RI_FAIL;
  }

  // The resource's own format (typeless for sampled depth); views carry the
  // typed format they are created with.
  out.d3d12.format = uint32_t(rd.Format);
  out.d3d12.width = desc.width;
  out.d3d12.height = desc.height;
  out.d3d12.depth = uint16_t(desc.depth);
  out.d3d12.mipNum = uint16_t(desc.mipNum ? desc.mipNum : 1);
  out.d3d12.layerNum = uint16_t(desc.layerNum ? desc.layerNum : 1);
  out.d3d12.sampleCount = uint16_t(desc.sampleCount ? desc.sampleCount : 1);
  out.d3d12.usage = desc.usage;
  return RI_SUCCESS;
}

void RID3D12_DisposeTexture(struct RIDevice &device,
                            struct RITexture &texture) {
  (void)device;
  ri_d3d12_release_texture(texture);
}

bool RID3D12_TextureIsEmpty(const struct RITexture &texture) {
  return texture.d3d12.resource == nullptr;
}

void RID3D12_SetTextureDebugName(struct RIDevice &device,
                                 struct RITexture &texture,
                                 const char *name) {
  (void)device;
  if (!texture.d3d12.resource || !name)
    return;

  wchar_t wide[256] = {};
  int sourceLength = int(strlen(name));
  int converted = 0;
  while (sourceLength > 0) {
    converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                    sourceLength, wide, 255);
    if (converted > 0)
      break;
    --sourceLength;
  }
  if (converted > 0) {
    wide[converted] = L'\0';
    texture.d3d12.resource->SetName(wide);
    if (texture.d3d12.allocation)
      texture.d3d12.allocation->SetName(wide);
  }
}

int RID3D12_CreateTextureView(struct RIDevice &device,
                              const struct RITexture &tex,
                              const struct RITextureViewDesc &desc,
                              struct RITextureView &out) {
  (void)device;
  out = RITextureView{};
  if (!tex.d3d12.resource)
    return RI_FAIL;

  const D3D12_RESOURCE_DESC resourceDesc = tex.d3d12.resource->GetDesc();
  const bool is3D = resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const bool is1D = resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
  const bool view3D = desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_3D ||
                      desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D;
  const bool view1D = desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D ||
                     desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY ||
                     desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D ||
                     desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY;
  const uint32_t mipCount = tex.d3d12.mipNum;
  const uint32_t layerCount = is3D ? 1u : tex.d3d12.layerNum;
  if (!desc.mipNum || desc.baseMip >= mipCount ||
      desc.mipNum > mipCount - desc.baseMip ||
      !desc.layerNum || desc.baseLayer >= layerCount ||
      desc.layerNum > layerCount - desc.baseLayer ||
      (is3D != view3D) || (is1D != view1D) ||
      (view3D && (desc.baseLayer != 0 || desc.layerNum != 1)) ||
      (desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE && desc.layerNum != 6) ||
      (desc.viewType == RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY &&
       (desc.baseLayer % 6 != 0 || desc.layerNum % 6 != 0)))
    return RI_FAIL;

  const DXGI_FORMAT format = RIFormatToD3D12(desc.format);
  if (format == DXGI_FORMAT_UNKNOWN) {
    hpl::Warning("RI D3D12: texture view format is unsupported\n");
    return RI_FAIL;
  }
  out.d3d12 = {tex.d3d12.resource, tex.d3d12.allocation, uint32_t(format),
               uint32_t(desc.viewType), desc.baseMip, desc.mipNum,
               desc.baseLayer, desc.layerNum};
  return RI_SUCCESS;
}

void RID3D12_DisposeTextureView(struct RIDevice &device,
                                struct RITextureView &view) {
  (void)device;
  memset(&view.d3d12, 0, sizeof(view.d3d12));
}

bool RID3D12_TextureViewIsEmpty(const struct RITextureView &view) {
  return view.d3d12.resource == nullptr;
}

void RID3D12_DisposeSampler(struct RIDevice &device, struct RISampler &sampler) {
  (void)device;
  memset(&sampler.d3d12, 0, sizeof(sampler.d3d12));
}

bool RID3D12_SamplerIsEmpty(const struct RISampler &sampler) {
  return sampler.d3d12.initialized == 0;
}

// Acceleration structures live in RID3D12AccelStructure.cpp.

#endif // DEVICE_IMPL_D3D12
