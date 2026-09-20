-- premake/external.lua -- CMake-driven SDL2, openal-soft, NRD, and FSR.
--
-- These libraries ship large, feature-probing CMake build systems that are
-- impractical (and brittle) to reimplement in premake. Each is wrapped in a
-- premake kind "Makefile" project whose buildcommands run the library's own
-- CMake (configure + build) into build-premake/external/<name>/<config>.
-- Consumers pull in the headers/links via link_sdl2() / link_openal() / link_nrd().
--
-- Linux keeps the shared-library deployment flow and copies the resulting .so
-- files next to the game. Windows builds static SDL2/openal-soft libraries, so
-- no SDL2/OpenAL DLLs need to be deployed next to the executable.

EXT_ROOT = ROOT .. "/build-premake/external"
local RUNTIME_LIBS = ROOT .. "/build-premake/amnesia/%{cfg.buildcfg}/libs"
-- Kept for any future Windows shared-library externals; SDL2/openal-soft are
-- static on Windows and therefore do not use this path.
local RUNTIME_EXE  = ROOT .. "/build-premake/amnesia/%{cfg.buildcfg}"
local function winpath(p) return (p:gsub("/", "\\")) end
local SDL2_PROJECT = "SDL2"
local OPENAL_PROJECT = "OpenALSoft"
local NRD_PROJECT = "NRD"
-- Keep the Premake wrapper project distinct from the archive's linker name.
-- Otherwise links { "NRD" } is resolved as a project dependency and gmake2
-- omits -lNRD from final executable link lines.
local NRD_BUILD_PROJECT = "NRDExternal"

-- unix_args / win_args: platform-specific CMake configure arguments.
-- copy_glob: optional posix shared-lib glob copied next to the game (Linux, into libs/).
-- win_glob: optional Windows DLL glob copied next to the .exe.
-- copy_dir: optional directory copied next to the game on both platforms.
-- Makefile projects have no reliable output timestamp for MSBuild to use, so
-- more than one top-level consumer can enter the same wrapper concurrently.
-- A named mutex keeps a shared external build tree single-writer while still
-- allowing unrelated configurations (and unrelated externals) to build in
-- parallel. The commands are passed through cmd.exe so their existing quoting
-- and generator-specific arguments remain unchanged.
local function windows_serialized_commands(commands, mutex_name)
    local body = {}
    for _, command in ipairs(commands) do
        table.insert(body, string.format("& cmd.exe /D /C '%s'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }", command))
    end
    -- The generated command is itself parsed by cmd.exe. Escape the quotes
    -- belonging to the nested CMake commands so quoted executable/path names
    -- survive both cmd.exe and powershell.exe argument parsing.
    local script = string.format(
        "& {$mutex = [System.Threading.Mutex]::new($false, 'Local\\%s'); $acquired = $false; try {try {$mutex.WaitOne(); $acquired = $true} catch [System.Threading.AbandonedMutexException] {$acquired = $true}; %s} finally {if ($acquired) {$mutex.ReleaseMutex()}; $mutex.Dispose()}}",
        mutex_name, table.concat(body, " "))
    script = script:gsub('"', '\\"')
    return "powershell -NoProfile -ExecutionPolicy Bypass -Command \"" .. script .. "\""
end

