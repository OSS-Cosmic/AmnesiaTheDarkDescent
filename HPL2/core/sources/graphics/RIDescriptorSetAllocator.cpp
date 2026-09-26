#include "graphics/RIDescriptorSetAllocator.h"
#include "graphics/RID3D12.h"
#include "system/stb_ds.h"
#include <cassert>
#include <cstring>

#if ( DEVICE_IMPL_D3D12 )
#include "graphics/RIDevice.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {
// D3D12 permits one shader-visible CBV/SRV/UAV heap of up to 1,000,000 entries
// and one sampler heap of up to 2048 entries.  All bindless and raw geometry
// SRVs therefore use this device-wide arena; command lists cannot bind a second
// shader-visible resource heap concurrently.  The limit is decimal, not 2^20:
// NVIDIA rejects a 1024*1024 heap with E_INVALIDARG, which failed device init.
static const uint32_t kResourceCapacity = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2;
static const uint32_t kSamplerCapacity = 2048u;

struct ArenaRange {
	uint32_t offset;
	uint32_t count;
};
struct PendingRange {
	RIDescriptorArenaAllocation allocation;
	RIDescriptorArenaFence fence;
};
// A content-keyed sampler table shared by every program table that carries
// the same samplers. The descs are kept so a hash hit can be confirmed before
// two different tables alias. lastRelease is the fence of the most recent
// release; an unreferenced table is evicted behind it.
struct SamplerTable {
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t refs = 0;
	bool unfenced = false;
	RIDescriptorArenaFence lastRelease = {};
	std::vector<D3D12_SAMPLER_DESC> descs;
};
struct ArenaState {
	RIDevice *device = nullptr;
	ID3D12DescriptorHeap *resourceHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	uint32_t nextResource = 0;
	uint32_t nextGeometry = 0;
	uint32_t nextSampler = 0;
	std::vector<ArenaRange> freeResources;
	std::vector<ArenaRange> freeSamplers;
	// Geometry slots whose buffers have been retired against the D3D12
	// timeline (see recycleGeometryDescriptorArena).
	std::vector<ArenaRange> freeGeometry;
	// Ranges released without a queue fence are quarantined until arena
	// teardown. They are never eligible for immediate reuse.
	std::vector<ArenaRange> retiredResources;
	std::vector<ArenaRange> retiredSamplers;
	std::vector<PendingRange> pending;
	// Only exact live allocations may be released. This prevents malformed or
	// partially overlapping ranges from entering a free list.
	std::vector<RIDescriptorArenaAllocation> live;
	std::vector<RIDescriptorArenaAllocation> liveGeometry;
	std::unordered_map<uint64_t, SamplerTable> samplerTables;
};
static std::vector<ArenaState> g_arenas;

static ArenaState *findArena( RIDevice *device ) {
	for( ArenaState &arena : g_arenas ) if( arena.device == device ) return &arena;
	return nullptr;
}

static void mergeRange( std::vector<ArenaRange> &ranges, ArenaRange range ) {
	if( !range.count ) return;
	ranges.push_back( range );
	std::sort( ranges.begin(), ranges.end(), []( const ArenaRange &a, const ArenaRange &b ) {
		return a.offset < b.offset;
	} );
	std::vector<ArenaRange> merged;
	for( const ArenaRange &item : ranges ) {
		if( !merged.empty() && uint64_t(merged.back().offset) + merged.back().count >= item.offset ) {
			const uint64_t end = std::max<uint64_t>( uint64_t(merged.back().offset) + merged.back().count,
				uint64_t(item.offset) + item.count );
			merged.back().count = uint32_t( end - merged.back().offset );
		} else merged.push_back( item );
	}
	ranges.swap( merged );
}

