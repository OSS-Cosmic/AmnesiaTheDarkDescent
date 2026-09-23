// The engine does not link FSR. It loads one ffx-api module per backend at
// runtime and resolves five entry points from it, so nothing in the ordinary
// build catches a module that compiles but is unusable. Three failures in
// particular are invisible until the game asks for an upscaler:
//
//   * ffx_api.h decorates its entry points with FFX_API_ENTRY, which upstream
//     hard-codes to __declspec(dllexport). A module built with that macro
//     neutralised exports nothing while still linking cleanly.
//   * The staging project may not have copied the module next to the
//     executable, so loading it by name finds nothing.
//   * The Vulkan and D3D12 modules each carry their own copy of
//     ffx_shader_blobs.cpp. If they ever collapse into one loaded image, one
//     backend silently receives the other's shader blobs.
//
// This test needs no GPU and creates no context: it loads from the DEPLOYED
// location, resolves the symbols, and makes one device-free call into each
// module to prove its code actually runs.

// windows.h has to precede utest.h: utest.h declares QueryPerformanceCounter
// and QueryPerformanceFrequency itself when it does not already see them, and
// the SDK's own extern "C" declarations then collide with it (C2733).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef HPL2_FSR_MODULE_DIR
#error "HPL2_FSR_MODULE_DIR must name the directory the modules are staged into"
#endif

namespace {

#if defined(_WIN32)
using ModuleHandle = HMODULE;
constexpr ModuleHandle kNullModule = nullptr;
#else
using ModuleHandle = void *;
constexpr ModuleHandle kNullModule = nullptr;
#endif

// The five entry points every module exports, whatever backend it serves.
constexpr const char *kEntryPoints[] = {
    "ffxCreateContext", "ffxDestroyContext", "ffxConfigure",
    "ffxQuery",         "ffxDispatch",
};

std::string ModulePath(const char *baseName) {
  std::string path = HPL2_FSR_MODULE_DIR;
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
    path.push_back('/');
#if defined(_WIN32)
  path += baseName;
  path += ".dll";
#else
  path += "lib";
  path += baseName;
  path += ".so";
#endif
  return path;
}

// Absolute path rather than a bare name: the point is to prove the staging step
// put the module where the game will look for it, not that some other copy
// happens to be reachable through the loader's search path.
ModuleHandle LoadModule(const std::string &path, std::string *reason) {
#if defined(_WIN32)
  const ModuleHandle module = LoadLibraryA(path.c_str());
  if (module == kNullModule && reason) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "LoadLibraryA failed (%lu)",
                  static_cast<unsigned long>(GetLastError()));
    *reason = buffer;
  }
#else
  // RTLD_NOW so an unresolved dependency fails here rather than crashing at the
  // first call. RTLD_LOCAL keeps the module's symbols out of the global scope,
  // matching how the engine loads it.
  const ModuleHandle module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (module == kNullModule && reason) {
    const char *error = dlerror();
    *reason = error ? error : "dlopen failed";
  }
#endif
  return module;
}

void *ResolveSymbol(ModuleHandle module, const char *name) {
#if defined(_WIN32)
  return reinterpret_cast<void *>(GetProcAddress(module, name));
#else
  return dlsym(module, name);
#endif
}

void UnloadModule(ModuleHandle module) {
  if (module == kNullModule)
    return;
#if defined(_WIN32)
  FreeLibrary(module);
#else
  dlclose(module);
#endif
}

struct LoadedModule {
  ModuleHandle handle = kNullModule;
  PfnFfxQuery query = nullptr;
};

// Every module this build is expected to have staged.
std::vector<const char *> ExpectedModules() {
  std::vector<const char *> modules{"ffx_fsr3upscaler_api_vk"};
#if defined(HPL2_FSR_D3D12_MODULE_AVAILABLE) && HPL2_FSR_D3D12_MODULE_AVAILABLE
  modules.push_back("ffx_fsr3upscaler_api_dx12");
#endif
  return modules;
}

} // namespace

struct FsrModuleFixture {
  std::vector<LoadedModule> modules;
};

UTEST_F_SETUP(FsrModuleFixture) {
  for (const char *baseName : ExpectedModules()) {
    const std::string path = ModulePath(baseName);
    std::string reason;
    LoadedModule loaded;
    loaded.handle = LoadModule(path, &reason);
    ASSERT_TRUE_MSG(loaded.handle != kNullModule,
                    (path + ": " + reason).c_str());
    loaded.query = reinterpret_cast<PfnFfxQuery>(
        ResolveSymbol(loaded.handle, "ffxQuery"));
    utest_fixture->modules.push_back(loaded);
  }
  ASSERT_FALSE(utest_fixture->modules.empty());
}

UTEST_F_TEARDOWN(FsrModuleFixture) {
  for (LoadedModule &loaded : utest_fixture->modules)
    UnloadModule(loaded.handle);
  utest_fixture->modules.clear();
}

// The direct regression test for the FFX_API_ENTRY patch: a module whose export
// macro was neutralised builds and loads, but resolves nothing.
UTEST_F(FsrModuleFixture, ExportsEveryEntryPoint) {
  for (const LoadedModule &loaded : utest_fixture->modules) {
    for (const char *name : kEntryPoints) {
      void *symbol = ResolveSymbol(loaded.handle, name);
      ASSERT_TRUE_MSG(symbol != nullptr, name);
    }
  }
}

// Each backend must keep its own copy of the shared sources. If the two modules
// ever resolve to one image, ffxGetPermutationBlobByIndex is shared and one
// backend receives the other's shader blobs -- which is exactly why cmake/fsr
// builds separate modules instead of two static archives.
UTEST_F(FsrModuleFixture, BackendModulesAreSeparateImages) {
  if (utest_fixture->modules.size() < 2u) {
    UTEST_SKIP("only one backend module is built in this configuration");
  }
  void *first = ResolveSymbol(utest_fixture->modules[0].handle,
                              "ffxCreateContext");
  void *second = ResolveSymbol(utest_fixture->modules[1].handle,
                               "ffxCreateContext");
  ASSERT_TRUE(first != nullptr && second != nullptr);
  ASSERT_TRUE(first != second);
}

// Prove the module's own code runs, not merely that the symbols exist.
//
// ffxQuery accepts a null context for a version enumeration: ffx_api.cpp
// resolves the provider from the descriptor type instead, and the upscaler
// provider only dereferences the context for the GPU-memory-usage query. A null
// device keeps the D3D12 module out of its driver-extension probe. The count is
// also a regression test on patches/portable_ffx_api.cmake, which trims the
// provider registry down to the FSR3 upscaler alone.
UTEST_F(FsrModuleFixture, ReportsExactlyTheUpscaleProvider) {
  for (const LoadedModule &loaded : utest_fixture->modules) {
    ASSERT_TRUE(loaded.query != nullptr);

    uint64_t versionCount = 0;
    ffxQueryDescGetVersions versions = {};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.header.pNext = nullptr;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = nullptr;
    versions.outputCount = &versionCount;
    versions.versionIds = nullptr;
    versions.versionNames = nullptr;

    const ffxReturnCode_t result = loaded.query(nullptr, &versions.header);
    ASSERT_EQ(static_cast<ffxReturnCode_t>(FFX_API_RETURN_OK), result);
    ASSERT_EQ(1u, static_cast<unsigned>(versionCount));
  }
}

UTEST_MAIN();
