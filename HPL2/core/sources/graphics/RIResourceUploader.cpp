#include "graphics/RIResourceUploader.h"
#include "graphics/RIFormat.h"
#include "graphics/RID3D12.h"
#include "graphics/RIDevice.h"
#include "graphics/RIVK.h"
#include "graphics/RITimeline.h"

#include <system/LowLevelSystem.h>
#include <system/Types.h>
#include <system/stb_ds.h>

#include <algorithm>
#include <cassert>
#include <cstring>

// Wait for the active set's fence (signalled means the GPU is done with the
// previous use), free overflow temporaries, reset the pool, begin the cmd
// buffer. No-op if the group is already recording.
static VkCommandBuffer __AcquireCmd( struct RIDevice *device, struct RITransferCommandGroup *group )
{
#if ( DEVICE_IMPL_VULKAN )
	if( !group->is_recording ) {
		VkFence fence = group->vk.fences[group->active_set];
		if( vkGetFenceStatus( device->vk.device, fence ) == VK_NOT_READY ) {
			VK_WrapResult( vkWaitForFences( device->vk.device, 1, &fence, VK_TRUE, UINT64_MAX ) );
		}

		group->staging_buffer_offset = 0;
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[group->active_set] ); j++ ) {
			vkDestroyBuffer( device->vk.device, group->temporary_buffers[group->active_set][j].vk.buffer, NULL );
			vmaFreeMemory( device->vk.vmaAllocator, group->temporary_buffers[group->active_set][j].vk.allocation );
		}
		arrsetlen( group->temporary_buffers[group->active_set], 0 );

		VK_WrapResult( vkResetCommandPool( device->vk.device, group->cmd_pool[group->active_set].vk.pool, 0 ) );
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_WrapResult( vkBeginCommandBuffer( group->cmd[group->active_set].vk.cmd, &beginInfo ) );

		group->is_recording = true;
	}
	return group->cmd[group->active_set].vk.cmd;
#else
	assert( false && "Vulkan not selected" );
#endif
	return VK_NULL_HANDLE;
}

#if ( DEVICE_IMPL_D3D12 )
static void __AcquireCmdD3D12( struct RIDevice *device, struct RITransferCommandGroup *group )
{
	if( !RIIsTargetSelected( RI_DEVICE_API_D3D12 ) || group->is_recording )
		return;

	const size_t set = group->active_set;
	if( group->d3d12_set_signal_values[set] > 0 )
		group->d3d12_timeline.wait( device, group->d3d12_set_signal_values[set] );
	if( group->d3d12_mip_owner )
		group->d3d12_mip_owner->reclaim( *device, uint32_t(set) );

	group->staging_buffer_offset = 0;
	for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[set] ); j++ )
		group->temporary_buffers[set][j].dispose( device );
	arrsetlen( group->temporary_buffers[set], 0 );

	group->cmd_pool[set].reset( device );
	group->cmd[set].begin( device );
	group->is_recording = true;
}
#endif

static void __FreeTransferCommandGroup( struct RIDevice *device, struct RITransferCommandGroup *group );

static void __InitTransferCommandGroup( struct RIDevice *device, struct RITransferCommandGroup *group, struct RIQueue *queue )
{
	memset( group, 0, sizeof( *group ) );
	group->queue = queue;
	group->active_set = 0;
	group->is_recording = false;

#if ( DEVICE_IMPL_VULKAN )
	if( RIIsTargetSelected( RI_DEVICE_API_VK ) ) for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		{
			VkCommandPoolCreateInfo info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			info.queueFamilyIndex = queue->vk.queueFamilyIdx;
			info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			group->cmd_pool[i].vk.queue = queue->vk.queue;
			VK_WrapResult( vkCreateCommandPool( device->vk.device, &info, NULL, &group->cmd_pool[i].vk.pool ) );

			// Keep command allocation and capability capture in one path. Direct
			// Vulkan allocation here left barrier conversion with zero features.
			group->cmd[i].init( device, &group->cmd_pool[i] );
		}

		group->staging_buffer[i] = RIBuffer::create(
			device, {(uint64_t)RI_RESOURCE_STAGE_BUFFER_SIZE,
			         RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST,
			         RI_MEMORY_HOST_UPLOAD, 0});

		// Fence starts signalled so the first acquire doesn't block.
		{
			VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VK_WrapResult( vkCreateFence( device->vk.device, &info, NULL, &group->vk.fences[i] ) );
		}

		{
			VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			VK_WrapResult( vkCreateSemaphore( device->vk.device, &info, NULL, &group->vk.semaphores[i] ) );
		}

		group->temporary_buffers[i] = NULL;
	}
