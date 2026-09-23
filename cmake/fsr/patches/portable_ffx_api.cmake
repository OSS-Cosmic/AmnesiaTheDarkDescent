# Anchor-checked edits applied only to the staged FidelityFX ffx-api sources.
#
# Two jobs:
#   1. Trim the provider registry to the FSR3 upscaler. The engine integrates the
#      upscaler only, and every other provider drags in another effect's sources
#      and its own shader permutations.
#   2. Make the provider registry portable. ffx-api is built by AMD as a Windows
#      DLL, so ffx_provider.cpp includes <d3d12.h> unconditionally, uses the MSVC
#      _countof, and carries a D3D12-only AMD driver-extension override. The
#      Vulkan module has to build on Linux too.
#
# ffx_sdk_replace_required is defined by portable_sdk.cmake, which is included
# first. Every replacement checks its anchor against the pristine SDK source, so
# an SDK update fails configuration instead of silently producing a different
# module.

if(NOT COMMAND ffx_sdk_replace_required)
    message(FATAL_ERROR
        "portable_ffx_api.cmake requires ffx_sdk_replace_required; include patches/portable_sdk.cmake first")
endif()

set(_ffx_api_upstream_provider "${FSR_SDK_ROOT}/ffx-api/src/ffx_provider.cpp")

# 1. Drop the trimmed providers' headers and stop including <d3d12.h> on a
#    Vulkan build. <iterator> replaces the MSVC-only _countof below.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
[=[#include "ffx_provider.h"

#include "ffx_provider_fsr2.h"
#include "ffx_provider_fsr3upscale.h"
#include "ffx_provider_framegeneration.h"
#include "ffx_provider_external.h"

#include <array>
#include <optional>

#include <d3d12.h>

#ifdef FFX_BACKEND_DX12
#include "dx12/ffx_provider_framegenerationswapchain_dx12.h"
#endif // FFX_BACKEND_DX12

#ifdef FFX_BACKEND_VK
#include "vk/ffx_provider_framegenerationswapchain_vk.h"
#endif // FFX_BACKEND_VK]=]
[=[#include "ffx_provider.h"

#include "ffx_provider_fsr3upscale.h"
#include "ffx_provider_external.h"

#include <array>
#include <iterator>
#include <optional>

#ifdef FFX_BACKEND_DX12
#include <d3d12.h>
#endif // FFX_BACKEND_DX12]=]
    "upscaler-only provider includes")

# 2. Trim the registry itself, and replace the MSVC-only _countof.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
[=[static constexpr ffxProvider* providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
    &ffxProvider_FSR2::Instance,
    &ffxProvider_FrameGeneration::Instance,
#ifdef FFX_BACKEND_DX12
    &ffxProvider_FrameGenerationSwapChain_DX12::Instance,
#endif // FFX_BACKEND_DX12
#ifdef FFX_BACKEND_VK
    &ffxProvider_FrameGenerationSwapChain_VK::Instance,
#endif // FFX_BACKEND_VK
};
static constexpr size_t providerCount = _countof(providers);]=]
[=[static constexpr ffxProvider* providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
};
static constexpr size_t providerCount = std::size(providers);]=]
    "upscaler-only provider registry")

# 3. Fence the AMD driver-extension override behind the D3D12 backend. It is
#    reached through IAmdExtFfxApi in amdxc64.dll against an ID3D12Device, so it
#    has no meaning on Vulkan and does not compile without the D3D12 headers.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
    "MIDL_INTERFACE(\"b58d6601-7401-4234-8180-6febfc0e484c\")"
    "#ifdef FFX_BACKEND_DX12\nMIDL_INTERFACE(\"b58d6601-7401-4234-8180-6febfc0e484c\")"
    "open D3D12 guard around the driver-side provider override")

