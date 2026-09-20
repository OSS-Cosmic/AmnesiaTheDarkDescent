#ifndef HPL_PARTICLE_PIPELINE_DESC_H
#define HPL_PARTICLE_PIPELINE_DESC_H

#include "graphics/RIFormat.h"       // RI_Format_e
#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc

#include <cstdint>

namespace hpl {

// Pipeline for the particle (translucent) pass. One variant per blend mode --
// the hardware blend factors come from the legacy translucencyBlendTable
// mapping in RendererDeferred. Depth test is on but depth write is off so
// particles sort against opaque geometry without occluding each other in the
// wrong order. Cull mode is NONE because particle billboards may face the
// camera either way; the legacy renderer behaves the same. No vertex input
// bindings -- the VS pulls via BDA from opaque*Handles[].

// Enum-only carrier; see the note on TranslucentMeshPipelineDesc.
struct ParticlePipelineDesc {
  // Values are mirrored by kBlendMode* in amnesia/slang/Constants.h -- do not
  // renumber.
  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };
};

// depthTest=false mirrors materials with DepthTest="False" (torch halos,
// nodepth fog sprites): the sprite ignores scene depth entirely.
RIGraphicsPipelineDesc
MakeParticlePipelineDesc(RI_Format_e swapchainFormat, RI_Format_e depthFormat,
                         ParticlePipelineDesc::BlendMode mode,
                         bool depthTest = true);

} // namespace hpl

#endif
