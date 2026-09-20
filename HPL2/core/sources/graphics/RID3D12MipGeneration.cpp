#include "graphics/RID3D12MipGeneration.h"
#if DEVICE_IMPL_D3D12
#include "graphics/RICommand.h"
#include "graphics/RIDevice.h"
#include "graphics/RIFormat.h"
#include "graphics/RIResourceUploader.h"
#include "ri_d3d12_mips.h"
#include <D3D12MemAlloc.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kConstantBufferSize = 1024u * 1024u;
constexpr uint32_t kConstantBufferAlignment =
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kMaxMipDispatches =
    kConstantBufferSize / kConstantBufferAlignment;
constexpr uint32_t kDescriptorsPerDispatch = 2;
static_assert(kConstantBufferAlignment == 256u,
              "D3D12 root CBV offsets must remain 256-byte aligned");

struct MipFormat {
  DXGI_FORMAT target, scratch;
  uint32_t riScratch;
};
static bool mipFormat(uint32_t f, MipFormat &o) {
  switch (f) {
  case RI_FORMAT_RGBA8_UNORM:
    o = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
         RI_FORMAT_RGBA8_UNORM};
    return true;
  case RI_FORMAT_RGBA8_SRGB:
    o = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
         RI_FORMAT_RGBA8_UNORM};
    return true;
  case RI_FORMAT_RGBA16_SFLOAT:
    o = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
         RI_FORMAT_RGBA16_SFLOAT};
    return true;
  case RI_FORMAT_RGBA32_SFLOAT:
    o = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
         RI_FORMAT_RGBA32_SFLOAT};
    return true;
  default:
    return false;
  }
}
static bool supports(ID3D12Device *d, DXGI_FORMAT f, D3D12_FORMAT_SUPPORT1 dim,
                     bool uav) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT s = {};
  s.Format = f;
  if (FAILED(
          d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s))))
    return false;
  const auto sample =
      D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
  return (s.Support1 & dim) == dim && (s.Support1 & sample) == sample &&
         (!uav || (s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
}
static bool copyCompatible(DXGI_FORMAT target, DXGI_FORMAT scratch) {
  return (target == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
          scratch == DXGI_FORMAT_R8G8B8A8_UNORM) ||
         target == scratch;
}
static void barrier(RICmd &c, RITexture &t, uint32_t before, uint32_t after,
                    uint32_t bs, uint32_t as, uint32_t mip, uint32_t layer,
                    uint32_t layers) {
  RITextureBarrier b(&t, before, after, bs, as);
  b.baseMip = uint16_t(mip);
  b.mipCount = 1;
  b.baseLayer = uint16_t(layer);
  b.layerCount = uint16_t(layers);
  c.vk_d3d12_textureBarrier(b);
}
static void views(RIDevice &d, RID3D12MipScratch &s, uint32_t srcMip,
                  uint32_t dstMip, bool is3d, uint32_t layers,
                  D3D12_CPU_DESCRIPTOR_HANDLE cpu, uint32_t stride) {
  RITexture &source = (srcMip & 1) ? s.intermediate : s.source;
  RITexture &intermediate = (srcMip & 1) ? s.source : s.intermediate;
  const DXGI_FORMAT f = static_cast<DXGI_FORMAT>(source.d3d12.format);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = f;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = f;
  if (is3d) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D = {srcMip, 1};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D = {dstMip, 0, UINT_MAX};
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray = {srcMip, 1, 0, layers, 0};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray = {dstMip, 0, layers, 0};
  }
  d.d3d12.device->CreateShaderResourceView(source.d3d12.resource, &srv, cpu);
  cpu.ptr += stride;
  d.d3d12.device->CreateUnorderedAccessView(intermediate.d3d12.resource,
                                            nullptr, &uav, cpu);
}
static void views(RIDevice &d, RID3D12MipScratch &s, uint32_t srcMip,
                  uint32_t dstMip, uint32_t layers,
                  D3D12_CPU_DESCRIPTOR_HANDLE cpu, uint32_t stride) {
  views(d, s, srcMip, dstMip, s.is3d, layers, cpu, stride);
}
static void copyMip(ID3D12GraphicsCommandList *l, RITexture &src,
                    RITexture &dst, uint32_t sm, uint32_t dm, uint32_t srcLayer,
                    uint32_t dstLayer, uint32_t layers, uint32_t w, uint32_t h,
                    uint32_t depth, bool is3d) {
  for (uint32_t n = 0; n < (is3d ? 1u : layers); ++n) {
    D3D12_TEXTURE_COPY_LOCATION a = {};
    a.pResource = src.d3d12.resource;
    a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    a.SubresourceIndex = sm + (is3d ? 0u : (srcLayer + n) * src.d3d12.mipNum);
    D3D12_TEXTURE_COPY_LOCATION b = {};
    b.pResource = dst.d3d12.resource;
    b.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    b.SubresourceIndex = dm + (is3d ? 0u : (dstLayer + n) * dst.d3d12.mipNum);
    D3D12_BOX box = {0, 0, 0, w, h, depth};
    l->CopyTextureRegion(&b, 0, 0, 0, &a, &box);
  }
}
} // namespace

