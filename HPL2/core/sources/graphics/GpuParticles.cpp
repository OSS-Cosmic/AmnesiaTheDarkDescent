#include "graphics/GpuParticles.h"

#include "graphics/Graphics.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace hpl {

//-----------------------------------------------------------------------

bool cGpuParticleSystem::Enabled() {
  static const bool kEnabled = std::getenv("HPL_GPU_PARTICLES") != nullptr;
  return kEnabled;
}

//-----------------------------------------------------------------------

cGpuParticleSystem::cGpuParticleSystem(cGraphics *apGraphics)
    : mpGraphics(apGraphics) {
  if (!mpGraphics || mpGraphics->device.vk.device == VK_NULL_HANDLE)
    return;

  m_freeRuns.push_back({0u, kGpuParticleCapacity});

  if (!CreateBuffers())
    return;
  if (!SeedQuadIndexBuffer())
    return;

  m_ready = true;
}

//-----------------------------------------------------------------------

cGpuParticleSystem::~cGpuParticleSystem() {
  // The pool outlives individual frames, so park the buffers on the deferral
  // queue rather than disposing them under a frame that may still be in flight.
  if (!mpGraphics)
    return;
  m_stateBuffer = {};
  m_paramBuffer = {};
  m_quadIndexBuffer = {};
}

//-----------------------------------------------------------------------

bool cGpuParticleSystem::CreateBuffers() {
  auto create = [&](RISharedPointer<RIBuffer> &target, uint64_t size,
                    uint32_t usage, const char *name) -> bool {
    RIBufferDesc desc = {};
    desc.size = size;
    desc.usage = usage;
    desc.location = RI_MEMORY_DEVICE;
    RIBuffer buffer = RIBuffer::create(&mpGraphics->device, desc);
    if (buffer.isEmpty()) {
      Error("cGpuParticleSystem: failed to allocate %s (%llu bytes).\n", name,
            (unsigned long long)size);
      return false;
    }
    buffer.setDebugObjectName(&mpGraphics->device, name);
    target = RISharedPointer<RIBuffer>(&mpGraphics->device, buffer);
    return true;
  };

  // The state buffer is read through a descriptor by the three compute passes
  // and never by the host; DEVICE_ADDRESS is there so a future pass can pull it
  // by BDA without a re-allocation.
  if (!create(m_stateBuffer,
              (uint64_t)kGpuParticleCapacity * sizeof(GpuParticle),
              RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                  RI_BUFFER_USAGE_TRANSFER_DST | RI_BUFFER_USAGE_DEVICE_ADDRESS,
              "gParticleState"))
    return false;

  if (!create(m_paramBuffer,
              (uint64_t)kGpuEmitterParamCapacity * sizeof(GpuEmitterParams),
              RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE |
                  RI_BUFFER_USAGE_TRANSFER_DST | RI_BUFFER_USAGE_DEVICE_ADDRESS,
              "gParticleEmitterParams"))
    return false;

  if (!create(m_quadIndexBuffer,
              (uint64_t)kMaxSliceParticles * 6u * sizeof(uint32_t),
              RI_BUFFER_USAGE_INDEX_BUFFER | RI_BUFFER_USAGE_TRANSFER_DST |
                  RI_BUFFER_USAGE_DEVICE_ADDRESS,
              "gParticleQuadIndices"))
    return false;

  return true;
}

//-----------------------------------------------------------------------

bool cGpuParticleSystem::SeedQuadIndexBuffer() {
  const size_t indexCount = (size_t)kMaxSliceParticles * 6u;

  RIResourceBufferTransaction transaction = {};
  transaction.target = *m_quadIndexBuffer.Get();
  transaction.size = indexCount * sizeof(uint32_t);
  transaction.currentState = RI_RESOURCE_STATE_UNDEFINED;
  transaction.currentStages = RI_STAGE_NONE;
  transaction.postState = RI_RESOURCE_STATE_INDEX_BUFFER;
  // INDEX_BUFFER is consumed by the fixed-function input assembler, not the
  // vertex shader. RI has no explicit vertex-input stage bit, so leave the
  // hint empty and let the barrier backend derive INDEX_INPUT from the state.
  transaction.postStages = RI_STAGE_NONE;
  RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                             &transaction);
  if (!transaction.mapped.data) {
    Error("cGpuParticleSystem: could not stage the shared quad index buffer.\n");
    return false;
  }

  // Two triangles per quad: 0,1,2, 2,3,0 offset by the quad's vertex base.
  // Identical to the pattern BuildScratchGeometry rewrote every frame for every
  // emitter for every viewport (ParticleEmitter.cpp:306-312).
  uint32_t *dst = reinterpret_cast<uint32_t *>(transaction.mapped.data);
  for (uint32_t q = 0; q < kMaxSliceParticles; ++q) {
    const uint32_t s = q * 4u;
    dst[q * 6 + 0] = s;
    dst[q * 6 + 1] = s + 1u;
    dst[q * 6 + 2] = s + 2u;
    dst[q * 6 + 3] = s + 2u;
    dst[q * 6 + 4] = s + 3u;
    dst[q * 6 + 5] = s;
  }

  RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                           &transaction);
  return true;
}

//-----------------------------------------------------------------------

