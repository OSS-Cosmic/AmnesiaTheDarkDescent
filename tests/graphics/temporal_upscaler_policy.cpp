#include "graphics/TemporalUpscalerPolicy.h"
#include "utest.h"

#include <cstdint>

namespace {

using hpl::TemporalHistoryResetTriggers;
using hpl::TemporalProviderFailureKey;
using hpl::TemporalProviderFailureState;
using hpl::TemporalRenderExtentDecision;
using hpl::TemporalRenderExtentOwner;
using hpl::TemporalRenderExtentResolution;
using hpl::TemporalRenderExtentState;
using hpl::TemporalUpscalerExtent;
using hpl::TemporalUpscalerProvider;
using hpl::TemporalUpscalerQuality;
using hpl::TemporalUpscalerSettings;

bool SameExtent(TemporalUpscalerExtent left, TemporalUpscalerExtent right) {
  return left.width == right.width && left.height == right.height;
}

// This test double does not derive from iTemporalUpscaler because that
// interface exposes RICmd*, RITexture*, and cGraphics::FrameContext*, which
// would pull the Vulkan/volk headers into this dependency-free test project.
struct StubTemporalUpscaler {
  bool supports = true;
  bool prepareSucceeds = true;
  bool resolveSucceeds = true;
  TemporalUpscalerExtent recommended = {1280, 720};
  uint32_t phaseCount = 8;
  uint32_t prepareCalls = 0;
  uint32_t resolveCalls = 0;

  bool Supports(TemporalUpscalerProvider, TemporalUpscalerQuality) const {
    return supports;
  }

  TemporalUpscalerExtent GetRecommendedRenderExtent(
      TemporalUpscalerExtent, TemporalUpscalerQuality) const {
    return recommended;
  }

  uint32_t GetJitterPhaseCount(TemporalUpscalerExtent,
                               TemporalUpscalerExtent) const {
    return phaseCount;
  }

  bool PrepareContext(const TemporalUpscalerSettings &,
                      TemporalUpscalerExtent,
                      TemporalUpscalerExtent) {
    ++prepareCalls;
    return prepareSucceeds;
  }

