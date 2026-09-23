// Device-free coverage for the backend-neutral pipeline description.
//
// The first job of this file is the #error block below: RIPipelineDesc.h
// exists so a pipeline can be described without either backend's vocabulary,
// and that property is invisible at runtime. It regresses the moment someone
// adds a convenience include, so it is asserted at compile time here rather
// than left to review.
#include "graphics/RIPipelineDesc.h"

#ifdef VK_HEADER_VERSION
#error "RIPipelineDesc.h must not pull in Vulkan headers"
#endif
#ifdef VULKAN_CORE_H_
#error "RIPipelineDesc.h must not pull in Vulkan headers"
#endif
#ifdef __ID3D12Device_INTERFACE_DEFINED__
#error "RIPipelineDesc.h must not pull in D3D12 headers"
#endif
#ifdef DXGI_FORMAT_DEFINED
#error "RIPipelineDesc.h must not pull in DXGI headers"
#endif

#include "utest.h"

using namespace hpl;

namespace {

hash_t Hash(const RIGraphicsPipelineDesc &desc) {
  return RIHashGraphicsPipelineDesc(HASH_INITIAL_VALUE, desc);
}

// A desc with every section populated, so the mutation tests below start from
// something that is not all-defaults.
RIGraphicsPipelineDesc Populated() {
  RIGraphicsPipelineDesc desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.vertexInput.bindingCount = 1;
  desc.vertexInput.bindings[0] = {0, 32, RI_VERTEX_INPUT_RATE_VERTEX};
  desc.vertexInput.attributeCount = 2;
  desc.vertexInput.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};
  desc.vertexInput.attributes[1] = {1, 0, RI_FORMAT_RG32_SFLOAT, 12};
  desc.raster.cullMode = RI_CULL_MODE_BACK;
  desc.depthStencil.depthTest = true;
  desc.depthStencil.depthWrite = true;
  desc.blendCount = 1;
  desc.blend[0].blendEnable = true;
  desc.blend[0].srcColor = RI_BLEND_SRC_ALPHA;
  desc.blend[0].dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = RI_FORMAT_RGBA16_SFLOAT;
  desc.renderTarget.depthFormat = RI_FORMAT_D32_SFLOAT;
  return desc;
}

} // namespace

UTEST(RIPipelineDesc, HashIsStableForEqualDescs) {
  EXPECT_EQ(Hash(Populated()), Hash(Populated()));

  const RIGraphicsPipelineDesc defaults = {};
  EXPECT_EQ(Hash(defaults), Hash(RIGraphicsPipelineDesc{}));
}

// Every field the hash skips is a pair of distinct pipelines that would share
// a cache slot and silently render with the wrong state, so each section gets
// its own mutation.
UTEST(RIPipelineDesc, HashDistinguishesEverySection) {
  const hash_t base = Hash(Populated());

  {
    RIGraphicsPipelineDesc d = Populated();
    d.topology = RI_TOPOLOGY_LINE_LIST;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.raster.cullMode = RI_CULL_MODE_FRONT;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.raster.depthBiasSlope = 1.5f;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.depthStencil.depthWrite = false;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.depthStencil.depthCompare = RI_COMPARE_GREATER;
    EXPECT_NE(base, Hash(d));
  }
  {
    // A stencil reference that did not reach the hash would let the outline
    // mark pass (REPLACE with 1) share a cache slot with a REPLACE-with-0
    // pipeline.
    RIGraphicsPipelineDesc d = Populated();
    d.depthStencil.stencilTest = true;
    d.depthStencil.stencilReference = 1;
    RIGraphicsPipelineDesc e = d;
    e.depthStencil.stencilReference = 0;
    EXPECT_NE(Hash(d), Hash(e));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.depthStencil.front.passOp = RI_STENCIL_OP_REPLACE;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.blend[0].dstColor = RI_BLEND_ONE;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.blend[0].writeMask = RI_COLOR_WRITE_RGB;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.renderTarget.colorFormats[0] = RI_FORMAT_RGBA8_UNORM;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.renderTarget.depthFormat = RI_FORMAT_D16_UNORM;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.sampleCount = 4;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.alphaToCoverage = true;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.vertexInput.bindings[0].stride = 24;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.vertexInput.attributes[1].offset = 16;
    EXPECT_NE(base, Hash(d));
  }
  {
    RIGraphicsPipelineDesc d = Populated();
    d.vertexInput.attributes[1].format = RI_FORMAT_RGBA8_UNORM;
    EXPECT_NE(base, Hash(d));
  }
}

// The hash walks only the live prefix of each fixed-capacity array, so two
// descs that agree on their counts must agree on their hash however the
// unused tail happens to be filled.
UTEST(RIPipelineDesc, HashIgnoresUnusedArrayTail) {
  RIGraphicsPipelineDesc a = Populated();
  RIGraphicsPipelineDesc b = Populated();

  b.renderTarget.colorFormats[1] = RI_FORMAT_RGBA8_UNORM;
  b.blend[1].blendEnable = true;
  b.blend[1].srcColor = RI_BLEND_DST_ALPHA;
  b.vertexInput.bindings[4].stride = 999;
  b.vertexInput.attributes[7].offset = 64;

  EXPECT_EQ(Hash(a), Hash(b));
}

// The desc replaced a set of non-copyable holder structs whose only purpose
// was keeping a Vulkan pointer chain alive. Copying must be safe and must not
// change identity.
UTEST(RIPipelineDesc, IsACopyableValue) {
  const RIGraphicsPipelineDesc original = Populated();
  RIGraphicsPipelineDesc copy = original;
  EXPECT_EQ(Hash(original), Hash(copy));

  copy.renderTarget.colorFormats[0] = RI_FORMAT_RGBA8_UNORM;
  EXPECT_NE(Hash(original), Hash(copy));
  EXPECT_EQ(RI_FORMAT_RGBA16_SFLOAT, original.renderTarget.colorFormats[0]);
}
