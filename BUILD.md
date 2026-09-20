# Building Amnesia64

Amnesia64 is built with premake5. The root [`premake5.lua`](premake5.lua) defines the Linux gmake2 and Windows Visual Studio projects; the wrapper scripts cover containerized Linux and Windows command-line builds. CI and the Linux build image pin premake5 to `5.0.0-beta8`. macOS builds are not yet supported.

## Quick start

Pick the path matching your host:

| Host                         | Command                                                        |
| ---------------------------- | -------------------------------------------------------------- |
| Linux (containerized; canonical) | `./build-linux-docker.sh`                                  |
| Linux (native)               | `./build-linux.sh` (native wrapper); or `premake5 gmake2`, then `make -C build-premake config=release -j"$(nproc)"` |
| Windows (PowerShell)         | `.\build-windows.ps1`                                         |

The wrappers default to a release build. Runtime output is placed under `build-premake/amnesia/<Debug|Release>/` (or the equivalent backslash-separated path on Windows).

## 1. Clone the repository

The build pulls in third-party dependencies as git submodules. Clone recursively, or initialize the submodules before configuring:

```bash
git clone --recurse-submodules https://github.com/<your-fork>/Amnesia64.git
cd Amnesia64
```

For an existing checkout:

```bash
git submodule update --init --recursive
```

## 2. Game assets (`deploy.sh` / `deploy.ps1`)

[`deploy.sh`](deploy.sh) and [`deploy.ps1`](deploy.ps1) stage a self-contained run directory after a build: they copy the installed Amnesia: The Dark Descent assets into `build-premake/amnesia/Debug/` and `build-premake/amnesia/Release/` (whichever exist), then bring in the Redux resources from `amnesia/resources`. You need a legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam).

```bash
./deploy.sh --game-dir "/path/to/Amnesia The Dark Descent"
./deploy.sh --resources merge
./deploy.sh --config debug --no-game-assets
```

The native Windows equivalents are:

```powershell
.\deploy.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
.\deploy.ps1 -Resources merge
.\deploy.ps1 -Config debug -NoGameAssets
```

- `--game-dir <path>` — installed game (fallback: `AMNESIA_GAME_DIRECTORY`, then the default Steam library path; `deploy.ps1` also accepts `ATDD_DIR`)
- `--config release|debug|all` — output directories to stage (default: all that exist)
- `--resources copy|merge|none` — `copy` (default) places the `.map_delta` / `.ent_delta` overlay next to the retail files and the engine applies it at load; `merge` bakes the deltas into the deployed `.map` / `.ent` files with `scripts/mapdelta.py` (originals kept as `<file>.mapdelta-orig`) and copies only the non-delta assets
- `--no-game-assets` — skip the install copy and refresh only the Redux resources

Game assets skip names beginning with `Amnesia` and files ending in `.rar`, `.pdf`, `.dll`, or `.exe`; nothing is written into the game installation. Files the Redux step placed are listed in `<Config>/.redux_overlay_manifest`, and files no longer placed (deleted assets, or delta files after switching to `merge`) are removed on the next run.

## 3. Linux build (containerized or native)

The canonical Linux command is [`build-linux-docker.sh`](build-linux-docker.sh). It builds the [`Dockerfile`](Dockerfile) image, then runs the complete build inside the container:

```bash
./build-linux-docker.sh [release|debug] [options] [-- <extra premake args>]
```

Inside the container, the wrapper optionally removes `build-premake/` for `--clean`, runs `premake5 gmake2`, optionally exports the compile database, and builds with `make`. Unit-test projects and the Python tests are skipped unless `-with-test` is supplied. Stage assets afterwards with `./deploy.sh`. Its options are:

- `release|debug` — build configuration (default: `release`)
- `--clean` — remove `build-premake/` before generating projects
- `-with-test` — build and run the unit tests
- `--compile-commands` — run `premake5 export-compile-commands` and symlink the selected database to `compile_commands.json` in the repository root
- `-- <args>` — forward extra arguments to `premake5 gmake2`