local function cmake_makefile(name, srcdir, unix_args, win_args, copy_glob, win_glob, copy_dir, serialize_name)
    local bdir = EXT_ROOT .. "/" .. name .. "/%{cfg.buildcfg}"
    local unix_configure = string.format('"%s" -Wno-deprecated -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=%%{cfg.buildcfg} %s',
        CMAKE, srcdir, bdir, unix_args)
    local windows_configure = string.format('"%s" -Wno-deprecated -G "Visual Studio 17 2022" -A x64 -S "%s" -B "%s" -DCMAKE_BUILD_TYPE=%%{cfg.buildcfg} %s',
        CMAKE, srcdir, bdir, win_args)
    -- The outer solution already builds independent external projects in
    -- parallel. Keep each nested CMake/MSBuild invocation single-project so
    -- Debug PDB writers cannot multiply past mspdbsrv's practical limits.
    local build = string.format('"%s" --build "%s" --config %%{cfg.buildcfg} --parallel 1', CMAKE, bdir)
    project(name)
        kind "Makefile"
        location (ROOT .. "/build-premake/projects")

        filter "system:not windows"
            buildcommands {
                unix_configure,
                build,
            }
        filter "system:windows"
            -- Force the Visual Studio generator so the output layout matches the
            -- multi-config libdirs below regardless of whether ninja is on PATH.
            -- Without this, CMake picks Ninja when available and writes .lib into
            -- <bdir>/ instead of <bdir>/<config>/, breaking the link stage.
            if serialize_name then
                buildcommands {
                    windows_serialized_commands({ windows_configure, build }, serialize_name),
                }
            else
                buildcommands { windows_configure, build }
            end
        filter {}
        filter "system:not windows"
            rebuildcommands { build }
        filter "system:windows"
            rebuildcommands {
                serialize_name and windows_serialized_commands({ build }, serialize_name) or build,
            }
        filter {}
        cleancommands {
            string.format('{RMDIR} "%s"', bdir),
        }
        if copy_glob then
            filter "system:not windows"
                buildcommands {
                    string.format('mkdir -p "%s"', RUNTIME_LIBS),
                    string.format('cp -P %s "%s"/ 2>/dev/null || true', bdir .. "/" .. copy_glob, RUNTIME_LIBS),
                }
        end
        if win_glob then
            filter "system:windows"
                buildcommands {
                    string.format('if not exist "%s" mkdir "%s"', winpath(RUNTIME_EXE), winpath(RUNTIME_EXE)),
                    string.format('copy /Y "%s" "%s\\"', winpath(bdir .. "/%{cfg.buildcfg}/" .. win_glob), winpath(RUNTIME_EXE)),
                }
        end
        if copy_dir then
            local runtime_copy_dir = runtime_dir(copy_dir)
            filter "system:not windows"
                buildcommands {
                    string.format('mkdir -p "%s"', runtime_copy_dir),
                    -- Staged permissions are fixed in cmake/fsr/CMakeLists.txt;
                    -- keep -f so older read-only destinations or future
                    -- read-only inputs are replaced instead of failing.
                    string.format('cp -Rf "%s"/. "%s"/', bdir .. "/" .. copy_dir, runtime_copy_dir),
                }
            filter "system:windows"
                buildcommands {
                    string.format('if not exist "%s" mkdir "%s"', winpath(runtime_copy_dir), winpath(runtime_copy_dir)),
                    -- /R is the Windows counterpart of cp -f for read-only files.
                    string.format('xcopy /E /I /Y /R "%s\\*" "%s\\" >nul',
                        winpath(bdir .. "/" .. copy_dir), winpath(runtime_copy_dir)),
                }
        end
        filter {}
end

-- ---- SDL2 -----------------------------------------------------------------
-- /FS on Windows: SDL2's CMake build enables /MP via CMake's Visual Studio
-- generator defaults, so the compile PDB writes must be serialised or MSBuild
-- fails with C1041 under parallel builds. Same reasoning applies to openal-soft
-- and NRD below.
cmake_makefile(SDL2_PROJECT,
    DEPS_EXTERN .. "/SDL",
    "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_INSTALL=ON -DSDL_RPATH=OFF",
    "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_INSTALL=ON -DSDL_RPATH=OFF "
        .. "-DCMAKE_C_FLAGS=\"/FS\" -DCMAKE_CXX_FLAGS=\"/FS\"",
    "libSDL2*.so*", nil, nil, "Redux-Amnesia-SDL2-%{cfg.buildcfg}")

function link_sdl2()
    dependson { SDL2_PROJECT }
    -- ORDER MATTERS: the generated SDL_config.h (which #defines the X11/Wayland
    -- video drivers) must be found before the submodule's committed
    -- include/SDL_config.h, whose Linux fallback is SDL_config_minimal.h (no
    -- video drivers). With the wrong order the engine compiles SDL_syswm with no
    -- SDL_VIDEO_DRIVER_* defined and GetWindowHandle() hits assert(false).
    includedirs {
        -- generated SDL_config.h / SDL_revision.h from the SDL2 build tree --
        -- FIRST so they win over the submodule's committed SDL_config.h (whose
        -- Linux fallback is SDL_config_minimal.h). The generated config also matches
        -- exactly what the SDL library was built with (X11/Wayland drivers, etc.).
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include-config-release/SDL2",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include-config-debug/SDL2",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include",
        EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/include/SDL2",
        -- submodule public headers (committed SDL_config.h resolves to minimal)
        DEPS_EXTERN .. "/SDL/include",
        DEPS_EXTERN .. "/SDL/include/SDL2",
    }
    filter "system:linux"
        libdirs { EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}" }
    filter { "system:linux", "configurations:Release" }
        links { "SDL2-2.0" }
    filter { "system:linux", "configurations:Debug" }
        links { "SDL2-2.0d" }
    filter "system:windows"
        -- Support both single-config generators (Ninja, archive in the build
        -- root) and multi-config generators (Visual Studio, config subdir).
        libdirs {
            EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}",
            EXT_ROOT .. "/" .. SDL2_PROJECT .. "/%{cfg.buildcfg}/%{cfg.buildcfg}",
        }
    -- SDL2's CMake build applies the debug postfix 'd', so the Debug static
    -- library on disk is SDL2-staticd.lib.
    filter { "system:windows", "configurations:Debug" }
        links { "SDL2-staticd" }
    filter { "system:windows", "configurations:Release" }
        links { "SDL2-static" }
    filter {}
