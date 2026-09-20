#ifndef HPL_POSTEFFECT_HELPERS_H
#define HPL_POSTEFFECT_HELPERS_H

#include "graphics/RITypes.h"

#include "graphics/RIPipelineDesc.h" // RIGraphicsPipelineDesc
#include "graphics/RIPreamble.h"

#include <cstdint>

namespace hpl {

// Color attachment + sampled scratch target owned by a single post-effect
// instance (e.g. Bloom's two quarter-res blur buffers, ImageTrail's
// accumulator). The sampled-image descriptor is produced on demand via
// descriptor() (cookie lives on the view).
//
// One texture, two views, as RI_PogoBufferInit does it: `view` is the sampled
// view behind descriptor(), `attachmentView` is the one to put in an
// RIRenderingAttachment. D3D12 requires an attachment's view to carry
// RI_VIEWTYPE_COLOR_ATTACHMENT and rejects a sampled view outright; Vulkan
// accepts a single VkImageView for both.
//
// Lifecycle: create via CreatePostEffectColorTarget, destroy via
// DestroyPostEffectColorTarget. Destruction is deferred to graphicsDefer, so
// it is safe to call mid-frame. The owner re-creates when the viewport
// dimensions change (compare against `width` / `height`).
struct PostEffectColorTarget {
    struct RITexture     texture {};
    struct RITextureView view {};
    struct RITextureView attachmentView {};
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;

    RIDescriptor descriptor() {
        return RIDescriptor::sampledImage(nullptr, &view);
    }
};

// Allocate `out` with the given dimensions / format and a usage of
// (SAMPLED | COLOR_ATTACHMENT | additionalUsage). additionalUsage covers
// uncommon needs (e.g. TRANSFER_SRC for the ImageTrail accumulator).
// Destroy via DestroyPostEffectColorTarget before exit; destruction is
// deferred to graphicsDefer and is safe to call mid-frame.
void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, enum RI_Format_e format,
                                 uint32_t additionalUsage, // RITextureUsageBits_e
                                 const char *debugName);

void DestroyPostEffectColorTarget(PostEffectColorTarget &target);

// Pipeline state for a fullscreen post-effect pass: no vertex input, no
// depth/stencil, cull NONE, dynamic viewport+scissor, a single colour
// attachment at `colorFormat`. When `alphaBlend` is true the blend attachment
// is (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) for both colour and alpha; otherwise
// blending is disabled.
//
// Callers that need a depth-only or multi-target variant (TemporalPresentation's
// depth resolve) start from this and override `renderTarget` / `blendCount` /
// `depthStencil` on the returned value -- it is a plain value, so there is no
// lifetime coupling to respect.
RIGraphicsPipelineDesc MakePostEffectPipelineDesc(RI_Format_e colorFormat,
                                                  bool alphaBlend);

} // namespace hpl

#endif
