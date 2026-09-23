# Portability patches for the GLSL-only FidelityFX_SC build.
#
# This file is included after the required SDK sources have been copied into
# _FFX_SC_STAGE_DIR. Every replacement checks its anchor, so an SDK source
# change fails configuration instead of silently producing a different tool.

function(ffx_sc_replace_required _file _old _new _description)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "FidelityFX_SC portability patch '${_description}' cannot read ${_file}")
    endif()

    file(READ "${_file}" _contents)
    string(FIND "${_contents}" "${_old}" _anchor_position)
    if(_anchor_position LESS 0)
        message(FATAL_ERROR "FidelityFX_SC portability patch '${_description}' did not find its SDK anchor in ${_file}")
    endif()

    string(REPLACE "${_old}" "${_new}" _patched_contents "${_contents}")
    file(WRITE "${_file}" "${_patched_contents}")
endfunction()

set(_ffx_sc_pch "${_FFX_SC_STAGE_DIR}/src/pch.hpp")
set(_ffx_sc_pch_old [=[#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atlcomcli.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
#include <exception>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <bitset>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <d3d12shader.h>

#include <filesystem>
namespace fs = std::filesystem;

#include <process.hpp>
namespace tpl = TinyProcessLib;]=])
set(_ffx_sc_pch_new [=[#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

#include <process.hpp>
namespace tpl = TinyProcessLib;]=])
# The HLSL half needs the Windows and DXC headers back. Keep the portable
# standard-library block, since the other patches in this file rely on it.
#
# ATL is an optional Visual Studio component that is not installed on the CI
# image, so upstream's <atlcomcli.h> is replaced by WRL's ComPtr from the
# Windows SDK. The tool only uses operator->, operator& (including through
# IID_PPV_ARGS) and Release(); the first two are API-compatible, and Release()
# is spelled Reset() in WRL, so a thin subclass covers the difference.
if(FSR_ENABLE_HLSL)
    set(_ffx_sc_pch_new [=[#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wrl/client.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

template <typename T>
struct CComPtr : Microsoft::WRL::ComPtr<T>
{
    using Microsoft::WRL::ComPtr<T>::ComPtr;

    // ATL releases and nulls the pointer; WRL spells that Reset().
    void Release() { this->Reset(); }
};

namespace fs = std::filesystem;

#include <process.hpp>
namespace tpl = TinyProcessLib;]=])
endif()
ffx_sc_replace_required("${_ffx_sc_pch}" "${_ffx_sc_pch_old}" "${_ffx_sc_pch_new}" "portable standard-library precompiled header")

