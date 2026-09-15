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
    trigger = "fsr-sdk-dir",
    value = "PATH",
    description = "Path to a local FidelityFX SDK root containing sdk/ (skips SDK acquisition).",
}

newoption {
    trigger = "with-xess",
    value = "yes/no",
    description = "Enable the optional Intel XeSS Vulkan super-resolution backend (Windows only; ignored on Linux).",
    allowed = { { "yes", "Build XeSS" }, { "no", "Skip XeSS" } },
    default = "yes",
}

newoption {
    trigger = "xess-sdk-dir",
    value = "PATH",
    description = "Path to a local XeSS SDK root containing inc/xess/xess_vk.h (skips SDK acquisition).",
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
