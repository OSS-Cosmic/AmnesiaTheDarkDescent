#include "../../amnesia/slang/LightChange.h"
#include "utest.h"

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace {

// Mirrors World.cpp StampLightChange for one slot across frames: returns the
// (prevPower, prevRadius) the light is stamped with this frame.
struct Slot {
  float prevPower = 0.0f;
  float prevRadius = 0.0f;
  std::pair<float, float> Step(float maxChannel, float intensity, float radius) {
    const std::pair<float, float> stamped{prevPower, prevRadius};
    prevPower = hpl::lightChangePower(maxChannel, intensity, radius);
    prevRadius = radius;
    return stamped;
  }
};

// Mirrors LightGridBuildPass CellChange for a cell holding the given
// (weight, prevWeight) pairs.
float CellFraction(std::initializer_list<std::pair<float, float>> lights) {
  float sumDelta = 0.0f, sumCur = 0.0f, sumPrev = 0.0f;
  for (const auto &[w, prev] : lights) {
    sumDelta += w > prev ? w - prev : prev - w;
    sumCur += w;
    sumPrev += prev;
  }
  return hpl::lightChangeFraction(sumDelta, sumCur, sumPrev);
}

} // namespace

UTEST(LightChange, SteadyLightKeepsItsPower) {
  Slot slot;
  slot.Step(1.0f, 4.0f, 2.0f);
  const auto stamped = slot.Step(1.0f, 4.0f, 2.0f);
  EXPECT_NEAR(stamped.first, 4.0f, 1e-6f);
  EXPECT_NEAR(stamped.second, 2.0f, 1e-6f);
}

UTEST(LightChange, NewlyLitSlotHasNoPrevious) {
  // First frame for a slot (or a slot that held a hole) has no prior power.
  Slot slot;
  const auto stamped = slot.Step(1.0f, 4.0f, 2.0f);
  EXPECT_EQ(stamped.first, 0.0f);
  EXPECT_EQ(stamped.second, 0.0f);
}

UTEST(LightChange, SwitchOffKeepsLastFramesPower) {
  // Off: intensity 0 makes the light invisible, which uploads radius 0. The
  // grid still needs last frame's power and reach to see the loss.
  Slot slot;
  slot.Step(1.0f, 4.0f, 2.0f);
  const auto off = slot.Step(1.0f, 0.0f, 0.0f);
  EXPECT_NEAR(off.first, 4.0f, 1e-6f);
  EXPECT_NEAR(off.second, 2.0f, 1e-6f);
  const auto back = slot.Step(1.0f, 4.0f, 2.0f);
  EXPECT_EQ(back.first, 0.0f);
}

UTEST(LightChange, FadeToZeroKeepsLastFramesPower) {
  // A fade can reach intensity 0 with the radius still set.
  Slot slot;
  slot.Step(1.0f, 4.0f, 2.0f);
  const auto faded = slot.Step(1.0f, 0.0f, 2.0f);
  EXPECT_NEAR(faded.first, 4.0f, 1e-6f);
  EXPECT_EQ(hpl::lightChangePower(1.0f, 0.0f, 2.0f), 0.0f);
}

UTEST(LightChange, CellFraction) {
  EXPECT_EQ(CellFraction({}), 0.0f);
  EXPECT_NEAR(CellFraction({{3.0f, 3.0f}, {1.0f, 1.0f}}), 0.0f, 1e-6f);
  // The only light just switched on: all of the cell's lighting is new.
  EXPECT_NEAR(CellFraction({{2.0f, 0.0f}}), 1.0f, 1e-6f);
  // ...or just switched off: all of last frame's lighting is gone.
  EXPECT_NEAR(CellFraction({{0.0f, 2.0f}}), 1.0f, 1e-6f);
  // A dim newcomer next to a steady bright light barely moves the cell.
  EXPECT_NEAR(CellFraction({{9.0f, 9.0f}, {1.0f, 0.0f}}), 0.1f, 1e-6f);
  // A light at half its previous power: |1 - 2| / 2.
  EXPECT_NEAR(CellFraction({{1.0f, 2.0f}}), 0.5f, 1e-6f);
}

UTEST(LightChange, GridCountPacking) {
  for (uint32_t count = 0; count <= 8; ++count) {
    for (float f : {0.0f, 0.25f, 0.5f, 1.0f}) {
      const uint32_t packed = hpl::lightGridPackCount(count, f);
      EXPECT_EQ(hpl::lightGridUnpackCount(packed), count);
      EXPECT_NEAR(hpl::lightGridUnpackChange(packed), f, 1.0f / 255.0f);
    }
  }
  // Out-of-range fractions clamp rather than bleeding into other bits.
  EXPECT_EQ(hpl::lightGridUnpackCount(hpl::lightGridPackCount(3, 7.0f)), 3u);
  EXPECT_EQ(hpl::lightGridUnpackChange(hpl::lightGridPackCount(3, 7.0f)), 1.0f);
  EXPECT_EQ(hpl::lightGridUnpackChange(hpl::lightGridPackCount(3, -1.0f)), 0.0f);
}

UTEST(LightChange, HistoryClamp) {
  EXPECT_NEAR(hpl::lightChangeHistoryClamp(0.0f, 30.0f, 0.5f), 30.0f, 1e-5f);
  EXPECT_NEAR(hpl::lightChangeHistoryClamp(0.25f, 30.0f, 0.5f), 15.5f, 1e-5f);
  EXPECT_NEAR(hpl::lightChangeHistoryClamp(0.5f, 30.0f, 0.5f), 1.0f, 1e-5f);
  EXPECT_NEAR(hpl::lightChangeHistoryClamp(1.0f, 30.0f, 0.5f), 1.0f, 1e-5f);
}

UTEST(LightChange, HistoryConfidence) {
  EXPECT_NEAR(hpl::lightChangeHistoryConfidence(0.0f, 0.5f, 0.1f), 1.0f, 1e-6f);
  EXPECT_NEAR(hpl::lightChangeHistoryConfidence(0.25f, 0.5f, 0.1f), 0.55f, 1e-6f);
  EXPECT_NEAR(hpl::lightChangeHistoryConfidence(0.5f, 0.5f, 0.1f), 0.1f, 1e-6f);
  EXPECT_NEAR(hpl::lightChangeHistoryConfidence(1.0f, 0.5f, 0.1f), 0.1f, 1e-6f);
  // A zero reset fraction drops straight to the floor.
  EXPECT_NEAR(hpl::lightChangeHistoryConfidence(0.0f, 0.0f, 0.1f), 0.1f, 1e-6f);
}