static bool takeRange( std::vector<ArenaRange> &ranges, uint32_t count, uint32_t *offset ) {
	if( !count ) { *offset = 0; return true; }
	for( size_t i = 0; i < ranges.size(); ++i ) {
		if( ranges[i].count < count ) continue;
		*offset = ranges[i].offset;
		ranges[i].offset += count;
		ranges[i].count -= count;
		if( !ranges[i].count ) ranges.erase( ranges.begin() + i );
		return true;
	}
	return false;
}

static bool rangeContains(const ArenaRange &outer, uint32_t offset, uint32_t count) {
	return count && offset >= outer.offset &&
		uint64_t(offset) + count <= uint64_t(outer.offset) + outer.count;
}

static bool sameAllocation(const RIDescriptorArenaAllocation &a,
	const RIDescriptorArenaAllocation &b) {
	return a.resourceOffset == b.resourceOffset &&
		a.samplerOffset == b.samplerOffset &&
		a.resourceCount == b.resourceCount &&
		a.samplerCount == b.samplerCount;
}

static bool validOrdinaryAllocation(const RIDescriptorArenaAllocation &allocation) {
	return (allocation.resourceCount || allocation.resourceOffset == 0) &&
		(allocation.samplerCount || allocation.samplerOffset == 0) &&
		allocation.resourceCount <= kResourceCapacity &&
		(allocation.resourceCount == 0 ||
		 (allocation.resourceOffset >= RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY &&
		  allocation.resourceOffset <= kResourceCapacity - allocation.resourceCount)) &&
		allocation.samplerCount <= kSamplerCapacity &&
		(allocation.samplerCount == 0 ||
		 allocation.samplerOffset <= kSamplerCapacity - allocation.samplerCount);
}

static bool eraseLive(std::vector<RIDescriptorArenaAllocation> &live,
	const RIDescriptorArenaAllocation &allocation) {
	for (size_t i = 0; i < live.size(); ++i) {
		if (!sameAllocation(live[i], allocation)) continue;
		live.erase(live.begin() + i);
		return true;
	}
	return false;
}

static bool alreadyReleased(const ArenaState &arena,
	const RIDescriptorArenaAllocation &allocation) {
	for (const PendingRange &pending : arena.pending)
		if (sameAllocation(pending.allocation, allocation)) return true;
	for (const ArenaRange &range : arena.retiredSamplers)
		if (rangeContains(range, allocation.samplerOffset, allocation.samplerCount)) return true;
	for (const ArenaRange &range : arena.retiredResources)
		if (rangeContains(range, allocation.resourceOffset, allocation.resourceCount)) return true;
	if (!allocation.resourceCount) {
		for (const ArenaRange &range : arena.freeSamplers)
			if (rangeContains(range, allocation.samplerOffset, allocation.samplerCount)) return true;
		return false;
	}
	for (const ArenaRange &range : arena.freeResources)
		if (rangeContains(range, allocation.resourceOffset, allocation.resourceCount)) {
			bool samplerFree = !allocation.samplerCount;
			for (const ArenaRange &sampler : arena.freeSamplers)
				if (rangeContains(sampler, allocation.samplerOffset, allocation.samplerCount))
					samplerFree = true;
			if (samplerFree) return true;
		}
	return false;
}

}
#endif

struct RIDescriptorSetSlot *allocDescriptorSetSlot( struct RIDescriptorSetAlloc *alloc )
{
	if( alloc->blocks == NULL || alloc->blockIndex == RESERVE_BLOCK_SIZE ) {
		struct RIDescriptorSetSlot *block = (struct RIDescriptorSetSlot*)calloc( RESERVE_BLOCK_SIZE, sizeof( struct RIDescriptorSetSlot ) );
		alloc->blockIndex = 0;
		arrpush( alloc->blocks, block );
		return block + ( alloc->blockIndex++ );
	}
	return alloc->blocks[arrlen( alloc->blocks ) - 1] + ( alloc->blockIndex++ );
}

void attachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot )
{
	assert( slot );
	{
		slot->quNext = NULL;
		slot->quPrev = alloc->queue_end;
		if( alloc->queue_end ) {
			alloc->queue_end->quNext = slot;
		}
		alloc->queue_end = slot;
		if( !alloc->queue_begin ) {
			alloc->queue_begin = slot;
		}
	}
	{
		const size_t hashIndex = slot->hash % ALLOC_HASH_RESERVE;
		slot->hPrev = NULL;
		slot->hNext = NULL;
		if( alloc->hash_slots[hashIndex] ) {
			alloc->hash_slots[hashIndex]->hPrev = slot;
			slot->hNext = alloc->hash_slots[hashIndex];
		}
		alloc->hash_slots[hashIndex] = slot;
	}
}

void detachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot )
{
	assert( slot );
	// remove from queue
	{
		if( alloc->queue_begin == slot ) {
			alloc->queue_begin = slot->quNext;
			if( slot->quNext ) {
				slot->quNext->quPrev = NULL;
			}
		} else if( alloc->queue_end == slot ) {
			alloc->queue_end = slot->quPrev;
			if( slot->quPrev ) {
				slot->quPrev->quNext = NULL;
			}
		} else {
			if( slot->quPrev ) {
				slot->quPrev->quNext = slot->quNext;
			}
			if( slot->quNext ) {
				slot->quNext->quPrev = slot->quPrev;
			}
		}
	}
	// free from hashTable
	{
		const size_t hashIndex = slot->hash % ALLOC_HASH_RESERVE;
		if( alloc->hash_slots[hashIndex] == slot ) {
			alloc->hash_slots[hashIndex] = slot->hNext;
			if( slot->hNext ) {
				slot->hNext->hPrev = NULL;
			}
		} else {
			if( slot->hPrev ) {
				slot->hPrev->hNext = slot->hNext;
			}
			if( slot->hNext ) {
				slot->hNext->hPrev = slot->hPrev;
			}
		}
		slot->hPrev = NULL;
		slot->hNext = NULL;
	}
}

struct RIDescriptorSetResult resolveDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc, uint32_t frameCount, hash_t hash )
{
	struct RIDescriptorSetResult result = { 0 };
	const size_t hashIndex = hash % ALLOC_HASH_RESERVE;
	for( struct RIDescriptorSetSlot *c = alloc->hash_slots[hashIndex]; c; c = c->hNext ) {
		if( c->hash == hash ) {
			if( alloc->queue_end != c ) {
				if( alloc->queue_begin == c ) {
					alloc->queue_begin = c->quNext;
					if( c->quNext ) {
						c->quNext->quPrev = NULL;
					}
				} else {
					if( c->quPrev ) {
						c->quPrev->quNext = c->quNext;
					}
					if( c->quNext ) {
						c->quNext->quPrev = c->quPrev;
					}
				}
				c->quNext = NULL;
				c->quPrev = alloc->queue_end;
				if( alloc->queue_end ) {
					alloc->queue_end->quNext = c;
				}
				alloc->queue_end = c;
			}

			c->frameCount = frameCount;
			result.set = c;
			result.found = true;
			assert(result.set);
			return result;
		}
	}

	if( alloc->queue_begin && frameCount > alloc->queue_begin->frameCount + alloc->framesInFlight) {
		struct RIDescriptorSetSlot *slot = alloc->queue_begin;
		detachDescriptorSlot( alloc, slot );
		slot->frameCount = frameCount;
		slot->hash = hash;
		attachDescriptorSlot( alloc, slot );
		result.set = slot;
		result.found = false;
		assert(result.set);
		return result;
	}

	if( arrlen( alloc->reservedSlots ) == 0 ) {
		alloc->descriptor_alloc_handle(device, alloc);
		assert(arrlen(alloc->reservedSlots) > 0); // we didn't reserve any slots ...
	}
	struct RIDescriptorSetSlot *slot = arrpop( alloc->reservedSlots );
	slot->hash = hash;
	slot->frameCount = frameCount;
	attachDescriptorSlot( alloc, slot );
	result.set = slot;
	result.found = false;
	assert(result.set);
	return result;
}

void freeDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc )
{
#if ( DEVICE_IMPL_VULKAN )
	for( size_t i = 0; i < static_cast<size_t>(arrlen( alloc->blocks )); i++ ) {
		// TODO: do i need to free indivudal descriptor sets or can i just free the entire pool
		// for(size_t blockIdx = 0; blockIdx < RESERVE_BLOCK_SIZE; blockIdx++) {
		//	vkFreeDescriptorSets(device->vk.device, alloc->blocks[i]->vk.pool, )
		//}
		free( alloc->blocks[i] );
	}
	arrfree( alloc->blocks );
	for( size_t i = 0; i < static_cast<size_t>(arrlen( alloc->pools )); i++ ) {
		vkDestroyDescriptorPool( device->vk.device, alloc->pools[i].vk.handle, NULL );
	}
	arrfree( alloc->pools );
	arrfree( alloc->reservedSlots );
	// The queue/hash entries point into the freed blocks — clear them so the
	// alloc struct is inert (and reusable) after a dispose.
	alloc->queue_begin = NULL;
	alloc->queue_end = NULL;
	memset( alloc->hash_slots, 0, sizeof( alloc->hash_slots ) );
	alloc->blockIndex = 0;
#endif
}

#if ( DEVICE_IMPL_D3D12 )
bool initDescriptorArena( struct RIDevice *device )
{
	if( !device || !device->d3d12.device ) return false;
	if( findArena( device ) ) return true;
	ArenaState arena;
	arena.device = device;
	D3D12_DESCRIPTOR_HEAP_DESC resourceDesc = {};
	resourceDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	resourceDesc.NumDescriptors = kResourceCapacity;
	resourceDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if( !D3D12_WrapResult( device->d3d12.device->CreateDescriptorHeap( &resourceDesc,
		IID_PPV_ARGS( &arena.resourceHeap ) ) ) ) return false;
	D3D12_DESCRIPTOR_HEAP_DESC samplerDesc = {};
	samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerDesc.NumDescriptors = kSamplerCapacity;
	samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if( !D3D12_WrapResult( device->d3d12.device->CreateDescriptorHeap( &samplerDesc,
		IID_PPV_ARGS( &arena.samplerHeap ) ) ) ) {
		arena.resourceHeap->Release();
		return false;
	}
	g_arenas.push_back( arena );
	return true;
}

bool getDescriptorArenaHeaps( struct RIDevice *device,
	ID3D12DescriptorHeap **resourceHeap, ID3D12DescriptorHeap **samplerHeap )
{
	if (resourceHeap) *resourceHeap = nullptr;
	if (samplerHeap) *samplerHeap = nullptr;
	ArenaState *arena = findArena( device );
	if( !arena || !arena->resourceHeap || !arena->samplerHeap ) return false;
	if( resourceHeap ) *resourceHeap = arena->resourceHeap;
	if( samplerHeap ) *samplerHeap = arena->samplerHeap;
	return true;
}

void reclaimDescriptorArena( struct RIDevice *device )
{
	ArenaState *arena = findArena( device );
	if( !arena ) return;
	for( size_t i = 0; i < arena->pending.size(); ) {
		PendingRange &pending = arena->pending[i];
		if( pending.fence.fence && pending.fence.fence->GetCompletedValue() < pending.fence.value ) {
			++i;
			continue;
		}
		mergeRange( arena->freeResources, { pending.allocation.resourceOffset, pending.allocation.resourceCount } );
		mergeRange( arena->freeSamplers, { pending.allocation.samplerOffset, pending.allocation.samplerCount } );
		if( pending.fence.fence ) pending.fence.fence->Release();
		arena->pending.erase( arena->pending.begin() + i );
	}
}

