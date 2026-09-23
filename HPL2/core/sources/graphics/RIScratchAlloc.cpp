#include "graphics/RIScratchAlloc.h"
#include "system/stb_ds.h"
#include "graphics/RIRenderer.h"
#include "system/Types.h"
#include <limits>

static bool RIScratchValidAlignment(size_t alignment) {
  return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

static bool RIScratchAddSize(size_t lhs, size_t rhs, size_t *result) {
  if (lhs > std::numeric_limits<size_t>::max() - rhs)
    return false;
  *result = lhs + rhs;
  return true;
}

static bool RIScratchAlignedSize(size_t size, size_t alignment,
                                 size_t *result) {
  if (!RIScratchValidAlignment(alignment) ||
      !RIScratchAddSize(size, alignment - 1, result))
    return false;
  *result &= ~(alignment - 1);
  return true;
}

static bool RIScratchAlignedAddress(uint64_t address, size_t alignment,
                                    uint64_t *result) {
  if (!RIScratchValidAlignment(alignment) ||
      (uint64_t)(alignment - 1) >
          std::numeric_limits<uint64_t>::max() - address)
    return false;
  *result = (address + (uint64_t)alignment - 1) & ~((uint64_t)alignment - 1);
  return true;
}

static uint64_t RIScratchBufferAlignment(struct RIDevice *device,
                                         const struct RIScratchAlloc *scratch) {
#if (DEVICE_IMPL_D3D12)
  if (RIIsTargetSelected(RI_DEVICE_API_D3D12))
    return 0; // D3D12 committed buffers do not support explicit alignment.
#endif
  (void)device;
  return scratch->alignmentReq;
}

static bool RIScratchBackingSize(struct RIDevice *device,
                                 const struct RIScratchAlloc *scratch,
                                 size_t logicalSize, size_t *backingSize) {
  const size_t alignment = (size_t)RIScratchBufferAlignment(device, scratch);
  if (alignment == 0) {
    *backingSize = logicalSize;
  } else if (!RIScratchAddSize(logicalSize, alignment - 1, backingSize)) {
    return false;
  }
  return *backingSize <= (size_t)std::numeric_limits<uint64_t>::max();
}

struct RIBlockMem RIUniformScratchAllocHandler(struct RIDevice *device,
                                               struct RIScratchAlloc *scratch,
                                               size_t size) {
  struct RIBlockMem mem = {};
  size_t backingSize = 0;
  if (!RIScratchBackingSize(device, scratch, size, &backingSize))
    return mem;
  uint32_t usage = RI_BUFFER_USAGE_CONSTANT_BUFFER;
#if (DEVICE_IMPL_VULKAN)
  // Vulkan needs SHADER_DEVICE_ADDRESS so the allocator can align each slice
  // against the buffer's actual GPU address. D3D12 CBVs use the resource GPU
  // VA directly; DEVICE_ADDRESS would unnecessarily register every transient
  // UBO block in the finite, monotonic raw-geometry descriptor namespace.
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    usage |= RI_BUFFER_USAGE_DEVICE_ADDRESS;
#endif
  mem.buffer = RIBuffer::create(
      device, {(uint64_t)backingSize, usage, RI_MEMORY_HOST_UPLOAD,
               RIScratchBufferAlignment(device, scratch)});
  if (!mem.buffer.isEmpty() && mem.buffer.mappedAddress != nullptr)
    mem.deviceAddress = mem.buffer.GetDeviceHandle(device);
  if (mem.buffer.isEmpty() || mem.buffer.mappedAddress == nullptr ||
      mem.deviceAddress == 0) {
    if (!mem.buffer.isEmpty())
      mem.buffer.dispose(device);
    return {};
  }
  return mem;
}

struct RIBlockMem RIAccelScratchAllocHandler(struct RIDevice *device,
                                             struct RIScratchAlloc *scratch,
                                             size_t size) {
  struct RIBlockMem mem = {};
  size_t backingSize = 0;
  if (!RIScratchBackingSize(device, scratch, size, &backingSize))
    return mem;
  // AS build scratch is GPU-only: mappedAddress may remain NULL, but the
  // allocation must have a valid GPU address.
  mem.buffer = RIBuffer::create(
      device,
      {(uint64_t)backingSize,
       RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_DEVICE_ADDRESS,
       RI_MEMORY_DEVICE, RIScratchBufferAlignment(device, scratch)});
  if (!mem.buffer.isEmpty())
    mem.deviceAddress = mem.buffer.GetDeviceHandle(device);
  if (mem.buffer.isEmpty() || mem.deviceAddress == 0) {
    if (!mem.buffer.isEmpty())
      mem.buffer.dispose(device);
    return {};
  }
  return mem;
}

void InitRIScratchAlloc(struct RIDevice *device, struct RIScratchAlloc *pool,
                        const struct RIScratchAllocDesc *desc) {
  if (pool == nullptr)
    return;
  memset(pool, 0, sizeof(struct RIScratchAlloc));
  if (desc == nullptr || !RIScratchValidAlignment(desc->alignmentReq) ||
      desc->blockSize == 0 || desc->alloc == nullptr)
    return;
  pool->alignmentReq = desc->alignmentReq;
  pool->blockSize = desc->blockSize;
  pool->alloc = desc->alloc;
}

static inline bool __isPoolSlotEmpty(struct RIDevice *device,
                                     struct RIBlockMem *block) {
  (void)device;
  return block->buffer.isEmpty();
}

static inline bool __isValidBlock(const struct RIBlockMem *block,
                                  bool requiresMappedAddress) {
  return !block->buffer.isEmpty() && block->deviceAddress != 0 &&
         (!requiresMappedAddress || block->buffer.mappedAddress != nullptr);
}

static inline void __FreeRIBlockMem(struct RIDevice *device,
                                    struct RIBlockMem *block) {
  if (!block->buffer.isEmpty()) {
    block->buffer.dispose(device);
  }
}

void FreeRIScratchAlloc(struct RIDevice *device, struct RIScratchAlloc *pool) {
  if (!pool->current.buffer.isEmpty()) {
    __FreeRIBlockMem(device, &pool->current);
  }

  for (size_t i = 0; i < static_cast<size_t>(arrlen(pool->recycle)); i++) {
    __FreeRIBlockMem(device, &pool->recycle[i]);
  }

  for (size_t i = 0; i < static_cast<size_t>(arrlen(pool->pool)); i++) {
    __FreeRIBlockMem(device, &pool->pool[i]);
  }

  for (size_t i = 0; i < static_cast<size_t>(arrlen(pool->oversized)); i++) {
    __FreeRIBlockMem(device, &pool->oversized[i]);
  }
  arrfree(pool->recycle);
  arrfree(pool->pool);
  arrfree(pool->oversized);
}

void RIResetScratchAlloc(struct RIDevice *device, struct RIScratchAlloc *pool) {
  for (size_t i = 0; i < static_cast<size_t>(arrlen(pool->recycle)); i++) {
    arrpush(pool->pool, pool->recycle[i]);
  }
  arrsetlen(pool->recycle, 0);

  // Oversized one-shots can't be reused — they don't match blockSize.
  // Free them outright; a caller asking for the same size next frame
  // will pay for a fresh allocation.
  for (size_t i = 0; i < static_cast<size_t>(arrlen(pool->oversized)); i++) {
    __FreeRIBlockMem(device, &pool->oversized[i]);
  }
  arrsetlen(pool->oversized, 0);

  pool->blockOffset = 0;
}

size_t RINumberOfUsedBlock(struct RIDevice *device,
                           struct RIScratchAlloc *pool) {
  size_t numBlock = 0;
  if (!__isPoolSlotEmpty(device, &pool->current)) {
    numBlock++;
  }
  numBlock += arrlen(pool->recycle);
  numBlock += arrlen(pool->oversized);
  return numBlock;
}
struct RIBlockMem *RIGetUsedBlock(struct RIDevice *device,
                                  struct RIScratchAlloc *pool, size_t index) {
  // Iteration order: current (if any) → recycle[..] → oversized[..].
  size_t cursor = 0;
  if (!__isPoolSlotEmpty(device, &pool->current)) {
    if (index == 0)
      return &pool->current;
    cursor = 1;
  }
  const size_t recycleEnd = cursor + (size_t)arrlen(pool->recycle);
  if (index < recycleEnd)
    return &pool->recycle[index - cursor];
  return &pool->oversized[index - recycleEnd];
}

struct RIBufferScratchAllocReq
RIAllocBufferFromScratchAlloc(struct RIDevice *device,
                              struct RIScratchAlloc *pool, size_t reqSize) {
  struct RIBufferScratchAllocReq empty = {};
  if (pool == nullptr || pool->alloc == nullptr || pool->blockSize == 0 ||
      !RIScratchValidAlignment(pool->alignmentReq) || reqSize == 0)
    return empty;
  size_t alignReqSize = 0;
  if (!RIScratchAlignedSize(reqSize, pool->alignmentReq, &alignReqSize))
    return empty;
  const bool requiresMappedAddress =
      pool->alloc == RIUniformScratchAllocHandler;

  // Oversized one-shot: allocate a block sized exactly to this request,
  // stash it on the oversized list so reset frees it, and hand the whole
  // thing back. Leaves pool->current untouched so any subsequent normal
  // allocation keeps filling it at the same offset.
  if (alignReqSize > pool->blockSize) {
    size_t backingSize = 0;
    if (!RIScratchBackingSize(device, pool, alignReqSize, &backingSize))
      return empty;
    struct RIBlockMem oneShot = pool->alloc(device, pool, alignReqSize);
    if (!__isValidBlock(&oneShot, requiresMappedAddress)) {
      if (!oneShot.buffer.isEmpty())
        oneShot.buffer.dispose(device);
      return {};
    }
    uint64_t curBda = oneShot.deviceAddress;
    uint64_t alignedBda = 0;
    if (!RIScratchAlignedAddress(curBda, pool->alignmentReq, &alignedBda)) {
      oneShot.buffer.dispose(device);
      return empty;
    }
    const uint64_t bufferOffset64 = alignedBda - curBda;
    if (bufferOffset64 > backingSize ||
        (uint64_t)reqSize > (uint64_t)backingSize - bufferOffset64) {
      oneShot.buffer.dispose(device);
      return empty;
    }
    arrpush(pool->oversized, oneShot);

    struct RIBufferScratchAllocReq req = {};
    req.block = oneShot;
    req.pMappedAddress = oneShot.buffer.mappedAddress;
    req.deviceAddress = alignedBda;
    req.bufferOffset = (size_t)bufferOffset64;
    req.bufferSize = reqSize;
    return req;
  }

  if (__isPoolSlotEmpty(device, &pool->current)) {
    pool->current = pool->alloc(device, pool, pool->blockSize);
    pool->blockOffset = 0;
    if (!__isValidBlock(&pool->current, requiresMappedAddress)) {
      if (!pool->current.buffer.isEmpty())
        pool->current.buffer.dispose(device);
      pool->current = {};
      return {};
    }
  }

  // BDA-anchored alignment: pad blockOffset so (BDA + offset) is a multiple
  // of pool->alignmentReq. The buffer's base address may still need this
  // correction even when an explicit alignment was requested.
  size_t alignedStartOffset = 0;
  size_t logicalBlockEnd = 0;
  {
    uint64_t blockBaseBda = pool->current.deviceAddress;
    uint64_t alignedStartBda = 0;
    if (!RIScratchAlignedAddress(blockBaseBda, pool->alignmentReq,
                                 &alignedStartBda) ||
        alignedStartBda - blockBaseBda > std::numeric_limits<size_t>::max())
      return empty;
    alignedStartOffset = (size_t)(alignedStartBda - blockBaseBda);
    if (!RIScratchAddSize(alignedStartOffset, pool->blockSize,
                          &logicalBlockEnd))
      return empty;

    if (pool->blockOffset >
        std::numeric_limits<uint64_t>::max() - pool->current.deviceAddress)
      return empty;
    uint64_t curBda = pool->current.deviceAddress + pool->blockOffset;
    uint64_t alignedBda = 0;
    if (!RIScratchAlignedAddress(curBda, pool->alignmentReq, &alignedBda) ||
        alignedBda - curBda >
            std::numeric_limits<size_t>::max() - pool->blockOffset)
      return empty;
    pool->blockOffset += (size_t)(alignedBda - curBda);
  }

  if (pool->blockOffset > logicalBlockEnd ||
      alignReqSize > logicalBlockEnd - pool->blockOffset) {
    const size_t poolSize = arrlen(pool->pool);
    struct RIBlockMem next = {};
    if (poolSize > 0) {
      next = pool->pool[poolSize - 1];
      arrsetlen(pool->pool, poolSize - 1);
    } else {
      next = pool->alloc(device, pool, pool->blockSize);
    }
    if (!__isValidBlock(&next, requiresMappedAddress)) {
      if (!next.buffer.isEmpty())
        next.buffer.dispose(device);
      return {};
    }
    arrpush(pool->recycle, pool->current);
    pool->current = next;
    pool->blockOffset = 0;
    // Re-anchor offset to the new/recycled block's BDA.
    uint64_t curBda = pool->current.deviceAddress;
    uint64_t alignedBda = 0;
    if (!RIScratchAlignedAddress(curBda, pool->alignmentReq, &alignedBda) ||
        alignedBda - curBda > std::numeric_limits<size_t>::max())
      return empty;
    alignedStartOffset = (size_t)(alignedBda - curBda);
    if (!RIScratchAddSize(alignedStartOffset, pool->blockSize,
                          &logicalBlockEnd))
      return empty;
    pool->blockOffset = alignedStartOffset;
  }

  struct RIBufferScratchAllocReq req = {};
  if (pool->blockOffset >
      std::numeric_limits<uint64_t>::max() - pool->current.deviceAddress)
    return empty;
  req.block = pool->current;
  req.pMappedAddress = pool->current.buffer.mappedAddress;
  req.deviceAddress = pool->current.deviceAddress + pool->blockOffset;
  req.bufferOffset = pool->blockOffset;
  req.bufferSize = reqSize;
  pool->blockOffset += alignReqSize;
  return req;
}

void RIFinishScrachReq(struct RIDevice *device,
                       struct RIBufferScratchAllocReq *req) {
  if (req == nullptr || req->pMappedAddress == nullptr)
    return;
  req->block.buffer.flushMappedRange(device, req->bufferOffset,
                                     req->bufferSize);
}
