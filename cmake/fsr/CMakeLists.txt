cmake_minimum_required(VERSION 3.20)

project(FidelityFX_FSR3Upscaler_Vulkan LANGUAGES CXX)

set(FSR_SDK_ROOT "" CACHE PATH "FidelityFX SDK 1.1.4 root")
set(FSR_VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers include directory")
set(FSR_VOLK_HEADERS "" CACHE PATH "volk include directory")
set(FSR_OUTPUT_LIB_DIR "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Directory for the FSR static archive")
set(FSR_OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Directory for the FSR ffx-api shared modules")

if(NOT IS_DIRECTORY "${FSR_SDK_ROOT}/sdk/include")
    message(FATAL_ERROR "FSR_SDK_ROOT must contain sdk/include: ${FSR_SDK_ROOT}")
endif()
if(NOT EXISTS "${FSR_VULKAN_HEADERS}/vulkan/vulkan.h")
    message(FATAL_ERROR "FSR_VULKAN_HEADERS must contain vulkan/vulkan.h: ${FSR_VULKAN_HEADERS}")
endif()
if(NOT EXISTS "${FSR_VOLK_HEADERS}/volk.h")
    message(FATAL_ERROR "FSR_VOLK_HEADERS must contain volk.h: ${FSR_VOLK_HEADERS}")
endif()

get_filename_component(FSR_OUTPUT_LIB_DIR "${FSR_OUTPUT_LIB_DIR}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
file(MAKE_DIRECTORY "${FSR_OUTPUT_LIB_DIR}")
get_filename_component(FSR_OUTPUT_BIN_DIR "${FSR_OUTPUT_BIN_DIR}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
file(MAKE_DIRECTORY "${FSR_OUTPUT_BIN_DIR}")

# Multi-config generators append the configuration name unless every
# per-configuration archive directory is set explicitly.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${FSR_OUTPUT_LIB_DIR}")
set(_fsr_archive_configs Debug Release RelWithDebInfo MinSizeRel)
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND _fsr_archive_configs ${CMAKE_CONFIGURATION_TYPES})
endif()
list(REMOVE_DUPLICATES _fsr_archive_configs)
foreach(_fsr_config IN LISTS _fsr_archive_configs)
    string(TOUPPER "${_fsr_config}" _fsr_config_upper)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_fsr_config_upper} "${FSR_OUTPUT_LIB_DIR}")
endforeach()

set(_fsr_license_dir "${CMAKE_BINARY_DIR}/licenses")
file(MAKE_DIRECTORY "${_fsr_license_dir}")
if(NOT EXISTS "${FSR_SDK_ROOT}/LICENSE.txt" OR NOT EXISTS "${FSR_SDK_ROOT}/sdk/LICENSE.txt")
    message(FATAL_ERROR "FidelityFX SDK license files are missing under ${FSR_SDK_ROOT}")
endif()
configure_file(
    "${FSR_SDK_ROOT}/LICENSE.txt"
    "${_fsr_license_dir}/FidelityFX-SDK-1.1.4-LICENSE.txt"
    COPYONLY)
configure_file(
    "${FSR_SDK_ROOT}/sdk/LICENSE.txt"
    "${_fsr_license_dir}/FidelityFX-SDK-sdk-LICENSE.txt"
    COPYONLY)
# The SDK checkout is read-only, and configure_file(COPYONLY) preserves its mode.
# Writable staged licenses keep rebuild copies and {RMDIR} cleanup working.
file(CHMOD "${_fsr_license_dir}/FidelityFX-SDK-1.1.4-LICENSE.txt"
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
file(CHMOD "${_fsr_license_dir}/FidelityFX-SDK-sdk-LICENSE.txt"
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# These cache variables are also consumed by the FSR shader-generation
# wrapper. GLSL compilation is required; SPIR-V validation is optional.
set(FSR_GLSLANG "" CACHE FILEPATH "Path to glslangValidator")
if(FSR_GLSLANG)
    if(NOT EXISTS "${FSR_GLSLANG}")
        message(FATAL_ERROR "FSR_GLSLANG points to a missing glslangValidator: ${FSR_GLSLANG}")
    endif()
else()
    # glslang >= 16 renamed the driver to plain `glslang`; Ubuntu's glslang-tools
    # still ships it as glslangValidator. Both accept the same command line.
    find_program(_FFX_SC_GLSLANG NAMES glslangValidator glslang)
    if(NOT _FFX_SC_GLSLANG)
        message(FATAL_ERROR "glslangValidator (or glslang) is required for FSR shader generation. Install the glslang-tools package or set FSR_GLSLANG.")
    endif()
    set(FSR_GLSLANG "${_FFX_SC_GLSLANG}" CACHE FILEPATH "Path to glslangValidator" FORCE)
endif()

set(Python3_EXECUTABLE "" CACHE FILEPATH "Python 3 interpreter used by the FSR shader driver")
if(Python3_EXECUTABLE)
    if(NOT EXISTS "${Python3_EXECUTABLE}")
        message(FATAL_ERROR "Python3_EXECUTABLE points to a missing Python interpreter: ${Python3_EXECUTABLE}")
    endif()
else()
    find_program(_FFX_SC_PYTHON NAMES python3 python)
    if(NOT _FFX_SC_PYTHON)
        message(FATAL_ERROR "Python 3 is required for FSR shader generation. Install Python 3 or set Python3_EXECUTABLE.")
    endif()
    set(Python3_EXECUTABLE "${_FFX_SC_PYTHON}" CACHE FILEPATH "Python 3 interpreter used by the FSR shader driver" FORCE)
endif()

set(FSR_SPIRV_VAL "" CACHE FILEPATH "Optional path to spirv-val")
if(NOT FSR_SPIRV_VAL)
    find_program(_FFX_SC_SPIRV_VAL NAMES spirv-val)
    if(_FFX_SC_SPIRV_VAL)
        set(FSR_SPIRV_VAL "${_FFX_SC_SPIRV_VAL}" CACHE FILEPATH "Optional path to spirv-val" FORCE)
    endif()
elseif(NOT EXISTS "${FSR_SPIRV_VAL}")
    message(FATAL_ERROR "FSR_SPIRV_VAL points to a missing spirv-val: ${FSR_SPIRV_VAL}")
endif()

if(NOT FSR_GLSLANG)
    message(FATAL_ERROR "FSR_GLSLANG must be set for FSR shader generation.")
endif()
if(NOT Python3_EXECUTABLE)
    message(FATAL_ERROR "Python3_EXECUTABLE must be set for FSR shader generation.")
endif()

# Declared here rather than only in ffx_sc/CMakeLists.txt so it exists on both
# paths below: when the shader compiler is reused as an imported executable that
# subdirectory is never processed, and the D3D12 module still has to know
# whether a DXC-capable FidelityFX_SC is available.
set(FSR_ENABLE_HLSL_DEFAULT OFF)
if(WIN32)
    set(FSR_ENABLE_HLSL_DEFAULT ON)
endif()
set(FSR_ENABLE_HLSL "${FSR_ENABLE_HLSL_DEFAULT}" CACHE BOOL
    "Build the HLSL/DXC half of FidelityFX_SC (required for the D3D12 backend)")

set(_fsr_dxc_dll "${FSR_SDK_ROOT}/sdk/tools/ffx_shader_compiler/libs/dxc/bin/x64/dxcompiler.dll")

# The shader compiler is an independent native target and owns its staged
# portability edits.  The wrapper consumes its executable as a build tool.
# ffx_sc/CMakeLists.txt restages its complete source tree on every configure;
# once a single-config build has produced the tool, reuse it as an imported
# executable so a configure followed by a rebuild remains a true no-op.
set(_fsr_sc_executable "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc/FidelityFX_SC${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT CMAKE_CONFIGURATION_TYPES AND EXISTS "${_fsr_sc_executable}")
    add_executable(FidelityFX_SC IMPORTED GLOBAL)
    set_target_properties(FidelityFX_SC PROPERTIES
        IMPORTED_LOCATION "${_fsr_sc_executable}")
else()
    add_subdirectory(ffx_sc)
endif()

set(_fsr_generated_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
set(_fsr_shader_stamp "${_fsr_generated_shader_dir}/fsr3upscaler.stamp")
set(_fsr_shader_driver "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/compile_fsr_shaders.py")
if(NOT EXISTS "${_fsr_shader_driver}")
    message(FATAL_ERROR "FSR shader driver is missing: ${_fsr_shader_driver}")
endif()

# Keep the fallback dependency list useful for Visual Studio, whose CMake
# generator does not consume DEPFILE.  It mirrors the driver's input set.
file(GLOB _fsr_shader_sources CONFIGURE_DEPENDS
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl")
file(GLOB_RECURSE _fsr_gpu_include_sources CONFIGURE_DEPENDS
    "${FSR_SDK_ROOT}/sdk/include/FidelityFX/gpu/*")
set(_fsr_shader_inputs
    ${_fsr_shader_sources}
    ${_fsr_gpu_include_sources}
    "${FSR_SDK_ROOT}/sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt"
    "${FSR_SDK_ROOT}/sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt"
    "${FSR_SDK_ROOT}/sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt"
    "${_fsr_shader_driver}")
if(FSR_GLSLANG)
    list(APPEND _fsr_shader_inputs "${FSR_GLSLANG}")
endif()
if(FSR_SPIRV_VAL)
    list(APPEND _fsr_shader_inputs "${FSR_SPIRV_VAL}")
endif()

set(_fsr_shader_depfile_args)
set(_fsr_shader_depfile_driver_args)
set(_fsr_shader_spirv_val_args)
if(FSR_SPIRV_VAL)
    set(_fsr_shader_spirv_val_args --spirv-val "${FSR_SPIRV_VAL}")
endif()
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(_fsr_shader_depfile "${_fsr_generated_shader_dir}/fsr3upscaler.d")
    set(_fsr_shader_depfile_args DEPFILE "${_fsr_shader_depfile}")
    set(_fsr_shader_depfile_driver_args --depfile "${_fsr_shader_depfile}")
endif()

add_custom_command(
    OUTPUT "${_fsr_shader_stamp}"
    COMMAND "${Python3_EXECUTABLE}" "${_fsr_shader_driver}"
        --sdk-root "${FSR_SDK_ROOT}"
        --ffx-sc "$<TARGET_FILE:FidelityFX_SC>"
        --glslang "${FSR_GLSLANG}"
        --out-dir "${_fsr_generated_shader_dir}"
        --stamp "${_fsr_shader_stamp}"
        ${_fsr_shader_depfile_driver_args}
        ${_fsr_shader_spirv_val_args}
    COMMAND "${CMAKE_COMMAND}" -E touch "${_fsr_shader_stamp}"
    DEPENDS FidelityFX_SC ${_fsr_shader_inputs}
    VERBATIM
    ${_fsr_shader_depfile_args})
add_custom_target(ffx_fsr3upscaler_shaders DEPENDS "${_fsr_shader_stamp}")

set(_fsr_stage_dir "${CMAKE_CURRENT_BINARY_DIR}/fsr_sdk_staged")
set(_fsr_temp_stage_dir "${CMAKE_CURRENT_BINARY_DIR}/fsr_sdk_staged.tmp")
file(REMOVE_RECURSE "${_fsr_temp_stage_dir}")
function(fsr_stage_sdk_file relative_path)
    set(source "${FSR_SDK_ROOT}/${relative_path}")
    set(destination "${_fsr_temp_stage_dir}/${relative_path}")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "FidelityFX SDK source is missing: ${source}")
    endif()
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_directory}")
    configure_file("${source}" "${destination}" COPYONLY)
    # configure_file(COPYONLY) preserves the SDK's read-only mode, while the
    # anchor-checked patcher needs to rewrite its disposable copy.
    file(CHMOD "${destination}"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    set(_fsr_staged_sources
        ${_fsr_staged_sources}
        "${destination}"
        PARENT_SCOPE)
endfunction()

set(_fsr_sdk_source_list
    sdk/src/shared/ffx_assert.cpp
    sdk/src/shared/ffx_breadcrumbs_list.cpp
    sdk/src/shared/ffx_message.cpp
    sdk/src/shared/ffx_object_management.cpp
    sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    sdk/src/components/fsr3upscaler/ffx_fsr3upscaler_private.h
    sdk/src/backends/vk/ffx_vk.cpp
    # Staged unconditionally so the publish loop stays symmetric; it is only
    # compiled into the D3D12 module, which is Windows-only.
    sdk/src/backends/dx12/ffx_dx12.cpp
    sdk/src/backends/shared/ffx_shader_blobs.cpp
    sdk/src/backends/shared/ffx_shader_blobs.h
    sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.h)

# The ffx-api layer. This is what the engine actually loads: it exposes five
# backend-independent entry points (ffxCreateContext/DestroyContext/Configure/
# Query/Dispatch) from a shared module, so the Vulkan and D3D12 builds export
# the same surface and the backend is chosen by chaining a create-context
# descriptor rather than by linking.
#
# ffx_provider_fsr2, ffx_provider_framegeneration and the two
# framegenerationswapchain providers are deliberately not staged; the registry
# in ffx_provider.cpp is trimmed to the upscaler by portable_ffx_api.cmake.
list(APPEND _fsr_sdk_source_list
    ffx-api/src/ffx_api.cpp
    ffx-api/src/ffx_provider.cpp
    ffx-api/src/ffx_provider.h
    ffx-api/src/ffx_provider_external.h
    ffx-api/src/ffx_provider_fsr3upscale.cpp
    ffx-api/src/ffx_provider_fsr3upscale.h
    ffx-api/src/backends.cpp
    ffx-api/src/backends.h
    ffx-api/src/validation.cpp
    ffx-api/src/validation.h)

# The ffx-api PUBLIC headers are staged rather than consumed in place, because
# ffx_api.h hard-codes FFX_API_ENTRY to __declspec(dllexport) with no compiler
# guard -- unlike the SDK's own FFX_API in sdk/include/FidelityFX/host/ffx_types.h,
# which is why sdk/include can be consumed in place. GCC and Clang on ELF targets
# reject the token outright ("'__declspec' attributes are not enabled"), so the
# pristine header cannot be compiled on Linux at all. These headers are included
# by the modules AND by the engine, so the patched copy has to be the one both
# see; portable_ffx_api.cmake rewrites the macro.
#
# ffx_api_loader.h is deliberately NOT staged: it #include <windows.h> and
# resolves the entry points with GetProcAddress, so it cannot serve the engine's
# Linux path. Leaving it unstaged makes an accidental use fail loudly.
list(APPEND _fsr_sdk_source_list
    ffx-api/include/ffx_api/ffx_api.h
    ffx-api/include/ffx_api/ffx_api.hpp
    ffx-api/include/ffx_api/ffx_api_types.h
    ffx-api/include/ffx_api/ffx_upscale.h
    ffx-api/include/ffx_api/ffx_upscale.hpp
    ffx-api/include/ffx_api/ffx_framegeneration.h
    ffx-api/include/ffx_api/ffx_framegeneration.hpp
    ffx-api/include/ffx_api/vk/ffx_api_vk.h
    ffx-api/include/ffx_api/dx12/ffx_api_dx12.h)

set(_fsr_staged_sources)
foreach(_fsr_sdk_source IN LISTS _fsr_sdk_source_list)
    fsr_stage_sdk_file("${_fsr_sdk_source}")
endforeach()

set(FSR_STAGED_VK_SOURCE "${_fsr_temp_stage_dir}/sdk/src/backends/vk/ffx_vk.cpp")
set(FSR_STAGED_FSR3_SOURCE "${_fsr_temp_stage_dir}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp")
set(FSR_STAGED_MESSAGE_SOURCE "${_fsr_temp_stage_dir}/sdk/src/shared/ffx_message.cpp")
set(FFX_API_STAGED_PROVIDER_SOURCE "${_fsr_temp_stage_dir}/ffx-api/src/ffx_provider.cpp")
set(FFX_API_STAGED_BACKENDS_SOURCE "${_fsr_temp_stage_dir}/ffx-api/src/backends.cpp")
set(FFX_API_STAGED_HEADER "${_fsr_temp_stage_dir}/ffx-api/include/ffx_api/ffx_api.h")
set(FFX_API_STAGED_PROVIDER_HEADER "${_fsr_temp_stage_dir}/ffx-api/src/ffx_provider.h")
set(FFX_API_STAGED_BACKENDS_HEADER "${_fsr_temp_stage_dir}/ffx-api/src/backends.h")
set(FFX_API_STAGED_FSR3UPSCALE_SOURCE
    "${_fsr_temp_stage_dir}/ffx-api/src/ffx_provider_fsr3upscale.cpp")
set(FFX_API_STAGED_DX12_HEADER
    "${_fsr_temp_stage_dir}/ffx-api/include/ffx_api/dx12/ffx_api_dx12.h")
include("${CMAKE_CURRENT_SOURCE_DIR}/patches/portable_sdk.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/patches/portable_ffx_api.cmake")

# Publish patched files only when their content changed. This keeps the real
# stage paths stable for downstream build tools while making reconfigure
# restage from the pristine SDK on every configure.
foreach(_fsr_sdk_source IN LISTS _fsr_sdk_source_list)
    set(_fsr_temp_source "${_fsr_temp_stage_dir}/${_fsr_sdk_source}")
    set(_fsr_stage_destination "${_fsr_stage_dir}/${_fsr_sdk_source}")
    get_filename_component(_fsr_stage_destination_directory
        "${_fsr_stage_destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${_fsr_stage_destination_directory}")
    if(EXISTS "${_fsr_stage_destination}")
        file(CHMOD "${_fsr_stage_destination}"
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endif()
    configure_file("${_fsr_temp_source}" "${_fsr_stage_destination}" COPYONLY)
    file(CHMOD "${_fsr_stage_destination}"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endforeach()

set(_fsr_compile_sources
    "${_fsr_stage_dir}/sdk/src/shared/ffx_assert.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_breadcrumbs_list.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_message.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_object_management.cpp"
    "${_fsr_stage_dir}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${_fsr_stage_dir}/sdk/src/backends/vk/ffx_vk.cpp"
    "${_fsr_stage_dir}/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "${_fsr_stage_dir}/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp")

add_library(ffx_fsr3upscaler_vk STATIC ${_fsr_compile_sources})
add_dependencies(ffx_fsr3upscaler_vk ffx_fsr3upscaler_shaders)
target_compile_features(ffx_fsr3upscaler_vk PRIVATE cxx_std_17)
set_target_properties(ffx_fsr3upscaler_vk PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(ffx_fsr3upscaler_vk PRIVATE FFX_FSR3UPSCALER)
target_include_directories(ffx_fsr3upscaler_vk PRIVATE
    "${FSR_SDK_ROOT}/sdk/include"
    "${FSR_SDK_ROOT}/sdk/src/shared"
    "${FSR_SDK_ROOT}/sdk/src/backends/shared"
    "${FSR_SDK_ROOT}/sdk/src/components"
    "${_fsr_generated_shader_dir}"
    "${FSR_VULKAN_HEADERS}"
    "${FSR_VOLK_HEADERS}"
    "${_fsr_stage_dir}/sdk/src/shared"
    "${_fsr_stage_dir}/sdk/src/backends/shared"
    "${_fsr_stage_dir}/sdk/src/components")

if(MSVC)
    target_compile_options(ffx_fsr3upscaler_vk PRIVATE
        /W3
        "/FI${FSR_VOLK_HEADERS}/volk.h")
    target_compile_definitions(ffx_fsr3upscaler_vk PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ffx_fsr3upscaler_vk PRIVATE
        "-include${FSR_VOLK_HEADERS}/volk.h"
        "-include${CMAKE_CURRENT_SOURCE_DIR}/ffx_portability.h")
endif()

# No Vulkan library is linked here. ffx_vk.cpp does call a handful of loader
# entry points directly (vkGetDeviceProcAddr, vkCreateBuffer,
# vkEnumerateDeviceExtensionProperties, vkGetPhysicalDeviceFeatures/Features2,
# vkGetPhysicalDeviceProperties/Properties2, vkGetPhysicalDeviceMemoryProperties),
# so the archive leaves those undefined on purpose: the engine resolves them from
# volk, which defines these names as pointer variables. The SDK must see Volk's
# declarations at compile time; otherwise its calls become direct jumps to the
# pointer storage. An ordinary successful link does not prove that the call ABI
# matches. Premake already links volk into every final executable. Adding
# -lvulkan here would fight that arrangement.

# ---------------------------------------------------------------------------
# Shared ffx-api modules
# ---------------------------------------------------------------------------
#
# The archive above stays as it is: it is what the Vulkan loader regression
# tests link against, because they reach internal ffx_vk.cpp symbols such as
# findMemoryTypeIndex that a shared module does not export.
#
# What the engine loads is a shared module per backend. Each exports the same
# five ffx-api entry points (ffxCreateContext, ffxDestroyContext, ffxConfigure,
# ffxQuery, ffxDispatch), so the engine resolves one symbol set regardless of
# backend and picks the backend by chaining a create-context descriptor.
#
# They must be separate modules rather than two archives: both contain
# ffx_shader_blobs.cpp defining ffxGetPermutationBlobByIndex, which both
# backends call. Linked as archives, the linker resolves that symbol once from
# whichever archive it reaches first and never pulls the second copy, silently
# handing one backend the other's shader blobs. As modules each keeps its own
# copy internally and neither takes part in the engine's link.

set(_ffx_api_common_sources
    "${_fsr_stage_dir}/sdk/src/shared/ffx_assert.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_breadcrumbs_list.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_message.cpp"
    "${_fsr_stage_dir}/sdk/src/shared/ffx_object_management.cpp"
    "${_fsr_stage_dir}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${_fsr_stage_dir}/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "${_fsr_stage_dir}/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${_fsr_stage_dir}/ffx-api/src/ffx_api.cpp"
    "${_fsr_stage_dir}/ffx-api/src/ffx_provider.cpp"
    "${_fsr_stage_dir}/ffx-api/src/ffx_provider_fsr3upscale.cpp"
    "${_fsr_stage_dir}/ffx-api/src/backends.cpp"
    "${_fsr_stage_dir}/ffx-api/src/validation.cpp")

# backend_api is spelled VK or DX12 and selects both the FFX_BACKEND_<api>
# switch inside ffx-api and the SDK backend source compiled into the module.
function(ffx_add_upscaler_module target backend_api backend_source generated_shader_dir)
    add_library(${target} SHARED
        ${_ffx_api_common_sources}
        "${backend_source}")

    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    # Export exactly the five FFX_API_ENTRY functions and nothing else.
    #
    # This is not tidiness. On ELF, RTLD_LOCAL stops the module's symbols from
    # entering the global scope, but it does NOT stop the executable's globals
    # from interposing the module's own references, and two collisions exist:
    #
    #   * ffx_vk_loader.cpp defines vkCreateBuffer, vkGetDeviceProcAddr and the
    #     rest as global symbols, and the engine links volk, which defines those
    #     same names as global pointer variables. Default visibility lets the
    #     module's loads and stores bind to volk's copies, silently bypassing
    #     ffxVkLoaderEnsureInitialized().
    #   * ffx_shader_blobs.cpp defines ffxGetPermutationBlobByIndex, which is
    #     exactly the cross-contamination the module/archive split below exists
    #     to prevent -- it reappears between the executable and the module for as
    #     long as anything in the process still links ffx_fsr3upscaler_vk.
    #
    # The resulting export count is assertable: `nm -D --defined-only` on the
    # Linux module should list five ' T ' symbols.
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    # Pin the module output directory the same way CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    # is pinned above, so premake's staging step has one path to copy from on
    # every generator instead of <bdir>/ on Ninja and <bdir>/<Config>/ on VS.
    #
    # Set per target rather than through CMAKE_RUNTIME_OUTPUT_DIRECTORY: that
    # variable would also move FidelityFX_SC, which the imported-executable reuse
    # above and tests/fsr/check_shader_regeneration.py both locate by path.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${FSR_OUTPUT_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${FSR_OUTPUT_BIN_DIR}")
    foreach(_fsr_module_config IN LISTS _fsr_archive_configs)
        string(TOUPPER "${_fsr_module_config}" _fsr_module_config_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_fsr_module_config_upper} "${FSR_OUTPUT_BIN_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_${_fsr_module_config_upper} "${FSR_OUTPUT_BIN_DIR}")
    endforeach()

    # FFX_BACKEND_<api> has to be 1 rather than empty: backends.cpp selects the
    # Vulkan case with `#elif FFX_BACKEND_VK`, which evaluates the macro as an
    # expression instead of testing whether it is defined.
    target_compile_definitions(${target} PRIVATE
        FFX_FSR3UPSCALER
        FFX_BACKEND_${backend_api}=1
        FFXDLL_VERSION_MAJOR=1
        FFXDLL_VERSION_MINOR=0
        FFXDLL_VERSION_PATCH=1
        FFXDLL_VERSION_API=${backend_api}
        CI_PIPELINE_ID=0
        _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

    target_include_directories(${target} PRIVATE
        "${FSR_SDK_ROOT}/sdk/include"
        # The staged ffx-api headers, not the pristine ones: portable_ffx_api.cmake
        # rewrites FFX_API_ENTRY there. The engine includes the same staged
        # directory, so both sides see one definition of the export macro.
        "${_fsr_stage_dir}/ffx-api/include"
        "${FSR_SDK_ROOT}/sdk/src/shared"
        "${FSR_SDK_ROOT}/sdk/src/backends/shared"
        "${FSR_SDK_ROOT}/sdk/src/components"
        "${generated_shader_dir}"
        "${_fsr_stage_dir}/sdk/src/shared"
        "${_fsr_stage_dir}/sdk/src/backends/shared"
        "${_fsr_stage_dir}/sdk/src/components"
        "${_fsr_stage_dir}/ffx-api/src")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)

        # FFX_BUILD_AS_DLL is MSVC-only, and the guard that reads it is the
        # reason why. sdk/include/FidelityFX/host/ffx_types.h spells it
        #
        #     #if defined(FFX_GCC) || !defined(FFX_BUILD_AS_DLL)
        #     #define FFX_API
        #     #else
        #     #define FFX_API __declspec(dllexport)
        #
        # and FFX_GCC is defined by nothing: not the SDK, not the compiler, not
        # this build. So on GCC and Clang the only reachable way out of that
        # guard is leaving FFX_BUILD_AS_DLL undefined; defining it there feeds
        # every FFX_API declaration a bare __declspec and the translation unit
        # dies at the first one. The static archive above has always compiled
        # this way on every platform.
        #
        # Nothing is lost by it. FFX_API decorates the SDK's internal entry
        # points, which ffx_provider_fsr3upscale.cpp and backends.cpp call from
        # inside this same module; the module's actual export surface is the
        # five FFX_API_ENTRY functions, which portable_ffx_api.cmake rewrites to
        # __attribute__((visibility("default"))) and which the visibility
        # settings above are there to keep alone.
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            FFX_BUILD_AS_DLL)
    else()
        target_compile_options(${target} PRIVATE
            "-include${CMAKE_CURRENT_SOURCE_DIR}/ffx_portability.h")
    endif()
endfunction()

ffx_add_upscaler_module(ffx_fsr3upscaler_api_vk VK
    "${_fsr_stage_dir}/sdk/src/backends/vk/ffx_vk.cpp"
    "${_fsr_generated_shader_dir}")
add_dependencies(ffx_fsr3upscaler_api_vk ffx_fsr3upscaler_shaders)

# Unlike the archive, this module cannot borrow volk's pointer variables from
# the executable, so it carries ffx_vk_loader.cpp and force-includes the header
# that turns the Vulkan API into pointers it resolves from the system loader.
# That keeps the Vulkan SDK out of the build the same way volk does for the
# engine -- only Vulkan-Headers is needed here.
target_sources(ffx_fsr3upscaler_api_vk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ffx_vk_loader.cpp")
target_include_directories(ffx_fsr3upscaler_api_vk PRIVATE
    "${FSR_VULKAN_HEADERS}"
    "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(ffx_fsr3upscaler_api_vk PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/ffx_vk_loader.h")
else()
    target_compile_options(ffx_fsr3upscaler_api_vk PRIVATE
        "-include${CMAKE_CURRENT_SOURCE_DIR}/ffx_vk_loader.h")
    target_link_libraries(ffx_fsr3upscaler_api_vk PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# D3D12 module
# ---------------------------------------------------------------------------
#
# Windows-only, and only when FidelityFX_SC was built with its DXC half: the
# permutations are HLSL compiled to DXIL, which no other toolchain here can
# produce. The DXC that does it is vendored by the SDK, so this adds no
# dependency beyond the pinned archive.
if(WIN32 AND FSR_ENABLE_HLSL)
    set(_fsr_dx12_generated_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders_dx12")
    set(_fsr_dx12_shader_stamp "${_fsr_dx12_generated_shader_dir}/fsr3upscaler.stamp")

    # A separate directory is not optional: the D3D12 and Vulkan permutation
    # headers have identical filenames and would otherwise overwrite each other.
    file(GLOB _fsr_dx12_shader_sources CONFIGURE_DEPENDS
        "${FSR_SDK_ROOT}/sdk/src/backends/dx12/shaders/fsr3upscaler/*.hlsl")
    set(_fsr_dx12_shader_inputs
        ${_fsr_dx12_shader_sources}
        ${_fsr_gpu_include_sources}
        "${FSR_SDK_ROOT}/sdk/src/backends/dx12/CMakeShadersFSR3Upscaler.txt"
        "${FSR_SDK_ROOT}/sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt"
        "${FSR_SDK_ROOT}/sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt"
        "${_fsr_shader_driver}"
        "${_fsr_dxc_dll}")

    set(_fsr_dx12_depfile_args)
    set(_fsr_dx12_depfile_driver_args)
    if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        set(_fsr_dx12_depfile "${_fsr_dx12_generated_shader_dir}/fsr3upscaler.d")
        set(_fsr_dx12_depfile_args DEPFILE "${_fsr_dx12_depfile}")
        set(_fsr_dx12_depfile_driver_args --depfile "${_fsr_dx12_depfile}")
    endif()

    add_custom_command(
        OUTPUT "${_fsr_dx12_shader_stamp}"
        COMMAND "${Python3_EXECUTABLE}" "${_fsr_shader_driver}"
            --backend dx12
            --sdk-root "${FSR_SDK_ROOT}"
            --ffx-sc "$<TARGET_FILE:FidelityFX_SC>"
            --dxc-dll "${_fsr_dxc_dll}"
            --out-dir "${_fsr_dx12_generated_shader_dir}"
            --stamp "${_fsr_dx12_shader_stamp}"
            ${_fsr_dx12_depfile_driver_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_fsr_dx12_shader_stamp}"
        DEPENDS FidelityFX_SC ${_fsr_dx12_shader_inputs}
        VERBATIM
        ${_fsr_dx12_depfile_args})
    add_custom_target(ffx_fsr3upscaler_dx12_shaders DEPENDS "${_fsr_dx12_shader_stamp}")

    ffx_add_upscaler_module(ffx_fsr3upscaler_api_dx12 DX12
        "${_fsr_stage_dir}/sdk/src/backends/dx12/ffx_dx12.cpp"
        "${_fsr_dx12_generated_shader_dir}")
    add_dependencies(ffx_fsr3upscaler_api_dx12 ffx_fsr3upscaler_dx12_shaders)

    # d3dx12.h ships inside the SDK, so no Agility SDK headers are needed. The
    # module shares the process's D3D12 runtime, which the executable selects
    # through its own D3D12SDKVersion/D3D12SDKPath exports.
    target_link_libraries(ffx_fsr3upscaler_api_dx12 PRIVATE d3d12 dxgi dxguid)

    # ffx_dx12.cpp includes "pix/pix3.h" for GPU capture markers; the SDK
    # vendors PIX under sdk/libs. WinPixEventRuntime.dll itself is loaded at
    # run time and its absence only disables the markers, so nothing extra has
    # to be deployed.
    target_include_directories(ffx_fsr3upscaler_api_dx12 PRIVATE
        "${FSR_SDK_ROOT}/sdk/libs")

    # Mirrors the SDK's own build: NOMINMAX and WIN32_LEAN_AND_MEAN from
    # sdk/src/backends/dx12/CMakeLists.txt, UNICODE/_UNICODE from
    # sdk/CMakeLists.txt. Without NOMINMAX the max() macro swallows
    # std::numeric_limits<UINT>::max(); without UNICODE the backend's wide
    # LoadLibrary call resolves to the ANSI entry point.
    target_compile_definitions(ffx_fsr3upscaler_api_dx12 PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)

    # Stage the vendored DXC's licences beside the SDK's own. dxcompiler.dll and
    # dxil.dll are build-time only and are deliberately not deployed.
    foreach(_dxc_license LICENSE-LLVM.txt LICENSE-MIT.txt LICENSE-MS.txt)
        set(_dxc_license_source
            "${FSR_SDK_ROOT}/sdk/tools/ffx_shader_compiler/libs/dxc/${_dxc_license}")
        if(NOT EXISTS "${_dxc_license_source}")
            message(FATAL_ERROR "Vendored DXC licence file is missing: ${_dxc_license_source}")
        endif()
        configure_file("${_dxc_license_source}"
            "${_fsr_license_dir}/FidelityFX-SDK-dxc-${_dxc_license}" COPYONLY)
        file(CHMOD "${_fsr_license_dir}/FidelityFX-SDK-dxc-${_dxc_license}"
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endforeach()
endif()
