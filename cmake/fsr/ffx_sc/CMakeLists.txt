cmake_minimum_required(VERSION 3.17)

project(FidelityFX_SC LANGUAGES CXX)

set(FSR_SDK_ROOT "" CACHE PATH "FidelityFX-SDK root directory (the directory containing sdk/)")
if(NOT FSR_SDK_ROOT)
    message(FATAL_ERROR "FSR_SDK_ROOT is not set. Point it at the FidelityFX-SDK-1.1.4 directory containing sdk/.")
endif()

get_filename_component(FSR_SDK_ROOT "${FSR_SDK_ROOT}" ABSOLUTE)
set(_FFX_SC_SDK_TOOL_ROOT "${FSR_SDK_ROOT}/sdk/tools/ffx_shader_compiler")
set(_FFX_SC_SDK_SRC_ROOT "${_FFX_SC_SDK_TOOL_ROOT}/src")
set(_FFX_SC_SDK_LIB_ROOT "${_FFX_SC_SDK_TOOL_ROOT}/libs")

# The D3D12 permutations are HLSL compiled to DXIL by DXC, so the tool's HLSL
# half has to be built as well. It is Windows-only: it needs the Windows SDK's
# COM headers and dxcapi.h, and it is only ever used to produce DXIL.
#
# The SDK vendors its own DXC under sdk/tools/ffx_shader_compiler/libs/dxc --
# headers, dxcompiler.dll, dxil.dll and an import library -- so enabling this
# adds no toolchain dependency beyond the already pinned SDK archive.
set(FSR_ENABLE_HLSL_DEFAULT OFF)
if(WIN32)
    set(FSR_ENABLE_HLSL_DEFAULT ON)
endif()
set(FSR_ENABLE_HLSL "${FSR_ENABLE_HLSL_DEFAULT}" CACHE BOOL
    "Build the HLSL/DXC half of FidelityFX_SC (required for the D3D12 backend)")

set(_FFX_SC_DXC_ROOT "${_FFX_SC_SDK_TOOL_ROOT}/libs/dxc")
if(FSR_ENABLE_HLSL)
    foreach(_required_dxc_path
            "${_FFX_SC_DXC_ROOT}/inc/dxcapi.h"
            "${_FFX_SC_DXC_ROOT}/inc/d3d12shader.h"
            "${_FFX_SC_DXC_ROOT}/bin/x64/dxcompiler.dll")
        if(NOT EXISTS "${_required_dxc_path}")
            message(FATAL_ERROR
                "FSR_ENABLE_HLSL is ON but the SDK's vendored DXC is incomplete: ${_required_dxc_path}")
        endif()
    endforeach()
endif()

foreach(_required_path
        "${FSR_SDK_ROOT}/sdk"
        "${_FFX_SC_SDK_SRC_ROOT}/ffx_sc.cpp"
        "${_FFX_SC_SDK_SRC_ROOT}/glsl_compiler.cpp"
        "${_FFX_SC_SDK_SRC_ROOT}/compiler.h"
        "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/spirv_reflect.c"
        "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/include/spirv/unified1/spirv.h")
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "FSR_SDK_ROOT does not look like FidelityFX-SDK-1.1.4: missing ${_required_path}")
    endif()
endforeach()