set(_ffx_sc_main "${_FFX_SC_STAGE_DIR}/src/ffx_sc.cpp")
set(_ffx_sc_main_includes_old [=[#include "hlsl_compiler.h"
#include "glsl_compiler.h"
#include "utils.h"

#include <Windows.h>
#include <pathcch.h>
#include <vector>
#include <string_view>
#include <filesystem>
#include <unordered_set>
#include <locale>
#include <stdexcept>


#pragma comment(lib, "pathcch.lib")]=])
set(_ffx_sc_main_includes_new [=[#ifdef FFX_SC_ENABLE_HLSL
#include "hlsl_compiler.h"
#endif
#include "glsl_compiler.h"
#include "utils.h"

#include <vector>
#include <string_view>
#include <filesystem>
#include <unordered_set>
#include <locale>
#include <stdexcept>]=])
ffx_sc_replace_required("${_ffx_sc_main}" "${_ffx_sc_main_includes_old}" "${_ffx_sc_main_includes_new}" "GLSL-only compiler includes")

set(_ffx_sc_output_path_old [=[void LaunchParameters::EnsureOutputPathExistsAndMakeCanonical(std::wstring& inoutOutputPath)
{
    std::replace(inoutOutputPath.begin(), inoutOutputPath.end(), L'/', L'\\');

    PWSTR canonicalOutputPath = NULL;

    // Make the path canonical, convert to long path if needed and add the trailing slash
    HRESULT hr = PathAllocCanonicalize(inoutOutputPath.c_str(), PATHCCH_ALLOW_LONG_PATHS | PATHCCH_ENSURE_TRAILING_SLASH, &canonicalOutputPath);
    if (hr == S_OK)
    {
        PWSTR componentStart = NULL;

        // Find the first character after "root" indicator -- which means a folder (path component) or file
        hr = PathCchSkipRoot(canonicalOutputPath, &componentStart);
        if (hr == S_OK)
        {
            // Try search for the next delimiter
            wchar_t* componentEnd = wcsstr(componentStart, L"\\");

            // If the delimiter is found, make sure the folder is created
            while (componentEnd != NULL)
            {
                // Temporally replace delimiter '\\' with null-terminator, create directory, and restore the delimiter
                *componentEnd = L'\0';
                CreateDirectoryW(canonicalOutputPath, NULL);
                *componentEnd = L'\\';

                // advance to the next component (file or folder) and try to find the next delimiter (meaning -- it's folder), and repeat the loop.
                componentStart = componentEnd + 1;
                componentEnd = wcsstr(componentStart, L"\\");
            }
        }
        inoutOutputPath = canonicalOutputPath;
        LocalFree(canonicalOutputPath);
    }
}]=])
set(_ffx_sc_output_path_new [=[void LaunchParameters::EnsureOutputPathExistsAndMakeCanonical(std::wstring& inoutOutputPath)
{
    fs::path outputPath = fs::u8path(WCharToUTF8(inoutOutputPath));
    if (outputPath.empty())
        throw std::runtime_error("Shader output path is empty.");

    fs::create_directories(outputPath);
    inoutOutputPath = UTF8ToWChar(fs::absolute(outputPath).u8string());
}]=])
ffx_sc_replace_required("${_ffx_sc_main}" "${_ffx_sc_output_path_old}" "${_ffx_sc_output_path_new}" "portable output-directory creation")

set(_ffx_sc_full_path_old [=[std::wstring Application::MakeFullPath(const std::wstring & outputPath, const std::wstring & fileName)
{
    // Append file name, optionally converting to long path again, because the outputPath alone could be normal path, but when filename is added -- it could become a long path
    PWSTR canonicalFileNameRaw = NULL;
    HRESULT hr = PathAllocCombine(outputPath.c_str(), fileName.c_str(), PATHCCH_ALLOW_LONG_PATHS, &canonicalFileNameRaw);

    if (S_OK == hr)
    {
        std::wstring canonicalFileName(canonicalFileNameRaw);
        LocalFree(canonicalFileNameRaw);
        return canonicalFileName;
    }
    return fileName;
}]=])
set(_ffx_sc_full_path_new [=[std::wstring Application::MakeFullPath(const std::wstring & outputPath, const std::wstring & fileName)
{
    fs::path fullPath = fs::u8path(WCharToUTF8(outputPath)) / fs::u8path(WCharToUTF8(fileName));
    return UTF8ToWChar(fullPath.u8string());
}]=])
ffx_sc_replace_required("${_ffx_sc_main}" "${_ffx_sc_full_path_old}" "${_ffx_sc_full_path_new}" "portable output-file path joining")

set(_ffx_sc_compiler_selection_old [=[    if (m_Params.compiler.empty())
    {
        // Check file extension
        size_t       extensionPos = m_Params.inputFile.find_last_of('.');
        std::wstring extension    = m_Params.inputFile.substr(extensionPos + 1, m_Params.inputFile.size() - extensionPos - 1);

        if (extension == L"hlsl")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::DXC, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (extension == L"glsl")
            m_Compiler = std::unique_ptr<GLSLCompiler>(new GLSLCompiler(glslangExe, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else
            throw std::runtime_error("Unknown shader source file extension. Please use the -compiler option to specify which compiler to use.");
    }
    else
    {
        if (m_Params.compiler == L"dxc")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::DXC, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"gdk.scarlett.x64")
            m_Compiler = std::unique_ptr<HLSLCompiler>(new HLSLCompiler(
                HLSLCompiler::GDK_SCARLETT_X64, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"gdk.xboxone.x64")
            m_Compiler = std::unique_ptr<HLSLCompiler>(new HLSLCompiler(
                HLSLCompiler::GDK_XBOXONE_X64, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"fxc")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::FXC, d3dDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"glslang")
            m_Compiler = std::unique_ptr<GLSLCompiler>(new GLSLCompiler(glslangExe, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else
            throw std::runtime_error("Unknown compiler requested (valid options: dxc, fxc or glslang)");
    }]=])
set(_ffx_sc_compiler_selection_new [=[    if (m_Params.compiler.empty())
    {
        // Check file extension
        size_t       extensionPos = m_Params.inputFile.find_last_of('.');
        std::wstring extension    = m_Params.inputFile.substr(extensionPos + 1, m_Params.inputFile.size() - extensionPos - 1);

#ifdef FFX_SC_ENABLE_HLSL
        if (extension == L"hlsl")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::DXC, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else
#endif
        if (extension == L"glsl")
            m_Compiler = std::unique_ptr<GLSLCompiler>(new GLSLCompiler(glslangExe, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else
            throw std::runtime_error("This FidelityFX_SC build supports GLSL only; use a .glsl source file.");
    }
    else
    {
#ifdef FFX_SC_ENABLE_HLSL
        if (m_Params.compiler == L"dxc")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::DXC, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"gdk.scarlett.x64")
            m_Compiler = std::unique_ptr<HLSLCompiler>(new HLSLCompiler(
                HLSLCompiler::GDK_SCARLETT_X64, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"gdk.xboxone.x64")
            m_Compiler = std::unique_ptr<HLSLCompiler>(new HLSLCompiler(
                HLSLCompiler::GDK_XBOXONE_X64, dxcDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"fxc")
            m_Compiler = std::unique_ptr<HLSLCompiler>(
                new HLSLCompiler(HLSLCompiler::FXC, d3dDll, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else if (m_Params.compiler == L"glslang")
#else
        if (m_Params.compiler == L"glslang")
#endif
            m_Compiler = std::unique_ptr<GLSLCompiler>(new GLSLCompiler(glslangExe, shaderPath, shaderName, shaderFileName, outputPath, m_Params.disableLogs, m_Params.debugCompile));
        else
            throw std::runtime_error("Unknown compiler requested; this FidelityFX_SC build supports only -compiler=glslang.");
    }]=])
ffx_sc_replace_required("${_ffx_sc_main}" "${_ffx_sc_compiler_selection_old}" "${_ffx_sc_compiler_selection_new}" "GLSL-only compiler selection")

ffx_sc_replace_required("${_ffx_sc_main}" "_wfopen_s(&fp, outputPath.c_str(), L\"wb\");" "fp = fopen(WCharToUTF8(outputPath).c_str(), \"wb\");" "portable permutation header file open")
ffx_sc_replace_required("${_ffx_sc_main}" "_wfopen_s(&fp, depfilePath.c_str(), L\"wb\");" "fp = fopen(WCharToUTF8(depfilePath).c_str(), \"wb\");" "portable GCC depfile open")

set(_ffx_sc_glsl_compiler "${_FFX_SC_STAGE_DIR}/src/glsl_compiler.cpp")
ffx_sc_replace_required(
    "${_ffx_sc_glsl_compiler}"
    "std::string cmdLine = m_GlslangExe + \" \";"
    "std::string cmdLine = \"\\\"\" + m_GlslangExe + \"\\\" \";"
    "quote glslang executable path")

ffx_sc_replace_required(
    "${_ffx_sc_glsl_compiler}"
    "        std::snprintf(out_ptr, 32, \"%02x\", sig[i]);"
    "        std::snprintf(out_ptr, sizeof(out) - static_cast<size_t>(out_ptr - out), \"%02x\", sig[i]);"
    "bound MD5 digest formatter")

set(_ffx_sc_main_old [=[int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc <= 1)
        {
            LaunchParameters::PrintCommandLineSyntax();
            return 1;
        }

        LaunchParameters params;
        params.ParseCommandLine(argc - 1, argv + 1);

        Application app(params);
        app.Process();

        return 0;
    }
    catch (const std::exception& ex)
    {   
        fprintf(stderr, "ffx_sc failed: %s\n", ex.what());
        fflush(stderr);
        return -1;
    }
}]=])
set(_ffx_sc_main_new [=[int main(int argc, char** argv)
{
    try
    {
        if (argc <= 1)
        {
            LaunchParameters::PrintCommandLineSyntax();
            return 1;
        }

        std::vector<std::wstring> wideArguments;
        std::vector<const wchar_t*> wideArgumentPointers;
        wideArguments.reserve(static_cast<size_t>(argc - 1));
        wideArgumentPointers.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i)
            wideArguments.push_back(UTF8ToWChar(argv[i]));
        for (const auto& argument : wideArguments)
            wideArgumentPointers.push_back(argument.c_str());

        LaunchParameters params;
        params.ParseCommandLine(argc - 1, wideArgumentPointers.data());

        Application app(params);
        app.Process();

        return 0;
    }
    catch (const std::exception& ex)
    {
        fprintf(stderr, "ffx_sc failed: %s\n", ex.what());
        fflush(stderr);
        return -1;
    }
}]=])
ffx_sc_replace_required("${_ffx_sc_main}" "${_ffx_sc_main_old}" "${_ffx_sc_main_new}" "portable UTF-8 process entry point")

# DXBCChecksum backs the FXC path inside hlsl_compiler.cpp, so it is only
# unused when the HLSL half is fenced off.
if(NOT FSR_ENABLE_HLSL)
    set(_ffx_sc_utils_h "${_FFX_SC_STAGE_DIR}/src/utils.h")
    ffx_sc_replace_required("${_ffx_sc_utils_h}" "#include \"DXBCChecksum.h\"\n" "" "remove unused DXBC checksum dependency from GLSL build")
endif()

set(_ffx_sc_utils "${_FFX_SC_STAGE_DIR}/src/utils.cpp")
set(_ffx_sc_utils_old [=[#include "utils.h"

std::string WCharToUTF8(const std::wstring& wstr)
{
    if (wstr.empty())
        return std::string();

    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.size(), nullptr, 0, nullptr, nullptr);

    std::string str;
    str.resize(size);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.size(), &str[0], size, nullptr, nullptr);

    return str;
}

std::wstring UTF8ToWChar(const std::string& str)
{
    if (str.empty())
        return std::wstring();

    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), nullptr, 0);

    std::wstring wstr;
    wstr.resize(size);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), &wstr[0], size);

    return wstr;
}]=])
set(_ffx_sc_utils_new [=[#include "utils.h"

std::string WCharToUTF8(const std::wstring& wstr)
{
    if (wstr.empty())
        return std::string();

#ifdef _WIN32
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(wstr);
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(wstr);
#endif
}

std::wstring UTF8ToWChar(const std::string& str)
{
    if (str.empty())
        return std::wstring();

#ifdef _WIN32
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(str);
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(str);
#endif
}]=])
ffx_sc_replace_required("${_ffx_sc_utils}" "${_ffx_sc_utils_old}" "${_ffx_sc_utils_new}" "portable UTF-8 conversion")
