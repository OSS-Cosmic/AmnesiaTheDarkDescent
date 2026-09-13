#include "graphics/FsrUpscalerParams.h"
#include "utest.h"

#include <cmath>
#include <limits>

namespace {
bool Near(float a, float b, float epsilon = 2.0e-6f) {
  return std::fabs(a - b) <= epsilon;
}

hpl::FsrFrameParamsInput ValidInput() {
  hpl::FsrFrameParamsInput input;
  input.renderExtent = {1023, 767};
  input.outputExtent = {1920, 1080};
  input.jitterPixels[0] = 0.1f;
  input.jitterPixels[1] = -0.2f;
  input.deltaTimeMs = 16.7f;
  input.zNear = 0.1f;
  input.zFar = 100.0f;
  input.verticalFovRadians = 1.0f;
  input.preExposure = 1.0f;
  input.resetHistory = true;
  return input;
}

bool SameDispatch(const hpl::FsrDispatchParams &a,
                  const hpl::FsrDispatchParams &b) {
  return a.jitterX == b.jitterX && a.jitterY == b.jitterY &&
         a.motionVectorScaleX == b.motionVectorScaleX &&
         a.motionVectorScaleY == b.motionVectorScaleY &&
         a.renderWidth == b.renderWidth && a.renderHeight == b.renderHeight &&
         a.upscaleWidth == b.upscaleWidth &&
         a.upscaleHeight == b.upscaleHeight &&
         a.frameTimeDeltaMs == b.frameTimeDeltaMs &&
         a.cameraNear == b.cameraNear && a.cameraFar == b.cameraFar &&
         a.cameraFovAngleVertical == b.cameraFovAngleVertical &&
         a.preExposure == b.preExposure && a.reset == b.reset &&
         a.enableSharpening == b.enableSharpening && a.sharpness == b.sharpness;
}
bool SameDispatchExceptJitter(const hpl::FsrDispatchParams &a,
                              const hpl::FsrDispatchParams &b) {
  hpl::FsrDispatchParams aa = a, bb = b;
  aa.jitterX = aa.jitterY = bb.jitterX = bb.jitterY = 0.0f;
  return SameDispatch(aa, bb);
}
bool SameMaskPolicy(const hpl::FsrMaskPolicy &a, const hpl::FsrMaskPolicy &b) {
  return a.bindReactiveMask == b.bindReactiveMask &&
         a.bindCompositionMask == b.bindCompositionMask &&
         a.bindOpaqueColor == b.bindOpaqueColor &&
         a.enableAutoReactive == b.enableAutoReactive;
}
hpl::FsrMaskBindingInfo PresentBinding(hpl::FsrExtent extent) {
  hpl::FsrMaskBindingInfo binding;
  binding.present = true;
  binding.extent = extent;
  return binding;
}

void CheckParamFailure(int *utest_result, const hpl::FsrFrameParamsInput &input,
                       hpl::FsrParamsError expected, const char *resultName,
                       const char *untouchedName) {
  hpl::FsrDispatchParams sentinel = {};
  sentinel.jitterX = 17.0f;
  sentinel.jitterY = -19.0f;
  sentinel.motionVectorScaleX = 23.0f;
  sentinel.motionVectorScaleY = -29.0f;
  sentinel.renderWidth = 31;
  sentinel.renderHeight = 37;
  sentinel.upscaleWidth = 41;
  sentinel.upscaleHeight = 43;
  sentinel.frameTimeDeltaMs = 47.0f;
  sentinel.cameraNear = 53.0f;
  sentinel.cameraFar = 59.0f;
  sentinel.cameraFovAngleVertical = 61.0f;
  sentinel.preExposure = 67.0f;
  sentinel.reset = true;
  sentinel.enableSharpening = true;
  sentinel.sharpness = 71.0f;
  hpl::FsrDispatchParams output = sentinel;
  EXPECT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &output), expected,
                resultName);
  EXPECT_TRUE_MSG(SameDispatch(output, sentinel), untouchedName);
  (void)utest_result;
}
void CheckMaskFailure(int *utest_result, const hpl::FsrMaskPolicyInput &input,
                      hpl::FsrMaskError expected, const char *resultName,
                      const char *untouchedName) {
  const hpl::FsrMaskPolicy sentinel = {true, true, true, true};
  hpl::FsrMaskPolicy output = sentinel;
  EXPECT_EQ_MSG(hpl::FsrBuildMaskPolicy(input, &output), expected, resultName);
  EXPECT_TRUE_MSG(SameMaskPolicy(output, sentinel), untouchedName);
  (void)utest_result;
}
} // namespace

