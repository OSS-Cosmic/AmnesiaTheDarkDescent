#include "graphics/GraphicsTypes.h"
#include "graphics/Graphics.h"
#include "graphics/Texture.h"
#include "graphics/BlockCompressionDecode.h"
#include "graphics/CubeMipGen.h"
#include "graphics/RIFormat.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"

#include "graphics/Bitmap.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/SystemTypes.h"

#include <codecvt>
#include <cassert>
#include <locale>
#include <limits>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace hpl {
cTexture::cTexture(cTexture &&other) noexcept
    : handle(other.handle), format(other.format), view(other.view),
      width(other.width), height(other.height), depth(other.depth),
      mipNum(other.mipNum) {
  // RITextureView keeps a pointer to its owning RITexture so descriptors can
  // retain the allocation. Rebase that self-reference before clearing the
  // moved-from object; otherwise the view points at other.handle after it has
  // been reset (and, after a return by value, potentially destroyed).
  if (view.resource == &other.handle)
    view.resource = &handle;
  // Zero the source so its destructor disposes nothing (dispose is null-safe).
  other.handle = RITexture{};
  other.view = RITextureView{};
}

cTexture &cTexture::operator=(cTexture &&other) noexcept {
  if (this != &other) {
    // Defer release of our own GPU resources past the in-flight frames instead
    // of disposing them here: unlike ~cTexture (whose owning Image is parked in
    // a frame set's resourceLink and waited before free), a move-assign swaps
    // the texture out from under a still-live Image — the old view may still be
    // referenced by an in-flight descriptor set (e.g. the bindless heap when an
    // editor pane texture is resized). graphicsDefer holds it until the GPU
    // timeline passes this frame, then disposes (drainAll covers teardown).
    if (!view.isEmpty() || !handle.isEmpty()) {
      RITextureView oldView = view;
      RITexture oldHandle = handle;
      Interface<cGraphics>::Get()->graphicsDefer.push(std::function<void()>([oldView, oldHandle]() mutable {
        cGraphics* pGraphics = Interface<cGraphics>::Get();
        oldView.dispose(&pGraphics->device);
        oldHandle.dispose(&pGraphics->device);
      }));
    }
    handle = other.handle;
    format = other.format;
    view = other.view;
    if (view.resource == &other.handle)
      view.resource = &handle;
    width = other.width;
    height = other.height;
    depth = other.depth;
    mipNum = other.mipNum;
    other.handle = RITexture{};
    other.view = RITextureView{};
  }
  return *this;
}

cTexture::~cTexture() {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  // Free the GPU resources directly — no freelist deferral needed. The owning
  // Image is parked (via a SharedResourcePin) in that frame set's resourceLink,
  // and BeginActiveSet waits the ring fence before clearing it, so by the time
  // the Image (and thus this texture) is freed the GPU is done with it.
  view.dispose(&pGraphics->device);   // owned image view (was binding's inline view)
  handle.dispose(&pGraphics->device);  // image + VMA allocation
}

RIDescriptor cTexture::descriptor() {
  return RIDescriptor::sampledImage(&Interface<cGraphics>::Get()->device, &view);
}

RI_Format from_hpl_format(ePixelFormat format) {
  switch (format) {
  case ePixelFormat_Alpha:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_Luminance:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_LuminanceAlpha:
    return RI_FORMAT_RG8_UNORM;
  case ePixelFormat_RGB: 
    return RI_FORMAT_RGB8_UNORM;
  case ePixelFormat_RGBA:
    return RI_FORMAT_RGBA8_UNORM;
  case ePixelFormat_BGRA:
    return RI_FORMAT_BGRA8_UNORM;
  case ePixelFormat_DXT1:
    return RI_FORMAT_BC1_RGBA_UNORM;
  case ePixelFormat_DXT2:
  case ePixelFormat_DXT3:
    return RI_FORMAT_BC2_RGBA_UNORM;
  case ePixelFormat_DXT4:
  case ePixelFormat_DXT5:
    return RI_FORMAT_BC3_RGBA_UNORM;
  case ePixelFormat_Depth16:
    return RI_FORMAT_D16_UNORM;
  case ePixelFormat_Depth24:
    return RI_FORMAT_D32_SFLOAT_S8_UINT;
  case ePixelFormat_Depth32:
    return RI_FORMAT_D32_SFLOAT;
  case ePixelFormat_Alpha16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_Luminance16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_LuminanceAlpha16:
    return RI_FORMAT_RG16_UNORM;
  case ePixelFormat_RGBA16:
    return RI_FORMAT_RGBA16_UNORM;
  case ePixelFormat_RGB16:
    return RI_FORMAT_RGB16_UNORM;
  case ePixelFormat_Alpha32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_Luminance32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_LuminanceAlpha32:
    return RI_FORMAT_RG32_SFLOAT;
  case ePixelFormat_RGBA32:
    return RI_FORMAT_RGBA32_SFLOAT;
  case ePixelFormat_BGR:
    return RI_FORMAT_BGR8_UNORM;
  case ePixelFormat_RGB32:
    return RI_FORMAT_RGB32_SFLOAT;
  default:
    assert(false && "Unsupported texture format");
    break;
  }
  return RI_FORMAT_UNKNOWN;
}


