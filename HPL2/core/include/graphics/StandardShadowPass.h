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
    uint32_t drawCount = 0;
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
  bool RenderAtlas(cGraphics::FrameContext *frame, RICmd *cmd, uint32_t frameIndex,
                   RITexture *atlas, uint32_t layer, uint32_t atlasSize,
                   RIBuffer *indirect, std::span<const Tile> tiles,
                   RIProgram::DescriptorBinding frameBinding);

private:
  cGraphics *mpGraphics;
  cResources *mpResources;
  std::shared_ptr<RIProgram> m_program;
  bool m_loaded = false;
};
}
