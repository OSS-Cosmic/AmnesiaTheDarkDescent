#ifndef RI_PIPELINE_H
#define RI_PIPELINE_H

// Pipeline / draw-state enums, batched by use case.
//
// A leaf header by design: it includes neither RITypes.h (which includes THIS,
// so the dependency runs one way) nor RIPreamble.h. Plain enums have no
// layout/ODR exposure for the prelude to guard, and pulling it in would drag
// volk/VMA/d3d12/dxgi along -- staying backend-header-free is what lets
// RIPipelineDesc.h describe a pipeline in neither backend's vocabulary.
#include <stdint.h>

enum RITopology_e {
  RI_TOPOLOGY_POINT_LIST,
  RI_TOPOLOGY_LINE_LIST,
  RI_TOPOLOGY_LINE_STRIP,
  RI_TOPOLOGY_TRIANGLE_LIST,
  RI_TOPOLOGY_TRIANGLE_STRIP,
  RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_PATCH_LIST
};

// R - fragment's depth or stencil reference
// D - depth or stencil buffer
enum RICompareFunc_e {
  RI_COMPARE_NONE,         // test is disabled
  RI_COMPARE_ALWAYS,       // true
  RI_COMPARE_NEVER,        // false
  RI_COMPARE_EQUAL,        // R == D
  RI_COMPARE_NOT_EQUAL,    // R != D
  RI_COMPARE_LESS,         // R < D
  RI_COMPARE_LESS_EQUAL,   // R <= D
  RI_COMPARE_GREATER,      // R > D
  RI_COMPARE_GREATER_EQUAL // R >= D
};

enum RICullMode_e {
  RI_CULL_MODE_NONE = 0,
  RI_CULL_MODE_FRONT = 0x1,
  RI_CULL_MODE_BACK = 0x2,
  RI_CULL_MODE_BOTH = RI_CULL_MODE_FRONT | RI_CULL_MODE_BACK
};

enum RIIndexType_e { RI_INDEX_TYPE_16, RI_INDEX_TYPE_32 };

enum RIColorWriteMask_e {
  RI_COLOR_WRITE_NONE = 0,
  RI_COLOR_WRITE_R = 0x1,
  RI_COLOR_WRITE_G = 0x2,
  RI_COLOR_WRITE_B = 0x4,
  RI_COLOR_WRITE_A = 0x8,

  RI_COLOR_WRITE_RGB = RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B,

  RI_COLOR_WRITE_RGBA =
      RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B | RI_COLOR_WRITE_A
};

// S0 - source color 0
// S1 - source color 1
// D - destination color
// C - blend constants (RI exposes no setter for these yet)
enum RIBlendFactor_e {          // RGB                               ALPHA
  RI_BLEND_ZERO,                // 0                                 0
  RI_BLEND_ONE,                 // 1                                 1
  RI_BLEND_SRC_COLOR,           // S0.r, S0.g, S0.b                  S0.a
  RI_BLEND_ONE_MINUS_SRC_COLOR, // 1 - S0.r, 1 - S0.g, 1 - S0.b      1 - S0.a
  RI_BLEND_DST_COLOR,           // D.r, D.g, D.b                     D.a
  RI_BLEND_ONE_MINUS_DST_COLOR, // 1 - D.r, 1 - D.g, 1 - D.b         1 - D.a
  RI_BLEND_SRC_ALPHA,           // S0.a                              S0.a
  RI_BLEND_ONE_MINUS_SRC_ALPHA, // 1 - S0.a                          1 - S0.a
  RI_BLEND_DST_ALPHA,           // D.a                               D.a
  RI_BLEND_ONE_MINUS_DST_ALPHA, // 1 - D.a                           1 - D.a
  RI_BLEND_CONSTANT_COLOR,      // C.r, C.g, C.b                     C.a
  RI_BLEND_ONE_MINUS_CONSTANT_COLOR, // 1 - C.r, 1 - C.g, 1 - C.b         1 -
                                     // C.a
  RI_BLEND_CONSTANT_ALPHA,           // C.a                               C.a
  RI_BLEND_ONE_MINUS_CONSTANT_ALPHA, // 1 - C.a                           1 -
                                     // C.a
  RI_BLEND_SRC_ALPHA_SATURATE,       // min(S0.a, 1 - D.a)                1
  RI_BLEND_SRC1_COLOR,               // S1.r, S1.g, S1.b                  S1.a
  RI_BLEND_ONE_MINUS_SRC1_COLOR, // 1 - S1.r, 1 - S1.g, 1 - S1.b      1 - S1.a
  RI_BLEND_SRC1_ALPHA,           // S1.a                              S1.a
  RI_BLEND_ONE_MINUS_SRC1_ALPHA  // 1 - S1.a                          1 - S1.a
};

// S - source color
// D - destination color
enum RIBlendOp_e {              // RGB / ALPHA
  RI_BLEND_OP_ADD,              // S + D
  RI_BLEND_OP_SUBTRACT,         // S - D
  RI_BLEND_OP_REVERSE_SUBTRACT, // D - S
  RI_BLEND_OP_MIN,              // min(S, D)
  RI_BLEND_OP_MAX               // max(S, D)
};

// R - stencil reference, from RIDepthStencilDesc::stencilReference
// D - stencil buffer
enum RIStencilOp_e {
  RI_STENCIL_OP_KEEP,                // D
  RI_STENCIL_OP_ZERO,                // 0
  RI_STENCIL_OP_REPLACE,             // R
  RI_STENCIL_OP_INCREMENT_AND_CLAMP, // min(D + 1, maxValue)
  RI_STENCIL_OP_DECREMENT_AND_CLAMP, // max(D - 1, 0)
  RI_STENCIL_OP_INVERT,              // ~D
  RI_STENCIL_OP_INCREMENT_AND_WRAP,  // D + 1, wrapping to 0
  RI_STENCIL_OP_DECREMENT_AND_WRAP   // D - 1, wrapping to maxValue
};

// POINT is intentionally absent: D3D12 has no point fill mode
// (D3D12_FILL_MODE is SOLID or WIREFRAME only).
enum RIPolygonMode_e { RI_POLYGON_MODE_FILL, RI_POLYGON_MODE_LINE };

// Winding order that identifies a front-facing triangle.
enum RIFrontFace_e {
  RI_FRONT_FACE_COUNTER_CLOCKWISE,
  RI_FRONT_FACE_CLOCKWISE
};

// How often a vertex buffer binding advances.
enum RIVertexInputRate_e {
  RI_VERTEX_INPUT_RATE_VERTEX,  // once per vertex
  RI_VERTEX_INPUT_RATE_INSTANCE // once per instance
};

#endif // RI_PIPELINE_H
