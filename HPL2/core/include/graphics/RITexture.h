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
  // The texture is sampled and storage-written without a layout transition
  // between the two -- Vulkan's VK_IMAGE_LAYOUT_GENERAL idiom. D3D12 has no
  // layout admitting both accesses (UNORDERED_ACCESS takes only UAV, COMMON and
  // GENERIC_READ only SRV/copy), so it needs
  // D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS: an immutable layout that
  // permits any number of reads plus one concurrent write. Vulkan ignores this
  // bit -- GENERAL already covers it.
  //
  // Set it only where a combined read+write state is deliberate. Such textures
  // lose compression and the D3D12 spec notes they are "generally much slower
  // than standard texture accesses", so it is not a blanket substitute for
  // transitioning a texture between sampled and storage use.
  RI_USAGE_SIMULTANEOUS_ACCESS = 0x80,
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

// The value a render-target / depth-stencil image is expected to be cleared
// with. D3D12 bakes it into the resource (pOptimizedClearValue) so the clear
// takes the fast path; a clear to any other value falls back to the slow path
// and the debug layer reports CLEAR*VIEW_MISMATCHINGCLEARVALUE. Vulkan has no
// equivalent and ignores it.
struct RITextureClearValue {
  float color[4];
  float depth;
  uint32_t stencil;
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
  // Attachment images only; ignored without COLOR_ATTACHMENT /
  // DEPTH_STENCIL_ATTACHMENT usage. Unset means the engine default: depth 1.0 /
  // stencil 0 for depth-stencil, {0,0,0,0} for color, which is what nearly
  // every pass clears to.
  std::optional<RITextureClearValue> clearValue;
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
      // D3D12MA sub-allocates placed resources, and a placed resource created
      // with ALLOW_RENDER_TARGET / ALLOW_DEPTH_STENCIL must be initialized by a
      // Discard / Clear / Copy before anything reads it (debug layer id 1422).
      // A compute UAV write or a LOAD_OP_LOAD attachment bind does not count.
      // Set at creation and cleared by the whole-resource DiscardResource that
      // RID3D12_ResourceBarrier issues on the texture's first barrier.
      uint8_t needsInitialization;
    } d3d12;
#endif
  };
  // Neutral resource metadata retained for descriptor payloads.
  uint32_t format; // RI_Format_e of the backing image (not a view reinterpretation)
  uint32_t type; // RITextureType_e
  hash_t cookie;
};

#endif // RI_TEXTURE_H