end

-- ---- openal-soft -----------------------------------------------------------
-- ALSOFT_ENABLE_MODULES=OFF: openal-soft auto-enables C++20 modules with the
-- "Visual Studio 17 2022" generator on MSVC 14.34+ (and Ninja on GCC 15+/Clang 17+).
-- Its sources then `import alc.context;` / `import gsl;`, which the MSBuild module
-- build fails to resolve here (C2230 could not find module 'alc.context'). The
-- non-module #include path is the supported fallback, so force it everywhere.
cmake_makefile(OPENAL_PROJECT,
    DEPS_EXTERN .. "/openal-soft",
    "-DLIBTYPE=SHARED -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL=OFF "
        .. "-DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF "
        .. "-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF",
    "-DLIBTYPE=STATIC -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL=OFF "
        .. "-DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF "
        .. "-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF "
        .. "-DCMAKE_C_FLAGS=\"/FS\" -DCMAKE_CXX_FLAGS=\"/FS /EHsc /wd4267 /wd4875\"",
    "libopenal.so*", nil, nil, "Redux-Amnesia-OpenAL-%{cfg.buildcfg}")

function link_openal()
    dependson { OPENAL_PROJECT }
    includedirs {
        DEPS_EXTERN .. "/openal-soft/include",
        DEPS_EXTERN .. "/openal-soft/include/AL",
    }
    filter "system:linux"
        libdirs { EXT_ROOT .. "/" .. OPENAL_PROJECT .. "/%{cfg.buildcfg}" }
        links { "openal" }
    filter "system:windows"
        libdirs {
            EXT_ROOT .. "/" .. OPENAL_PROJECT .. "/%{cfg.buildcfg}",
            EXT_ROOT .. "/" .. OPENAL_PROJECT .. "/%{cfg.buildcfg}/%{cfg.buildcfg}",
        }
        links { "OpenAL32" }
    filter {}
end

-- ---- NRD (NVIDIA Real-Time Denoisers) --------------------------------------
-- NRD's CMake compiles its HLSL denoiser shaders with ShaderMake and embeds the
-- resulting blobs into the library. ShaderMake downloads a pinned DXC at CMake
-- configure time ("ShaderMake: downloading DXC v1.8.2505..."), so no DXC has to
-- be supplied by this repo -- the slangc-only toolchain here is untouched, NRD's
-- shaders never pass through slangc.
--
-- MathLib is pulled by NRD's own FetchContent into its build tree; that copy is
-- private to NRD and deliberately separate from HPL2/extern/MathLib (mathlib_use()).
--
-- Only SPIR-V blobs are embedded: this engine is Vulkan-only, and DXIL/DXBC would
-- additionally require FXC on Windows.
--
-- NRD_SHADERS_PATH / CMAKE_*_OUTPUT_DIRECTORY are overridden because NRD defaults
-- them to _Shaders/ and _Bin/ *inside its own source tree*, which would dirty the
-- submodule on every build.
local NRD_BUILD = EXT_ROOT .. "/" .. NRD_PROJECT .. "/%{cfg.buildcfg}"
-- The CMake *binary* dir, which cmake_makefile() derives from the project name
-- (NRDExternal), not from NRD_BUILD. Anything CMake places relative to the build
-- tree instead of the overridden output dirs -- notably FetchContent's
-- _deps/shadermake-build -- lives under here.
local NRD_CMAKE_BINARY = EXT_ROOT .. "/" .. NRD_BUILD_PROJECT .. "/%{cfg.buildcfg}"
local NRD_COMMON_ARGS =
    "-DNRD_STATIC_LIBRARY=ON -DNRD_NRI=OFF -DNRD_EMBEDS_SPIRV_SHADERS=ON "
        .. string.format('-DNRD_SHADERS_PATH="%s/_Shaders" ', NRD_BUILD)
        .. string.format('-DCMAKE_RUNTIME_OUTPUT_DIRECTORY="%s/_Bin" ', NRD_BUILD)
        .. string.format('-DCMAKE_LIBRARY_OUTPUT_DIRECTORY="%s/_Bin"', NRD_BUILD)

