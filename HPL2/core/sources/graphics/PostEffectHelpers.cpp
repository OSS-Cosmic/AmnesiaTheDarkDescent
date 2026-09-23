#include "graphics/PostEffectHelpers.h"

#include "graphics/Graphics.h"
#include "graphics/RIRenderer.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RIVK.h"

#include <cstring>

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

namespace hpl {

void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, enum RI_Format_e format,
                                 uint32_t additionalUsage,
                                 const char *debugName,
                                 std::optional<RITextureClearValue> clearValue) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  out.width = width;
  out.height = height;
  out.valid = false;

  RITextureDesc desc = {};
  desc.type = RI_TEXTURE_2D;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.usage =
      RI_USAGE_SHADER_RESOURCE | RI_USAGE_COLOR_ATTACHMENT | additionalUsage;
  desc.clearValue = clearValue;
  out.texture = RITexture::create(&pGraphics->device, desc);
  if (out.texture.isEmpty())
    return;

  RITextureViewDesc viewDesc = {};
  viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
  viewDesc.format = format;
  viewDesc.mipNum = 1;
  viewDesc.layerNum = 1;
  out.view = RITextureView::create(&pGraphics->device, &out.texture, viewDesc);

  // Separate attachment view: D3D12 binds a render target through an RTV and
  // rejects a sampled view (RI_VIEWTYPE_COLOR_ATTACHMENT is checked in
  // vk_d3d12_beginRendering / ri_d3d12_makeRTV).
  viewDesc.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
  out.attachmentView =
      RITextureView::create(&pGraphics->device, &out.texture, viewDesc);

  if (out.view.isEmpty() || out.attachmentView.isEmpty()) {
    Error("CreatePostEffectColorTarget: failed to create %s views for '%s' "
          "(%ux%u)\n",
          out.view.isEmpty() ? "sampled" : "attachment",
          debugName ? debugName : "<unnamed>", width, height);
    DestroyPostEffectColorTarget(out);
    return;
  }

  if (debugName)
    out.texture.setDebugObjectName(&pGraphics->device, debugName);

  out.valid = true;
}

void DestroyPostEffectColorTarget(PostEffectColorTarget &target) {
  cGraphics *pGraphics = Interface<cGraphics>::Get();
  // Backend-neutral: FrameDeferral is sealed and drained on both the Vulkan
  // and D3D12 frame paths, so the release lands once the GPU has retired the
  // frames that still reference these views.
  pGraphics->graphicsDefer.push(
      RISharedPointer<RITextureView>(&pGraphics->device, target.view));
  pGraphics->graphicsDefer.push(RISharedPointer<RITextureView>(
      &pGraphics->device, target.attachmentView));
  pGraphics->graphicsDefer.push(
      RISharedPointer<RITexture>(&pGraphics->device, target.texture));
  target.view = RITextureView{};
  target.attachmentView = RITextureView{};
  target.texture = RITexture{};
  target.valid = false;
  target.width = target.height = 0;
}

RIGraphicsPipelineDesc MakePostEffectPipelineDesc(RI_Format_e colorFormat,
                                                  bool alphaBlend) {
  RIGraphicsPipelineDesc desc = {};
  // Fullscreen triangle from SV_VertexID -- no vertex input.
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

  desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
  desc.raster.cullMode = RI_CULL_MODE_NONE;
  // Inert with culling off, but spelled out because the Vulkan struct this
  // replaced left it zero-initialized (COUNTER_CLOCKWISE) rather than taking
  // the RI default.
  desc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;

  // Depth/stencil entirely off; the RI defaults already say so.

  desc.renderTarget.colorCount = 1;
  desc.renderTarget.colorFormats[0] = colorFormat;

  desc.blendCount = 1;
  RIBlendAttachmentDesc &blend = desc.blend[0];
  blend.writeMask = RI_COLOR_WRITE_RGBA;
  if (alphaBlend) {
    blend.blendEnable = true;
    blend.colorOp = RI_BLEND_OP_ADD;
    blend.alphaOp = RI_BLEND_OP_ADD;
    blend.srcColor = RI_BLEND_SRC_ALPHA;
    blend.dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.srcAlpha = RI_BLEND_SRC_ALPHA;
    blend.dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
  }
  return desc;
}

} // namespace hpl
