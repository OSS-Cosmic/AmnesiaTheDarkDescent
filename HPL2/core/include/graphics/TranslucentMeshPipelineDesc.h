#ifndef HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H
#define HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H

#include "graphics/RIFormat.h"       // RI_Format_e
#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc

#include <cstdint>

namespace hpl {

// Pipeline for the non-particle translucent (mesh) pass. Same shape as the
// particle desc, with two deliberate divergences:
//   - cullMode is BACK: meshes have orientation, unlike particle billboards
//     (which use NONE so either face draws).
//   - frontFace is CLOCKWISE to match MakeGBufferMRTPipelineDesc: both raster
//     passes run under the same Y-flipped viewport (negative height), so the
//     same mesh winding must declare the same front face across the opaque +
//     translucent passes.
// Depth is read-only <= unless the material disables testing (e.g. hand-held
// lantern halos). Unlike the particle/opaque pipelines this one uses
// traditional fixed-function vertex bindings (see Translucent.vert.slang).

// Enum-only carrier. The struct that used to own a VkGraphicsPipelineCreateInfo
// is gone -- the factory below returns a plain value instead -- but the name is
// kept so the ~20 `TranslucentMeshPipelineDesc::BLEND_*` references elsewhere
// stay put, and so the numeric values stay visibly pinned to the shader side.
struct TranslucentMeshPipelineDesc {
  // Values are mirrored by kBlendMode* in amnesia/slang/Constants.h and are
  // pushed to the shader as a constant -- do not renumber.
  //
  // Same layout as ParticlePipelineDesc::BlendMode so the
  // eMaterialBlendMode -> pipeline-variant remap from the particle path is
  // directly reusable.
  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    // Blend disabled: refractive draws compose against the scene copy in the
    // shader, as the legacy renderer did with eMaterialBlendMode_None.
    BLEND_REPLACE = 5,
    BLEND_LAST_ENUM = 6,
  };
};

// The five fixed-function vertex streams shared by the translucent and decal
// passes: position / normal / tangent / color / texcoord. Order and formats
// MUST match the input struct in Translucent.vert.slang (and Decal.vert.slang,
// which reuses the same layout).
//
// `vertexPresentMask` is a bitset of eVertexElementFlag_* naming the optional
// streams the renderable actually supplies. An absent stream gets its binding
// stride zeroed, so the bound single-vertex fallback buffer
// (cGraphics::fallback*Vertex) feeds its one default to every vertex. Position
// is always present. The strides are part of the desc, so RIProgram caches a
// distinct pipeline per presence combination without the caller hashing it.
RIVertexInputDesc MakeMeshVertexInputDesc(uint32_t vertexPresentMask);

RIGraphicsPipelineDesc MakeTranslucentMeshPipelineDesc(
    RI_Format_e colorFormat, RI_Format_e depthFormat,
    TranslucentMeshPipelineDesc::BlendMode mode, uint32_t vertexPresentMask,
    bool depthTest = true);

} // namespace hpl

#endif