#endif
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		group->d3d12_timeline.init( device );
		if( group->d3d12_timeline.d3d12.fence == nullptr )
			return;
		for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
			group->cmd_pool[i].init( device, queue );
			if( group->cmd_pool[i].d3d12.allocator == nullptr ) {
				__FreeTransferCommandGroup( device, group );
				return;
			}
			group->cmd[i].init( device, &group->cmd_pool[i] );
			if( group->cmd[i].d3d12.cmdList == nullptr ) {
				__FreeTransferCommandGroup( device, group );
				return;
			}
			group->staging_buffer[i] = RIBuffer::create( device, {(uint64_t)RI_RESOURCE_STAGE_BUFFER_SIZE,
				RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST,
				RI_MEMORY_HOST_UPLOAD, 0} );
			if( group->staging_buffer[i].isEmpty() ) {
				__FreeTransferCommandGroup( device, group );
				return;
			}
			group->d3d12_set_signal_values[i] = 0;
			group->temporary_buffers[i] = NULL;
		}
	}
#endif
}

static void __FreeTransferCommandGroup( struct RIDevice *device, struct RITransferCommandGroup *group )
{
#if ( DEVICE_IMPL_VULKAN )
	if( RIIsTargetSelected( RI_DEVICE_API_VK ) ) for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[i] ); j++ ) {
			vkDestroyBuffer( device->vk.device, group->temporary_buffers[i][j].vk.buffer, NULL );
			vmaFreeMemory( device->vk.vmaAllocator, group->temporary_buffers[i][j].vk.allocation );
		}
		arrfree( group->temporary_buffers[i] );

		vkFreeCommandBuffers( device->vk.device, group->cmd_pool[i].vk.pool, 1, &group->cmd[i].vk.cmd );
		vkDestroyCommandPool( device->vk.device, group->cmd_pool[i].vk.pool, NULL );

		vmaDestroyBuffer( device->vk.vmaAllocator, group->staging_buffer[i].vk.buffer, group->staging_buffer[i].vk.allocation );

		vkDestroyFence( device->vk.device, group->vk.fences[i], NULL );
		vkDestroySemaphore( device->vk.device, group->vk.semaphores[i], NULL );
	}
#endif
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
			if( group->d3d12_set_signal_values[i] > 0 )
				group->d3d12_timeline.wait( device, group->d3d12_set_signal_values[i] );
			if( group->d3d12_mip_owner )
				group->d3d12_mip_owner->reclaim( *device, uint32_t(i) );
			for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[i] ); j++ )
				group->temporary_buffers[i][j].dispose( device );
			arrfree( group->temporary_buffers[i] );
			if( !group->cmd[i].isEmpty() ) group->cmd[i].dispose( device );
			if( group->cmd_pool[i].d3d12.allocator ) group->cmd_pool[i].dispose( device );
			if( !group->staging_buffer[i].isEmpty() ) group->staging_buffer[i].dispose( device );
		}
		if( group->d3d12_timeline.d3d12.fence ) group->d3d12_timeline.dispose( device );
	}
#endif
}

