#pragma once

#include "graphics/Graphics.h"
#include "graphics/HiZPyramid.h"
#include "graphics/RIProgram.h"

#include <memory>

namespace hpl {
class cResources;

// Builds the camera depth pyramid the occlusion cull tests against.
//
// Level 0 is half the render extent; every further level is a 2x2 MAX reduce of
// the one before, so each texel holds the FARTHEST depth in its footprint. That
// operator is what makes the cull a proof rather than a guess -- see the header
// comment in Standard.hiz.cs.slang.
//
// The whole build runs with the pyramid in GENERAL layout: each dispatch reads
// the level above through a whole-chain SRV and writes one level through a
// single-level storage view, and one image cannot be SHADER_READ_ONLY and
// STORAGE at once. Build() leaves it in SHADER_RESOURCE for the cull.
class cStandardHiZPass {
public:
  cStandardHiZPass(cGraphics *graphics, cResources *resources);
  ~cStandardHiZPass();

  bool LoadData();
  void DestroyData();
  bool IsLoaded() const { return m_loaded && m_program != nullptr; }

  // Records the full chain. `depth` is the scene depth as a shader resource;
  // the caller owns the barrier that put it there, and sourceWidth/Height are
  // that image's extent, not the pyramid's. Returns false without recording
  // anything when the caller has no pyramid for this image.
  bool Build(RICmd *cmd, uint32_t frameIndex, HiZPyramid &pyramid,
             uint32_t image, uint32_t sourceWidth, uint32_t sourceHeight,
             RITextureView *depth);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
} // namespace hpl
