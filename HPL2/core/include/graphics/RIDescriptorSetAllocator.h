#ifndef R_DESCRIPTOR_POOL_H
#define R_DESCRIPTOR_POOL_H

#include <cstddef>
#include <stdint.h>

#include "graphics/RITypes.h"
#include "system/Hasher.h"

#define RESERVE_BLOCK_SIZE 1024
#define ALLOC_HASH_RESERVE 256
#define DESCRIPTOR_MAX_SIZE 64
#define DESCRIPTOR_RESERVED_SIZE 64

struct RIDescriptorSetSlot {
	hash_t hash;
	uint32_t frameCount;
	// queue
	struct RIDescriptorSetSlot *quNext;
	struct RIDescriptorSetSlot *quPrev;

	// hash
	struct RIDescriptorSetSlot *hNext;
	struct RIDescriptorSetSlot *hPrev;
	union {
#if ( DEVICE_IMPL_VULKAN )
		struct {
			VkDescriptorPool pool;
			VkDescriptorSet handle;
		} vk;
#endif
#if ( DEVICE_IMPL_D3D12 )
		struct {
			uint32_t resourceOffset;
			uint32_t samplerOffset;
			uint32_t resourceCount;
			uint32_t samplerCount;
		} d3d12;
#endif
	};
};

// A range in the device-wide shader-visible heaps.  The offsets are relative
// to the heap starts and are deliberately not GPU handles: callers can build
// either a graphics or compute root table from the same allocation.
struct RIDescriptorArenaAllocation {
	uint32_t resourceOffset;
	uint32_t samplerOffset;
	uint32_t resourceCount;
	uint32_t samplerCount;
};

struct RIDescriptorArenaFence {
	#if ( DEVICE_IMPL_D3D12 )
		ID3D12Fence *fence;
	#else
		void *fence;
	#endif
	uint64_t value;
};

struct RIDescriptorPoolAllocSlot {
		union {
#if ( DEVICE_IMPL_VULKAN )
			struct {
				VkDescriptorPool handle;
			} vk;
#endif
		};
};

struct RIDescriptorSetAlloc;
typedef void ( *RIDescriptorSetAlloc_Create )( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc );

struct RIDescriptorSetAlloc {
	RIDescriptorSetAlloc_Create descriptor_alloc_handle;
	uint8_t framesInFlight; // the number of frames in flight 

	struct RIDescriptorSetSlot *hash_slots[ALLOC_HASH_RESERVE];
	struct RIDescriptorSetSlot *queue_begin;
	struct RIDescriptorSetSlot *queue_end;

	struct RIDescriptorSetSlot **reservedSlots; // stb arrays
	struct RIDescriptorPoolAllocSlot* pools; // stb arrays
	struct RIDescriptorSetSlot **blocks;
	size_t blockIndex;
};

struct RIDescriptorSetResult {
	bool found;
	struct RIDescriptorSetSlot *set; // the associated slot
};

struct RIDescriptorSetResult resolveDescriptorSetAlloc( struct RIDevice *device,
													 struct RIDescriptorSetAlloc *alloc,
													 uint32_t frameCount,
													 hash_t hash);
void freeDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc );

// D3D12 uses one arena per RIDevice, shared by all programs.  Allocation is
// ordinary range allocation from shader-visible CBV/SRV/UAV and sampler heaps;
// it does not consume or replace a future externally-owned bindless table.
// A release is not reusable until its fence has completed.  A null fence is
// quarantined until arena teardown, because descriptor destruction does not
// prove that all submitted command lists have stopped referring to the range.
// freeDescriptorArena likewise leaves the heaps alive while fenced work is
// pending.
bool initDescriptorArena( struct RIDevice *device );
void freeDescriptorArena( struct RIDevice *device );
bool getDescriptorArenaHeaps( struct RIDevice *device,
	#if ( DEVICE_IMPL_D3D12 )
		ID3D12DescriptorHeap **resourceHeap, ID3D12DescriptorHeap **samplerHeap
	#else
		void **resourceHeap, void **samplerHeap
	#endif
);
bool allocateDescriptorArena( struct RIDevice *device, uint32_t resourceCount,
	uint32_t samplerCount, struct RIDescriptorArenaAllocation *out );
// Geometry raw SRVs have their own fixed sub-range.  They must not consume
// ordinary program-table slots: geometry handles are published as raw heap
// indices and are therefore valid only while this sub-range remains stable.
bool allocateGeometryDescriptorArena( struct RIDevice *device,
	uint32_t resourceCount, struct RIDescriptorArenaAllocation *out );
void releaseGeometryDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation );
// Returns a released geometry slot for reuse.  Only call once every submission
// that could reference the slot has completed (timeline-retired).
void recycleGeometryDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation );
void releaseDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation,
	const struct RIDescriptorArenaFence *fence );
void reclaimDescriptorArena( struct RIDevice *device );
#if ( DEVICE_IMPL_D3D12 )
// Sampler tables are shared device-wide by content. The shader-visible sampler
// heap holds only 2048 entries, and nearly every cached program table carries
// the same few samplers, so giving each table its own copy exhausts the heap
// long before the resource heap fills. acquire returns a table whose contents
// equal descs[0..count), writing it on first use; *outKey identifies it for
// the matching release. A table whose last reference is released stays cached
// for the next caller and is only handed back to the heap, behind the fence of
// its last release, when an allocation would otherwise fail.
bool acquireSamplerTableArena( struct RIDevice *device,
	const D3D12_SAMPLER_DESC *descs, uint32_t count,
	uint32_t *outOffset, uint64_t *outKey );
void releaseSamplerTableArena( struct RIDevice *device, uint64_t key,
	const struct RIDescriptorArenaFence *fence );
#endif
// Diagnostic snapshot of the ordinary-table arena (geometry sub-range excluded).
struct RIDescriptorArenaStats {
	uint32_t resourceCapacity, resourceBumped, resourceLive, resourceFree, resourcePending, resourceRetired;
	uint32_t samplerCapacity, samplerBumped, samplerLive, samplerFree, samplerPending, samplerRetired;
	uint32_t liveAllocations, pendingAllocations;
	// Shared sampler tables (see acquireSamplerTableArena); their slots are
	// included in samplerLive.
	uint32_t samplerTables, samplerTablesReferenced;
};
bool getDescriptorArenaStats( struct RIDevice *device, struct RIDescriptorArenaStats *out );
// utility
struct RIDescriptorSetSlot *allocDescriptorSetSlot( struct RIDescriptorSetAlloc *alloc );
void attachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot );
void detachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot );

#endif
