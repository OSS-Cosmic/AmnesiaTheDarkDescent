#include "graphics/FfxApiLoader.h"

#if defined(HPL2_FSR_AVAILABLE) && HPL2_FSR_AVAILABLE

#include "graphics/RIDefines.h"
#include "graphics/RIDevice.h"
#include "system/LowLevelSystem.h"

// ffx-api is resolved at runtime rather than linked; see LoadFfxApi below.
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>
#include <cstring>

namespace hpl {

namespace {

void SetUnavailable(FfxApi &api, const char *reason) {
  api.available = false;
  if (!reason || !reason[0])
    reason = "unspecified reason";
  std::strncpy(api.unavailableReason, reason,
               sizeof(api.unavailableReason) - 1);
  api.unavailableReason[sizeof(api.unavailableReason) - 1] = '\0';
}

// The module base name for a backend, or nullptr when this platform/build has
// none. Deliberately without extension or "lib" prefix; LoadFfxApi spells those.
const char *ModuleBaseName(uint8_t backendApi) {
  switch (backendApi) {
  case RI_DEVICE_API_VK:
    return "ffx_fsr3upscaler_api_vk";
  case RI_DEVICE_API_D3D12:
    // The D3D12 module's permutations are DXIL, so cmake/fsr builds it only on
    // Windows and only when the DXC half of the shader compiler was built.
#if defined(HPL2_FSR_D3D12_MODULE_AVAILABLE) && HPL2_FSR_D3D12_MODULE_AVAILABLE
    return "ffx_fsr3upscaler_api_dx12";
#else
    return nullptr;
#endif
  default:
    return nullptr;
  }
}

const char *BackendDisplayName(uint8_t backendApi) {
  switch (backendApi) {
  case RI_DEVICE_API_VK:
    return "Vulkan";
  case RI_DEVICE_API_D3D12:
    return "Direct3D 12";
  default:
    return "this renderer";
  }
}

void LoadFfxApi(FfxApi &api, uint8_t backendApi) {
  api.backendApi = backendApi;
  SetUnavailable(api, "the FSR runtime module has not been loaded");

  const char *baseName = ModuleBaseName(backendApi);
  if (!baseName) {
    char reason[128];
    std::snprintf(reason, sizeof(reason),
                  "this build has no FSR runtime module for %s",
                  BackendDisplayName(backendApi));
    SetUnavailable(api, reason);
    return;
  }

  char libraryName[128];
#if defined(_WIN32)
  // The staging project puts the module next to the executable, which is the
  // first directory the default search order looks in.
  std::snprintf(libraryName, sizeof(libraryName), "%s.dll", baseName);
  HMODULE library = LoadLibraryA(libraryName);
#else
  // The game and tools link with -Wl,-rpath,'$ORIGIN/libs', where the staging
  // project puts it. RTLD_LOCAL keeps ffx-api's symbols out of the global scope
  // -- the module carries its own copy of the shader blobs and, on the Vulkan
  // side, its own Vulkan entry-point table, neither of which may be shared with
  // the executable's volk.
  std::snprintf(libraryName, sizeof(libraryName), "lib%s.so", baseName);
  void *library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
#endif
  if (!library) {
    char reason[128];
    std::snprintf(reason, sizeof(reason),
                  "%s could not be loaded; FSR is disabled", libraryName);
    SetUnavailable(api, reason);
    return;
  }

  // The module is deliberately never unloaded: it owns the FSR context state
  // and its shader blobs are read straight out of its data segment.
#if defined(_WIN32)
#define FFX_LOAD_SYMBOL(member, name)                                          \
  api.member =                                                                 \
      reinterpret_cast<decltype(api.member)>(GetProcAddress(library, name));
#else
#define FFX_LOAD_SYMBOL(member, name)                                          \
  api.member = reinterpret_cast<decltype(api.member)>(dlsym(library, name));
#endif

#define FFX_REQUIRE_SYMBOL(member, name)                                       \
  FFX_LOAD_SYMBOL(member, name)                                                \
  if (!api.member) {                                                           \
    char reason[128];                                                          \
    std::snprintf(reason, sizeof(reason), "%s is missing the symbol %s",       \
                  libraryName, name);                                          \
    SetUnavailable(api, reason);                                               \
    return;                                                                    \
  }

  FFX_REQUIRE_SYMBOL(CreateContext, "ffxCreateContext");
  FFX_REQUIRE_SYMBOL(DestroyContext, "ffxDestroyContext");
  FFX_REQUIRE_SYMBOL(Configure, "ffxConfigure");
  FFX_REQUIRE_SYMBOL(Query, "ffxQuery");
  FFX_REQUIRE_SYMBOL(Dispatch, "ffxDispatch");

#undef FFX_REQUIRE_SYMBOL
#undef FFX_LOAD_SYMBOL

  api.available = true;
  api.unavailableReason[0] = '\0';
}

FfxApi MakeLoaded(uint8_t backendApi) {
  FfxApi loaded;
  LoadFfxApi(loaded, backendApi);
  if (loaded.available)
    Log("FfxApiLoader: FSR runtime module for %s loaded\n",
        BackendDisplayName(backendApi));
  else
    Warning("FfxApiLoader: %s\n", loaded.unavailableReason);
  return loaded;
}

// One cache per backend, each behind its own accessor.
//
// Two function-local statics in a single body would both initialise on the
// first call regardless of which branch ran, loading a module this process may
// never use. Separate functions keep each load lazy, and the magic-static rules
// give the thread safety.
//
// A single process-wide cache -- as NrdIntegration.cpp uses -- would be wrong
// here: DEVICE_MULTI_BACKEND builds pick the backend at runtime, so a cache
// keyed on "whichever backend asked first" could hand the D3D12 renderer the
// Vulkan module.
const FfxApi &FfxApiVk() {
  static const FfxApi api = MakeLoaded(RI_DEVICE_API_VK);
  return api;
}

const FfxApi &FfxApiD3D12() {
  static const FfxApi api = MakeLoaded(RI_DEVICE_API_D3D12);
  return api;
}

const FfxApi &FfxApiNone(uint8_t backendApi) {
  // Not cached per backend value: every unsupported backend gets the same
  // answer, and nothing about it can change over the life of the process.
  static const FfxApi api = [] {
    FfxApi unavailable;
    SetUnavailable(unavailable,
                   "FSR has no runtime module for the active renderer");
    return unavailable;
  }();
  (void)backendApi;
  return api;
}

} // namespace

const FfxApi &FfxApiFor(uint8_t backendApi) {
  switch (backendApi) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return FfxApiVk();
#endif
#if (DEVICE_IMPL_D3D12)
  case RI_DEVICE_API_D3D12:
    return FfxApiD3D12();
#endif
  default:
    return FfxApiNone(backendApi);
  }
}

const FfxApi &FfxApiActive() { return FfxApiFor(RIActiveBackendApi()); }

} // namespace hpl

#endif // HPL2_FSR_AVAILABLE