Memory tracking is an opt-in Debug/Release build option. It defaults to `no` in
both configurations. See the [memory-tracking note](notes/memory_tracking.md)
for report paths, lifecycle guidance, and verification boundaries. The
canonical Linux rollout sequence is:

```bash
./build-linux-docker.sh debug -- --memory-tracking=yes
./build-linux-docker.sh debug -- --memory-tracking=no
```

The second command returns to the normal build. Regenerate the Premake project
and rebuild after changing `--memory-tracking`; changing the option does not
retrofit an already generated or compiled build.

Examples:

```bash
./build-linux-docker.sh
./build-linux-docker.sh debug --clean
./build-linux-docker.sh debug -with-test
./build-linux-docker.sh release --compile-commands
./build-linux-docker.sh release -- --with-tools=no
```

For a native Linux build, use [`build-linux.sh`](build-linux.sh) as the native wrapper:

```bash
./build-linux.sh [release|debug] [options] [-- <extra premake args>]
```

It requires premake5 `5.0.0-beta8` on `PATH`, a working C/C++ toolchain on the host, GNU Make, and Python 3. It runs `premake5 gmake2` and builds the selected configuration; pass `-with-test` to also build and run the unit tests. Stage assets afterwards with `./deploy.sh`. Unlike the container wrapper, it has no `--compile-commands` option and does not use any `AMNESIA_DOCKER_*` environment variables. Its options are:

- `release|debug` — build configuration (default: `release`)
- `--clean` — remove `build-premake/` before generating projects
- `-with-test` — build and run the unit tests
- `-h, --help` — show help
- `-- <args>` — forward extra arguments to `premake5 gmake2`

Examples:

```bash
./build-linux.sh
./build-linux.sh debug --clean
./build-linux.sh debug -with-test
./build-linux.sh release -- --with-tools=no
```

The underlying commands can also be run directly:

```bash
premake5 gmake2 [options]
make -C build-premake config=release -j"$(nproc)"
# Or: make -C build-premake config=debug -j"$(nproc)"
```

Premake writes runtime output to `build-premake/amnesia/Release/` and `build-premake/amnesia/Debug/`. After the build, stage the game assets with `./deploy.sh --game-dir "/path/to/Amnesia The Dark Descent"`.

### Container environment and mounts

The container wrapper auto-selects rootless Podman when `podman` is on `PATH`, otherwise Docker. Override the selection with `AMNESIA_DOCKER_RUNTIME=podman` or `AMNESIA_DOCKER_RUNTIME=docker`. The other supported environment variables are:

- `AMNESIA_DOCKER_IMAGE` â€” image tag to build and run (default: `amnesia64-build:ubuntu-24.04`)
- `AMNESIA_DOCKER_MOUNTS` â€” colon-separated paths outside the repository to bind-mount at the same paths inside the container
- `AMNESIA_GAME_DIRECTORY` â€” fallback game-install path for deploy

The project tree is bind-mounted at its real host path, so absolute paths in generated `build-premake/` makefiles line up between containerized and native Premake runs. With `--compile-commands`, the generated database is under `build-premake/compile_commands/` and the wrapper creates the repository-root `compile_commands.json` symlink. You can switch between the native and containerized Premake paths without a full clean; cleaning the first switch is useful when object files were built with a different host toolchain.

Anything outside the project tree, such as a locally built `slangc`, must be mounted explicitly. The example forwards a Premake option after `--`:

```bash
AMNESIA_DOCKER_MOUNTS=/home/me/projects/slang \
    ./build-linux-docker.sh release \
    -- --slangc=/home/me/projects/slang/build/Release/bin/slangc
```

Multiple mount paths can be colon-separated. Each is mounted at the same path inside the container, so arguments referring to those paths work unchanged.

### Rootless Podman: cleaning a `build-premake/` owned by a subuid

Rootless Podman maps container root back to the host user. If a `build-premake/` directory contains files owned by a subuid, remove it through `podman unshare`:

```bash
podman unshare rm -rf build-premake/
```