// Hands every unreferenced shared sampler table back to the heap behind the
// fence of its last release, then reclaims whatever has already retired.
// Returns whether anything was evicted.
static bool evictUnreferencedSamplerTables( RIDevice *device, ArenaState *arena )
{
	bool evicted = false;
	for( auto it = arena->samplerTables.begin(); it != arena->samplerTables.end(); ) {
		SamplerTable &table = it->second;
		if( table.refs ) { ++it; continue; }
		RIDescriptorArenaAllocation allocation = {};
		allocation.samplerOffset = table.offset;
		allocation.samplerCount = table.count;
		if( !table.unfenced && table.lastRelease.fence ) {
			// The table's reference to the fence moves into the pending entry.
			arena->pending.push_back( { allocation, table.lastRelease } );
		} else {
			if( table.lastRelease.fence ) table.lastRelease.fence->Release();
			mergeRange( arena->retiredSamplers, { table.offset, table.count } );
		}
		it = arena->samplerTables.erase( it );
		evicted = true;
	}
	if( evicted ) reclaimDescriptorArena( device );
	return evicted;
}

// Claims a sampler range, from the free list first and the bump cursor second.
// Nothing needs rolling back on failure.
static bool claimSamplers( ArenaState *arena, uint32_t count, uint32_t *offset )
{
	if( takeRange( arena->freeSamplers, count, offset ) ) return true;
	if( arena->nextSampler > kSamplerCapacity || count > kSamplerCapacity - arena->nextSampler )
		return false;
	*offset = arena->nextSampler;
	arena->nextSampler += count;
	return true;
}

bool acquireSamplerTableArena( struct RIDevice *device,
	const D3D12_SAMPLER_DESC *descs, uint32_t count,
	uint32_t *outOffset, uint64_t *outKey )
{
	if( !outOffset || !outKey || !count || !descs ) return false;
	*outOffset = 0;
	*outKey = 0;
	if( !initDescriptorArena( device ) ) return false;
	ArenaState *arena = findArena( device );
	const size_t bytes = sizeof( D3D12_SAMPLER_DESC ) * count;
	uint64_t key = hash_data( hash_u64( HASH_INITIAL_VALUE, count ), descs, bytes );
	// Key 0 means "no table" to callers; a collision probes to the next key.
	for( ;; ++key ) {
		if( !key ) continue;
		auto it = arena->samplerTables.find( key );
		if( it == arena->samplerTables.end() ) break;
		SamplerTable &table = it->second;
		if( table.count == count && memcmp( table.descs.data(), descs, bytes ) == 0 ) {
			table.refs++;
			*outOffset = table.offset;
			*outKey = key;
			return true;
		}
	}
	reclaimDescriptorArena( device );
	uint32_t offset = 0;
	if( !claimSamplers( arena, count, &offset ) &&
		!( evictUnreferencedSamplerTables( device, arena ) &&
		   claimSamplers( arena, count, &offset ) ) )
		return false;
#ifndef NDEBUG
	for( const RIDescriptorArenaAllocation &other : arena->live )
		assert( !( other.samplerCount && offset < other.samplerOffset + other.samplerCount &&
			other.samplerOffset < offset + count ) &&
			"shared sampler table overlaps a live sampler range" );
	for( const auto &other : arena->samplerTables )
		assert( !( offset < other.second.offset + other.second.count &&
			other.second.offset < offset + count ) &&
			"shared sampler table overlaps another shared table" );
#endif
	const UINT stride = device->d3d12.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER );
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = arena->samplerHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T( offset ) * stride;
	for( uint32_t i = 0; i < count; ++i, cpu.ptr += stride )
		device->d3d12.device->CreateSampler( &descs[i], cpu );
	SamplerTable table;
	table.offset = offset;
	table.count = count;
	table.refs = 1;
	table.descs.assign( descs, descs + count );
	arena->samplerTables.emplace( key, std::move( table ) );
	*outOffset = offset;
	*outKey = key;
	return true;
}