struct FsrUpscalerParamsIndexed {
  size_t index;
};
UTEST_I_SETUP(FsrUpscalerParamsIndexed) { utest_fixture->index = utest_index; }
UTEST_I_TEARDOWN(FsrUpscalerParamsIndexed) {}

UTEST_I(FsrUpscalerParamsIndexed, JitterConvention, 3) {
  const float jitters[][2] = {{0.25f, -0.375f}, {-0.5f, 0.5f}, {0.1f, 0.2f}};
  auto input = ValidInput();
  input.jitterPixels[0] = jitters[utest_fixture->index][0];
  input.jitterPixels[1] = jitters[utest_fixture->index][1];
  hpl::FsrDispatchParams params;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &params),
                hpl::FsrParamsError::None,
                "numeric jitter convention validates");
  const float engineX =
      -2.0f * input.jitterPixels[0] / input.renderExtent.width;
  const float engineY =
      2.0f * input.jitterPixels[1] / input.renderExtent.height;
  EXPECT_TRUE_MSG(
      Near(engineX, 2.0f * params.jitterX / input.renderExtent.width, 1.0e-7f),
      "numeric jitter x translations agree");
  EXPECT_TRUE_MSG(Near(engineY,
                       -2.0f * params.jitterY / input.renderExtent.height,
                       1.0e-7f),
                  "numeric jitter y translations agree");
}

UTEST(FsrUpscalerParams, OddExtentAndMotionScale) {
  auto input = ValidInput();
  hpl::FsrDispatchParams params;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &params),
                hpl::FsrParamsError::None, "odd render extent validates");
  EXPECT_TRUE_MSG(params.motionVectorScaleX == -1023.0f &&
                      params.motionVectorScaleY == -767.0f,
                  "motion scale uses negative odd render extent");
  EXPECT_TRUE_MSG(params.renderWidth == 1023 && params.renderHeight == 767 &&
                      params.upscaleWidth == 1920 &&
                      params.upscaleHeight == 1080,
                  "odd render and output extents round-trip");
}
UTEST(FsrUpscalerParams, NativeAAAndExceedingOutput) {
  auto input = ValidInput();
  input.outputExtent = input.renderExtent;
  hpl::FsrDispatchParams params;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &params),
                hpl::FsrParamsError::None,
                "equal render and output extents validate as NativeAA");
  input = ValidInput();
  input.renderExtent.width = input.outputExtent.width + 1;
  EXPECT_EQ_MSG(hpl::FsrBuildDispatchParams(input, nullptr),
                hpl::FsrParamsError::RenderExceedsOutput,
                "render exceeding output width is rejected");
  input = ValidInput();
  input.renderExtent.height = input.outputExtent.height + 1;
  EXPECT_EQ_MSG(hpl::FsrBuildDispatchParams(input, nullptr),
                hpl::FsrParamsError::RenderExceedsOutput,
                "render exceeding output height is rejected");
}
UTEST(FsrUpscalerParams, StationaryJitteredCamera) {
  auto firstInput = ValidInput();
  firstInput.jitterPixels[0] = 0.25f;
  firstInput.jitterPixels[1] = -0.375f;
  auto secondInput = firstInput;
  secondInput.jitterPixels[0] = -0.5f;
  secondInput.jitterPixels[1] = 0.5f;
  hpl::FsrDispatchParams first, second;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(firstInput, &first),
                hpl::FsrParamsError::None, "first stationary jitter validates");
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(secondInput, &second),
                hpl::FsrParamsError::None,
                "second stationary jitter validates");
  EXPECT_TRUE_MSG(
      first.cameraNear == firstInput.zNear &&
          first.cameraFar == firstInput.zFar &&
          first.cameraFovAngleVertical == firstInput.verticalFovRadians &&
          second.cameraNear == secondInput.zNear &&
          second.cameraFar == secondInput.zFar &&
          second.cameraFovAngleVertical == secondInput.verticalFovRadians,
      "stationary camera scalars pass through verbatim");
  EXPECT_TRUE_MSG(SameDispatchExceptJitter(first, second) &&
                      first.jitterX != second.jitterX &&
                      first.jitterY != second.jitterY,
                  "stationary camera changes only jitter fields");
}
UTEST(FsrUpscalerParams, CameraMotionAndDelta) {
  auto input = ValidInput();
  input.zNear = 0.25f;
  input.zFar = 250.0f;
  input.verticalFovRadians = 0.9f;
  input.preExposure = 0.75f;
  hpl::FsrDispatchParams params;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &params),
                hpl::FsrParamsError::None, "nonzero camera motion validates");
  EXPECT_TRUE_MSG(
      params.cameraNear == input.zNear && params.cameraFar == input.zFar &&
          params.cameraFovAngleVertical == input.verticalFovRadians &&
          params.frameTimeDeltaMs == 16.7f,
      "camera motion scalars and normal delta pass through");
  EXPECT_NE_MSG(params.frameTimeDeltaMs, 0.0f,
                "nonzero camera motion delta remains nonzero");
  input.deltaTimeMs = 50000.0f;
  ASSERT_EQ_MSG(hpl::FsrBuildDispatchParams(input, &params),
                hpl::FsrParamsError::None,
                "absurd hitch validates before clamping");
  EXPECT_EQ_MSG(params.frameTimeDeltaMs, 1000.0f,
                "absurd hitch delta clamps at one second");
}