// Upgrade a UNORM color format to its sRGB variant so the sampler decodes
// sRGB→linear on read. Formats with no sRGB sibling (single-channel, 16-bit,
// floats, depth) pass through unchanged.
static RI_Format to_srgb_format(RI_Format f) {
  switch (f) {
  case RI_FORMAT_RGBA8_UNORM: return RI_FORMAT_RGBA8_SRGB;
  case RI_FORMAT_BGRA8_UNORM: return RI_FORMAT_BGRA8_SRGB;
  case RI_FORMAT_BC1_RGBA_UNORM: return RI_FORMAT_BC1_RGBA_SRGB;
  case RI_FORMAT_BC2_RGBA_UNORM: return RI_FORMAT_BC2_RGBA_SRGB;
  case RI_FORMAT_BC3_RGBA_UNORM: return RI_FORMAT_BC3_RGBA_SRGB;
  case RI_FORMAT_BC7_RGBA_UNORM: return RI_FORMAT_BC7_RGBA_SRGB;
  default: return f;
  }
}

RI_Format to_image_supported_format(ePixelFormat format) {
  switch (format) {
  case ePixelFormat_Alpha:
  case ePixelFormat_Luminance:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_LuminanceAlpha:
    return RI_FORMAT_RG8_UNORM;
  case ePixelFormat_RGB: // generally not supported most hardware does not
                         // support 24 bit formats
  case ePixelFormat_RGBA:
    return RI_FORMAT_RGBA8_UNORM;
  case ePixelFormat_BGRA:
    return RI_FORMAT_BGRA8_UNORM;
  case ePixelFormat_DXT1:
    return RI_FORMAT_BC1_RGBA_UNORM;
  case ePixelFormat_DXT2:
  case ePixelFormat_DXT3:
    return RI_FORMAT_BC2_RGBA_UNORM;
  case ePixelFormat_DXT4:
  case ePixelFormat_DXT5:
    return RI_FORMAT_BC3_RGBA_UNORM;
  case ePixelFormat_Depth16:
    return RI_FORMAT_D16_UNORM;
  case ePixelFormat_Depth24:
    return RI_FORMAT_D32_SFLOAT_S8_UINT;
  case ePixelFormat_Depth32:
    return RI_FORMAT_D32_SFLOAT;
  case ePixelFormat_Alpha16:
  case ePixelFormat_Luminance16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_LuminanceAlpha16:
    return RI_FORMAT_RG16_UNORM;
  case ePixelFormat_RGBA16:
  case ePixelFormat_RGB16:
    return RI_FORMAT_RGBA16_UNORM;
  case ePixelFormat_Alpha32:
  case ePixelFormat_Luminance32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_LuminanceAlpha32:
    return RI_FORMAT_RG32_SFLOAT;
  case ePixelFormat_RGBA32:
    return RI_FORMAT_RGBA32_SFLOAT;
  case ePixelFormat_BGR:
    return RI_FORMAT_BGRA8_UNORM;
  default:
    assert(false && "Unsupported texture format");
    break;
  }
  return RI_FORMAT_UNKNOWN;
}