# Take the device as void* so the call sites need no D3D12 type.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
[=[void GetExternalProviders(ID3D12Device* device, uint64_t descType)
{
    static IAmdExtFfxApi* apiExtension = nullptr;]=]
[=[static void GetExternalProviders(void* deviceHandle, uint64_t descType)
{
    ID3D12Device* device = reinterpret_cast<ID3D12Device*>(deviceHandle);
    static IAmdExtFfxApi* apiExtension = nullptr;]=]
    "device handle passed as void* to the driver-side provider override")

# Close the guard and supply the no-op used when D3D12 is not the backend.
# GetExternalProviders ends immediately above this signature, which is unique in
# the translation unit. Anchoring on the signature rather than on the end of the
# function avoids depending on the trailing whitespace upstream leaves there.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
[=[const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void* device)]=]
[=[#else  // FFX_BACKEND_DX12
// No driver-side provider override outside D3D12.
static void GetExternalProviders(void*, uint64_t)
{
}
#endif // FFX_BACKEND_DX12

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void* device)]=]
    "close D3D12 guard around the driver-side provider override")

# 4. Resolve the Vulkan entry points the SDK backend calls by plain name before
#    entering it. ffx_vk_loader.h is force-included into every source of the
#    shared Vulkan module, so the declaration is already visible here, and the
#    call sits inside the FFX_BACKEND_VK block so the D3D12 module drops it.
#    ffxGetScratchMemorySizeVK, immediately below, already calls
#    vkEnumerateDeviceExtensionProperties, so this has to come first.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_BACKENDS_SOURCE}"
    "${FSR_SDK_ROOT}/ffx-api/src/backends.cpp"
[=[            const auto *backendDesc = reinterpret_cast<const ffxCreateBackendVKDesc*>(it);
            VkDeviceContext deviceContext = { backendDesc->vkDevice, backendDesc->vkPhysicalDevice, backendDesc->vkDeviceProcAddr };]=]
[=[            if (!ffxVkLoaderEnsureInitialized())
                return FFX_API_RETURN_ERROR_RUNTIME_ERROR;

            const auto *backendDesc = reinterpret_cast<const ffxCreateBackendVKDesc*>(it);
            VkDeviceContext deviceContext = { backendDesc->vkDevice, backendDesc->vkPhysicalDevice, backendDesc->vkDeviceProcAddr };]=]
    "resolve Vulkan entry points before creating the backend")

# Both call sites are identical; string(REPLACE) rewrites them together.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_SOURCE}"
    "${_ffx_api_upstream_provider}"
    "GetExternalProviders(reinterpret_cast<ID3D12Device*>(device), descType);"
    "GetExternalProviders(device, descType);"
    "portable driver-side provider override call sites")

# 5. Make the ffx-api export macro portable.
#
# ffx-api ships only as a Windows DLL upstream, so ffx_api.h decorates its five
# entry points with a bare __declspec(dllexport) and no compiler guard. GCC and
# Clang on ELF targets do not merely ignore the token, they reject it outright:
#
#     error: '__declspec' attributes are not enabled; use '-fdeclspec' or
#            '-fms-extensions' to enable support for __declspec attributes
#
# That breaks the Linux module build AND every engine translation unit that
# includes this header -- which the engine needs for ffxCreateBackendVKDesc,
# ffxCreateContextDescUpscale and the PfnFfx* typedefs. The SDK proper has a
# guard on its own FFX_API macro in sdk/include/FidelityFX/host/ffx_types.h,
# which is why sdk/include needs no staging; mirror that guard here.
#
# That guard is conditional, not unconditional, and the difference matters to
# anyone editing the module's compile definitions. It reads
#
#     #if defined(FFX_GCC) || !defined(FFX_BUILD_AS_DLL)
#
# and FFX_GCC is defined nowhere -- not by the SDK, not by the compiler, not by
# this build. sdk/include survives GCC and Clang purely because nothing defines
# FFX_BUILD_AS_DLL on those toolchains, so cmake/fsr/CMakeLists.txt keeps that
# define inside its if(MSVC) leg. Adding it unconditionally reintroduces the
# same __declspec parse error one header over.
#
# visibility("default") rather than nothing on GCC/Clang: the module targets set
# CXX_VISIBILITY_PRESET hidden so the .so exports exactly these five symbols and
# cannot be interposed by the executable's volk or its copy of
# ffxGetPermutationBlobByIndex.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_HEADER}"
    "${FSR_SDK_ROOT}/ffx-api/include/ffx_api/ffx_api.h"
    "#define FFX_API_ENTRY __declspec(dllexport)"