UTEST_I(FsrUpscalerParamsIndexed, DispatchValidation, 24) {
  auto input = ValidInput();
  switch (utest_fixture->index) {
  case 0:
    input.renderExtent.width = 0;
    break;
  case 1:
    input.renderExtent.height = 16385;
    break;
  case 2:
    input.outputExtent.width = 0;
    break;
  case 3:
    input.outputExtent.height = 16385;
    break;
  case 4:
    input.renderExtent.width = input.outputExtent.width + 1;
    break;
  case 5:
    input.jitterPixels[0] = 0.6f;
    break;
  case 6:
    input.jitterPixels[0] = std::numeric_limits<float>::quiet_NaN();
    break;
  case 7:
    input.jitterPixels[1] = std::numeric_limits<float>::infinity();
    break;
  case 8:
    input.zNear = 0.0f;
    break;
  case 9:
    input.zFar = 0.0f;
    break;
  case 10:
    input.zNear = 100.0f;
    break;
  case 11:
    input.zNear = std::numeric_limits<float>::quiet_NaN();
    break;
  case 12:
    input.zFar = std::numeric_limits<float>::infinity();
    break;
  case 13:
    input.verticalFovRadians = 0.0f;
    break;
  case 14:
    input.verticalFovRadians = 3.14159265358979323846f;
    break;
  case 15:
    input.verticalFovRadians = std::numeric_limits<float>::quiet_NaN();
    break;
  case 16:
    input.verticalFovRadians = std::numeric_limits<float>::infinity();
    break;
  case 17:
    input.deltaTimeMs = -1.0f;
    break;
  case 18:
    input.deltaTimeMs = std::numeric_limits<float>::quiet_NaN();
    break;
  case 19:
    input.deltaTimeMs = std::numeric_limits<float>::infinity();
    break;
  case 20:
    input.preExposure = 0.0f;
    break;
  case 21:
    input.preExposure = -1.0f;
    break;
  case 22:
    input.preExposure = std::numeric_limits<float>::quiet_NaN();
    break;
  case 23:
    input.preExposure = std::numeric_limits<float>::infinity();
    break;
  }
  const hpl::FsrParamsError expected[] = {
      hpl::FsrParamsError::BadRenderExtent,
      hpl::FsrParamsError::BadRenderExtent,
      hpl::FsrParamsError::BadOutputExtent,
      hpl::FsrParamsError::BadOutputExtent,
      hpl::FsrParamsError::RenderExceedsOutput,
      hpl::FsrParamsError::BadJitter,
      hpl::FsrParamsError::BadJitter,
      hpl::FsrParamsError::BadJitter,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadDeltaTime,
      hpl::FsrParamsError::BadDeltaTime,
      hpl::FsrParamsError::BadDeltaTime,
      hpl::FsrParamsError::BadPreExposure,
      hpl::FsrParamsError::BadPreExposure,
      hpl::FsrParamsError::BadPreExposure,
      hpl::FsrParamsError::BadPreExposure};
  CheckParamFailure(utest_result, input, expected[utest_fixture->index],
                    "dispatch validation returns expected error",
                    "invalid dispatch input leaves output untouched");
}
UTEST(FsrUpscalerParams, NullDispatchOutput) {
  auto input = ValidInput();
  EXPECT_EQ_MSG(hpl::FsrBuildDispatchParams(input, nullptr),
                hpl::FsrParamsError::None,
                "null dispatch output is safe for validation-only success");
  input.jitterPixels[0] = 0.6f;
  EXPECT_EQ_MSG(hpl::FsrBuildDispatchParams(input, nullptr),
                hpl::FsrParamsError::BadJitter,
                "null dispatch output is safe for validation-only failure");
}