  bool RecordResolve(TemporalUpscalerExtent, TemporalUpscalerExtent) {
    ++resolveCalls;
    return resolveSucceeds;
  }
};

struct DrivenViewport {
  TemporalRenderExtentState extent = {};
  TemporalProviderFailureState failure = {};
  bool providerPrepared = false;
  TemporalUpscalerSettings preparedSettings = {};
  TemporalUpscalerExtent preparedRenderExtent = {};
  TemporalUpscalerExtent preparedDisplayExtent = {};
  uint32_t jitterPhaseCount = 0;
};

struct PrepareResult {
  bool attempted = false;
  bool prepared = false;
  bool historyReset = false;
};

void ApplyExtentDecision(DrivenViewport &viewport,
                         const TemporalRenderExtentDecision &decision,
                         bool *historyReset = nullptr) {
  viewport.extent = decision.state;
  if (historyReset != nullptr)
    *historyReset = decision.historyReset;
}

PrepareResult FailPreparation(DrivenViewport &viewport,
                              const TemporalProviderFailureKey &key) {
  LatchTemporalProviderFailure(viewport.failure, key);
  viewport.providerPrepared = false;
  const TemporalRenderExtentDecision released =
      ReleaseTemporalProviderRenderExtent(viewport.extent);
  bool reset = false;
  ApplyExtentDecision(viewport, released, &reset);
  TemporalHistoryResetTriggers failureReset;
  failureReset.providerFailure = true;
  reset = reset || TemporalHistoryResetRequired(failureReset);
  return {true, false, reset};
}

PrepareResult Prepare(DrivenViewport &viewport, StubTemporalUpscaler &adapter,
                      const TemporalUpscalerSettings &settings,
                      TemporalUpscalerExtent displayExtent) {
  const TemporalProviderFailureKey key = {
      settings.provider, settings.quality, displayExtent};
  if (viewport.failure.latched &&
      !TemporalProviderFailureKeyMatches(viewport.failure.key, key)) {
    ClearTemporalProviderFailure(viewport.failure);
  }

  if (settings.provider == TemporalUpscalerProvider::Off ||
      TemporalProviderFailureSuppressed(viewport.failure, key)) {
    viewport.providerPrepared = false;
    const TemporalRenderExtentDecision released =
        ReleaseTemporalProviderRenderExtent(viewport.extent);
    bool reset = false;
    ApplyExtentDecision(viewport, released, &reset);
    return {false, false, reset};
  }

  if (!adapter.Supports(settings.provider, settings.quality))
    return FailPreparation(viewport, key);

  const TemporalUpscalerExtent renderExtent =
      settings.quality == TemporalUpscalerQuality::NativeAA
          ? displayExtent
          : adapter.GetRecommendedRenderExtent(displayExtent, settings.quality);
  if (!TemporalUpscalerNegotiatedExtentValid(renderExtent, displayExtent,
                                             settings.quality)) {
    return FailPreparation(viewport, key);
  }

  if (!adapter.PrepareContext(settings, renderExtent, displayExtent))
    return FailPreparation(viewport, key);

  const bool hadPreparedProvider = viewport.providerPrepared;
  const TemporalUpscalerSettings previousSettings = viewport.preparedSettings;
  const TemporalUpscalerExtent previousDisplayExtent =
      viewport.preparedDisplayExtent;
  const bool settingChanged =
      hadPreparedProvider &&
      (previousSettings.provider != settings.provider ||
       previousSettings.quality != settings.quality ||
       !SameExtent(previousDisplayExtent, displayExtent));
  ClearTemporalProviderFailure(viewport.failure);
  viewport.providerPrepared = true;
  viewport.preparedSettings = settings;
  viewport.preparedRenderExtent = renderExtent;
  viewport.preparedDisplayExtent = displayExtent;
  viewport.jitterPhaseCount = adapter.GetJitterPhaseCount(renderExtent,
                                                            displayExtent);
  const TemporalRenderExtentDecision claimed =
      ClaimTemporalProviderRenderExtent(viewport.extent, renderExtent);
  bool reset = false;
  ApplyExtentDecision(viewport, claimed, &reset);

  if (settingChanged) {
    TemporalHistoryResetTriggers triggers;
    triggers.providerChanged = previousSettings.provider != settings.provider;
    triggers.qualityChanged = previousSettings.quality != settings.quality;
    triggers.outputExtentChanged = !SameExtent(previousDisplayExtent,
                                               displayExtent);
    reset = reset || TemporalHistoryResetRequired(triggers);
  }
  return {true, true, reset};
}

TemporalRenderExtentDecision Resolve(DrivenViewport &viewport,
                                     float devRenderScale,
                                     TemporalUpscalerExtent displayExtent) {
  const TemporalRenderExtentResolution input = {
      viewport.providerPrepared,
      viewport.preparedRenderExtent,
      devRenderScale,
      displayExtent,
      viewport.extent};
  const TemporalRenderExtentDecision decision =
      ResolveTemporalRenderExtent(input);
  ApplyExtentDecision(viewport, decision);
  return decision;
}

} // namespace

UTEST(TemporalUpscalerPolicy, ExtentValidation) {
  const TemporalUpscalerExtent display = {1920, 1080};
  EXPECT_TRUE_MSG(TemporalUpscalerNegotiatedExtentValid(
                      {1280, 720}, display, TemporalUpscalerQuality::Quality),
                  "valid negotiated extent");
  EXPECT_TRUE_MSG(!TemporalUpscalerNegotiatedExtentValid(
                      {0, 720}, display, TemporalUpscalerQuality::Quality),
                  "zero width is rejected");
  EXPECT_TRUE_MSG(!TemporalUpscalerNegotiatedExtentValid(
                      {1921, 1080}, display, TemporalUpscalerQuality::Quality),
                  "extent larger than display is rejected");
  EXPECT_TRUE_MSG(!TemporalUpscalerNegotiatedExtentValid(
                      {1280, 720}, display, TemporalUpscalerQuality::NativeAA),
                  "NativeAA must use display extent");
  EXPECT_TRUE_MSG(TemporalUpscalerNegotiatedExtentValid(
                      display, display, TemporalUpscalerQuality::NativeAA),
                  "NativeAA at display extent is valid");
}

