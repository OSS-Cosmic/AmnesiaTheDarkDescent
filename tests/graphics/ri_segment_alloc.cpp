// Headless coverage for RISegmentAlloc's multi-frame ring behaviour.
//
// These exist because the Standard renderer's shadow rings were sized to one
// Draw's worth of elements while the allocator hands the SAME ring to every
// frame in flight: a segment is only reclaimed once the frame that took it is
// numSegments old. A ring sized to a single frame's budget therefore starts
// refusing requests partway through the second frame, and the renderer reads
// that refusal as "this light gets no shadow" (or, for the contiguous tile and
// group publishes, "no light gets a shadow this frame"), which is what made
// shadows flash in rooms with many shadowed point lights.
//
// The invariant under test: a ring sized for framesInFlight + 1 sustains a
// per-frame request of its nominal budget forever, and one sized for a single
// frame does not.

#include "graphics/RISegmentAlloc.h"
#include "utest.h"

#include <cstdint>
#include <vector>

namespace {

constexpr size_t kSegments = 6; // RI_NUMBER_FRAME_SEGMENTS
constexpr uint16_t kFramesInFlight = 2; // RI_NUMBER_FRAMES_FLIGHT

RISegmentAlloc<kSegments> MakeRing(uint32_t maxElements) {
  RISegmentAllocDesc desc = {};
  desc.numSegments = kFramesInFlight;
  desc.elementStride = sizeof(uint32_t);
  desc.maxElements = maxElements;
  return RISegmentAlloc<kSegments>(&desc);
}

// Drives `frames` frames, each asking for `perFrame` elements in `requests`
// separate calls, and returns the first frame index that could not be served
// in full. Returns -1 when every frame was served.
int FirstStarvedFrame(uint32_t maxElements, uint32_t perFrame,
                      uint32_t requests, uint32_t frames) {
  RISegmentAlloc<kSegments> ring = MakeRing(maxElements);
  const uint32_t chunk = perFrame / requests;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    for (uint32_t r = 0; r < requests; ++r) {
      RISegmentReq req = {};
      if (!ring.request(frame, chunk, &req))
        return static_cast<int>(frame);
    }
  }
  return -1;
}

} // namespace

// The defect this file exists for. A ring sized to exactly one frame's budget
// is full once that frame has taken its slice, and a full ring has its head
// and tail offsets equal -- exactly like an empty one. Reading that as "empty"
// handed the next frame the range its predecessor was still reading, so every
// frame staged over the in-flight frame's cull tiles, candidates and draw
// counts. A full ring must refuse instead.
UTEST(ri_segment_alloc, full_ring_refuses_rather_than_aliasing_an_inflight_frame) {
  RISegmentAlloc<kSegments> ring = MakeRing(1024);
  RISegmentReq first = {};
  ASSERT_TRUE(ring.request(0, 1024, &first));
  ASSERT_EQ(0u, first.elementOffset);

  // Frame 1 cannot be served: frame 0 is still in flight and owns every
  // element. Before the fix this returned true with elementOffset 0.
  RISegmentReq second = {};
  ASSERT_FALSE(ring.request(1, 1024, &second));
  // Not even a single element is free.
  ASSERT_FALSE(ring.request(1, 1, &second));

  // Frame 2 may reclaim frame 0, so the ring opens up again.
  RISegmentReq third = {};
  ASSERT_TRUE(ring.request(2, 1024, &third));
}

// The same starvation stated through the frame driver, including the case
// where the budget is spent in many small requests -- how the per-tile draw
// count and indirect rings are consumed.
UTEST(ri_segment_alloc, single_frame_ring_starves_the_next_frame) {
  ASSERT_EQ(1, FirstStarvedFrame(/*maxElements*/ 1024, /*perFrame*/ 1024,
                                 /*requests*/ 1, /*frames*/ 8));
  ASSERT_EQ(1, FirstStarvedFrame(1024, 1024, 512, 8));
}

// The threshold is framesInFlight frames' worth, not one frame's worth: a ring
// that can hold every in-flight frame at once sustains, and one that cannot
// starves as soon as the second frame asks. 512 x 2 fits in 1024; 600 x 2 does
// not.
UTEST(ri_segment_alloc, ring_must_hold_every_inflight_frame_at_once) {
  ASSERT_EQ(-1, FirstStarvedFrame(1024, 1024 / kFramesInFlight, 1, 64));
  ASSERT_EQ(1, FirstStarvedFrame(1024, 600, 1, 8));
}

