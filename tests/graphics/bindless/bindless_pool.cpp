// Headless coverage for the bindless LRU slot pools (LRUCache and its
// templated sibling LRUCacheState). No GPU, no engine: BindlessPool only needs
// IndexPool + ObjectPool + the header-only hasher.
//
// The regression these pin: the cache-hit path used to relink an entry that
// was already the most-recently-used tail, which pointed the entry's quPrev at
// itself. A later hit on that entry then failed to unlink it, orphaning its
// neighbour from the queue for good. Each orphan permanently loses one id, so
// the pool drained and every later request reported `exhausted` — which the
// renderer surfaces as "Material Slot exhausted" and a dropped draw.

// ObjectPool pulls in stb_ds; its implementation lives in this project's
// stb_ds_impl.cpp rather than the engine's System.cpp, which would drag in the
// whole engine.
#include "graphics/BindlessPool.h"
#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <set>

// ---------------------------------------------------------------- LRUCache

// Touching an entry while it is already the queue tail used to relink it to
// itself, and the next hit on that entry then failed to unlink it — which cut
// its neighbour out of the queue for good. The orphaned slot keeps its id but
// can never be reached from the LRU head again, so the pool quietly loses
// capacity until nothing can be evicted and every request reports exhausted.
// Drive that exact access pattern, then check the pool can still hold a full
// capacity's worth of live cookies.
UTEST(LRUCache, TailHitDoesNotOrphanSlots) {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 0);

  // All within one frame, so nothing is old enough to evict and each miss has
  // to take a fresh id.
  cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 2, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit on the tail
  cache.request(/*cookie*/ 4, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit from the middle

  // Every slot is now stale, so a fresh set of cookies must be able to recycle
  // all of them. One unreachable slot and the last of these reports exhausted.
  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    auto req = cache.request(/*cookie*/ 100 + i, /*frameIndex*/ 10);
    if (req.exhausted) {
      std::printf("  exhausted after %u of %u slots were recycled\n", i,
                  kCapacity);
    }
    ASSERT_FALSE_MSG(req.exhausted, "LRUCache recycled-slot loop exhausted");
    ASSERT_LT(req.id, kCapacity);
    ids.insert(req.id);
  }
  ASSERT_EQ(ids.size(), kCapacity);
}


// A working set that fits inside the pool must never exhaust it, no matter how
// the accesses are ordered. The ordering here is the one that used to orphan
// slots: touch an entry twice in a row (so it is hit while it is the tail),
// push another entry behind it, then touch it again from the middle.
UTEST(LRUCache, WorkingSetNeverExhausts) {
  constexpr uint32_t kCapacity = 8;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 0);

  for (uint32_t frame = 0; frame < 500; ++frame) {
    const hash_t a = 1000 + (frame % 4);
    const hash_t b = 2000 + (frame % 4);
    const hash_t cookies[] = {a, a, b, a, b};
    for (hash_t cookie : cookies) {
      auto req = cache.request(cookie, frame);
      if (req.exhausted) {
        std::printf("  exhausted at frame %u on cookie %llu\n", frame,
                    (unsigned long long)cookie);
      }
      ASSERT_FALSE_MSG(req.exhausted, "LRUCache working-set loop exhausted");
      ASSERT_LT(req.id, kCapacity);
    }
  }
}

// Cycling through far more cookies than the pool holds must keep working: the
// least-recently-used entry is recycled once it is older than frameInFlight.
UTEST(LRUCache, EvictionKeepsRecycling) {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 1);

  std::set<uint32_t> seenIds;
  for (uint32_t frame = 0; frame < 200; ++frame) {
    auto req = cache.request(500 + frame, frame);
    ASSERT_FALSE_MSG(req.exhausted, "LRUCache eviction loop exhausted");
    ASSERT_LT(req.id, kCapacity);
    seenIds.insert(req.id);
  }
  ASSERT_LE(seenIds.size(), kCapacity);
}

// A one-element queue makes the entry both head and tail; detaching it has to
// clear both, or the next attach links the entry to itself.
UTEST(LRUCache, SingleSlotQueue) {
  hpl::LRUCache cache(/*numElements*/ 1, /*frameInFlight*/ 0);

  auto first = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  ASSERT_TRUE(!first.exhausted && !first.found);

  auto second = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  ASSERT_TRUE(!second.exhausted && !second.found && second.id == first.id);

  auto again = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  ASSERT_TRUE(again.found && again.id == second.id);

  auto evicted = cache.request(/*cookie*/ 11, /*frameIndex*/ 9);
  ASSERT_FALSE(evicted.found);
}

