#include "graphics/RIFormat.h"
#include "utest.h"

#include <cstdint>

namespace {
struct UploadRowPitchCase {
  RI_Format format; uint32_t deviceRowAlignment, width, sliceNum;
  uint32_t expectedStride, expectedBlockWidth, expectedBlockHeight;
  const char *name;
};

uint64_t ExpectedLcm(int *utest_result, uint32_t alignment, uint32_t stride) {
  if (alignment == 0 || stride == 0) {
    *utest_result = UTEST_TEST_FAILURE;
    return 0;
  }
  uint64_t a = alignment, b = stride;
  // Independent Euclidean GCD/LCM oracle; do not restate the implementation.
  while (b != 0) { const uint64_t remainder = a % b; a = b; b = remainder; }
  return (static_cast<uint64_t>(alignment) / a) * stride;
}

void CheckUploadRowPitch(int *utest_result, const UploadRowPitchCase &testCase) {
  const RIFormatProps *props = GetRIFormatProps(testCase.format);
  ASSERT_NE_MSG(props, nullptr, testCase.name);
  ASSERT_GT_MSG(props->stride, 0u, testCase.name);
  ASSERT_GT_MSG(props->blockWidth, 0u, testCase.name);
  ASSERT_GT_MSG(props->blockHeight, 0u, testCase.name);
  ASSERT_GT_MSG(testCase.deviceRowAlignment, 0u, testCase.name);
  ASSERT_EQ_MSG(props->stride, testCase.expectedStride, testCase.name);
  ASSERT_EQ_MSG(props->blockWidth, testCase.expectedBlockWidth, testCase.name);
  ASSERT_EQ_MSG(props->blockHeight, testCase.expectedBlockHeight, testCase.name);
  const uint64_t rowPitch = static_cast<uint64_t>(RIFormatBlockCount(
      testCase.width, props->blockWidth)) * props->stride;
  const uint64_t alignRowPitch = RIFormatAlignRowPitch(
      rowPitch, testCase.deviceRowAlignment, props->stride);
  const uint64_t expectedLcm = ExpectedLcm(utest_result,
                                           testCase.deviceRowAlignment,
                                           props->stride);
  ASSERT_EQ_MSG(alignRowPitch % props->stride, 0u, testCase.name);
  ASSERT_EQ_MSG(alignRowPitch % testCase.deviceRowAlignment, 0u, testCase.name);
  ASSERT_GE_MSG(alignRowPitch, rowPitch, testCase.name);
  ASSERT_LT_MSG(alignRowPitch - rowPitch, expectedLcm, testCase.name);
  const uint64_t rowBlockNum = alignRowPitch / props->stride;
  const uint64_t bufferRowLength = rowBlockNum * props->blockWidth;
  ASSERT_EQ_MSG((bufferRowLength / props->blockWidth) * props->stride,
                alignRowPitch, testCase.name);
  const uint64_t alignSlicePitch = static_cast<uint64_t>(testCase.sliceNum) * alignRowPitch;
  ASSERT_EQ_MSG(alignSlicePitch % alignRowPitch, 0u, testCase.name);
  ASSERT_EQ_MSG(alignSlicePitch / alignRowPitch, testCase.sliceNum, testCase.name);
}
} // namespace

struct UploadRowPitch { size_t index; };
UTEST_I_SETUP(UploadRowPitch) { utest_fixture->index = utest_index; }
UTEST_I_TEARDOWN(UploadRowPitch) {}

UTEST_I(UploadRowPitch, Rows, 22) {
  const UploadRowPitchCase cases[] = {
      {RI_FORMAT_RGB8_UNORM, 256, 5, 2, 3, 1, 1, "RGB8 5x2 row alignment 256"},
      {RI_FORMAT_BGR8_UNORM, 256, 5, 2, 3, 1, 1, "BGR8 5x2 row alignment 256"},
      {RI_FORMAT_RGB8_UNORM, 1, 6, 1, 3, 1, 1, "RGB8 width multiple of stride"},
      {RI_FORMAT_BGR8_UNORM, 4, 5, 3, 3, 1, 1, "BGR8 row alignment 4"},
      {RI_FORMAT_RGB8_UNORM, 64, 6, 2, 3, 1, 1, "RGB8 row alignment 64"},
      {RI_FORMAT_BGR8_UNORM, 512, 7, 4, 3, 1, 1, "BGR8 row alignment 512"},
      {RI_FORMAT_R8_UNORM, 1, 5, 1, 1, 1, 1, "R8 width not aligned"},
      {RI_FORMAT_R8_UNORM, 64, 64, 2, 1, 1, 1, "R8 width aligned"},
      {RI_FORMAT_RGBA8_UNORM, 4, 5, 2, 4, 1, 1, "RGBA8 width not aligned"},
      {RI_FORMAT_RGBA8_UNORM, 256, 64, 3, 4, 1, 1, "RGBA8 width aligned"},
      {RI_FORMAT_RG16_UNORM, 64, 7, 2, 4, 1, 1, "RG16 width not aligned"},
      {RI_FORMAT_RG16_SFLOAT, 512, 8, 4, 4, 1, 1, "RG16 width aligned"},
      {RI_FORMAT_BC1_RGBA_UNORM, 1, 4, 1, 8, 4, 4, "BC1 4x4"},
      {RI_FORMAT_BC1_RGBA_UNORM, 256, 5, 2, 8, 4, 4, "BC1 5x5"},
      {RI_FORMAT_BC1_RGBA_UNORM, 64, 7, 2, 8, 4, 4, "BC1 7x7"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 8, 2, 8, 4, 4, "BC1 8x8"},
      {RI_FORMAT_BC3_RGBA_UNORM, 256, 5, 2, 16, 4, 4, "BC3 5x5"},
      {RI_FORMAT_BC3_RGBA_UNORM, 512, 7, 2, 16, 4, 4, "BC3 7x7"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 8, 2, 16, 4, 4, "BC3 8x8"},
      {RI_FORMAT_BC7_RGBA_UNORM, 256, 5, 2, 16, 4, 4, "BC7 5x5"},
      {RI_FORMAT_BC7_RGBA_UNORM, 64, 7, 2, 16, 4, 4, "BC7 7x7"},
      {RI_FORMAT_BC7_RGBA_UNORM, 4, 8, 2, 16, 4, 4, "BC7 8x8"},
  };
  CheckUploadRowPitch(utest_result, cases[utest_fixture->index]);
}
