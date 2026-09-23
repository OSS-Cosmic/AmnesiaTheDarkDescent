#ifndef HPL_FFX_API_LOADER_H
#define HPL_FFX_API_LOADER_H

#include <cstdint>

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE

#include <ffx_api/ffx_api.h>

namespace hpl {

// The five backend-independent ffx-api entry points, resolved from a shared
// module at runtime rather than linked.
//
// cmake/fsr builds one module per backend -- ffx_fsr3upscaler_api_vk and
// ffx_fsr3upscaler_api_dx12 -- each exporting this same symbol set. They must be
// separate modules rather than two archives: both carry ffx_shader_blobs.cpp
// defining ffxGetPermutationBlobByIndex, so linking them would resolve that
// symbol once and silently hand one backend the other's shader blobs. The
// backend is therefore chosen by loading the matching module and chaining a
// create-context descriptor, never by linking.
struct FfxApi {
  PfnFfxCreateContext CreateContext = nullptr;
  PfnFfxDestroyContext DestroyContext = nullptr;
  PfnFfxConfigure Configure = nullptr;
  PfnFfxQuery Query = nullptr;
  PfnFfxDispatch Dispatch = nullptr;

  bool available = false;
  // RIDeviceAPI_e this module serves, valid only when available.
  uint8_t backendApi = 0;
  char unavailableReason[128] = {};
};

// Resolves the module for one backend, once per backend, and caches the result;
// the module is never unloaded. Safe to call before any device exists and from
// the options menu: this loads a shared library and resolves symbols, but
// creates no GPU context, allocates no GPU resource and records no command.
//
// `backendApi` is an RIDeviceAPI_e. An unknown backend, or a backend this build
// has no module for, returns an entry with available == false and a reason
// rather than failing.
const FfxApi &FfxApiFor(uint8_t backendApi);

// The module matching the renderer that is live right now.
//
// Callers that stash a function pointer for later use -- notably deferred
// context destruction, which drains frames after the fact -- must capture the
// pointer alongside the context instead of calling this again. In a build with
// both backends compiled in, the active backend can change in between, and
// destroying a context through the other module's entry point walks into the
// wrong provider table.
const FfxApi &FfxApiActive();

} // namespace hpl

#endif // HPL2_FSR_AVAILABLE

#endif // HPL_FFX_API_LOADER_H