// With free ids left, a new cookie takes one instead of recycling a live entry
// (recycling would throw away that entry's already-uploaded payload).
UTEST(LRUCache, FreeIdsBeatEviction) {
  hpl::LRUCache cache(/*numElements*/ 4, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 2, /*frameIndex*/ 10);
  ASSERT_TRUE(!a.exhausted && !b.exhausted && a.id != b.id);

  auto aAgain = cache.request(/*cookie*/ 1, /*frameIndex*/ 10);
  ASSERT_TRUE(aAgain.found && aAgain.id == a.id);
}

// free() must unlink before the slot memory goes back to the pool, and must
// hand the id back.
UTEST(LRUCache, FreeReleasesSlot) {
  hpl::LRUCache cache(/*numElements*/ 2, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  ASSERT_TRUE(!a.exhausted && !b.exhausted);

  cache.free(/*cookie*/ 7);

  auto reborn = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  ASSERT_TRUE(!reborn.exhausted && !reborn.found);

  auto stillThere = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  ASSERT_TRUE(stillThere.found && stillThere.id == b.id);
}

// ----------------------------------------------------- LRUCacheState<T>

// The templated copy carries its own duplicate of the queue mechanics, so it
// needs the same orphan check (see TailHitDoesNotOrphanSlots).
UTEST(LRUCacheState, TailHitDoesNotOrphanSlots) {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 0);

  cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 2, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit on the tail
  cache.request(/*cookie*/ 4, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit from the middle

  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    auto req = cache.request(/*cookie*/ 100 + i, /*frameIndex*/ 10);
    ASSERT_FALSE_MSG(req.exhausted,
                     "LRUCacheState recycled-slot loop exhausted");
    ASSERT_LT(req.id, kCapacity);
    ASSERT_NE(req.state, nullptr);
    ids.insert(req.id);
  }
  ASSERT_EQ(ids.size(), kCapacity);
}


// Same contracts against the templated copy, which carries its own duplicate
// of the queue mechanics.
UTEST(LRUCacheState, WorkingSetNeverExhausts) {
  constexpr uint32_t kCapacity = 8;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 0);

  for (uint32_t frame = 0; frame < 500; ++frame) {
    const hash_t a = 1000 + (frame % 4);
    const hash_t b = 2000 + (frame % 4);
    const hash_t cookies[] = {a, a, b, a, b};
    for (hash_t cookie : cookies) {
      auto req = cache.request(cookie, frame);
      if (req.exhausted)
        std::printf("  exhausted at frame %u on cookie %llu\n", frame,
                    (unsigned long long)cookie);
      ASSERT_FALSE_MSG(req.exhausted,
                       "LRUCacheState working-set loop exhausted");
      ASSERT_LT(req.id, kCapacity);
      ASSERT_NE(req.state, nullptr);
    }
  }
}

// The per-entry state is reset when a slot is recycled for a new cookie, and
// preserved across cache hits.
UTEST(LRUCacheState, Lifetime) {
  hpl::LRUCacheState<int> cache(/*numElements*/ 1, /*frameInFlight*/ 0);

  auto first = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  ASSERT_TRUE(!first.exhausted && first.state != nullptr);
  ASSERT_EQ(*first.state, 0);
  *first.state = 42;

  auto hit = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  ASSERT_TRUE(hit.found && hit.state != nullptr);
  ASSERT_EQ(*hit.state, 42);

  auto recycled = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  ASSERT_TRUE(!recycled.exhausted && !recycled.found && recycled.state != nullptr);
  ASSERT_EQ(*recycled.state, 0);
}

UTEST(LRUCacheState, EvictionKeepsRecycling) {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 1);

  std::set<uint32_t> seenIds;
  for (uint32_t frame = 0; frame < 200; ++frame) {
    auto req = cache.request(500 + frame, frame);
    ASSERT_FALSE_MSG(req.exhausted,
                     "LRUCacheState eviction loop exhausted");
    ASSERT_LT(req.id, kCapacity);
    ASSERT_NE(req.state, nullptr);
    seenIds.insert(req.id);
  }
  ASSERT_LE(seenIds.size(), kCapacity);
}

UTEST(LRUCacheState, FreeReleasesSlot) {
  hpl::LRUCacheState<int> cache(/*numElements*/ 2, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  ASSERT_TRUE(!a.exhausted && a.state != nullptr && !b.exhausted &&
              b.state != nullptr);
  *b.state = 5;

  cache.free(/*cookie*/ 7);

  auto reborn = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  ASSERT_TRUE(!reborn.exhausted && !reborn.found && reborn.state != nullptr);

  auto stillThere = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  ASSERT_TRUE(stillThere.found && stillThere.state != nullptr);
  ASSERT_EQ(*stillThere.state, 5);
}

UTEST_MAIN();