UTEST(TemporalUpscalerPolicy, MaterialMipBias) {
  const TemporalUpscalerExtent display = {2560, 1440};
  const TemporalUpscalerExtent qualityRender = {1706, 960};
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1280, 720}, display, false),
                  0.0f, 0.0f,
                  "development render-scale half extent has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1920, 1080}, display, false),
                  0.0f, 0.0f,
                  "development render-scale three-quarter extent has no material mip bias");
  // Literal expectation, not a restatement of the implementation:
  // log2(1920 / 2560) - 1 = -1.4150375.
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1920, 1080}, display, true),
                  -1.41504f, 1e-4f,
                  "provider at the same extent as the development override is biased");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias(display, display, true), 0.0f,
                  0.0f, "NativeAA at display extent has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({3840, 2160}, display, true),
                  0.0f, 0.0f, "larger-than-display extent has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({0, 720}, display, true), 0.0f,
                  0.0f, "zero render width has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1280, 0}, display, true), 0.0f,
                  0.0f, "zero render height has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1280, 720}, {0, 1440}, true),
                  0.0f, 0.0f, "zero display width has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1280, 720}, {2560, 0}, true),
                  0.0f, 0.0f, "zero display height has no material mip bias");
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias({1280, 720}, display, true),
                  -2.0f, 0.0f, "half-width render extent has minus-two mip bias");
  // Literal expectation, not a restatement of the implementation:
  // log2(1706 / 2560) - 1 = -1.5855262.
  EXPECT_NEAR_MSG(hpl::TemporalMaterialMipBias(qualityRender, display, true),
                  -1.58553f, 1e-4f,
                  "quality render extent has two-thirds mip bias");
}

UTEST(TemporalUpscalerPolicy, PreparationAndSteadyState) {
  const TemporalUpscalerExtent display = {1920, 1080};
  const TemporalUpscalerSettings settings = {
      TemporalUpscalerProvider::Fsr, TemporalUpscalerQuality::Quality};
  StubTemporalUpscaler adapter;
  DrivenViewport viewport;

  const PrepareResult first = Prepare(viewport, adapter, settings, display);
  ASSERT_TRUE_MSG(first.attempted && first.prepared && first.historyReset,
                  "first provider preparation claims and resets");
  EXPECT_TRUE_MSG(viewport.extent.owner == TemporalRenderExtentOwner::Provider &&
                      SameExtent(viewport.extent.extent, adapter.recommended),
                  "prepared provider owns negotiated extent");
  EXPECT_TRUE_MSG(viewport.jitterPhaseCount == adapter.phaseCount,
                  "prepared provider supplies jitter phase count");

  const TemporalRenderExtentDecision resolved =
      Resolve(viewport, 0.5f, display);
  EXPECT_TRUE_MSG(resolved.state.owner == TemporalRenderExtentOwner::Provider &&
                      resolved.devRenderScaleIgnored,
                  "provider wins over development scale");
  EXPECT_TRUE_MSG(!resolved.historyReset,
                  "unchanged provider claim does not reset history");

  const uint32_t prepareCalls = adapter.prepareCalls;
  const PrepareResult steady = Prepare(viewport, adapter, settings, display);
  const TemporalRenderExtentDecision steadyResolution =
      Resolve(viewport, 0.5f, display);
  const bool resolveOutput =
      adapter.RecordResolve(viewport.preparedRenderExtent, display);
  EXPECT_TRUE_MSG(steady.attempted && steady.prepared,
                  "steady provider preparation remains successful");
  EXPECT_TRUE_MSG(adapter.prepareCalls == prepareCalls + 1,
                  "steady adapter call is isolated per frame");
  EXPECT_TRUE_MSG(!steadyResolution.historyReset,
                  "repeated unchanged claim keeps history");
  EXPECT_TRUE_MSG(resolveOutput && adapter.resolveCalls == 1,
                  "stub resolve success is consumed");
  EXPECT_TRUE_MSG(adapter.GetJitterPhaseCount(viewport.preparedRenderExtent,
                                              display) == adapter.phaseCount,
                  "stub phase query uses prepared extents");
}

