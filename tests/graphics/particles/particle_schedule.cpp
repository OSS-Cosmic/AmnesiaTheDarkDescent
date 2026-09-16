#include "graphics/ParticleSchedule.h"
#include "utest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

// The contract under test: with simulation on the GPU, cParticleSchedule must
// reproduce cParticleEmitter_UserData's particle bookkeeping EXACTLY, because
// mlNumOfParticles / mlMaxParticles / mbDying drive IsDead(), which drives
// cWorld's GetRemoveWhenDead() destruction (World.cpp:2205) and three
// LuxSaved* gates. A conservative approximation is not good enough: a count
// that drains even one frame early retires a system early.
//
// ReferenceEmitter below is a transcription of the creation and death branches
// of UpdateMotion (ParticleEmitter_UserData.cpp:910-921 and :1064-1086) over a
// plain array -- what the engine does today, including the SwapRemove skip.
// Each parity test drives both models from the same inputs and asserts they
// agree on every tick.

namespace {

using hpl::cParticleSchedule;

// Today's model: a dense array of live particles, SwapRemove on death.
struct ReferenceEmitter {
  std::vector<float> life;
  int maxParticles = 0;
  bool respawn = false;
  bool paused = false;
  bool dying = false;
  float createCount = 0.0f;
  float particlesPerSecond = 0.0f;

  int NumParticles() const { return (int)life.size(); }

  void Create(float dt, const std::vector<float> &lifeDraws, size_t &cursor) {
    if (paused || NumParticles() >= maxParticles)
      return;
    createCount += particlesPerSecond * dt;
    while (createCount >= 0.99999f && NumParticles() < maxParticles) {
      life.push_back(lifeDraws[cursor++]);
      createCount -= 1.0f;
    }
  }

  // Note the loop shape: `++i` runs even on removal, and the bound shrinks, so
  // the particle swapped down into index i is skipped this frame.
  void Age(float dt, const std::vector<float> &lifeDraws, size_t &cursor) {
    size_t count = life.size();
    for (size_t i = 0; i < count; ++i) {
      life[i] -= dt;
      if (life[i] > 0.0f)
        continue;

      if (respawn && !paused) {
        life[i] = lifeDraws[cursor++];
        continue;
      }

      std::swap(life[i], life[count - 1]); // SwapRemove
      --count;
      if (!respawn) {
        --maxParticles;
        if (maxParticles <= 0)
          dying = true;
      }
    }
    life.resize(count);
  }
};

// The GPU-era model: cParticleSchedule plus the same emitter-level counters.
struct ScheduledEmitter : cParticleSchedule::iDeathHandler {
  cParticleSchedule schedule;
  int maxParticles = 0;
  bool respawn = false;
  bool paused = false;
  bool dying = false;
  float createCount = 0.0f;
  float particlesPerSecond = 0.0f;

  const std::vector<float> *draws = nullptr;
  size_t *cursor = nullptr;

  int NumParticles() const { return (int)schedule.LiveCount(); }

  void Reset(int capacity) {
    schedule.Reset((uint32_t)capacity);
    maxParticles = capacity;
  }

  bool OnParticleDied(uint32_t, float *apNewLife) override {
    if (respawn && !paused) {
      *apNewLife = (*draws)[(*cursor)++];
      return true;
    }
    if (!respawn) {
      --maxParticles;
      if (maxParticles <= 0)
        dying = true;
    }
    return false;
  }

  void Create(float dt, const std::vector<float> &lifeDraws, size_t &c) {
    if (paused || NumParticles() >= maxParticles)
      return;
    createCount += particlesPerSecond * dt;
    while (createCount >= 0.99999f && NumParticles() < maxParticles) {
      if (schedule.Spawn(lifeDraws[c]) == cParticleSchedule::kInvalidSlot)
        break;
      ++c;
      createCount -= 1.0f;
    }
  }

  void Age(float dt, const std::vector<float> &lifeDraws, size_t &c) {
    draws = &lifeDraws;
    cursor = &c;
    schedule.Tick(dt, this);
  }
};

// Deterministic pseudo-random life spans, shared by both models so the only
// difference under test is the bookkeeping.
std::vector<float> MakeLifeDraws(size_t count, float lo, float hi,
                                 uint32_t seed) {
  std::vector<float> out;
  out.reserve(count);
  uint32_t state = seed;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1664525u + 1013904223u;
    const float u = (float)(state >> 8) * (1.0f / 16777216.0f);
    out.push_back(lo + u * (hi - lo));
  }
  return out;
}

struct FreeAll : cParticleSchedule::iDeathHandler {
  bool OnParticleDied(uint32_t, float *) override { return false; }
};

struct RespawnWith : cParticleSchedule::iDeathHandler {
  float life;
  explicit RespawnWith(float l) : life(l) {}
  bool OnParticleDied(uint32_t, float *apNewLife) override {
    *apNewLife = life;
    return true;
  }
};

} // namespace