RID3D12MipGeneration::RID3D12MipGeneration()
    : root_signature(nullptr), pipeline_2d_array(nullptr), pipeline_3d(nullptr),
      descriptor_heap{nullptr, nullptr}, constant_buffer{nullptr, nullptr},
      constant_allocation{nullptr, nullptr}, constant_data{nullptr, nullptr},
      constant_offset{0, 0}, descriptor_offset{0, 0}, descriptor_capacity{0, 0},
      constant_mapped{false, false}, descriptor_size(0), initialized(false) {}
bool RID3D12_MipFormatSupported(RIDevice &d, uint32_t f, bool is3d) {
  MipFormat m = {};
  if (!d.d3d12.device || !mipFormat(f, m) ||
      !copyCompatible(m.target, m.scratch))
    return false;
  auto dim =
      is3d ? D3D12_FORMAT_SUPPORT1_TEXTURE3D : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
  return supports(d.d3d12.device, m.target, dim, false) &&
         supports(d.d3d12.device, m.scratch, dim, true);
}
bool RID3D12MipGeneration::init(RIDevice &d) {
  if (!d.d3d12.device || !d.d3d12.allocator)
    return false;
  D3D12_DESCRIPTOR_RANGE r[2] = {};
  r[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
          D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  r[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 1,
          D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER p[3] = {};
  p[0] = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          {1, &r[0]},
          D3D12_SHADER_VISIBILITY_ALL};
  p[1] = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          {1, &r[1]},
          D3D12_SHADER_VISIBILITY_ALL};
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  p[2].Descriptor = {0, 0};
  D3D12_ROOT_SIGNATURE_DESC sd = {3, p, 0, nullptr,
                                  D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *b = nullptr, *e = nullptr;
  if (FAILED(D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &b,
                                         &e))) {
    if (e)
      e->Release();
    return false;
  }
  HRESULT hr = d.d3d12.device->CreateRootSignature(
      0, b->GetBufferPointer(), b->GetBufferSize(),
      IID_PPV_ARGS(&root_signature));
  b->Release();
  if (e)
    e->Release();
  if (FAILED(hr))
    return false;
  auto pso = [&](const hpl::ri_embedded::ShaderBlob &s,
                 ID3D12PipelineState **o) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC x = {};
    x.pRootSignature = root_signature;
    x.CS = {s.data, s.size};
    return d.d3d12.device->CreateComputePipelineState(&x, IID_PPV_ARGS(o));
  };
  if (FAILED(pso(hpl::ri_embedded::ri_d3d12_mips_2d_array_dxil_blob,
                 &pipeline_2d_array)) ||
      FAILED(pso(hpl::ri_embedded::ri_d3d12_mips_3d_dxil_blob, &pipeline_3d))) {
    dispose(d);
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.NumDescriptors = kMaxMipDispatches * kDescriptorsPerDispatch;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  for (uint32_t i = 0; i < 2; ++i) {
    if (FAILED(d.d3d12.device->CreateDescriptorHeap(
            &hd, IID_PPV_ARGS(&descriptor_heap[i])))) {
      dispose(d);
      return false;
    }
    descriptor_capacity[i] = hd.NumDescriptors;
  }
  descriptor_size = d.d3d12.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (uint32_t i = 0; i < 2; ++i) {
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kConstantBufferSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12MA::ALLOCATION_DESC ad = {};
    ad.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    ad.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
    if (!D3D12_WrapResult(d.d3d12.allocator->CreateResource(
            &ad, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            &constant_allocation[i], IID_PPV_ARGS(&constant_buffer[i])))) {
      dispose(d);
      return false;
    }
    if (!D3D12_WrapResult(constant_buffer[i]->Map(
            0, nullptr, reinterpret_cast<void **>(&constant_data[i])))) {
      dispose(d);
      return false;
    }
    constant_mapped[i] = true;
  }
  initialized = true;
  return true;
}
void RID3D12MipGeneration::reclaim(RIDevice &d, uint32_t set) {
  if (set >= RI_RESOURCE_MAX_SETS)
    return;
  for (auto &s : scratch[set]) {
    s.source.dispose(&d);
    s.intermediate.dispose(&d);
  }
  scratch[set].clear();
  constant_offset[set] = 0;
  descriptor_offset[set] = 0;
}
void RID3D12MipGeneration::dispose(RIDevice &d) {
  reclaim(d, 0);
  reclaim(d, 1);
  for (uint32_t i = 0; i < 2; ++i) {
    if (constant_buffer[i]) {
      if (constant_mapped[i])
        constant_buffer[i]->Unmap(0, nullptr);
      constant_buffer[i]->Release();
      constant_buffer[i] = nullptr;
    }
    if (constant_allocation[i]) {
      constant_allocation[i]->Release();
      constant_allocation[i] = nullptr;
    }
    constant_data[i] = nullptr;
    constant_mapped[i] = false;
  }
  for (uint32_t i = 0; i < 2; ++i) {
    if (descriptor_heap[i])
      descriptor_heap[i]->Release();
    descriptor_heap[i] = nullptr;
    descriptor_capacity[i] = 0;
  }
  if (pipeline_3d)
    pipeline_3d->Release();
  if (pipeline_2d_array)
    pipeline_2d_array->Release();
  if (root_signature)
    root_signature->Release();
  pipeline_3d = pipeline_2d_array = nullptr;
  root_signature = nullptr;
  descriptor_size = 0;
  initialized = false;
}
bool RID3D12MipGeneration::generate(RIDevice &d, RICmd &cmd, uint32_t set,
                                    const RIGenerateMipsDesc &x) {
  MipFormat mf = {};
  bool is3d = x.depth > 1;
  uint32_t layers = x.layerNum ? x.layerNum : 1;
  if (!initialized || set >= RI_RESOURCE_MAX_SETS || !mipFormat(x.format, mf) ||
      !RID3D12_MipFormatSupported(d, x.format, is3d) ||
      !x.target.d3d12.resource || !x.mipNum || x.mipNum < 2 ||
      x.mipNum > x.target.d3d12.mipNum ||
      (!is3d && (layers > x.target.d3d12.layerNum ||
                 x.arrayOffset > x.target.d3d12.layerNum - layers)))
    return false;
  const uint32_t dispatches = x.mipNum - 1;
  if (dispatches > kMaxMipDispatches || !cmd.d3d12.cmdList ||
      !descriptor_heap[set] || !constant_buffer[set] || !constant_data[set] ||
      !descriptor_size ||
      dispatches * kDescriptorsPerDispatch > descriptor_capacity[set] ||
      constant_offset[set] >
          kConstantBufferSize - dispatches * kConstantBufferAlignment ||
      descriptor_offset[set] >
          descriptor_capacity[set] - dispatches * kDescriptorsPerDispatch)
    return false;
  RID3D12MipScratch s = {};
  s.format = mf.riScratch;
  s.width = x.width;
  s.height = x.height;
  s.depth = is3d ? x.depth : 1;
  s.mipNum = x.mipNum;
  s.layerNum = is3d ? 1 : layers;
  s.is3d = is3d;
  RITextureDesc td = {};
  td.type = is3d ? RI_TEXTURE_3D : RI_TEXTURE_2D;
  td.format = s.format;
  td.width = x.width;
  td.height = x.height;
  td.depth = s.depth;
  td.mipNum = x.mipNum;
  td.layerNum = s.layerNum;
  td.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_SHADER_RESOURCE_STORAGE |
             RI_USAGE_TRANSFER_SRC | RI_USAGE_TRANSFER_DST;
  scratch[set].reserve(scratch[set].size() + 1);
  s.source = RITexture::create(&d, td);
  s.intermediate = RITexture::create(&d, td);
  if (s.source.isEmpty() || s.intermediate.isEmpty()) {
    s.source.dispose(&d);
    s.intermediate.dispose(&d);
    return false;
  }
  scratch[set].push_back(std::move(s));
  RID3D12MipScratch &sc = scratch[set].back();
  ID3D12GraphicsCommandList *l = cmd.d3d12.cmdList;
  // This pass drives its own private heap and root signature on the caller's
  // command list, so it goes through the shadowed setters: leaving the
  // program-level cache claiming the arena heaps are still bound would make a
  // later bindPipeline skip a root-signature set it actually needs.
  RID3D12_SetDescriptorHeaps(cmd, descriptor_heap[set], nullptr);
  RID3D12_SetComputeRootSignature(cmd, root_signature);
  // This pass owns its root signature rather than an RIProgram, so it declares
  // its own root-argument contract: the two tables it sets per dispatch.
  cmd.d3d12.computePipelineBound = true;
  RID3D12_NoteBoundProgramRootArgs(cmd, true, 0x3ull, "RID3D12MipGeneration");
  l->SetPipelineState(is3d ? pipeline_3d : pipeline_2d_array);
  barrier(cmd, const_cast<RITexture &>(x.target), x.currentState,
          RI_RESOURCE_STATE_COPY_SRC, x.currentStages, RI_STAGE_COPY, 0,
          x.arrayOffset, is3d ? 1 : layers);
  barrier(cmd, sc.source, RI_RESOURCE_STATE_UNDEFINED,
          RI_RESOURCE_STATE_COPY_DST, RI_STAGE_NONE, RI_STAGE_COPY, 0, 0,
          sc.layerNum);
  copyMip(l, const_cast<RITexture &>(x.target), sc.source, 0, 0, x.arrayOffset,
          0, layers, x.width, x.height, is3d ? x.depth : 1, is3d);
  barrier(cmd, sc.source, RI_RESOURCE_STATE_COPY_DST,
          RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY, RI_STAGE_COMPUTE, 0,
          0, sc.layerNum);
  RITexture *src = &sc.source, *dst = &sc.intermediate;
  for (uint32_t mip = 1; mip < x.mipNum; ++mip) {
    uint32_t sw = std::max(1u, x.width >> (mip - 1)),
             sh = std::max(1u, x.height >> (mip - 1)),
             sd = std::max(1u, x.depth >> (mip - 1)),
             dw = std::max(1u, x.width >> mip),
             dh = std::max(1u, x.height >> mip),
             dd = std::max(1u, x.depth >> mip);
    barrier(cmd, *dst, RI_RESOURCE_STATE_UNDEFINED,
            RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE,
            mip, 0, is3d ? 1 : layers);
    auto cpu = descriptor_heap[set]->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += descriptor_offset[set] * descriptor_size;
    views(d, sc, mip - 1, mip, is3d ? 1 : layers, cpu, descriptor_size);
    auto gpu = descriptor_heap[set]->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += descriptor_offset[set] * descriptor_size;
    auto uav = gpu;
    uav.ptr += descriptor_size;
    RID3D12_SetComputeRootDescriptorTable(cmd, 0, gpu);
    RID3D12_SetComputeRootDescriptorTable(cmd, 1, uav);
    uint32_t c[8] = {sw,
                     sh,
                     sd,
                     0,
                     dw,
                     dh,
                     is3d ? dd : layers,
                     x.format == RI_FORMAT_RGBA8_SRGB};
    std::memcpy(constant_data[set] + constant_offset[set], c, sizeof(c));
    l->SetComputeRootConstantBufferView(
        2, constant_buffer[set]->GetGPUVirtualAddress() + constant_offset[set]);
    constant_offset[set] += kConstantBufferAlignment;
    descriptor_offset[set] += kDescriptorsPerDispatch;
    cmd.dispatch(&d, (dw + 7) / 8, (dh + 7) / 8, is3d ? dd : layers);
    barrier(cmd, *dst, RI_RESOURCE_STATE_STORAGE_WRITE,
            RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_COMPUTE, RI_STAGE_COPY, mip, 0,
            is3d ? 1 : layers);
    barrier(cmd, const_cast<RITexture &>(x.target), RI_RESOURCE_STATE_UNDEFINED,
            RI_RESOURCE_STATE_COPY_DST, RI_STAGE_NONE, RI_STAGE_COPY, mip,
            x.arrayOffset, is3d ? 1 : layers);
    copyMip(l, *dst, const_cast<RITexture &>(x.target), mip, mip, 0,
            x.arrayOffset, layers, dw, dh, is3d ? dd : 1, is3d);
    if (mip + 1 < x.mipNum)
      barrier(cmd, *dst, RI_RESOURCE_STATE_COPY_SRC,
              RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COPY,
              RI_STAGE_COMPUTE, mip, 0, is3d ? 1 : layers);
    std::swap(src, dst);
  }
  if (x.postState != RI_RESOURCE_STATE_UNDEFINED)
    for (uint32_t mip = 0; mip < x.mipNum; ++mip)
      barrier(cmd, const_cast<RITexture &>(x.target),
              mip ? RI_RESOURCE_STATE_COPY_DST : RI_RESOURCE_STATE_COPY_SRC,
              x.postState, RI_STAGE_COPY, x.postStages, mip, x.arrayOffset,
              is3d ? 1 : layers);
  return true;
}
#endif