UTEST(TemporalUpscalerPolicy, PreparationFailureAndRetryKey) {
  const TemporalUpscalerExtent display = {1920, 1080};
  TemporalUpscalerSettings settings = {
      TemporalUpscalerProvider::Fsr, TemporalUpscalerQuality::Quality};
  StubTemporalUpscaler adapter;
  DrivenViewport viewport;

  const PrepareResult initial = Prepare(viewport, adapter, settings, display);
  ASSERT_TRUE_MSG(initial.attempted && initial.prepared,
                  "preparation-failure setup starts prepared");
  adapter.prepareSucceeds = false;
  const PrepareResult failed = Prepare(viewport, adapter, settings, display);
  const uint32_t callsAfterFailure = adapter.prepareCalls;
  const PrepareResult suppressed = Prepare(viewport, adapter, settings, display);
  EXPECT_TRUE_MSG(failed.attempted && !failed.prepared && failed.historyReset,
                  "preparation failure resets history");
  EXPECT_TRUE_MSG(viewport.extent.owner == TemporalRenderExtentOwner::Native &&
                      SameExtent(viewport.extent.extent, {}),
                  "failed preparation releases stale provider claim");
  EXPECT_TRUE_MSG(TemporalProviderFailureSuppressed(
                      viewport.failure,
                      {settings.provider, settings.quality, display}),
                  "failure key is latched");
  EXPECT_TRUE_MSG(!suppressed.attempted && adapter.prepareCalls == callsAfterFailure,
                  "unchanged failure key suppresses retry");

  adapter.prepareSucceeds = true;
  settings.quality = TemporalUpscalerQuality::Balanced;
  const PrepareResult qualityChanged =
      Prepare(viewport, adapter, settings, display);
  EXPECT_TRUE_MSG(qualityChanged.attempted && qualityChanged.prepared &&
                      qualityChanged.historyReset,
                  "quality change retries and resets");

  adapter.prepareSucceeds = false;
  const PrepareResult failedAgain = Prepare(viewport, adapter, settings, display);
  EXPECT_TRUE_MSG(failedAgain.attempted && !failedAgain.prepared,
                  "changed-quality failure is independently latched");

  adapter.prepareSucceeds = true;
  settings.provider = TemporalUpscalerProvider::XeSS;
  const PrepareResult providerChanged =
      Prepare(viewport, adapter, settings, display);
  EXPECT_TRUE_MSG(providerChanged.attempted && providerChanged.prepared &&
                      providerChanged.historyReset,
                  "provider change retries and resets");

  adapter.prepareSucceeds = false;
  const PrepareResult failedAtNewProvider =
      Prepare(viewport, adapter, settings, display);
  adapter.prepareSucceeds = true;
  const TemporalUpscalerExtent resizedDisplay = {2560, 1440};
  const PrepareResult displayChanged =
      Prepare(viewport, adapter, settings, resizedDisplay);
  EXPECT_TRUE_MSG(failedAtNewProvider.attempted && !failedAtNewProvider.prepared,
                  "new provider failure is latched");
  EXPECT_TRUE_MSG(displayChanged.attempted && displayChanged.prepared &&
                      displayChanged.historyReset,
                  "display extent change retries and resets");
}

UTEST(TemporalUpscalerPolicy, ResolveFailureAndOwnershipTransitions) {
  const TemporalUpscalerExtent display = {1920, 1080};
  const TemporalUpscalerSettings settings = {
      TemporalUpscalerProvider::Fsr, TemporalUpscalerQuality::Quality};
  StubTemporalUpscaler adapter;
  DrivenViewport viewport;
  ASSERT_TRUE_MSG(Prepare(viewport, adapter, settings, display).prepared,
                  "resolve-failure setup prepares");

  adapter.resolveSucceeds = false;
  const bool failedResolve =
      adapter.RecordResolve(viewport.preparedRenderExtent, display);
  const TemporalRenderExtentDecision consumed =
      ConsumeTemporalProviderFailure(viewport.extent);
  ApplyExtentDecision(viewport, consumed);
  viewport.providerPrepared = false;
  const TemporalRenderExtentDecision overrideDecision =
      Resolve(viewport, 0.5f, display);
  EXPECT_TRUE_MSG(!failedResolve && adapter.resolveCalls == 1,
                  "stub resolve failure is observed");
  EXPECT_TRUE_MSG(consumed.state.owner == TemporalRenderExtentOwner::Native &&
                      SameExtent(consumed.state.extent, {}) && consumed.historyReset,
                  "resolve failure clears ownership to native with reset");
  EXPECT_TRUE_MSG(overrideDecision.state.owner ==
                      TemporalRenderExtentOwner::DevRenderScale &&
                      SameExtent(overrideDecision.state.extent, {960, 540}),
                  "development scale reclaims after resolve failure");
  EXPECT_TRUE_MSG(overrideDecision.historyReset,
                  "same-evaluation override carries the temporal reset");

  DrivenViewport nativeAaViewport;
  adapter.resolveSucceeds = true;
  const TemporalUpscalerSettings nativeAa = {
      TemporalUpscalerProvider::Fsr, TemporalUpscalerQuality::NativeAA};
  ASSERT_TRUE_MSG(Prepare(nativeAaViewport, adapter, nativeAa, display).prepared,
                  "NativeAA preparation succeeds");
  const TemporalRenderExtentDecision nativeAaDecision =
      Resolve(nativeAaViewport, 0.5f, display);
  EXPECT_TRUE_MSG(nativeAaDecision.state.owner == TemporalRenderExtentOwner::Provider &&
                      SameExtent(nativeAaDecision.state.extent, display),
                  "NativeAA provider owns display extent");
  EXPECT_TRUE_MSG(nativeAaDecision.devRenderScaleIgnored,
                  "NativeAA still wins over development scale");

  const TemporalUpscalerSettings off = {
      TemporalUpscalerProvider::Off, TemporalUpscalerQuality::Quality};
  const PrepareResult released =
      Prepare(nativeAaViewport, adapter, off, display);
  const bool releasedToNative =
      nativeAaViewport.extent.owner == TemporalRenderExtentOwner::Native &&
      SameExtent(nativeAaViewport.extent.extent, {});
  const TemporalRenderExtentDecision offDecision =
      Resolve(nativeAaViewport, 0.5f, display);
  EXPECT_TRUE_MSG(!released.attempted && !released.prepared && releasedToNative,
                  "Off teardown restores native sentinel");
  EXPECT_TRUE_MSG(offDecision.state.owner == TemporalRenderExtentOwner::DevRenderScale,
                  "development scale takes over after provider release");
}

