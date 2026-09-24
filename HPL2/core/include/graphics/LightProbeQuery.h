/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_LIGHT_PROBE_QUERY_H
#define HPL_LIGHT_PROBE_QUERY_H

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIBuffer.h"
#include "math/MathTypes.h"

#include <array>
#include <cstdint>

// The RI layer lives at global scope, not in hpl -- declaring these inside the
// namespace would shadow the real types for every later `struct RICmd*`
// parameter in a translation unit that includes this header first.
struct RICmd;
struct RIDevice;

namespace hpl {

class iLight;

//------------------------------------------------------------------------
// GPU illumination sensor, CPU side.
//
// Gameplay asks "how lit is this world point?" for a handful of points; the
// renderer answers with the data it already has (the world-space light grid,
// ILight radiance, ray-traced occlusion, and one-bounce surface lighting) in
// LightProbePass.cs, and the answer comes back over a host-readback buffer.
//
// This replaces the base game's CPU sensor (cLuxMapHelper::GetLightLevelAtPos),
// which walked every light in the world and never occlusion-tested point lights
// at all, so a player standing behind cover inside a lamp's radius still read as
// fully lit.
//
// Timing: the result is the GPU's answer from a couple of frames ago
// (RI_NUMBER_FRAMES_FLIGHT), because the CPU only reads a readback slot once the
// graphics timeline says the copy has actually executed — no stall, ever. Every
// consumer of this is a slow sensor (the player's light level updates twice a
// second), so the latency does not matter; what matters is that a caller must
// cope with having NO result yet, which is the state on the first frames after a
// map load and forever on a device that never dispatched the pass. GetResult
// reports that honestly so callers can retain their last reading or startup default.
//------------------------------------------------------------------------

class cLightProbeQuery {
public:
  // Mirrors kMaxLightProbes / kMaxLightProbeExcludes in amnesia/slang/Constants.h.
  // A static_assert in the .cpp keeps them in lockstep.
  static const int kMaxProbes = 16;
  static const int kMaxExcludes = 4;

  cLightProbeQuery();
  ~cLightProbeQuery();

  void Init(RIDevice *apDevice);
  void Dispose(RIDevice *apDevice);

  /////////////////////////////////////////////////
  // Gameplay side

  // Replace the world-space probe positions. Results arrive asynchronously.
  void SetProbes(const cVector3f *apPositions, int alCount);

  // Lights the probe must not see. The player carries lights of his own — the
  // darkness "eye adaptation" ambient light follows the camera and brightens
  // BECAUSE the player is in the dark, so sensing it would cancel the darkness
  // that switched it on. Lights with no GPU slot yet are skipped.
  void SetExcludedLights(iLight **apLights, int alCount);

  // Newest completed answer for probe `alIndex`. False when no readback has
  // landed yet (first frames after a map load, or a build that never
  // dispatched) or when the index is beyond the submitted probe count — the
  // caller retains its last reading or startup default.
  bool GetResult(int alIndex, cVector3f &avIrradiance) const;

  // Drop every pending and completed result. Call on map change: the answers
  // describe a world that no longer exists, and a stale "you are lit" would
  // suppress the first darkness tick in the new one.
  void Reset();

  /////////////////////////////////////////////////
  // Renderer side

  // Harvest any readback whose copy the GPU has finished. Cheap; call once a
  // frame before recording.
  void Poll(RIDevice *apDevice, uint64_t alCompletedTimelineValue);

  // True when there is something to evaluate this frame. The renderer skips the
  // whole pass otherwise, so menu and load frames cost nothing.
  bool WantsDispatch() const { return mlProbeCount > 0; }

  // Upload this frame's requests into the frame's slot. Returns false (nothing
  // recorded) if the buffers are not up. Call before binding descriptors.
  bool BeginFrame(uint32_t alFrameIndex);

  RIBuffer *GetRequestBuffer(uint32_t alFrameIndex);
  RIBuffer *GetResultBuffer(uint32_t alFrameIndex);
  uint64_t GetRequestBufferRange() const;
  uint64_t GetResultBufferRange() const;

  // Push-constant payload, matching LightProbePC in LightProbePass.cs.slang.
  struct cPushConstants {
    uint32_t mlProbeCount;
    uint32_t mlExcludeCount;
    uint32_t mvExcludeIds[kMaxExcludes];
  };
  cPushConstants GetPushConstants() const;

  // Record the results->host copy and arm the timeline value the copy will have
  // passed once it has executed. Call after the dispatch, inside the same
  // command buffer.
  void RecordReadback(RIDevice *apDevice, RICmd *apCmd, uint32_t alFrameIndex,
                      uint64_t alPendingTimelineValue);

private:
  // Matches LightProbeRequest / LightProbeResult in SceneTypes.slang under
  // scalar layout (16 bytes each).
  struct cGpuRequest {
    float mvPos[3];
    float mfPadding;
  };
  struct cGpuResult {
    float mvIrradiance[3];
    float mfPadding;
  };

  static_assert(sizeof(cGpuRequest) == 16, "LightProbeRequest must be 16 bytes");
  static_assert(sizeof(cGpuResult) == 16, "LightProbeResult must be 16 bytes");

  struct cFrameSlot {
    RIBuffer mRequests = {};  // host-upload, read by the pass
    RIBuffer mResults = {};   // device-local, written by the pass
    RIBuffer mReadback = {};  // host-readback, the copy target
    // 0 = no copy in flight. Otherwise the graphics-timeline value the copy
    // will have passed once the GPU has executed it.
    uint64_t mlStageValue = 0;
    int mlProbeCount = 0;
  };

  bool mbInitialized = false;
  std::array<cFrameSlot, RI_NUMBER_FRAMES_FLIGHT> mvSlots;

  // Pending request set, owned by the gameplay thread.
  std::array<cGpuRequest, kMaxProbes> mvRequests = {};
  int mlProbeCount = 0;
  std::array<uint32_t, kMaxExcludes> mvExcludeIds = {};
  int mlExcludeCount = 0;

  // Newest harvested answers.
  std::array<cGpuResult, kMaxProbes> mvResults = {};
  int mlResultCount = 0;
  bool mbHasResult = false;
};

} // namespace hpl

#endif // HPL_LIGHT_PROBE_QUERY_H
