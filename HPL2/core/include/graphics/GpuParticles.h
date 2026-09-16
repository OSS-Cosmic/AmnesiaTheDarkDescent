#ifndef HPL_GPU_PARTICLES_H
#define HPL_GPU_PARTICLES_H

#include "graphics/RIBuffer.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RITypes.h"

#include <cstdint>
#include <vector>

#include "Constants.h"
#include "SceneTypes.slang"

namespace hpl {

// The shared structs cross the host/shader boundary by raw memory, so drift is
// silent and catastrophic. Slang compiles these under -fvk-use-scalar-layout,
// which matches C++ natural layout for these field orders; the sizes below are
// the ones spirv-dis reports as ArrayStride for the matching StructuredBuffers.
static_assert(sizeof(GpuParticle) == 64,
              "GpuParticle must stay one cache line and match its scalar-layout "
              "ArrayStride in Standard.particleSim.cs.slang");
static_assert(sizeof(GpuEmitterParams) == 384,
              "GpuEmitterParams must match its scalar-layout ArrayStride");
static_assert(sizeof(GpuEmitterInstance) == 240,
              "GpuEmitterInstance must match its scalar-layout ArrayStride");
static_assert(sizeof(GpuParticleSpawn) == 16,
              "GpuParticleSpawn must match its scalar-layout ArrayStride");

class cGraphics;

// The C++ side of the GPU-driven particle system.
//
// Owns the one persistent particle pool, the static authored-parameter table,
// and the slice allocator that hands each live emitter a fixed range of the
// pool. Per-frame data (emitter instances, spawn commands, the workgroup map,
// the expanded quads) rides the frame rings and is not owned here.
//
// The pool is deliberately a single flat allocation with fixed per-emitter
// slices rather than a global free list of individual particles: an emitter's
// capacity is its authored MaxParticleNum and never changes, and a contiguous
// slice is what lets the expand pass write quads at stable positions and the
// draw use a constant index count (see the degenerate-quad contract in
// Standard.particleExpand.cs.slang).
class cGpuParticleSystem {
public:
  // Returned by AcquireSlice. `count == 0` means the pool is exhausted and the
  // caller must fall back to the legacy CPU path.
  struct Slice {
    uint32_t offset = 0;
    uint32_t count = 0;
    bool valid() const { return count != 0; }
  };

  explicit cGpuParticleSystem(cGraphics *apGraphics);
  ~cGpuParticleSystem();

  cGpuParticleSystem(const cGpuParticleSystem &) = delete;
  cGpuParticleSystem &operator=(const cGpuParticleSystem &) = delete;

  // False when the device is missing or a buffer failed to allocate; every
  // caller must treat that as "use the CPU path".
  bool IsReady() const { return m_ready; }

  // HPL_GPU_PARTICLES, read once at construction. Matches the
  // HPL_STANDARD_FORCE_FALLBACK precedent (StandardRenderer.cpp:771).
  static bool Enabled();

  // First-fit over the free list, coalescing on release. Emitters are
  // long-lived and the pool is several times oversized, so fragmentation is not
  // a concern in practice; an exhausted pool returns an invalid slice rather
  // than failing the emitter's construction.
  Slice AcquireSlice(uint32_t alCount);
  void ReleaseSlice(const Slice &aSlice);

  // Registers one authored <ParticleEmitter> definition and uploads it. Returns
  // the index into gParticleEmitterParams, or UINT32_MAX when the table is
  // full. Called once per emitter data at .ps load, never per instance.
  uint32_t RegisterParams(const GpuEmitterParams &aParams);

  RIBuffer *StateBuffer() { return m_stateBuffer.Get(); }
  RIBuffer *ParamBuffer() { return m_paramBuffer.Get(); }
  RIBuffer *QuadIndexBuffer() { return m_quadIndexBuffer.Get(); }

  uint32_t ParamCount() const { return m_paramCount; }
  uint32_t SliceHighWater() const { return m_highWater; }

  // Largest slice the shared quad index buffer can address. An emitter that
  // authors more than this is clamped, because every emitter draws
  // capacity * 6 indices out of one static buffer.
  static constexpr uint32_t kMaxSliceParticles = 8192;

private:
  bool CreateBuffers();
  // Fills the shared 0,1,2, 2,3,0 + q*4 index pattern once. Every emitter and
  // every renderer shares it: the pattern is position-independent because each
  // emitter's vertex base rides in its own stream handle, which is why the
  // per-frame index writes in BuildScratchGeometry (ParticleEmitter.cpp:305)
  // disappear entirely.
  bool SeedQuadIndexBuffer();
  // Zeroes a freshly acquired slice so its slots read back dead. Matches the
  // vkCmdFillBuffer seeding the light grid uses (GlobalManagedSets.cpp:157).
  void ClearSlice(const Slice &aSlice);

  struct FreeRun {
    uint32_t offset;
    uint32_t count;
  };

  cGraphics *mpGraphics = nullptr;
  bool m_ready = false;

  RISharedPointer<RIBuffer> m_stateBuffer;     // GpuParticle[kGpuParticleCapacity]
  RISharedPointer<RIBuffer> m_paramBuffer;     // GpuEmitterParams[kGpuEmitterParamCapacity]
  RISharedPointer<RIBuffer> m_quadIndexBuffer; // uint32[kMaxSliceParticles * 6]

  std::vector<FreeRun> m_freeRuns;
  uint32_t m_paramCount = 0;
  uint32_t m_highWater = 0;
  // The first slice clear owns the state buffer's UNDEFINED -> COPY_DST
  // transition; later ones must not discard other emitters' live particles.
  bool m_everCleared = false;
};

} // namespace hpl

#endif // HPL_GPU_PARTICLES_H