// The invariant the renderer actually depends on: while a frame is in flight,
// no later frame may be handed any element it owns. Walks many frames of
// varying demand against a correctly sized ring and checks every served range
// against the ranges the still-in-flight frames hold.
UTEST(ri_segment_alloc, served_ranges_never_alias_an_inflight_frame) {
  const uint32_t perFrame = 256;
  const uint32_t maxElements = perFrame * (kFramesInFlight + 1);
  RISegmentAlloc<kSegments> ring = MakeRing(maxElements);

  // Owner of each element, by frame index; -1 when free.
  std::vector<int> owner(maxElements, -1);

  for (uint32_t frame = 0; frame < 256; ++frame) {
    // A frame is reclaimable once it is kFramesInFlight old.
    for (uint32_t e = 0; e < maxElements; ++e)
      if (owner[e] >= 0 && frame >= static_cast<uint32_t>(owner[e]) + kFramesInFlight)
        owner[e] = -1;

    const uint32_t want = 1 + (frame * 37) % perFrame;
    RISegmentReq req = {};
    if (!ring.request(frame, want, &req))
      continue;
    ASSERT_EQ(want, req.numElements);
    ASSERT_LE(req.elementOffset + want, maxElements);
    for (uint32_t e = req.elementOffset; e < req.elementOffset + want; ++e) {
      ASSERT_EQ(-1, owner[e]); // must not belong to a frame still in flight
      owner[e] = static_cast<int>(frame);
    }
  }
}

// Sized for every frame in flight plus one frame of slack, the same per-frame
// budget is served indefinitely. The extra frame covers the tail the allocator
// discards when a contiguous request will not fit before the end of the ring.
UTEST(ri_segment_alloc, ring_sized_for_frames_in_flight_sustains_budget) {
  const uint32_t perFrame = 1024;
  const uint32_t ring = perFrame * (kFramesInFlight + 1);
  ASSERT_EQ(-1, FirstStarvedFrame(ring, perFrame, 1, 64));
  ASSERT_EQ(-1, FirstStarvedFrame(ring, perFrame, 512, 64));
  // A large contiguous request -- the shadow tile and group publishes are one
  // call for the whole Draw -- is the case the discard path exists for.
  ASSERT_EQ(-1, FirstStarvedFrame(ring, perFrame, 2, 64));
}

// Frames that vary in size, which is what a moving camera produces, must not
// desynchronise the reclaim. Alternate a full and a light frame.
UTEST(ri_segment_alloc, ring_survives_varying_per_frame_demand) {
  const uint32_t perFrame = 1024;
  RISegmentAlloc<kSegments> ring = MakeRing(perFrame * (kFramesInFlight + 1));
  for (uint32_t frame = 0; frame < 64; ++frame) {
    const uint32_t want = (frame % 2 == 0) ? perFrame : perFrame / 4;
    RISegmentReq req = {};
    ASSERT_TRUE(ring.request(frame, want, &req));
    ASSERT_LT(req.elementOffset, perFrame * (kFramesInFlight + 1));
    ASSERT_EQ(want, req.numElements);
  }
}

// A request larger than the ring itself must fail rather than hand back a
// range that runs off the end of the buffer.
UTEST(ri_segment_alloc, oversized_request_fails) {
  RISegmentAlloc<kSegments> ring = MakeRing(1024);
  RISegmentReq req = {};
  ASSERT_FALSE(ring.request(0, 1025, &req));
}

// Every served range must lie wholly inside the buffer: the renderer memcpys
// into mappedAddress + elementOffset and the cull kernel indexes with the same
// absolute offset, so an out-of-range offset is a buffer overrun.
UTEST(ri_segment_alloc, served_ranges_stay_inside_the_ring) {
  const uint32_t maxElements = 4096;
  RISegmentAlloc<kSegments> ring = MakeRing(maxElements);
  for (uint32_t frame = 0; frame < 64; ++frame) {
    for (uint32_t r = 0; r < 8; ++r) {
      RISegmentReq req = {};
      if (!ring.request(frame, 97, &req))
        continue;
      ASSERT_LE(req.elementOffset + 97u, maxElements);
    }
  }
}
