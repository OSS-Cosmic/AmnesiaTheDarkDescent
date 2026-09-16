#pragma once

#include "graphics/Graphics.h"
#include "graphics/RICommand.h"
#include "graphics/VertexBuffer.h"

#include <cstdint>

namespace hpl {

// Binds the 5-stream layout DecalPipelineDesc declares, substituting the
// global single-vertex defaults for absent optional streams (mirrors the
// Hybrid renderer's helper, which is file-local there). Shared by the Standard
// renderer's mesh decals and its billboard halo occlusion queries.
inline bool BindMeshDecalStreams(RICmd *cmd, cGraphics *graphics, cVertexBuffer *vb,
                                 uint32_t *outPresentMask) {
  auto bufferOf = [vb](eVertexBufferElement type) -> RIBuffer * {
    const auto *element = vb->GetElement(type);
    return element ? element->GetBuffer() : nullptr;
  };
  RIBuffer *position = bufferOf(eVertexBufferElement_Position);
  RIBuffer *index = vb->GetIndexRIBuffer();
  if (!position || !index)
    return false;
  RIBuffer *normal = bufferOf(eVertexBufferElement_Normal);
  RIBuffer *tangent = bufferOf(eVertexBufferElement_Texture1Tangent);
  RIBuffer *color = bufferOf(eVertexBufferElement_Color0);
  RIBuffer *uv = bufferOf(eVertexBufferElement_Texture0);
  uint32_t mask = eVertexElementFlag_Position;
  if (normal) mask |= eVertexElementFlag_Normal;
  if (tangent) mask |= eVertexElementFlag_Texture1;
  if (color) mask |= eVertexElementFlag_Color0;
  if (uv) mask |= eVertexElementFlag_Texture0;
  *outPresentMask = mask;
  RIBuffer *streams[5] = {
      position,
      normal ? normal : &graphics->fallbackNormalVertex,
      tangent ? tangent : &graphics->fallbackTangentVertex,
      color ? color : &graphics->fallbackColorVertex,
      uv ? uv : &graphics->fallbackUv0Vertex};
  cmd->bindVertexBuffers<5>(0, 5, streams);
  cmd->bindIndexBuffer(&graphics->device, index, 0, RI_INDEX_TYPE_32);
  return true;
}

} // namespace hpl
