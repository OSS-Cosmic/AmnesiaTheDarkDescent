#pragma once

// Per-frame UBO binding for Slang shaders. Mirrors the GLSL declaration at
// amnesia/glsl/per_frame.resource.glsl::PerFrameBlock — same set / binding,
// same struct layout (SceneConstants is shared via SceneTypes.slang).
//
// Usage:
//   import PerFrame.resource;
//   ...
//   float3 camPos = gPerFrame.posW;
//
// Note: do NOT read camera position as `gPerFrame.invViewMat[3].xyz` in
// Slang/HLSL — `M[i]` returns row i, not column i (the GLSL convention),
// so for a standard math invView it resolves to (0,0,0,1). Use the
// dedicated posW field (or `invViewMat._m03_m13_m23` if a per-shot column
// extraction is unavoidable).

#include "../HostDefinitions.h"
import SceneTypes;

// The DXIL register is pinned because a D3D12 program's root signature is
// built by merging the reflections of its stages, and a resource must land on
// the same register in every one of them. Left to Slang, it does not: a
// fragment shader with push constants lowers gPushConstants to b0 and gets
// gPerFrame at b1, while the vertex shader it is paired with has no push
// constants and gets gPerFrame at b0. That is invisible while both entry
// points live in one source (Slang then numbers the whole unit consistently,
// as in Standard.particle.3d.slang), but the hybrid renderer splits vertex and
// fragment into separate files -- Particle.vert/frag and Translucent.vert/frag
// -- which are compiled independently and so disagree.
//
// b3 is above every register Slang currently auto-assigns in space0 (b0-b2),
// so it cannot collide. Vulkan is unaffected: vk::binding governs there, and
// register() is HLSL-only. The guard keeps it off the SPIR-V path entirely,
// since only the DXIL compile is given -DDXIL.
[vk::binding(0, 1)]
#ifdef DXIL
public ConstantBuffer<SceneConstants> gPerFrame : register(b3);
#else
public ConstantBuffer<SceneConstants> gPerFrame;
#endif