UTEST_I(FsrUpscalerParamsIndexed, ParameterErrorStrings, 11) {
  const hpl::FsrParamsError errors[] = {
      hpl::FsrParamsError::None,
      hpl::FsrParamsError::BadRenderExtent,
      hpl::FsrParamsError::BadOutputExtent,
      hpl::FsrParamsError::RenderExceedsOutput,
      hpl::FsrParamsError::BadJitter,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadDeltaTime,
      hpl::FsrParamsError::BadPreExposure,
      static_cast<hpl::FsrParamsError>(-1),
      static_cast<hpl::FsrParamsError>(999)};
  const char *expected[] = {"none",
                            "bad render extent",
                            "bad output extent",
                            "render exceeds output",
                            "bad jitter",
                            "bad camera planes",
                            "bad field of view",
                            "bad delta time",
                            "bad pre-exposure",
                            "unknown FSR parameter error",
                            "unknown FSR parameter error"};
  EXPECT_STREQ_MSG(hpl::FsrParamsErrorString(errors[utest_fixture->index]),
                   expected[utest_fixture->index],
                   "parameter error string is stable");
}
UTEST_I(FsrUpscalerParamsIndexed, MaskPolicyCombinations, 5) {
  const hpl::FsrExtent extent = {1023, 767};
  const auto present = PresentBinding(extent);
  hpl::FsrMaskPolicyInput input = {};
  input.renderExtent = extent;
  // Explicit masks must suppress automatic generation even with opaque color
  // present.
  if (utest_fixture->index != 0)
    input.opaqueColor = present;
  switch (utest_fixture->index) {
  case 2:
    input.reactiveMask = present;
    break;
  case 3:
    input.compositionMask = present;
    break;
  case 4:
    input.reactiveMask = present;
    input.compositionMask = present;
    break;
  }
  const hpl::FsrMaskPolicy expected[] = {{false, false, false, false},
                                         {false, false, true, true},
                                         {true, false, false, false},
                                         {false, true, false, false},
                                         {true, true, false, false}};
  hpl::FsrMaskPolicy policy;
  ASSERT_EQ_MSG(hpl::FsrBuildMaskPolicy(input, &policy),
                hpl::FsrMaskError::None, "mask policy combination validates");
  EXPECT_TRUE_MSG(SameMaskPolicy(policy, expected[utest_fixture->index]),
                  "mask policy combination has expected bindings");
}
UTEST_I(FsrUpscalerParamsIndexed, MaskValidation, 6) {
  const hpl::FsrExtent extent = {1023, 767};
  const auto present = PresentBinding(extent);
  hpl::FsrMaskPolicyInput input = {};
  input.renderExtent = extent;
  hpl::FsrMaskError expected;
  switch (utest_fixture->index) {
  case 0:
    input.reactiveMask = present;
    input.reactiveMask.extent.width++;
    expected = hpl::FsrMaskError::BadReactiveMask;
    break;
  case 1:
    input.reactiveMask = present;
    input.reactiveMask.mipCount = 2;
    expected = hpl::FsrMaskError::BadReactiveMask;
    break;
  case 2:
    input.compositionMask = present;
    input.compositionMask.layerCount = 2;
    expected = hpl::FsrMaskError::BadCompositionMask;
    break;
  case 3:
    input.opaqueColor = present;
    input.opaqueColor.extent.height++;
    expected = hpl::FsrMaskError::BadOpaqueColor;
    break;
  case 4:
    input.renderExtent.width = 0;
    expected = hpl::FsrMaskError::BadRenderExtent;
    break;
  default:
    input.renderExtent.width = 16385;
    expected = hpl::FsrMaskError::BadRenderExtent;
    break;
  }
  CheckMaskFailure(utest_result, input, expected,
                   "mask validation returns expected error",
                   "invalid mask input leaves output untouched");
}
UTEST(FsrUpscalerParams, NullMaskOutput) {
  hpl::FsrMaskPolicyInput input = {};
  input.renderExtent = {1023, 767};
  EXPECT_EQ_MSG(hpl::FsrBuildMaskPolicy(input, nullptr),
                hpl::FsrMaskError::None,
                "null mask output is safe for validation-only success");
}
UTEST_I(FsrUpscalerParamsIndexed, MaskErrorStrings, 7) {
  const hpl::FsrMaskError errors[] = {hpl::FsrMaskError::None,
                                      hpl::FsrMaskError::BadRenderExtent,
                                      hpl::FsrMaskError::BadReactiveMask,
                                      hpl::FsrMaskError::BadCompositionMask,
                                      hpl::FsrMaskError::BadOpaqueColor,
                                      static_cast<hpl::FsrMaskError>(-1),
                                      static_cast<hpl::FsrMaskError>(999)};
  const char *expected[] = {"none",
                            "bad render extent",
                            "bad reactive mask",
                            "bad composition mask",
                            "bad opaque color",
                            "unknown FSR mask error",
                            "unknown FSR mask error"};
  EXPECT_STREQ_MSG(hpl::FsrMaskErrorString(errors[utest_fixture->index]),
                   expected[utest_fixture->index],
                   "mask error string is stable");
}
UTEST_MAIN();
