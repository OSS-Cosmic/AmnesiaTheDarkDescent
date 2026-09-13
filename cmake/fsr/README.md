# FSR shader compiler staging

`ffx_sc/CMakeLists.txt` copies only the GLSL-capable FidelityFX_SC sources,
SPIRV-Reflect, MD5, and the platform-matched tiny-process backend from the
pinned SDK into the CMake binary tree. `patches/portable_glsl.cmake` applies narrowly scoped,
anchor-checked edits to that staged copy: it removes DX/HLSL-only dependencies,
replaces Windows path/file/entry-point APIs with C++17 and UTF-8 equivalents,
and keeps `-compiler=glslang` as the only non-error compiler selection.

The SDK checkout remains read-only. The patches preserve the upstream compiler's
permutation, reflection, binary-header, and GCC-depfile formats; they only make
the GLSL/Vulkan tool build and run on non-Windows hosts. `FSR_GLSLANG` and
`Python3_EXECUTABLE` are required tool overrides with `find_program` fallbacks;
`FSR_SPIRV_VAL` is optional because validation is an optional driver argument.
The standalone `tests/fsr/check_patch_restaging.py` checker configures and
builds this project in a disposable copy of `cmake/fsr`, so it exercises both
this staging flow and the SDK source restaging without modifying the checkout.

The shader driver also stages an anchor-checked correction to SDK 1.1.4's GLSL
luma-history storage format: `rgba16f` matches the SDK's RGBA16F allocation;
the upstream `rgba8` declaration produces undefined history data on Vulkan.
`FsrShaderBlobTests` checks the embedded SPIR-V format across permutations.