static bool __AllocateFromStageBuffer( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, size_t alignment, struct RIMappedMemoryRange *out )
{
	const size_t alignedSize = ALIGN_TO( size, alignment );
	const size_t alignedOffset = ALIGN_TO( group->staging_buffer_offset, alignment );

	if( alignedOffset >= RI_RESOURCE_STAGE_BUFFER_SIZE )
		return false;
	if( alignedSize > RI_RESOURCE_STAGE_BUFFER_SIZE - alignedOffset )
		return false;

	const size_t set = group->active_set;
	out->offset = alignedOffset;
	out->size = alignedSize;
	out->data = (uint8_t *)group->staging_buffer[set].mappedAddress + alignedOffset;
	out->buffer = group->staging_buffer[set];

	group->staging_buffer_offset = alignedOffset + alignedSize;
	return true;
}

static void __AllocateTemporaryBuffer( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, struct RIMappedMemoryRange *out )
{
	struct RIBuffer tmp = RIBuffer::create(
		device, {(uint64_t)size,
		         RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST,
			         RI_MEMORY_HOST_UPLOAD, 0});
	if( tmp.isEmpty() ) {
		out->offset = 0;
		out->size = 0;
		out->data = NULL;
		out->buffer = tmp;
		return;
	}

	arrpush( group->temporary_buffers[group->active_set], tmp );

	out->offset = 0;
	out->size = size;
	out->data = tmp.mappedAddress;
	out->buffer = tmp;
}

static void __ResolveStageMemory( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, size_t alignment, struct RIMappedMemoryRange *out )
{
	if( !__AllocateFromStageBuffer( device, group, size, alignment, out ) )
		__AllocateTemporaryBuffer( device, group, size, out );
}

void RI_InitResourceUploader( struct RIDevice *device, struct RIResourceUploader *res )
{
	assert( res );
	// RIResourceUploader owns RID3D12MipGeneration, which in turn owns
	// std::vectors. Clearing the complete object as raw bytes corrupts those
	// containers before the first mip-generation scratch allocation.
	res->upload_resource = {};
	res->copy_resource = {};

	__InitTransferCommandGroup( device, &res->upload_resource, &device->queues[RI_QUEUE_GRAPHICS] );

	struct RIQueue *copyQueue = &device->queues[RI_QUEUE_COPY];
#if ( DEVICE_IMPL_VULKAN )
	if( RIIsTargetSelected( RI_DEVICE_API_VK ) && copyQueue->vk.queue == VK_NULL_HANDLE )
		copyQueue = &device->queues[RI_QUEUE_GRAPHICS];
#endif
	__InitTransferCommandGroup( device, &res->copy_resource, copyQueue );
#if ( DEVICE_IMPL_D3D12 )
	res->upload_resource.d3d12_mip_owner = &res->d3d12_mip_generation;
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) && !res->d3d12_mip_generation.init( *device ) )
		hpl::Warning( "RI: failed to initialize D3D12 mip generation\n" );
#endif
}

void RI_FreeResourceUploader( struct RIDevice *device, struct RIResourceUploader *res )
{
	assert( res );
	__FreeTransferCommandGroup( device, &res->upload_resource );
	__FreeTransferCommandGroup( device, &res->copy_resource );
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) )
		res->d3d12_mip_generation.dispose( *device );
#endif
	res->upload_resource = {};
	res->copy_resource = {};
}

void RI_ResourceBeginCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans )
{
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
#if ( DEVICE_IMPL_D3D12 )
		__AcquireCmdD3D12( device, &res->upload_resource );
#endif
	} else {
		__AcquireCmd( device, &res->upload_resource );
	}
	__ResolveStageMemory( device, &res->upload_resource, trans->size, 4, &trans->mapped );
}