UTEST(TemporalUpscalerPolicy, HistoryResetTriggers) {
  TemporalHistoryResetTriggers triggers;
  EXPECT_TRUE_MSG(!TemporalHistoryResetRequired(triggers),
                  "ordinary frame keeps history");
  EXPECT_TRUE_MSG(TemporalHistoryResetRequired(
                      TemporalHistoryResetTriggers{true}),
                  "first frame resets history");

  bool *resetCauses[] = {
      &triggers.inputExtentChanged,   &triggers.outputExtentChanged,
      &triggers.providerChanged,      &triggers.qualityChanged,
      &triggers.worldReplaced,        &triggers.cameraReplaced,
      &triggers.missedRenderedFrames, &triggers.cameraCut,
      &triggers.providerFailure};
  const char *names[] = {
      "input extent change resets history",
      "output extent change resets history",
      "provider change resets history",
      "quality change resets history",
      "world replacement resets history",
      "camera replacement resets history",
      "missed rendered frames reset history",
      "camera cut resets history",
      "provider failure resets history"};
  for (uint32_t i = 0; i < sizeof(resetCauses) / sizeof(resetCauses[0]); ++i) {
    *resetCauses[i] = true;
    EXPECT_TRUE_MSG(TemporalHistoryResetRequired(triggers), names[i]);
    *resetCauses[i] = false;
  }
  triggers.ordinaryCameraMovement = true;
  EXPECT_TRUE_MSG(!TemporalHistoryResetRequired(triggers),
                  "ordinary camera movement does not reset history");
  triggers.ordinaryCameraMovement = false;
  triggers.haltonSampleChanged = true;
  EXPECT_TRUE_MSG(!TemporalHistoryResetRequired(triggers),
                  "changing Halton sample does not reset history");
}

UTEST(TemporalUpscalerPolicy, IndependentViewports) {
  const TemporalUpscalerExtent display = {1920, 1080};
  const TemporalUpscalerSettings settings = {
      TemporalUpscalerProvider::Fsr, TemporalUpscalerQuality::Quality};
  StubTemporalUpscaler firstAdapter;
  StubTemporalUpscaler secondAdapter;
  DrivenViewport first;
  DrivenViewport second;
  firstAdapter.prepareSucceeds = false;

  const PrepareResult firstFailure =
      Prepare(first, firstAdapter, settings, display);
  const PrepareResult secondSuccess =
      Prepare(second, secondAdapter, settings, display);
  EXPECT_TRUE_MSG(firstFailure.attempted && !firstFailure.prepared,
                  "first viewport failure is isolated");
  EXPECT_TRUE_MSG(secondSuccess.prepared &&
                      second.extent.owner == TemporalRenderExtentOwner::Provider,
                  "second viewport retains provider ownership");

  const uint32_t firstCalls = firstAdapter.prepareCalls;
  TemporalUpscalerSettings secondSettings = settings;
  secondSettings.quality = TemporalUpscalerQuality::Performance;
  const PrepareResult secondChanged =
      Prepare(second, secondAdapter, secondSettings, display);
  const PrepareResult firstStillSuppressed =
      Prepare(first, firstAdapter, settings, display);
  EXPECT_TRUE_MSG(secondChanged.attempted && secondChanged.prepared &&
                      secondChanged.historyReset,
                  "second viewport settings change resets independently");
  EXPECT_TRUE_MSG(!firstStillSuppressed.attempted &&
                      firstAdapter.prepareCalls == firstCalls,
                  "first viewport failure latch is unaffected");
}
