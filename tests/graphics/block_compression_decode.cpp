#include "graphics/BlockCompressionDecode.h"
#include "utest.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void SetLE16(uint8_t *bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

void SetLE32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

void MakeBC1Block(uint8_t block[8], uint16_t color0, uint16_t color1,
                 uint32_t colorIndices) {
  std::memset(block, 0, 8);
  SetLE16(block, color0);
  SetLE16(block + 2, color1);
  SetLE32(block + 4, colorIndices);
}

void SetAlphaIndices(uint8_t *block, const uint8_t indices[8]) {
  uint64_t packed = 0;
  for (unsigned int index = 0; index < 8; ++index)
    packed |= static_cast<uint64_t>(indices[index]) << (index * 3);
  for (unsigned int byte = 0; byte < 6; ++byte)
    block[2 + byte] = static_cast<uint8_t>((packed >> (byte * 8)) & 0xff);
}

void MakeBC3Block(uint8_t block[16], uint8_t alpha0, uint8_t alpha1,
                 const uint8_t alphaIndices[8], uint16_t color0,
                 uint16_t color1, uint32_t colorIndices) {
  std::memset(block, 0, 16);
  block[0] = alpha0;
  block[1] = alpha1;
  SetAlphaIndices(block, alphaIndices);
  SetLE16(block + 8, color0);
  SetLE16(block + 10, color1);
  SetLE32(block + 12, colorIndices);
}

void CheckPixel(int *utest_result, const std::vector<uint8_t> &pixels,
                uint32_t width,
                uint32_t x, uint32_t y, const uint8_t expected[4],
                const char *name) {
  const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
  EXPECT_TRUE_MSG(std::memcmp(pixels.data() + offset, expected, 4) == 0, name);
}

void CheckFormatSupportAndMapping(int *utest_result) {
  const RI_Format decodable[] = {
      RI_FORMAT_BC1_RGBA_UNORM,
      RI_FORMAT_BC1_RGBA_SRGB,
      RI_FORMAT_BC3_RGBA_UNORM,
      RI_FORMAT_BC3_RGBA_SRGB,
  };
  for (const RI_Format format : decodable) {
    EXPECT_TRUE_MSG(hpl::BC_FormatIsDecodable(format),
                    "BC1 and BC3 formats are decodable");
  }

  const RI_Format unsupported[] = {
      RI_FORMAT_RGBA8_UNORM,
      RI_FORMAT_BC2_RGBA_UNORM,
      RI_FORMAT_BC2_RGBA_SRGB,
      RI_FORMAT_BC4_R_UNORM,
      RI_FORMAT_BC4_R_SNORM,
      RI_FORMAT_BC5_RG_UNORM,
      RI_FORMAT_BC5_RG_SNORM,
      RI_FORMAT_BC6H_RGB_UFLOAT,
      RI_FORMAT_BC6H_RGB_SFLOAT,
      RI_FORMAT_BC7_RGBA_UNORM,
      RI_FORMAT_BC7_RGBA_SRGB,
  };
  for (const RI_Format format : unsupported) {
    EXPECT_TRUE_MSG(!hpl::BC_FormatIsDecodable(format),
                    "non-BC1/BC3 formats are not decodable");
  }

  EXPECT_TRUE_MSG(hpl::BC_DecodedFormat(RI_FORMAT_BC1_RGBA_UNORM) ==
                      RI_FORMAT_RGBA8_UNORM &&
                      hpl::BC_DecodedFormat(RI_FORMAT_BC3_RGBA_UNORM) ==
                          RI_FORMAT_RGBA8_UNORM,
                  "UNORM BC formats map to RGBA8_UNORM");
  EXPECT_TRUE_MSG(hpl::BC_DecodedFormat(RI_FORMAT_BC1_RGBA_SRGB) ==
                      RI_FORMAT_RGBA8_SRGB &&
                      hpl::BC_DecodedFormat(RI_FORMAT_BC3_RGBA_SRGB) ==
                          RI_FORMAT_RGBA8_SRGB,
                  "sRGB BC formats map to RGBA8_SRGB");
}