void RI_ResourceEndCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans )
{
#if ( DEVICE_IMPL_VULKAN )
	if( RIIsTargetSelected( RI_DEVICE_API_VK ) ) {
	VkBufferCopy region = { 0 };
	region.srcOffset = trans->mapped.offset;
	region.dstOffset = trans->offset;
	region.size = trans->size;

	VkCommandBuffer cmd = __AcquireCmd( device, &res->upload_resource );
	struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];

	if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RIBufferBarrier pre_barrier = {};
		pre_barrier.buffer = &trans->target;
		pre_barrier.before = trans->currentState;
		pre_barrier.beforeStages = trans->currentStages;
		pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
		pre_barrier.afterStages = RI_STAGE_COPY;
		barrierCmd->vk_d3d12_bufferBarrier( pre_barrier );
	}

	vkCmdCopyBuffer( cmd, trans->mapped.buffer.vk.buffer, trans->target.vk.buffer, 1, &region );

	if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RIBufferBarrier post_barrier = {};
		post_barrier.buffer = &trans->target;
		post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
		post_barrier.beforeStages = RI_STAGE_COPY;
		post_barrier.after = trans->postState;
		post_barrier.afterStages = trans->postStages;
		barrierCmd->vk_d3d12_bufferBarrier( post_barrier );
	}
	}
#endif
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		__AcquireCmdD3D12( device, &res->upload_resource );
		struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];
		if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
			struct RIBufferBarrier pre_barrier = {};
			pre_barrier.buffer = &trans->target;
			pre_barrier.before = trans->currentState;
			pre_barrier.beforeStages = trans->currentStages;
			pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
			pre_barrier.afterStages = RI_STAGE_COPY;
			barrierCmd->vk_d3d12_bufferBarrier( pre_barrier );
		}
		barrierCmd->copyBuffer( device, &trans->mapped.buffer, trans->mapped.offset,
			&trans->target, trans->offset, trans->size );
		if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
			struct RIBufferBarrier post_barrier = {};
			post_barrier.buffer = &trans->target;
			post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
			post_barrier.beforeStages = RI_STAGE_COPY;
			post_barrier.after = trans->postState;
			post_barrier.afterStages = trans->postStages;
			barrierCmd->vk_d3d12_bufferBarrier( post_barrier );
		}
	}
#endif
}

void RI_ResourceBeginCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans )
{
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );
		const uint64_t alignedRowPitch = RIFormatAlignRowPitch( trans->rowPitch,
			device->physicalAdapter.uploadBufferTextureRowAlignment, formatProps->stride );
		const uint64_t alignedSlicePitch = (uint64_t)trans->sliceNum * alignedRowPitch;
		const uint32_t depth = trans->depth ? trans->depth : 1;
		const uint64_t totalSize = alignedSlicePitch * depth;

		trans->alignRowPitch = (uint32_t)alignedRowPitch;
		trans->alignSlicePitch = (uint32_t)alignedSlicePitch;

		size_t offsetAlign = device->physicalAdapter.uploadBufferOffsetAlignment;
		if( formatProps->stride > offsetAlign )
			offsetAlign = formatProps->stride;
		if( offsetAlign < 4 )
			offsetAlign = 4;

		__AcquireCmdD3D12( device, &res->upload_resource );
		__ResolveStageMemory( device, &res->upload_resource, totalSize, offsetAlign, &trans->mapped );
		return;
	}
#endif
#if ( DEVICE_IMPL_VULKAN )
	if( !RIIsTargetSelected( RI_DEVICE_API_VK ) ) return;
	const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );
	const uint64_t alignedRowPitch = RIFormatAlignRowPitch( trans->rowPitch,
		device->physicalAdapter.uploadBufferTextureRowAlignment, formatProps->stride );
	const uint64_t alignedSlicePitch = (uint64_t)trans->sliceNum * alignedRowPitch;
	const uint32_t depth = trans->depth ? trans->depth : 1;
	const uint64_t totalSize = alignedSlicePitch * depth;

	trans->alignRowPitch = (uint32_t)alignedRowPitch;
	trans->alignSlicePitch = (uint32_t)alignedSlicePitch;

	// bufferOffset must be a multiple of the texel block size and the device's
	// optimalBufferCopyOffsetAlignment (Vulkan spec, vkCmdCopyBufferToImage).
	size_t offsetAlign = device->physicalAdapter.uploadBufferOffsetAlignment;
	if( formatProps->stride > offsetAlign )
		offsetAlign = formatProps->stride;
	if( offsetAlign < 4 )
		offsetAlign = 4;

	__AcquireCmd( device, &res->upload_resource );
	__ResolveStageMemory( device, &res->upload_resource, totalSize, offsetAlign, &trans->mapped );