cGpuParticleSystem::Slice cGpuParticleSystem::AcquireSlice(uint32_t alCount) {
  Slice slice;
  if (!m_ready || alCount == 0)
    return slice;

  // Every emitter draws capacity * 6 indices out of the one shared index
  // buffer, so a slice can never exceed what that buffer addresses.
  const uint32_t wanted = std::min(alCount, kMaxSliceParticles);

  for (size_t i = 0; i < m_freeRuns.size(); ++i) {
    if (m_freeRuns[i].count < wanted)
      continue;

    slice.offset = m_freeRuns[i].offset;
    slice.count = wanted;

    if (m_freeRuns[i].count == wanted)
      m_freeRuns.erase(m_freeRuns.begin() + i);
    else {
      m_freeRuns[i].offset += wanted;
      m_freeRuns[i].count -= wanted;
    }

    m_highWater = std::max(m_highWater, slice.offset + slice.count);
    ClearSlice(slice);
    return slice;
  }

  Warning("cGpuParticleSystem: pool exhausted, %u particles requested; this "
          "emitter falls back to the CPU path.\n",
          alCount);
  return slice;
}

//-----------------------------------------------------------------------

void cGpuParticleSystem::ReleaseSlice(const Slice &aSlice) {
  if (!aSlice.valid())
    return;

  // Insert sorted, then coalesce with either neighbour. Slices are acquired
  // once per emitter and released once, so this stays short.
  auto it = std::lower_bound(m_freeRuns.begin(), m_freeRuns.end(), aSlice.offset,
                             [](const FreeRun &run, uint32_t offset) {
                               return run.offset < offset;
                             });
  it = m_freeRuns.insert(it, {aSlice.offset, aSlice.count});

  if (it + 1 != m_freeRuns.end() &&
      it->offset + it->count == (it + 1)->offset) {
    it->count += (it + 1)->count;
    m_freeRuns.erase(it + 1);
  }
  if (it != m_freeRuns.begin()) {
    auto prev = it - 1;
    if (prev->offset + prev->count == it->offset) {
      prev->count += it->count;
      m_freeRuns.erase(it);
    }
  }
}

//-----------------------------------------------------------------------

void cGpuParticleSystem::ClearSlice(const Slice &aSlice) {
  if (!aSlice.valid())
    return;

  // Zeroed slots have packed == 0, so the alive bit is clear and the expand
  // pass emits degenerate quads for them until the scheduler spawns into them.
  // Without this a recycled slice would render whatever the previous emitter
  // left behind on its first frame.
  //
  // The RI layer exposes no fillBuffer, so this stages zeros through the
  // uploader. That is fine here: a slice is acquired once per emitter, and the
  // median authored emitter is 15 particles (960 bytes).
  const size_t bytes = (size_t)aSlice.count * sizeof(GpuParticle);

  RIResourceBufferTransaction transaction = {};
  transaction.target = *m_stateBuffer.Get();
  transaction.size = bytes;
  transaction.offset = (size_t)aSlice.offset * sizeof(GpuParticle);
  // Only the very first slice owns the UNDEFINED transition; after that the
  // buffer holds other emitters' live particles that must survive this copy.
  transaction.currentState = m_everCleared ? RI_RESOURCE_STATE_UNORDERED_ACCESS
                                           : RI_RESOURCE_STATE_UNDEFINED;
  transaction.currentStages = m_everCleared ? RI_STAGE_COMPUTE : RI_STAGE_NONE;
  transaction.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
  transaction.postStages = RI_STAGE_COMPUTE;
  RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                             &transaction);
  if (!transaction.mapped.data) {
    Error("cGpuParticleSystem: could not stage a slice clear (%zu bytes).\n",
          bytes);
    return;
  }
  std::memset(transaction.mapped.data, 0, bytes);
  RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                           &transaction);
  m_everCleared = true;
}

//-----------------------------------------------------------------------

uint32_t cGpuParticleSystem::RegisterParams(const GpuEmitterParams &aParams) {
  if (!m_ready || m_paramCount >= kGpuEmitterParamCapacity) {
    Warning("cGpuParticleSystem: emitter parameter table full (%u).\n",
            kGpuEmitterParamCapacity);
    return UINT32_MAX;
  }

  const uint32_t index = m_paramCount++;

  RIResourceBufferTransaction transaction = {};
  transaction.target = *m_paramBuffer.Get();
  transaction.size = sizeof(GpuEmitterParams);
  transaction.offset = (size_t)index * sizeof(GpuEmitterParams);
  // Only the first write owns the UNDEFINED -> COPY_DST transition; afterwards
  // the buffer already holds live records that later writes must not discard.
  transaction.currentState = (index == 0) ? RI_RESOURCE_STATE_UNDEFINED
                                          : RI_RESOURCE_STATE_SHADER_RESOURCE;
  transaction.currentStages = (index == 0) ? RI_STAGE_NONE : RI_STAGE_COMPUTE;
  transaction.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
  transaction.postStages = RI_STAGE_COMPUTE;
  RI_ResourceBeginCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                             &transaction);
  if (!transaction.mapped.data) {
    Error("cGpuParticleSystem: could not stage emitter params %u.\n", index);
    --m_paramCount;
    return UINT32_MAX;
  }
  std::memcpy(transaction.mapped.data, &aParams, sizeof(GpuEmitterParams));
  RI_ResourceEndCopyBuffer(&mpGraphics->device, &mpGraphics->uploader,
                           &transaction);

  return index;
}

//-----------------------------------------------------------------------

} // namespace hpl