[=[#if defined(_MSC_VER)
#define FFX_API_ENTRY __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define FFX_API_ENTRY __attribute__((visibility("default")))
#else
#define FFX_API_ENTRY
#endif]=]
    "portable ffx-api export macro")

# 6. Stop the two backend headers colliding when both are included at once.
#
# ffx_api_vk.h and ffx_api_dx12.h each declare a differently-named enum --
# FfxApiConfigureFrameGenerationSwapChainKeyVK and ...DX12 -- but give both the
# same two enumerator names. C++ puts unscoped enumerators in the enclosing
# scope, so including both headers in one translation unit is a redefinition:
#
#     error C2365: 'FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK':
#                  redefinition; previous definition was 'enumerator'
#
# Upstream never hits this because a build targets one backend and includes one
# header. The engine includes both, because one translation unit has to be able
# to fill in either ffxCreateBackendVKDesc or ffxCreateBackendDX12Desc.
#
# Suffixing the D3D12 spellings is safe: these two enumerators are declared and
# never referenced anywhere in the SDK, and they belong to frame generation,
# which this integration does not build.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_DX12_HEADER}"
    "${FSR_SDK_ROOT}/ffx-api/include/ffx_api/dx12/ffx_api_dx12.h"
[=[    FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK = 0,                     ///< Sets FfxWaitCallbackFunc
    FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING = 2,                ///< Sets FfxApiSwapchainFramePacingTuning]=]
[=[    FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK_DX12 = 0,                ///< Sets FfxWaitCallbackFunc
    FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING_DX12 = 2,           ///< Sets FfxApiSwapchainFramePacingTuning]=]
    "distinct D3D12 frame-generation swapchain configure keys")

# 7. Spell the SDK include directory the way the filesystem spells it.
#
# Two ffx-api headers reach into the SDK through <FidelityFx/...> with a
# lowercase x, while the directory on disk is sdk/include/FidelityFX. NTFS and
# the MSVC search path do not care; ext4 and every other case-sensitive
# filesystem do, and the include fails outright:
#
#     fatal error: FidelityFx/host/ffx_types.h: No such file or directory
#
# Both spellings appear in upstream v1.1.4 -- the rest of ffx-api already writes
# FidelityFX -- so this is a typo the Windows-only build could not surface, not
# a deliberate alias.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_PROVIDER_HEADER}"
    "${FSR_SDK_ROOT}/ffx-api/src/ffx_provider.h"
    "#include <FidelityFx/host/ffx_types.h>"
    "#include <FidelityFX/host/ffx_types.h>"
    "case-correct SDK include in ffx_provider.h")

ffx_sdk_replace_required(
    "${FFX_API_STAGED_BACKENDS_HEADER}"
    "${FSR_SDK_ROOT}/ffx-api/src/backends.h"
    "#include <FidelityFx/host/ffx_interface.h>"
    "#include <FidelityFX/host/ffx_interface.h>"
    "case-correct SDK include in backends.h")

# 8. Replace the MSVC integer-literal suffix in the provider's id.
#
# `ui64` is a Microsoft extension. GCC and Clang parse it as a user-defined
# literal operator, find none, and stop:
#
#     error: unable to find numeric literal operator 'operator""ui64'
#
# ULL is the standard spelling with the same width and signedness, so the
# constant the provider registry matches on is bit-for-bit unchanged. The digit
# separator stays: that one is standard C++14.
ffx_sdk_replace_required(
    "${FFX_API_STAGED_FSR3UPSCALE_SOURCE}"
    "${FSR_SDK_ROOT}/ffx-api/src/ffx_provider_fsr3upscale.cpp"
    "return 0xF5A5'CA1Eui64 << 32"
    "return 0xF5A5'CA1EULL << 32"
    "portable provider id literal")
