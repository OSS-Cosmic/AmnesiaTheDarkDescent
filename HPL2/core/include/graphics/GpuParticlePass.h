#pragma once

#include "graphics/Graphics.h"
#include "graphics/RIProgram.h"
#include "graphics/RITextureView.h"

#include <cstdint>
#include <memory>

namespace hpl {

class cResources;

// The three GPU particle compute pipelines, in one place.
//
// Pipeline construction is abstracted here so the backends do not each
// re-implement program loading, descriptor naming and push-constant layout.
// RECORDING stays with the backend: cStandardRenderer and cHybridRenderer each
// own an instance and issue their own dispatches, because each one knows where
// in its own frame the work belongs and which scene depth to hand the collision
// sweep. Only one backend is live in a process (cGraphics picks one at Init),
// so the duplicated programs cost nothing at runtime.
//
// Lifecycle follows cStandardAmbientOcclusionPass: the owning renderer calls
// LoadData from its LoadData and DestroyData from its DestroyData, which is
// what makes shader hot-reload work without any extra plumbing.
class cGpuParticlePass {
public:
  // Buffers the host rebuilds every frame. All three dispatches read the same
  // emitter instance and parameter tables; offsets and ranges are in bytes so
  // these can be sub-allocations of a per-frame ring.
  struct FrameBuffers {
    RIBuffer *emitters = nullptr; // GpuEmitterInstance[]
    uint64_t emitterOffset = 0;
    uint64_t emitterRange = 0;

    RIBuffer *params = nullptr; // GpuEmitterParams[] (static table)
    uint64_t paramOffset = 0;
    uint64_t paramRange = 0;

    RIBuffer *state = nullptr; // GpuParticle[] (the persistent pool)
    uint64_t stateOffset = 0;
    uint64_t stateRange = 0;
  };

  // One entry per workgroup: {emitter slot, first slice index}. Built from a
  // prefix sum over the live emitters' capacities so the dispatch is exactly as
  // wide as the live pool rather than maxCapacity * emitterCount.
  struct WorkgroupMap {
    RIBuffer *groups = nullptr; // uint2[]
    uint64_t offset = 0;
    uint64_t range = 0;
    uint32_t groupCount = 0;
  };

  struct SpawnArgs {
    FrameBuffers buffers;
    RIBuffer *spawnCmds = nullptr; // GpuParticleSpawn[]
    uint64_t spawnOffset = 0;
    uint64_t spawnRange = 0;
    uint32_t spawnCount = 0;
  };

  struct SimArgs {
    FrameBuffers buffers;
    WorkgroupMap work;
    // Previous frame's scene depth. Required even when collision is off: a
    // reflected-but-unwritten binding trips the debug check in bindDescriptors.
    RITextureView *collisionDepth = nullptr;
    bool collisionEnabled = false;
    // View-space slab behind a depth sample still treated as solid, so a fast
    // particle cannot tunnel through a wall in one step.
    float collisionThickness = 0.5f;
  };

  struct ExpandArgs {
    FrameBuffers buffers;
    WorkgroupMap work;
    RIBuffer *sliceOffsets = nullptr; // uint[] per emitter slot, this viewport
    uint64_t sliceOffsetOffset = 0;
    uint64_t sliceOffsetRange = 0;
    RIBuffer *quadVerts = nullptr; // float[] quad ring, this viewport
    uint64_t quadOffset = 0;
    uint64_t quadRange = 0;
    // cFrustum::GetOrigin, for the per-viewport distance fade.
    float cameraOrigin[3] = {0.0f, 0.0f, 0.0f};
    // cFrustum::GetInvertsCullMode: flips the quad corners in y for a
    // reflection frustum, as BuildViewportVertices does today.
    bool invertCull = false;
  };

  cGpuParticlePass(cGraphics *apGraphics, cResources *apResources);
  ~cGpuParticlePass();

  cGpuParticlePass(const cGpuParticlePass &) = delete;
  cGpuParticlePass &operator=(const cGpuParticlePass &) = delete;

  bool LoadData();
  void DestroyData();
  bool IsLoaded() const { return m_loaded && m_spawn && m_sim && m_expand; }

  // Applies the host scheduler's spawn commands. Once per frame, before Sim, so
  // a particle created this step is also integrated this step -- UpdateMotion's
  // create-then-loop ordering.
  void RecordSpawn(RICmd *apCmd, uint32_t alFrameIndex, const SpawnArgs &aArgs);

  // Integrates every live particle. View-independent, so once per frame no
  // matter how many viewports show the world.
  void RecordSim(RICmd *apCmd, uint32_t alFrameIndex,
                 RIProgram::DescriptorBinding *apFrameBinding,
                 const SimArgs &aArgs);

  // Billboards the pool into quads for ONE viewport. View-dependent, so once
  // per viewport.
  void RecordExpand(RICmd *apCmd, uint32_t alFrameIndex,
                    RIProgram::DescriptorBinding *apFrameBinding,
                    const ExpandArgs &aArgs);

  // Barrier helpers, so both backends agree on the stage masks. Getting the
  // expand->draw one wrong is silent on some drivers and garbage geometry on
  // others, because Standard/Hybrid pull the quad streams by BDA in the vertex
  // shader while Simple/WireFrame bind the same memory as a vertex buffer.
  static void BarrierSpawnToSim(RICmd *apCmd);
  static void BarrierSimToExpand(RICmd *apCmd);
  static void BarrierExpandToDraw(RICmd *apCmd);

private:
  // Shared prologue for all three: bind the pipeline, the bindless set, and the
  // emitter/param/state buffers every one of them reads.
  void BindCommon(RICmd *apCmd, uint32_t alFrameIndex, RIProgram *apProgram,
                  const char *asDebugName, const FrameBuffers &aBuffers,
                  RIProgram::DescriptorBinding *apFrameBinding,
                  std::vector<RIProgram::DescriptorBinding> &aBindings);

  cGraphics *mpGraphics = nullptr;
  cResources *mpResources = nullptr;

  std::shared_ptr<RIProgram> m_spawn;
  std::shared_ptr<RIProgram> m_sim;
  std::shared_ptr<RIProgram> m_expand;
  bool m_loaded = false;
};

} // namespace hpl
