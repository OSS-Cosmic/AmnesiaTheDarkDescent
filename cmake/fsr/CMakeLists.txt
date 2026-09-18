cmake_minimum_required(VERSION 3.20)

project(FidelityFX_FSR3Upscaler_Vulkan LANGUAGES CXX)

set(FSR_SDK_ROOT "" CACHE PATH "FidelityFX SDK 1.1.4 root")
set(FSR_VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers include directory")
set(FSR_VOLK_HEADERS "" CACHE PATH "volk include directory")
set(FSR_OUTPUT_LIB_DIR "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Directory for the FSR static archive")

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
    sdk/src/backends/shared/ffx_shader_blobs.cpp
    sdk/src/backends/shared/ffx_shader_blobs.h
    sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.h)
set(_fsr_staged_sources)
foreach(_fsr_sdk_source IN LISTS _fsr_sdk_source_list)
    fsr_stage_sdk_file("${_fsr_sdk_source}")
endforeach()

set(FSR_STAGED_VK_SOURCE "${_fsr_temp_stage_dir}/sdk/src/backends/vk/ffx_vk.cpp")
set(FSR_STAGED_FSR3_SOURCE "${_fsr_temp_stage_dir}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp")
set(FSR_STAGED_MESSAGE_SOURCE "${_fsr_temp_stage_dir}/sdk/src/shared/ffx_message.cpp")
include("${CMAKE_CURRENT_SOURCE_DIR}/patches/portable_sdk.cmake")

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