UTEST(ParticleSchedule, SpawnHandsOutDistinctSlotsAndTracksLiveCount) {
  cParticleSchedule schedule;
  schedule.Reset(4);
  ASSERT_EQ(4u, schedule.Capacity());
  ASSERT_EQ(0u, schedule.LiveCount());

  std::vector<uint32_t> slots;
  for (int i = 0; i < 4; ++i) {
    const uint32_t slot = schedule.Spawn(1.0f + (float)i);
    ASSERT_NE(cParticleSchedule::kInvalidSlot, slot);
    ASSERT_TRUE(schedule.IsLive(slot));
    slots.push_back(slot);
  }
  ASSERT_EQ(4u, schedule.LiveCount());
  ASSERT_TRUE(schedule.IsFull());

  std::sort(slots.begin(), slots.end());
  ASSERT_TRUE(std::unique(slots.begin(), slots.end()) == slots.end());

  // At capacity the emitter stops creating, as the while-loop guard in
  // UpdateMotion does.
  ASSERT_EQ(cParticleSchedule::kInvalidSlot, schedule.Spawn(9.0f));
}

UTEST(ParticleSchedule, TickAgesParticlesAndRetiresThemAtZero) {
  cParticleSchedule schedule;
  schedule.Reset(2);
  const uint32_t slot = schedule.Spawn(0.25f);
  FreeAll freeAll;

  schedule.Tick(0.1f, &freeAll);
  ASSERT_EQ(1u, schedule.LiveCount());
  ASSERT_TRUE(schedule.IsLive(slot));

  schedule.Tick(0.1f, &freeAll);
  ASSERT_EQ(1u, schedule.LiveCount());

  schedule.Tick(0.1f, &freeAll); // life goes negative
  ASSERT_EQ(0u, schedule.LiveCount());
  ASSERT_FALSE(schedule.IsLive(slot));

  // The retired slot is reusable.
  ASSERT_NE(cParticleSchedule::kInvalidSlot, schedule.Spawn(1.0f));
  ASSERT_EQ(1u, schedule.LiveCount());
}

UTEST(ParticleSchedule, RespawnKeepsTheSameSlotLive) {
  cParticleSchedule schedule;
  schedule.Reset(2);
  const uint32_t slot = schedule.Spawn(0.1f);
  RespawnWith respawn(0.5f);

  schedule.Tick(0.2f, &respawn);
  ASSERT_EQ(1u, schedule.LiveCount());
  ASSERT_TRUE(schedule.IsLive(slot));
  // Re-armed in place, not removed and recreated.
  ASSERT_EQ(slot, schedule.LiveSlots()[0]);
  ASSERT_TRUE(schedule.LifeOf(slot) > 0.49f);
}

UTEST(ParticleSchedule, ClearDropsEveryLiveParticle) {
  cParticleSchedule schedule;
  schedule.Reset(3);
  schedule.Spawn(1.0f);
  schedule.Spawn(1.0f);
  ASSERT_EQ(2u, schedule.LiveCount());

  // KillInstantly() takes the count straight to zero.
  schedule.Clear();
  ASSERT_EQ(0u, schedule.LiveCount());

  for (int i = 0; i < 3; ++i)
    ASSERT_NE(cParticleSchedule::kInvalidSlot, schedule.Spawn(1.0f));
  ASSERT_TRUE(schedule.IsFull());
}

UTEST(ParticleSchedule, MatchesReferenceCountsForARespawningEmitter) {
  const std::vector<float> draws = MakeLifeDraws(400000, 0.4f, 2.5f, 12345u);

  ReferenceEmitter reference;
  ScheduledEmitter scheduled;
  reference.maxParticles = 64;
  reference.respawn = true;
  reference.particlesPerSecond = 40.0f;
  scheduled.Reset(64);
  scheduled.respawn = true;
  scheduled.particlesPerSecond = 40.0f;

  size_t referenceCursor = 0, scheduledCursor = 0;
  const float dt = 1.0f / 60.0f;
  for (int frame = 0; frame < 4000; ++frame) {
    reference.Create(dt, draws, referenceCursor);
    reference.Age(dt, draws, referenceCursor);
    scheduled.Create(dt, draws, scheduledCursor);
    scheduled.Age(dt, draws, scheduledCursor);

    ASSERT_EQ(reference.NumParticles(), scheduled.NumParticles());
    ASSERT_EQ(reference.maxParticles, scheduled.maxParticles);
    ASSERT_EQ(reference.dying, scheduled.dying);
  }

  ASSERT_EQ(64, reference.NumParticles());
  ASSERT_FALSE(scheduled.dying);
}

