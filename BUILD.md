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

## 2. Game assets (`deploy.sh`)

[`deploy.sh`](deploy.sh) stages a self-contained run directory after a build: it copies the installed Amnesia: The Dark Descent assets into `build-premake/amnesia/Debug/` and `build-premake/amnesia/Release/` (whichever exist), then brings in the Redux resources from `amnesia/resources`. You need a legitimate copy of **Amnesia: The Dark Descent** (e.g. via Steam).

```bash
./deploy.sh --game-dir "/path/to/Amnesia The Dark Descent"
./deploy.sh --resources merge
./deploy.sh --config debug --no-game-assets
```

- `--game-dir <path>` — installed game (fallback: `AMNESIA_GAME_DIRECTORY`, then the default Steam library path)
- `--config release|debug|all` — output directories to stage (default: all that exist)
- `--resources copy|merge|none` — `copy` (default) places the `.map_delta` / `.ent_delta` overlay next to the retail files and the engine applies it at load; `merge` bakes the deltas into the deployed `.map` / `.ent` files with `scripts/mapdelta.py` (originals kept as `<file>.mapdelta-orig`) and copies only the non-delta assets
- `--no-game-assets` — skip the install copy and refresh only the Redux resources

Game assets skip names beginning with `Amnesia` and files ending in `.rar`, `.pdf`, `.dll`, or `.exe`; nothing is written into the game installation. Files the Redux step placed are listed in `<Config>/.redux_overlay_manifest`, and files no longer placed (deleted assets, or delta files after switching to `merge`) are removed on the next run.

## 3. Linux build (containerized or native)

The canonical Linux command is [`build-linux-docker.sh`](build-linux-docker.sh). It builds the [`Dockerfile`](Dockerfile) image, then runs the complete build inside the container:

```bash
./build-linux-docker.sh [release|debug] [options] [-- <extra premake args>]
```

Inside the container, the wrapper optionally removes `build-premake/` for `--clean`, runs `premake5 gmake2`, optionally exports the compile database, builds with `make`, and runs the Python tests. Stage assets afterwards with `./deploy.sh`. Its options are:

- `release|debug` — build configuration (default: `release`)
- `--clean` — remove `build-premake/` before generating projects
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
./build-linux-docker.sh release --compile-commands
./build-linux-docker.sh release -- --with-tools=no
```

For a native Linux build, use [`build-linux.sh`](build-linux.sh) as the native wrapper:

```bash
./build-linux.sh [release|debug] [options] [-- <extra premake args>]
```

It requires premake5 `5.0.0-beta8` on `PATH`, a working C/C++ toolchain on the host, GNU Make, and Python 3. It runs `premake5 gmake2`, builds the selected configuration, and runs the Python tests. Stage assets afterwards with `./deploy.sh`. Unlike the container wrapper, it has no `--compile-commands` option and does not use any `AMNESIA_DOCKER_*` environment variables. Its options are:

- `release|debug` — build configuration (default: `release`)
- `--clean` — remove `build-premake/` before generating projects
- `-h, --help` — show help
- `-- <args>` — forward extra arguments to `premake5 gmake2`

Examples:

```bash
./build-linux.sh
./build-linux.sh debug --clean
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

- `AMNESIA_DOCKER_IMAGE` — image tag to build and run (default: `amnesia64-build:ubuntu-24.04`)
- `AMNESIA_DOCKER_MOUNTS` — colon-separated paths outside the repository to bind-mount at the same paths inside the container
- `AMNESIA_GAME_DIRECTORY` — fallback game-install path for deploy

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

Requires `premake5.exe` on `PATH` and a Visual Studio 2026 installation. MSBuild is auto-located with `vswhere`, so any PowerShell works — not just a Developer PowerShell:

```powershell
.\build-windows.ps1                                  # release
.\build-windows.ps1 debug                            # debug
.\build-windows.ps1 release -Clean                   # wipe build-premake\
.\build-windows.ps1 release -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
.\build-windows.ps1 release -- --with-tools=no
```

The script generates a Visual Studio solution under `build-premake\` with `premake5 vs2026`, then runs `msbuild` for `x64`. Stage assets with `deploy.sh` (for example from Git Bash or WSL). Extra arguments after `--` are forwarded to `premake5 vs2026` as Premake options, not to MSBuild. Generated project files and runtime output stay under `build-premake\`; runtime output is `build-premake\amnesia\<Config>\`.

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

Engine shaders are Slang (`.slang`, with a stage suffix such as `.vert.slang`, `.frag.slang`, `.comp.slang`, `.rgen.slang`). [`premake/slang.lua`](premake/slang.lua) compiles each to SPIR-V with a prebuilt `slangc`, which Premake downloads at configuration time into `build-premake/_deps/slang-prebuilt/`. The pinned version is `SLANG_VERSION = "2026.11"` in that file. Override the download with `--slangc=/path/to/slangc`.

There is no glslang step for engine shaders. `glslang-tools` and `spirv-tools` are in the `Dockerfile` for the FidelityFX SDK's own shader compilation; `--glslang=PATH` and `--spirv-val=PATH` are forwarded to that SDK's CMake wrapper only.

### Windows

The direct native Windows flow is the same generator and MSBuild sequence used by the wrapper:

```powershell
premake5 vs2026
msbuild build-premake\Amnesia.sln /p:Configuration=Release /p:Platform=x64 /m
```

On a Visual Studio 2022 installation, use `premake5 vs2022` instead; the generated projects are the same for this repository.

## 6. Python tests

The Python tests under `tests/` run in the Linux build's `Python tests` step before the expensive build, in `build-linux-docker.sh` after `make`, in `build-linux.sh` after `make`, and as a post-build check of the Premake test projects when tests are enabled (`--with-tests`/`--with-python-tests`). They use the standard-library `unittest` module and must not require a game installation, GPU, or display; tests build fake trees under `tempfile.TemporaryDirectory` instead.

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
