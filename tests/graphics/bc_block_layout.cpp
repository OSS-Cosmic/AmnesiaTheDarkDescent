#include "graphics/RIFormat.h"
#include "utest.h"

#include <cstdint>

namespace {
struct LayoutCase { RI_Format format; uint32_t width, height; const char *name; };

void CheckFormatProps(int *utest_result, RI_Format format, uint32_t blockWidth,
                      uint32_t blockHeight, uint32_t bytes, const char *name) {
  const RIFormatProps *props = GetRIFormatProps(format);
  ASSERT_NE_MSG(props, nullptr, name);
  ASSERT_EQ_MSG(props->blockWidth, blockWidth, name);
  ASSERT_EQ_MSG(props->blockHeight, blockHeight, name);
  ASSERT_EQ_MSG(props->stride, bytes, name);
}

void CheckLayout(int *utest_result, const LayoutCase &testCase) {
  const RIFormatProps *props = GetRIFormatProps(testCase.format);
  ASSERT_NE_MSG(props, nullptr, testCase.name);
  ASSERT_GT_MSG(props->blockWidth, 0u, testCase.name);
  ASSERT_GT_MSG(props->blockHeight, 0u, testCase.name);
  ASSERT_GT_MSG(props->stride, 0u, testCase.name);
  // Independent ceil-div oracle; do not restate RIFormatBlockCount here.
  const uint32_t expectedBlocksWide =
      (testCase.width + props->blockWidth - 1u) / props->blockWidth;
  const uint32_t expectedBlocksHigh =
      (testCase.height + props->blockHeight - 1u) / props->blockHeight;
  const uint32_t expectedRowPitch = expectedBlocksWide * props->stride;
  const uint32_t expectedStagingBytes = expectedRowPitch * expectedBlocksHigh;
  const uint32_t blocksWide = RIFormatBlockCount(testCase.width, props->blockWidth);
  const uint32_t blocksHigh = RIFormatBlockCount(testCase.height, props->blockHeight);
  const uint32_t rowPitch = blocksWide * props->stride;
  const uint32_t stagingBytes = rowPitch * blocksHigh;
  ASSERT_EQ_MSG(blocksWide, expectedBlocksWide, testCase.name);
  ASSERT_EQ_MSG(blocksHigh, expectedBlocksHigh, testCase.name);
  ASSERT_EQ_MSG(rowPitch, expectedRowPitch, testCase.name);
  ASSERT_EQ_MSG(stagingBytes, expectedStagingBytes, testCase.name);
}
} // namespace

struct BcBlockLayout { size_t index; };
UTEST_I_SETUP(BcBlockLayout) { utest_fixture->index = utest_index; }
UTEST_I_TEARDOWN(BcBlockLayout) {}

UTEST(BcBlockLayout, FormatProperties) {
  CheckFormatProps(utest_result, RI_FORMAT_BC1_RGBA_UNORM, 4, 4, 8,
                   "BC1 format props use 4x4 blocks and 8 bytes");
  CheckFormatProps(utest_result, RI_FORMAT_BC3_RGBA_UNORM, 4, 4, 16,
                   "BC3 format props use 4x4 blocks and 16 bytes");
  CheckFormatProps(utest_result, RI_FORMAT_RGBA8_UNORM, 1, 1, 4,
                   "RGBA8 format props use one 4-byte texel");
}

UTEST_I(BcBlockLayout, Rows, 29) {
  const LayoutCase cases[] = {
      {RI_FORMAT_BC1_RGBA_UNORM, 5, 5, "BC1 5x5"}, {RI_FORMAT_BC1_RGBA_UNORM, 6, 3, "BC1 6x3"},
      {RI_FORMAT_BC1_RGBA_UNORM, 7, 7, "BC1 7x7"}, {RI_FORMAT_BC1_RGBA_UNORM, 4, 6, "BC1 4x6"},
      {RI_FORMAT_BC3_RGBA_UNORM, 5, 5, "BC3 5x5"}, {RI_FORMAT_BC3_RGBA_UNORM, 6, 3, "BC3 6x3"},
      {RI_FORMAT_BC3_RGBA_UNORM, 7, 7, "BC3 7x7"}, {RI_FORMAT_BC3_RGBA_UNORM, 4, 6, "BC3 4x6"},
      {RI_FORMAT_BC1_RGBA_UNORM, 1, 1, "BC1 1x1 tail mip"}, {RI_FORMAT_BC1_RGBA_UNORM, 2, 2, "BC1 2x2 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 3, 3, "BC1 3x3 tail mip"}, {RI_FORMAT_BC1_RGBA_UNORM, 1, 4, "BC1 1x4 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 1, "BC1 4x1 tail mip"}, {RI_FORMAT_BC3_RGBA_UNORM, 1, 1, "BC3 1x1 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 2, 2, "BC3 2x2 tail mip"}, {RI_FORMAT_BC3_RGBA_UNORM, 3, 3, "BC3 3x3 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 1, 4, "BC3 1x4 tail mip"}, {RI_FORMAT_BC3_RGBA_UNORM, 4, 1, "BC3 4x1 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 4, "BC1 4x4 exact multiple"}, {RI_FORMAT_BC1_RGBA_UNORM, 8, 8, "BC1 8x8 exact multiple"},
      {RI_FORMAT_BC1_RGBA_UNORM, 16, 16, "BC1 16x16 exact multiple"}, {RI_FORMAT_BC1_RGBA_UNORM, 16, 4, "BC1 16x4 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 4, "BC3 4x4 exact multiple"}, {RI_FORMAT_BC3_RGBA_UNORM, 8, 8, "BC3 8x8 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 16, 16, "BC3 16x16 exact multiple"}, {RI_FORMAT_BC3_RGBA_UNORM, 16, 4, "BC3 16x4 exact multiple"},
      {RI_FORMAT_BC1_RGBA_UNORM, 1023, 17, "BC1 1023x17"}, {RI_FORMAT_BC3_RGBA_UNORM, 1023, 17, "BC3 1023x17"},
      {RI_FORMAT_RGBA8_UNORM, 5, 1, "RGBA8 5 texels with blockDim 1"},
  };
  CheckLayout(utest_result, cases[utest_fixture->index]);
}