void CheckBC1FourColourMode(int *utest_result) {
  uint8_t block[8];
  // RGB565 0xffff expands to (255,255,255), and 0x0000 to (0,0,0).
  // Since color0 > color1, BC1 defines entries 2 and 3 as
  // (2*255+0)/3 = 170 and (255+2*0)/3 = 85. The low byte 0xe4 packs
  // indices 0,1,2,3 into the first four pixels: 0 + 1<<2 + 2<<4 + 3<<6.
  MakeBC1Block(block, 0xffff, 0x0000, 0x000000e4);
  std::vector<uint8_t> decoded(4 * 4);
  ASSERT_TRUE_MSG(hpl::BC_DecodeToRGBA8(RI_FORMAT_BC1_RGBA_UNORM, block,
                                        sizeof(block), 4, 1, decoded.data(),
                                        decoded.size()),
                  "BC1 four-colour block decodes");

  const uint8_t expected[][4] = {
      {255, 255, 255, 255},
      {0, 0, 0, 255},
      {170, 170, 170, 255},
      {85, 85, 85, 255},
  };
  for (uint32_t x = 0; x < 4; ++x) {
    CheckPixel(utest_result, decoded, 4, x, 0, expected[x],
               "BC1 four-colour palette entry has RGBA bytes");
  }
}

void CheckBC1PunchThroughMode(int *utest_result) {
  uint8_t block[8];
  // RGB565 black (0x0000) is <= RGB565 white (0xffff), selecting BC1's
  // three-colour mode. Entry 2 is floor((0+255)/2) = 127 with alpha 255;
  // entry 3 is the punch-through transparent black value.
  // 0x0e packs indices 2 and 3 into the first two pixels: 2 + 3<<2.
  MakeBC1Block(block, 0x0000, 0xffff, 0x0000000e);
  std::vector<uint8_t> decoded(2 * 4);
  ASSERT_TRUE_MSG(hpl::BC_DecodeToRGBA8(RI_FORMAT_BC1_RGBA_UNORM, block,
                                        sizeof(block), 2, 1, decoded.data(),
                                        decoded.size()),
                  "BC1 punch-through block decodes");

  const uint8_t midpoint[] = {127, 127, 127, 255};
  const uint8_t transparent[] = {0, 0, 0, 0};
  CheckPixel(utest_result, decoded, 2, 0, 0, midpoint,
             "BC1 three-colour entry 2 is opaque midpoint");
  CheckPixel(utest_result, decoded, 2, 1, 0, transparent,
             "BC1 three-colour entry 3 is transparent");
}

void CheckBC3EightValueAlpha(int *utest_result) {
  uint8_t block[16];
  const uint8_t indices[] = {0, 1, 2, 3, 4, 5, 6, 7};
  // alpha0=255 > alpha1=0 selects the eight-value mode. Its entries are
  // 255, 0, (6*255+0)/7=218, (5*255+0)/7=182,
  // (4*255+0)/7=145, (3*255+0)/7=109, (2*255+0)/7=72,
  // (255+6*0)/7=36 (integer division).
  // The colour endpoints are black <= white, but BC3 always uses four-colour
  // mode: the first row therefore has RGB entries black, white, 85, 170.
  MakeBC3Block(block, 255, 0, indices, 0x0000, 0xffff, 0x000000e4);
  std::vector<uint8_t> decoded(4 * 2 * 4);
  ASSERT_TRUE_MSG(hpl::BC_DecodeToRGBA8(RI_FORMAT_BC3_RGBA_UNORM, block,
                                        sizeof(block), 4, 2, decoded.data(),
                                        decoded.size()),
                  "BC3 eight-value alpha block decodes");

  const uint8_t expected[][4] = {
      {0, 0, 0, 255},   {255, 255, 255, 0}, {85, 85, 85, 218},
      {170, 170, 170, 182}, {0, 0, 0, 145},   {0, 0, 0, 109},
      {0, 0, 0, 72},    {0, 0, 0, 36},
  };
  for (uint32_t pixel = 0; pixel < 8; ++pixel) {
    CheckPixel(utest_result, decoded, 4, pixel % 4, pixel / 4, expected[pixel],
               "BC3 eight-value alpha interpolation is exact");
  }
}

void CheckBC3SixValueAlphaAndForcedColourMode(int *utest_result) {
  uint8_t block[16];
  const uint8_t indices[] = {0, 1, 2, 3, 4, 5, 6, 7};
  // alpha0=0 <= alpha1=255 selects the six-value mode. The entries are
  // 0, 255, (4*0+255)/5=51, (3*0+2*255)/5=102,
  // (2*0+3*255)/5=153, (0+4*255)/5=204, 0, 255.
  // color0=0x0000 <= color1=0xffff would make BC1 entry 3 transparent, but
  // BC3's colour block is always four-colour. Thus its entries are black,
  // white, (2*0+255)/3=85, and (0+2*255)/3=170, all with alpha from BC3.
  MakeBC3Block(block, 0, 255, indices, 0x0000, 0xffff, 0x000000e4);
  std::vector<uint8_t> decoded(4 * 2 * 4);
  ASSERT_TRUE_MSG(hpl::BC_DecodeToRGBA8(RI_FORMAT_BC3_RGBA_UNORM, block,
                                        sizeof(block), 4, 2, decoded.data(),
                                        decoded.size()),
                  "BC3 six-value alpha block decodes");

  const uint8_t expected[][4] = {
      {0, 0, 0, 0},     {255, 255, 255, 255}, {85, 85, 85, 51},
      {170, 170, 170, 102}, {0, 0, 0, 153},     {0, 0, 0, 204},
      {0, 0, 0, 0},     {0, 0, 0, 255},
  };
  for (uint32_t pixel = 0; pixel < 8; ++pixel) {
    CheckPixel(utest_result, decoded, 4, pixel % 4, pixel / 4, expected[pixel],
               "BC3 six-value alpha and forced colour mode are exact");
  }
}

