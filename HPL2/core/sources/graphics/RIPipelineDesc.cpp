#include "graphics/RIPipelineDesc.h"

namespace hpl {

namespace {

hash_t hashBool(hash_t hash, bool value) {
  return hash_u32(hash, value ? 1u : 0u);
}

hash_t hashVertexInput(hash_t hash, const RIVertexInputDesc &vi) {
  hash = hash_u32(hash, vi.bindingCount);
  for (uint32_t i = 0; i < vi.bindingCount; ++i) {
    const RIVertexBindingDesc &b = vi.bindings[i];
    hash = hash_u32(hash, b.binding);
    hash = hash_u32(hash, b.stride);
    hash = hash_u32(hash, static_cast<uint32_t>(b.inputRate));
  }
  hash = hash_u32(hash, vi.attributeCount);
  for (uint32_t i = 0; i < vi.attributeCount; ++i) {
    const RIVertexAttributeDesc &a = vi.attributes[i];
    hash = hash_u32(hash, a.location);
    hash = hash_u32(hash, a.binding);
    hash = hash_u32(hash, a.offset);
    hash = hash_u32(hash, static_cast<uint32_t>(a.format));
  }
  return hash;
}

hash_t hashStencilFace(hash_t hash, const RIStencilFaceDesc &face) {
  hash = hash_u32(hash, static_cast<uint32_t>(face.failOp));
  hash = hash_u32(hash, static_cast<uint32_t>(face.passOp));
  hash = hash_u32(hash, static_cast<uint32_t>(face.depthFailOp));
  hash = hash_u32(hash, static_cast<uint32_t>(face.compareFunc));
  hash = hash_u32(hash, face.compareMask);
  hash = hash_u32(hash, face.writeMask);
  return hash;
}

} // namespace

hash_t RIHashGraphicsPipelineDesc(hash_t seed,
                                  const RIGraphicsPipelineDesc &desc) {
  // Walks the fields rather than hashing the object's bytes: the struct has
  // padding, and the trailing entries of the fixed-capacity arrays are not
  // part of the state when the matching count is lower.
  hash_t hash = hashVertexInput(seed, desc.vertexInput);

  hash = hash_u32(hash, static_cast<uint32_t>(desc.topology));
  hash = hashBool(hash, desc.primitiveRestart);

  const RIRasterizationDesc &rs = desc.raster;
  hash = hash_u32(hash, static_cast<uint32_t>(rs.polygonMode));
  hash = hash_u32(hash, static_cast<uint32_t>(rs.cullMode));
  hash = hash_u32(hash, static_cast<uint32_t>(rs.frontFace));
  hash = hashBool(hash, rs.depthClamp);
  hash = hashBool(hash, rs.depthBiasEnable);
  hash = hash_f32(hash, rs.depthBiasConstant);
  hash = hash_f32(hash, rs.depthBiasClamp);
  hash = hash_f32(hash, rs.depthBiasSlope);
  hash = hash_f32(hash, rs.lineWidth);

  hash = hash_u32(hash, desc.sampleCount);
  hash = hashBool(hash, desc.alphaToCoverage);

  const RIDepthStencilDesc &ds = desc.depthStencil;
  hash = hashBool(hash, ds.depthTest);
  hash = hashBool(hash, ds.depthWrite);
  hash = hash_u32(hash, static_cast<uint32_t>(ds.depthCompare));
  hash = hashBool(hash, ds.stencilTest);
  hash = hash_u32(hash, ds.stencilReference);
  hash = hashStencilFace(hash, ds.front);
  hash = hashStencilFace(hash, ds.back);

  hash = hash_u32(hash, desc.blendCount);
  for (uint32_t i = 0; i < desc.blendCount; ++i) {
    const RIBlendAttachmentDesc &b = desc.blend[i];
    hash = hashBool(hash, b.blendEnable);
    hash = hash_u32(hash, static_cast<uint32_t>(b.srcColor));
    hash = hash_u32(hash, static_cast<uint32_t>(b.dstColor));
    hash = hash_u32(hash, static_cast<uint32_t>(b.colorOp));
    hash = hash_u32(hash, static_cast<uint32_t>(b.srcAlpha));
    hash = hash_u32(hash, static_cast<uint32_t>(b.dstAlpha));
    hash = hash_u32(hash, static_cast<uint32_t>(b.alphaOp));
    hash = hash_u32(hash, static_cast<uint32_t>(b.writeMask));
  }

  const RIRenderTargetDesc &rt = desc.renderTarget;
  hash = hash_u32(hash, rt.colorCount);
  for (uint32_t i = 0; i < rt.colorCount; ++i)
    hash = hash_u32(hash, static_cast<uint32_t>(rt.colorFormats[i]));
  hash = hash_u32(hash, static_cast<uint32_t>(rt.depthFormat));
  hash = hash_u32(hash, static_cast<uint32_t>(rt.stencilFormat));

  return hash;
}

} // namespace hpl
