#include "graphics/RIPogoBuffer.h"
#include "graphics/Graphics.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

#include <cstdio>

void RI_PogoBufferInit(struct RIDevice *device, struct RI_PogoBuffer *pogo,
                       uint32_t width, uint32_t height,
                       enum RI_Format_e format) {
  hpl::cGraphics *pGraphics = hpl::Interface<hpl::cGraphics>::Get();
  for (size_t p = 0; p < 2; p++) {
    if (!pogo->pogoView[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->pogoView[p]);
    if (!pogo->attachmentView[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->attachmentView[p]);
    if (!pogo->textures[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->textures[p]);
    pogo->pogoView[p] = {};
    pogo->attachmentView[p] = {};
    pogo->textures[p] = {};
  }
  pogo->attachmentIndex = 0;

  RITextureDesc desc = {};
  desc.type = RI_TEXTURE_2D;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  // STORAGE lets MainCompositePass (a compute pass) write the pogo attach
  // via an RWTexture2D; still color-attachment + sampled for the raster passes
  // and post-effect chain. TRANSFER_SRC/DST back the guard-band crop blit
  // (overscan render-pogo center → authored pogo) at the end of Draw.
  desc.usage = RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
               RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_TRANSFER_SRC |
               RI_USAGE_TRANSFER_DST;

  for (size_t p = 0; p < 2; p++) {
    pogo->textures[p] =
        RISharedPointer<RITexture>(device, RITexture::create(device, desc));
    if (!pogo->textures[p].isEmpty()) {
      char debugName[32];
      snprintf(debugName, sizeof(debugName), "PogoBuffer.half[%zu]", p);
      pogo->textures[p]->setDebugObjectName(device, debugName);
    }

    RITextureViewDesc viewDesc = {};
    viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
    viewDesc.format = format;
    viewDesc.mipNum = 1;
    viewDesc.layerNum = 1;
    pogo->pogoView[p] = RISharedPointer<RITextureView>(
        device,
        RITextureView::create(device, pogo->textures[p].Get(), viewDesc));

    viewDesc.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
    pogo->attachmentView[p] = RISharedPointer<RITextureView>(
        device,
        RITextureView::create(device, pogo->textures[p].Get(), viewDesc));
  }
}

void RI_PogoBufferDestroy(struct RIDevice *device, struct RI_PogoBuffer *pogo) {
  hpl::cGraphics *pGraphics = hpl::Interface<hpl::cGraphics>::Get();
  for (size_t p = 0; p < 2; p++) {
    if (!pogo->pogoView[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->pogoView[p]);
    if (!pogo->attachmentView[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->attachmentView[p]);
    if (!pogo->textures[p].isEmpty())
      pGraphics->graphicsDefer.push(pogo->textures[p]);
    pogo->pogoView[p] = {};
    pogo->attachmentView[p] = {};
    pogo->textures[p] = {};
  }
}

void RI_PogoBufferToggle(struct RIDevice *device, struct RI_PogoBuffer *pogo,
                         struct RICmd *handle) {
  struct RITextureBarrier barriers[2] = {};
  barriers[0] =
      RI_PogoShaderBarrier(pogo->textures[pogo->attachmentIndex].Get(), false);
  pogo->attachmentIndex = ((pogo->attachmentIndex + 1) % 2);
  barriers[1] = RI_PogoAttachmentBarrier(
      pogo->textures[pogo->attachmentIndex].Get(), false);
  handle->vk_d3d12_textureBarriers<2>(2, barriers);
}