static inline bool GetSurfaceInfo(
	uint32_t width,
	uint32_t height,
	const RIFormatProps* prop,
	uint32_t* outNumBytes,
	uint32_t* outRowBytes,
	uint32_t* outNumRows)
{

	uint64_t numBytes = 0;
	uint64_t rowBytes = 0;
	uint64_t numRows = 0;
  
	uint32_t bpp = prop->stride * 8;
	bool compressed = prop->isCompressed;
	if (compressed)
	{
		uint32_t blockWidth = prop->blockWidth;
		uint32_t blockHeight = prop->blockHeight;
		uint32_t numBlocksWide = 0;
		uint32_t numBlocksHigh = 0;
		if (width > 0)
		{
			numBlocksWide = std::max(1U, (width + (blockWidth - 1)) / blockWidth);
		}
		if (height > 0)
		{
			numBlocksHigh = std::max(1u, (height + (blockHeight - 1)) / blockHeight);
		}

		rowBytes = numBlocksWide * (bpp >> 3);
		numRows = numBlocksHigh;
		numBytes = rowBytes * numBlocksHigh;
	}
	else
	{
		if (!bpp)
			return false;

		rowBytes = (uint64_t(width) * bpp + 7u) / 8u; // round up to nearest byte
		numRows = uint64_t(height);
		numBytes = rowBytes * height;
	}

	if (numBytes > UINT32_MAX || rowBytes > UINT32_MAX || numRows > UINT32_MAX) //-V560
		return false;

	if (outNumBytes)
	{
		*outNumBytes = (uint32_t)numBytes;
	}
	if (outRowBytes)
	{
		*outRowBytes = (uint32_t)rowBytes;
	}
	if (outNumRows)
	{
		*outNumRows = (uint32_t)numRows;
	}

	return true;
}

void cTexture::setDebugName(const tWString& name) {
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  std::string utf8 = converter.to_bytes(name);
  setDebugName(utf8.c_str());
}

void cTexture::setDebugName(const char* name) {
	assert(!handle.isEmpty());
	handle.setDebugObjectName(&Interface<cGraphics>::Get()->device, name);
}

// Stage-copies one (arrIndex, mipLevel) subresource worth of `srcData` into
// `target` through the resource uploader, converting from the bitmap's pixel
// layout (sourceFormat) to destFormat. current/post states parameterize the
// uploader's barriers: UNDEFINED for the first fill of a fresh image,
// SHADER_RESOURCE for an in-place re-upload of a fragment-sampled texture.
static void __UploadBitmapSubresource(struct RITexture &target,
                                      const unsigned char *srcData,
                                      RI_Format sourceFormat, RI_Format destFormat,
                                      uint32_t arrIndex, uint32_t mipLevel,
                                      uint32_t w, uint32_t h, uint32_t depth,
                                      enum RIResourceState_e currentState,
                                      uint32_t currentStages,
                                      enum RIResourceState_e postState,
                                      uint32_t postStages) {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  const struct RIFormatProps *srcProps = GetRIFormatProps(sourceFormat);
  const struct RIFormatProps *destProps = GetRIFormatProps(destFormat);

  // Compressed axes use ceil division: 5..7 pixels need two 4x4 blocks.
  // Sub-block mips still occupy one full block; without the min-one clamp,
  // rowPitch == 0 triggers a divide-by-zero in RI_ResourceEndCopyTexture.
  const uint32_t srcSliceNum = RIFormatBlockCount(h, srcProps->blockHeight);
  const uint32_t srcRowPitch = RIFormatBlockCount(w, srcProps->blockWidth) * srcProps->stride;

  struct RIResourceTextureTransaction uploadDesc = {};
  uploadDesc.target = target;
  uploadDesc.width = w;
  uploadDesc.height = h;
  uploadDesc.sliceNum = RIFormatBlockCount(h, destProps->blockHeight);
  uploadDesc.rowPitch = RIFormatBlockCount(w, destProps->blockWidth) * destProps->stride;
  uploadDesc.arrayOffset = arrIndex;
  uploadDesc.mipOffset = mipLevel;
  uploadDesc.x = 0;
  uploadDesc.y = 0;
  uploadDesc.depth = depth;
  uploadDesc.format = destFormat;
  uploadDesc.currentState = currentState;
  uploadDesc.currentStages = currentStages;
  uploadDesc.postState = postState;
  uploadDesc.postStages = postStages;
  RI_ResourceBeginCopyTexture(&pGraphics->device, &pGraphics->uploader, &uploadDesc);

  // Iterate rows-of-blocks (== rows when blockHeight == 1).
  // alignSlicePitch is already bytes-per-slice, so the z term must not
  // be multiplied by alignRowPitch a second time.
  const size_t rowCount = uploadDesc.sliceNum;
  if (destProps->isCompressed) {
    assert(srcRowPitch == uploadDesc.rowPitch); // compressed formats must match
    assert(srcSliceNum == uploadDesc.sliceNum);
    for (size_t z = 0; z < depth; ++z) {
      for (size_t slice = 0; slice < rowCount; ++slice) {
        const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
        const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
        memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart],
               &srcData[srcRowStart], uploadDesc.rowPitch);
      }
    }
  } else {
    for (size_t z = 0; z < depth; ++z) {
      for (size_t slice = 0; slice < rowCount; ++slice) {
        const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
        const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
        memset(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart], 255, uploadDesc.rowPitch);
        for (size_t column = 0; column < uploadDesc.width; ++column) {
          memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart + (destProps->stride * column)],
                 &srcData[srcRowStart + (srcProps->stride * column)],
                 std::min(srcProps->stride, destProps->stride));
        }
      }
    }
  }
  RI_ResourceEndCopyTexture(&pGraphics->device, &pGraphics->uploader, &uploadDesc);
}