## 4. `build-windows.ps1`

Requires a Visual Studio 2026 installation. The wrapper uses Premake `5.0.0-beta8` from `PATH` when available; otherwise it downloads and verifies the pinned release under `build-premake\_deps\premake`. MSBuild is auto-located with `vswhere`, so any PowerShell works — not just a Developer PowerShell:

```powershell
.\build-windows.ps1                                  # release
.\build-windows.ps1 debug                            # debug
.\build-windows.ps1 debug -WithTest                  # build and run tests
.\build-windows.ps1 release -Clean                   # wipe build-premake\
.\build-windows.ps1 release -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
.\build-windows.ps1 release -- --with-tools=no
```

The script generates a Visual Studio solution under `build-premake\` with `premake5 vs2026`, then runs `msbuild` for `x64`. Unit-test projects are skipped unless `-WithTest` is supplied. Premake currently names the VS 2026 solution `Amnesia.slnx`; the wrapper also accepts `Amnesia.sln` for compatible generators. Stage assets with `.\deploy.ps1` (or `deploy.sh` from Git Bash/WSL). Additional `--foo` / `--foo=bar` arguments are forwarded to `premake5 vs2026` as Premake options, not to MSBuild. Generated project files and runtime output stay under `build-premake\`; runtime output is `build-premake\amnesia\<Config>\`.

The Windows CI workflow ([`.github/workflows/windows-build.yml`](.github/workflows/windows-build.yml)) uses `premake5 vs2022` because its hosted runner provides Visual Studio 2022. Both `vs2022` and `vs2026` are valid here: [`premake5.lua`](premake5.lua) does not pin `_ACTION`, so they generate the same projects. CI builds with `msbuild` targeting `x64` as well.

## 5. Native build details (when you want to drop the wrappers)

### Linux

The Linux toolchain is premake5 `5.0.0-beta8`, a C/C++ compiler, GNU Make, Python 3, and the recursively initialized submodules. The [`Dockerfile`](Dockerfile) is the source of truth for the Ubuntu 24.04 development environment and installs these packages:

| Area | Packages installed by `Dockerfile` |
| ---- | ----------------------------------- |
| Build and scripting tools | `build-essential`, `clang`, `ninja-build`, `cmake`, `python3`, `git`, `ca-certificates`, `curl` |
| Shader tools | `glslang-tools`, `spirv-tools` |
| X11 | `libx11-dev`, `libxext-dev`, `libxi-dev`, `libxcursor-dev`, `libxrandr-dev`, `libxss-dev` |
| Graphics and Wayland | `libgl-dev`, `libglu1-mesa-dev`, `libegl1-mesa-dev`, `libwayland-dev`, `libxkbcommon-dev`, `libdecor-0-dev` |
| Audio | `libasound2-dev`, `libpulse-dev`, `libdbus-1-dev`, `libsamplerate0-dev` |
| Image support | `liblcms2-dev` |

`cmake` is still required as an installed tool: [`premake/external.lua`](premake/external.lua) drives the bundled SDL2 and openal-soft builds through their own CMake projects. The `--cmake=PATH` option points Premake at that executable. The FidelityFX SDK is built through the [`cmake/fsr/CMakeLists.txt`](cmake/fsr/CMakeLists.txt) wrapper. CMake is not the repository's top-level build driver.

### Shader compilers

Engine shaders are Slang (`.slang`, with a stage suffix such as `.vert.slang`, `.frag.slang`, `.comp.slang`, `.rgen.slang`). [`premake/slang.lua`](premake/slang.lua) compiles each to SPIR-V with a prebuilt `slangc`, which Premake downloads at configuration time into `build-premake/_deps/slang-prebuilt/`. The pinned version is `SLANG_VERSION = "2026.17"` in that file. Override the download with `--slangc=/path/to/slangc`.

There is no glslang step for engine shaders. `glslang-tools` and `spirv-tools` are in the `Dockerfile` for the FidelityFX SDK's own shader compilation; `--glslang=PATH` and `--spirv-val=PATH` are forwarded to that SDK's CMake wrapper only.

### Windows

The direct native Windows flow is the same generator and MSBuild sequence used by the wrapper:

```powershell
premake5 vs2026
msbuild build-premake\Amnesia.sln /p:Configuration=Release /p:Platform=x64 /m:4
```

On a Visual Studio 2022 installation, use `premake5 vs2022` instead; the generated projects are the same for this repository.

### Vulkan mapped-buffer smoke (Windows)

The default Windows build uses Vulkan. The always-available manual target below
is the startup-style VMA mapped-allocation flush check:

```bat
premake5 vs2022 --with-d3d12=no
msbuild build-premake\Amnesia.sln /t:RIVulkanMappedBufferFlushSmoke /p:Configuration=Debug /p:Platform=x64
build-premake\tests\Debug\RIVulkanMappedBufferFlushSmoke.exe
```

It creates, maps, writes, flushes, invalidates, and disposes host buffers. This
is the first check for a startup stack ending in `vmaFlushAllocation` from
`cGraphics::Init`.

### DirectX 12 bring-up (experimental, Windows only)

Ordinary generation uses the Vulkan runtime. On Windows, generation with `--with-d3d12=yes` uses the DirectX 12 runtime and D3D12MA. The opt-in backend currently ships adapter enumeration, device/queue/fence lifecycle, command allocator and command-list lifecycle, D3D12MA-backed buffer and texture resources, and partial swapchain create/dispose/acquire/present.

Generate with `premake5 vs2022 --with-d3d12=yes` (or `premake5 vs2026 --with-d3d12=yes`) to use the DirectX 12 runtime and D3D12MA, then build with MSBuild as usual. Run the opt-in smoke test manually:

Switching an existing checkout between the default Vulkan generation and
`--with-d3d12=yes` is safe for incremental builds. The ABI-changing engine and
consumer artifacts are kept in backend-specific mode directories, and the
generated mode stamp changes when the backend changes so final executables are
relinked. At runtime, explicitly requesting a backend that was omitted from
the build returns `RI_FAIL`; the renderer never silently falls back to another
backend.

```powershell
build-premake\tests\<Config>\RID3D12DeviceSmoke.exe
build-premake\tests\<Config>\RID3D12DeviceSmoke.exe --vulkan
```

Run the DX12 and Vulkan modes as separate process invocations. The smoke test exercises WARP itself by toggling `g_riD3D12EnableDebugLayer`; users do not need a separate switch, and the game's default adapter selection is unchanged. Debug-layer messages appear in the debugger output window when running under a debugger.

Buffer resource lifecycle can be exercised with the opt-in `RID3D12BufferSmoke`:

```powershell
build-premake\tests\<Config>\RID3D12BufferSmoke.exe
build-premake\tests\<Config>\RID3D12BufferSmoke.exe --vulkan
```

The D3D12 backend uses D3D12MA for DEFAULT, UPLOAD, and READBACK buffer allocations. The smoke covers create/dispose, mapping, GPU addresses and error paths only â€” command-list-driven upload/readback verification lands with the copy/barrier work.

#### D3D12MA ownership

Every successfully initialized D3D12 `RIDevice` owns one D3D12MA allocator and releases it during device disposal. RI-created D3D12 buffers and textures own their D3D12MA allocation handles; disposal releases the resource before its allocation and clears both handles. The mip-generation uploader follows the same rule for its internal upload constant buffers and scratch textures, and releases them when `RI_FreeResourceUploader` is called. The D3D12 smoke tests verify non-null allocator/allocation handles, matching resource handles for mip resources, and cleared handles after disposal with the debug layer enabled.

There are two intentional borrowed-resource exceptions. Swapchain texture objects are aliases of the swapchain images and are owned by `RISwapchain::d3d12.images`, so they do not own a D3D12MA allocation. The device and copy smoke tests also create a small number of raw committed D3D12 resources as test fixtures; those fixtures are released directly by the test and are not RI/D3D12MA-owned.

When the bundled allocator source is unavailable or a local checkout is required, pass `--d3d12ma-dir=PATH` to Premake. `PATH` must be the D3D12 Memory Allocator source root containing `include/D3D12MemAlloc.h` and `src/D3D12MemAlloc.cpp`; without the override, Premake uses `HPL2/extern/D3D12MemoryAllocator`.

Mip-generation lifecycle can be exercised with the opt-in smoke test:

```powershell
build-premake\tests\<Config>\RID3D12MipGenerationSmoke.exe
build-premake\tests\<Config>\RID3D12MipGenerationSmoke.exe --no-debug-layer
```

The compiler-only `RID3D12ShaderFixtures` target builds Slang-to-DXIL fixtures under `tests/graphics/ri_d3d12/shaders/` into `build-premake\tests\<Config>\compiled_shaders\d3d12\`: `triangle.slang` provides `VSMain` (vertex, output `triangle.vert.dxil`) and `PSMain` (fragment, output `triangle.frag.dxil`), and `compute.slang` provides `CSMain` (compute, output `compute.comp.dxil`). Each entry is compiled with `-target dxil -profile sm_6_6 -matrix-layout-column-major` and explicit `-entry`/`-stage`; no `-fvk-*` or SPIR-V flags are passed. Build it explicitly after generating the solution:

```powershell
msbuild build-premake\Amnesia.sln /t:RID3D12ShaderFixtures /p:Configuration=Debug /p:Platform=x64
```

The pinned Slang 2026.17 distribution used by [`premake/slang.lua`](premake/slang.lua) emits DXIL directly, so no external DXC or `dxil.dll` is required for these fixtures; the generated `.dxil` files carry the standard `DXBC` container magic and DXIL entry-point/shader-model metadata. This target adds no DXIL loading, root signatures, PSOs, or rendering: it is compilation infrastructure only. The production engine continues to load its SPIR-V shader artifacts unchanged, and the existing SPIR-V build rules for the `Amnesia` project are unaffected by enabling this target.

#### Agility SDK integration

`--with-d3d12=yes` pins the stable Microsoft DirectX 12 Agility SDK (`Microsoft.Direct3D.D3D12` 1.619.5, SDK version 619). Premake auto-acquires the NuGet package into `build-premake/_deps/agility-sdk/` and validates it by SHA-256 (`0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6`). A local extracted copy of the same version can be pointed at with `--agility-sdk-dir=PATH`. The pinned headers precede the Windows SDK's `d3d12.h` at workspace scope so HPL2 and every RI header consumer agrees on declarations.

Each DX12 executable (Amnesia, editors, MshConverter, RID3D12*Smoke) carries C-linkage data exports `D3D12SDKVersion = 619` and `D3D12SDKPath = ".\\D3D12\\"` from `premake/runtime/D3D12AgilityExports.cpp`. Windows resolves `.\D3D12\` relative to the executable, so runtime file layout next to each `.exe` is:

```text
Amnesia.exe (or RID3D12DeviceSmoke.exe, ...)
D3D12\D3D12Core.dll
D3D12\d3d12SDKLayers.dll   (used only with the debug layer enabled)
licenses\agility\LICENSE.txt
licenses\agility\LICENSE-CODE.txt
licenses\agility\distributable files.txt
licenses\agility\README.md
```

The staging is owned by `AgilityRuntimeGame` (into `build-premake\amnesia\<Config>\`) and `AgilityRuntimeTests` (into `build-premake\tests\<Config>\`); a missing destination file is restored on the next build without requiring the executable itself to relink. `msbuild /t:Clean` scrubs the staged `D3D12\` and `licenses\agility\` subfolders.

Warm builds reuse `build-premake/_deps/agility-sdk/microsoft.direct3d.d3d12.1.619.5/` and do NOT touch the network. Manual verification of one target:

```powershell
dumpbin /exports build-premake\tests\<Config>\RID3D12DeviceSmoke.exe | Select-String "D3D12SDK"
```

Expected output includes both `D3D12SDKVersion` and `D3D12SDKPath` exactly once each. The `RID3D12DeviceSmoke` executable also self-checks these exports on startup and reports the actual loaded `D3D12Core.dll` path — a newer OS runtime is a legitimate outcome and is reported rather than treated as a failure.

`--with-d3d12=no` and Linux generation reference no Agility artifacts (no include dirs, no staged DLLs, no exports source, no staging projects).

Command submission + timeline lifecycle can be exercised with the opt-in `RID3D12QueueSmoke`:

```powershell
build-premake\tests\<Config>\RID3D12QueueSmoke.exe
build-premake\tests\<Config>\RID3D12QueueSmoke.exe --vulkan
```

This test exercises RIQueue::submit, RICommandRingBuffer wraparound over more than `RI_COMMAND_RING_POOL_COUNT` iterations, cross-queue timeline dependencies, and multiple acquisitions from one pool. Lifetime contract enforced by the test:
- An RICommandRingElement acquired by `acquire()` is NOT pending on DX12 until its owning submit succeeds â€” its completion value is 0 until then, and `element.wait()` is a no-op.
- Callers must `element.wait()` on the LAST acquisition from a pool before calling `pool->reset()`; the ring reuses one command allocator per pool, and D3D12 rejects a reset while any list from that allocator is still executing.
- `hpl::RITimeline::completed()` reports GPU progress; unlike a header-only fallback it never lies by returning `signalValue`. The DX12 path returns `UINT64_MAX` on device removal.

Resource-state barrier translation can be exercised with the opt-in `RID3D12BarrierSmoke`:

```powershell
build-premake\tests\<Config>\RID3D12BarrierSmoke.exe
build-premake\tests\<Config>\RID3D12BarrierSmoke.exe --vulkan
```

This test transitions two RIBuffers through COPY_SRC/COPY_DST to SHADER_RESOURCE via
`RICmd::vk_d3d12_resourceBarrier`, exercises `vk_d3d12_memoryBarrier` and
`vk_d3d12_bufferBarrier`, submits, and waits. The D3D12 arm uses
`ID3D12GraphicsCommandList7::Barrier` when
`RIPhysicalAdapter::isEnchancedBarrierSupported` is set and the runtime command
list can be queried for `ID3D12GraphicsCommandList7`; otherwise it falls back
to the legacy `ResourceBarrier` path. The smoke test asserts that the path
taken matches the adapter feature.

Per-image swapchain fence semantics can be exercised with the opt-in `RID3D12SwapchainSmoke`:

```powershell
build-premake\tests\<Config>\RID3D12SwapchainSmoke.exe
```

This test runs four present cycles on a WARP swapchain over a hidden HWND and confirms that
per-image fence values match the queue's monotonic signals before `AcquireNextTexture` returns.

## 6. Python tests

The Python tests under `tests/` run after `make` when either Linux wrapper receives `-with-test`, and as a post-build check of the Premake test projects when tests are enabled (`--with-tests`/`--with-python-tests`). They use the standard-library `unittest` module and must not require a game installation, GPU, or display; tests build fake trees under `tempfile.TemporaryDirectory` instead.

Run them from the repository root:

```bash
python3 scripts/run_python_tests.py -v
```

Test modules must use the `*_test.py` naming pattern. Every directory from `tests/` to a test module must contain an `__init__.py` file so unittest discovery can recurse into it.

An empty suite is not an error: the runner maps exit 5 to 0, while test failures and import errors still fail the build.

## 7. Premake options

The options below are defined in [`premake/options.lua`](premake/options.lua). Pass them directly to `premake5 gmake2` or `premake5 vs2026` (and likewise `vs2022` in CI). When using a wrapper, put Premake options after `--`, for example `./build-linux-docker.sh release -- --with-fsr=no` or `.\build-windows.ps1 release -- --with-tools=no`.

| Option | Default | Purpose |
| ------ | ------- | ------- |
| `--slangc=PATH` | Auto-download the pinned compiler | Use a local `slangc`; otherwise Premake downloads it to `build-premake/_deps/slang-prebuilt/` during configuration. |
| `--build-version=VERSION` | `V0000` | Embed the displayed release version. CI release builds use the release tag. The short commit hash is read from the checkout when Premake generates the build. |
| `--with-fsr=yes\|no` | `yes` | Build and link the FidelityFX Super Resolution SDK. |
| `--fsr-sdk-dir=PATH` | Unset; auto-acquire when FSR is enabled | Use a local FidelityFX SDK root containing `sdk/`. |
| `--with-xess=yes\|no` | `yes` | Enable XeSS on Windows; ignored on Linux. |
| `--xess-sdk-dir=PATH` | Unset; auto-acquire on Windows when XeSS is enabled | Use a local XeSS SDK root containing `inc/xess/xess_vk.h`. |
| `--with-d3d12=yes\|no` | `no` | Windows only; ordinary generation uses Vulkan. With `--with-d3d12=yes`, use the DirectX 12 runtime and D3D12MA. |
| `--d3d12ma-dir=PATH` | `HPL2/extern/D3D12MemoryAllocator` | Windows/DX12 only; use a local D3D12 Memory Allocator source root containing `include/` and `src/` instead of the bundled copy. |
| `--agility-sdk-dir=PATH` | Unset; auto-acquire when DX12 is enabled | Windows only; use a locally extracted copy of the pinned `Microsoft.Direct3D.D3D12` 1.619.5 NuGet package instead of downloading. |
| `--python=PATH` | Unset; find `python3`/`python` | Select the Python executable forwarded to the FidelityFX SDK wrapper and used by Premake Python test projects. |
| `--glslang=PATH` | Unset; find `glslangValidator`/`glslang` | Select the GLSL compiler for FidelityFX shader generation. |
| `--spirv-val=PATH` | Unset; use `spirv-val` if found | Select the optional SPIR-V validator for FidelityFX shader generation. |
| `--with-tools=yes\|no` | `yes` | Build the HPL2 editors and tools. |
| `--with-tests=yes\|no` | `yes` | Build and run the headless unit tests. |
| `--with-python-tests=yes\|no` | `yes` | Build and run the Python unit tests in the Premake test projects. |
| `--memory-tracking=yes\|no` | `no` | Opt in to FluidStudios tracking for global replaceable C++ operators and HPL buffers; applies to Debug and Release. Regenerate and rebuild when changing it. |
| `--graphics-x11=on\|off` | `on` on Linux | Enable the X11 Vulkan surface backend. |
| `--graphics-wayland=on\|off` | `on` on Linux | Enable the Wayland Vulkan surface backend. |
| `--cmake=PATH` | `cmake` on `PATH` | Select the CMake executable Premake uses for the bundled SDL2 and openal-soft builds and the FSR wrapper. |

## 8. Running

The release workflow packages the binaries and compiled shaders together with
all files from `amnesia/resources`, preserving their paths relative to the game
directory. Extract the release into an existing game installation, merge folders
and replace matching files, then launch the release's `Amnesia` executable. The
release includes repository asset overrides, not the complete retail game data.
Ordinary CI build artifacts contain the build output; the resource overlay is
added when the final GitHub/itch.io release payloads are assembled.

The main menu shows the build's version and short commit hash; both are also
written to the startup log. For a local versioned build, pass
`--build-version=v1.2.3` to Premake or through a build wrapper after `--`.

Run the built game from the self-contained runtime directory, such as `build-premake/amnesia/Release/` or `build-premake/amnesia/Debug/`. `deploy.sh` copies the game assets from the install directory you pass with `--game-dir <path>` into those output directories, together with the Redux resources; it does not put the newly built executable into the game installation.

If the game complains about missing files, verify the install path and rerun asset staging:

```bash
./deploy.sh --game-dir "/path/to/Amnesia The Dark Descent"
```

`deploy.sh` falls back to `AMNESIA_GAME_DIRECTORY`, then the default Steam library path. The Windows wrapper's `-GameDir <path>` (or `ATDD_DIR`) only sets the Visual Studio debugging directory.