# Do not edit the SDK. The source files are copied to a private build-tree
# staging area and patched there so an SDK update cannot modify the checkout.
set(_FFX_SC_REAL_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc_staged")
set(_FFX_SC_TEMP_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc_staged.tmp")
file(REMOVE_RECURSE "${_FFX_SC_TEMP_STAGE_DIR}")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/src")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/MD5")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect")
file(MAKE_DIRECTORY "${_FFX_SC_TEMP_STAGE_DIR}/libs/tiny-process-library")

# Patch a disposable copy. The final configure_file(COPYONLY) pass below
# publishes only changed files, preserving the staged source mtimes across a
# no-op reconfigure while still making a real patch change visible to the
# compiler.
set(_FFX_SC_STAGE_DIR "${_FFX_SC_TEMP_STAGE_DIR}")

function(ffx_sc_copy_writable source destination)
    configure_file("${source}" "${destination}" COPYONLY)
    # configure_file(COPYONLY) preserves the SDK's read-only mode, while the
    # patcher needs to rewrite these disposable copies.
    file(CHMOD "${destination}"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endfunction()

set(_FFX_SC_SRC_FILES
    ffx_sc.cpp
    glsl_compiler.cpp
    glsl_compiler.h
    compiler.h
    utils.cpp
    utils.h
    pch.cpp
    pch.hpp)
if(FSR_ENABLE_HLSL)
    # DXBCChecksum belongs to the FXC path, which shares hlsl_compiler.cpp with
    # DXC; it has to come along even though only DXC is used here.
    list(APPEND _FFX_SC_SRC_FILES
        hlsl_compiler.cpp
        hlsl_compiler.h
        DXBCChecksum.cpp
        DXBCChecksum.h)
endif()

foreach(_source_file IN LISTS _FFX_SC_SRC_FILES)
    ffx_sc_copy_writable("${_FFX_SC_SDK_SRC_ROOT}/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/src/${_source_file}")
endforeach()

foreach(_source_file md5.cpp md5.h md5_loc.h)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/MD5/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/MD5/${_source_file}")
endforeach()

foreach(_source_file spirv_reflect.c spirv_reflect.h)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}")
endforeach()
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1")
ffx_sc_copy_writable(
    "${_FFX_SC_SDK_LIB_ROOT}/SPIRV-Reflect/include/spirv/unified1/spirv.h"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h")

foreach(_source_file process.cpp process.hpp process_unix.cpp process_win.cpp)
    ffx_sc_copy_writable("${_FFX_SC_SDK_LIB_ROOT}/tiny-process-library/${_source_file}"
                         "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/${_source_file}")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../patches/portable_glsl.cmake")

set(_FFX_SC_STAGE_DIR "${_FFX_SC_REAL_STAGE_DIR}")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/src")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/MD5")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1")
file(MAKE_DIRECTORY "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library")

foreach(_source_file IN LISTS _FFX_SC_SRC_FILES)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/src/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/src/${_source_file}" COPYONLY)
endforeach()

foreach(_source_file md5.cpp md5.h md5_loc.h)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/MD5/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/MD5/${_source_file}" COPYONLY)
endforeach()

foreach(_source_file spirv_reflect.c spirv_reflect.h)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/${_source_file}" COPYONLY)
endforeach()
configure_file(
    "${_FFX_SC_TEMP_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/include/spirv/unified1/spirv.h" COPYONLY)

foreach(_source_file process.cpp process.hpp process_unix.cpp process_win.cpp)
    configure_file("${_FFX_SC_TEMP_STAGE_DIR}/libs/tiny-process-library/${_source_file}"
                   "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/${_source_file}" COPYONLY)
endforeach()

set(_FFX_SC_REFLECT_SOURCE "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/spirv_reflect.c")
set_source_files_properties("${_FFX_SC_REFLECT_SOURCE}" PROPERTIES LANGUAGE CXX)
if(WIN32)
    set(_FFX_SC_PROCESS_PLATFORM_SOURCE "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process_win.cpp")
else()
    set(_FFX_SC_PROCESS_PLATFORM_SOURCE "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process_unix.cpp")
endif()

set(_FFX_SC_STAGED_SRC_FILES)
foreach(_source_file IN LISTS _FFX_SC_SRC_FILES)
    list(APPEND _FFX_SC_STAGED_SRC_FILES "${_FFX_SC_STAGE_DIR}/src/${_source_file}")
endforeach()

add_executable(FidelityFX_SC
    ${_FFX_SC_STAGED_SRC_FILES}
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5.cpp"
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5.h"
    "${_FFX_SC_STAGE_DIR}/libs/MD5/md5_loc.h"
    "${_FFX_SC_REFLECT_SOURCE}"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect/spirv_reflect.h"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process.cpp"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library/process.hpp"
    "${_FFX_SC_PROCESS_PLATFORM_SOURCE}")

target_compile_features(FidelityFX_SC PRIVATE cxx_std_17)
set_target_properties(FidelityFX_SC PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(FidelityFX_SC PRIVATE
    "${_FFX_SC_STAGE_DIR}/src"
    "${_FFX_SC_STAGE_DIR}/libs/MD5"
    "${_FFX_SC_STAGE_DIR}/libs/SPIRV-Reflect"
    "${_FFX_SC_STAGE_DIR}/libs/tiny-process-library")

if(FSR_ENABLE_HLSL)
    # FFX_SC_ENABLE_HLSL re-enables the HLSL branches that portable_glsl.cmake
    # fences off. dxcompiler is loaded at run time through -dxcdll, so only the
    # SDK's vendored headers are needed at build time.
    target_compile_definitions(FidelityFX_SC PRIVATE FFX_SC_ENABLE_HLSL)
    target_include_directories(FidelityFX_SC PRIVATE "${_FFX_SC_DXC_ROOT}/inc")
    # hlsl_compiler.cpp carries the FXC path alongside DXC, and its reflection
    # helper references IID_ID3D11ShaderReflection. Only this GUID is needed;
    # both compilers are still loaded dynamically.
    target_link_libraries(FidelityFX_SC PRIVATE dxguid)
endif()

if(MSVC)
    target_compile_options(FidelityFX_SC PRIVATE /W3)
    target_compile_definitions(FidelityFX_SC PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(FidelityFX_SC PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(FidelityFX_SC PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