-- /EHsc on Windows: MSVC 14.44's STL emits C4530 from `__msvc_ostream.hpp`
-- when instantiating `std::operator<<` without unwind semantics, and NRD's
-- FetchContent'd ShaderMakeBlob target compiles with /W4 /WX, so the missing
-- /EHsc breaks the ShaderBlob.cpp build and prevents NRD.lib from linking.
-- Setting CMAKE_CXX_FLAGS overrides CMake's implicit /EHsc default, so it has
-- to be re-added explicitly here (same pattern as openal-soft above).
cmake_makefile(NRD_BUILD_PROJECT,
    DEPS_EXTERN .. "/NRD",
    NRD_COMMON_ARGS,
    NRD_COMMON_ARGS .. " -DNRD_EMBEDS_DXIL_SHADERS=OFF -DNRD_EMBEDS_DXBC_SHADERS=OFF "
        .. "-DCMAKE_C_FLAGS=\"/FS\" -DCMAKE_CXX_FLAGS=\"/FS /EHsc\"",
    nil, nil, nil, "Redux-Amnesia-NRD-%{cfg.buildcfg}")

function link_nrd()
    dependson { NRD_BUILD_PROJECT }
    -- NRD.h includes NRDDescs.h/NRDSettings.h from the same directory and
    -- nothing else beyond <cstddef>/<cstdint>, so Include/ is the whole public
    -- surface. (Integration/ is deliberately not exposed: NRDIntegration.hpp
    -- needs NRI, which is disabled via -DNRD_NRI=OFF.)
    includedirs {
        DEPS_EXTERN .. "/NRD/Include",
    }
    -- NRD links ShaderMakeBlob PRIVATE, which for a static library means the
    -- consumer still has to resolve ShaderMake::FindPermutationInBlob().
    filter "system:linux"
        libdirs {
            NRD_BUILD .. "/_Bin",
            NRD_CMAKE_BINARY .. "/_deps/shadermake-build",
        }
    filter "system:windows"
        -- MSBuild is multi-config, so it appends the configuration name to the
        -- output directories.
        libdirs {
            NRD_BUILD .. "/_Bin/%{cfg.buildcfg}",
            NRD_BUILD .. "/_Bin",
            NRD_CMAKE_BINARY .. "/_deps/shadermake-build/%{cfg.buildcfg}",
            NRD_CMAKE_BINARY .. "/_deps/shadermake-build",
        }
    filter {}
    links { "NRD", "ShaderMakeBlob" }
end

-- ---- FidelityFX Super Resolution ------------------------------------------
-- The CMake wrapper builds the Vulkan FSR 3.1 upscaler from the pinned SDK and
-- emits one configuration-independent archive under its build directory.
local FSR_VERSION = "1.1.4"
local FSR_DEPS = ROOT .. "/build-premake/_deps/fsr-sdk"
local FSR_ARCHIVE = FSR_DEPS .. "/v" .. FSR_VERSION .. ".zip"
local FSR_ARCHIVE_URL = "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/zip/refs/tags/v"
    .. FSR_VERSION
local FSR_ROOT_NAME = "FidelityFX-SDK-" .. FSR_VERSION
local FSR_BUILD_PROJECT = "FsrSdkExternal"
local FSR_BUILD = EXT_ROOT .. "/" .. FSR_BUILD_PROJECT .. "/%{cfg.buildcfg}"

