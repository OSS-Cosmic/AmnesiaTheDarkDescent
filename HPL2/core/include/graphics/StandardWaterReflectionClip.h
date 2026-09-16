#pragma once

#include "math/MathTypes.h"

#include <array>
#include <cstdint>
#include <span>

namespace hpl {
class cFrustum;
class cBoundingVolume;

// Bounds for one planar water reflection capture: the half-spaces the capture
// culls against, plus the scissor that keeps its fill inside the water
// surface's screen footprint.
//
// This is cRendererDeferred::RenderSubMeshEntityReflection's occlusion-plane
// and scissor setup, expressed as pure math so it can be exercised without a
// GPU. The legacy pass pushed the same planes into
// cRenderSettings::mvOcclusionPlanes, where CheckObjectIsVisible consumed them;
// here they go to rendering::WalkAndPrepareRenderList instead.
struct StandardWaterReflectionClip {
  // [0] is the max-reflection-distance plane when present, the rest are the
  // screen-rect bounds. Order does not matter to the caller; count does.
  std::array<cPlanef, 5> planes{};
  uint32_t planeCount = 0;
  cRect2l scissor{};
  bool hasScissor = false;
  // True when the water surface itself is beyond the distance plane, i.e. the
  // reflection would be faded out entirely. The legacy renderer skipped the
  // whole capture in that case.
  bool surfaceOutOfRange = false;

  std::span<cPlanef> Planes() { return {planes.data(), planeCount}; }
};

// surfacePlane is the water plane already oriented toward the main camera.
// maxReflectionDistance <= 0 skips the distance plane; it is the authored
// ReflectionFadeEnd, which is exactly what the legacy material exposed as
// GetMaxReflectionDistance. clipScreenRect mirrors
// cRenderSettings::mbClipReflectionScreenRect. reflectionExtent is the capture
// target's size, i.e. the half-resolution extent, not the main viewport's.
StandardWaterReflectionClip BuildStandardWaterReflectionClip(
    cFrustum *mainFrustum, cFrustum *reflectedFrustum,
    cBoundingVolume &surfaceBounds, const cPlanef &surfacePlane,
    float maxReflectionDistance, bool clipScreenRect,
    const cVector2l &reflectionExtent);
} // namespace hpl
