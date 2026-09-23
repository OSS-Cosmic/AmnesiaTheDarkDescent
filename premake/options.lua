-- premake/options.lua -- command-line options for the Premake build.

newoption {
    trigger = "build-version",
    value = "VERSION",
    description = "Release version displayed in the main menu (default V0000).",
    default = "V0000",
}

newoption {
    trigger = "slangc",
    value = "PATH",
    description = "Path to a slangc executable. If omitted, the script reuses one already "
        .. "extracted under build-premake/_deps/slang-prebuilt, "
        .. "otherwise it downloads the pinned release "
        .. "(SLANG_VERSION in premake/slang.lua) at configure time."
}

newoption {
    trigger = "with-fsr",
    value = "yes/no",
    description = "Build and link the FidelityFX Super Resolution SDK (default yes).",
    allowed = { { "yes", "Build FSR" }, { "no", "Skip FSR" } },
    default = "yes",
}

newoption {
    trigger = "with-fsr-hlsl",
    value = "yes/no",
    description = "Build the FSR D3D12 runtime module and its DXIL shader permutations "
        .. "(requires Windows and --with-d3d12=yes; ignored elsewhere). Turning this off "
        .. "leaves FSR available on Vulkan only.",
    allowed = { { "yes", "Build the FSR D3D12 module" }, { "no", "Skip the FSR D3D12 module" } },
    default = "yes",
}

newoption {
    trigger = "fsr-sdk-dir",
    value = "PATH",
    description = "Path to a local FidelityFX SDK root containing sdk/ (skips SDK acquisition).",
}

newoption {
    trigger = "with-xess",
    value = "yes/no",
    description = "Enable the optional Intel XeSS super-resolution backend (Windows only; ignored on Linux).",
    allowed = { { "yes", "Build XeSS" }, { "no", "Skip XeSS" } },
    default = "yes",
}

newoption {
    trigger = "with-d3d12",
    value = "yes/no",
    description = "Build the DirectX 12 runtime backend (Windows only; enabled by default there). "
        .. "Windows builds compile both backends and default to D3D12 at runtime, with --vulkan "
        .. "on the game's command line to override; other platforms are Vulkan-only.",
    allowed = { { "yes", "Build DX12 backend" }, { "no", "Skip DX12 backend" } },
    -- Platform-derived. Premake resolves _TARGET_OS before any project script
    -- runs, so os.target() is valid here and honours an explicit --os= for
    -- cross-generation. Off Windows this stays "no", which keeps the explicit
    -- --with-d3d12=yes guard in premake5.lua meaningful.
    default = (os.target() == "windows") and "yes" or "no",
}

newoption {
    trigger = "d3d12ma-dir",
    value = "PATH",
    description = "Path to the D3D12 Memory Allocator source root (default: HPL2/extern/D3D12MemoryAllocator).",
}

newoption {
    trigger = "agility-sdk-dir",
    value = "PATH",
    description = "Path to a local DirectX 12 Agility SDK package root (skips SDK acquisition).",
}

newoption {
    trigger = "xess-sdk-dir",
    value = "PATH",
    description = "Path to a local XeSS SDK root containing inc/xess/xess.h (skips SDK acquisition).",
}

newoption {
    trigger = "python",
    value = "PATH",
    description = "Path to Python forwarded to the FidelityFX SDK CMake wrapper.",
}

newoption {
    trigger = "glslang",
    value = "PATH",
    description = "Path to glslangValidator forwarded to the FidelityFX SDK CMake wrapper.",
}

newoption {
    trigger = "spirv-val",
    value = "PATH",
    description = "Path to spirv-val forwarded to the FidelityFX SDK CMake wrapper.",
}

newoption {
    trigger = "with-tools",
    value = "yes/no",
    description = "Build the HPL2 editors/tools (default yes).",
    allowed = { { "yes", "Build tools" }, { "no", "Skip tools" } },
    default = "yes",
}

newoption {
    trigger = "with-tests",
    value = "yes/no",
    description = "Build and run the headless unit tests (default yes).",
    allowed = { { "yes", "Build tests" }, { "no", "Skip tests" } },
    default = "yes",
}

newoption {
    trigger = "memory-tracking",
    value = "yes/no",
    description = "Enable HPL2 memory tracking (default no).",
    allowed = { { "yes", "Enable memory tracking" }, { "no", "Disable memory tracking" } },
    default = "no",
}

newoption {
    trigger = "with-python-tests",
    value = "yes/no",
    description = "Build and run the Python unit tests (default yes).",
    allowed = { { "yes", "Build Python tests" }, { "no", "Skip Python tests" } },
    default = "yes",
}

newoption {
    trigger = "graphics-x11",
    value = "on/off",
    description = "(Linux) Enable the X11 Vulkan surface backend (default on).",
    allowed = { { "on", "" }, { "off", "" } },
    default = "on",
}

newoption {
    trigger = "graphics-wayland",
    value = "on/off",
    description = "(Linux) Enable the Wayland Vulkan surface backend (default on).",
    allowed = { { "on", "" }, { "off", "" } },
    default = "on",
}

newoption {
    trigger = "cmake",
    value = "PATH",
    description = "Path to the cmake executable used to build SDL2 + openal-soft (default 'cmake' on PATH)."
}
