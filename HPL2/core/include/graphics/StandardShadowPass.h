#pragma once

#include "graphics/RIProgram.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include "graphics/RICommand.h"
#include "graphics/Graphics.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace hpl {
class cResources;

// Depth-only shadow raster pass for the Standard shadow atlas. Spot tiles and
// point-light cube-face tiles are packed into square pages of an array image
// owned by the caller (one layer per page), so a Draw owns all image/view
// lifetime until the graphics defer queue retires it; this is safe for panes
// and in-flight frames. Each page is cleared and all of its tiles are drawn
// inside one rendering scope.
class cStandardShadowPass {
public:
  // Shadow pages carry no stencil.
  static constexpr RI_Format_e kAtlasFormat = RI_FORMAT_D32_SFLOAT;
  static constexpr uint32_t kMaxAtlasSize = 8192;

  struct Tile {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t size = 0;
    size_t indirectOffset = 0;
    // Upper bound on the draws this tile can issue: the number of candidates
    // the cull kernel was given, which is also the number of indirect elements
    // reserved for it. The actual count is decided on the GPU and read from
    // drawCountOffset, so the survivor count never has to come back to the CPU.
    // When the device has no drawIndirectCount this doubles as the literal draw
    // count, with culled entries carrying instanceCount = 0.
    uint32_t maxDrawCount = 0;
    size_t drawCountOffset = 0;
    uint32_t variabilityMask = 0;
    float slopeScaleBias = 0.0f;
    float viewProjection[16] = {};
  };

  cStandardShadowPass(cGraphics *graphics, cResources *resources);
  ~cStandardShadowPass();
  bool LoadData();
  void DestroyData();
  // Clears page `layer` of `atlas` and rasterizes every tile into its rect.
  // All inputs are validated before anything is recorded; on success the page
  // is left in SHADER_RESOURCE.
  // `drawCounts` holds one GPU-written survivor count per tile, indexed by
  // Tile::drawCountOffset. Passing it selects vkCmdDrawIndirectCount; passing
  // null means the caller is supplying host counts, so every tile issues
  // maxDrawCount draws and the cull kernel must have run in its in-place mode
  // where culled entries carry instanceCount = 0. Deriving the choice from the
  // argument rather than latching a device capability keeps it impossible for
  // the kernel's mode and the draw call to disagree.
  bool RenderAtlas(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
                   RITexture *atlas, uint32_t layer, uint32_t atlasSize,
                   RIBuffer *indirect, RIBuffer *drawCounts,
                   std::span<const Tile> tiles,
                   RIProgram::DescriptorBinding frameBinding);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
