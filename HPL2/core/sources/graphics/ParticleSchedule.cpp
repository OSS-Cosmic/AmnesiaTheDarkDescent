#include "graphics/ParticleSchedule.h"

namespace hpl {

//-----------------------------------------------------------------------

void cParticleSchedule::Reset(uint32_t alCapacity) {
  mlCapacity = alCapacity;

  mvLive.clear();
  mvLife.clear();
  mvLive.reserve(alCapacity);
  mvLife.reserve(alCapacity);

  mvSlotIndex.assign(alCapacity, kInvalidSlot);

  // Pushed high-first so popping from the back hands out slot 0 upwards, which
  // keeps a mostly-empty emitter's live particles packed near the start of its
  // pool slice.
  mvFreeSlots.clear();
  mvFreeSlots.reserve(alCapacity);
  for (uint32_t i = alCapacity; i > 0; --i)
    mvFreeSlots.push_back(i - 1u);
}

//-----------------------------------------------------------------------

uint32_t cParticleSchedule::Spawn(float afLifeSpan) {
  if (mvFreeSlots.empty())
    return kInvalidSlot;

  const uint32_t slot = mvFreeSlots.back();
  mvFreeSlots.pop_back();

  mvSlotIndex[slot] = (uint32_t)mvLive.size();
  mvLive.push_back(slot);
  mvLife.push_back(afLifeSpan);
  return slot;
}

//-----------------------------------------------------------------------

void cParticleSchedule::Tick(float afTimeStep, iDeathHandler *apHandler) {
  // Deliberately mirrors UpdateMotion's loop rather than iterating to
  // mvLive.size(): `count` is the live count read fresh each comparison, and a
  // retirement both decrements it and lets ++i step past the particle swapped
  // into this index. That skipped particle gets one extra frame, which is the
  // engine's actual behaviour (see the header).
  size_t count = mvLive.size();
  for (size_t i = 0; i < count; ++i) {
    mvLife[i] -= afTimeStep;
    if (mvLife[i] > 0.0f)
      continue;

    float newLife = 0.0f;
    const bool respawn =
        apHandler && apHandler->OnParticleDied(mvLive[i], &newLife);

    if (respawn) {
      mvLife[i] = newLife;
      continue;
    }

    // SwapRemove(i): swap with the last live entry and shrink.
    const uint32_t deadSlot = mvLive[i];
    const size_t last = count - 1;
    if (i != last) {
      mvLive[i] = mvLive[last];
      mvLife[i] = mvLife[last];
      mvSlotIndex[mvLive[i]] = (uint32_t)i;
      mvLive[last] = deadSlot;
    }
    mvSlotIndex[deadSlot] = kInvalidSlot;
    mvFreeSlots.push_back(deadSlot);
    --count;
  }

  mvLive.resize(count);
  mvLife.resize(count);
}

//-----------------------------------------------------------------------

void cParticleSchedule::Clear() {
  for (uint32_t slot : mvLive) {
    mvSlotIndex[slot] = kInvalidSlot;
    mvFreeSlots.push_back(slot);
  }
  mvLive.clear();
  mvLife.clear();
}

//-----------------------------------------------------------------------

bool cParticleSchedule::IsLive(uint32_t alSlot) const {
  return alSlot < mlCapacity && mvSlotIndex[alSlot] != kInvalidSlot;
}

//-----------------------------------------------------------------------

float cParticleSchedule::LifeOf(uint32_t alSlot) const {
  if (!IsLive(alSlot))
    return 0.0f;
  return mvLife[mvSlotIndex[alSlot]];
}

//-----------------------------------------------------------------------

} // namespace hpl