bool cTexture::LoadBitmap(
                          enum RIResourceState_e postState, uint32_t postStages,
                            cBitmap &bitmap,
                            const BitmapLoadOptions &options) {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  width = static_cast<uint16_t>(bitmap.GetWidth());
  height = static_cast<uint16_t>(bitmap.GetHeight());
  depth = static_cast<uint16_t>(bitmap.GetDepth());
  const RI_Format bitmapSourceFormat = from_hpl_format(bitmap.GetPixelFormat());
  RI_Format sourceFormat = bitmapSourceFormat;
  if (options.sRGB) {
    sourceFormat = to_srgb_format(sourceFormat);
  }
  RI_Format destFormat = hpl::to_image_supported_format(bitmap.GetPixelFormat());
  if (options.sRGB) {
    destFormat = to_srgb_format(destFormat);
  }
  const uint32_t sourceMipLevels =
      options.use_mipmaps ? bitmap.GetNumOfMipMaps() : 1;
  uint32_t arrayLayers = options.use_array ? bitmap.GetNumOfImages() : 1;
  if (options.use_cubemap) {
    if (options.use_array) {
      assert(bitmap.GetNumOfImages() % 6 == 0 &&
             "Cube map array must have a multiple of 6 images");
      arrayLayers = bitmap.GetNumOfImages();
    } else {
      assert(bitmap.GetNumOfImages() == 6 && "Cube map must have 6 images");
      arrayLayers = 6;
    }
  }
  // Generate missing levels with a GPU blit for ordinary textures, or with the
  // seam-aware CPU generator for eligible cubes. For sRGB textures, both paths
  // decode to linear, filter, and re-encode, so mips are not darkened; per-layer
  // blits cannot filter across cube face seams, so cubes use the CPU path.
  // Block-compressed formats cannot be a blit destination, so
  // RI_FormatSupportsMipGeneration rejects them; the mipless BC case is
  // handled by the decode-to-RGBA8 fallback below.
  const uint32_t maxDimension =
      std::max<uint32_t>(bitmap.GetWidth(), bitmap.GetHeight());
  const bool mipGenerationRequested =
      options.use_mipmaps && options.generate_mipmaps &&
      sourceMipLevels == 1 && depth == 1 && maxDimension > 1;

  CubeMipTexelLayout cubeMipLayout = {};
  bool cubeMipLayoutSupported = false;
  const RIFormatProps *bitmapSourceProps =
      GetRIFormatProps(bitmapSourceFormat);
  const uint32_t channelCount = RIFormatChannelCount(bitmapSourceFormat);
  const bool plain8Unorm =
      bitmapSourceProps->blockWidth == 1 &&
      bitmapSourceProps->isNorm && !bitmapSourceProps->isFloat &&
      !bitmapSourceProps->isInteger && !bitmapSourceProps->isPacked &&
      !bitmapSourceProps->isDepth && !bitmapSourceProps->isStencil &&
      !bitmapSourceProps->isSigned && !bitmapSourceProps->isSrgb &&
      !bitmapSourceProps->isCompressed &&
      channelCount >= 1 && channelCount <= 4 &&
      bitmapSourceProps->stride == channelCount &&
      ((channelCount == 1 && bitmapSourceProps->redBits == 8) ||
       (channelCount == 2 && bitmapSourceProps->redBits == 8 &&
        bitmapSourceProps->greenBits == 8) ||
       (channelCount == 3 && bitmapSourceProps->redBits == 8 &&
        bitmapSourceProps->greenBits == 8 &&
        bitmapSourceProps->blueBits == 8) ||
       (channelCount == 4 && bitmapSourceProps->redBits == 8 &&
        bitmapSourceProps->greenBits == 8 &&
        bitmapSourceProps->blueBits == 8 &&
        bitmapSourceProps->alphaBits == 8));
  const bool plain32Float =
      bitmapSourceProps->blockWidth == 1 && bitmapSourceProps->isFloat &&
      !bitmapSourceProps->isPacked && !bitmapSourceProps->isDepth &&
      !bitmapSourceProps->isStencil && channelCount >= 1 && channelCount <= 4 &&
      bitmapSourceProps->stride == channelCount * 4 &&
      ((channelCount == 1 && bitmapSourceProps->redBits == 32) ||
       (channelCount == 2 && bitmapSourceProps->redBits == 32 &&
        bitmapSourceProps->greenBits == 32) ||
       (channelCount == 3 && bitmapSourceProps->redBits == 32 &&
        bitmapSourceProps->greenBits == 32 &&
        bitmapSourceProps->blueBits == 32) ||
       (channelCount == 4 && bitmapSourceProps->redBits == 32 &&
        bitmapSourceProps->greenBits == 32 &&
        bitmapSourceProps->blueBits == 32 &&
        bitmapSourceProps->alphaBits == 32));
  if (plain8Unorm || plain32Float) {
    cubeMipLayout.channelCount = channelCount;
    cubeMipLayout.bytesPerChannel = plain8Unorm ? 1 : 4;
    cubeMipLayout.sRGB = options.sRGB;
    cubeMipLayoutSupported = CubeMipGenSupportsLayout(cubeMipLayout);
  }
  const bool generateCubeMips =
      options.use_cubemap && options.use_mipmaps &&
      options.generate_mipmaps && sourceMipLevels == 1 && depth == 1 &&
      bitmap.GetWidth() == bitmap.GetHeight() && bitmap.GetWidth() > 1 &&
      (bitmap.GetWidth() & (bitmap.GetWidth() - 1)) == 0 &&
      arrayLayers % 6 == 0 && cubeMipLayoutSupported;
  // A cube that asked for mips but failed the predicate above is uploaded with
  // a single level, which is invisible in-game until a gobo or reflection
  // aliases. Name the first condition that rejected it. The abUseMipMaps=false
  // opt-out never reaches here: mipGenerationRequested already excludes it.
  if (options.use_cubemap && mipGenerationRequested && !generateCubeMips) {
    const char *cubeMipFallbackReason = nullptr;
    if (bitmapSourceProps->isCompressed || bitmap.IsCompressed()) {
      cubeMipFallbackReason =
          "the source is block-compressed; the seam-aware CPU generator needs "
          "uncompressed texels";
    } else if (!plain8Unorm && !plain32Float) {
      cubeMipFallbackReason =
          "the layout is neither 8-bit unorm nor 32-bit float";
    } else if (!cubeMipLayoutSupported) {
      cubeMipFallbackReason =
          "the layout is unsupported by the seam-aware CPU generator";
    } else if (bitmap.GetWidth() != bitmap.GetHeight()) {
      cubeMipFallbackReason = "the faces are not square";
    } else if ((bitmap.GetWidth() & (bitmap.GetWidth() - 1)) != 0) {
      cubeMipFallbackReason = "the face size is not a power of two";
    } else {
      cubeMipFallbackReason =
          "the cube face count is not a multiple of six";
    }
    Warning("cTexture::LoadBitmap: texture '%s' cannot generate cube mips "
            "because %s; the texture will be sampled at a single mip level. "
            "Re-author the cube map with an authored mip chain.\n",
            cString::To8Char(bitmap.GetFileName()).c_str(),
            cubeMipFallbackReason);
  }

  // DevIL leaves BC data in the bitmap as raw blocks. A block-compressed image
  // cannot be the destination of vkCmdBlitImage, so decode the one source
  // level before the image is created. Authored BC mip chains never enter this
  // path and remain compressed. Cubes are excluded: they take the CPU
  // generator, which reads the source bytes and so cannot consume a decode
  // that happens per-subresource further down — decoding them here would cost
  // the VRAM without producing a mip chain.
  // The shipped-content census currently finds no block-compressed cube map
  // without a mip chain, so this gate serves an empty set today; see
  // notes/cube_map_inventory.md.
  const RI_Format decodedFormat = BC_DecodedFormat(sourceFormat);
  const bool decompressForMipGeneration =
      bitmap.IsCompressed() && mipGenerationRequested &&
      !options.use_cubemap && BC_FormatIsDecodable(sourceFormat) &&
      RI_FormatSupportsMipGeneration(&pGraphics->device, decodedFormat);
  if (decompressForMipGeneration) {
    Warning("cTexture::LoadBitmap: texture '%s' ships block-compressed without "
            "mipmaps; decompressing to RGBA8 at load (costs VRAM). Re-author "
            "it with a mip chain.\n",
            cString::To8Char(bitmap.GetFileName()).c_str());
    destFormat = decodedFormat;
  }
  format = destFormat; // remembered so material setup can probe channel count
  const struct RIFormatProps *formatProps = GetRIFormatProps(destFormat);
  uint32_t texFlags = RI_TEXTURE_FLAG_NONE;
  if (formatProps->blockWidth > 1) {
    // block-compressed format can be viewed with an uncompressed format (1
    // texel covers 1 block)
    texFlags |= RI_TEXTURE_FLAG_BLOCK_TEXEL_VIEW_COMPATIBLE;
  }
  if (options.use_cubemap) {
    texFlags |= RI_TEXTURE_FLAG_CUBE_COMPATIBLE; // allow cube maps
  }
  const bool generateMipmaps =
      !options.use_cubemap && !generateCubeMips && mipGenerationRequested &&
      RI_FormatSupportsMipGeneration(&pGraphics->device, destFormat);
  uint32_t mipLevels = sourceMipLevels;
  if (generateMipmaps || generateCubeMips) {
    // Use integer division by two so the mip count is floor(log2(maxDim)) + 1.
    mipLevels = 1;
    for (uint32_t dimension = maxDimension; dimension > 1; dimension >>= 1) {
      ++mipLevels;
    }
  }
  // 1xN / Nx1 sources stay RI_TEXTURE_2D (a 1-tall or 1-wide 2D image is
  // valid). Every texture created here is bound into the `textures_2d[]`
  // bindless array, whose shader OpTypeImage is 2D; a 1D view in that slot
  // fails the descriptor view-type match at submit
  // (VUID-vkCmdDraw-viewType-07752).
  const enum RITextureType_e texType =
      depth > 1 ? RI_TEXTURE_3D : RI_TEXTURE_2D;

  RITextureDesc desc = {};
  desc.type = texType;
  desc.format = destFormat;
  desc.width = bitmap.GetWidth();
  desc.height = bitmap.GetHeight();
  desc.depth = bitmap.GetDepth();
  desc.mipNum = mipLevels;
  desc.layerNum = arrayLayers;
  desc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC |
               RI_USAGE_TRANSFER_DST;
  desc.flags = texFlags;
  handle = RITexture::create(&pGraphics->device, desc);
  if (handle.isEmpty()) {
    // Leave no metadata describing a texture that does not exist.
    width = height = depth = 0;
    mipNum = 0;
    return false;
  }
  // cTexture stores the final mip count in uint8_t; bitmap dimensions keep it
  // within range, and the cast makes the narrowing intentional.
  mipNum = static_cast<uint8_t>(mipLevels);

	RITextureViewDesc viewDesc = {};
	// View type must match texType and the bindless array's 2D OpTypeImage.
	// texType is only ever 3D (depth>1) or 2D here, so 1xN/Nx1 textures get a
	// 2D view that the textures_2d[] array accepts.
	if (options.use_cubemap) {
		viewDesc.viewType = options.use_array ? RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY : RI_VIEWTYPE_SHADER_RESOURCE_CUBE;
	} else if (texType == RI_TEXTURE_3D) {
		viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_3D;
	} else {
		viewDesc.viewType = options.use_array ? RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY : RI_VIEWTYPE_SHADER_RESOURCE_2D;
	}
	viewDesc.format = destFormat;
	viewDesc.mipNum = std::max<uint32_t>(mipLevels, 1);
	viewDesc.layerNum = arrayLayers;
	view = RITextureView::create( &pGraphics->device, &handle, viewDesc );
	
	setDebugName(bitmap.GetFileName());
#define MIP_REDUCE(s, mip) (std::max<uint32_t>(1u, (uint32_t)((s) >> (mip))))
  for (uint32_t arrIndex = 0; arrIndex < arrayLayers; arrIndex++) {
    for (uint32_t mipLevel = 0; mipLevel < sourceMipLevels; mipLevel++) {
      const uint32_t w = MIP_REDUCE(desc.width, mipLevel);
      const uint32_t h = MIP_REDUCE(desc.height, mipLevel);

      const auto& input = bitmap.GetData(arrIndex, mipLevel);
      if (input == NULL || input->mpData == NULL) {
        Warning("cTexture::LoadBitmap: missing source data for array %u mip %u\n",
                arrIndex, mipLevel);
        continue;
      }

      const unsigned char* uploadData = input->mpData;
      RI_Format uploadSourceFormat = sourceFormat;
      std::vector<unsigned char> decodedData;
      if (decompressForMipGeneration) {
        const RIFormatProps* sourceProps = GetRIFormatProps(sourceFormat);
        const size_t decodedSliceSize = BC_DecodedSizeBytes(w, h);
        const size_t blocksWide = (static_cast<size_t>(w) + 3) / 4;
        const size_t blocksHigh = (static_cast<size_t>(h) + 3) / 4;
        const size_t compressedSliceSize =
            blocksWide * blocksHigh * sourceProps->stride;
        const size_t depthSlices = std::max<uint32_t>(1u, desc.depth);
        const size_t decodedSize = decodedSliceSize * depthSlices;
        const size_t sourceSize = compressedSliceSize * depthSlices;
        if (decodedSliceSize == std::numeric_limits<size_t>::max() ||
            decodedSize / depthSlices != decodedSliceSize ||
            sourceSize / depthSlices != compressedSliceSize ||
            input->mlSize < 0 || static_cast<size_t>(input->mlSize) < sourceSize) {
          Warning("cTexture::LoadBitmap: failed to decode BC source for texture "
                  "'%s' (array %u mip %u)\n",
                  cString::To8Char(bitmap.GetFileName()).c_str(), arrIndex,
                  mipLevel);
          continue;
        }
        decodedData.resize(decodedSize);
        bool decodeSucceeded = true;
        for (size_t depthSlice = 0; depthSlice < depthSlices; ++depthSlice) {
          if (!BC_DecodeToRGBA8(
                  sourceFormat, input->mpData + depthSlice * compressedSliceSize,
                  compressedSliceSize, w, h,
                  decodedData.data() + depthSlice * decodedSliceSize,
                  decodedSliceSize)) {
            decodeSucceeded = false;
            break;
          }
        }
        if (!decodeSucceeded) {
          Warning("cTexture::LoadBitmap: failed to decode BC source for texture "
                  "'%s' (array %u mip %u)\n",
                  cString::To8Char(bitmap.GetFileName()).c_str(), arrIndex,
                  mipLevel);
          continue;
        }
        uploadData = decodedData.data();
        // The decoded bytes are ordinary RGBA8 texels. The destination format
        // carries sRGB semantics; the source staging layout must remain UNORM.
        uploadSourceFormat = RI_FORMAT_RGBA8_UNORM;
      }

      __UploadBitmapSubresource(handle, uploadData, uploadSourceFormat,
                                destFormat, arrIndex, mipLevel, w, h, desc.depth,
                                RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
                                generateMipmaps ? RI_RESOURCE_STATE_COPY_DST
                                                : postState,
                                generateMipmaps ? RI_STAGE_COPY : postStages);
    }
  }
  if (generateCubeMips) {
    for (uint32_t baseLayer = 0; baseLayer < arrayLayers; baseLayer += 6) {
      const uint8_t *faces[6] = {};
      for (uint32_t face = 0; face < 6; ++face) {
        const auto &input = bitmap.GetData(baseLayer + face, 0);
        faces[face] = input == NULL ? nullptr : input->mpData;
      }

      std::vector<std::vector<uint8_t>> outLevels;
      if (!GenerateSeamAwareCubeMips(
              faces, bitmap.GetWidth(), cubeMipLayout, mipLevels, outLevels)) {
        Warning("cTexture::LoadBitmap: failed to generate seam-aware cube "
                "mips for texture '%s' (base layer %u)\n",
                cString::To8Char(bitmap.GetFileName()).c_str(), baseLayer);
        continue;
      }

      for (uint32_t mipLevel = 1; mipLevel < mipLevels; ++mipLevel) {
        const uint32_t w = MIP_REDUCE(desc.width, mipLevel);
        const uint32_t h = MIP_REDUCE(desc.height, mipLevel);
        for (uint32_t face = 0; face < 6; ++face) {
          __UploadBitmapSubresource(
              handle, outLevels[(mipLevel - 1) * 6 + face].data(),
              sourceFormat, destFormat, baseLayer + face, mipLevel, w, h,
              1, RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE, postState,
              postStages);
        }
      }
    }
  }
  if (generateMipmaps) {
    RIGenerateMipsDesc generateDesc = {};
    generateDesc.target = handle;
    generateDesc.format = destFormat;
    generateDesc.width = desc.width;
    generateDesc.height = desc.height;
    generateDesc.depth = desc.depth;
    generateDesc.mipNum = mipLevels;
    generateDesc.arrayOffset = 0;
    generateDesc.layerNum = arrayLayers;
    generateDesc.currentState = RI_RESOURCE_STATE_COPY_DST;
    generateDesc.currentStages = RI_STAGE_COPY;
    generateDesc.postState = postState;
    generateDesc.postStages = postStages;
    RI_ResourceGenerateMips(&pGraphics->device, &pGraphics->uploader,
                            &generateDesc);
  }
  return true;
}