#else
	(void)device; (void)res; (void)trans;
#endif
}

void RI_ResourceEndCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans )
{
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );
		const uint32_t rowBlockNum = trans->alignRowPitch / formatProps->stride;
		const uint32_t bufferRowLength = rowBlockNum * formatProps->blockWidth;
		const uint32_t sliceRowNum = trans->alignSlicePitch / trans->alignRowPitch;
		const uint32_t bufferImageHeight = sliceRowNum * formatProps->blockHeight;

		__AcquireCmdD3D12( device, &res->upload_resource );
		struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];

		if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
			struct RITextureBarrier pre_barrier = {};
			pre_barrier.texture = &trans->target;
			pre_barrier.before = trans->currentState;
			pre_barrier.beforeStages = trans->currentStages;
			pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
			pre_barrier.afterStages = RI_STAGE_COPY;
			pre_barrier.baseMip = trans->mipOffset;
			pre_barrier.mipCount = 1;
			pre_barrier.baseLayer = trans->arrayOffset;
			pre_barrier.layerCount = 1;
			barrierCmd->vk_d3d12_textureBarrier( pre_barrier );
		}

		struct RIBufferTextureCopyDesc desc = {};
		desc.bufferOffset = trans->mapped.offset;
		desc.bufferRowLength = bufferRowLength;
		desc.bufferImageHeight = bufferImageHeight;
		desc.bytesPerRow = trans->alignRowPitch;
		desc.bytesPerImage = trans->alignSlicePitch;
		desc.mipLevel = trans->mipOffset;
		desc.arrayLayer = trans->arrayOffset;
		desc.x = trans->x;
		desc.y = trans->y;
		desc.z = trans->z;
		desc.width = trans->width;
		desc.height = trans->height;
		desc.depth = trans->depth;
		barrierCmd->copyBufferToTexture( device, &trans->mapped.buffer, &trans->target, desc );

		if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
			struct RITextureBarrier post_barrier = {};
			post_barrier.texture = &trans->target;
			post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
			post_barrier.beforeStages = RI_STAGE_COPY;
			post_barrier.after = trans->postState;
			post_barrier.afterStages = trans->postStages;
			post_barrier.baseMip = trans->mipOffset;
			post_barrier.mipCount = 1;
			post_barrier.baseLayer = trans->arrayOffset;
			post_barrier.layerCount = 1;
			barrierCmd->vk_d3d12_textureBarrier( post_barrier );
		}
		return;
	}