void CheckClippedEdgeBlocks(int *utest_result) {
  uint8_t source[16];
  // A 5x3 surface has two 4x4 BC1 blocks. Block zero is black. Block one is
  // RGB565 red (0xf800 -> 255,0,0), so only its clipped x=4 column should be
  // red; no source texel may wrap into another destination row or column.
  MakeBC1Block(source, 0x0000, 0x0000, 0x00000000);
  MakeBC1Block(source + 8, 0xf800, 0x0000, 0x00000000);
  const size_t decodedSize = hpl::BC_DecodedSizeBytes(5, 3);
  std::vector<uint8_t> decoded(decodedSize + 4, 0xcd);
  ASSERT_EQ_MSG(decodedSize, 5u * 3u * 4u,
                "5x3 BC1 destination size is width times height times RGBA");
  ASSERT_TRUE_MSG(hpl::BC_DecodeToRGBA8(RI_FORMAT_BC1_RGBA_UNORM, source,
                                        sizeof(source), 5, 3, decoded.data(),
                                        decoded.size()),
                  "non-multiple-of-four BC1 surface decodes");

  const uint8_t black[] = {0, 0, 0, 255};
  const uint8_t red[] = {255, 0, 0, 255};
  for (uint32_t y = 0; y < 3; ++y) {
    for (uint32_t x = 0; x < 5; ++x) {
      CheckPixel(utest_result, decoded, 5, x, y, x == 4 ? red : black,
                 "partial BC1 blocks are clipped without wrapping");
    }
  }
  for (size_t index = decodedSize; index < decoded.size(); ++index) {
    EXPECT_EQ_MSG(decoded[index], 0xcd,
                  "BC1 decoder writes only width times height RGBA bytes");
  }
}

void CheckSizingAndRejection(int *utest_result) {
  uint8_t block[8];
  MakeBC1Block(block, 0xffff, 0x0000, 0x00000000);
  const size_t decodedSize = hpl::BC_DecodedSizeBytes(4, 1);
  std::vector<uint8_t> decoded(decodedSize);
  EXPECT_EQ_MSG(hpl::BC_DecodedSizeBytes(5, 3), 5u * 3u * 4u,
                "decoded size is width times height times four");
  EXPECT_TRUE_MSG(!hpl::BC_DecodeToRGBA8(RI_FORMAT_BC1_RGBA_UNORM, block,
                                         sizeof(block) - 1, 4, 1,
                                         decoded.data(), decoded.size()),
                  "one-byte-short BC source is rejected");
  EXPECT_TRUE_MSG(!hpl::BC_DecodeToRGBA8(RI_FORMAT_BC1_RGBA_UNORM, block,
                                         sizeof(block), 4, 1, decoded.data(),
                                         decodedSize - 1),
                  "undersized destination is rejected");
  EXPECT_TRUE_MSG(!hpl::BC_DecodeToRGBA8(RI_FORMAT_RGBA8_UNORM, block,
                                         sizeof(block), 4, 1, decoded.data(),
                                         decoded.size()),
                  "non-decodable format is rejected");
}

} // namespace

UTEST(BlockCompressionDecode, FormatSupportAndMapping) {
  CheckFormatSupportAndMapping(utest_result);
}
UTEST(BlockCompressionDecode, BC1FourColourMode) {
  CheckBC1FourColourMode(utest_result);
}
UTEST(BlockCompressionDecode, BC1PunchThroughMode) {
  CheckBC1PunchThroughMode(utest_result);
}
UTEST(BlockCompressionDecode, BC3EightValueAlpha) {
  CheckBC3EightValueAlpha(utest_result);
}
UTEST(BlockCompressionDecode, BC3SixValueAlphaAndForcedColourMode) {
  CheckBC3SixValueAlphaAndForcedColourMode(utest_result);
}
UTEST(BlockCompressionDecode, ClippedEdgeBlocks) {
  CheckClippedEdgeBlocks(utest_result);
}
UTEST(BlockCompressionDecode, SizingAndRejection) {
  CheckSizingAndRejection(utest_result);
}
