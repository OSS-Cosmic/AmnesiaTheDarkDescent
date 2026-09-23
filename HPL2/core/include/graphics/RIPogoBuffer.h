#ifndef RI_POGO_BUFFER_H
#define RI_POGO_BUFFER_H

#include "RIFormat.h"
#include "RITypes.h"

struct FrameState_s;

struct RI_PogoBuffer {
	RISharedPointer<RITexture> textures[2];
	// Sampled (shader-resource) view of each half.
	RISharedPointer<RITextureView> pogoView[2];
	// Color-attachment view of each half. D3D12 needs an RTV-type view to
	// render into; the sampled view is rejected by beginRendering.
	RISharedPointer<RITextureView> attachmentView[2];
	uint16_t attachmentIndex;
};

// Hand-off transition: pogo attachment -> fragment-sampled resource.
// initial=true treats the contents as discarded (UNDEFINED source).
static inline struct RITextureBarrier RI_PogoShaderBarrier( struct RITexture *texture, bool initial )
{
	struct RITextureBarrier barrier = {};
	barrier.texture = texture;
	barrier.before = initial ? RI_RESOURCE_STATE_UNDEFINED : RI_RESOURCE_STATE_RENDER_TARGET;
	barrier.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
	barrier.afterStages = RI_STAGE_FRAGMENT;
	return barrier;
}

// Reuse transition: fragment-sampled resource -> pogo attachment.
// initial=true treats the contents as discarded (UNDEFINED source).
static inline struct RITextureBarrier RI_PogoAttachmentBarrier( struct RITexture *texture, bool initial )
{
	struct RITextureBarrier barrier = {};
	barrier.texture = texture;
	barrier.before = initial ? RI_RESOURCE_STATE_UNDEFINED : RI_RESOURCE_STATE_SHADER_RESOURCE;
	barrier.beforeStages = initial ? RI_STAGE_NONE : RI_STAGE_FRAGMENT;
	barrier.after = RI_RESOURCE_STATE_RENDER_TARGET;
	return barrier;
}

void RI_PogoBufferInit( struct RIDevice *device, struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height, enum RI_Format_e format );
void RI_PogoBufferDestroy( struct RIDevice *device, struct RI_PogoBuffer *pogo );
void RI_PogoBufferToggle( struct RIDevice *device, struct RI_PogoBuffer *pogo, struct RICmd *handle );

// Produced on demand: the pogo views carry their cookie (stamped at init).
static inline struct RIDescriptor RI_PogoBufferAttachment( struct RI_PogoBuffer *pogo )
{
	return RIDescriptor::sampledImage( nullptr, pogo->pogoView[pogo->attachmentIndex].Get() );
}

static inline struct RIDescriptor RI_PogoBufferShaderResource( struct RI_PogoBuffer *pogo )
{
	return RIDescriptor::sampledImage( nullptr, pogo->pogoView[( pogo->attachmentIndex + 1 ) % 2].Get() );
}

#endif


