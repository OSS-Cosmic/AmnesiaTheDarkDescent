#ifndef RI_SCRATCH_ALLOC_H
#define RI_SCRATCH_ALLOC_H
#include "RITypes.h"

#define RI_UNIFORM_SCRATCH_ALLLOC_SIZE (256 * 128)
#define RI_UNIFORM_SCRATCH_REQ_ALIGNMENT (256) 

struct RIBlockMem {
	struct RIBuffer buffer;       // Backend buffer + mappedAddress
	uint64_t deviceAddress;        // GPU address of buffer offset 0
};

struct RIScratchAlloc;
typedef struct RIBlockMem ( *RIAllocBlock_Func)(struct RIDevice* device, struct RIScratchAlloc* scratch, size_t size);

struct RIScratchAlloc {
	struct RIBlockMem *recycle;
	struct RIBlockMem *pool;
	// One-shot blocks for reqSize > blockSize. Freed on reset rather than
	// pooled — the wrong size to be reused for normal block-sized allocs.
	struct RIBlockMem *oversized;

	size_t alignmentReq;
	size_t blockSize;
	RIAllocBlock_Func alloc;

	// the current buffer
	struct RIBlockMem current;
	size_t blockOffset;
};

struct RIScratchAllocDesc{
	size_t blockSize;
	size_t alignmentReq;
	RIAllocBlock_Func alloc;
};

struct RIBufferScratchAllocReq {
	struct RIBlockMem block;
	void* pMappedAddress;
	size_t bufferOffset;
	size_t bufferSize;
	uint64_t deviceAddress;  // = block.deviceAddress + bufferOffset
};

size_t RINumberOfUsedBlock(struct RIDevice *device,struct RIScratchAlloc* pool);
struct RIBlockMem* RIGetUsedBlock(struct RIDevice *device,struct RIScratchAlloc* pool,size_t index);

struct RIBlockMem RIUniformScratchAllocHandler(struct RIDevice* device, struct RIScratchAlloc* scratch, size_t size);
struct RIBlockMem RIAccelScratchAllocHandler(struct RIDevice* device, struct RIScratchAlloc* scratch, size_t size);

void InitRIScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool, const struct RIScratchAllocDesc *desc );
void FreeRIScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool ); 
void RIResetScratchAlloc( struct RIDevice *device, struct RIScratchAlloc  *pool );

struct RIBufferScratchAllocReq RIAllocBufferFromScratchAlloc( struct RIDevice *device, struct RIScratchAlloc  *pool, size_t reqSize );
void RIFinishScrachReq( struct RIDevice *device, struct RIBufferScratchAllocReq *req );

#endif