void releaseSamplerTableArena( struct RIDevice *device, uint64_t key,
	const struct RIDescriptorArenaFence *fence )
{
	ArenaState *arena = findArena( device );
	if( !arena || !key ) return;
	auto it = arena->samplerTables.find( key );
	if( it == arena->samplerTables.end() || !it->second.refs ) return;
	SamplerTable &table = it->second;
	// Track the latest release fence: the timeline only moves forward, so it
	// covers every earlier submission that referenced the table. A null fence
	// carries no completion proof, so once one is seen the table retires
	// permanently on eviction whatever later releases supply.
	if( !fence || !fence->fence || !fence->value ) table.unfenced = true;
	else {
		fence->fence->AddRef();
		if( table.lastRelease.fence ) table.lastRelease.fence->Release();
		table.lastRelease = *fence;
	}
	table.refs--;
}

bool allocateDescriptorArena( struct RIDevice *device, uint32_t resourceCount,
	uint32_t samplerCount, struct RIDescriptorArenaAllocation *out )
{
	if( !out ) return false;
	*out = {};
	// Empty descriptor tables are valid (and useful for programs containing
	// only root constants), and need no heap state at all.
	if( !resourceCount && !samplerCount ) return true;
	if( !initDescriptorArena( device ) ) return false;
	ArenaState *arena = findArena( device );
	reclaimDescriptorArena( device );
	uint32_t resourceOffset = 0, samplerOffset = 0;
	const bool resourceFromFree = takeRange( arena->freeResources, resourceCount, &resourceOffset );
	if( !resourceFromFree ) {
		// Keep the raw geometry namespace disjoint from all program-owned
		// descriptor tables.  Geometry indices are embedded in UniformObject and
		// cannot be retargeted when a normal table is recycled.
		if (arena->nextResource < RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY)
			arena->nextResource = RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY;
		if( arena->nextResource > kResourceCapacity || resourceCount > kResourceCapacity - arena->nextResource ) return false;
		resourceOffset = arena->nextResource;
		arena->nextResource += resourceCount;
	}
	if( !claimSamplers( arena, samplerCount, &samplerOffset ) &&
		!( evictUnreferencedSamplerTables( device, arena ) &&
		   claimSamplers( arena, samplerCount, &samplerOffset ) ) ) {
		// Roll back the resource claim.  It is not fence-gated yet.  A bump
		// claim must rewind the bump cursor; returning it only to the free
		// list would leave the cursor above the restored range and makes the
		// ownership invariant needlessly fragile.
		if( resourceFromFree )
			mergeRange( arena->freeResources, { resourceOffset, resourceCount } );
		else
			arena->nextResource = resourceOffset;
		return false;
	}
	out->resourceOffset = resourceOffset;
	out->samplerOffset = samplerOffset;
	out->resourceCount = resourceCount;
	out->samplerCount = samplerCount;
#ifndef NDEBUG
	// A range handed out twice lets one table silently overwrite another's
	// descriptors; catch it where the second owner is created.
	auto overlaps = []( uint32_t aOffset, uint32_t aCount, uint32_t bOffset, uint32_t bCount ) {
		return aCount && bCount && uint64_t( aOffset ) < uint64_t( bOffset ) + bCount &&
			uint64_t( bOffset ) < uint64_t( aOffset ) + aCount;
	};
	for( const RIDescriptorArenaAllocation &other : arena->live ) {
		assert( !overlaps( resourceOffset, resourceCount, other.resourceOffset, other.resourceCount ) &&
			"descriptor arena resource range handed out twice" );
		assert( !overlaps( samplerOffset, samplerCount, other.samplerOffset, other.samplerCount ) &&
			"descriptor arena sampler range handed out twice" );
	}
	for( const auto &table : arena->samplerTables )
		assert( !overlaps( samplerOffset, samplerCount, table.second.offset, table.second.count ) &&
			"descriptor arena sampler range overlaps a shared sampler table" );
#endif
	arena->live.push_back(*out);
	return true;
}

