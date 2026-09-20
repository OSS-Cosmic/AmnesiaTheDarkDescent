#ifndef RI_TEXTURE_H
#define RI_TEXTURE_H

#include "graphics/RIDefines.h"
#include "system/Hasher.h"
#include <cstring>
#include <optional>
#include <stdint.h>

#include "graphics/RIPreamble.h"

struct RIDevice;
struct RIRenderer;
#if (DEVICE_IMPL_D3D12)
struct ID3D12Resource;
namespace D3D12MA { class Allocation; }
#endif

enum RITextureType_e { RI_TEXTURE_1D, RI_TEXTURE_2D, RI_TEXTURE_3D };

enum RITextureUsageBits_e {
  RI_USAGE_NONE = 0,
  RI_USAGE_SHADER_RESOURCE = 0x1,
  RI_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_USAGE_COLOR_ATTACHMENT = 0x4,
  RI_USAGE_DEPTH_STENCIL_ATTACHMENT = 0x8,
  RI_USAGE_SHADING_RATE = 0x10,
  RI_USAGE_TRANSFER_SRC = 0x20,
  RI_USAGE_TRANSFER_DST = 0x40,
};

enum RISampleCount_e {
  RI_SAMPLE_COUNT_1 = 1,
  RI_SAMPLE_COUNT_2 = 2,
  RI_SAMPLE_COUNT_4 = 4,
  RI_SAMPLE_COUNT_8 = 8,
  RI_SAMPLE_COUNT_16 = 16,
  RI_SAMPLE_COUNT_COUNT = 5,
};

enum RITextureFlagBits_e {
  RI_TEXTURE_FLAG_NONE = 0,
  // Image may back a cube / cube-array view (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT).
  RI_TEXTURE_FLAG_CUBE_COMPATIBLE = 0x1,
  // Block-compressed image may be viewed with an uncompressed format
  // (VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT).
  RI_TEXTURE_FLAG_BLOCK_TEXEL_VIEW_COMPATIBLE = 0x2,
};

// Backend-neutral image descriptor consumed by RITexture::create.
struct RITextureDesc {
  enum RITextureType_e type; // RI_TEXTURE_2D, ...
  uint32_t format;           // RI_Format_e
  uint32_t width;
  uint32_t height;
  uint32_t depth;            // 0/1 = 2D
  uint32_t mipNum;           // 0 = 1
  uint32_t layerNum;         // 0 = 1
  uint32_t sampleCount;      // RISampleCount_e; 0/1 = no MSAA
  uint32_t usage;            // RITextureUsageBits_e bitmask
  uint32_t flags;            // RITextureFlagBits_e bitmask
};

struct RITexture {
  RITexture() { memset(this, 0, sizeof(*this)); }
  // Backend-neutral image creation (VK: vmaCreateImage, D3D12:
  // RID3D12_CreateTexture). The caller owns the returned texture and disposes
  // it. `cookie` is stamped for use as a bindless/descriptor cache key.
  static struct RITexture create(struct RIDevice *device,
                                 const struct RITextureDesc &desc,
                                 std::optional<hash_t> hash = {});
  bool isEmpty() const;
  void dispose(struct RIDevice *device);
  void setDebugObjectName(struct RIDevice *device, const char *name);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImage image;
      struct VmaAllocation_T *allocation;
    } vk;
#endif
#if (DEVICE_IMPL_D3D12)
    struct {
      // Normally owned and released by dispose. Swapchain textures are borrowed
      // aliases whose single COM reference is owned by RISwapchain::d3d12.images.
      ID3D12Resource *resource;
      D3D12MA::Allocation *allocation; // owned; released after resource
      uint32_t format;           // DXGI_FORMAT captured at creation
      uint32_t width;
      uint32_t height;
      uint16_t depth;
      uint16_t mipNum;
      uint16_t layerNum;
      uint16_t sampleCount;
      uint32_t usage;            // RITextureUsageBits_e snapshot
    } d3d12;
#endif
  };
  // Neutral resource metadata retained for descriptor payloads.
  uint32_t format; // RI_Format_e of the backing image (not a view reinterpretation)
  uint32_t type; // RITextureType_e
  hash_t cookie;
};

#endif // RI_TEXTURE_H
