#ifndef HPL_DECAL_PIPELINE_DESC_H
#define HPL_DECAL_PIPELINE_DESC_H

#include "graphics/RIFormat.h"       // RI_Format_e
#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc

#include <cstdint>

namespace hpl {

// Pipeline for the decal overlay pass. Structurally identical to
// MakeTranslucentMeshPipelineDesc (it shares MakeMeshVertexInputDesc, depth
// read-only <=, hardware blend per BlendMode, Y-flipped viewport so the
// CLOCKWISE front face matches the GBuffer) with two divergences:
//   - cullMode is NONE: decals are thin clipped meshes that can be
//     single-sided with arbitrary winding, so both faces must draw to
//     guarantee visibility. (Tighten to BACK only if z-fighting appears.)
//   - the write mask is RGB only, and the MUL/MULX2 alpha factors differ.
// The decal fragment shader emits raw diffuse x vertex colour and relies on
// the hardware blend state, matching Decal.frag.slang.

// Enum-only carrier; see the note on TranslucentMeshPipelineDesc.
struct DecalPipelineDesc {
  // Values are mirrored by kBlendMode* in amnesia/slang/Constants.h -- do not
  // renumber. Same layout as TranslucentMeshPipelineDesc::BlendMode so the
  // eMaterialBlendMode -> pipeline-variant remap is directly reusable.
  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };
};

// `vertexPresentMask` names the optional streams the renderable supplies; see
// MakeMeshVertexInputDesc in TranslucentMeshPipelineDesc.h.
RIGraphicsPipelineDesc MakeDecalPipelineDesc(RI_Format_e colorFormat,
                                             RI_Format_e depthFormat,
                                             DecalPipelineDesc::BlendMode mode,
                                             uint32_t vertexPresentMask);

} // namespace hpl

#endif