bool allocateGeometryDescriptorArena( struct RIDevice *device,
	uint32_t resourceCount, struct RIDescriptorArenaAllocation *out )
{
	if (!out || !resourceCount || resourceCount > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY)
		return false;
	*out = {};
	if (!initDescriptorArena(device)) return false;
	ArenaState *arena = findArena(device);
	reclaimDescriptorArena(device);
	// Geometry indices are embedded in UniformObject payloads and buffers can
	// be destroyed while previously submitted frames still execute.  Slots are
	// only returned to freeGeometry once the owning buffer's retire value has
	// completed on the timeline, so no in-flight frame can see an alias.
	uint32_t offset = 0;
	if (!takeRange(arena->freeGeometry, resourceCount, &offset)) {
		if (arena->nextGeometry > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY ||
		    resourceCount > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY - arena->nextGeometry)
			return false;
		offset = arena->nextGeometry;
		arena->nextGeometry += resourceCount;
	}
	// Geometry allocations are represented in the common allocation type, but
	// are released through the geometry entry point so a range can never be
	// returned to the ordinary-table free list by mistake.
	out->resourceOffset = offset;
	out->resourceCount = resourceCount;
	arena->liveGeometry.push_back(*out);
	return true;
}

void releaseGeometryDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation )
{
	ArenaState *arena = findArena(device);
	if (!arena || !allocation || !allocation->resourceCount) return;
	if (allocation->resourceOffset >= RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY ||
	    allocation->resourceCount > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY - allocation->resourceOffset)
		return;
	if (allocation->samplerCount || !eraseLive(arena->liveGeometry, *allocation))
		return;
	// Not reusable yet: a release has no queue fence and must not make a stale
	// packed handle alias a newly-created raw SRV.  The slot becomes reusable
	// through recycleGeometryDescriptorArena once the timeline retires it.
}

void recycleGeometryDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation )
{
	ArenaState *arena = findArena(device);
	if (!arena || !allocation || !allocation->resourceCount || allocation->samplerCount) return;
	if (allocation->resourceOffset >= RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY ||
	    allocation->resourceCount > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY - allocation->resourceOffset)
		return;
	// A slot still owned by a live buffer must never re-enter the free list.
	for (const RIDescriptorArenaAllocation &live : arena->liveGeometry)
		if (sameAllocation(live, *allocation)) return;
	mergeRange(arena->freeGeometry, { allocation->resourceOffset, allocation->resourceCount });
}

void releaseDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation,
	const struct RIDescriptorArenaFence *fence )
{
	ArenaState *arena = findArena( device );
	if( !arena || !allocation || (!allocation->resourceCount && !allocation->samplerCount) ) return;
	if (!validOrdinaryAllocation(*allocation) ||
		!eraseLive(arena->live, *allocation))
		return;
	// Destruction paths can converge (explicit teardown plus owner cleanup).
	// Treat a second release of the same logical allocation as a no-op so a
	// range cannot be returned twice and handed to two live descriptor sets.
	if (alreadyReleased(*arena, *allocation)) return;
	// A caller with a queue fence can reclaim the range after completion.  A
	// null fence has no completion proof, so it is quarantined permanently.
	if( !fence || !fence->fence || !fence->value ) {
		// Null is used by destruction paths that cannot supply the submission
		// fence. Retire permanently instead of making the range reusable while
		// an older command list may still reference it.
		mergeRange( arena->retiredResources,
			{ allocation->resourceOffset, allocation->resourceCount } );
		mergeRange( arena->retiredSamplers,
			{ allocation->samplerOffset, allocation->samplerCount } );
		return;
	}
	fence->fence->AddRef();
	arena->pending.push_back( { *allocation, *fence } );
}