UTEST(ParticleSchedule, MatchesReferenceIncludingTheDyingTransition) {
  const std::vector<float> draws = MakeLifeDraws(400000, 0.2f, 1.1f, 99u);

  ReferenceEmitter reference;
  ScheduledEmitter scheduled;
  reference.maxParticles = 32;
  reference.respawn = false; // Kill() semantics: mbRespawn = false
  reference.particlesPerSecond = 25.0f;
  scheduled.Reset(32);
  scheduled.respawn = false;
  scheduled.particlesPerSecond = 25.0f;

  size_t referenceCursor = 0, scheduledCursor = 0;
  const float dt = 1.0f / 60.0f;
  int referenceDyingFrame = -1, scheduledDyingFrame = -1;

  for (int frame = 0; frame < 4000; ++frame) {
    reference.Create(dt, draws, referenceCursor);
    reference.Age(dt, draws, referenceCursor);
    scheduled.Create(dt, draws, scheduledCursor);
    scheduled.Age(dt, draws, scheduledCursor);

    ASSERT_EQ(reference.NumParticles(), scheduled.NumParticles());
    ASSERT_EQ(reference.maxParticles, scheduled.maxParticles);

    if (reference.dying && referenceDyingFrame < 0)
      referenceDyingFrame = frame;
    if (scheduled.dying && scheduledDyingFrame < 0)
      scheduledDyingFrame = frame;
  }

  // IsDead() is mlNumOfParticles == 0 && mbDying, so both the count and the
  // frame on which mbDying flips must agree, or cWorld destroys the system at
  // the wrong time.
  ASSERT_TRUE(reference.dying);
  ASSERT_EQ(referenceDyingFrame, scheduledDyingFrame);
  ASSERT_EQ(0, reference.NumParticles());
  ASSERT_EQ(0, scheduled.NumParticles());
}

UTEST(ParticleSchedule, MatchesReferenceAcrossPauseTransitions) {
  const std::vector<float> draws = MakeLifeDraws(400000, 0.3f, 1.4f, 4242u);

  ReferenceEmitter reference;
  ScheduledEmitter scheduled;
  reference.maxParticles = 48;
  reference.respawn = true;
  reference.particlesPerSecond = 30.0f;
  scheduled.Reset(48);
  scheduled.respawn = true;
  scheduled.particlesPerSecond = 30.0f;

  size_t referenceCursor = 0, scheduledCursor = 0;
  const float dt = 1.0f / 60.0f;
  for (int frame = 0; frame < 3000; ++frame) {
    // A pause window: a respawning emitter retires expiring particles instead
    // of re-arming them, so the population drains and then refills.
    const bool paused = (frame / 120) % 2 == 1;
    reference.paused = paused;
    scheduled.paused = paused;

    reference.Create(dt, draws, referenceCursor);
    reference.Age(dt, draws, referenceCursor);
    scheduled.Create(dt, draws, scheduledCursor);
    scheduled.Age(dt, draws, scheduledCursor);

    ASSERT_EQ(reference.NumParticles(), scheduled.NumParticles());
  }
}

UTEST(ParticleSchedule, MatchesReferenceAtAVarietyOfRatesAndLifeSpans) {
  struct Case {
    int capacity;
    float perSecond;
    float minLife;
    float maxLife;
    bool respawn;
  };
  // Shapes drawn from the shipped .ps census: median capacity 15, p95 75, one
  // 5000 outlier, rates from a trickle to a burst.
  const Case cases[] = {
      {15, 5.0f, 0.5f, 1.0f, true},   {15, 5.0f, 0.5f, 1.0f, false},
      {75, 120.0f, 0.05f, 0.2f, true}, {75, 120.0f, 0.05f, 0.2f, false},
      {1, 1.0f, 2.0f, 2.0f, true},     {1, 60.0f, 0.016f, 0.016f, false},
      {200, 0.5f, 5.0f, 9.0f, true},   {200, 400.0f, 0.01f, 0.05f, false},
  };

  const std::vector<float> unitDraws = MakeLifeDraws(400000, 0.0f, 1.0f, 777u);
  const float dt = 1.0f / 60.0f;

  for (const Case &c : cases) {
    std::vector<float> draws;
    draws.reserve(unitDraws.size());
    for (float u : unitDraws)
      draws.push_back(c.minLife + u * (c.maxLife - c.minLife));

    ReferenceEmitter reference;
    ScheduledEmitter scheduled;
    reference.maxParticles = c.capacity;
    reference.respawn = c.respawn;
    reference.particlesPerSecond = c.perSecond;
    scheduled.Reset(c.capacity);
    scheduled.respawn = c.respawn;
    scheduled.particlesPerSecond = c.perSecond;

    size_t referenceCursor = 0, scheduledCursor = 0;
    for (int frame = 0; frame < 1500; ++frame) {
      reference.Create(dt, draws, referenceCursor);
      reference.Age(dt, draws, referenceCursor);
      scheduled.Create(dt, draws, scheduledCursor);
      scheduled.Age(dt, draws, scheduledCursor);

      ASSERT_EQ(reference.NumParticles(), scheduled.NumParticles());
      ASSERT_EQ(reference.maxParticles, scheduled.maxParticles);
      ASSERT_EQ(reference.dying, scheduled.dying);
    }
  }
}

UTEST_MAIN();