#endif
#if ( DEVICE_IMPL_VULKAN )
	if( !RIIsTargetSelected( RI_DEVICE_API_VK ) ) return;
	const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );

	// bufferRowLength / bufferImageHeight describe the layout of the staging
	// buffer in texels, and must match the strides used to write it. The CPU
	// writes rows at alignRowPitch (which equals rowPitch on AMD/Intel where
	// optimalBufferCopyRowPitchAlignment == 1, but is 256-aligned on NVIDIA).
	// Deriving from the unaligned rowPitch caused texture corruption on NVIDIA.
	// The aligned pitch is also a whole number of format strides.
	const uint32_t rowBlockNum = trans->alignRowPitch / formatProps->stride;
	const uint32_t bufferRowLength = rowBlockNum * formatProps->blockWidth;
	const uint32_t sliceRowNum = trans->alignSlicePitch / trans->alignRowPitch;
	const uint32_t bufferImageHeight = sliceRowNum * formatProps->blockHeight;

	VkBufferImageCopy region = { 0 };
	region.bufferOffset = trans->mapped.offset;
	region.bufferRowLength = bufferRowLength;
	region.bufferImageHeight = bufferImageHeight;
	region.imageOffset.x = trans->x;
	region.imageOffset.y = trans->y;
	region.imageOffset.z = trans->z;
	region.imageExtent.width = trans->width;
	region.imageExtent.height = trans->height;
	region.imageExtent.depth = trans->depth;
	region.imageSubresource.mipLevel = trans->mipOffset;
	region.imageSubresource.baseArrayLayer = trans->arrayOffset;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkCommandBuffer cmd = __AcquireCmd( device, &res->upload_resource );
	struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];

	// Barriers cover only the single (mipOffset, arrayOffset) subresource the
	// copy writes.
	if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RITextureBarrier pre_barrier = {};
		pre_barrier.texture = &trans->target;
		pre_barrier.before = trans->currentState;
		pre_barrier.beforeStages = trans->currentStages;
		pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
		pre_barrier.afterStages = RI_STAGE_COPY;
		pre_barrier.baseMip = trans->mipOffset;
		pre_barrier.mipCount = 1;
		pre_barrier.baseLayer = trans->arrayOffset;
		pre_barrier.layerCount = 1;
		barrierCmd->vk_d3d12_textureBarrier( pre_barrier );
	}

	vkCmdCopyBufferToImage( cmd, trans->mapped.buffer.vk.buffer, trans->target.vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

	if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RITextureBarrier post_barrier = {};
		post_barrier.texture = &trans->target;
		post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
		post_barrier.beforeStages = RI_STAGE_COPY;
		post_barrier.after = trans->postState;
		post_barrier.afterStages = trans->postStages;
		post_barrier.baseMip = trans->mipOffset;
		post_barrier.mipCount = 1;
		post_barrier.baseLayer = trans->arrayOffset;
		post_barrier.layerCount = 1;
		barrierCmd->vk_d3d12_textureBarrier( post_barrier );
	}
#endif
}

bool RI_FormatSupportsMipGeneration( struct RIDevice *device, uint32_t format )
{
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) )
		return RID3D12_MipFormatSupported( *device, format, false );
#endif
#if ( DEVICE_IMPL_VULKAN )
	if( !RIIsTargetSelected( RI_DEVICE_API_VK ) ) return false;
	const struct RIFormatProps *formatProps = GetRIFormatProps( format );
	if( formatProps->blockWidth > 1 ) {
		// vkCmdBlitImage cannot write compressed images; BC sources come from DDS
		// and already carry authored mips.
		return false;
	}

	VkFormatProperties properties = {};
	vkGetPhysicalDeviceFormatProperties( device->physicalAdapter.vk.physicalDevice, RIFormatToVK( format ), &properties );
	const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
		VK_FORMAT_FEATURE_BLIT_DST_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	return ( properties.optimalTilingFeatures & required ) == required;
#else
	(void)device;
	(void)format;
	return false;
#endif
}

void RI_ResourceGenerateMips( struct RIDevice *device, struct RIResourceUploader *res, struct RIGenerateMipsDesc *desc )
{
#if ( DEVICE_IMPL_D3D12 )
	if( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) ) {
		assert( desc );
		if( !desc || desc->mipNum <= 1 ) return;
		if( !res->d3d12_mip_generation.isReady() ) return;
		__AcquireCmdD3D12( device, &res->upload_resource );
		if( !res->d3d12_mip_generation.generate( *device,
			res->upload_resource.cmd[res->upload_resource.active_set], uint32_t(res->upload_resource.active_set), *desc ) )
			hpl::Warning( "RI: D3D12 mip generation could not be recorded\n" );
		return;
	}