bool getDescriptorArenaStats( struct RIDevice *device, struct RIDescriptorArenaStats *out )
{
	if( !out ) return false;
	*out = {};
	ArenaState *arena = findArena( device );
	if( !arena ) return false;
	auto sum = []( const std::vector<ArenaRange> &ranges ) {
		uint32_t total = 0;
		for( const ArenaRange &r : ranges ) total += r.count;
		return total;
	};
	out->resourceCapacity = kResourceCapacity - RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY;
	out->resourceBumped = arena->nextResource > RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY
		? arena->nextResource - RI_D3D12_GEOMETRY_DESCRIPTOR_CAPACITY : 0;
	out->resourceFree = sum( arena->freeResources );
	out->resourceRetired = sum( arena->retiredResources );
	out->samplerCapacity = kSamplerCapacity;
	out->samplerBumped = arena->nextSampler;
	out->samplerFree = sum( arena->freeSamplers );
	out->samplerRetired = sum( arena->retiredSamplers );
	for( const RIDescriptorArenaAllocation &a : arena->live ) {
		out->resourceLive += a.resourceCount;
		out->samplerLive += a.samplerCount;
	}
	for( const auto &[key, table] : arena->samplerTables ) {
		out->samplerLive += table.count;
		out->samplerTables++;
		if( table.refs ) out->samplerTablesReferenced++;
	}
	for( const PendingRange &p : arena->pending ) {
		out->resourcePending += p.allocation.resourceCount;
		out->samplerPending += p.allocation.samplerCount;
	}
	out->liveAllocations = (uint32_t)arena->live.size();
	out->pendingAllocations = (uint32_t)arena->pending.size();
	return true;
}

void freeDescriptorArena( struct RIDevice *device )
{
	// The heaps remain GPU-visible until all fence-retired ranges are gone.
	// Callers that own device teardown must first establish queue idleness and
	// call reclaimDescriptorArena; otherwise retaining the arena is safer than
	// releasing descriptors still referenced by submitted work.
	reclaimDescriptorArena( device );
	for( size_t i = 0; i < g_arenas.size(); ++i ) {
		if( g_arenas[i].device != device ) continue;
		if( !g_arenas[i].pending.empty() ) return;
		for( auto &[key, table] : g_arenas[i].samplerTables )
			if( table.lastRelease.fence ) table.lastRelease.fence->Release();
		g_arenas[i].samplerTables.clear();
		if( g_arenas[i].resourceHeap ) g_arenas[i].resourceHeap->Release();
		if( g_arenas[i].samplerHeap ) g_arenas[i].samplerHeap->Release();
		g_arenas.erase( g_arenas.begin() + i );
		return;
	}
}
#else
bool initDescriptorArena( struct RIDevice *device ) { (void)device; return false; }
void freeDescriptorArena( struct RIDevice *device ) { (void)device; }
bool getDescriptorArenaHeaps( struct RIDevice *device, void **resourceHeap, void **samplerHeap )
{ (void)device; if (resourceHeap) *resourceHeap = nullptr; if (samplerHeap) *samplerHeap = nullptr; return false; }
bool allocateDescriptorArena( struct RIDevice *device, uint32_t resourceCount,
	uint32_t samplerCount, struct RIDescriptorArenaAllocation *out )
{ (void)device; (void)resourceCount; (void)samplerCount; (void)out; return false; }
bool allocateGeometryDescriptorArena( struct RIDevice *device, uint32_t resourceCount,
	struct RIDescriptorArenaAllocation *out )
{ (void)device; (void)resourceCount; (void)out; return false; }
void releaseGeometryDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation )
{ (void)device; (void)allocation; }
void releaseDescriptorArena( struct RIDevice *device,
	const struct RIDescriptorArenaAllocation *allocation,
	const struct RIDescriptorArenaFence *fence )
{ (void)device; (void)allocation; (void)fence; }
void reclaimDescriptorArena( struct RIDevice *device ) { (void)device; }
bool getDescriptorArenaStats( struct RIDevice *device, struct RIDescriptorArenaStats *out )
{ (void)device; if (out) *out = {}; return false; }
#endif
