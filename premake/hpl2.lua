-- HPL2 engine -- static library.
local CORE = ROOT .. "/HPL2/core"

project "HPL2"
    kind "StaticLib"
    language "C++"
    set_output("static")
    -- USE_OALWRAPPER only selects the legacy OALWrapper/-prefixed header path in
    -- LowLevelSoundOpenAL.cpp and OpenALSound{Data,Channel}.h (it gates no backend
    -- code -- those sources are compiled unconditionally via the glob below). The
    -- legacy OALWrapper layout put headers under include/OALWrapper/; the vendored
    -- layout puts them under HPL2/extern/OpenAL/OpenAL/, so the #else "OpenAL/"
    -- prefix is the correct one. Leave the macro undefined to take that branch.
    defines { "USE_SDL2" }

    -- All source patterns are run through glob()/os.matchfiles; patterns matching
    -- nothing are silently dropped (several literal names in the impl list don't
    -- exist on disk).
    local IMPL = CORE .. "/sources/impl/"
    local common_dirs = {
        "ai", "engine", "generate", "graphics", "gui", "haptic", "input",
        "math", "physics", "resources", "scene", "sound", "system",
    }
    local patterns = {}
    for _, d in ipairs(common_dirs) do
        table.insert(patterns, CORE .. "/sources/" .. d .. "/*.cpp")
        table.insert(patterns, CORE .. "/sources/" .. d .. "/*.c")
    end
    -- Implementation-specific sources.
    local impl_patterns = {
        "SqScript.cpp", "scriptarray.cpp", "scripthelper.cpp",
        "scriptstring.cpp", "scriptstring_utils.cpp",
        "BitmapLoader*", "*Newton.cpp", "LegacyVertexBuffer.cpp",
        "GamepadSDL.cpp", "GamepadSDL2.cpp", "KeyboardSDL.cpp", "MouseSDL.cpp",
        "TimerSDL.cpp", "LowLevelInputSDL.cpp",
        "LowLevelResourcesSDL.cpp", "LowLevelSystemSDL.cpp", "SDLEngineSetup.cpp",
        "SDLFontData.cpp", "LowLevelSoundOpenAL.cpp", "OpenAL*",
        "MeshLoaderCollada.cpp", "MeshLoaderColladaHelpers.cpp",
        "MeshLoaderColladaLoader.cpp", "MeshLoaderMSH.cpp", "MeshLoaderFBX.cpp",
        "MeshLoaderGLTF.cpp",
        "ThreadSDL.cpp", "MutexSDL.cpp", "VertexBuffer.cpp",
    }
    for _, p in ipairs(impl_patterns) do table.insert(patterns, IMPL .. p) end
    table.insert(patterns, CORE .. "/sources/platform/sdl2/*.cpp")
    local file_list = glob(patterns)
    -- RID3D12.cpp is Windows-only (opt-in DX12 backend). Prune it from the source
    -- list on non-Windows targets so gmake doesn't emit a compile target for it;
    -- the file's own `#if DEVICE_IMPL_D3D12` guard already keeps it a trivially
    -- empty TU on Windows Vulkan-only builds.
    if os.target() ~= "windows" then
        for i = #file_list, 1, -1 do
            if file_list[i]:match("RID3D12%.cpp$") then
                table.remove(file_list, i)
            end
        end
    end
    files (file_list)
    files { CORE .. "/include/**.h" }   -- headers for IDE/source groups
    memory_engine()
    memory_rebuild_engine()

    filter "system:linux"
        files (glob { IMPL .. "PlatformUnix.cpp", IMPL .. "PlatformSDL.cpp" })
    filter "system:windows"
        files (glob { IMPL .. "PlatformWin32.cpp", IMPL .. "MutexWin32.cpp", IMPL .. "ThreadWin32.cpp" })
    filter "system:macosx"
        -- macOS is not currently built/verified in this setup; PlatformMacOSX.mm
        -- (Cocoa + FSEvents file watcher) pairs with the SDL thread/mutex/timer impls.
        files (glob { IMPL .. "PlatformMacOSX.mm", IMPL .. "PlatformSDL.cpp" })
        links { "CoreServices.framework" }   -- FSEvents (cFileWatcherFSEvents)
    filter {}

    -- BuildID_HPL2_0.h is committed under core/include and only referenced from
    -- a commented-out line, so no generated source is needed here.

    -- Include directories. premake does not propagate dependency includes, so
    -- everything HPL2's sources #include must be listed explicitly.
    includedirs {
        CORE .. "/include",
        ROOT .. "/HPL2/include",            -- BuildID_HPL2_0.h
        ROOT .. "/amnesia/glsl",
        ROOT .. "/amnesia/slang",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
        DEPS_EXTERN .. "/rapidjson/include", -- RIProgram's reflection parser
        DEPS_EXTERN .. "/zlib",             -- zlib.h/zconf.h for BinaryBuffer/SerializeClass
        DEPS_EXTERN .. "/cgltf",            -- cgltf.h single-header glTF 2.0 parser (MeshLoaderGLTF)
    }
    generated_includes()
    d3d12ma_includes()
    if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
        defines { "DEVICE_SUPPORT_D3D12" }
        dependson { "D3D12MA" }
        slang_d3d12_mips_prebuild()
    end
    deps_public_includes()   -- ogg/vorbis/IL/Newton/OALWrapper public headers
    vulkan_includes()
    link_sdl2()      -- SDL2 headers + link + dependson
    link_openal()    -- openal-soft headers + link + dependson
    link_nrd()       -- NRD denoiser headers + link + dependson
    link_fsr()       -- FidelityFX Super Resolution headers + link + dependson
    xess_use()       -- Intel XeSS headers + availability define (Windows only)
    mathlib_use()

    -- Keep HPL2 aware of its dependency set; final executables still call
    -- link_engine() because static-library links are not relied on transitively.
    links {
        "OALWrapper", "AngelScript", "Newton", "tinyxml2",
        "vorbisfile", "vorbis", "ogg", "freealut",
        "zlib", "volk", "IL", "png", "jpeg",
    }

    filter "system:linux"
        links { "pthread", "dl" }
    filter "toolset:gcc or clang"
        exceptionhandling "Off"             -- mirrors -fno-exceptions
        linkgroups "On"                     -- resolve circular static-lib deps
    filter "system:windows"
        -- _NEWTON_USE_LIB matches the define in premake/deps/newton.lua so
        -- Newton.h drops its __declspec(dllimport) decoration on consumer
        -- TUs (we link the static Newton.lib, not a DLL).
        -- AL_LIBTYPE_STATIC does the same for OpenAL Soft's AL/ALC headers.
        defines {
            "WIN32_LEAN_AND_MEAN",
            "SDL_MAIN_HANDLED",
            "IL_STATIC_LIB",
            "_NEWTON_USE_LIB=1",
            "AL_LIBTYPE_STATIC",
        }
        -- Premake's MSVC projects include STL-heavy generated headers; keeping
        -- unwind semantics enabled avoids C4530 warnings from headers like <vector>.
        exceptionhandling "On"
    filter {}