#endif
#if ( DEVICE_IMPL_VULKAN )
	if( !RIIsTargetSelected( RI_DEVICE_API_VK ) ) return;
	assert( desc );
	if( !desc || desc->mipNum <= 1 )
		return;

	// The blit must be recorded on the graphics queue group, not copy_resource:
	// vkCmdBlitImage requires a queue with graphics capability.
	VkCommandBuffer cmd = __AcquireCmd( device, &res->upload_resource );
	struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];
	const uint32_t layerNum = desc->layerNum ? desc->layerNum : 1;

	struct RITextureBarrier mip0_barrier = {};
	mip0_barrier.texture = &desc->target;
	mip0_barrier.before = desc->currentState;
	mip0_barrier.beforeStages = desc->currentStages;
	mip0_barrier.after = RI_RESOURCE_STATE_COPY_SRC;
	mip0_barrier.afterStages = RI_STAGE_BLIT;
	mip0_barrier.baseMip = 0;
	mip0_barrier.mipCount = 1;
	mip0_barrier.baseLayer = (uint16_t)desc->arrayOffset;
	mip0_barrier.layerCount = (uint16_t)layerNum;
	barrierCmd->vk_d3d12_textureBarrier( mip0_barrier );

	struct RITextureBarrier destination_barrier = {};
	destination_barrier.texture = &desc->target;
	destination_barrier.before = RI_RESOURCE_STATE_UNDEFINED;
	destination_barrier.after = RI_RESOURCE_STATE_COPY_DST;
	destination_barrier.afterStages = RI_STAGE_BLIT;
	destination_barrier.baseMip = 1;
	destination_barrier.mipCount = (uint16_t)( desc->mipNum - 1 );
	destination_barrier.baseLayer = (uint16_t)desc->arrayOffset;
	destination_barrier.layerCount = (uint16_t)layerNum;
	barrierCmd->vk_d3d12_textureBarrier( destination_barrier );

	for( uint32_t i = 1; i < desc->mipNum; i++ ) {
		const uint32_t srcWidth = std::max( 1u, desc->width >> ( i - 1 ) );
		const uint32_t srcHeight = std::max( 1u, desc->height >> ( i - 1 ) );
		const uint32_t srcDepth = std::max( 1u, desc->depth >> ( i - 1 ) );
		const uint32_t dstWidth = std::max( 1u, desc->width >> i );
		const uint32_t dstHeight = std::max( 1u, desc->height >> i );
		const uint32_t dstDepth = std::max( 1u, desc->depth >> i );

		VkImageBlit region = {};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = i - 1;
		region.srcSubresource.baseArrayLayer = desc->arrayOffset;
		region.srcSubresource.layerCount = layerNum;
		region.srcOffsets[1] = {(int32_t)srcWidth, (int32_t)srcHeight, (int32_t)srcDepth};
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel = i;
		region.dstSubresource.baseArrayLayer = desc->arrayOffset;
		region.dstSubresource.layerCount = layerNum;
		region.dstOffsets[1] = {(int32_t)dstWidth, (int32_t)dstHeight, (int32_t)dstDepth};

		vkCmdBlitImage( cmd, desc->target.vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			desc->target.vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR );

		struct RITextureBarrier source_barrier = {};
		source_barrier.texture = &desc->target;
		source_barrier.before = RI_RESOURCE_STATE_COPY_DST;
		source_barrier.beforeStages = RI_STAGE_BLIT;
		source_barrier.after = RI_RESOURCE_STATE_COPY_SRC;
		source_barrier.afterStages = RI_STAGE_BLIT;
		source_barrier.baseMip = (uint16_t)i;
		source_barrier.mipCount = 1;
		source_barrier.baseLayer = (uint16_t)desc->arrayOffset;
		source_barrier.layerCount = (uint16_t)layerNum;
		barrierCmd->vk_d3d12_textureBarrier( source_barrier );
	}

	struct RITextureBarrier post_barrier = {};
	post_barrier.texture = &desc->target;
	post_barrier.before = RI_RESOURCE_STATE_COPY_SRC;
	post_barrier.beforeStages = RI_STAGE_BLIT;
	post_barrier.after = desc->postState;
	post_barrier.afterStages = desc->postStages;
	post_barrier.baseMip = 0;
	post_barrier.mipCount = (uint16_t)desc->mipNum;
	post_barrier.baseLayer = (uint16_t)desc->arrayOffset;
	post_barrier.layerCount = (uint16_t)layerNum;
	barrierCmd->vk_d3d12_textureBarrier( post_barrier );
