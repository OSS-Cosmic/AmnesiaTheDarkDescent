#include "graphics/DisplayDepthPolicy.h"
#include "utest.h"

namespace {

using hpl::DisplayDepthCandidate;
using hpl::DisplayDepthInputs;
using hpl::DisplayDepthSource;

DisplayDepthCandidate Candidate(uint32_t width, uint32_t height,
                                bool hasImage = true,
                                bool hasAttachmentView = true) {
  DisplayDepthCandidate candidate;
  candidate.allocatedExtent = {width, height};
  candidate.hasImage = hasImage;
  candidate.hasAttachmentView = hasAttachmentView;
  return candidate;
}

DisplayDepthInputs NativeSceneInputs() {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.scene = Candidate(1920, 1080);
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  return inputs;
}

} // namespace

UTEST(DisplayDepthPolicy, AllocationContract) {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.scene = Candidate(960, 540);
  inputs.sceneIndexInRange = true;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "reduced scene allocation is rejected");

  inputs.sceneExtentCompatible = true;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "compatible logical extent does not prove allocation size");
}

UTEST(DisplayDepthPolicy, PresentationPreference) {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.presentation = Candidate(1920, 1080);
  inputs.presentationCurrent = true;
  inputs.scene = Candidate(960, 540);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) ==
                      DisplayDepthSource::Presentation,
                  "current presentation depth wins over reduced scene depth");
}

UTEST(DisplayDepthPolicy, SceneFallback) {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.presentation = Candidate(1920, 1080);
  inputs.presentationCurrent = false;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::Scene,
                  "stale presentation falls back to native scene depth");

  inputs.scene.hasAttachmentView = false;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "missing scene attachment view has no fallback");

  inputs.scene.hasAttachmentView = true;
  inputs.sceneIndexInRange = false;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "out-of-range scene index has no fallback");
}

UTEST(DisplayDepthPolicy, RequestedExtent) {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {960, 540};
  inputs.scene = Candidate(960, 540);
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepthForExtent(inputs, 1920, 1080) ==
                      DisplayDepthSource::None,
                  "offscreen depth is rejected for a larger GUI extent");

  EXPECT_TRUE_MSG(hpl::SelectDisplayDepthForExtent(inputs, 960, 540) ==
                      DisplayDepthSource::Scene,
                  "offscreen depth is selected at its own extent");
}

UTEST(DisplayDepthPolicy, ExtentMismatches) {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.presentation = Candidate(1919, 1080);
  inputs.presentationCurrent = true;
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "presentation width mismatch is rejected");

  inputs.presentation = Candidate(1920, 1079);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "presentation height mismatch is rejected");

  inputs.presentationCurrent = false;
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  inputs.scene = Candidate(1919, 1080);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "scene width mismatch is rejected");

  inputs.scene = Candidate(1920, 1079);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "scene height mismatch is rejected");
}

UTEST(DisplayDepthPolicy, InvalidInputs) {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.displayExtent = {0, 0};
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "zero display extent is rejected");

  inputs = NativeSceneInputs();
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepthForExtent(inputs, 0, 1080) ==
                      DisplayDepthSource::None,
                  "zero requested width is rejected");
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepthForExtent(inputs, 1920, 0) ==
                      DisplayDepthSource::None,
                  "zero requested height is rejected");

  inputs = NativeSceneInputs();
  inputs.scene = Candidate(1920, 1080, false, true);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "missing scene image is rejected");

  inputs.scene = Candidate(1920, 1080, true, false);
  EXPECT_TRUE_MSG(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
                  "missing scene attachment view is rejected");
}