bool cTexture::UpdateBitmap(cBitmap &bitmap) {
  // Size and format must match the original LoadBitmap — this only refreshes
  // the pixels of mip 0 / layer 0 (the SetRawData replacement for procedural
  // textures like the color picker's box/slider).
  assert(!handle.isEmpty());
  assert(bitmap.GetWidth() == width && bitmap.GetHeight() == height &&
         bitmap.GetDepth() == depth);
  if (to_image_supported_format(bitmap.GetPixelFormat()) != format) {
    Warning("cTexture::UpdateBitmap: bitmap format does not match the loaded "
            "texture format; update rejected\n");
    return false;
  }

  const auto &input = bitmap.GetData(0, 0);
  if (input == NULL || input->mpData == NULL) {
    Warning("cTexture::UpdateBitmap: missing source data\n");
    return false;
  }

  RI_Format sourceFormat = from_hpl_format(bitmap.GetPixelFormat());
  __UploadBitmapSubresource(handle, input->mpData, sourceFormat, (RI_Format)format,
                            0, 0, width, height, depth,
                            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
  return true;
}

//cTexture::cTexture() {
//
//}
//
//cTexture::cTexture(struct RIResourceUploader* upload,cBitmap& bitmap, const BitmapLoadOptions& options) {
//	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
//	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; // typeless
//	const struct RIFormatProps *formatProps = GetRIFormatProps( destFormat );
//	//bitmap.GetPixelFormat()
//	////if( formatProps->blockWidth > 1 )
//	////	info.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT; // format can be used to create a view with an uncompressed format (1 texel covers 1 block)
//	////if( image->flags & IT_CUBEMAP )
//	////	info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; // allow cube maps
//	//info.imageType = VK_IMAGE_TYPE_2D;
//	//info.format = RIFormatToVK( destFormat );
//	//info.extent.width = bitmap.GetWidth();
//	//info.extent.height = bitmap.GetHeight();
//	//info.extent.depth = 1;
//	//info.mipLevels = image->mipNum;
//	//info.arrayLayers = ( image->flags & IT_CUBEMAP ) ? 6 : 1;
//	//info.samples = 1;
//	//info.tiling = VK_IMAGE_TILING_OPTIMAL;
//	//info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
//
//	//info.pQueueFamilyIndices = queueFamilies;
//	//VK_ConfigureImageQueueFamilies( &info, rsh.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
//	//info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//	//
//	//VmaAllocationCreateInfo memCreateInfo = { 0 };
//	//memCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
//	//if(!VK_WrapResult(vmaCreateImage(rsh.device.vk.vmaAllocator, &info, &memCreateInfo, &image->handle.vk.image, &image->vk.vmaAlloc, NULL))) {
//	//	ri.Com_Printf( S_COLOR_YELLOW "Failed to Create Image: %s\n", image->name.buf );
//	//	__FreeImage( image );
//	//	image = NULL;
//	//	return NULL;
//	//}
//
//} 
//

} // namespace hpl