#else
	(void)device;
	(void)res;
	(void)desc;
#endif
}

#if ( DEVICE_IMPL_VULKAN )
struct RIResourceUploaderVKResult RI_VKFlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res, size_t num_semaphores, VkSemaphoreSubmitInfo *wait_semaphore_info )
{
	assert( RIIsTargetSelected( RI_DEVICE_API_VK ) );
	struct RITransferCommandGroup *group = &res->upload_resource;
	const size_t active_set = group->active_set;

	// Nothing recorded — return current set's fence/semaphore so the caller can branch on signaled.
	if( !group->is_recording ) {
		struct RIResourceUploaderVKResult result = {};
		result.signaled = false;
		result.vk.fence = group->vk.fences[active_set];
		result.vk.semaphore = group->vk.semaphores[active_set];
		return result;
	}

	VK_WrapResult( vkEndCommandBuffer( group->cmd[active_set].vk.cmd ) );

	VkCommandBufferSubmitInfo cmdSubmit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	cmdSubmit.commandBuffer = group->cmd[active_set].vk.cmd;
	cmdSubmit.deviceMask = 0;

	VkSemaphoreSubmitInfo signalSem = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
	signalSem.semaphore = group->vk.semaphores[active_set];
	signalSem.value = 0; /* binary semaphore */
	signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	signalSem.deviceIndex = 0;

	VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &cmdSubmit;
	submitInfo.waitSemaphoreInfoCount = (uint32_t)num_semaphores;
	submitInfo.pWaitSemaphoreInfos = wait_semaphore_info;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos = &signalSem;

	/* Fence must already be signalled before we reset it; assert to catch bugs. */
	assert( vkGetFenceStatus( device->vk.device, group->vk.fences[active_set] ) == VK_SUCCESS );
	VK_WrapResult( vkResetFences( device->vk.device, 1, &group->vk.fences[active_set] ) );
	VK_WrapResult( vkQueueSubmit2( group->queue->vk.queue, 1, &submitInfo, group->vk.fences[active_set] ) );

	group->active_set = ( active_set + 1 ) % RI_RESOURCE_MAX_SETS;
	group->is_recording = false;

	struct RIResourceUploaderVKResult result = {};
	result.signaled = true;
	result.vk.fence = group->vk.fences[active_set];
	result.vk.semaphore = group->vk.semaphores[active_set];
	return result;
}
#endif

#if ( DEVICE_IMPL_D3D12 )
struct RIResourceUploaderD3D12Result RI_D3D12FlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res )
{
	assert( RIIsTargetSelected( RI_DEVICE_API_D3D12 ) );
	struct RITransferCommandGroup *group = &res->upload_resource;
	const size_t active = group->active_set;
	if( !group->is_recording )
		return {true, false, nullptr, 0};

	group->cmd[active].end( device );
	RICmd *cmds[] = {&group->cmd[active]};
	RITimelineOp signal = {&group->d3d12_timeline, group->d3d12_timeline.next(), RI_STAGE_COPY};
	RISubmitDesc desc = {};
	desc.cmds = cmds;
	desc.cmdCount = 1;
	desc.signals = &signal;
	desc.signalCount = 1;
	if( group->queue->submit( device, desc ) == RI_FAIL ) {
		hpl::Warning( "RI: D3D12 resource upload submission failed\n" );
		return {false, false, nullptr, 0};
	}

	group->d3d12_set_signal_values[active] = signal.value;
	group->active_set = (active + 1) % RI_RESOURCE_MAX_SETS;
	group->is_recording = false;
	return {true, true, &group->d3d12_timeline, signal.value};
}
#endif
