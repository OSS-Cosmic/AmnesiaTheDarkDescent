#ifndef RI_D3D12_MIP_GENERATION_H
#define RI_D3D12_MIP_GENERATION_H

#include "RIPreamble.h"
#include "RITexture.h"

#if (DEVICE_IMPL_D3D12)
#include <d3d12.h>
#include <vector>

struct RIDevice;
struct RICmd;
struct RIGenerateMipsDesc;
namespace D3D12MA {
class Allocation;
}

struct RID3D12MipScratch {
  RITexture source;
  RITexture intermediate;
  uint32_t format;
  uint32_t width, height, depth, mipNum, layerNum;
  bool is3d;
};

struct RID3D12MipGeneration {
  ID3D12RootSignature *root_signature;
  ID3D12PipelineState *pipeline_2d_array;
  ID3D12PipelineState *pipeline_3d;
  ID3D12DescriptorHeap *descriptor_heap[2];
  ID3D12Resource *constant_buffer[2];
  D3D12MA::Allocation *constant_allocation[2];
  uint8_t *constant_data[2];
  uint32_t constant_offset[2];
  uint32_t descriptor_offset[2];
  uint32_t descriptor_capacity[2];
  bool constant_mapped[2];
  uint32_t descriptor_size;
  bool initialized;
  std::vector<RID3D12MipScratch> scratch[2];

  RID3D12MipGeneration();
  bool init(struct RIDevice &device);
  bool isReady() const { return initialized; }
  void dispose(struct RIDevice &device);
  void reclaim(struct RIDevice &device, uint32_t descriptorSet);
  bool generate(struct RIDevice &device, struct RICmd &cmd,
                uint32_t descriptorSet, const struct RIGenerateMipsDesc &desc);
};
bool RID3D12_MipFormatSupported(struct RIDevice &device, uint32_t format,
                                bool is3d);
#endif

#endif