-- The codeload .zip is not downloaded during normal development because the
-- extracted SDK is checked first. Regenerate the digest with:
--   curl -L "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/zip/refs/tags/v1.1.4" -o build-premake/_deps/fsr-sdk/v1.1.4.zip && sha256sum build-premake/_deps/fsr-sdk/v1.1.4.zip
-- The download path stays fail-closed if this is ever cleared.
-- (The .tar.gz of the same tag is 25c46398a656150397597f78d44bf7cb445e9e177dd116b309bbfdf50d50cc9f;
-- the .zip is what premake's zip.extract can read, so that is what is pinned.)
local FSR_ZIP_SHA256 = "9a19e1f20bf269163f052186ed563209c5a1d0f7b1cf7e567e643dee6cc61179"

local function fsr_sdk_root_is_valid(root)
    return os.isdir(root .. "/sdk/include")
end

local function resolve_fsr_sdk()
    local requested = _OPTIONS["fsr-sdk-dir"]
    if requested then
        local root = path.getabsolute(requested)
        if not fsr_sdk_root_is_valid(root) then
            error("FSR: --fsr-sdk-dir must name a FidelityFX SDK root containing sdk/include: " .. root)
        end
        return root
    end

    -- An extracted tree wins over every archive path. In particular, do not
    -- re-extract the SDK supplied in build-premake/_deps/fsr-sdk.
    local extracted = FSR_DEPS .. "/" .. FSR_ROOT_NAME
    if fsr_sdk_root_is_valid(extracted) then return extracted end

    os.mkdir(FSR_DEPS)
    if not FSR_ZIP_SHA256 then
        error("FSR: the v" .. FSR_VERSION .. " codeload .zip SHA-256 is not pinned; "
            .. "set FSR_ZIP_SHA256 from the command in the comment above before downloading")
    end
    if not os.isfile(FSR_ARCHIVE) then
        print("FSR: downloading " .. FSR_ARCHIVE_URL)
        local res, code = http.download(FSR_ARCHIVE_URL, FSR_ARCHIVE, {})
        if res ~= "OK" then
            os.remove(FSR_ARCHIVE)
            error(string.format("FSR: download failed (%s, code %s). URL: %s",
                tostring(res), tostring(code), FSR_ARCHIVE_URL))
        end
    end

    local hash_command
    if os.target() == "windows" then
        hash_command = 'certutil -hashfile "' .. winpath(FSR_ARCHIVE) .. '" SHA256'
    else
        hash_command = 'sha256sum "' .. FSR_ARCHIVE .. '"'
    end
    local hash_output = os.outputof(hash_command) or ""
    local actual_hash
    for candidate in hash_output:gmatch("%x+") do
        if #candidate == 64 then
            actual_hash = candidate:lower()
            break
        end
    end
    if not actual_hash then
        error("FSR: could not calculate SHA-256 for " .. FSR_ARCHIVE)
    end
    if actual_hash ~= FSR_ZIP_SHA256:lower() then
        error(string.format("FSR: SHA-256 mismatch for %s (expected %s, got %s)",
            FSR_ARCHIVE, FSR_ZIP_SHA256, actual_hash))
    end

    print("FSR: extracting " .. FSR_ARCHIVE)
    zip.extract(FSR_ARCHIVE, FSR_DEPS)
    if not fsr_sdk_root_is_valid(extracted) then
        error("FSR: could not locate sdk/include in extracted archive at " .. extracted)
    end
    return extracted
end

local function require_fsr_cmake()
    local version = os.outputof('"' .. CMAKE .. '" --version 2>&1') or ""
    if not version:lower():find("cmake version", 1, true) then
        error("FSR: cmake is required when FSR is enabled; could not run '" .. CMAKE .. " --version'")
    end
end

local FSR_ENABLED = _OPTIONS["with-fsr"] ~= "no"
if FSR_ENABLED then
    require_fsr_cmake()
    FSR_SDK_ROOT = resolve_fsr_sdk()

    local fsr_common_args = string.format(
        '-DFSR_SDK_ROOT="%s" -DFSR_VULKAN_HEADERS="%s" -DFSR_OUTPUT_LIB_DIR="%s/lib" -DFSR_VOLK_HEADERS="%s"',
        FSR_SDK_ROOT, ROOT .. "/HPL2/extern/Vulkan-Headers/include", FSR_BUILD,
        ROOT .. "/HPL2/extern/volk")
    if _OPTIONS["python"] then
        fsr_common_args = fsr_common_args .. ' -DPython3_EXECUTABLE="' .. _OPTIONS["python"] .. '"'
    end
    if _OPTIONS["glslang"] then
        fsr_common_args = fsr_common_args .. ' -DFSR_GLSLANG="' .. _OPTIONS["glslang"] .. '"'
    end
    if _OPTIONS["spirv-val"] then
        fsr_common_args = fsr_common_args .. ' -DFSR_SPIRV_VAL="' .. _OPTIONS["spirv-val"] .. '"'
    end

    cmake_makefile(FSR_BUILD_PROJECT,
        ROOT .. "/cmake/fsr",
        fsr_common_args,
        fsr_common_args .. " -DCMAKE_C_FLAGS=\"/FS\" -DCMAKE_CXX_FLAGS=\"/FS\"",
        nil, nil, "licenses", "Redux-Amnesia-FSR-%{cfg.buildcfg}")
end

function link_fsr()
    if not FSR_ENABLED then
        defines { "HPL2_FSR_AVAILABLE=0" }
        return
    end

    dependson { FSR_BUILD_PROJECT }
    includedirs { FSR_SDK_ROOT .. "/sdk/include" }
    libdirs { FSR_BUILD .. "/lib" }
    links { "ffx_fsr3upscaler_vk" }
    defines { "HPL2_FSR_AVAILABLE=1" }
end

-- The shader-blob validation executable includes the same staged SDK headers
-- that were used to build the archive. Keep these paths behind a helper so
-- tests.lua does not need to reach into FSR_BUILD/FSR_ENABLED locals.
function fsr_shader_blob_test_use()
    if not FSR_ENABLED then
        return false
    end

    includedirs {
        FSR_BUILD .. "/fsr_sdk_staged/sdk/src/backends/shared",
        FSR_BUILD .. "/fsr_sdk_staged/sdk/src/backends/shared/blob_accessors",
        FSR_BUILD .. "/fsr_sdk_staged/sdk/src/components",
        FSR_BUILD .. "/generated_shaders",
    }
    defines { "FFX_FSR3UPSCALER" }
    return true
end

-- ---- Intel XeSS -----------------------------------------------------------
-- XeSS is a prebuilt Vulkan SDK. The engine loads libxess.dll dynamically at
-- runtime, so there is no CMake build or import-library link here.
local XESS_VERSION = "3.0.2"
local XESS_DEPS = ROOT .. "/build-premake/_deps/xess-sdk"
local XESS_ARCHIVE = XESS_DEPS .. "/XeSS_SDK_3.0.2.zip"
local XESS_ARCHIVE_URL = "https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip"
local XESS_ROOT_NAME = "XeSS_SDK_3.0.2"

-- The v3.0.2 tag resolves to commit 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0.
-- The release .zip is not downloaded when the staged/extracted SDK is present.
-- Regenerate the digest with:
--   curl -L "https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip" -o build-premake/_deps/xess-sdk/XeSS_SDK_3.0.2.zip && sha256sum build-premake/_deps/xess-sdk/XeSS_SDK_3.0.2.zip
-- The download path stays fail-closed if this is ever cleared.
local XESS_ZIP_SHA256 = "88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990"

local function xess_sdk_root_is_valid(root)
    return os.isfile(root .. "/inc/xess/xess_vk.h")
end

local function resolve_xess_sdk()
    local requested = _OPTIONS["xess-sdk-dir"]
    if requested then
        local root = path.getabsolute(requested)
        if not xess_sdk_root_is_valid(root) then
            error("XeSS: --xess-sdk-dir must name a XeSS SDK root containing inc/xess/xess_vk.h: " .. root)
        end
        return root
    end

    -- An extracted tree wins over every archive path. In particular, reuse the
    -- SDK supplied in build-premake/_deps/xess-sdk without downloading it.
    local extracted = XESS_DEPS .. "/" .. XESS_ROOT_NAME
    if xess_sdk_root_is_valid(extracted) then return extracted end

    os.mkdir(XESS_DEPS)
    if not XESS_ZIP_SHA256 then
        error("XeSS: the v" .. XESS_VERSION .. " release .zip SHA-256 is not pinned; "
            .. "set XESS_ZIP_SHA256 from the command in the comment above before downloading")
    end
    if not os.isfile(XESS_ARCHIVE) then
        print("XeSS: downloading " .. XESS_ARCHIVE_URL)
        local res, code = http.download(XESS_ARCHIVE_URL, XESS_ARCHIVE, {})
        if res ~= "OK" then
            os.remove(XESS_ARCHIVE)
            error(string.format("XeSS: download failed (%s, code %s). URL: %s",
                tostring(res), tostring(code), XESS_ARCHIVE_URL))
        end
    end

    local hash_command
    if os.target() == "windows" then
        hash_command = 'certutil -hashfile "' .. winpath(XESS_ARCHIVE) .. '" SHA256'
    else
        hash_command = 'sha256sum "' .. XESS_ARCHIVE .. '"'
    end
    local hash_output = os.outputof(hash_command) or ""
    local actual_hash
    for candidate in hash_output:gmatch("%x+") do
        if #candidate == 64 then
            actual_hash = candidate:lower()
            break
        end
    end
    if not actual_hash then
        error("XeSS: could not calculate SHA-256 for " .. XESS_ARCHIVE)
    end
    if actual_hash ~= XESS_ZIP_SHA256:lower() then
        error(string.format("XeSS: SHA-256 mismatch for %s (expected %s, got %s)",
            XESS_ARCHIVE, XESS_ZIP_SHA256, actual_hash))
    end

    print("XeSS: extracting " .. XESS_ARCHIVE)
    os.mkdir(extracted)
    -- The release archive is flat (inc/, lib/, bin/, ...), so extract directly
    -- into the named SDK directory rather than into XESS_DEPS.
    zip.extract(XESS_ARCHIVE, extracted)
    if not xess_sdk_root_is_valid(extracted) then
        error("XeSS: could not locate inc/xess/xess_vk.h in extracted archive at " .. extracted)
    end
    return extracted
end

local XESS_ENABLED = _OPTIONS["with-xess"] ~= "no" and os.target() == "windows"
local XESS_SDK_ROOT
if XESS_ENABLED then
    XESS_SDK_ROOT = resolve_xess_sdk()
end

-- Add XeSS's public headers and availability define without adding runtime
-- deployment commands to the HPL2 static-library project.
function xess_use()
    if not XESS_ENABLED then
        defines { "HPL2_XESS_AVAILABLE=0" }
        return
    end

    includedirs { XESS_SDK_ROOT .. "/inc" }
    defines { "HPL2_XESS_AVAILABLE=1" }
end

-- Stage the shared XeSS runtime once for the whole game/tool output directory.
-- Attaching these copies to every final executable makes a parallel solution
-- build overwrite libxess.dll from several post-build events at once, which can
-- intermittently fail with "Access is denied".
function xess_declare_staging_projects()
    if not XESS_ENABLED then return end

    local runtime = runtime_dir("")
    local license_dir = runtime_dir("licenses/xess")
    local commands = {
        string.format('if not exist "%s" mkdir "%s"', winpath(runtime), winpath(runtime)),
        string.format('if exist "%s" (copy /Y "%s" "%s\\" >nul) else (echo XeSS: missing runtime DLL "%s")',
            winpath(XESS_SDK_ROOT .. "/bin/libxess.dll"),
            winpath(XESS_SDK_ROOT .. "/bin/libxess.dll"), winpath(runtime),
            winpath(XESS_SDK_ROOT .. "/bin/libxess.dll")),
        string.format('if not exist "%s" mkdir "%s"', winpath(license_dir), winpath(license_dir)),
    }
    for _, item in ipairs({
        { filename = "LICENSE.txt", description = "license file" },
        { filename = "third-party-programs.txt", description = "third-party notices" },
    }) do
        local source = winpath(XESS_SDK_ROOT .. "/" .. item.filename)
        local destination = winpath(license_dir .. "/" .. item.filename)
        table.insert(commands, string.format('if exist "%s" attrib -R "%s" >nul 2>&1',
            destination, destination))
        table.insert(commands, string.format('if exist "%s" (copy /Y "%s" "%s\\" >nul) else (echo XeSS: missing %s "%s")',
            source, source, winpath(license_dir), item.description, source))
    end

    project "XeSSRuntimeGame"
        kind "Makefile"
        location (ROOT .. "/build-premake/projects")
        filter "system:windows"
            buildcommands(commands)
            rebuildcommands(commands)
        filter {}
end

function link_xess(target_layout)
    xess_use()
    if not XESS_ENABLED then
        return
    end
    -- Engine-linked test executables do not load XeSS. Deploying the same DLL
    -- from every parallel smoke-test postbuild races on the shared game output.
    if target_layout == "tests" then
        return
    end
    dependson { "XeSSRuntimeGame" }
end
