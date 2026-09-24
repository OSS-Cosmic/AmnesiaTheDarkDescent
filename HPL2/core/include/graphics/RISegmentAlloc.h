#ifndef RI_RINAGE_ALLOC_H
#define RI_RINAGE_ALLOC_H

#include "RITypes.h"
#include "system/Types.h"
#include <array>
#include <cassert>

struct RISegmentAllocDesc {
  uint16_t elementStride;
  uint16_t numSegments;
  uint32_t maxElements; // element count — requests can exceed 65535
};

struct RISegmentReq {
  uint16_t elementStride;
  uint32_t elementOffset;
  uint32_t numElements;
};

template <size_t N> struct RISegmentAlloc {
  static constexpr uint32_t SEGMENTS = N;
  RISegmentAlloc() { segment.fill(Segment{0, 0}); }
  RISegmentAlloc(struct RISegmentAllocDesc *desc);
  bool request(uint32_t frameIndex, size_t numElements,
               struct RISegmentReq *req);

  uint16_t elementStride = 0;
  uint16_t numSegments = 0;
  uint32_t maxElements = 0;

  // data
  int16_t tail = 0;
  int16_t head = 1;
  uint32_t numElements = 0;
  size_t elementOffset = 0;
  struct Segment {
    uint64_t frameNum;
    size_t numElements; // size of the generation
  };
  std::array<Segment, N> segment;
};

template <size_t N>
RISegmentAlloc<N>::RISegmentAlloc(struct RISegmentAllocDesc *desc) {
  assert(desc);
  segment.fill(Segment{0, 0});
  elementStride = desc->elementStride;
  tail = 0;
  head = 1;
  numSegments = desc->numSegments;
  maxElements = desc->maxElements;
  assert(numSegments != 0);
  assert(numSegments <= N);
  assert(elementStride > 0);
}
template <size_t N>
bool RISegmentAlloc<N>::request(uint32_t frameIndex, size_t reqElements,
                                struct RISegmentReq *req) {
  if (maxElements == 0)
    return false;
  // reclaim segments that are unused
  while (tail != head && frameIndex >= (segment[tail].frameNum + numSegments)) {
    assert(numElements >= segment[tail].numElements);
    numElements -= static_cast<uint32_t>(segment[tail].numElements);
    elementOffset = (elementOffset + segment[tail].numElements) % maxElements;
    segment[tail].numElements = 0;
    segment[tail].frameNum = 0;
    tail = (tail + 1) % segment.size();
  }

  // the frame has change
  if (frameIndex != segment[head].frameNum) {
    head = (head + 1) % segment.size();
    segment[head].frameNum = frameIndex;
    segment[head].numElements = 0;
    assert(head != tail); // this shouldn't happen
  }

  if (reqElements > maxElements)
    return false;

  size_t elmentEndOffset = (elementOffset + numElements) % maxElements;
  assert(elementOffset < maxElements);
  assert(elmentEndOffset < maxElements);

  // Free space comes from the live element count, NOT from comparing the head
  // and tail offsets. When the ring is exactly full those two offsets are
  // equal, which is indistinguishable from empty: the old comparison read that
  // as "the whole buffer is free" and handed the caller a range an in-flight
  // frame was still reading. Every frame then staged over its predecessor's
  // data instead of being told the ring was exhausted.
  const size_t remainingSpace = maxElements - numElements;

  // A range must be contiguous, so one that will not fit before the end of the
  // buffer forfeits the tail and restarts at 0. The forfeited elements are
  // charged to this frame's segment, so they are reclaimed along with it, and
  // they have to be paid for out of the same free space as the request itself.
  size_t forfeited = 0;
  if (elmentEndOffset + reqElements > maxElements)
    forfeited = maxElements - elmentEndOffset;

  // there is not enough avalaible space we need to reallocate
  if (reqElements + forfeited > remainingSpace) {
    return false;
  }
  if (forfeited > 0) {
    segment[head].numElements += forfeited;
    numElements += static_cast<uint32_t>(forfeited);
    elmentEndOffset = 0;
    assert((elementOffset + numElements) % maxElements == 0);
  }
  segment[head].numElements += reqElements;
  numElements += static_cast<uint32_t>(reqElements);
  assert(numElements <= maxElements);

  req->elementOffset = static_cast<uint32_t>(elmentEndOffset);
  req->elementStride = elementStride;
  req->numElements =
      static_cast<uint32_t>(reqElements); // includes the padding on the end of the buffer
  return true;
}

static inline bool IsRISegmentBufferContinous(size_t currentOffset,
                                              size_t currentNumElements,
                                              size_t nextOffset) {
  return currentOffset + currentNumElements == nextOffset;
}

#endif
