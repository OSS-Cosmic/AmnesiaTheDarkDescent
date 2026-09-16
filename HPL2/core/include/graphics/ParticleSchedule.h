#ifndef HPL_PARTICLE_SCHEDULE_H
#define HPL_PARTICLE_SCHEDULE_H

#include <cstdint>
#include <vector>

namespace hpl {

// CPU-side particle bookkeeping for one GPU-driven emitter.
//
// When simulation moves to the GPU the host can no longer count live particles
// by walking an array, but cParticleEmitter_UserData's lifetime contract depends
// on that count in ways that are easy to get subtly wrong:
//
//   * Kill() sets mbRespawn = false, NOT mbDying
//     (ParticleEmitter_UserData.cpp:646);
//   * IsDying() is overridden to !mbRespawn || mbDying (:635);
//   * mbDying is only ever set when mlMaxParticles decrements to zero inside the
//     update, or by KillInstantly();
//   * cWorld destroys a system on GetRemoveWhenDead() && IsDead()
//     (World.cpp:2205), and three LuxSaved* paths gate on IsDying().
//
// So this reproduces the count exactly rather than approximating it. Every
// input was already host-side; only the integration needed to move.
//
// It keeps a DENSE array of live slots and walks it with UpdateMotion's own
// loop shape, because the engine's particle lifetime is order-dependent in a
// way a priority queue cannot reproduce: the loop is
// `for(i = 0; i < mlNumOfParticles; ++i)` and SwapRemove decrements
// mlNumOfParticles (ParticleEmitter.cpp:852), so the particle swapped down into
// index i is skipped for the rest of the frame and gets one extra frame of
// life. An expiry heap is asymptotically nicer but drops that quirk, which
// makes IsDead() fire up to a frame EARLY -- the unsafe direction. A dense
// mirror is exact by construction, and its per-tick cost is one float subtract
// and compare per live particle, against today's trig / Normalize / pow.
//
// Deliberately free of engine and graphics dependencies so it can be unit
// tested headlessly.
class cParticleSchedule {
public:
  static constexpr uint32_t kInvalidSlot = 0xffffffffu;

  // What the owning emitter wants done with a particle that just expired.
  // Which one applies is emitter policy (respawn / paused / max-particle
  // accounting), so the decision stays with the caller -- it is the same branch
  // that drives mlMaxParticles and mbDying.
  class iDeathHandler {
  public:
    virtual ~iDeathHandler() {}
    // Return true to respawn this slot in place, writing the new life span to
    // apNewLife; false to retire the slot. Respawn-in-place mirrors
    // SetParticleDefaults being called on the same cParticle rather than the
    // particle being removed and a new one created
    // (ParticleEmitter_UserData.cpp:1070).
    virtual bool OnParticleDied(uint32_t alSlot, float *apNewLife) = 0;
  };

  // Drops all state and sizes the free pool to alCapacity.
  void Reset(uint32_t alCapacity);

  uint32_t Capacity() const { return mlCapacity; }
  uint32_t LiveCount() const { return (uint32_t)mvLive.size(); }
  bool IsFull() const { return mvLive.size() >= mlCapacity; }

  // Claims a free slot with the given life span. Returns kInvalidSlot when the
  // emitter is at capacity, the same condition that stops today's creation loop
  // (ParticleEmitter_UserData.cpp:911).
  uint32_t Spawn(float afLifeSpan);

  // One tick of UpdateMotion's life branch over every live particle.
  void Tick(float afTimeStep, iDeathHandler *apHandler);

  // Drops every live particle. KillInstantly() (ParticleEmitter.cpp) relies on
  // the count going straight to zero.
  void Clear();

  bool IsLive(uint32_t alSlot) const;
  // Remaining life for a live slot, or zero. Only used by tests and debug
  // overlays; the GPU owns the authoritative value.
  float LifeOf(uint32_t alSlot) const;

  // Live slots in engine order. The expand pass does not need this -- it walks
  // the pool slice directly -- but the spawn command builder does.
  const std::vector<uint32_t> &LiveSlots() const { return mvLive; }

private:
  uint32_t mlCapacity = 0;

  // Dense, in the same order mvParticles holds live particles today.
  std::vector<uint32_t> mvLive;
  // Parallel to mvLive: remaining life, counting down exactly as
  // cParticle::mfLife does.
  std::vector<float> mvLife;
  // Slots not currently in mvLive.
  std::vector<uint32_t> mvFreeSlots;
  // Position in mvLive for a live slot, or kInvalidSlot.
  std::vector<uint32_t> mvSlotIndex;
};

} // namespace hpl

#endif // HPL_PARTICLE_SCHEDULE_H
